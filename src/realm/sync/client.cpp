
#include <memory>
#include <tuple>
#include <atomic>

#include "realm/sync/client_base.hpp"
#include "realm/sync/protocol.hpp"
#include "realm/util/optional.hpp"
#include <realm/sync/client.hpp>
#include <realm/sync/config.hpp>
#include <realm/sync/noinst/client_reset.hpp>
#include <realm/sync/noinst/client_history_impl.hpp>
#include <realm/sync/noinst/client_impl_base.hpp>
#include <realm/sync/noinst/pending_bootstrap_store.hpp>
#include <realm/sync/subscriptions.hpp>
#include <realm/util/bind_ptr.hpp>
#include <realm/util/circular_buffer.hpp>
#include <realm/util/platform_info.hpp>
#include <realm/util/thread.hpp>
#include <realm/util/uri.hpp>
#include <realm/util/value_reset_guard.hpp>
#include <realm/version.hpp>

namespace realm {
namespace sync {

namespace {
using namespace realm::util;


// clang-format off
using SessionImpl                     = ClientImpl::Session;
using SyncTransactReporter            = ClientHistory::SyncTransactReporter;
using SyncTransactCallback            = Session::SyncTransactCallback;
using ProgressHandler                 = Session::ProgressHandler;
using WaitOperCompletionHandler       = Session::WaitOperCompletionHandler;
using ConnectionStateChangeListener   = Session::ConnectionStateChangeListener;
using port_type                       = Session::port_type;
using connection_ident_type           = std::int_fast64_t;
using ProxyConfig                     = SyncConfig::ProxyConfig;
// clang-format on


const char* get_error_message(ClientError error_code)
{
    switch (error_code) {
        case ClientError::connection_closed:
            return "Connection closed (no error)";
        case ClientError::unknown_message:
            return "Unknown type of input message";
        case ClientError::bad_syntax:
            return "Bad syntax in input message head";
        case ClientError::limits_exceeded:
            return "Limits exceeded in input message";
        case ClientError::bad_session_ident:
            return "Bad session identifier in input message";
        case ClientError::bad_message_order:
            return "Bad input message order";
        case ClientError::bad_client_file_ident:
            return "Bad client file identifier (IDENT)";
        case ClientError::bad_progress:
            return "Bad progress information (DOWNLOAD)";
        case ClientError::bad_changeset_header_syntax:
            return "Bad progress information (DOWNLOAD)";
        case ClientError::bad_changeset_size:
            return "Bad changeset size in changeset header (DOWNLOAD)";
        case ClientError::bad_origin_file_ident:
            return "Bad origin file identifier in changeset header (DOWNLOAD)";
        case ClientError::bad_server_version:
            return "Bad server version in changeset header (DOWNLOAD)";
        case ClientError::bad_changeset:
            return "Bad changeset (DOWNLOAD)";
        case ClientError::bad_request_ident:
            return "Bad request identifier (MARK)";
        case ClientError::bad_error_code:
            return "Bad error code (ERROR)";
        case ClientError::bad_compression:
            return "Bad compression (DOWNLOAD)";
        case ClientError::bad_client_version:
            return "Bad last integrated client version in changeset header (DOWNLOAD)";
        case ClientError::ssl_server_cert_rejected:
            return "SSL server certificate rejected";
        case ClientError::pong_timeout:
            return "Timeout on reception of PONG respone message";
        case ClientError::bad_client_file_ident_salt:
            return "Bad client file identifier salt (IDENT)";
        case ClientError::bad_file_ident:
            return "Bad file identifier (ALLOC)";
        case ClientError::connect_timeout:
            return "Sync connection was not fully established in time";
        case ClientError::bad_timestamp:
            return "Bad timestamp (PONG)";
        case ClientError::bad_protocol_from_server:
            return "Bad or missing protocol version information from server";
        case ClientError::client_too_old_for_server:
            return "Protocol version negotiation failed: Client is too old for server";
        case ClientError::client_too_new_for_server:
            return "Protocol version negotiation failed: Client is too new for server";
        case ClientError::protocol_mismatch:
            return ("Protocol version negotiation failed: No version supported by both "
                    "client and server");
        case ClientError::bad_state_message:
            return "Bad state message (STATE)";
        case ClientError::missing_protocol_feature:
            return "Requested feature missing in negotiated protocol version";
        case ClientError::http_tunnel_failed:
            return "Failure to establish HTTP tunnel with configured proxy";
        case ClientError::auto_client_reset_failure:
            return "Automatic recovery from client reset failed";
    }
    return nullptr;
}


class ErrorCategoryImpl : public std::error_category {
public:
    const char* name() const noexcept override final
    {
        return "realm::sync::ClientError";
    }
    std::string message(int error_code) const override final
    {
        const char* msg = get_error_message(ClientError(error_code));
        if (!msg)
            msg = "Unknown error";
        std::string msg_2{msg}; // Throws (copy)
        return msg_2;
    }
};

ErrorCategoryImpl g_error_category;

} // unnamed namespace


// Life cycle states of a session wrapper:
//
//  - Uninitiated
//  - Unactualized
//  - Actualized
//  - Finalized
//
// The session wrapper moves from the Uninitiated to the Unactualized state when
// it is initiated, i.e., when initiate() is called. This may happen on any
// thread.
//
// The session wrapper moves from the Unactualized to the Actualized state when
// it is associated with a session object, i.e., when `m_sess` is made to refer
// to an object of type SessionImpl. This always happens on the event loop
// thread.
//
// The session wrapper moves from the Actualized to the Finalized state when it
// is dissociated from the session object. This happens in response to the
// session wrapper having been abandoned by the application. This always happens
// on the event loop thread.
//
// The session wrapper will exist in the Finalized state only while referenced
// from a post handler waiting to be executed.
//
// If the session wrapper is abandoned by the application while in the
// Uninitiated state, it will be destroyed immediately, since no post handlers
// can have been scheduled prior to initiation.
//
// If the session wrapper is abandoned while in the Unactivated state, it will
// move immediately to the Finalized state. This may happen on any thread.
//
// The moving of a session wrapper to, or from the Actualized state always
// happen on the event loop thread. All other state transitions may happen on
// any thread.
//
// NOTE: Activation of the session happens no later than during actualization,
// and initiation of deactivation happens no earlier than during
// finalization. See also activate_session() and initiate_session_deactivation()
// in ClientImpl::Connection.
class SessionWrapper final : public util::AtomicRefCountBase, public SyncTransactReporter {
public:
    SessionWrapper(ClientImpl&, DBRef db, std::shared_ptr<SubscriptionStore>, Session::Config);
    ~SessionWrapper() noexcept;

    ClientReplication& get_replication() noexcept;
    ClientImpl& get_client() noexcept;

    bool has_flx_subscription_store() const;
    SubscriptionStore* get_flx_subscription_store();
    PendingBootstrapStore* get_flx_pending_bootstrap_store();

    void set_sync_transact_handler(util::UniqueFunction<SyncTransactCallback>);
    void set_progress_handler(util::UniqueFunction<ProgressHandler>);
    void set_connection_state_change_listener(util::UniqueFunction<ConnectionStateChangeListener>);

    void initiate();
    void initiate(ProtocolEnvelope, std::string server_address, port_type server_port, std::string virt_path,
                  std::string signed_access_token);

    void nonsync_transact_notify(version_type new_version);
    void cancel_reconnect_delay();

    void async_wait_for(bool upload_completion, bool download_completion, WaitOperCompletionHandler);
    bool wait_for_upload_complete_or_client_stopped();
    bool wait_for_download_complete_or_client_stopped();

    void refresh(std::string signed_access_token);

    static void abandon(util::bind_ptr<SessionWrapper>) noexcept;

    // These are called from ClientImpl
    void actualize(ServerEndpoint);
    void finalize();
    void finalize_before_actualization() noexcept;

    // Overriding member function in SyncTransactReporter
    void report_sync_transact(VersionID, VersionID) override;

    void on_new_flx_subscription_set(int64_t new_version);

    util::Future<std::string> send_test_command(std::string body);

    void handle_pending_client_reset_acknowledgement();

private:
    ClientImpl& m_client;
    DBRef m_db;
    Replication* m_replication;

    const ProtocolEnvelope m_protocol_envelope;
    const std::string m_server_address;
    const port_type m_server_port;
    const std::string m_authorization_header_name;
    const std::map<std::string, std::string> m_custom_http_headers;
    const bool m_verify_servers_ssl_certificate;
    const bool m_simulate_integration_error;
    const Optional<std::string> m_ssl_trust_certificate_path;
    const std::function<SyncConfig::SSLVerifyCallback> m_ssl_verify_callback;
    const size_t m_flx_bootstrap_batch_size_bytes;

    // This one is different from null when, and only when the session wrapper
    // is in ClientImpl::m_abandoned_session_wrappers.
    SessionWrapper* m_next = nullptr;

    // After initiation, these may only be accessed by the event loop thread.
    std::string m_http_request_path_prefix;
    std::string m_virt_path;
    std::string m_signed_access_token;

    util::Optional<ClientReset> m_client_reset_config;

    util::Optional<ProxyConfig> m_proxy_config;

    util::UniqueFunction<SyncTransactCallback> m_sync_transact_handler;
    util::UniqueFunction<ProgressHandler> m_progress_handler;
    util::UniqueFunction<ConnectionStateChangeListener> m_connection_state_change_listener;

    std::function<SyncClientHookAction(SyncClientHookData data)> m_debug_hook;
    bool m_in_debug_hook = false;

    std::shared_ptr<SubscriptionStore> m_flx_subscription_store;
    int64_t m_flx_active_version = 0;
    int64_t m_flx_last_seen_version = 0;
    int64_t m_flx_latest_version = 0;
    int64_t m_flx_pending_mark_version = 0;
    std::unique_ptr<PendingBootstrapStore> m_flx_pending_bootstrap_store;

    bool m_initiated = false;

    // Set to true when this session wrapper is actualized (or when it is
    // finalized before proper actualization). It is then never modified again.
    //
    // A session specific post handler submitted after the initiation of the
    // session wrapper (initiate()) will always find that `m_actualized` is
    // true. This is the case, because the scheduling of such a post handler
    // will have been preceded by the triggering of
    // `ClientImpl::m_actualize_and_finalize` (in
    // ClientImpl::register_unactualized_session_wrapper()), which ensures that
    // ClientImpl::actualize_and_finalize_session_wrappers() gets to execute
    // before the post handler. If the session wrapper is no longer in
    // `ClientImpl::m_unactualized_session_wrappers` when
    // ClientImpl::actualize_and_finalize_session_wrappers() executes, it must
    // have been abandoned already, but in that case,
    // finalize_before_actualization() has already been called.
    bool m_actualized = false;

    bool m_suspended = false;

    // Set to true when the first DOWNLOAD message is received to indicate that
    // the byte-level download progress parameters can be considered reasonable
    // reliable. Before that, a lot of time may have passed, so our record of
    // the download progress is likely completely out of date.
    bool m_reliable_download_progress = false;

    // Set to point to an activated session object during actualization of the
    // session wrapper. Set to null during finalization of the session
    // wrapper. Both modifications are guaranteed to be performed by the event
    // loop thread.
    //
    // If a session specific post handler, that is submitted after the
    // initiation of the session wrapper, sees that `m_sess` is null, it can
    // conclude that the session wrapper has been both abandoned and
    // finalized. This is true, because the scheduling of such a post handler
    // will have been preceded by the triggering of
    // `ClientImpl::m_actualize_and_finalize` (in
    // ClientImpl::register_unactualized_session_wrapper()), which ensures that
    // ClientImpl::actualize_and_finalize_session_wrappers() gets to execute
    // before the post handler, so the session wrapper must have been actualized
    // unless it was already abandoned by the application. If it was abandoned
    // before it was actualized, it will already have been finalized by
    // finalize_before_actualization().
    //
    // Must only be accessed from the event loop thread.
    SessionImpl* m_sess = nullptr;

    // These must only be accessed from the event loop thread.
    std::vector<WaitOperCompletionHandler> m_upload_completion_handlers;
    std::vector<WaitOperCompletionHandler> m_download_completion_handlers;
    std::vector<WaitOperCompletionHandler> m_sync_completion_handlers;

    // `m_target_*load_mark` and `m_reached_*load_mark` are protected by
    // `m_client.m_mutex`. `m_staged_*load_mark` must only be accessed by the
    // event loop thread.
    std::int_fast64_t m_target_upload_mark = 0, m_target_download_mark = 0;
    std::int_fast64_t m_staged_upload_mark = 0, m_staged_download_mark = 0;
    std::int_fast64_t m_reached_upload_mark = 0, m_reached_download_mark = 0;

    void do_initiate(ProtocolEnvelope, std::string server_address, port_type server_port);

    void on_sync_progress();
    void on_upload_completion();
    void on_download_completion();
    void on_suspended(const SessionErrorInfo& error_info);
    void on_resumed();
    void on_connection_state_changed(ConnectionState, const util::Optional<SessionErrorInfo>&);
    void on_flx_sync_progress(int64_t new_version, DownloadBatchState batch_state);
    void on_flx_sync_error(int64_t version, std::string_view err_msg);
    void on_flx_sync_version_complete(int64_t version);

    void report_progress();

    friend class SessionWrapperStack;
    friend class ClientImpl::Session;
};


// ################ SessionWrapperStack ################

inline bool SessionWrapperStack::empty() const noexcept
{
    return !m_back;
}


inline void SessionWrapperStack::push(util::bind_ptr<SessionWrapper> w) noexcept
{
    REALM_ASSERT(!w->m_next);
    w->m_next = m_back;
    m_back = w.release();
}


inline util::bind_ptr<SessionWrapper> SessionWrapperStack::pop() noexcept
{
    util::bind_ptr<SessionWrapper> w{m_back, util::bind_ptr_base::adopt_tag{}};
    if (m_back) {
        m_back = m_back->m_next;
        w->m_next = nullptr;
    }
    return w;
}


inline void SessionWrapperStack::clear() noexcept
{
    while (m_back) {
        util::bind_ptr<SessionWrapper> w{m_back, util::bind_ptr_base::adopt_tag{}};
        m_back = w->m_next;
    }
}


inline SessionWrapperStack::SessionWrapperStack(SessionWrapperStack&& q) noexcept
    : m_back{q.m_back}
{
    q.m_back = nullptr;
}


SessionWrapperStack::~SessionWrapperStack()
{
    clear();
}


// ################ ClientImpl ################


ClientImpl::~ClientImpl()
{
    // Since no other thread is allowed to be accessing this client or any of
    // its subobjects at this time, no mutex locking is necessary.

    drain();
    // Session wrappers are removed from m_unactualized_session_wrappers as they
    // are abandoned.
    REALM_ASSERT(m_stopped);
    REALM_ASSERT(m_unactualized_session_wrappers.empty());
}


void ClientImpl::cancel_reconnect_delay()
{
    // Thread safety required
    post([this](Status status) {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        for (auto& p : m_server_slots) {
            ServerSlot& slot = p.second;
            if (m_one_connection_per_session) {
                REALM_ASSERT(!slot.connection);
                for (const auto& p : slot.alt_connections) {
                    ClientImpl::Connection& conn = *p.second;
                    conn.resume_active_sessions(); // Throws
                    conn.cancel_reconnect_delay(); // Throws
                }
            }
            else {
                REALM_ASSERT(slot.alt_connections.empty());
                if (slot.connection) {
                    ClientImpl::Connection& conn = *slot.connection;
                    conn.resume_active_sessions(); // Throws
                    conn.cancel_reconnect_delay(); // Throws
                }
                else {
                    slot.reconnect_info.reset();
                }
            }
        }
    }); // Throws
}


bool ClientImpl::wait_for_session_terminations_or_client_stopped()
{
    // Thread safety required

    {
        std::lock_guard lock{m_mutex};
        m_sessions_terminated = false;
    }

    // The technique employed here relies on the fact that
    // actualize_and_finalize_session_wrappers() must get to execute at least
    // once before the post handler submitted below gets to execute, but still
    // at a time where all session wrappers, that are abandoned prior to the
    // execution of wait_for_session_terminations_or_client_stopped(), have been
    // added to `m_abandoned_session_wrappers`.
    //
    // To see that this is the case, consider a session wrapper that was
    // abandoned before wait_for_session_terminations_or_client_stopped() was
    // invoked. Then the session wrapper will have been added to
    // `m_abandoned_session_wrappers`, and an invocation of
    // actualize_and_finalize_session_wrappers() will have been scheduled. The
    // guarantees mentioned in the documentation of Trigger then ensure
    // that at least one execution of actualize_and_finalize_session_wrappers()
    // will happen after the session wrapper has been added to
    // `m_abandoned_session_wrappers`, but before the post handler submitted
    // below gets to execute.
    post([this](Status status) mutable {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        std::lock_guard lock{m_mutex};
        m_sessions_terminated = true;
        m_wait_or_client_stopped_cond.notify_all();
    }); // Throws

    bool completion_condition_was_satisfied;
    {
        std::unique_lock lock{m_mutex};
        while (!m_sessions_terminated && !m_stopped)
            m_wait_or_client_stopped_cond.wait(lock);
        completion_condition_was_satisfied = !m_stopped;
    }
    return completion_condition_was_satisfied;
}


void ClientImpl::drain_connections_on_loop()
{
    post([this](Status status) mutable {
        REALM_ASSERT(status.is_ok());
        actualize_and_finalize_session_wrappers();
        drain_connections();
    });
}

void ClientImpl::drain()
{
    stop();
    {
        std::lock_guard lock{m_drain_mutex};
        if (m_drained) {
            return;
        }
    }

    drain_connections_on_loop();

    std::unique_lock lock{m_drain_mutex};

    logger.debug("Waiting for %1 connections to drain", m_num_connections);
    m_drain_cv.wait(lock, [&] {
        return m_num_connections == 0 && m_outstanding_posts == 0;
    });

    m_drained = true;
}

void ClientImpl::stop() noexcept
{
    std::lock_guard lock{m_mutex};
    if (m_stopped)
        return;
    m_stopped = true;
    m_wait_or_client_stopped_cond.notify_all();
}


void ClientImpl::register_unactualized_session_wrapper(SessionWrapper* wrapper, ServerEndpoint endpoint)
{
    // Thread safety required.

    std::lock_guard lock{m_mutex};
    REALM_ASSERT(m_actualize_and_finalize);
    m_unactualized_session_wrappers.emplace(wrapper, std::move(endpoint)); // Throws
    bool retrigger = !m_actualize_and_finalize_needed;
    m_actualize_and_finalize_needed = true;
    // The conditional triggering needs to happen before releasing the mutex,
    // because if two threads call register_unactualized_session_wrapper()
    // roughly concurrently, then only the first one is guaranteed to be asked
    // to retrigger, but that retriggering must have happened before the other
    // thread returns from register_unactualized_session_wrapper().
    //
    // Note that a similar argument applies when two threads call
    // register_abandoned_session_wrapper(), and when one thread calls one of
    // them and another thread call the other.
    if (retrigger)
        m_actualize_and_finalize->trigger();
}


void ClientImpl::register_abandoned_session_wrapper(util::bind_ptr<SessionWrapper> wrapper) noexcept
{
    // Thread safety required.

    std::lock_guard lock{m_mutex};
    REALM_ASSERT(m_actualize_and_finalize);

    // If the session wrapper has not yet been actualized (on the event loop
    // thread), it can be immediately finalized. This ensures that we will
    // generally not actualize a session wrapper that has already been
    // abandoned.
    auto i = m_unactualized_session_wrappers.find(wrapper.get());
    if (i != m_unactualized_session_wrappers.end()) {
        m_unactualized_session_wrappers.erase(i);
        wrapper->finalize_before_actualization();
        return;
    }
    m_abandoned_session_wrappers.push(std::move(wrapper));
    bool retrigger = !m_actualize_and_finalize_needed;
    m_actualize_and_finalize_needed = true;
    // The conditional triggering needs to happen before releasing the
    // mutex. See implementation of register_unactualized_session_wrapper() for
    // details.
    if (retrigger)
        m_actualize_and_finalize->trigger();
}


// Must be called from the event loop thread.
void ClientImpl::actualize_and_finalize_session_wrappers()
{
    std::map<SessionWrapper*, ServerEndpoint> unactualized_session_wrappers;
    SessionWrapperStack abandoned_session_wrappers;
    {
        std::lock_guard lock{m_mutex};
        m_actualize_and_finalize_needed = false;
        swap(m_unactualized_session_wrappers, unactualized_session_wrappers);
        swap(m_abandoned_session_wrappers, abandoned_session_wrappers);
    }
    // Note, we need to finalize old session wrappers before we actualize new
    // ones. This ensures that deactivation of old sessions is initiated before
    // new session are activated. This, in turn, ensures that the server does
    // not see two overlapping sessions for the same local Realm file.
    while (util::bind_ptr<SessionWrapper> wrapper = abandoned_session_wrappers.pop())
        wrapper->finalize(); // Throws
    for (auto& p : unactualized_session_wrappers) {
        SessionWrapper& wrapper = *p.first;
        ServerEndpoint server_endpoint = std::move(p.second);
        wrapper.actualize(std::move(server_endpoint)); // Throws
    }
}


ClientImpl::Connection&
ClientImpl::get_connection(ServerEndpoint endpoint, const std::string& authorization_header_name,
                           const std::map<std::string, std::string>& custom_http_headers,
                           bool verify_servers_ssl_certificate, Optional<std::string> ssl_trust_certificate_path,
                           std::function<SyncConfig::SSLVerifyCallback> ssl_verify_callback,
                           Optional<ProxyConfig> proxy_config, SyncServerMode sync_mode, bool& was_created)
{
    ServerSlot& server_slot = m_server_slots[endpoint]; // Throws

    // TODO: enable multiplexing with proxies
    if (server_slot.connection && !m_one_connection_per_session && !proxy_config) {
        // Use preexisting connection
        REALM_ASSERT(server_slot.alt_connections.empty());
        return *server_slot.connection;
    }

    // Create a new connection
    REALM_ASSERT(!server_slot.connection);
    connection_ident_type ident = m_prev_connection_ident + 1;
    std::unique_ptr<ClientImpl::Connection> conn_2 = std::make_unique<ClientImpl::Connection>(
        *this, ident, std::move(endpoint), authorization_header_name, custom_http_headers,
        verify_servers_ssl_certificate, std::move(ssl_trust_certificate_path), std::move(ssl_verify_callback),
        std::move(proxy_config), server_slot.reconnect_info,
        sync_mode); // Throws
    ClientImpl::Connection& conn = *conn_2;
    if (!m_one_connection_per_session) {
        server_slot.connection = std::move(conn_2);
    }
    else {
        server_slot.alt_connections[ident] = std::move(conn_2); // Throws
    }
    m_prev_connection_ident = ident;
    was_created = true;
    {
        std::lock_guard lk(m_drain_mutex);
        ++m_num_connections;
    }
    return conn;
}


void ClientImpl::remove_connection(ClientImpl::Connection& conn) noexcept
{
    const ServerEndpoint& endpoint = conn.get_server_endpoint();
    auto i = m_server_slots.find(endpoint);
    REALM_ASSERT(i != m_server_slots.end()); // Must be found
    ServerSlot& server_slot = i->second;
    if (!m_one_connection_per_session) {
        REALM_ASSERT(server_slot.alt_connections.empty());
        REALM_ASSERT(&*server_slot.connection == &conn);
        server_slot.reconnect_info = conn.get_reconnect_info();
        server_slot.connection.reset();
    }
    else {
        REALM_ASSERT(!server_slot.connection);
        connection_ident_type ident = conn.get_ident();
        auto j = server_slot.alt_connections.find(ident);
        REALM_ASSERT(j != server_slot.alt_connections.end()); // Must be found
        REALM_ASSERT(&*j->second == &conn);
        server_slot.alt_connections.erase(j);
    }

    {
        std::lock_guard lk(m_drain_mutex);
        --m_num_connections;
        m_drain_cv.notify_all();
    }
}


// ################ SessionImpl ################


void SessionImpl::on_connection_state_changed(ConnectionState state,
                                              const util::Optional<SessionErrorInfo>& error_info)
{
    m_wrapper.on_connection_state_changed(state, error_info); // Throws
}


const std::string& SessionImpl::get_virt_path() const noexcept
{
    return m_wrapper.m_virt_path;
}

const std::string& SessionImpl::get_realm_path() const noexcept
{
    return m_wrapper.m_db->get_path();
}

DBRef SessionImpl::get_db() const noexcept
{
    return m_wrapper.m_db;
}

SyncTransactReporter* SessionImpl::get_transact_reporter() noexcept
{
    return &m_wrapper;
}

ClientReplication& SessionImpl::access_realm()
{
    return m_wrapper.get_replication();
}

util::Optional<ClientReset>& SessionImpl::get_client_reset_config() noexcept
{
    return m_wrapper.m_client_reset_config;
}

void SessionImpl::initiate_integrate_changesets(std::uint_fast64_t downloadable_bytes, DownloadBatchState batch_state,
                                                const SyncProgress& progress, const ReceivedChangesets& changesets)
{
    try {
        bool simulate_integration_error = (m_wrapper.m_simulate_integration_error && !changesets.empty());
        if (simulate_integration_error) {
            throw IntegrationException(ClientError::bad_changeset, "simulated failure");
        }
        version_type client_version;
        if (REALM_LIKELY(!get_client().is_dry_run())) {
            VersionInfo version_info;
            ClientReplication& repl = access_realm(); // Throws
            integrate_changesets(repl, progress, downloadable_bytes, changesets, version_info,
                                 batch_state); // Throws
            client_version = version_info.realm_version;
        }
        else {
            // Fake it for "dry run" mode
            client_version = m_last_version_available + 1;
        }
        on_changesets_integrated(client_version, progress); // Throws
    }
    catch (const IntegrationException& e) {
        on_integration_failure(e);
    }
    m_wrapper.on_sync_progress(); // Throws
}


void SessionImpl::on_upload_completion()
{
    m_wrapper.on_upload_completion(); // Throws
}


void SessionImpl::on_download_completion()
{
    m_wrapper.on_download_completion(); // Throws
}


void SessionImpl::on_suspended(const SessionErrorInfo& error_info)
{
    m_wrapper.on_suspended(error_info); // Throws
}


void SessionImpl::on_resumed()
{
    m_wrapper.on_resumed(); // Throws
}

void SessionImpl::handle_pending_client_reset_acknowledgement()
{
    m_wrapper.handle_pending_client_reset_acknowledgement();
}


bool SessionImpl::process_flx_bootstrap_message(const SyncProgress& progress, DownloadBatchState batch_state,
                                                int64_t query_version, const ReceivedChangesets& received_changesets)
{
    if (is_steady_state_download_message(batch_state, query_version)) {
        return false;
    }

    auto bootstrap_store = m_wrapper.get_flx_pending_bootstrap_store();
    util::Optional<SyncProgress> maybe_progress;
    if (batch_state == DownloadBatchState::LastInBatch) {
        maybe_progress = progress;
    }

    bool new_batch = false;
    try {
        bootstrap_store->add_batch(query_version, std::move(maybe_progress), received_changesets, &new_batch);
    }
    catch (const LogicError& ex) {
        if (ex.kind() == LogicError::binary_too_big) {
            IntegrationException ex(ClientError::bad_changeset_size,
                                    "bootstrap changeset too large to store in pending bootstrap store");
            on_integration_failure(ex);
            return true;
        }
        throw;
    }

    // If we've started a new batch and there is more to come, call on_flx_sync_progress to mark the subscription as
    // bootstrapping.
    if (new_batch && batch_state == DownloadBatchState::MoreToCome) {
        on_flx_sync_progress(query_version, DownloadBatchState::MoreToCome);
    }

    auto hook_action = call_debug_hook(SyncClientHookEvent::BootstrapMessageProcessed, progress, query_version,
                                       batch_state, received_changesets.size());
    if (hook_action == SyncClientHookAction::EarlyReturn) {
        return true;
    }
    REALM_ASSERT(hook_action == SyncClientHookAction::NoAction);

    if (batch_state == DownloadBatchState::MoreToCome) {
        return true;
    }

    try {
        process_pending_flx_bootstrap();
    }
    catch (const IntegrationException& e) {
        on_integration_failure(e);
    }

    return true;
}


void SessionImpl::process_pending_flx_bootstrap()
{
    if (!m_is_flx_sync_session) {
        return;
    }
    auto bootstrap_store = m_wrapper.get_flx_pending_bootstrap_store();
    if (!bootstrap_store->has_pending()) {
        return;
    }

    auto pending_batch_stats = bootstrap_store->pending_stats();
    logger.info("Begin processing pending FLX bootstrap for query version %1. (changesets: %2, original total "
                "changeset size: %3)",
                pending_batch_stats.query_version, pending_batch_stats.pending_changesets,
                pending_batch_stats.pending_changeset_bytes);
    auto& history = access_realm().get_history();
    VersionInfo new_version;
    SyncProgress progress;
    int64_t query_version = -1;
    size_t changesets_processed = 0;

    // Used to commit each batch after it was transformed.
    TransactionRef transact = get_db()->start_write();
    while (bootstrap_store->has_pending()) {
        auto start_time = std::chrono::steady_clock::now();
        auto pending_batch = bootstrap_store->peek_pending(m_wrapper.m_flx_bootstrap_batch_size_bytes);
        if (!pending_batch.progress) {
            logger.info("Incomplete pending bootstrap found for query version %1", pending_batch.query_version);
            // Close the write transation before clearing the bootstrap store to avoid a deadlock because the
            // bootstrap store requires a write transaction itself.
            transact->close();
            bootstrap_store->clear();
            return;
        }

        auto batch_state =
            pending_batch.remaining_changesets > 0 ? DownloadBatchState::MoreToCome : DownloadBatchState::LastInBatch;
        uint64_t downloadable_bytes = 0;
        query_version = pending_batch.query_version;
        bool simulate_integration_error =
            (m_wrapper.m_simulate_integration_error && !pending_batch.changesets.empty());
        if (simulate_integration_error) {
            throw IntegrationException(ClientError::bad_changeset, "simulated failure");
        }

        history.integrate_server_changesets(
            *pending_batch.progress, &downloadable_bytes, pending_batch.changesets, new_version, batch_state, logger,
            transact,
            [&](const TransactionRef& tr, util::Span<Changeset> changesets_applied) {
                REALM_ASSERT_3(changesets_applied.size(), <=, pending_batch.changesets.size());
                bootstrap_store->pop_front_pending(tr, changesets_applied.size());
            },
            get_transact_reporter());
        progress = *pending_batch.progress;
        changesets_processed += pending_batch.changesets.size();
        auto duration = std::chrono::steady_clock::now() - start_time;

        auto action = call_debug_hook(SyncClientHookEvent::DownloadMessageIntegrated, progress, query_version,
                                      batch_state, pending_batch.changesets.size());
        REALM_ASSERT(action == SyncClientHookAction::NoAction);

        logger.info("Integrated %1 changesets from pending bootstrap for query version %2, producing client version "
                    "%3 in %4 ms. %5 changesets remaining in bootstrap",
                    pending_batch.changesets.size(), pending_batch.query_version, new_version.realm_version,
                    std::chrono::duration_cast<std::chrono::milliseconds>(duration).count(),
                    pending_batch.remaining_changesets);
    }
    on_changesets_integrated(new_version.realm_version, progress);

    REALM_ASSERT_3(query_version, !=, -1);
    m_wrapper.on_sync_progress();
    on_flx_sync_progress(query_version, DownloadBatchState::LastInBatch);

    auto action = call_debug_hook(SyncClientHookEvent::BootstrapProcessed, progress, query_version,
                                  DownloadBatchState::LastInBatch, changesets_processed);
    // NoAction/EarlyReturn are both valid no-op actions to take here.
    REALM_ASSERT(action == SyncClientHookAction::NoAction || action == SyncClientHookAction::EarlyReturn);
}

void SessionImpl::on_new_flx_subscription_set(int64_t new_version)
{
    // If m_state == State::Active then we know that we haven't sent an UNBIND message and all we need to
    // check is that we have completed the IDENT message handshake and have not yet received an ERROR
    // message to call ensure_enlisted_to_send().
    if (m_state == State::Active && m_ident_message_sent && !m_error_message_received) {
        logger.trace("Requesting QUERY change message for new subscription set version %1", new_version);
        ensure_enlisted_to_send();
    }
}

void SessionImpl::on_flx_sync_error(int64_t version, std::string_view err_msg)
{
    m_wrapper.on_flx_sync_error(version, err_msg);
}

void SessionImpl::on_flx_sync_progress(int64_t version, DownloadBatchState batch_state)
{
    m_wrapper.on_flx_sync_progress(version, batch_state);
}

SubscriptionStore* SessionImpl::get_flx_subscription_store()
{
    return m_wrapper.get_flx_subscription_store();
}

void SessionImpl::non_sync_flx_completion(int64_t version)
{
    m_wrapper.on_flx_sync_version_complete(version);
}

SyncClientHookAction SessionImpl::call_debug_hook(const SyncClientHookData& data)
{
    // Make sure we don't call the debug hook recursively.
    if (m_wrapper.m_in_debug_hook) {
        return SyncClientHookAction::NoAction;
    }
    m_wrapper.m_in_debug_hook = true;
    auto in_hook_guard = util::make_scope_exit([&]() noexcept {
        m_wrapper.m_in_debug_hook = false;
    });

    auto action = m_wrapper.m_debug_hook(data);
    switch (action) {
        case realm::SyncClientHookAction::SuspendWithRetryableError: {
            SessionErrorInfo err_info(make_error_code(ProtocolError::other_session_error), "hook requested error",
                                      true);
            err_info.server_requests_action = ProtocolErrorInfo::Action::Transient;

            auto err_processing_err = receive_error_message(err_info);
            REALM_ASSERT(!err_processing_err);
            return SyncClientHookAction::EarlyReturn;
        }
        default:
            return action;
    }
}

SyncClientHookAction SessionImpl::call_debug_hook(SyncClientHookEvent event, const SyncProgress& progress,
                                                  int64_t query_version, DownloadBatchState batch_state,
                                                  size_t num_changesets)
{
    if (REALM_LIKELY(!m_wrapper.m_debug_hook)) {
        return SyncClientHookAction::NoAction;
    }

    SyncClientHookData data;
    data.event = event;
    data.batch_state = batch_state;
    data.progress = progress;
    data.num_changesets = num_changesets;
    data.query_version = query_version;

    return call_debug_hook(data);
}

SyncClientHookAction SessionImpl::call_debug_hook(SyncClientHookEvent event, const ProtocolErrorInfo& error_info)
{
    if (REALM_LIKELY(!m_wrapper.m_debug_hook)) {
        return SyncClientHookAction::NoAction;
    }

    SyncClientHookData data;
    data.event = event;
    data.batch_state = DownloadBatchState::SteadyState;
    data.progress = m_progress;
    data.num_changesets = 0;
    data.query_version = 0;
    data.error_info = &error_info;

    return call_debug_hook(data);
}

bool SessionImpl::is_steady_state_download_message(DownloadBatchState batch_state, int64_t query_version)
{
    if (batch_state == DownloadBatchState::SteadyState) {
        return true;
    }

    if (!m_is_flx_sync_session) {
        return true;
    }

    // If this is a steady state DOWNLOAD, no need for special handling.
    if (batch_state == DownloadBatchState::LastInBatch && query_version == m_wrapper.m_flx_active_version) {
        return true;
    }

    return false;
}

util::Future<std::string> SessionImpl::send_test_command(std::string body)
{
    try {
        auto json_body = nlohmann::json::parse(body.begin(), body.end());
        if (auto it = json_body.find("command"); it == json_body.end() || !it->is_string()) {
            return Status{ErrorCodes::LogicError,
                          "Must supply command name in \"command\" field of test command json object"};
        }
        if (json_body.size() > 1 && json_body.find("args") == json_body.end()) {
            return Status{ErrorCodes::LogicError, "Only valid fields in a test command are \"command\" and \"args\""};
        }
    }
    catch (const nlohmann::json::parse_error& e) {
        return Status{ErrorCodes::LogicError, util::format("Invalid json input to send_test_command: %1", e.what())};
    }

    auto pf = util::make_promise_future<std::string>();

    get_client().post([this, promise = std::move(pf.promise), body = std::move(body)](Status status) mutable {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        auto id = ++m_last_pending_test_command_ident;
        m_pending_test_commands.push_back(PendingTestCommand{id, std::move(body), std::move(promise)});
        ensure_enlisted_to_send();
    });

    return std::move(pf.future);
}

// ################ SessionWrapper ################

SessionWrapper::SessionWrapper(ClientImpl& client, DBRef db, std::shared_ptr<SubscriptionStore> flx_sub_store,
                               Session::Config config)
    : m_client{client}
    , m_db(std::move(db))
    , m_replication(m_db->get_replication())
    , m_protocol_envelope{config.protocol_envelope}
    , m_server_address{std::move(config.server_address)}
    , m_server_port{config.server_port}
    , m_authorization_header_name{config.authorization_header_name}
    , m_custom_http_headers{config.custom_http_headers}
    , m_verify_servers_ssl_certificate{config.verify_servers_ssl_certificate}
    , m_simulate_integration_error{config.simulate_integration_error}
    , m_ssl_trust_certificate_path{std::move(config.ssl_trust_certificate_path)}
    , m_ssl_verify_callback{std::move(config.ssl_verify_callback)}
    , m_flx_bootstrap_batch_size_bytes(config.flx_bootstrap_batch_size_bytes)
    , m_http_request_path_prefix{std::move(config.service_identifier)}
    , m_virt_path{std::move(config.realm_identifier)}
    , m_signed_access_token{std::move(config.signed_user_token)}
    , m_client_reset_config{std::move(config.client_reset_config)}
    , m_proxy_config{config.proxy_config} // Throws
    , m_debug_hook(std::move(config.on_sync_client_event_hook))
    , m_flx_subscription_store(std::move(flx_sub_store))
{
    REALM_ASSERT(m_db);
    REALM_ASSERT(m_db->get_replication());
    REALM_ASSERT(dynamic_cast<ClientReplication*>(m_db->get_replication()));

    if (m_flx_subscription_store) {
        auto versions_info = m_flx_subscription_store->get_version_info();
        m_flx_active_version = versions_info.active;
        m_flx_latest_version = versions_info.latest;
        m_flx_pending_mark_version = versions_info.pending_mark;
    }
}

SessionWrapper::~SessionWrapper() noexcept
{
    if (m_db && m_actualized)
        m_db->release_sync_agent();
}


inline ClientReplication& SessionWrapper::get_replication() noexcept
{
    return static_cast<ClientReplication&>(*m_replication);
}


inline ClientImpl& SessionWrapper::get_client() noexcept
{
    return m_client;
}

bool SessionWrapper::has_flx_subscription_store() const
{
    return static_cast<bool>(m_flx_subscription_store);
}

void SessionWrapper::on_new_flx_subscription_set(int64_t new_version)
{
    if (!m_initiated) {
        return;
    }

    auto self = util::bind_ptr<SessionWrapper>(this);
    m_client.post([new_version, self = std::move(self)](Status status) {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);
        REALM_ASSERT(self->m_actualized);
        if (REALM_UNLIKELY(!self->m_sess)) {
            return; // Already finalized
        }
        auto& sess = *self->m_sess;
        sess.recognize_sync_version(self->m_db->get_version_of_latest_snapshot());
        self->m_flx_latest_version = new_version;
        sess.on_new_flx_subscription_set(new_version);
    });
}

void SessionWrapper::on_flx_sync_error(int64_t version, std::string_view err_msg)
{
    REALM_ASSERT(m_flx_latest_version != 0);
    REALM_ASSERT(m_flx_latest_version >= version);

    auto mut_subs = get_flx_subscription_store()->get_mutable_by_version(version);
    mut_subs.update_state(SubscriptionSet::State::Error, err_msg);
    mut_subs.commit();
}

void SessionWrapper::on_flx_sync_version_complete(int64_t version)
{
    m_flx_last_seen_version = version;
    m_flx_active_version = version;
}

void SessionWrapper::on_flx_sync_progress(int64_t new_version, DownloadBatchState batch_state)
{
    if (!has_flx_subscription_store()) {
        return;
    }
    REALM_ASSERT(new_version >= m_flx_last_seen_version);
    REALM_ASSERT(new_version >= m_flx_active_version);
    REALM_ASSERT(batch_state != DownloadBatchState::SteadyState);

    SubscriptionSet::State new_state = SubscriptionSet::State::Uncommitted; // Initialize to make compiler happy

    switch (batch_state) {
        case DownloadBatchState::SteadyState:
            // Cannot be called with this value.
            REALM_UNREACHABLE();
        case DownloadBatchState::LastInBatch:
            if (m_flx_active_version == new_version) {
                return;
            }
            on_flx_sync_version_complete(new_version);
            if (new_version == 0) {
                new_state = SubscriptionSet::State::Complete;
            }
            else {
                new_state = SubscriptionSet::State::AwaitingMark;
                m_flx_pending_mark_version = new_version;
            }
            break;
        case DownloadBatchState::MoreToCome:
            if (m_flx_last_seen_version == new_version) {
                return;
            }

            m_flx_last_seen_version = new_version;
            new_state = SubscriptionSet::State::Bootstrapping;
            break;
    }

    auto mut_subs = get_flx_subscription_store()->get_mutable_by_version(new_version);
    mut_subs.update_state(new_state);
    mut_subs.commit();
}

SubscriptionStore* SessionWrapper::get_flx_subscription_store()
{
    return m_flx_subscription_store.get();
}

PendingBootstrapStore* SessionWrapper::get_flx_pending_bootstrap_store()
{
    return m_flx_pending_bootstrap_store.get();
}

inline void SessionWrapper::set_sync_transact_handler(util::UniqueFunction<SyncTransactCallback> handler)
{
    REALM_ASSERT(!m_initiated);
    m_sync_transact_handler = std::move(handler); // Throws
}


inline void SessionWrapper::set_progress_handler(util::UniqueFunction<ProgressHandler> handler)
{
    REALM_ASSERT(!m_initiated);
    m_progress_handler = std::move(handler);
}


inline void
SessionWrapper::set_connection_state_change_listener(util::UniqueFunction<ConnectionStateChangeListener> listener)
{
    REALM_ASSERT(!m_initiated);
    m_connection_state_change_listener = std::move(listener);
}


inline void SessionWrapper::initiate()
{
    // FIXME: Storing connection related information in the session object seems
    // unnecessary and goes against the idea that a session should be truely
    // lightweight (when many share a single connection). The original idea was
    // that all connection related information is passed directly from the
    // caller of initiate() to the connection constructor.
    do_initiate(m_protocol_envelope, m_server_address, m_server_port); // Throws
}


inline void SessionWrapper::initiate(ProtocolEnvelope protocol, std::string server_address, port_type server_port,
                                     std::string virt_path, std::string signed_access_token)
{
    m_virt_path = std::move(virt_path);
    m_signed_access_token = std::move(signed_access_token);
    do_initiate(protocol, std::move(server_address), server_port); // Throws
}


void SessionWrapper::nonsync_transact_notify(version_type new_version)
{
    // Thread safety required
    REALM_ASSERT(m_initiated);

    util::bind_ptr<SessionWrapper> self{this};
    m_client.post([self = std::move(self), new_version](Status status) {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        REALM_ASSERT(self->m_actualized);
        if (REALM_UNLIKELY(!self->m_sess))
            return; // Already finalized
        SessionImpl& sess = *self->m_sess;
        sess.recognize_sync_version(new_version); // Throws
        self->report_progress();                  // Throws
    });                                           // Throws
}


void SessionWrapper::cancel_reconnect_delay()
{
    // Thread safety required
    REALM_ASSERT(m_initiated);

    util::bind_ptr<SessionWrapper> self{this};
    m_client.post([self = std::move(self)](Status status) {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        REALM_ASSERT(self->m_actualized);
        if (REALM_UNLIKELY(!self->m_sess))
            return; // Already finalized
        SessionImpl& sess = *self->m_sess;
        sess.cancel_resumption_delay(); // Throws
        ClientImpl::Connection& conn = sess.get_connection();
        conn.cancel_reconnect_delay(); // Throws
    });                                // Throws
}

void SessionWrapper::async_wait_for(bool upload_completion, bool download_completion,
                                    WaitOperCompletionHandler handler)
{
    REALM_ASSERT(upload_completion || download_completion);
    REALM_ASSERT(m_initiated);

    util::bind_ptr<SessionWrapper> self{this};
    m_client.post([self = std::move(self), handler = std::move(handler), upload_completion,
                   download_completion](Status status) mutable {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        REALM_ASSERT(self->m_actualized);
        if (REALM_UNLIKELY(!self->m_sess)) {
            // Already finalized
            handler(util::error::operation_aborted); // Throws
            return;
        }
        if (upload_completion) {
            if (download_completion) {
                // Wait for upload and download completion
                self->m_sync_completion_handlers.push_back(std::move(handler)); // Throws
            }
            else {
                // Wait for upload completion only
                self->m_upload_completion_handlers.push_back(std::move(handler)); // Throws
            }
        }
        else {
            // Wait for download completion only
            self->m_download_completion_handlers.push_back(std::move(handler)); // Throws
        }
        SessionImpl& sess = *self->m_sess;
        if (upload_completion)
            sess.request_upload_completion_notification(); // Throws
        if (download_completion)
            sess.request_download_completion_notification(); // Throws
    });                                                      // Throws
}


bool SessionWrapper::wait_for_upload_complete_or_client_stopped()
{
    // Thread safety required
    REALM_ASSERT(m_initiated);

    std::int_fast64_t target_mark;
    {
        std::lock_guard lock{m_client.m_mutex};
        target_mark = ++m_target_upload_mark;
    }

    util::bind_ptr<SessionWrapper> self{this};
    m_client.post([self = std::move(self), target_mark](Status status) {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        REALM_ASSERT(self->m_actualized);
        // The session wrapper may already have been finalized. This can only
        // happen if it was abandoned, but in that case, the call of
        // wait_for_upload_complete_or_client_stopped() must have returned
        // already.
        if (REALM_UNLIKELY(!self->m_sess))
            return;
        if (target_mark > self->m_staged_upload_mark) {
            self->m_staged_upload_mark = target_mark;
            SessionImpl& sess = *self->m_sess;
            sess.request_upload_completion_notification(); // Throws
        }
    }); // Throws

    bool completion_condition_was_satisfied;
    {
        std::unique_lock lock{m_client.m_mutex};
        while (m_reached_upload_mark < target_mark && !m_client.m_stopped)
            m_client.m_wait_or_client_stopped_cond.wait(lock);
        completion_condition_was_satisfied = !m_client.m_stopped;
    }
    return completion_condition_was_satisfied;
}


bool SessionWrapper::wait_for_download_complete_or_client_stopped()
{
    // Thread safety required
    REALM_ASSERT(m_initiated);

    std::int_fast64_t target_mark;
    {
        std::lock_guard lock{m_client.m_mutex};
        target_mark = ++m_target_download_mark;
    }

    util::bind_ptr<SessionWrapper> self{this};
    m_client.post([self = std::move(self), target_mark](Status status) {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        REALM_ASSERT(self->m_actualized);
        // The session wrapper may already have been finalized. This can only
        // happen if it was abandoned, but in that case, the call of
        // wait_for_download_complete_or_client_stopped() must have returned
        // already.
        if (REALM_UNLIKELY(!self->m_sess))
            return;
        if (target_mark > self->m_staged_download_mark) {
            self->m_staged_download_mark = target_mark;
            SessionImpl& sess = *self->m_sess;
            sess.request_download_completion_notification(); // Throws
        }
    }); // Throws

    bool completion_condition_was_satisfied;
    {
        std::unique_lock lock{m_client.m_mutex};
        while (m_reached_download_mark < target_mark && !m_client.m_stopped)
            m_client.m_wait_or_client_stopped_cond.wait(lock);
        completion_condition_was_satisfied = !m_client.m_stopped;
    }
    return completion_condition_was_satisfied;
}


void SessionWrapper::refresh(std::string signed_access_token)
{
    // Thread safety required
    REALM_ASSERT(m_initiated);

    m_client.post([self = util::bind_ptr(this), token = std::move(signed_access_token)](Status status) {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        REALM_ASSERT(self->m_actualized);
        if (REALM_UNLIKELY(!self->m_sess))
            return; // Already finalized
        self->m_signed_access_token = std::move(token);
        SessionImpl& sess = *self->m_sess;
        ClientImpl::Connection& conn = sess.get_connection();
        // FIXME: This only makes sense when each session uses a separate connection.
        conn.update_connect_info(self->m_http_request_path_prefix, self->m_signed_access_token); // Throws
        sess.cancel_resumption_delay();                                                          // Throws
        conn.cancel_reconnect_delay();                                                           // Throws
    });
}


inline void SessionWrapper::abandon(util::bind_ptr<SessionWrapper> wrapper) noexcept
{
    if (wrapper->m_initiated) {
        ClientImpl& client = wrapper->m_client;
        client.register_abandoned_session_wrapper(std::move(wrapper));
    }
}


// Must be called from event loop thread
void SessionWrapper::actualize(ServerEndpoint endpoint)
{
    REALM_ASSERT(!m_actualized);
    REALM_ASSERT(!m_sess);
    m_db->claim_sync_agent();

    SyncServerMode sync_mode = m_flx_subscription_store ? SyncServerMode::FLX : SyncServerMode::PBS;

    bool was_created = false;
    ClientImpl::Connection& conn = m_client.get_connection(
        std::move(endpoint), m_authorization_header_name, m_custom_http_headers, m_verify_servers_ssl_certificate,
        m_ssl_trust_certificate_path, m_ssl_verify_callback, m_proxy_config, sync_mode,
        was_created); // Throws
    try {
        // FIXME: This only makes sense when each session uses a separate connection.
        conn.update_connect_info(m_http_request_path_prefix, m_signed_access_token);    // Throws
        std::unique_ptr<SessionImpl> sess = std::make_unique<SessionImpl>(*this, conn); // Throws
        if (sync_mode == SyncServerMode::FLX) {
            m_flx_pending_bootstrap_store = std::make_unique<PendingBootstrapStore>(m_db, sess->logger);
        }

        sess->logger.detail("Binding '%1' to '%2'", m_db->get_path(), m_virt_path); // Throws
        m_sess = sess.get();
        conn.activate_session(std::move(sess)); // Throws

        m_actualized = true;
    }
    catch (...) {
        if (was_created)
            m_client.remove_connection(conn);
        throw;
    }

    if (was_created)
        conn.activate(); // Throws

    if (m_connection_state_change_listener) {
        ConnectionState state = conn.get_state();
        if (state != ConnectionState::disconnected) {
            m_connection_state_change_listener(ConnectionState::connecting, util::none); // Throws
            if (state == ConnectionState::connected)
                m_connection_state_change_listener(ConnectionState::connected, util::none); // Throws
        }
    }

    if (!m_client_reset_config)
        report_progress(); // Throws
}


// Must be called from event loop thread
void SessionWrapper::finalize()
{
    REALM_ASSERT(m_actualized);
    REALM_ASSERT(m_sess);

    ClientImpl::Connection& conn = m_sess->get_connection();
    conn.initiate_session_deactivation(m_sess); // Throws

    // Delete the pending bootstrap store since it uses a reference to the logger in m_sess
    m_flx_pending_bootstrap_store.reset();
    m_sess = nullptr;

    // The Realm file can be closed now, as no access to the Realm file is
    // supposed to happen on behalf of a session after initiation of
    // deactivation.
    m_db->release_sync_agent();
    m_db = nullptr;

    // All outstanding wait operations must be canceled
    while (!m_upload_completion_handlers.empty()) {
        auto handler = std::move(m_upload_completion_handlers.back());
        m_upload_completion_handlers.pop_back();
        std::error_code ec = error::operation_aborted;
        handler(ec); // Throws
    }
    while (!m_download_completion_handlers.empty()) {
        auto handler = std::move(m_download_completion_handlers.back());
        m_download_completion_handlers.pop_back();
        std::error_code ec = error::operation_aborted;
        handler(ec); // Throws
    }
    while (!m_sync_completion_handlers.empty()) {
        auto handler = std::move(m_sync_completion_handlers.back());
        m_sync_completion_handlers.pop_back();
        std::error_code ec = error::operation_aborted;
        handler(ec); // Throws
    }
}


// Must be called only when an unactualized session wrapper becomes abandoned.
//
// Called with a lock on `m_client.m_mutex`.
inline void SessionWrapper::finalize_before_actualization() noexcept
{
    m_actualized = true;
}


inline void SessionWrapper::report_sync_transact(VersionID old_version, VersionID new_version)
{
    if (m_sync_transact_handler)
        m_sync_transact_handler(old_version, new_version); // Throws
}

void SessionWrapper::do_initiate(ProtocolEnvelope protocol, std::string server_address, port_type server_port)
{
    REALM_ASSERT(!m_initiated);
    ServerEndpoint server_endpoint{protocol, std::move(server_address), server_port};
    m_client.register_unactualized_session_wrapper(this, std::move(server_endpoint)); // Throws
    m_initiated = true;
}

inline void SessionWrapper::on_sync_progress()
{
    m_reliable_download_progress = true;
    report_progress(); // Throws
}


void SessionWrapper::on_upload_completion()
{
    while (!m_upload_completion_handlers.empty()) {
        auto handler = std::move(m_upload_completion_handlers.back());
        m_upload_completion_handlers.pop_back();
        std::error_code ec; // Success
        handler(ec);        // Throws
    }
    while (!m_sync_completion_handlers.empty()) {
        auto handler = std::move(m_sync_completion_handlers.back());
        m_download_completion_handlers.push_back(std::move(handler)); // Throws
        m_sync_completion_handlers.pop_back();
    }
    std::lock_guard lock{m_client.m_mutex};
    if (m_staged_upload_mark > m_reached_upload_mark) {
        m_reached_upload_mark = m_staged_upload_mark;
        m_client.m_wait_or_client_stopped_cond.notify_all();
    }
}


void SessionWrapper::on_download_completion()
{
    while (!m_download_completion_handlers.empty()) {
        auto handler = std::move(m_download_completion_handlers.back());
        m_download_completion_handlers.pop_back();
        std::error_code ec; // Success
        handler(ec);        // Throws
    }
    while (!m_sync_completion_handlers.empty()) {
        auto handler = std::move(m_sync_completion_handlers.back());
        m_upload_completion_handlers.push_back(std::move(handler)); // Throws
        m_sync_completion_handlers.pop_back();
    }

    if (m_flx_subscription_store && m_flx_pending_mark_version != SubscriptionSet::EmptyVersion) {
        m_sess->logger.debug("Marking query version %1 as complete after receiving MARK message",
                             m_flx_pending_mark_version);
        auto mutable_subs = m_flx_subscription_store->get_mutable_by_version(m_flx_pending_mark_version);
        mutable_subs.update_state(SubscriptionSet::State::Complete);
        mutable_subs.commit();
        m_flx_pending_mark_version = SubscriptionSet::EmptyVersion;
    }

    std::lock_guard lock{m_client.m_mutex};
    if (m_staged_download_mark > m_reached_download_mark) {
        m_reached_download_mark = m_staged_download_mark;
        m_client.m_wait_or_client_stopped_cond.notify_all();
    }
}


void SessionWrapper::on_suspended(const SessionErrorInfo& error_info)
{
    m_suspended = true;
    if (m_connection_state_change_listener) {
        m_connection_state_change_listener(ConnectionState::disconnected, error_info); // Throws
    }
}


void SessionWrapper::on_resumed()
{
    m_suspended = false;
    if (m_connection_state_change_listener) {
        ClientImpl::Connection& conn = m_sess->get_connection();
        if (conn.get_state() != ConnectionState::disconnected) {
            m_connection_state_change_listener(ConnectionState::connecting, util::none); // Throws
            if (conn.get_state() == ConnectionState::connected)
                m_connection_state_change_listener(ConnectionState::connected, util::none); // Throws
        }
    }
}


void SessionWrapper::on_connection_state_changed(ConnectionState state,
                                                 const util::Optional<SessionErrorInfo>& error_info)
{
    if (m_connection_state_change_listener) {
        if (!m_suspended)
            m_connection_state_change_listener(state, error_info); // Throws
    }
}


void SessionWrapper::report_progress()
{
    REALM_ASSERT(m_sess);

    if (!m_progress_handler)
        return;

    std::uint_fast64_t downloaded_bytes = 0;
    std::uint_fast64_t downloadable_bytes = 0;
    std::uint_fast64_t uploaded_bytes = 0;
    std::uint_fast64_t uploadable_bytes = 0;
    std::uint_fast64_t snapshot_version = 0;
    ClientHistory::get_upload_download_bytes(m_db.get(), downloaded_bytes, downloadable_bytes, uploaded_bytes,
                                             uploadable_bytes, snapshot_version);

    // uploadable_bytes is uploaded + remaining to upload, while downloadable_bytes
    // is only the remaining to download. This is confusing, so make them use
    // the same units.
    std::uint_fast64_t total_bytes = downloaded_bytes + downloadable_bytes;

    m_sess->logger.debug("Progress handler called, downloaded = %1, "
                         "downloadable(total) = %2, uploaded = %3, "
                         "uploadable = %4, reliable_download_progress = %5, "
                         "snapshot version = %6",
                         downloaded_bytes, total_bytes, uploaded_bytes, uploadable_bytes,
                         m_reliable_download_progress, snapshot_version);

    // FIXME: Why is this boolean status communicated to the application as
    // a 64-bit integer? Also, the name `progress_version` is confusing.
    std::uint_fast64_t progress_version = (m_reliable_download_progress ? 1 : 0);
    m_progress_handler(downloaded_bytes, total_bytes, uploaded_bytes, uploadable_bytes, progress_version,
                       snapshot_version);
}

util::Future<std::string> SessionWrapper::send_test_command(std::string body)
{
    if (!m_sess) {
        return util::Future<std::string>::make_ready(
            Status{ErrorCodes::RuntimeError, "session must be activated to send a test command"});
    }

    return m_sess->send_test_command(std::move(body));
}

void SessionWrapper::handle_pending_client_reset_acknowledgement()
{
    auto pending_reset = [&] {
        auto ft = m_db->start_frozen();
        return _impl::client_reset::has_pending_reset(ft);
    }();
    REALM_ASSERT(pending_reset);
    m_sess->logger.info("Tracking pending client reset of type \"%1\" from %2", pending_reset->type,
                        pending_reset->time);
    util::bind_ptr<SessionWrapper> self(this);
    async_wait_for(true, true, [self = std::move(self), pending_reset = *pending_reset](std::error_code ec) {
        if (ec == util::error::operation_aborted) {
            return;
        }
        auto& logger = self->m_sess->logger;
        if (ec) {
            logger.error("Error while tracking client reset acknowledgement: %1", ec.message());
            return;
        }

        auto wt = self->m_db->start_write();
        auto cur_pending_reset = _impl::client_reset::has_pending_reset(wt);
        if (!cur_pending_reset) {
            logger.debug(
                "Was going to remove client reset tracker for type \"%1\" from %2, but it was already removed",
                pending_reset.type, pending_reset.time);
            return;
        }
        else if (cur_pending_reset->type != pending_reset.type || cur_pending_reset->time != pending_reset.time) {
            logger.debug(
                "Was going to remove client reset tracker for type \"%1\" from %2, but found type \"%3\" from %4.",
                pending_reset.type, pending_reset.time, cur_pending_reset->type, cur_pending_reset->time);
        }
        else {
            logger.debug("Client reset of type \"%1\" from %2 has been acknowledged by the server. "
                         "Removing cycle detection tracker.",
                         pending_reset.type, pending_reset.time);
        }
        _impl::client_reset::remove_pending_client_resets(wt);
        wt->commit();
    });
}

// ################ ClientImpl::Connection ################

ClientImpl::Connection::Connection(ClientImpl& client, connection_ident_type ident, ServerEndpoint endpoint,
                                   const std::string& authorization_header_name,
                                   const std::map<std::string, std::string>& custom_http_headers,
                                   bool verify_servers_ssl_certificate,
                                   Optional<std::string> ssl_trust_certificate_path,
                                   std::function<SSLVerifyCallback> ssl_verify_callback,
                                   Optional<ProxyConfig> proxy_config, ReconnectInfo reconnect_info,
                                   SyncServerMode sync_mode)
    : logger_ptr{std::make_shared<util::PrefixLogger>(make_logger_prefix(ident), client.logger_ptr)} // Throws
    , logger{*logger_ptr}
    , m_client{client}
    , m_protocol_envelope{std::get<0>(endpoint)}
    , m_address{std::get<1>(endpoint)}
    , m_port{std::get<2>(endpoint)}
    , m_verify_servers_ssl_certificate{verify_servers_ssl_certificate}    // DEPRECATED
    , m_ssl_trust_certificate_path{std::move(ssl_trust_certificate_path)} // DEPRECATED
    , m_ssl_verify_callback{std::move(ssl_verify_callback)}               // DEPRECATED
    , m_proxy_config{std::move(proxy_config)}                             // DEPRECATED
    , m_reconnect_info{reconnect_info}
    , m_sync_mode(sync_mode)
    , m_ident{ident}
    , m_server_endpoint{std::move(endpoint)}
    , m_authorization_header_name{authorization_header_name} // DEPRECATED
    , m_custom_http_headers{custom_http_headers}             // DEPRECATED
{
    m_on_idle = m_client.create_trigger([this](Status status) {
        if (status == ErrorCodes::OperationAborted)
            return;
        else if (!status.is_ok())
            throw ExceptionForStatus(status);

        REALM_ASSERT(m_activated);
        if (m_state == ConnectionState::disconnected && m_num_active_sessions == 0) {
            on_idle(); // Throws
            // Connection object may be destroyed now.
        }
    });
}

inline connection_ident_type ClientImpl::Connection::get_ident() const noexcept
{
    return m_ident;
}


inline const ServerEndpoint& ClientImpl::Connection::get_server_endpoint() const noexcept
{
    return m_server_endpoint;
}

inline void ClientImpl::Connection::update_connect_info(const std::string& http_request_path_prefix,
                                                        const std::string& signed_access_token)
{
    m_http_request_path_prefix = http_request_path_prefix; // Throws (copy)
    m_signed_access_token = signed_access_token;           // Throws (copy)
}


void ClientImpl::Connection::resume_active_sessions()
{
    auto handler = [=](ClientImpl::Session& sess) {
        sess.cancel_resumption_delay(); // Throws
    };
    for_each_active_session(std::move(handler)); // Throws
}

void ClientImpl::Connection::on_idle()
{
    logger.debug("Destroying connection object");
    ClientImpl& client = get_client();
    client.remove_connection(*this);
    // NOTE: This connection object is now destroyed!
}


std::string ClientImpl::Connection::get_http_request_path() const
{
    using namespace std::string_view_literals;
    const auto param = m_http_request_path_prefix.find('?') == std::string::npos ? "?baas_at="sv : "&baas_at="sv;

    std::string path;
    path.reserve(m_http_request_path_prefix.size() + param.size() + m_signed_access_token.size());
    path += m_http_request_path_prefix;
    path += param;
    path += m_signed_access_token;

    return path;
}


std::string ClientImpl::Connection::make_logger_prefix(connection_ident_type ident)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "Connection[" << ident << "]: "; // Throws
    return out.str();                       // Throws
}


void ClientImpl::Connection::report_connection_state_change(ConnectionState state,
                                                            util::Optional<SessionErrorInfo> error_info)
{
    auto handler = [=](ClientImpl::Session& sess) {
        SessionImpl& sess_2 = static_cast<SessionImpl&>(sess);
        sess_2.on_connection_state_changed(state, error_info); // Throws
    };
    for_each_active_session(std::move(handler)); // Throws
}


Client::Client(Config config)
    : m_impl{new ClientImpl{std::move(config)}} // Throws
{
}


Client::Client(Client&& client) noexcept
    : m_impl{std::move(client.m_impl)}
{
}


Client::~Client() noexcept {}


void Client::stop() noexcept
{
    m_impl->stop();
}

void Client::drain()
{
    m_impl->drain();
}

void Client::cancel_reconnect_delay()
{
    m_impl->cancel_reconnect_delay();
}


bool Client::wait_for_session_terminations_or_client_stopped()
{
    return m_impl.get()->wait_for_session_terminations_or_client_stopped();
}


bool Client::decompose_server_url(const std::string& url, ProtocolEnvelope& protocol, std::string& address,
                                  port_type& port, std::string& path) const
{
    return m_impl->decompose_server_url(url, protocol, address, port, path); // Throws
}


Session::Session(Client& client, DBRef db, std::shared_ptr<SubscriptionStore> flx_sub_store, Config&& config)
{
    util::bind_ptr<SessionWrapper> sess;
    sess.reset(
        new SessionWrapper{*client.m_impl, std::move(db), std::move(flx_sub_store), std::move(config)}); // Throws
    // The reference count passed back to the application is implicitly
    // owned by a naked pointer. This is done to avoid exposing
    // implementation details through the header file (that is, through the
    // Session object).
    m_impl = sess.release();
}


void Session::set_sync_transact_callback(util::UniqueFunction<SyncTransactCallback> handler)
{
    m_impl->set_sync_transact_handler(std::move(handler)); // Throws
}


void Session::set_progress_handler(util::UniqueFunction<ProgressHandler> handler)
{
    m_impl->set_progress_handler(std::move(handler)); // Throws
}


void Session::set_connection_state_change_listener(util::UniqueFunction<ConnectionStateChangeListener> listener)
{
    m_impl->set_connection_state_change_listener(std::move(listener)); // Throws
}


void Session::bind()
{
    m_impl->initiate(); // Throws
}


void Session::bind(std::string server_url, std::string signed_access_token)
{
    ClientImpl& client = m_impl->get_client();
    ProtocolEnvelope protocol = {};
    std::string address;
    port_type port = {};
    std::string path;
    if (!client.decompose_server_url(server_url, protocol, address, port, path)) // Throws
        throw BadServerUrl();
    bind(std::move(address), std::move(path), std::move(signed_access_token), port, protocol); // Throws
}


void Session::bind(std::string server_address, std::string realm_identifier, std::string signed_access_token,
                   port_type server_port, ProtocolEnvelope protocol)
{
    m_impl->initiate(protocol, std::move(server_address), server_port, std::move(realm_identifier),
                     std::move(signed_access_token)); // Throws
}


void Session::nonsync_transact_notify(version_type new_version)
{
    m_impl->nonsync_transact_notify(new_version); // Throws
}


void Session::cancel_reconnect_delay()
{
    m_impl->cancel_reconnect_delay(); // Throws
}


void Session::async_wait_for(bool upload_completion, bool download_completion, WaitOperCompletionHandler handler)
{
    m_impl->async_wait_for(upload_completion, download_completion, std::move(handler)); // Throws
}


bool Session::wait_for_upload_complete_or_client_stopped()
{
    return m_impl->wait_for_upload_complete_or_client_stopped(); // Throws
}


bool Session::wait_for_download_complete_or_client_stopped()
{
    return m_impl->wait_for_download_complete_or_client_stopped(); // Throws
}


void Session::refresh(const std::string& signed_access_token)
{
    m_impl->refresh(signed_access_token); // Throws
}


void Session::abandon() noexcept
{
    REALM_ASSERT(m_impl);
    // Reabsorb the ownership assigned to the applications naked pointer by
    // Session constructor
    util::bind_ptr<SessionWrapper> wrapper{m_impl, util::bind_ptr_base::adopt_tag{}};
    SessionWrapper::abandon(std::move(wrapper));
}

void Session::on_new_flx_sync_subscription(int64_t new_version)
{
    m_impl->on_new_flx_subscription_set(new_version);
}

util::Future<std::string> Session::send_test_command(std::string body)
{
    return m_impl->send_test_command(std::move(body));
}

const std::error_category& client_error_category() noexcept
{
    return g_error_category;
}


std::error_code make_error_code(ClientError error_code) noexcept
{
    return std::error_code{int(error_code), g_error_category};
}

std::ostream& operator<<(std::ostream& os, ProxyConfig::Type proxyType)
{
    switch (proxyType) {
        case ProxyConfig::Type::HTTP:
            return os << "HTTP";
        case ProxyConfig::Type::HTTPS:
            return os << "HTTPS";
    }
    REALM_TERMINATE("Invalid Proxy Type object.");
}

} // namespace sync
} // namespace realm
