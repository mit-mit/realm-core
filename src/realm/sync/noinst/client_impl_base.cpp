#include "realm/sync/protocol.hpp"
#include <system_error>
#include <sstream>

#include <realm/util/assert.hpp>
#include <realm/util/safe_int_ops.hpp>
#include <realm/util/random.hpp>
#include <realm/util/memory_stream.hpp>
#include <realm/util/basic_system_errors.hpp>
#include <realm/util/scope_exit.hpp>
#include <realm/util/to_string.hpp>
#include <realm/util/uri.hpp>
#include <realm/util/platform_info.hpp>
#include <realm/sync/impl/clock.hpp>
#include <realm/impl/simulated_failure.hpp>
#include <realm/sync/network/http.hpp>
#include <realm/sync/noinst/client_history_impl.hpp>
#include <realm/sync/noinst/client_impl_base.hpp>
#include <realm/sync/noinst/compact_changesets.hpp>
#include <realm/sync/protocol.hpp>
#include <realm/version.hpp>
#include <realm/sync/changeset_parser.hpp>

#include <realm/sync/network/websocket.hpp> // Only for websocket::Error TODO remove

// NOTE: The protocol specification is in `/doc/protocol.md`


using namespace realm;
using namespace _impl;
using namespace realm::util;
using namespace realm::sync;

// clang-format off
using Connection      = ClientImpl::Connection;
using Session         = ClientImpl::Session;
using UploadChangeset = ClientHistory::UploadChangeset;

// These are a work-around for a bug in MSVC. It cannot find in-class types
// mentioned in signature of out-of-line member function definitions.
using ConnectionTerminationReason = ClientImpl::ConnectionTerminationReason;
using OutputBuffer                = ClientImpl::OutputBuffer;
using ReceivedChangesets          = ClientProtocol::ReceivedChangesets;
// clang-format on

void ErrorTryAgainBackoffInfo::update(const ProtocolErrorInfo& info)
{
    if (triggering_error && static_cast<ProtocolError>(info.raw_error_code) == *triggering_error) {
        return;
    }

    delay_info = info.resumption_delay_interval.value_or(ResumptionDelayInfo{});
    cur_delay_interval = util::none;
    triggering_error = static_cast<ProtocolError>(info.raw_error_code);
}

void ErrorTryAgainBackoffInfo::reset()
{
    triggering_error = util::none;
    cur_delay_interval = util::none;
    delay_info = ResumptionDelayInfo{};
}

std::chrono::milliseconds ErrorTryAgainBackoffInfo::delay_interval()
{
    if (!cur_delay_interval) {
        cur_delay_interval = delay_info.resumption_delay_interval;
        return *cur_delay_interval;
    }
    if (*cur_delay_interval >= delay_info.max_resumption_delay_interval) {
        return delay_info.max_resumption_delay_interval;
    }
    *cur_delay_interval *= delay_info.resumption_delay_backoff_multiplier;
    return *cur_delay_interval;
}

bool ClientImpl::decompose_server_url(const std::string& url, ProtocolEnvelope& protocol, std::string& address,
                                      port_type& port, std::string& path) const
{
    util::Uri uri(url); // Throws
    uri.canonicalize(); // Throws
    std::string userinfo, address_2, port_2;
    bool realm_scheme = (uri.get_scheme() == "realm:" || uri.get_scheme() == "realms:");
    bool ws_scheme = (uri.get_scheme() == "ws:" || uri.get_scheme() == "wss:");
    bool good = ((realm_scheme || ws_scheme) && uri.get_auth(userinfo, address_2, port_2) && userinfo.empty() &&
                 !address_2.empty() && uri.get_query().empty() && uri.get_frag().empty()); // Throws
    if (REALM_UNLIKELY(!good))
        return false;
    ProtocolEnvelope protocol_2;
    port_type port_3;
    if (realm_scheme) {
        if (uri.get_scheme() == "realm:") {
            protocol_2 = ProtocolEnvelope::realm;
            port_3 = (m_enable_default_port_hack ? 80 : 7800);
        }
        else {
            protocol_2 = ProtocolEnvelope::realms;
            port_3 = (m_enable_default_port_hack ? 443 : 7801);
        }
    }
    else {
        REALM_ASSERT(ws_scheme);
        if (uri.get_scheme() == "ws:") {
            protocol_2 = ProtocolEnvelope::ws;
            port_3 = 80;
        }
        else {
            protocol_2 = ProtocolEnvelope::wss;
            port_3 = 443;
        }
    }
    if (!port_2.empty()) {
        std::istringstream in(port_2);    // Throws
        in.imbue(std::locale::classic()); // Throws
        in >> port_3;
        if (REALM_UNLIKELY(!in || !in.eof() || port_3 < 1))
            return false;
    }
    std::string path_2 = uri.get_path(); // Throws (copy)

    protocol = protocol_2;
    address = std::move(address_2);
    port = port_3;
    path = std::move(path_2);
    return true;
}


ClientImpl::ClientImpl(ClientConfig config)
    : logger_ptr{config.logger ? std::move(config.logger) : std::make_shared<util::StderrLogger>()}
    , logger{*logger_ptr}
    , m_reconnect_mode{config.reconnect_mode}
    , m_connect_timeout{config.connect_timeout}
    , m_connection_linger_time{config.one_connection_per_session ? 0 : config.connection_linger_time}
    , m_ping_keepalive_period{config.ping_keepalive_period}
    , m_pong_keepalive_timeout{config.pong_keepalive_timeout}
    , m_fast_reconnect_limit{config.fast_reconnect_limit}
    , m_disable_upload_activation_delay{config.disable_upload_activation_delay}
    , m_dry_run{config.dry_run}
    , m_enable_default_port_hack{config.enable_default_port_hack}
    , m_disable_upload_compaction{config.disable_upload_compaction}
    , m_fix_up_object_ids{config.fix_up_object_ids}
    , m_roundtrip_time_handler{std::move(config.roundtrip_time_handler)}
    , m_socket_provider{std::move(config.socket_provider)}
    , m_client_protocol{} // Throws
    , m_one_connection_per_session{config.one_connection_per_session}
    , m_random{}
{
    // FIXME: Would be better if seeding was up to the application.
    util::seed_prng_nondeterministically(m_random); // Throws

    logger.debug("Realm sync client (%1)", REALM_VER_CHUNK); // Throws
    logger.debug("Supported protocol versions: %1-%2", get_oldest_supported_protocol_version(),
                 get_current_protocol_version()); // Throws
    logger.debug("Platform: %1", util::get_platform_info());
    const char* build_mode;
#if REALM_DEBUG
    build_mode = "Debug";
#else
    build_mode = "Release";
#endif
    logger.debug("Build mode: %1", build_mode);
    logger.debug("Config param: one_connection_per_session = %1",
                 config.one_connection_per_session); // Throws
    logger.debug("Config param: connect_timeout = %1 ms",
                 config.connect_timeout); // Throws
    logger.debug("Config param: connection_linger_time = %1 ms",
                 config.connection_linger_time); // Throws
    logger.debug("Config param: ping_keepalive_period = %1 ms",
                 config.ping_keepalive_period); // Throws
    logger.debug("Config param: pong_keepalive_timeout = %1 ms",
                 config.pong_keepalive_timeout); // Throws
    logger.debug("Config param: fast_reconnect_limit = %1 ms",
                 config.fast_reconnect_limit); // Throws
    logger.debug("Config param: disable_upload_compaction = %1",
                 config.disable_upload_compaction); // Throws
    logger.debug("Config param: disable_sync_to_disk = %1",
                 config.disable_sync_to_disk); // Throws

    if (config.reconnect_mode != ReconnectMode::normal) {
        logger.warn("Testing/debugging feature 'nonnormal reconnect mode' enabled - "
                    "never do this in production!");
    }

    if (config.dry_run) {
        logger.warn("Testing/debugging feature 'dry run' enabled - "
                    "never do this in production!");
    }

    REALM_ASSERT_EX(m_socket_provider, "Must provide socket provider in sync Client config");

    if (m_one_connection_per_session) {
        // FIXME: Re-enable this warning when the load balancer is able to handle
        // multiplexing.
        //        logger.warn("Testing/debugging feature 'one connection per session' enabled - "
        //            "never do this in production");
    }

    if (config.disable_upload_activation_delay) {
        logger.warn("Testing/debugging feature 'disable_upload_activation_delay' enabled - "
                    "never do this in production");
    }

    if (config.disable_sync_to_disk) {
        logger.warn("Testing/debugging feature 'disable_sync_to_disk' enabled - "
                    "never do this in production");
    }

    m_actualize_and_finalize = create_trigger([this](Status status) {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);
        actualize_and_finalize_session_wrappers(); // Throws
    });
}


void ClientImpl::post(SyncSocketProvider::FunctionHandler&& handler)
{
    REALM_ASSERT(m_socket_provider);
    {
        std::lock_guard lock(m_drain_mutex);
        ++m_outstanding_posts;
        m_drained = false;
    }
    m_socket_provider->post([handler = std::move(handler), this](Status status) {
        handler(status);

        std::lock_guard lock(m_drain_mutex);
        --m_outstanding_posts;
        m_drain_cv.notify_all();
    });
}


void ClientImpl::drain_connections()
{
    logger.debug("Draining connections during sync client shutdown");
    for (auto& server_slot_pair : m_server_slots) {
        auto& server_slot = server_slot_pair.second;

        if (server_slot.connection) {
            auto& conn = server_slot.connection;
            conn->force_close();
        }
        else {
            for (auto& conn_pair : server_slot.alt_connections) {
                conn_pair.second->force_close();
            }
        }
    }
}


SyncSocketProvider::SyncTimer ClientImpl::create_timer(std::chrono::milliseconds delay,
                                                       SyncSocketProvider::FunctionHandler&& handler)
{
    REALM_ASSERT(m_socket_provider);
    {
        std::lock_guard lock(m_drain_mutex);
        ++m_outstanding_posts;
        m_drained = false;
    }
    return m_socket_provider->create_timer(delay, [handler = std::move(handler), this](Status status) {
        handler(status);

        std::lock_guard lock(m_drain_mutex);
        --m_outstanding_posts;
        m_drain_cv.notify_all();
    });
}


ClientImpl::SyncTrigger ClientImpl::create_trigger(SyncSocketProvider::FunctionHandler&& handler)
{
    REALM_ASSERT(m_socket_provider);
    return std::make_unique<Trigger<ClientImpl>>(this, std::move(handler));
}

Connection::~Connection()
{
    if (m_websocket_sentinel) {
        m_websocket_sentinel->destroyed = true;
        m_websocket_sentinel.reset();
    }
}

void Connection::activate()
{
    REALM_ASSERT(m_on_idle);
    m_activated = true;
    if (m_num_active_sessions == 0)
        m_on_idle->trigger();
    // We cannot in general connect immediately, because a prior failure to
    // connect may require a delay before reconnecting (see `m_reconnect_info`).
    initiate_reconnect_wait(); // Throws
}


void Connection::activate_session(std::unique_ptr<Session> sess)
{
    REALM_ASSERT(&sess->m_conn == this);
    Session& sess_2 = *sess;
    session_ident_type ident = sess->m_ident;
    auto p = m_sessions.emplace(ident, std::move(sess)); // Throws
    bool was_inserted = p.second;
    REALM_ASSERT(was_inserted);
    sess_2.activate(); // Throws
    if (m_state == ConnectionState::connected) {
        bool fast_reconnect = false;
        sess_2.connection_established(fast_reconnect); // Throws
    }
    ++m_num_active_sessions;
}


void Connection::initiate_session_deactivation(Session* sess)
{
    REALM_ASSERT(&sess->m_conn == this);
    if (REALM_UNLIKELY(--m_num_active_sessions == 0)) {
        if (m_activated && m_state == ConnectionState::disconnected)
            m_on_idle->trigger();
    }
    sess->initiate_deactivation(); // Throws
    if (sess->m_state == Session::Deactivated) {
        // Session is now deactivated, so remove and destroy it.
        session_ident_type ident = sess->m_ident;
        m_sessions.erase(ident);
    }
}


void Connection::cancel_reconnect_delay()
{
    REALM_ASSERT(m_activated);

    if (m_reconnect_delay_in_progress) {
        if (m_nonzero_reconnect_delay)
            logger.detail("Canceling reconnect delay"); // Throws

        // Cancel the in-progress wait operation by destroying the timer
        // object. Destruction is needed in this case, because a new wait
        // operation might have to be initiated before the previous one
        // completes (its completion handler starts to execute), so the new wait
        // operation must be done on a new timer object.
        m_reconnect_disconnect_timer.reset();
        m_reconnect_delay_in_progress = false;
        m_reconnect_info.reset();
        initiate_reconnect_wait(); // Throws
        return;
    }
    if (m_state != ConnectionState::disconnected) {
        // A currently established connection, or an in-progress attempt to
        // establish the connection may be about to fail for a reason that
        // precedes the invocation of Session::cancel_reconnect_delay(). For
        // that reason, it is important that at least one new reconnect attempt
        // is initiated without delay after the invocation of
        // Session::cancel_reconnect_delay(). The operation that resets the
        // reconnect delay (ReconnectInfo::reset()) needs to be postponed,
        // because some parts of `m_reconnect_info` may get clobbered before
        // initiate_reconnect_wait() is called again.
        //
        // If a PONG message arrives, and it is a response to the urgent PING
        // message sent below, `m_reconnect_info.m_scheduled_reset` will be
        // reset back to false, because in that case, we know that the
        // connection was not about to fail for a reason that preceded the
        // invocation of cancel_reconnect_delay().
        m_reconnect_info.m_scheduled_reset = true;
        m_ping_after_scheduled_reset_of_reconnect_info = false;

        schedule_urgent_ping(); // Throws
        return;
    }
    // Nothing to do in this case. The next reconnect attemp will be made as
    // soon as there are any sessions that are both active and unsuspended.
}


void Connection::force_close()
{
    if (m_disconnect_delay_in_progress || m_reconnect_delay_in_progress) {
        m_reconnect_disconnect_timer.reset();
        m_disconnect_delay_in_progress = false;
        m_reconnect_delay_in_progress = false;
    }

    REALM_ASSERT(m_num_active_unsuspended_sessions == 0);
    REALM_ASSERT(m_num_active_sessions == 0);
    if (m_state == ConnectionState::disconnected) {
        return;
    }

    voluntary_disconnect();
    logger.info("Force disconnected");
}


void Connection::websocket_connected_handler(const std::string& protocol)
{
    if (!protocol.empty()) {
        std::string_view expected_prefix =
            is_flx_sync_connection() ? get_flx_websocket_protocol_prefix() : get_pbs_websocket_protocol_prefix();
        // FIXME: Use std::string_view::begins_with() in C++20.
        auto prefix_matches = [&](std::string_view other) {
            return protocol.size() >= other.size() && (protocol.substr(0, other.size()) == other);
        };
        if (prefix_matches(expected_prefix)) {
            util::MemoryInputStream in;
            in.set_buffer(protocol.data() + expected_prefix.size(), protocol.data() + protocol.size());
            in.imbue(std::locale::classic());
            in.unsetf(std::ios_base::skipws);
            int value_2 = 0;
            in >> value_2;
            if (in && in.eof() && value_2 >= 0) {
                bool good_version =
                    (value_2 >= get_oldest_supported_protocol_version() && value_2 <= get_current_protocol_version());
                if (good_version) {
                    logger.detail("Negotiated protocol version: %1", value_2);
                    m_negotiated_protocol_version = value_2;
                    handle_connection_established(); // Throws
                    return;
                }
            }
        }
        logger.error("Bad protocol info from server: '%1'", protocol); // Throws
    }
    else {
        logger.error("Missing protocol info from server"); // Throws
    }
    m_reconnect_info.m_reason = ConnectionTerminationReason::bad_headers_in_http_response;
    bool is_fatal = true;
    close_due_to_client_side_error(ClientError::bad_protocol_from_server, std::nullopt, is_fatal); // Throws
}


bool Connection::websocket_binary_message_received(util::Span<const char> data)
{
    std::error_code ec;
    using sf = SimulatedFailure;
    if (sf::trigger(sf::sync_client__read_head, ec)) {
        read_or_write_error(ec);
        return bool(m_websocket);
    }

    handle_message_received(data);
    return bool(m_websocket);
}


void Connection::websocket_error_handler()
{
    m_websocket_error_received = true;
}


bool Connection::websocket_closed_handler(bool was_clean, Status status)
{
    logger.info("Closing the websocket with status='%1', was_clean='%2'", status, was_clean);
    // Return early.
    if (status.is_ok()) {
        return bool(m_websocket);
    }

    auto&& status_code = status.code();
    std::error_code error_code{static_cast<int>(status_code), websocket::websocket_close_status_category()};

    // TODO: Use a switch statement once websocket errors have their own category in exception unification.
    if (status_code == ErrorCodes::ResolveFailed || status_code == ErrorCodes::ConnectionFailed) {
        m_reconnect_info.m_reason = ConnectionTerminationReason::connect_operation_failed;
        constexpr bool try_again = true;
        involuntary_disconnect(SessionErrorInfo{error_code, try_again}); // Throws
    }
    else if (status_code == ErrorCodes::ReadError || status_code == ErrorCodes::WriteError) {
        read_or_write_error(error_code);
    }
    else if (status_code == ErrorCodes::WebSocket_GoingAway || status_code == ErrorCodes::WebSocket_ProtocolError ||
             status_code == ErrorCodes::WebSocket_UnsupportedData || status_code == ErrorCodes::WebSocket_Reserved ||
             status_code == ErrorCodes::WebSocket_InvalidPayloadData ||
             status_code == ErrorCodes::WebSocket_PolicyViolation ||
             status_code == ErrorCodes::WebSocket_InavalidExtension) {
        m_reconnect_info.m_reason = ConnectionTerminationReason::websocket_protocol_violation;
        constexpr bool try_again = true;
        SessionErrorInfo error_info{error_code, status.reason(), try_again};
        involuntary_disconnect(std::move(error_info));
    }
    else if (status_code == ErrorCodes::WebSocket_MessageTooBig) {
        m_reconnect_info.m_reason = ConnectionTerminationReason::websocket_protocol_violation;
        constexpr bool try_again = true;
        auto ec = make_error_code(ProtocolError::limits_exceeded);
        auto message = util::format(
            "Sync websocket closed because the server received a message that was too large: %1", status.reason());
        SessionErrorInfo error_info(ec, message, try_again);
        error_info.server_requests_action = ProtocolErrorInfo::Action::ClientReset;
        involuntary_disconnect(std::move(error_info));
    }
    else if (status_code == ErrorCodes::WebSocket_TLSHandshakeFailed) {
        error_code = ClientError::ssl_server_cert_rejected;
        constexpr bool is_fatal = true;
        m_reconnect_info.m_reason = ConnectionTerminationReason::ssl_certificate_rejected;
        close_due_to_client_side_error(error_code, std::nullopt, is_fatal); // Throws
    }
    else if (status_code == ErrorCodes::WebSocket_Client_Too_Old) {
        error_code = ClientError::client_too_old_for_server;
        constexpr bool is_fatal = true;
        m_reconnect_info.m_reason = ConnectionTerminationReason::http_response_says_fatal_error;
        close_due_to_client_side_error(error_code, std::nullopt, is_fatal); // Throws
    }
    else if (status_code == ErrorCodes::WebSocket_Client_Too_New) {
        error_code = ClientError::client_too_new_for_server;
        constexpr bool is_fatal = true;
        m_reconnect_info.m_reason = ConnectionTerminationReason::http_response_says_fatal_error;
        close_due_to_client_side_error(error_code, std::nullopt, is_fatal); // Throws
    }
    else if (status_code == ErrorCodes::WebSocket_Protocol_Mismatch) {
        error_code = ClientError::protocol_mismatch;
        constexpr bool is_fatal = true;
        m_reconnect_info.m_reason = ConnectionTerminationReason::http_response_says_fatal_error;
        close_due_to_client_side_error(error_code, std::nullopt, is_fatal); // Throws
    }
    else if (status_code == ErrorCodes::WebSocket_Retry_Error || status_code == ErrorCodes::WebSocket_Forbidden) {
        constexpr bool is_fatal = true;
        m_reconnect_info.m_reason = ConnectionTerminationReason::http_response_says_fatal_error;
        close_due_to_client_side_error(error_code, std::nullopt, is_fatal); // Throws
    }
    else if (status_code == ErrorCodes::WebSocket_Unauthorized ||
             status_code == ErrorCodes::WebSocket_MovedPermanently ||
             status_code == ErrorCodes::WebSocket_InternalServerError ||
             status_code == ErrorCodes::WebSocket_AbnormalClosure ||
             status_code == ErrorCodes::WebSocket_Retry_Error) {
        constexpr bool is_fatal = false;
        m_reconnect_info.m_reason = ConnectionTerminationReason::http_response_says_nonfatal_error;
        close_due_to_client_side_error(error_code, std::nullopt, is_fatal); // Throws
    }

    return bool(m_websocket);
}

// Guarantees that handle_reconnect_wait() is never called from within the
// execution of initiate_reconnect_wait() (no callback reentrance).
void Connection::initiate_reconnect_wait()
{
    REALM_ASSERT(m_activated);
    REALM_ASSERT(!m_reconnect_delay_in_progress);
    REALM_ASSERT(!m_disconnect_delay_in_progress);

    using milliseconds_lim = ReconnectInfo::milliseconds_lim;

    constexpr milliseconds_type min_delay = 1000;   // 1 second (barring deductions)
    constexpr milliseconds_type max_delay = 300000; // 5 minutes

    // Delay must increase when scaled by a factor greater than 1.
    static_assert(min_delay > 0, "");
    static_assert(max_delay >= min_delay, "");

    if (m_reconnect_info.m_scheduled_reset)
        m_reconnect_info.reset();

    bool infinite_delay = false;
    milliseconds_type remaining_delay = 0;
    if (!m_reconnect_info.m_reason) {
        // Delay in progress. `m_time_point` specifies when the delay expires.
        if (m_reconnect_info.m_time_point == milliseconds_lim::max()) {
            infinite_delay = true;
        }
        else {
            milliseconds_type now = monotonic_clock_now();
            if (now < m_reconnect_info.m_time_point)
                remaining_delay = m_reconnect_info.m_time_point - now;
        }
    }
    else {
        // Compute a new reconnect delay
        bool zero_delay = false;
        switch (m_client.get_reconnect_mode()) {
            case ReconnectMode::normal:
                break;
            case ReconnectMode::testing:
                if (was_voluntary(*m_reconnect_info.m_reason)) {
                    zero_delay = true;
                }
                else {
                    infinite_delay = true;
                }
                break;
        }

        // Calculate delay
        milliseconds_type delay = 0;
        bool record_delay_as_zero = false;
        if (!zero_delay && !infinite_delay) {
            switch (*m_reconnect_info.m_reason) {
                case ConnectionTerminationReason::closed_voluntarily:
                case ConnectionTerminationReason::read_or_write_error:
                case ConnectionTerminationReason::pong_timeout:
                    // Minimum delay after successful connect operation
                    delay = min_delay;
                    break;
                case ConnectionTerminationReason::connect_operation_failed:
                case ConnectionTerminationReason::http_response_says_nonfatal_error:
                case ConnectionTerminationReason::sync_connect_timeout:
                    // The last attempt at establishing a connection failed. In
                    // this case, the reconnect delay will increase with the
                    // number of consecutive failures.
                    delay = m_reconnect_info.m_delay;
                    // Double the previous delay
                    if (util::int_multiply_with_overflow_detect(delay, 2))
                        delay = milliseconds_lim::max();
                    // Raise to minimum delay in case last delay was zero
                    if (delay < min_delay)
                        delay = min_delay;
                    // Cut off at a fixed maximum delay
                    if (delay > max_delay)
                        delay = max_delay;
                    break;
                case ConnectionTerminationReason::server_said_try_again_later:
                    record_delay_as_zero = true;
                    delay = m_reconnect_info.m_try_again_delay_info.delay_interval().count();
                    break;
                case ConnectionTerminationReason::ssl_certificate_rejected:
                case ConnectionTerminationReason::ssl_protocol_violation:
                case ConnectionTerminationReason::websocket_protocol_violation:
                case ConnectionTerminationReason::http_response_says_fatal_error:
                case ConnectionTerminationReason::bad_headers_in_http_response:
                case ConnectionTerminationReason::sync_protocol_violation:
                case ConnectionTerminationReason::server_said_do_not_reconnect:
                case ConnectionTerminationReason::missing_protocol_feature:
                    // Use a significantly longer delay in this case to avoid
                    // disturbing the server too much. It does make sense to try
                    // again eventually, because the server may get restarted in
                    // such a way the that problem goes away.
                    delay = 3600000L; // 1 hour
                    record_delay_as_zero = true;
                    break;
            }

            // Make a randomized deduction of up to 25% to prevent a large
            // number of clients from trying to reconnect in synchronicity.
            auto distr = std::uniform_int_distribution<milliseconds_type>(0, delay / 4);
            milliseconds_type randomized_deduction = distr(m_client.get_random());
            delay -= randomized_deduction;

            // Finally, deduct the time that has already passed since the last
            // connection attempt.
            milliseconds_type now = monotonic_clock_now();
            REALM_ASSERT(now >= m_reconnect_info.m_time_point);
            milliseconds_type time_since_delay_start = now - m_reconnect_info.m_time_point;
            if (time_since_delay_start < delay)
                remaining_delay = delay - time_since_delay_start;
        }

        // Calculate expiration time for delay
        milliseconds_type time_point;
        if (infinite_delay) {
            time_point = milliseconds_lim::max();
        }
        else {
            time_point = m_reconnect_info.m_time_point;
            if (util::int_add_with_overflow_detect(time_point, delay))
                time_point = milliseconds_lim::max();
        }

        // Indicate that a new delay is now in progress
        m_reconnect_info.m_reason = util::none;
        m_reconnect_info.m_time_point = time_point;
        if (record_delay_as_zero) {
            m_reconnect_info.m_delay = 0;
        }
        else {
            m_reconnect_info.m_delay = delay;
        }
    }

    if (infinite_delay) {
        logger.detail("Reconnection delayed indefinitely"); // Throws
        // Not actually starting a timer corresponds to an infinite wait
        m_reconnect_delay_in_progress = true;
        m_nonzero_reconnect_delay = true;
        return;
    }

    if (remaining_delay > 0) {
        logger.detail("Allowing reconnection in %1 milliseconds",
                      remaining_delay); // Throws
    }

    m_reconnect_disconnect_timer =
        m_client.create_timer(std::chrono::milliseconds(remaining_delay), [this](Status status) {
            // If the operation is aborted, the connection object may have been
            // destroyed.
            if (status != ErrorCodes::OperationAborted)
                handle_reconnect_wait(status); // Throws
        });                                    // Throws
    m_reconnect_delay_in_progress = true;
    m_nonzero_reconnect_delay = (remaining_delay > 0);
}


void Connection::handle_reconnect_wait(Status status)
{
    if (!status.is_ok()) {
        REALM_ASSERT(status != ErrorCodes::OperationAborted);
        throw ExceptionForStatus(status);
    }

    m_reconnect_delay_in_progress = false;

    if (m_num_active_unsuspended_sessions > 0)
        initiate_reconnect(); // Throws
}

struct Connection::WebSocketObserverShim : public sync::WebSocketObserver {
    explicit WebSocketObserverShim(Connection* conn)
        : conn(conn)
        , sentinel(conn->m_websocket_sentinel)
    {
    }

    Connection* conn;
    util::bind_ptr<LifecycleSentinel> sentinel;

    void websocket_connected_handler(const std::string& protocol) override
    {
        if (sentinel->destroyed) {
            return;
        }

        return conn->websocket_connected_handler(protocol);
    }

    void websocket_error_handler() override
    {
        if (sentinel->destroyed) {
            return;
        }

        conn->websocket_error_handler();
    }

    bool websocket_binary_message_received(util::Span<const char> data) override
    {
        if (sentinel->destroyed) {
            return false;
        }

        return conn->websocket_binary_message_received(data);
    }

    bool websocket_closed_handler(bool was_clean, Status status) override
    {
        if (sentinel->destroyed) {
            return true;
        }

        return conn->websocket_closed_handler(was_clean, std::move(status));
    }
};

void Connection::initiate_reconnect()
{
    REALM_ASSERT(m_activated);

    m_state = ConnectionState::connecting;
    report_connection_state_change(ConnectionState::connecting); // Throws
    if (m_websocket_sentinel) {
        m_websocket_sentinel->destroyed = true;
    }
    m_websocket_sentinel = util::make_bind<LifecycleSentinel>();
    m_websocket.reset();

    // In most cases, the reconnect delay will be counting from the point in
    // time of the initiation of the last reconnect operation (the initiation of
    // the DNS resolve operation). It may also be counting from the point in
    // time of the reception of an ERROR message, but in that case we can simply
    // update `m_reconnect_info.m_time_point`.
    m_reconnect_info.m_time_point = monotonic_clock_now();

    // Watchdog
    initiate_connect_wait(); // Throws

    // There are three outcomes of the connection operation; success, failure,
    // or cancellation. Since it is complicated to update the connection
    // termination reason on cancellation, we mark it as voluntarily closed now, and then
    // change it if the outcome ends up being success or failure.
    m_reconnect_info.m_reason = ConnectionTerminationReason::closed_voluntarily;

    std::vector<std::string> sec_websocket_protocol;
    {
        auto protocol_prefix =
            is_flx_sync_connection() ? get_flx_websocket_protocol_prefix() : get_pbs_websocket_protocol_prefix();
        int min = get_oldest_supported_protocol_version();
        int max = get_current_protocol_version();
        REALM_ASSERT(min <= max);
        // List protocol version in descending order to ensure that the server
        // selects the highest possible version.
        for (int version = max; version >= min; --version) {
            sec_websocket_protocol.push_back(util::format("%1%2", protocol_prefix, version)); // Throws
        }
    }

    m_websocket_error_received = false;
    m_websocket =
        m_client.m_socket_provider->connect(std::make_unique<WebSocketObserverShim>(this),
                                            WebSocketEndpoint{
                                                m_address,
                                                m_port,
                                                get_http_request_path(),
                                                std::move(sec_websocket_protocol),
                                                is_ssl(m_protocol_envelope),
                                                /// DEPRECATED - The following will be removed in a future release
                                                {m_custom_http_headers.begin(), m_custom_http_headers.end()},
                                                m_verify_servers_ssl_certificate,
                                                m_ssl_trust_certificate_path,
                                                m_ssl_verify_callback,
                                                m_proxy_config,
                                            });
}


void Connection::initiate_connect_wait()
{
    // Deploy a watchdog to enforce an upper bound on the time it can take to
    // fully establish the connection (including SSL and WebSocket
    // handshakes). Without such a watchdog, connect operations could take very
    // long, or even indefinite time.
    milliseconds_type time = m_client.m_connect_timeout;

    m_connect_timer = m_client.create_timer(std::chrono::milliseconds(time), [this](Status status) {
        // If the operation is aborted, the connection object may have been
        // destroyed.
        if (status != ErrorCodes::OperationAborted)
            handle_connect_wait(status); // Throws
    });                                  // Throws
}


void Connection::handle_connect_wait(Status status)
{
    if (!status.is_ok()) {
        REALM_ASSERT(status != ErrorCodes::OperationAborted);
        throw ExceptionForStatus(status);
    }

    REALM_ASSERT(m_state == ConnectionState::connecting);
    m_reconnect_info.m_reason = ConnectionTerminationReason::sync_connect_timeout;
    logger.info("Connect timeout"); // Throws
    constexpr bool try_again = true;
    involuntary_disconnect(SessionErrorInfo{ClientError::connect_timeout, try_again}); // Throws
}


void Connection::handle_connection_established()
{
    // Cancel connect timeout watchdog
    m_connect_timer.reset();

    m_state = ConnectionState::connected;

    // TODO(RCORE-1380) get this information in-band rather than from the websocket.
    if (auto coid = m_websocket->get_appservices_request_id(); !coid.empty()) {
        logger.info("Connected to app services with request id: \"%1\"", coid);
    }

    milliseconds_type now = monotonic_clock_now();
    m_pong_wait_started_at = now; // Initially, no time was spent waiting for a PONG message
    initiate_ping_delay(now);     // Throws

    bool fast_reconnect = false;
    if (m_disconnect_has_occurred) {
        milliseconds_type time = now - m_disconnect_time;
        if (time <= m_client.m_fast_reconnect_limit)
            fast_reconnect = true;
    }

    for (auto& p : m_sessions) {
        Session& sess = *p.second;
        sess.connection_established(fast_reconnect); // Throws
    }

    report_connection_state_change(ConnectionState::connected); // Throws
}


void Connection::schedule_urgent_ping()
{
    REALM_ASSERT(m_state != ConnectionState::disconnected);
    if (m_ping_delay_in_progress) {
        m_heartbeat_timer.reset();
        m_ping_delay_in_progress = false;
        m_minimize_next_ping_delay = true;
        milliseconds_type now = monotonic_clock_now();
        initiate_ping_delay(now); // Throws
        return;
    }
    REALM_ASSERT(m_state == ConnectionState::connecting || m_waiting_for_pong);
    if (!m_send_ping)
        m_minimize_next_ping_delay = true;
}


void Connection::initiate_ping_delay(milliseconds_type now)
{
    REALM_ASSERT(!m_ping_delay_in_progress);
    REALM_ASSERT(!m_waiting_for_pong);
    REALM_ASSERT(!m_send_ping);

    milliseconds_type delay = 0;
    if (!m_minimize_next_ping_delay) {
        delay = m_client.m_ping_keepalive_period;
        // Make a randomized deduction of up to 10%, or up to 100% if this is
        // the first PING message to be sent since the connection was
        // established. The purpose of this randomized deduction is to reduce
        // the risk of many connections sending PING messages simultaneously to
        // the server.
        milliseconds_type max_deduction = (m_ping_sent ? delay / 10 : delay);
        auto distr = std::uniform_int_distribution<milliseconds_type>(0, max_deduction);
        milliseconds_type randomized_deduction = distr(m_client.get_random());
        delay -= randomized_deduction;
        // Deduct the time spent waiting for PONG
        REALM_ASSERT(now >= m_pong_wait_started_at);
        milliseconds_type spent_time = now - m_pong_wait_started_at;
        if (spent_time < delay) {
            delay -= spent_time;
        }
        else {
            delay = 0;
        }
    }
    else {
        m_minimize_next_ping_delay = false;
    }


    m_ping_delay_in_progress = true;

    m_heartbeat_timer = m_client.create_timer(std::chrono::milliseconds(delay), [this](Status status) {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        handle_ping_delay();                                    // Throws
    });                                                         // Throws
    logger.debug("Will emit a ping in %1 milliseconds", delay); // Throws
}


void Connection::handle_ping_delay()
{
    REALM_ASSERT(m_ping_delay_in_progress);
    m_ping_delay_in_progress = false;
    m_send_ping = true;

    initiate_pong_timeout(); // Throws

    if (m_state == ConnectionState::connected && !m_sending)
        send_next_message(); // Throws
}


void Connection::initiate_pong_timeout()
{
    REALM_ASSERT(!m_ping_delay_in_progress);
    REALM_ASSERT(!m_waiting_for_pong);
    REALM_ASSERT(m_send_ping);

    m_waiting_for_pong = true;
    m_pong_wait_started_at = monotonic_clock_now();

    milliseconds_type time = m_client.m_pong_keepalive_timeout;
    m_heartbeat_timer = m_client.create_timer(std::chrono::milliseconds(time), [this](Status status) {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        handle_pong_timeout(); // Throws
    });                        // Throws
}


void Connection::handle_pong_timeout()
{
    REALM_ASSERT(m_waiting_for_pong);
    logger.debug("Timeout on reception of PONG message"); // Throws
    m_reconnect_info.m_reason = ConnectionTerminationReason::pong_timeout;
    close_due_to_client_side_error(ClientError::pong_timeout, std::nullopt, false);
}


void Connection::initiate_write_message(const OutputBuffer& out, Session* sess)
{
    // Stop sending messages if an websocket error was received.
    if (m_websocket_error_received)
        return;

    m_websocket->async_write_binary(out.as_span(), [this, sentinel = m_websocket_sentinel](Status status) {
        if (sentinel->destroyed) {
            return;
        }
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        handle_write_message(); // Throws
    });                         // Throws
    m_sending_session = sess;
    m_sending = true;
}


void Connection::handle_write_message()
{
    m_sending_session->message_sent(); // Throws
    if (m_sending_session->m_state == Session::Deactivated) {
        // Session is now deactivated, so remove and destroy it.
        session_ident_type ident = m_sending_session->m_ident;
        m_sessions.erase(ident);
    }
    m_sending_session = nullptr;
    m_sending = false;
    send_next_message(); // Throws
}


void Connection::send_next_message()
{
    REALM_ASSERT(m_state == ConnectionState::connected);
    REALM_ASSERT(!m_sending_session);
    REALM_ASSERT(!m_sending);
    if (m_send_ping) {
        send_ping(); // Throws
        return;
    }
    while (!m_sessions_enlisted_to_send.empty()) {
        // The state of being connected is not supposed to be able to change
        // across this loop thanks to the "no callback reentrance" guarantee
        // provided by Websocket::async_write_text(), and friends.
        REALM_ASSERT(m_state == ConnectionState::connected);

        Session& sess = *m_sessions_enlisted_to_send.front();
        m_sessions_enlisted_to_send.pop_front();
        sess.send_message(); // Throws

        if (sess.m_state == Session::Deactivated) {
            // Session is now deactivated, so remove and destroy it.
            session_ident_type ident = sess.m_ident;
            m_sessions.erase(ident);
        }

        // An enlisted session may choose to not send a message. In that case,
        // we should pass the opportunity to the next enlisted session.
        if (m_sending)
            break;
    }
}


void Connection::send_ping()
{
    REALM_ASSERT(!m_ping_delay_in_progress);
    REALM_ASSERT(m_waiting_for_pong);
    REALM_ASSERT(m_send_ping);

    m_send_ping = false;
    if (m_reconnect_info.m_scheduled_reset)
        m_ping_after_scheduled_reset_of_reconnect_info = true;

    m_last_ping_sent_at = monotonic_clock_now();
    logger.debug("Sending: PING(timestamp=%1, rtt=%2)", m_last_ping_sent_at,
                 m_previous_ping_rtt); // Throws

    ClientProtocol& protocol = get_client_protocol();
    OutputBuffer& out = get_output_buffer();
    protocol.make_ping(out, m_last_ping_sent_at, m_previous_ping_rtt); // Throws
    initiate_write_ping(out);                                          // Throws
    m_ping_sent = true;
}


void Connection::initiate_write_ping(const OutputBuffer& out)
{
    m_websocket->async_write_binary(out.as_span(), [this, sentinel = m_websocket_sentinel](Status status) {
        if (sentinel->destroyed) {
            return;
        }
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        handle_write_ping(); // Throws
    });                      // Throws
    m_sending = true;
}


void Connection::handle_write_ping()
{
    REALM_ASSERT(m_sending);
    REALM_ASSERT(!m_sending_session);
    m_sending = false;
    send_next_message(); // Throws
}


void Connection::handle_message_received(util::Span<const char> data)
{
    // parse_message_received() parses the message and calls the proper handler
    // on the Connection object (this).
    get_client_protocol().parse_message_received<Connection>(*this, std::string_view(data.data(), data.size()));
}


void Connection::initiate_disconnect_wait()
{
    REALM_ASSERT(!m_reconnect_delay_in_progress);

    if (m_disconnect_delay_in_progress) {
        m_reconnect_disconnect_timer.reset();
        m_disconnect_delay_in_progress = false;
    }

    milliseconds_type time = m_client.m_connection_linger_time;

    m_reconnect_disconnect_timer = m_client.create_timer(std::chrono::milliseconds(time), [this](Status status) {
        // If the operation is aborted, the connection object may have been
        // destroyed.
        if (status != ErrorCodes::OperationAborted)
            handle_disconnect_wait(status); // Throws
    });                                     // Throws
    m_disconnect_delay_in_progress = true;
}


void Connection::handle_disconnect_wait(Status status)
{
    if (!status.is_ok()) {
        REALM_ASSERT(status != ErrorCodes::OperationAborted);
        throw ExceptionForStatus(status);
    }

    m_disconnect_delay_in_progress = false;

    REALM_ASSERT(m_state != ConnectionState::disconnected);
    if (m_num_active_unsuspended_sessions == 0) {
        if (m_client.m_connection_linger_time > 0)
            logger.detail("Linger time expired"); // Throws
        voluntary_disconnect();                   // Throws
        logger.info("Disconnected");              // Throws
    }
}


void Connection::read_or_write_error(std::error_code ec)
{
    m_reconnect_info.m_reason = ConnectionTerminationReason::read_or_write_error;
    bool is_fatal = false;
    close_due_to_client_side_error(ec, std::nullopt, is_fatal); // Throws
}


void Connection::close_due_to_protocol_error(std::error_code ec, std::optional<std::string_view> msg)
{
    m_reconnect_info.m_reason = ConnectionTerminationReason::sync_protocol_violation;
    bool is_fatal = true;                              // A sync protocol violation is a fatal error
    close_due_to_client_side_error(ec, msg, is_fatal); // Throws
}


// Close connection due to error discovered on the client-side.
void Connection::close_due_to_client_side_error(std::error_code ec, std::optional<std::string_view> msg,
                                                bool is_fatal)
{
    logger.info("Connection closed due to error"); // Throws
    const bool try_again = !is_fatal;
    std::string message = ec.message();
    if (msg) {
        message += ": ";
        message += *msg;
    }
    involuntary_disconnect(SessionErrorInfo{ec, message, try_again}); // Throws
}


// Close connection due to error discovered on the server-side, and then
// reported to the client by way of a connection-level ERROR message.
void Connection::close_due_to_server_side_error(ProtocolError error_code, const ProtocolErrorInfo& info)
{
    if (info.try_again) {
        m_reconnect_info.m_reason = ConnectionTerminationReason::server_said_try_again_later;
    }
    else {
        m_reconnect_info.m_reason = ConnectionTerminationReason::server_said_do_not_reconnect;
    }

    m_reconnect_info.m_try_again_delay_info.update(info);

    // When the server asks us to reconnect later, it is important to make the
    // reconnect delay start at the time of the reception of the ERROR message,
    // rather than at the initiation of the connection, as is usually the
    // case. This is because the message may arrive at a point in time where the
    // connection has been open for a long time, so if we let the delay count
    // from the initiation of the connection, it could easly end up as no delay
    // at all.
    m_reconnect_info.m_time_point = monotonic_clock_now();

    logger.info("Connection closed due to error reported by server: %1 (%2)", info.message,
                int(error_code)); // Throws

    std::error_code ec = make_error_code(error_code);
    involuntary_disconnect(SessionErrorInfo{info, ec}); // Throws
}


void Connection::disconnect(const SessionErrorInfo& info)
{
    // Cancel connect timeout watchdog
    m_connect_timer.reset();

    if (m_state == ConnectionState::connected) {
        m_disconnect_time = monotonic_clock_now();
        m_disconnect_has_occurred = true;

        // Sessions that are in the Deactivating state at this time can be
        // immediately discarded, in part because they are no longer enlisted to
        // send. Such sessions will be taken to the Deactivated state by
        // Session::connection_lost(), and then they will be removed from
        // `m_sessions`.
        auto i = m_sessions.begin(), end = m_sessions.end();
        while (i != end) {
            // Prevent invalidation of the main iterator when erasing elements
            auto j = i++;
            Session& sess = *j->second;
            sess.connection_lost(); // Throws
            if (sess.m_state == Session::Unactivated || sess.m_state == Session::Deactivated)
                m_sessions.erase(j);
        }
    }

    change_state_to_disconnected();

    m_ping_delay_in_progress = false;
    m_waiting_for_pong = false;
    m_send_ping = false;
    m_minimize_next_ping_delay = false;
    m_ping_after_scheduled_reset_of_reconnect_info = false;
    m_ping_sent = false;
    m_heartbeat_timer.reset();
    m_previous_ping_rtt = 0;

    m_websocket_sentinel->destroyed = true;
    m_websocket_sentinel.reset();
    m_websocket.reset();
    m_input_body_buffer.reset();
    m_sending_session = nullptr;
    m_sessions_enlisted_to_send.clear();
    m_sending = false;

    report_connection_state_change(ConnectionState::disconnected, info); // Throws
    initiate_reconnect_wait();                                           // Throws
}

bool Connection::is_flx_sync_connection() const noexcept
{
    return m_sync_mode != SyncServerMode::PBS;
}

void Connection::receive_pong(milliseconds_type timestamp)
{
    logger.debug("Received: PONG(timestamp=%1)", timestamp);

    bool legal_at_this_time = (m_waiting_for_pong && !m_send_ping);
    if (REALM_UNLIKELY(!legal_at_this_time)) {
        logger.error("Illegal message at this time");
        std::error_code ec = ClientError::bad_message_order;
        close_due_to_protocol_error(ec); // Throws
        return;
    }

    if (REALM_UNLIKELY(timestamp != m_last_ping_sent_at)) {
        logger.error("Bad timestamp in PONG message");
        std::error_code ec = ClientError::bad_timestamp;
        close_due_to_protocol_error(ec); // Throws
        return;
    }

    milliseconds_type now = monotonic_clock_now();
    milliseconds_type round_trip_time = now - timestamp;
    logger.debug("Round trip time was %1 milliseconds", round_trip_time);
    m_previous_ping_rtt = round_trip_time;

    // If this PONG message is a response to a PING mesage that was sent after
    // the last invocation of cancel_reconnect_delay(), then the connection is
    // still good, and we do not have to skip the next reconnect delay.
    if (m_ping_after_scheduled_reset_of_reconnect_info) {
        REALM_ASSERT(m_reconnect_info.m_scheduled_reset);
        m_ping_after_scheduled_reset_of_reconnect_info = false;
        m_reconnect_info.m_scheduled_reset = false;
    }

    m_heartbeat_timer.reset();
    m_waiting_for_pong = false;

    initiate_ping_delay(now); // Throws

    if (m_client.m_roundtrip_time_handler)
        m_client.m_roundtrip_time_handler(m_previous_ping_rtt); // Throws
}


void Connection::receive_error_message(const ProtocolErrorInfo& info, session_ident_type session_ident)
{
    Session* sess = nullptr;
    if (session_ident != 0) {
        sess = get_session(session_ident);
        if (REALM_UNLIKELY(!sess)) {
            logger.error("Bad session identifier in ERROR message, session_ident = %1",
                         session_ident);                                 // Throws
            close_due_to_protocol_error(ClientError::bad_session_ident); // Throws
            return;
        }
        std::error_code ec = sess->receive_error_message(info); // Throws
        if (ec) {
            close_due_to_protocol_error(ec); // Throws
            return;
        }

        if (sess->m_state == Session::Deactivated) {
            // Session is now deactivated, so remove and destroy it.
            session_ident_type ident = sess->m_ident;
            m_sessions.erase(ident);
        }
        return;
    }

    logger.info("Received: ERROR \"%1\" (error_code=%2, try_again=%3, session_ident=%4, error_action=%5)",
                info.message, info.raw_error_code, info.try_again, session_ident,
                info.server_requests_action); // Throws

    bool known_error_code = bool(get_protocol_error_message(info.raw_error_code));
    if (REALM_LIKELY(known_error_code)) {
        ProtocolError error_code = ProtocolError(info.raw_error_code);
        if (REALM_LIKELY(!is_session_level_error(error_code))) {
            close_due_to_server_side_error(error_code, info); // Throws
            return;
        }
        logger.error("Not a connection-level error code"); // Throws
    }
    else {
        logger.error("Unknown error code"); // Throws
    }
    close_due_to_protocol_error(ClientError::bad_error_code); // Throws
}


void Connection::receive_query_error_message(int raw_error_code, std::string_view message, int64_t query_version,
                                             session_ident_type session_ident)
{
    if (session_ident == 0) {
        logger.error("Received query error message for session ident 0."); // throws;
        return close_due_to_protocol_error(ClientError::bad_session_ident);
    }

    if (!is_flx_sync_connection()) {
        logger.error("Received query error message on a non-FLX sync connection");
        return close_due_to_protocol_error(ClientError::bad_protocol_from_server);
    }

    auto session = get_session(session_ident);
    if (!session) {
        logger.error("Bad session identifier in QUERY_ERROR mesage, session_ident = %1", session_ident); // throws
        return close_due_to_protocol_error(ClientError::bad_session_ident);                              // throws
    }

    if (auto ec = session->receive_query_error_message(raw_error_code, message, query_version)) {
        close_due_to_protocol_error(ec);
    }
}


void Connection::receive_ident_message(session_ident_type session_ident, SaltedFileIdent client_file_ident)
{
    Session* sess = get_session(session_ident);
    if (REALM_UNLIKELY(!sess)) {
        logger.error("Bad session identifier in IDENT message, session_ident = %1",
                     session_ident);                                 // Throws
        close_due_to_protocol_error(ClientError::bad_session_ident); // Throws
        return;
    }

    std::error_code ec = sess->receive_ident_message(client_file_ident); // Throws
    if (ec)
        close_due_to_protocol_error(ec); // Throws
}

void Connection::receive_download_message(session_ident_type session_ident, const SyncProgress& progress,
                                          std::uint_fast64_t downloadable_bytes, int64_t query_version,
                                          DownloadBatchState batch_state,
                                          const ReceivedChangesets& received_changesets)
{
    Session* sess = get_session(session_ident);
    if (REALM_UNLIKELY(!sess)) {
        logger.error("Bad session identifier in DOWNLOAD message, session_ident = %1",
                     session_ident);                                 // Throws
        close_due_to_protocol_error(ClientError::bad_session_ident); // Throws
        return;
    }

    sess->receive_download_message(progress, downloadable_bytes, batch_state, query_version,
                                   received_changesets); // Throws
}

void Connection::receive_mark_message(session_ident_type session_ident, request_ident_type request_ident)
{
    Session* sess = get_session(session_ident);
    if (REALM_UNLIKELY(!sess)) {
        logger.error("Bad session identifier (%1) in MARK message", session_ident); // Throws
        close_due_to_protocol_error(ClientError::bad_session_ident);                // Throws
        return;
    }

    std::error_code ec = sess->receive_mark_message(request_ident); // Throws
    if (ec)
        close_due_to_protocol_error(ec); // Throws
}


void Connection::receive_unbound_message(session_ident_type session_ident)
{
    Session* sess = get_session(session_ident);
    if (REALM_UNLIKELY(!sess)) {
        logger.error("Bad session identifier in UNBOUND message, session_ident = %1",
                     session_ident);                                 // Throws
        close_due_to_protocol_error(ClientError::bad_session_ident); // Throws
        return;
    }

    std::error_code ec = sess->receive_unbound_message(); // Throws
    if (ec) {
        close_due_to_protocol_error(ec); // Throws
        return;
    }

    if (sess->m_state == Session::Deactivated) {
        // Session is now deactivated, so remove and destroy it.
        session_ident_type ident = sess->m_ident;
        m_sessions.erase(ident);
    }
}


void Connection::receive_test_command_response(session_ident_type session_ident, request_ident_type request_ident,
                                               std::string_view body)
{
    Session* sess = get_session(session_ident);
    if (REALM_UNLIKELY(!sess)) {
        logger.error("Bad session identifier in TEST_COMMAND response message, session_ident = %1",
                     session_ident);                                 // Throws
        close_due_to_protocol_error(ClientError::bad_session_ident); // Throws
        return;
    }

    if (auto ec = sess->receive_test_command_response(request_ident, body)) {
        close_due_to_protocol_error(ec);
    }
}


void Connection::handle_protocol_error(ClientProtocol::Error error)
{
    switch (error) {
        case ClientProtocol::Error::unknown_message:
            close_due_to_protocol_error(ClientError::unknown_message); // Throws
            break;
        case ClientProtocol::Error::bad_syntax:
            close_due_to_protocol_error(ClientError::bad_syntax); // Throws
            break;
        case ClientProtocol::Error::limits_exceeded:
            close_due_to_protocol_error(ClientError::limits_exceeded); // Throws
            break;
        case ClientProtocol::Error::bad_decompression:
            close_due_to_protocol_error(ClientError::bad_compression); // Throws
            break;
        case ClientProtocol::Error::bad_changeset_header_syntax:
            close_due_to_protocol_error(ClientError::bad_changeset_header_syntax); // Throws
            break;
        case ClientProtocol::Error::bad_changeset_size:
            close_due_to_protocol_error(ClientError::bad_changeset_size); // Throws
            break;
        case ClientProtocol::Error::bad_server_version:
            close_due_to_protocol_error(ClientError::bad_server_version); // Throws
            break;
        case ClientProtocol::Error::bad_error_code:
            close_due_to_protocol_error(ClientError::bad_error_code); // Throws
            break;
    }
}


// Sessions are guaranteed to be granted the opportunity to send a message in
// the order that they enlist. Note that this is important to ensure
// nonoverlapping communication with the server for consecutive sessions
// associated with the same Realm file.
//
// CAUTION: The specified session may get destroyed before this function
// returns, but only if its Session::send_message() puts it into the Deactivated
// state.
void Connection::enlist_to_send(Session* sess)
{
    REALM_ASSERT(m_state == ConnectionState::connected);
    m_sessions_enlisted_to_send.push_back(sess); // Throws
    if (!m_sending)
        send_next_message(); // Throws
}


void Session::cancel_resumption_delay()
{
    REALM_ASSERT(m_state == Active);

    if (!m_suspended)
        return;

    m_suspended = false;

    logger.debug("Resumed"); // Throws

    if (unbind_process_complete())
        initiate_rebind(); // Throws

    m_conn.one_more_active_unsuspended_session(); // Throws

    on_resumed(); // Throws
}


void Session::gather_pending_compensating_writes(util::Span<Changeset> changesets,
                                                 std::vector<ProtocolErrorInfo>* out)
{
    if (m_pending_compensating_write_errors.empty() || changesets.empty()) {
        return;
    }

#ifdef REALM_DEBUG
    REALM_ASSERT_DEBUG(
        std::is_sorted(m_pending_compensating_write_errors.begin(), m_pending_compensating_write_errors.end(),
                       [](const ProtocolErrorInfo& lhs, const ProtocolErrorInfo& rhs) {
                           return lhs.compensating_write_server_version < rhs.compensating_write_server_version;
                       }));
#endif

    while (!m_pending_compensating_write_errors.empty() &&
           m_pending_compensating_write_errors.front().compensating_write_server_version <=
               changesets.back().version) {
        auto& cur_error = m_pending_compensating_write_errors.front();
        REALM_ASSERT(cur_error.compensating_write_server_version >= changesets.front().version);
        out->push_back(std::move(cur_error));
        m_pending_compensating_write_errors.pop_front();
    }
}


void Session::integrate_changesets(ClientReplication& repl, const SyncProgress& progress,
                                   std::uint_fast64_t downloadable_bytes,
                                   const ReceivedChangesets& received_changesets, VersionInfo& version_info,
                                   DownloadBatchState download_batch_state)
{
    auto& history = repl.get_history();
    if (received_changesets.empty()) {
        if (download_batch_state == DownloadBatchState::MoreToCome) {
            throw IntegrationException(ClientError::bad_progress,
                                       "received empty download message that was not the last in batch");
        }
        history.set_sync_progress(progress, &downloadable_bytes, version_info); // Throws
        return;
    }

    std::vector<ProtocolErrorInfo> pending_compensating_write_errors;
    auto transact = get_db()->start_read();
    history.integrate_server_changesets(
        progress, &downloadable_bytes, received_changesets, version_info, download_batch_state, logger, transact,
        [&](const TransactionRef&, util::Span<Changeset> changesets) {
            gather_pending_compensating_writes(changesets, &pending_compensating_write_errors);
        },
        get_transact_reporter()); // Throws
    if (received_changesets.size() == 1) {
        logger.debug("1 remote changeset integrated, producing client version %1",
                     version_info.sync_version.version); // Throws
    }
    else {
        logger.debug("%2 remote changesets integrated, producing client version %1",
                     version_info.sync_version.version, received_changesets.size()); // Throws
    }

    for (const auto& pending_error : pending_compensating_write_errors) {
        logger.info("Reporting compensating write for client version %1 in server version %2: %3",
                    pending_error.compensating_write_rejected_client_version,
                    pending_error.compensating_write_server_version, pending_error.message);
        try {
            ProtocolError error_code = ProtocolError(pending_error.raw_error_code);
            on_connection_state_changed(m_conn.get_state(),
                                        SessionErrorInfo{pending_error, make_error_code(error_code)});
        }
        catch (...) {
            logger.error("Exception thrown while reporting compensating write: %1", exception_to_status());
        }
    }
}


void Session::on_integration_failure(const IntegrationException& error)
{
    REALM_ASSERT(m_state == Active);
    REALM_ASSERT(!m_client_error && !m_error_to_send);
    logger.error("Failed to integrate downloaded changesets: %1", error.what());

    m_client_error = util::make_optional<IntegrationException>(error);
    m_error_to_send = true;

    // Since the deactivation process has not been initiated, the UNBIND
    // message cannot have been sent unless an ERROR message was received.
    REALM_ASSERT(m_error_message_received || !m_unbind_message_sent);
    if (m_ident_message_sent && !m_error_message_received) {
        ensure_enlisted_to_send(); // Throws
    }
}

void Session::on_changesets_integrated(version_type client_version, const SyncProgress& progress)
{
    REALM_ASSERT(m_state == Active);
    REALM_ASSERT(progress.download.server_version >= m_download_progress.server_version);
    m_download_progress = progress.download;
    bool upload_progressed = (progress.upload.client_version > m_progress.upload.client_version);
    m_progress = progress;
    if (upload_progressed) {
        if (progress.upload.client_version > m_last_version_selected_for_upload) {
            if (progress.upload.client_version > m_upload_progress.client_version)
                m_upload_progress = progress.upload;
            m_last_version_selected_for_upload = progress.upload.client_version;
        }

        check_for_upload_completion();
    }

    do_recognize_sync_version(client_version); // Allows upload process to resume
    check_for_download_completion();           // Throws

    // Since the deactivation process has not been initiated, the UNBIND
    // message cannot have been sent unless an ERROR message was received.
    REALM_ASSERT(m_error_message_received || !m_unbind_message_sent);
    if (m_ident_message_sent && !m_error_message_received) {
        ensure_enlisted_to_send(); // Throws
    }
}


Session::~Session()
{
    //    REALM_ASSERT(m_state == Unactivated || m_state == Deactivated);
}


std::string Session::make_logger_prefix(session_ident_type ident)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "Session[" << ident << "]: "; // Throws
    return out.str();                    // Throws
}


void Session::activate()
{
    REALM_ASSERT(m_state == Unactivated);

    logger.debug("Activating"); // Throws

    bool has_pending_client_reset = false;
    if (REALM_LIKELY(!get_client().is_dry_run())) {
        // The reason we need a mutable reference from get_client_reset_config() is because we
        // don't want the session to keep a strong reference to the client_reset_config->fresh_copy
        // DB. If it did, then the fresh DB would stay alive for the duration of this sync session
        // and we want to clean it up once the reset is finished. Additionally, the fresh copy will
        // be set to a new copy on every reset so there is no reason to keep a reference to it.
        // The modification to the client reset config happens via std::move(client_reset_config->fresh_copy).
        // If the client reset config were a `const &` then this std::move would create another strong
        // reference which we don't want to happen.
        util::Optional<ClientReset>& client_reset_config = get_client_reset_config();

        bool file_exists = util::File::exists(get_realm_path());

        logger.info("client_reset_config = %1, Realm exists = %2, "
                    "client reset = %3",
                    client_reset_config ? "true" : "false", file_exists ? "true" : "false",
                    (client_reset_config && file_exists) ? "true" : "false"); // Throws
        if (client_reset_config && !m_client_reset_operation) {
            m_client_reset_operation = std::make_unique<_impl::ClientResetOperation>(
                logger, get_db(), std::move(client_reset_config->fresh_copy), client_reset_config->mode,
                std::move(client_reset_config->notify_before_client_reset),
                std::move(client_reset_config->notify_after_client_reset),
                client_reset_config->recovery_is_allowed); // Throws
        }

        if (!m_client_reset_operation) {
            const ClientReplication& repl = access_realm(); // Throws
            repl.get_history().get_status(m_last_version_available, m_client_file_ident, m_progress,
                                          &has_pending_client_reset); // Throws
        }
    }
    logger.debug("client_file_ident = %1, client_file_ident_salt = %2", m_client_file_ident.ident,
                 m_client_file_ident.salt); // Throws
    m_upload_target_version = m_last_version_available;
    m_upload_progress = m_progress.upload;
    m_last_version_selected_for_upload = m_upload_progress.client_version;
    m_download_progress = m_progress.download;
    REALM_ASSERT(m_last_version_available >= m_progress.upload.client_version);

    logger.debug("last_version_available  = %1", m_last_version_available);           // Throws
    logger.debug("progress_server_version = %1", m_progress.download.server_version); // Throws
    logger.debug("progress_client_version = %1",
                 m_progress.download.last_integrated_client_version); // Throws

    reset_protocol_state();
    m_state = Active;

    REALM_ASSERT(!m_suspended);
    m_conn.one_more_active_unsuspended_session(); // Throws

    try {
        process_pending_flx_bootstrap();
    }
    catch (const IntegrationException& error) {
        logger.error("Error integrating bootstrap changesets: %1", error.what());
        on_suspended(SessionErrorInfo{error.code(), false});
        m_conn.one_less_active_unsuspended_session(); // Throws
    }

    if (has_pending_client_reset) {
        handle_pending_client_reset_acknowledgement();
    }
}


// The caller (Connection) must discard the session if the session has become
// deactivated upon return.
void Session::initiate_deactivation()
{
    REALM_ASSERT(m_state == Active);

    logger.debug("Initiating deactivation"); // Throws

    m_state = Deactivating;

    if (!m_suspended)
        m_conn.one_less_active_unsuspended_session(); // Throws

    if (m_enlisted_to_send) {
        REALM_ASSERT(!unbind_process_complete());
        return;
    }

    // Deactivate immediately if the BIND message has not yet been sent and the
    // session is not enlisted to send, or if the unbinding process has already
    // completed.
    if (!m_bind_message_sent || unbind_process_complete()) {
        complete_deactivation(); // Throws
        // Life cycle state is now Deactivated
        return;
    }

    // Ready to send the UNBIND message, if it has not already been sent
    if (!m_unbind_message_sent) {
        enlist_to_send(); // Throws
        return;
    }
}


void Session::complete_deactivation()
{
    m_state = Deactivated;

    logger.debug("Deactivation completed"); // Throws
}


// Called by the associated Connection object when this session is granted an
// opportunity to send a message.
//
// The caller (Connection) must discard the session if the session has become
// deactivated upon return.
void Session::send_message()
{
    REALM_ASSERT(m_state == Active || m_state == Deactivating);
    REALM_ASSERT(m_enlisted_to_send);
    m_enlisted_to_send = false;
    if (m_state == Deactivating || m_error_message_received) {
        // Deactivation has been initiated. If the UNBIND message has not been
        // sent yet, there is no point in sending it. Instead, we can let the
        // deactivation process complete.
        if (!m_bind_message_sent) {
            return complete_deactivation(); // Throws
            // Life cycle state is now Deactivated
        }

        // Session life cycle state is Deactivating or the unbinding process has
        // been initiated by a session specific ERROR message
        if (!m_unbind_message_sent)
            send_unbind_message(); // Throws
        return;
    }

    // Session life cycle state is Active and the unbinding process has
    // not been initiated
    REALM_ASSERT(!m_unbind_message_sent);

    if (!m_bind_message_sent)
        return send_bind_message(); // Throws

    if (!m_ident_message_sent) {
        if (have_client_file_ident())
            send_ident_message(); // Throws
        return;
    }

    const auto has_pending_test_command = std::any_of(m_pending_test_commands.begin(), m_pending_test_commands.end(),
                                                      [](const PendingTestCommand& command) {
                                                          return command.pending;
                                                      });
    if (has_pending_test_command) {
        return send_test_command_message();
    }

    if (m_error_to_send)
        return send_json_error_message(); // Throws

    if (m_connection_to_close)
        return close_connection(); // Throws

    if (m_target_download_mark > m_last_download_mark_sent)
        return send_mark_message(); // Throws

    auto check_pending_flx_version = [&]() -> bool {
        if (!m_is_flx_sync_session) {
            return false;
        }

        if (!m_allow_upload) {
            return false;
        }

        m_pending_flx_sub_set = get_flx_subscription_store()->get_next_pending_version(
            m_last_sent_flx_query_version, m_upload_progress.client_version);

        if (!m_pending_flx_sub_set) {
            return false;
        }

        return m_upload_progress.client_version >= m_pending_flx_sub_set->snapshot_version;
    };

    if (check_pending_flx_version()) {
        return send_query_change_message(); // throws
    }

    REALM_ASSERT(m_upload_progress.client_version <= m_upload_target_version);
    REALM_ASSERT(m_upload_target_version <= m_last_version_available);
    if (m_allow_upload && (m_upload_target_version > m_upload_progress.client_version)) {
        return send_upload_message(); // Throws
    }
}


void Session::send_bind_message()
{
    REALM_ASSERT(m_state == Active);

    session_ident_type session_ident = m_ident;
    const std::string& path = get_virt_path();
    bool need_client_file_ident = !have_client_file_ident();
    const bool is_subserver = false;


    ClientProtocol& protocol = m_conn.get_client_protocol();
    int protocol_version = m_conn.get_negotiated_protocol_version();
    OutputBuffer& out = m_conn.get_output_buffer();
    // Discard the token since it's ignored by the server.
    std::string empty_access_token{};
    protocol.make_bind_message(protocol_version, out, session_ident, path, empty_access_token, need_client_file_ident,
                               is_subserver); // Throws
    m_conn.initiate_write_message(out, this); // Throws

    m_bind_message_sent = true;

    // Ready to send the IDENT message if the file identifier pair is already
    // available.
    if (!need_client_file_ident)
        enlist_to_send(); // Throws
}


void Session::send_ident_message()
{
    REALM_ASSERT(m_state == Active);
    REALM_ASSERT(m_bind_message_sent);
    REALM_ASSERT(!m_unbind_message_sent);
    REALM_ASSERT(have_client_file_ident());


    ClientProtocol& protocol = m_conn.get_client_protocol();
    OutputBuffer& out = m_conn.get_output_buffer();
    session_ident_type session_ident = m_ident;

    if (m_is_flx_sync_session) {
        const auto active_query_set = get_flx_subscription_store()->get_active();
        const auto active_query_body = active_query_set.to_ext_json();
        logger.debug("Sending: IDENT(client_file_ident=%1, client_file_ident_salt=%2, "
                     "scan_server_version=%3, scan_client_version=%4, latest_server_version=%5, "
                     "latest_server_version_salt=%6, query_version: %7 query_size: %8, query: \"%9\")",
                     m_client_file_ident.ident, m_client_file_ident.salt, m_progress.download.server_version,
                     m_progress.download.last_integrated_client_version, m_progress.latest_server_version.version,
                     m_progress.latest_server_version.salt, active_query_set.version(), active_query_body.size(),
                     active_query_body); // Throws
        protocol.make_flx_ident_message(out, session_ident, m_client_file_ident, m_progress,
                                        active_query_set.version(), active_query_body); // Throws
        m_last_sent_flx_query_version = active_query_set.version();
    }
    else {
        logger.debug("Sending: IDENT(client_file_ident=%1, client_file_ident_salt=%2, "
                     "scan_server_version=%3, scan_client_version=%4, latest_server_version=%5, "
                     "latest_server_version_salt=%6)",
                     m_client_file_ident.ident, m_client_file_ident.salt, m_progress.download.server_version,
                     m_progress.download.last_integrated_client_version, m_progress.latest_server_version.version,
                     m_progress.latest_server_version.salt);                                  // Throws
        protocol.make_pbs_ident_message(out, session_ident, m_client_file_ident, m_progress); // Throws
    }
    m_conn.initiate_write_message(out, this); // Throws

    m_ident_message_sent = true;

    // Other messages may be waiting to be sent
    enlist_to_send(); // Throws
}

void Session::send_query_change_message()
{
    REALM_ASSERT(m_state == Active);
    REALM_ASSERT(m_ident_message_sent);
    REALM_ASSERT(!m_unbind_message_sent);
    REALM_ASSERT(m_pending_flx_sub_set);
    REALM_ASSERT(m_pending_flx_sub_set->query_version > m_last_sent_flx_query_version);

    if (REALM_UNLIKELY(get_client().is_dry_run())) {
        return;
    }

    auto sub_store = get_flx_subscription_store();
    auto latest_sub_set = sub_store->get_by_version(m_pending_flx_sub_set->query_version);
    auto latest_queries = latest_sub_set.to_ext_json();
    logger.debug("Sending: QUERY(query_version=%1, query_size=%2, query=\"%3\"", latest_sub_set.version(),
                 latest_queries.size(), latest_queries);

    OutputBuffer& out = m_conn.get_output_buffer();
    session_ident_type session_ident = get_ident();
    ClientProtocol& protocol = m_conn.get_client_protocol();
    protocol.make_query_change_message(out, session_ident, latest_sub_set.version(), latest_queries);
    m_conn.initiate_write_message(out, this);

    m_last_sent_flx_query_version = latest_sub_set.version();

    request_download_completion_notification();
}

void Session::send_upload_message()
{
    REALM_ASSERT(m_state == Active);
    REALM_ASSERT(m_ident_message_sent);
    REALM_ASSERT(!m_unbind_message_sent);
    REALM_ASSERT(m_upload_target_version > m_upload_progress.client_version);

    if (REALM_UNLIKELY(get_client().is_dry_run()))
        return;

    auto target_upload_version = m_upload_target_version;
    if (m_is_flx_sync_session) {
        if (!m_pending_flx_sub_set || m_pending_flx_sub_set->snapshot_version < m_upload_progress.client_version) {
            m_pending_flx_sub_set = get_flx_subscription_store()->get_next_pending_version(
                m_last_sent_flx_query_version, m_upload_progress.client_version);
        }
        if (m_pending_flx_sub_set && m_pending_flx_sub_set->snapshot_version < m_upload_target_version) {
            logger.trace("Limiting UPLOAD message up to version %1 to send QUERY version %2",
                         m_pending_flx_sub_set->snapshot_version, m_pending_flx_sub_set->query_version);
            target_upload_version = m_pending_flx_sub_set->snapshot_version;
        }
    }

    const ClientReplication& repl = access_realm(); // Throws

    std::vector<UploadChangeset> uploadable_changesets;
    version_type locked_server_version = 0;
    repl.get_history().find_uploadable_changesets(m_upload_progress, target_upload_version, uploadable_changesets,
                                                  locked_server_version); // Throws

    if (uploadable_changesets.empty()) {
        // Nothing more to upload right now
        check_for_upload_completion(); // Throws
    }
    else {
        m_last_version_selected_for_upload = uploadable_changesets.back().progress.client_version;
    }

    version_type progress_client_version = m_upload_progress.client_version;
    version_type progress_server_version = m_upload_progress.last_integrated_server_version;

    logger.debug("Sending: UPLOAD(progress_client_version=%1, progress_server_version=%2, "
                 "locked_server_version=%3, num_changesets=%4)",
                 progress_client_version, progress_server_version, locked_server_version,
                 uploadable_changesets.size()); // Throws

    ClientProtocol& protocol = m_conn.get_client_protocol();
    ClientProtocol::UploadMessageBuilder upload_message_builder = protocol.make_upload_message_builder(); // Throws

    for (const UploadChangeset& uc : uploadable_changesets) {
        logger.debug("Fetching changeset for upload (client_version=%1, server_version=%2, "
                     "changeset_size=%3, origin_timestamp=%4, origin_file_ident=%5)",
                     uc.progress.client_version, uc.progress.last_integrated_server_version, uc.changeset.size(),
                     uc.origin_timestamp, uc.origin_file_ident); // Throws
        if (logger.would_log(util::Logger::Level::trace)) {
            BinaryData changeset_data = uc.changeset.get_first_chunk();
            if (changeset_data.size() < 1024) {
                logger.trace("Changeset: %1",
                             _impl::clamped_hex_dump(changeset_data)); // Throws
            }
            else {
                logger.trace("Changeset(comp): %1 %2", changeset_data.size(),
                             protocol.compressed_hex_dump(changeset_data));
            }

#if REALM_DEBUG
            ChunkedBinaryInputStream in{changeset_data};
            Changeset log;
            parse_changeset(in, log);
            std::stringstream ss;
            log.print(ss);
            logger.trace("Changeset (parsed):\n%1", ss.str());
#endif
        }

#if 0 // Upload log compaction is currently not implemented
        if (!get_client().m_disable_upload_compaction) {
            ChangesetEncoder::Buffer encode_buffer;

            {
                // Upload compaction only takes place within single changesets to
                // avoid another client seeing inconsistent snapshots.
                ChunkedBinaryInputStream stream{uc.changeset};
                Changeset changeset;
                parse_changeset(stream, changeset); // Throws
                // FIXME: What is the point of setting these? How can compaction care about them?
                changeset.version = uc.progress.client_version;
                changeset.last_integrated_remote_version = uc.progress.last_integrated_server_version;
                changeset.origin_timestamp = uc.origin_timestamp;
                changeset.origin_file_ident = uc.origin_file_ident;

                compact_changesets(&changeset, 1);
                encode_changeset(changeset, encode_buffer);

                logger.debug("Upload compaction: original size = %1, compacted size = %2", uc.changeset.size(),
                             encode_buffer.size()); // Throws
            }

            upload_message_builder.add_changeset(
                uc.progress.client_version, uc.progress.last_integrated_server_version, uc.origin_timestamp,
                uc.origin_file_ident, BinaryData{encode_buffer.data(), encode_buffer.size()}); // Throws
        }
        else
#endif
        {
            upload_message_builder.add_changeset(uc.progress.client_version,
                                                 uc.progress.last_integrated_server_version, uc.origin_timestamp,
                                                 uc.origin_file_ident,
                                                 uc.changeset); // Throws
        }
    }

    int protocol_version = m_conn.get_negotiated_protocol_version();
    OutputBuffer& out = m_conn.get_output_buffer();
    session_ident_type session_ident = get_ident();
    upload_message_builder.make_upload_message(protocol_version, out, session_ident, progress_client_version,
                                               progress_server_version,
                                               locked_server_version); // Throws
    m_conn.initiate_write_message(out, this);                          // Throws

    // Other messages may be waiting to be sent
    enlist_to_send(); // Throws
}


void Session::send_mark_message()
{
    REALM_ASSERT(m_state == Active);
    REALM_ASSERT(m_ident_message_sent);
    REALM_ASSERT(!m_unbind_message_sent);
    REALM_ASSERT(m_target_download_mark > m_last_download_mark_sent);

    request_ident_type request_ident = m_target_download_mark;
    logger.debug("Sending: MARK(request_ident=%1)", request_ident); // Throws

    ClientProtocol& protocol = m_conn.get_client_protocol();
    OutputBuffer& out = m_conn.get_output_buffer();
    session_ident_type session_ident = get_ident();
    protocol.make_mark_message(out, session_ident, request_ident); // Throws
    m_conn.initiate_write_message(out, this);                      // Throws

    m_last_download_mark_sent = request_ident;

    // Other messages may be waiting to be sent
    enlist_to_send(); // Throws
}


void Session::send_unbind_message()
{
    REALM_ASSERT(m_state == Deactivating || m_error_message_received);
    REALM_ASSERT(m_bind_message_sent);
    REALM_ASSERT(!m_unbind_message_sent);

    logger.debug("Sending: UNBIND"); // Throws

    ClientProtocol& protocol = m_conn.get_client_protocol();
    OutputBuffer& out = m_conn.get_output_buffer();
    session_ident_type session_ident = get_ident();
    protocol.make_unbind_message(out, session_ident); // Throws
    m_conn.initiate_write_message(out, this);         // Throws

    m_unbind_message_sent = true;
}


void Session::send_json_error_message()
{
    REALM_ASSERT(m_state == Active);
    REALM_ASSERT(m_ident_message_sent);
    REALM_ASSERT(!m_unbind_message_sent);
    REALM_ASSERT(m_error_to_send);
    REALM_ASSERT(m_client_error);

    ClientProtocol& protocol = m_conn.get_client_protocol();
    OutputBuffer& out = m_conn.get_output_buffer();
    session_ident_type session_ident = get_ident();
    std::error_code ec = m_client_error->code();
    auto message = m_client_error->what();

    logger.info("Sending: ERROR \"%1\" (error_code=%2, session_ident=%3)", message, ec.value(),
                session_ident); // Throws

    nlohmann::json error_body_json;
    error_body_json["message"] = message;
    protocol.make_json_error_message(out, session_ident, ec.value(), error_body_json.dump()); // Throws

    m_conn.initiate_write_message(out, this); // Throws

    m_error_to_send = false;
    m_connection_to_close = true;

    // Connection is waiting to be closed
    enlist_to_send(); // Throws
}


void Session::send_test_command_message()
{
    REALM_ASSERT(m_state == Active);

    auto it = std::find_if(m_pending_test_commands.begin(), m_pending_test_commands.end(),
                           [](const PendingTestCommand& command) {
                               return command.pending;
                           });
    REALM_ASSERT(it != m_pending_test_commands.end());

    ClientProtocol& protocol = m_conn.get_client_protocol();
    OutputBuffer& out = m_conn.get_output_buffer();
    auto session_ident = get_ident();

    logger.info("Sending: TEST_COMMAND \"%1\" (session_ident=%2, request_ident=%3)", it->body, session_ident, it->id);
    protocol.make_test_command_message(out, session_ident, it->id, it->body);

    m_conn.initiate_write_message(out, this); // Throws;
    it->pending = false;

    enlist_to_send();
}


void Session::close_connection()
{
    REALM_ASSERT(m_state == Active);
    REALM_ASSERT(m_ident_message_sent);
    REALM_ASSERT(!m_unbind_message_sent);
    REALM_ASSERT(m_connection_to_close);
    REALM_ASSERT(m_client_error);

    m_conn.close_due_to_protocol_error(m_client_error->code(), m_client_error->what()); // Throws
    m_connection_to_close = false;
    m_client_error = util::none;
}


std::error_code Session::receive_ident_message(SaltedFileIdent client_file_ident)
{
    logger.debug("Received: IDENT(client_file_ident=%1, client_file_ident_salt=%2)", client_file_ident.ident,
                 client_file_ident.salt); // Throws

    // Ignore the message if the deactivation process has been initiated,
    // because in that case, the associated Realm must not be accessed any
    // longer.
    if (m_state != Active)
        return std::error_code{}; // Success

    bool legal_at_this_time = (m_bind_message_sent && !have_client_file_ident() && !m_error_message_received &&
                               !m_unbound_message_received);
    if (REALM_UNLIKELY(!legal_at_this_time)) {
        logger.error("Illegal message at this time");
        return ClientError::bad_message_order;
    }
    if (REALM_UNLIKELY(client_file_ident.ident < 1)) {
        logger.error("Bad client file identifier in IDENT message");
        return ClientError::bad_client_file_ident;
    }
    if (REALM_UNLIKELY(client_file_ident.salt == 0)) {
        logger.error("Bad client file identifier salt in IDENT message");
        return ClientError::bad_client_file_ident_salt;
    }

    m_client_file_ident = client_file_ident;

    if (REALM_UNLIKELY(get_client().is_dry_run())) {
        // Ready to send the IDENT message
        ensure_enlisted_to_send(); // Throws
        return std::error_code{};  // Success
    }

    // access before the client reset (if applicable) because
    // the reset can take a while and the sync session might have died
    // by the time the reset finishes.
    ClientReplication& repl = access_realm(); // Throws

    auto client_reset_if_needed = [&]() -> bool {
        if (!m_client_reset_operation) {
            return false;
        }

        // ClientResetOperation::finalize() will return true only if the operation actually did
        // a client reset. It may choose not to do a reset if the local Realm does not exist
        // at this point (in that case there is nothing to reset). But in any case, we must
        // clean up m_client_reset_operation at this point as sync should be able to continue from
        // this point forward.
        auto client_reset_operation = std::move(m_client_reset_operation);
        util::UniqueFunction<void(int64_t)> on_flx_subscription_complete = [this](int64_t version) {
            this->non_sync_flx_completion(version);
        };
        if (!client_reset_operation->finalize(client_file_ident, get_flx_subscription_store(),
                                              std::move(on_flx_subscription_complete))) {
            return false;
        }
        realm::VersionID client_reset_old_version = client_reset_operation->get_client_reset_old_version();
        realm::VersionID client_reset_new_version = client_reset_operation->get_client_reset_new_version();

        // The fresh Realm has been used to reset the state
        logger.debug("Client reset is completed, path=%1", get_realm_path()); // Throws

        SaltedFileIdent client_file_ident;
        bool has_pending_client_reset = false;
        repl.get_history().get_status(m_last_version_available, client_file_ident, m_progress,
                                      &has_pending_client_reset); // Throws
        REALM_ASSERT_EX(m_client_file_ident.ident == client_file_ident.ident, m_client_file_ident.ident,
                        client_file_ident.ident);
        REALM_ASSERT_EX(m_client_file_ident.salt == client_file_ident.salt, m_client_file_ident.salt,
                        client_file_ident.salt);
        REALM_ASSERT_EX(m_progress.download.last_integrated_client_version == 0,
                        m_progress.download.last_integrated_client_version);
        REALM_ASSERT_EX(m_progress.upload.client_version == 0, m_progress.upload.client_version);
        REALM_ASSERT_EX(m_progress.upload.last_integrated_server_version == 0,
                        m_progress.upload.last_integrated_server_version);
        logger.trace("last_version_available  = %1", m_last_version_available); // Throws

        m_upload_target_version = m_last_version_available;
        m_upload_progress = m_progress.upload;
        // In recovery mode, there may be new changesets to upload and nothing left to download.
        // In FLX DiscardLocal mode, there may be new commits due to subscription handling.
        // For both, we want to allow uploads again without needing external changes to download first.
        m_allow_upload = true;
        REALM_ASSERT_EX(m_last_version_selected_for_upload == 0, m_last_version_selected_for_upload);

        get_transact_reporter()->report_sync_transact(client_reset_old_version, client_reset_new_version);

        if (has_pending_client_reset) {
            handle_pending_client_reset_acknowledgement();
        }
        return true;
    };
    // if a client reset happens, it will take care of setting the file ident
    // and if not, we do it here
    bool did_client_reset = false;
    try {
        did_client_reset = client_reset_if_needed();
    }
    catch (const std::exception& e) {
        logger.error("A fatal error occured during client reset: '%1'", e.what());
        return make_error_code(sync::ClientError::auto_client_reset_failure);
    }
    if (!did_client_reset) {
        repl.get_history().set_client_file_ident(client_file_ident, m_fix_up_object_ids); // Throws
        this->m_progress.download.last_integrated_client_version = 0;
        this->m_progress.upload.client_version = 0;
        this->m_last_version_selected_for_upload = 0;
    }

    // Ready to send the IDENT message
    ensure_enlisted_to_send(); // Throws
    return std::error_code{};  // Success
}

void Session::receive_download_message(const SyncProgress& progress, std::uint_fast64_t downloadable_bytes,
                                       DownloadBatchState batch_state, int64_t query_version,
                                       const ReceivedChangesets& received_changesets)
{
    if (is_steady_state_download_message(batch_state, query_version)) {
        batch_state = DownloadBatchState::SteadyState;
    }

    logger.debug("Received: DOWNLOAD(download_server_version=%1, download_client_version=%2, "
                 "latest_server_version=%3, latest_server_version_salt=%4, "
                 "upload_client_version=%5, upload_server_version=%6, downloadable_bytes=%7, "
                 "last_in_batch=%8, query_version=%9, num_changesets=%10, ...)",
                 progress.download.server_version, progress.download.last_integrated_client_version,
                 progress.latest_server_version.version, progress.latest_server_version.salt,
                 progress.upload.client_version, progress.upload.last_integrated_server_version, downloadable_bytes,
                 batch_state != DownloadBatchState::MoreToCome, query_version, received_changesets.size()); // Throws

    // Ignore the message if the deactivation process has been initiated,
    // because in that case, the associated Realm must not be accessed any
    // longer.
    if (m_state != Active)
        return;

    bool legal_at_this_time = (m_ident_message_sent && !m_error_message_received && !m_unbound_message_received);
    if (REALM_UNLIKELY(!legal_at_this_time)) {
        logger.error("Illegal message at this time");
        m_conn.close_due_to_protocol_error(ClientError::bad_message_order);
        return;
    }
    int error_code = 0;
    if (REALM_UNLIKELY(!check_received_sync_progress(progress, error_code))) {
        logger.error("Bad sync progress received (%1)", error_code);
        m_conn.close_due_to_protocol_error(ClientError::bad_progress);
        return;
    }

    version_type server_version = m_progress.download.server_version;
    version_type last_integrated_client_version = m_progress.download.last_integrated_client_version;
    for (const Transformer::RemoteChangeset& changeset : received_changesets) {
        // Check that per-changeset server version is strictly increasing, except in FLX sync where the server version
        // must be increasing, but can stay the same during bootstraps.
        bool good_server_version = m_is_flx_sync_session ? (changeset.remote_version >= server_version)
                                                         : (changeset.remote_version > server_version);
        if (!good_server_version) {
            logger.error("Bad server version in changeset header (DOWNLOAD) (%1, %2, %3)", changeset.remote_version,
                         server_version, progress.download.server_version);
            m_conn.close_due_to_protocol_error(ClientError::bad_server_version);
            return;
        }
        server_version = changeset.remote_version;
        // Check that per-changeset last integrated client version is "weakly"
        // increasing.
        bool good_client_version =
            (changeset.last_integrated_local_version >= last_integrated_client_version &&
             changeset.last_integrated_local_version <= progress.download.last_integrated_client_version);
        if (!good_client_version) {
            logger.error("Bad last integrated client version in changeset header (DOWNLOAD) "
                         "(%1, %2, %3)",
                         changeset.last_integrated_local_version, last_integrated_client_version,
                         progress.download.last_integrated_client_version);
            m_conn.close_due_to_protocol_error(ClientError::bad_client_version);
            return;
        }
        last_integrated_client_version = changeset.last_integrated_local_version;
        // Server shouldn't send our own changes, and zero is not a valid client
        // file identifier.
        bool good_file_ident =
            (changeset.origin_file_ident > 0 && changeset.origin_file_ident != m_client_file_ident.ident);
        if (!good_file_ident) {
            logger.error("Bad origin file identifier");
            m_conn.close_due_to_protocol_error(ClientError::bad_origin_file_ident);
            return;
        }
    }

    auto hook_action = call_debug_hook(SyncClientHookEvent::DownloadMessageReceived, progress, query_version,
                                       batch_state, received_changesets.size());
    if (hook_action == SyncClientHookAction::EarlyReturn) {
        return;
    }
    REALM_ASSERT(hook_action == SyncClientHookAction::NoAction);

    if (process_flx_bootstrap_message(progress, batch_state, query_version, received_changesets)) {
        clear_resumption_delay_state();
        return;
    }

    initiate_integrate_changesets(downloadable_bytes, batch_state, progress, received_changesets); // Throws

    hook_action = call_debug_hook(SyncClientHookEvent::DownloadMessageIntegrated, progress, query_version,
                                  batch_state, received_changesets.size());
    if (hook_action == SyncClientHookAction::EarlyReturn) {
        return;
    }
    REALM_ASSERT(hook_action == SyncClientHookAction::NoAction);

    // When we receive a DOWNLOAD message successfully, we can clear the backoff timer value used to reconnect
    // after a retryable session error.
    clear_resumption_delay_state();
}

std::error_code Session::receive_mark_message(request_ident_type request_ident)
{
    logger.debug("Received: MARK(request_ident=%1)", request_ident); // Throws

    // Ignore the message if the deactivation process has been initiated,
    // because in that case, the associated Realm must not be accessed any
    // longer.
    if (m_state != Active)
        return std::error_code{}; // Success

    bool legal_at_this_time = (m_ident_message_sent && !m_error_message_received && !m_unbound_message_received);
    if (REALM_UNLIKELY(!legal_at_this_time)) {
        logger.error("Illegal message at this time");
        return ClientError::bad_message_order;
    }
    bool good_request_ident =
        (request_ident <= m_last_download_mark_sent && request_ident > m_last_download_mark_received);
    if (REALM_UNLIKELY(!good_request_ident)) {
        logger.error("Bad request identifier in MARK message");
        return ClientError::bad_request_ident;
    }

    m_server_version_at_last_download_mark = m_progress.download.server_version;
    m_last_download_mark_received = request_ident;
    check_for_download_completion(); // Throws

    return std::error_code{}; // Success
}


// The caller (Connection) must discard the session if the session has become
// deactivated upon return.
std::error_code Session::receive_unbound_message()
{
    logger.debug("Received: UNBOUND");

    bool legal_at_this_time = (m_unbind_message_sent && !m_error_message_received && !m_unbound_message_received);
    if (REALM_UNLIKELY(!legal_at_this_time)) {
        logger.error("Illegal message at this time");
        return ClientError::bad_message_order;
    }

    // The fact that the UNBIND message has been sent, but an ERROR message has
    // not been received, implies that the deactivation process must have been
    // initiated, so this session must be in the Deactivating state.
    REALM_ASSERT(m_state == Deactivating);

    m_unbound_message_received = true;

    // Detect completion of the unbinding process
    if (m_unbind_message_sent_2) {
        // The deactivation process completes when the unbinding process
        // completes.
        complete_deactivation(); // Throws
        // Life cycle state is now Deactivated
    }

    return std::error_code{}; // Success
}


std::error_code Session::receive_query_error_message(int error_code, std::string_view message, int64_t query_version)
{
    logger.info("Received QUERY_ERROR \"%1\" (error_code=%2, query_version=%3)", message, error_code, query_version);
    on_flx_sync_error(query_version, std::string_view(message.data(), message.size())); // throws
    return {};
}

// The caller (Connection) must discard the session if the session has become
// deactivated upon return.
std::error_code Session::receive_error_message(const ProtocolErrorInfo& info)
{
    logger.info("Received: ERROR \"%1\" (error_code=%2, try_again=%3, error_action=%4)", info.message,
                info.raw_error_code, info.try_again, info.server_requests_action); // Throws

    auto debug_action = call_debug_hook(SyncClientHookEvent::ErrorMessageReceived, info);
    if (debug_action == SyncClientHookAction::EarlyReturn) {
        return {};
    }

    // Ignore the error because the connection is going to be closed.
    if (m_connection_to_close)
        return std::error_code{}; // Success

    bool legal_at_this_time = (m_bind_message_sent && !m_error_message_received && !m_unbound_message_received);
    if (REALM_UNLIKELY(!legal_at_this_time)) {
        logger.error("Illegal message at this time");
        return ClientError::bad_message_order;
    }

    bool known_error_code = bool(get_protocol_error_message(info.raw_error_code));
    if (REALM_UNLIKELY(!known_error_code)) {
        logger.error("Unknown error code"); // Throws
        return ClientError::bad_error_code;
    }
    ProtocolError error_code = ProtocolError(info.raw_error_code);
    if (REALM_UNLIKELY(!is_session_level_error(error_code))) {
        logger.error("Not a session level error code"); // Throws
        return ClientError::bad_error_code;
    }

    // For compensating write errors, we need to defer raising them to the SDK until after the server version
    // containing the compensating write has appeared in a download message.
    if (error_code == ProtocolError::compensating_write) {
        m_pending_compensating_write_errors.push_back(info);
        return {};
    }

    REALM_ASSERT(!m_suspended);
    REALM_ASSERT(m_state == Active || m_state == Deactivating);
    logger.debug("Suspended"); // Throws

    m_error_message_received = true;
    m_suspended = true;

    // Detect completion of the unbinding process
    if (m_unbind_message_sent_2) {
        // The fact that the UNBIND message has been sent, but an ERROR message
        // has not been received, implies that the deactivation process must
        // have been initiated, so this session must be in the Deactivating
        // state.
        REALM_ASSERT(m_state == Deactivating);

        // The deactivation process completes when the unbinding process
        // completes.
        complete_deactivation(); // Throws
        // Life cycle state is now Deactivated
    }

    // Notify the application of the suspension of the session if the session is
    // still in the Active state
    if (m_state == Active) {
        m_conn.one_less_active_unsuspended_session();                      // Throws
        on_suspended(SessionErrorInfo{info, make_error_code(error_code)}); // Throws
    }

    if (info.try_again) {
        begin_resumption_delay(info);
    }

    // Ready to send the UNBIND message, if it has not been sent already
    if (!m_unbind_message_sent)
        ensure_enlisted_to_send(); // Throws

    return std::error_code{}; // Success;
}

std::error_code Session::receive_test_command_response(request_ident_type ident, std::string_view body)
{
    logger.info("Received: TEST_COMMAND \"%1\" (session_ident=%2, request_ident=%3)", body, m_ident, ident);
    auto it = std::find_if(m_pending_test_commands.begin(), m_pending_test_commands.end(),
                           [&](const PendingTestCommand& command) {
                               return command.id == ident;
                           });
    if (it == m_pending_test_commands.end()) {
        logger.error("No matching pending test command for id %1", ident);
        return ClientError::bad_request_ident;
    }

    it->promise.emplace_value(std::string{body});
    m_pending_test_commands.erase(it);

    return {};
}

void Session::begin_resumption_delay(const ProtocolErrorInfo& error_info)
{
    REALM_ASSERT(!m_try_again_activation_timer);

    m_try_again_delay_info.update(error_info);
    auto try_again_interval = m_try_again_delay_info.delay_interval();
    if (ProtocolError(error_info.raw_error_code) == ProtocolError::session_closed) {
        // FIXME With compensating writes the server sends this error after completing a bootstrap. Doing the normal
        // backoff behavior would result in waiting up to 5 minutes in between each query change which is
        // not acceptable latency. So for this error code alone, we hard-code a 1 second retry interval.
        try_again_interval = std::chrono::milliseconds{1000};
    }
    logger.debug("Will attempt to resume session after %1 milliseconds", try_again_interval.count());
    m_try_again_activation_timer = get_client().create_timer(try_again_interval, [this](Status status) {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        m_try_again_activation_timer.reset();
        cancel_resumption_delay();
    });
}

void Session::clear_resumption_delay_state()
{
    if (m_try_again_activation_timer) {
        logger.debug("Clearing resumption delay state after successful download");
        m_try_again_delay_info.reset();
    }
}

bool ClientImpl::Session::check_received_sync_progress(const SyncProgress& progress, int& error_code) noexcept
{
    const SyncProgress& a = m_progress;
    const SyncProgress& b = progress;
    // Latest server version must be weakly increasing throughout a session.
    if (b.latest_server_version.version < a.latest_server_version.version) {
        error_code = 1;
        return false;
    }
    // Last integrated client version on server must be weakly increasing
    // throughout a session.
    if (b.upload.client_version < a.upload.client_version) {
        error_code = 2;
        return false;
    }
    // Last integrated client version on server cannot be greater than the
    // latest client version in existence.
    if (b.upload.client_version > m_last_version_available) {
        error_code = 3;
        return false;
    }
    // Download cursor must be weakly increasing throughout a session
    if (b.download.server_version < a.download.server_version) {
        error_code = 4;
        return false;
    }
    // Download cursor cannot be greater than the latest server version in
    // existence.
    if (b.download.server_version > b.latest_server_version.version) {
        error_code = 5;
        return false;
    }
    // The last integrated client version on the server at the position in the
    // server's history of the download cursor must be weakly increasing
    // throughout a session.
    if (b.download.last_integrated_client_version < a.download.last_integrated_client_version) {
        error_code = 6;
        return false;
    }
    // The last integrated client version on the server at the position in the
    // server's history of the download cursor cannot be greater than the latest
    // client version integrated on the server.
    if (b.download.last_integrated_client_version > b.upload.client_version) {
        error_code = 7;
        return false;
    }
    return true;
}


void Session::check_for_upload_completion()
{
    REALM_ASSERT(m_state == Active);
    if (!m_upload_completion_notification_requested) {
        return;
    }

    // during an ongoing client reset operation, we never upload anything
    if (m_client_reset_operation)
        return;

    // Upload process must have reached end of history
    REALM_ASSERT(m_upload_progress.client_version <= m_last_version_available);
    bool scan_complete = (m_upload_progress.client_version == m_last_version_available);
    if (!scan_complete)
        return;

    // All uploaded changesets must have been acknowledged by the server
    REALM_ASSERT(m_progress.upload.client_version <= m_last_version_selected_for_upload);
    bool all_uploads_accepted = (m_progress.upload.client_version == m_last_version_selected_for_upload);
    if (!all_uploads_accepted)
        return;

    m_upload_completion_notification_requested = false;
    on_upload_completion(); // Throws
}


void Session::check_for_download_completion()
{
    REALM_ASSERT(m_target_download_mark >= m_last_download_mark_received);
    REALM_ASSERT(m_last_download_mark_received >= m_last_triggering_download_mark);
    if (m_last_download_mark_received == m_last_triggering_download_mark)
        return;
    if (m_last_download_mark_received < m_target_download_mark)
        return;
    if (m_download_progress.server_version < m_server_version_at_last_download_mark)
        return;
    m_last_triggering_download_mark = m_target_download_mark;
    if (REALM_UNLIKELY(!m_allow_upload)) {
        // Activate the upload process now, and enable immediate reactivation
        // after a subsequent fast reconnect.
        m_allow_upload = true;
        ensure_enlisted_to_send(); // Throws
    }
    on_download_completion(); // Throws
}
