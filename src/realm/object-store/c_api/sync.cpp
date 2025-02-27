////////////////////////////////////////////////////////////////////////////
//
// Copyright 2021 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or utilied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include <realm/sync/config.hpp>
#include <realm/sync/client.hpp>
#include <realm/sync/protocol.hpp>
#include <realm/sync/network/network.hpp>
#include <realm/object-store/c_api/conversion.hpp>
#include <realm/object-store/sync/sync_manager.hpp>
#include <realm/object-store/sync/sync_session.hpp>
#include <realm/object-store/sync/async_open_task.hpp>
#include <realm/util/basic_system_errors.hpp>

#include "logging.hpp"
#include "types.hpp"
#include "util.hpp"


realm_async_open_task_progress_notification_token::~realm_async_open_task_progress_notification_token()
{
    task->unregister_download_progress_notifier(token);
}

realm_sync_session_connection_state_notification_token::~realm_sync_session_connection_state_notification_token()
{
    session->unregister_connection_change_callback(token);
}

namespace realm::c_api {

static_assert(realm_sync_client_metadata_mode_e(SyncClientConfig::MetadataMode::NoEncryption) ==
              RLM_SYNC_CLIENT_METADATA_MODE_PLAINTEXT);
static_assert(realm_sync_client_metadata_mode_e(SyncClientConfig::MetadataMode::Encryption) ==
              RLM_SYNC_CLIENT_METADATA_MODE_ENCRYPTED);
static_assert(realm_sync_client_metadata_mode_e(SyncClientConfig::MetadataMode::NoMetadata) ==
              RLM_SYNC_CLIENT_METADATA_MODE_DISABLED);

static_assert(realm_sync_client_reconnect_mode_e(ReconnectMode::normal) == RLM_SYNC_CLIENT_RECONNECT_MODE_NORMAL);
static_assert(realm_sync_client_reconnect_mode_e(ReconnectMode::testing) == RLM_SYNC_CLIENT_RECONNECT_MODE_TESTING);

static_assert(realm_sync_session_resync_mode_e(ClientResyncMode::Manual) == RLM_SYNC_SESSION_RESYNC_MODE_MANUAL);
static_assert(realm_sync_session_resync_mode_e(ClientResyncMode::DiscardLocal) ==
              RLM_SYNC_SESSION_RESYNC_MODE_DISCARD_LOCAL);
static_assert(realm_sync_session_resync_mode_e(ClientResyncMode::Recover) == RLM_SYNC_SESSION_RESYNC_MODE_RECOVER);
static_assert(realm_sync_session_resync_mode_e(ClientResyncMode::RecoverOrDiscard) ==
              RLM_SYNC_SESSION_RESYNC_MODE_RECOVER_OR_DISCARD);

static_assert(realm_sync_session_stop_policy_e(SyncSessionStopPolicy::Immediately) ==
              RLM_SYNC_SESSION_STOP_POLICY_IMMEDIATELY);
static_assert(realm_sync_session_stop_policy_e(SyncSessionStopPolicy::LiveIndefinitely) ==
              RLM_SYNC_SESSION_STOP_POLICY_LIVE_INDEFINITELY);
static_assert(realm_sync_session_stop_policy_e(SyncSessionStopPolicy::AfterChangesUploaded) ==
              RLM_SYNC_SESSION_STOP_POLICY_AFTER_CHANGES_UPLOADED);

static_assert(realm_sync_session_state_e(SyncSession::State::Active) == RLM_SYNC_SESSION_STATE_ACTIVE);
static_assert(realm_sync_session_state_e(SyncSession::State::Dying) == RLM_SYNC_SESSION_STATE_DYING);
static_assert(realm_sync_session_state_e(SyncSession::State::Inactive) == RLM_SYNC_SESSION_STATE_INACTIVE);
static_assert(realm_sync_session_state_e(SyncSession::State::WaitingForAccessToken) ==
              RLM_SYNC_SESSION_STATE_WAITING_FOR_ACCESS_TOKEN);
static_assert(realm_sync_session_state_e(SyncSession::State::Paused) == RLM_SYNC_SESSION_STATE_PAUSED);

static_assert(realm_sync_connection_state_e(SyncSession::ConnectionState::Disconnected) ==
              RLM_SYNC_CONNECTION_STATE_DISCONNECTED);
static_assert(realm_sync_connection_state_e(SyncSession::ConnectionState::Connecting) ==
              RLM_SYNC_CONNECTION_STATE_CONNECTING);
static_assert(realm_sync_connection_state_e(SyncSession::ConnectionState::Connected) ==
              RLM_SYNC_CONNECTION_STATE_CONNECTED);

static_assert(realm_sync_progress_direction_e(SyncSession::ProgressDirection::upload) ==
              RLM_SYNC_PROGRESS_DIRECTION_UPLOAD);
static_assert(realm_sync_progress_direction_e(SyncSession::ProgressDirection::download) ==
              RLM_SYNC_PROGRESS_DIRECTION_DOWNLOAD);

namespace {
using realm::sync::Client;
static_assert(realm_sync_errno_client_e(Client::Error::connection_closed) == RLM_SYNC_ERR_CLIENT_CONNECTION_CLOSED);
static_assert(realm_sync_errno_client_e(Client::Error::unknown_message) == RLM_SYNC_ERR_CLIENT_UNKNOWN_MESSAGE);
static_assert(realm_sync_errno_client_e(Client::Error::bad_syntax) == RLM_SYNC_ERR_CLIENT_BAD_SYNTAX);
static_assert(realm_sync_errno_client_e(Client::Error::limits_exceeded) == RLM_SYNC_ERR_CLIENT_LIMITS_EXCEEDED);
static_assert(realm_sync_errno_client_e(Client::Error::bad_session_ident) == RLM_SYNC_ERR_CLIENT_BAD_SESSION_IDENT);
static_assert(realm_sync_errno_client_e(Client::Error::bad_message_order) == RLM_SYNC_ERR_CLIENT_BAD_MESSAGE_ORDER);
static_assert(realm_sync_errno_client_e(Client::Error::bad_client_file_ident) ==
              RLM_SYNC_ERR_CLIENT_BAD_CLIENT_FILE_IDENT);
static_assert(realm_sync_errno_client_e(Client::Error::bad_progress) == RLM_SYNC_ERR_CLIENT_BAD_PROGRESS);
static_assert(realm_sync_errno_client_e(Client::Error::bad_changeset_header_syntax) ==
              RLM_SYNC_ERR_CLIENT_BAD_CHANGESET_HEADER_SYNTAX);
static_assert(realm_sync_errno_client_e(Client::Error::bad_changeset_size) == RLM_SYNC_ERR_CLIENT_BAD_CHANGESET_SIZE);
static_assert(realm_sync_errno_client_e(Client::Error::bad_origin_file_ident) ==
              RLM_SYNC_ERR_CLIENT_BAD_ORIGIN_FILE_IDENT);
static_assert(realm_sync_errno_client_e(Client::Error::bad_server_version) == RLM_SYNC_ERR_CLIENT_BAD_SERVER_VERSION);
static_assert(realm_sync_errno_client_e(Client::Error::bad_changeset) == RLM_SYNC_ERR_CLIENT_BAD_CHANGESET);
static_assert(realm_sync_errno_client_e(Client::Error::bad_request_ident) == RLM_SYNC_ERR_CLIENT_BAD_REQUEST_IDENT);
static_assert(realm_sync_errno_client_e(Client::Error::bad_error_code) == RLM_SYNC_ERR_CLIENT_BAD_ERROR_CODE);
static_assert(realm_sync_errno_client_e(Client::Error::bad_compression) == RLM_SYNC_ERR_CLIENT_BAD_COMPRESSION);
static_assert(realm_sync_errno_client_e(Client::Error::bad_client_version) == RLM_SYNC_ERR_CLIENT_BAD_CLIENT_VERSION);
static_assert(realm_sync_errno_client_e(Client::Error::ssl_server_cert_rejected) ==
              RLM_SYNC_ERR_CLIENT_SSL_SERVER_CERT_REJECTED);
static_assert(realm_sync_errno_client_e(Client::Error::pong_timeout) == RLM_SYNC_ERR_CLIENT_PONG_TIMEOUT);
static_assert(realm_sync_errno_client_e(Client::Error::bad_client_file_ident_salt) ==
              RLM_SYNC_ERR_CLIENT_BAD_CLIENT_FILE_IDENT_SALT);
static_assert(realm_sync_errno_client_e(Client::Error::bad_file_ident) == RLM_SYNC_ERR_CLIENT_BAD_FILE_IDENT);
static_assert(realm_sync_errno_client_e(Client::Error::connect_timeout) == RLM_SYNC_ERR_CLIENT_CONNECT_TIMEOUT);
static_assert(realm_sync_errno_client_e(Client::Error::bad_timestamp) == RLM_SYNC_ERR_CLIENT_BAD_TIMESTAMP);
static_assert(realm_sync_errno_client_e(Client::Error::bad_protocol_from_server) ==
              RLM_SYNC_ERR_CLIENT_BAD_PROTOCOL_FROM_SERVER);
static_assert(realm_sync_errno_client_e(Client::Error::client_too_old_for_server) ==
              RLM_SYNC_ERR_CLIENT_CLIENT_TOO_OLD_FOR_SERVER);
static_assert(realm_sync_errno_client_e(Client::Error::client_too_new_for_server) ==
              RLM_SYNC_ERR_CLIENT_CLIENT_TOO_NEW_FOR_SERVER);
static_assert(realm_sync_errno_client_e(Client::Error::protocol_mismatch) == RLM_SYNC_ERR_CLIENT_PROTOCOL_MISMATCH);
static_assert(realm_sync_errno_client_e(Client::Error::bad_state_message) == RLM_SYNC_ERR_CLIENT_BAD_STATE_MESSAGE);
static_assert(realm_sync_errno_client_e(Client::Error::missing_protocol_feature) ==
              RLM_SYNC_ERR_CLIENT_MISSING_PROTOCOL_FEATURE);
static_assert(realm_sync_errno_client_e(Client::Error::http_tunnel_failed) == RLM_SYNC_ERR_CLIENT_HTTP_TUNNEL_FAILED);
static_assert(realm_sync_errno_client_e(Client::Error::auto_client_reset_failure) ==
              RLM_SYNC_ERR_CLIENT_AUTO_CLIENT_RESET_FAILURE);
} // namespace

namespace {
using namespace realm::sync;
static_assert(realm_sync_errno_connection_e(ProtocolError::connection_closed) ==
              RLM_SYNC_ERR_CONNECTION_CONNECTION_CLOSED);
static_assert(realm_sync_errno_connection_e(ProtocolError::other_error) == RLM_SYNC_ERR_CONNECTION_OTHER_ERROR);
static_assert(realm_sync_errno_connection_e(ProtocolError::unknown_message) ==
              RLM_SYNC_ERR_CONNECTION_UNKNOWN_MESSAGE);
static_assert(realm_sync_errno_connection_e(ProtocolError::bad_syntax) == RLM_SYNC_ERR_CONNECTION_BAD_SYNTAX);
static_assert(realm_sync_errno_connection_e(ProtocolError::limits_exceeded) ==
              RLM_SYNC_ERR_CONNECTION_LIMITS_EXCEEDED);
static_assert(realm_sync_errno_connection_e(ProtocolError::wrong_protocol_version) ==
              RLM_SYNC_ERR_CONNECTION_WRONG_PROTOCOL_VERSION);
static_assert(realm_sync_errno_connection_e(ProtocolError::bad_session_ident) ==
              RLM_SYNC_ERR_CONNECTION_BAD_SESSION_IDENT);
static_assert(realm_sync_errno_connection_e(ProtocolError::reuse_of_session_ident) ==
              RLM_SYNC_ERR_CONNECTION_REUSE_OF_SESSION_IDENT);
static_assert(realm_sync_errno_connection_e(ProtocolError::bound_in_other_session) ==
              RLM_SYNC_ERR_CONNECTION_BOUND_IN_OTHER_SESSION);
static_assert(realm_sync_errno_connection_e(ProtocolError::bad_message_order) ==
              RLM_SYNC_ERR_CONNECTION_BAD_MESSAGE_ORDER);
static_assert(realm_sync_errno_connection_e(ProtocolError::bad_decompression) ==
              RLM_SYNC_ERR_CONNECTION_BAD_DECOMPRESSION);
static_assert(realm_sync_errno_connection_e(ProtocolError::bad_changeset_header_syntax) ==
              RLM_SYNC_ERR_CONNECTION_BAD_CHANGESET_HEADER_SYNTAX);
static_assert(realm_sync_errno_connection_e(ProtocolError::bad_changeset_size) ==
              RLM_SYNC_ERR_CONNECTION_BAD_CHANGESET_SIZE);
static_assert(realm_sync_errno_connection_e(ProtocolError::switch_to_flx_sync) ==
              RLM_SYNC_ERR_CONNECTION_SWITCH_TO_FLX_SYNC);
static_assert(realm_sync_errno_connection_e(ProtocolError::switch_to_pbs) == RLM_SYNC_ERR_CONNECTION_SWITCH_TO_PBS);

static_assert(realm_sync_errno_session_e(ProtocolError::session_closed) == RLM_SYNC_ERR_SESSION_SESSION_CLOSED);
static_assert(realm_sync_errno_session_e(ProtocolError::other_session_error) ==
              RLM_SYNC_ERR_SESSION_OTHER_SESSION_ERROR);
static_assert(realm_sync_errno_session_e(ProtocolError::token_expired) == RLM_SYNC_ERR_SESSION_TOKEN_EXPIRED);
static_assert(realm_sync_errno_session_e(ProtocolError::bad_authentication) ==
              RLM_SYNC_ERR_SESSION_BAD_AUTHENTICATION);
static_assert(realm_sync_errno_session_e(ProtocolError::illegal_realm_path) ==
              RLM_SYNC_ERR_SESSION_ILLEGAL_REALM_PATH);
static_assert(realm_sync_errno_session_e(ProtocolError::no_such_realm) == RLM_SYNC_ERR_SESSION_NO_SUCH_REALM);
static_assert(realm_sync_errno_session_e(ProtocolError::permission_denied) == RLM_SYNC_ERR_SESSION_PERMISSION_DENIED);
static_assert(realm_sync_errno_session_e(ProtocolError::bad_server_file_ident) ==
              RLM_SYNC_ERR_SESSION_BAD_SERVER_FILE_IDENT);
static_assert(realm_sync_errno_session_e(ProtocolError::bad_client_file_ident) ==
              RLM_SYNC_ERR_SESSION_BAD_CLIENT_FILE_IDENT);
static_assert(realm_sync_errno_session_e(ProtocolError::bad_server_version) ==
              RLM_SYNC_ERR_SESSION_BAD_SERVER_VERSION);
static_assert(realm_sync_errno_session_e(ProtocolError::bad_client_version) ==
              RLM_SYNC_ERR_SESSION_BAD_CLIENT_VERSION);
static_assert(realm_sync_errno_session_e(ProtocolError::diverging_histories) ==
              RLM_SYNC_ERR_SESSION_DIVERGING_HISTORIES);
static_assert(realm_sync_errno_session_e(ProtocolError::bad_changeset) == RLM_SYNC_ERR_SESSION_BAD_CHANGESET);
static_assert(realm_sync_errno_session_e(ProtocolError::partial_sync_disabled) ==
              RLM_SYNC_ERR_SESSION_PARTIAL_SYNC_DISABLED);
static_assert(realm_sync_errno_session_e(ProtocolError::unsupported_session_feature) ==
              RLM_SYNC_ERR_SESSION_UNSUPPORTED_SESSION_FEATURE);
static_assert(realm_sync_errno_session_e(ProtocolError::bad_origin_file_ident) ==
              RLM_SYNC_ERR_SESSION_BAD_ORIGIN_FILE_IDENT);
static_assert(realm_sync_errno_session_e(ProtocolError::bad_client_file) == RLM_SYNC_ERR_SESSION_BAD_CLIENT_FILE);
static_assert(realm_sync_errno_session_e(ProtocolError::server_file_deleted) ==
              RLM_SYNC_ERR_SESSION_SERVER_FILE_DELETED);
static_assert(realm_sync_errno_session_e(ProtocolError::client_file_blacklisted) ==
              RLM_SYNC_ERR_SESSION_CLIENT_FILE_BLACKLISTED);
static_assert(realm_sync_errno_session_e(ProtocolError::user_blacklisted) == RLM_SYNC_ERR_SESSION_USER_BLACKLISTED);
static_assert(realm_sync_errno_session_e(ProtocolError::transact_before_upload) ==
              RLM_SYNC_ERR_SESSION_TRANSACT_BEFORE_UPLOAD);
static_assert(realm_sync_errno_session_e(ProtocolError::client_file_expired) ==
              RLM_SYNC_ERR_SESSION_CLIENT_FILE_EXPIRED);
static_assert(realm_sync_errno_session_e(ProtocolError::user_mismatch) == RLM_SYNC_ERR_SESSION_USER_MISMATCH);
static_assert(realm_sync_errno_session_e(ProtocolError::too_many_sessions) == RLM_SYNC_ERR_SESSION_TOO_MANY_SESSIONS);
static_assert(realm_sync_errno_session_e(ProtocolError::invalid_schema_change) ==
              RLM_SYNC_ERR_SESSION_INVALID_SCHEMA_CHANGE);
static_assert(realm_sync_errno_session_e(ProtocolError::bad_query) == RLM_SYNC_ERR_SESSION_BAD_QUERY);
static_assert(realm_sync_errno_session_e(ProtocolError::object_already_exists) ==
              RLM_SYNC_ERR_SESSION_OBJECT_ALREADY_EXISTS);
static_assert(realm_sync_errno_session_e(ProtocolError::server_permissions_changed) ==
              RLM_SYNC_ERR_SESSION_SERVER_PERMISSIONS_CHANGED);
static_assert(realm_sync_errno_session_e(ProtocolError::initial_sync_not_completed) ==
              RLM_SYNC_ERR_SESSION_INITIAL_SYNC_NOT_COMPLETED);
static_assert(realm_sync_errno_session_e(ProtocolError::write_not_allowed) == RLM_SYNC_ERR_SESSION_WRITE_NOT_ALLOWED);
static_assert(realm_sync_errno_session_e(ProtocolError::compensating_write) ==
              RLM_SYNC_ERR_SESSION_COMPENSATING_WRITE);

static_assert(realm_sync_error_action_e(ProtocolErrorInfo::Action::NoAction) == RLM_SYNC_ERROR_ACTION_NO_ACTION);
static_assert(realm_sync_error_action_e(ProtocolErrorInfo::Action::ProtocolViolation) ==
              RLM_SYNC_ERROR_ACTION_PROTOCOL_VIOLATION);
static_assert(realm_sync_error_action_e(ProtocolErrorInfo::Action::ApplicationBug) ==
              RLM_SYNC_ERROR_ACTION_APPLICATION_BUG);
static_assert(realm_sync_error_action_e(ProtocolErrorInfo::Action::Warning) == RLM_SYNC_ERROR_ACTION_WARNING);
static_assert(realm_sync_error_action_e(ProtocolErrorInfo::Action::Transient) == RLM_SYNC_ERROR_ACTION_TRANSIENT);
static_assert(realm_sync_error_action_e(ProtocolErrorInfo::Action::DeleteRealm) ==
              RLM_SYNC_ERROR_ACTION_DELETE_REALM);
static_assert(realm_sync_error_action_e(ProtocolErrorInfo::Action::ClientReset) ==
              RLM_SYNC_ERROR_ACTION_CLIENT_RESET);
static_assert(realm_sync_error_action_e(ProtocolErrorInfo::Action::ClientResetNoRecovery) ==
              RLM_SYNC_ERROR_ACTION_CLIENT_RESET_NO_RECOVERY);

static_assert(realm_flx_sync_subscription_set_state_e(SubscriptionSet::State::Pending) ==
              RLM_SYNC_SUBSCRIPTION_PENDING);
static_assert(realm_flx_sync_subscription_set_state_e(SubscriptionSet::State::Bootstrapping) ==
              RLM_SYNC_SUBSCRIPTION_BOOTSTRAPPING);
static_assert(realm_flx_sync_subscription_set_state_e(SubscriptionSet::State::AwaitingMark) ==
              RLM_SYNC_SUBSCRIPTION_AWAITING_MARK);
static_assert(realm_flx_sync_subscription_set_state_e(SubscriptionSet::State::Complete) ==
              RLM_SYNC_SUBSCRIPTION_COMPLETE);
static_assert(realm_flx_sync_subscription_set_state_e(SubscriptionSet::State::Error) == RLM_SYNC_SUBSCRIPTION_ERROR);
static_assert(realm_flx_sync_subscription_set_state_e(SubscriptionSet::State::Superseded) ==
              RLM_SYNC_SUBSCRIPTION_SUPERSEDED);
static_assert(realm_flx_sync_subscription_set_state_e(SubscriptionSet::State::Uncommitted) ==
              RLM_SYNC_SUBSCRIPTION_UNCOMMITTED);

static_assert(realm_sync_error_resolve_e(network::ResolveErrors::host_not_found) ==
              RLM_SYNC_ERROR_RESOLVE_HOST_NOT_FOUND);
static_assert(realm_sync_error_resolve_e(network::ResolveErrors::host_not_found_try_again) ==
              RLM_SYNC_ERROR_RESOLVE_HOST_NOT_FOUND_TRY_AGAIN);
static_assert(realm_sync_error_resolve_e(network::ResolveErrors::no_data) == RLM_SYNC_ERROR_RESOLVE_NO_DATA);
static_assert(realm_sync_error_resolve_e(network::ResolveErrors::no_recovery) == RLM_SYNC_ERROR_RESOLVE_NO_RECOVERY);
static_assert(realm_sync_error_resolve_e(network::ResolveErrors::service_not_found) ==
              RLM_SYNC_ERROR_RESOLVE_SERVICE_NOT_FOUND);
static_assert(realm_sync_error_resolve_e(network::ResolveErrors::socket_type_not_supported) ==
              RLM_SYNC_ERROR_RESOLVE_SOCKET_TYPE_NOT_SUPPORTED);

} // namespace

realm_sync_error_code_t to_capi(const std::error_code& error_code, std::string& message)
{
    auto ret = realm_sync_error_code_t();

    const std::error_category& category = error_code.category();
    if (category == realm::sync::client_error_category()) {
        ret.category = RLM_SYNC_ERROR_CATEGORY_CLIENT;
    }
    else if (category == realm::sync::protocol_error_category()) {
        if (realm::sync::is_session_level_error(realm::sync::ProtocolError(error_code.value()))) {
            ret.category = RLM_SYNC_ERROR_CATEGORY_SESSION;
        }
        else {
            ret.category = RLM_SYNC_ERROR_CATEGORY_CONNECTION;
        }
    }
    else if (category == std::system_category() || category == realm::util::error::basic_system_error_category()) {
        ret.category = RLM_SYNC_ERROR_CATEGORY_SYSTEM;
    }
    else if (category == realm::sync::network::resolve_error_category()) {
        ret.category = RLM_SYNC_ERROR_CATEGORY_RESOLVE;
    }
    else {
        ret.category = RLM_SYNC_ERROR_CATEGORY_UNKNOWN;
    }

    ret.value = error_code.value();
    message = error_code.message(); // pass the string to the caller for lifetime purposes
    ret.message = message.c_str();


    return ret;
}

void sync_error_to_error_code(const realm_sync_error_code_t& sync_error_code, std::error_code* error_code_out)
{
    if (error_code_out) {
        const realm_sync_error_category_e category = sync_error_code.category;
        if (category == RLM_SYNC_ERROR_CATEGORY_CLIENT) {
            error_code_out->assign(sync_error_code.value, realm::sync::client_error_category());
        }
        else if (category == RLM_SYNC_ERROR_CATEGORY_SESSION || category == RLM_SYNC_ERROR_CATEGORY_CONNECTION) {
            error_code_out->assign(sync_error_code.value, realm::sync::protocol_error_category());
        }
        else if (category == RLM_SYNC_ERROR_CATEGORY_SYSTEM) {
            error_code_out->assign(sync_error_code.value, std::system_category());
        }
        else if (category == RLM_SYNC_ERROR_CATEGORY_RESOLVE) {
            error_code_out->assign(sync_error_code.value, realm::sync::network::resolve_error_category());
        }
        else if (category == RLM_SYNC_ERROR_CATEGORY_UNKNOWN) {
            error_code_out->assign(sync_error_code.value, realm::util::error::basic_system_error_category());
        }
    }
}

static Query add_ordering_to_realm_query(Query realm_query, const DescriptorOrdering& ordering)
{
    auto ordering_copy = util::make_bind<DescriptorOrdering>();
    *ordering_copy = ordering;
    realm_query.set_ordering(ordering_copy);
    return realm_query;
}

RLM_API realm_sync_client_config_t* realm_sync_client_config_new(void) noexcept
{
    return new realm_sync_client_config_t;
}

RLM_API void realm_sync_client_config_set_base_file_path(realm_sync_client_config_t* config,
                                                         const char* path) noexcept
{
    config->base_file_path = path;
}

RLM_API void realm_sync_client_config_set_metadata_mode(realm_sync_client_config_t* config,
                                                        realm_sync_client_metadata_mode_e mode) noexcept
{
    config->metadata_mode = SyncClientConfig::MetadataMode(mode);
}

RLM_API void realm_sync_client_config_set_metadata_encryption_key(realm_sync_client_config_t* config,
                                                                  const uint8_t key[64]) noexcept
{
    config->custom_encryption_key = std::vector<char>(key, key + 64);
}

RLM_API void realm_sync_client_config_set_log_callback(realm_sync_client_config_t* config, realm_log_func_t callback,
                                                       realm_userdata_t userdata,
                                                       realm_free_userdata_func_t userdata_free) noexcept
{
    config->logger_factory = make_logger_factory(callback, userdata, userdata_free);
}

RLM_API void realm_sync_client_config_set_log_level(realm_sync_client_config_t* config,
                                                    realm_log_level_e level) noexcept
{
    config->log_level = realm::util::Logger::Level(level);
}

RLM_API void realm_sync_client_config_set_reconnect_mode(realm_sync_client_config_t* config,
                                                         realm_sync_client_reconnect_mode_e mode) noexcept
{
    config->reconnect_mode = ReconnectMode(mode);
}
RLM_API void realm_sync_client_config_set_multiplex_sessions(realm_sync_client_config_t* config,
                                                             bool multiplex) noexcept
{
    config->multiplex_sessions = multiplex;
}

RLM_API void realm_sync_client_config_set_user_agent_binding_info(realm_sync_client_config_t* config,
                                                                  const char* info) noexcept
{
    config->user_agent_binding_info = info;
}

RLM_API void realm_sync_client_config_set_user_agent_application_info(realm_sync_client_config_t* config,
                                                                      const char* info) noexcept
{
    config->user_agent_application_info = info;
}

RLM_API void realm_sync_client_config_set_connect_timeout(realm_sync_client_config_t* config,
                                                          uint64_t timeout) noexcept
{
    config->timeouts.connect_timeout = timeout;
}

RLM_API void realm_sync_client_config_set_connection_linger_time(realm_sync_client_config_t* config,
                                                                 uint64_t time) noexcept
{
    config->timeouts.connection_linger_time = time;
}

RLM_API void realm_sync_client_config_set_ping_keepalive_period(realm_sync_client_config_t* config,
                                                                uint64_t period) noexcept
{
    config->timeouts.ping_keepalive_period = period;
}

RLM_API void realm_sync_client_config_set_pong_keepalive_timeout(realm_sync_client_config_t* config,
                                                                 uint64_t timeout) noexcept
{
    config->timeouts.pong_keepalive_timeout = timeout;
}

RLM_API void realm_sync_client_config_set_fast_reconnect_limit(realm_sync_client_config_t* config,
                                                               uint64_t limit) noexcept
{
    config->timeouts.fast_reconnect_limit = limit;
}

RLM_API void realm_config_set_sync_config(realm_config_t* config, realm_sync_config_t* sync_config)
{
    config->sync_config = std::make_shared<SyncConfig>(*sync_config);
}

RLM_API realm_sync_config_t* realm_sync_config_new(const realm_user_t* user, const char* partition_value) noexcept
{
    return new realm_sync_config_t(*user, partition_value);
}

RLM_API realm_sync_config_t* realm_flx_sync_config_new(const realm_user_t* user) noexcept
{
    return new realm_sync_config(*user, realm::SyncConfig::FLXSyncEnabled{});
}

RLM_API void realm_sync_config_set_session_stop_policy(realm_sync_config_t* config,
                                                       realm_sync_session_stop_policy_e policy) noexcept
{
    config->stop_policy = SyncSessionStopPolicy(policy);
}

RLM_API void realm_sync_config_set_error_handler(realm_sync_config_t* config, realm_sync_error_handler_func_t handler,
                                                 realm_userdata_t userdata,
                                                 realm_free_userdata_func_t userdata_free) noexcept
{
    auto cb = [handler, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](
                  std::shared_ptr<SyncSession> session, SyncError error) {
        auto c_error = realm_sync_error_t();

        std::string error_code_message;
        c_error.error_code = to_capi(error.error_code, error_code_message);
        c_error.detailed_message = error.message.c_str();
        c_error.is_fatal = error.is_fatal;
        c_error.is_unrecognized_by_client = error.is_unrecognized_by_client;
        c_error.is_client_reset_requested = error.is_client_reset_requested();
        c_error.server_requests_action = static_cast<realm_sync_error_action_e>(error.server_requests_action);
        c_error.c_original_file_path_key = error.c_original_file_path_key;
        c_error.c_recovery_file_path_key = error.c_recovery_file_path_key;

        std::vector<realm_sync_error_user_info_t> c_user_info;
        c_user_info.reserve(error.user_info.size());
        for (auto& info : error.user_info) {
            c_user_info.push_back({info.first.c_str(), info.second.c_str()});
        }

        c_error.user_info_map = c_user_info.data();
        c_error.user_info_length = c_user_info.size();

        std::vector<realm_sync_error_compensating_write_info_t> c_compensating_writes;
        for (const auto& compensating_write : error.compensating_writes_info) {
            c_compensating_writes.push_back({compensating_write.reason.c_str(),
                                             compensating_write.object_name.c_str(),
                                             to_capi(compensating_write.primary_key)});
        }
        c_error.compensating_writes = c_compensating_writes.data();
        c_error.compensating_writes_length = c_compensating_writes.size();

        realm_sync_session_t c_session(session);
        handler(userdata.get(), &c_session, std::move(c_error));
    };
    config->error_handler = std::move(cb);
}

RLM_API void realm_sync_config_set_client_validate_ssl(realm_sync_config_t* config, bool validate) noexcept
{
    config->client_validate_ssl = validate;
}

RLM_API void realm_sync_config_set_ssl_trust_certificate_path(realm_sync_config_t* config, const char* path) noexcept
{
    config->ssl_trust_certificate_path = std::string(path);
}

RLM_API void realm_sync_config_set_ssl_verify_callback(realm_sync_config_t* config,
                                                       realm_sync_ssl_verify_func_t callback,
                                                       realm_userdata_t userdata,
                                                       realm_free_userdata_func_t userdata_free) noexcept
{
    auto cb = [callback, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](
                  const std::string& server_address, SyncConfig::ProxyConfig::port_type server_port,
                  const char* pem_data, size_t pem_size, int preverify_ok, int depth) {
        return callback(userdata.get(), server_address.c_str(), server_port, pem_data, pem_size, preverify_ok, depth);
    };

    config->ssl_verify_callback = std::move(cb);
}

RLM_API void realm_sync_config_set_cancel_waits_on_nonfatal_error(realm_sync_config_t* config, bool cancel) noexcept
{
    config->cancel_waits_on_nonfatal_error = cancel;
}

RLM_API void realm_sync_config_set_authorization_header_name(realm_sync_config_t* config, const char* name) noexcept
{
    config->authorization_header_name = std::string(name);
}

RLM_API void realm_sync_config_set_custom_http_header(realm_sync_config_t* config, const char* name,
                                                      const char* value) noexcept
{
    config->custom_http_headers[name] = value;
}

RLM_API void realm_sync_config_set_recovery_directory_path(realm_sync_config_t* config, const char* path) noexcept
{
    config->recovery_directory = std::string(path);
}

RLM_API void realm_sync_config_set_resync_mode(realm_sync_config_t* config,
                                               realm_sync_session_resync_mode_e mode) noexcept
{
    config->client_resync_mode = ClientResyncMode(mode);
}

RLM_API realm_object_id_t realm_sync_subscription_id(const realm_flx_sync_subscription_t* subscription) noexcept
{
    REALM_ASSERT(subscription != nullptr);
    return to_capi(subscription->id);
}

RLM_API realm_string_t realm_sync_subscription_name(const realm_flx_sync_subscription_t* subscription) noexcept
{
    REALM_ASSERT(subscription != nullptr);
    return to_capi(subscription->name);
}

RLM_API realm_string_t
realm_sync_subscription_object_class_name(const realm_flx_sync_subscription_t* subscription) noexcept
{
    REALM_ASSERT(subscription != nullptr);
    return to_capi(subscription->object_class_name);
}

RLM_API realm_string_t
realm_sync_subscription_query_string(const realm_flx_sync_subscription_t* subscription) noexcept
{
    REALM_ASSERT(subscription != nullptr);
    return to_capi(subscription->query_string);
}

RLM_API realm_timestamp_t
realm_sync_subscription_created_at(const realm_flx_sync_subscription_t* subscription) noexcept
{
    REALM_ASSERT(subscription != nullptr);
    return to_capi(subscription->created_at);
}

RLM_API realm_timestamp_t
realm_sync_subscription_updated_at(const realm_flx_sync_subscription_t* subscription) noexcept
{
    REALM_ASSERT(subscription != nullptr);
    return to_capi(subscription->updated_at);
}

RLM_API void realm_sync_config_set_before_client_reset_handler(realm_sync_config_t* config,
                                                               realm_sync_before_client_reset_func_t callback,
                                                               realm_userdata_t userdata,
                                                               realm_free_userdata_func_t userdata_free) noexcept
{
    auto cb = [callback, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](SharedRealm before_realm) {
        realm_t r1{before_realm};
        if (!callback(userdata.get(), &r1)) {
            throw CallbackFailed();
        }
    };
    config->notify_before_client_reset = std::move(cb);
}

RLM_API void realm_sync_config_set_after_client_reset_handler(realm_sync_config_t* config,
                                                              realm_sync_after_client_reset_func_t callback,
                                                              realm_userdata_t userdata,
                                                              realm_free_userdata_func_t userdata_free) noexcept
{
    auto cb = [callback, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](
                  SharedRealm before_realm, ThreadSafeReference after_realm, bool did_recover) {
        realm_t r1{before_realm};
        auto tsr = realm_t::thread_safe_reference(std::move(after_realm));
        if (!callback(userdata.get(), &r1, &tsr, did_recover)) {
            throw CallbackFailed();
        }
    };
    config->notify_after_client_reset = std::move(cb);
}

RLM_API realm_flx_sync_subscription_set_t* realm_sync_get_latest_subscription_set(const realm_t* realm)
{
    REALM_ASSERT(realm != nullptr);
    return wrap_err([&]() {
        return new realm_flx_sync_subscription_set_t((*realm)->get_latest_subscription_set());
    });
}

RLM_API realm_flx_sync_subscription_set_t* realm_sync_get_active_subscription_set(const realm_t* realm)
{
    REALM_ASSERT(realm != nullptr);
    return wrap_err([&]() {
        return new realm_flx_sync_subscription_set_t((*realm)->get_active_subscription_set());
    });
}

RLM_API realm_flx_sync_subscription_set_state_e
realm_sync_on_subscription_set_state_change_wait(const realm_flx_sync_subscription_set_t* subscription_set,
                                                 realm_flx_sync_subscription_set_state_e notify_when) noexcept
{
    REALM_ASSERT(subscription_set != nullptr);
    SubscriptionSet::State state =
        subscription_set->get_state_change_notification(static_cast<SubscriptionSet::State>(notify_when)).get();
    return static_cast<realm_flx_sync_subscription_set_state_e>(state);
}

RLM_API bool
realm_sync_on_subscription_set_state_change_async(const realm_flx_sync_subscription_set_t* subscription_set,
                                                  realm_flx_sync_subscription_set_state_e notify_when,
                                                  realm_sync_on_subscription_state_changed_t callback,
                                                  realm_userdata_t userdata, realm_free_userdata_func_t userdata_free)
{
    REALM_ASSERT(subscription_set != nullptr && callback != nullptr);
    return wrap_err([&]() {
        auto future_state =
            subscription_set->get_state_change_notification(static_cast<SubscriptionSet::State>(notify_when));
        std::move(future_state)
            .get_async([callback, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](
                           const StatusWith<SubscriptionSet::State>& state) -> void {
                if (state.is_ok())
                    callback(userdata.get(), static_cast<realm_flx_sync_subscription_set_state_e>(state.get_value()));
                else
                    callback(userdata.get(), realm_flx_sync_subscription_set_state_e::RLM_SYNC_SUBSCRIPTION_ERROR);
            });
        return true;
    });
}

RLM_API int64_t
realm_sync_subscription_set_version(const realm_flx_sync_subscription_set_t* subscription_set) noexcept
{
    REALM_ASSERT(subscription_set != nullptr);
    return subscription_set->version();
}

RLM_API realm_flx_sync_subscription_set_state_e
realm_sync_subscription_set_state(const realm_flx_sync_subscription_set_t* subscription_set) noexcept
{
    REALM_ASSERT(subscription_set != nullptr);
    return static_cast<realm_flx_sync_subscription_set_state_e>(subscription_set->state());
}

RLM_API const char*
realm_sync_subscription_set_error_str(const realm_flx_sync_subscription_set_t* subscription_set) noexcept
{
    REALM_ASSERT(subscription_set != nullptr);
    return subscription_set->error_str().data();
}

RLM_API size_t realm_sync_subscription_set_size(const realm_flx_sync_subscription_set_t* subscription_set) noexcept
{
    REALM_ASSERT(subscription_set != nullptr);
    return subscription_set->size();
}

RLM_API realm_flx_sync_subscription_t*
realm_sync_find_subscription_by_name(const realm_flx_sync_subscription_set_t* subscription_set,
                                     const char* name) noexcept
{
    REALM_ASSERT(subscription_set != nullptr);
    auto ptr = subscription_set->find(name);
    if (!ptr)
        return nullptr;
    return new realm_flx_sync_subscription_t(*ptr);
}

RLM_API realm_flx_sync_subscription_t*
realm_sync_find_subscription_by_results(const realm_flx_sync_subscription_set_t* subscription_set,
                                        realm_results_t* results) noexcept
{
    REALM_ASSERT(subscription_set != nullptr);
    auto realm_query = add_ordering_to_realm_query(results->get_query(), results->get_ordering());
    auto ptr = subscription_set->find(realm_query);
    if (!ptr)
        return nullptr;
    return new realm_flx_sync_subscription_t{*ptr};
}

RLM_API realm_flx_sync_subscription_t*
realm_sync_subscription_at(const realm_flx_sync_subscription_set_t* subscription_set, size_t index)
{
    REALM_ASSERT(subscription_set != nullptr && index < subscription_set->size());
    try {
        return new realm_flx_sync_subscription_t{subscription_set->at(index)};
    }
    catch (...) {
        return nullptr;
    }
}

RLM_API realm_flx_sync_subscription_t*
realm_sync_find_subscription_by_query(const realm_flx_sync_subscription_set_t* subscription_set,
                                      realm_query_t* query) noexcept
{
    REALM_ASSERT(subscription_set != nullptr);
    auto realm_query = add_ordering_to_realm_query(query->get_query(), query->get_ordering());
    auto ptr = subscription_set->find(realm_query);
    if (!ptr)
        return nullptr;
    return new realm_flx_sync_subscription_t(*ptr);
}

RLM_API bool realm_sync_subscription_set_refresh(realm_flx_sync_subscription_set_t* subscription_set)
{
    REALM_ASSERT(subscription_set != nullptr);
    return wrap_err([&]() {
        subscription_set->refresh();
        return true;
    });
}

RLM_API realm_flx_sync_mutable_subscription_set_t*
realm_sync_make_subscription_set_mutable(realm_flx_sync_subscription_set_t* subscription_set)
{
    REALM_ASSERT(subscription_set != nullptr);
    return wrap_err([&]() {
        return new realm_flx_sync_mutable_subscription_set_t{subscription_set->make_mutable_copy()};
    });
}

RLM_API bool realm_sync_subscription_set_clear(realm_flx_sync_mutable_subscription_set_t* subscription_set)
{
    REALM_ASSERT(subscription_set != nullptr);
    return wrap_err([&]() {
        subscription_set->clear();
        return true;
    });
}

RLM_API bool
realm_sync_subscription_set_insert_or_assign_results(realm_flx_sync_mutable_subscription_set_t* subscription_set,
                                                     realm_results_t* results, const char* name, size_t* index,
                                                     bool* inserted)
{
    REALM_ASSERT(subscription_set != nullptr && results != nullptr);
    return wrap_err([&]() {
        auto realm_query = add_ordering_to_realm_query(results->get_query(), results->get_ordering());
        const auto [it, successful] = name ? subscription_set->insert_or_assign(name, realm_query)
                                           : subscription_set->insert_or_assign(realm_query);
        *index = std::distance(subscription_set->begin(), it);
        *inserted = successful;
        return true;
    });
}

RLM_API bool
realm_sync_subscription_set_insert_or_assign_query(realm_flx_sync_mutable_subscription_set_t* subscription_set,
                                                   realm_query_t* query, const char* name, size_t* index,
                                                   bool* inserted)
{
    REALM_ASSERT(subscription_set != nullptr && query != nullptr);
    return wrap_err([&]() {
        auto realm_query = add_ordering_to_realm_query(query->get_query(), query->get_ordering());
        const auto [it, successful] = name ? subscription_set->insert_or_assign(name, realm_query)
                                           : subscription_set->insert_or_assign(realm_query);
        *index = std::distance(subscription_set->begin(), it);
        *inserted = successful;
        return true;
    });
}

RLM_API bool realm_sync_subscription_set_erase_by_id(realm_flx_sync_mutable_subscription_set_t* subscription_set,
                                                     const realm_object_id_t* id, bool* erased)
{
    REALM_ASSERT(subscription_set != nullptr && id != nullptr);
    *erased = false;
    return wrap_err([&] {
        auto it = std::find_if(subscription_set->begin(), subscription_set->end(), [id](const Subscription& sub) {
            return from_capi(*id) == sub.id;
        });
        if (it != subscription_set->end()) {
            subscription_set->erase(it);
            *erased = true;
        }
        return true;
    });
}

RLM_API bool realm_sync_subscription_set_erase_by_name(realm_flx_sync_mutable_subscription_set_t* subscription_set,
                                                       const char* name, bool* erased)
{
    REALM_ASSERT(subscription_set != nullptr && name != nullptr);
    *erased = false;
    return wrap_err([&]() {
        *erased = subscription_set->erase(name);
        return true;
    });
}

RLM_API bool realm_sync_subscription_set_erase_by_query(realm_flx_sync_mutable_subscription_set_t* subscription_set,
                                                        realm_query_t* query, bool* erased)
{
    REALM_ASSERT(subscription_set != nullptr && query != nullptr);
    *erased = false;
    return wrap_err([&]() {
        auto realm_query = add_ordering_to_realm_query(query->get_query(), query->get_ordering());
        *erased = subscription_set->erase(realm_query);
        return true;
    });
}

RLM_API bool realm_sync_subscription_set_erase_by_results(realm_flx_sync_mutable_subscription_set_t* subscription_set,
                                                          realm_results_t* results, bool* erased)
{
    REALM_ASSERT(subscription_set != nullptr && results != nullptr);
    *erased = false;
    return wrap_err([&]() {
        auto realm_query = add_ordering_to_realm_query(results->get_query(), results->get_ordering());
        *erased = subscription_set->erase(realm_query);
        return true;
    });
}

RLM_API realm_flx_sync_subscription_set_t*
realm_sync_subscription_set_commit(realm_flx_sync_mutable_subscription_set_t* subscription_set)
{
    REALM_ASSERT(subscription_set != nullptr);
    return wrap_err([&]() {
        return new realm_flx_sync_subscription_set_t{std::move(*subscription_set).commit()};
    });
}

RLM_API realm_async_open_task_t* realm_open_synchronized(realm_config_t* config) noexcept
{
    return wrap_err([config] {
        return new realm_async_open_task_t(Realm::get_synchronized_realm(*config));
    });
}

RLM_API void realm_async_open_task_start(realm_async_open_task_t* task, realm_async_open_task_completion_func_t done,
                                         realm_userdata_t userdata, realm_free_userdata_func_t userdata_free) noexcept
{
    auto cb = [done, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](ThreadSafeReference realm,
                                                                                       std::exception_ptr error) {
        if (error) {
            realm_async_error_t c_error(std::move(error));
            done(userdata.get(), nullptr, &c_error);
        }
        else {
            auto tsr = new realm_t::thread_safe_reference(std::move(realm));
            done(userdata.get(), tsr, nullptr);
        }
    };
    (*task)->start(std::move(cb));
}

RLM_API void realm_async_open_task_cancel(realm_async_open_task_t* task) noexcept
{
    (*task)->cancel();
}

RLM_API realm_async_open_task_progress_notification_token_t*
realm_async_open_task_register_download_progress_notifier(realm_async_open_task_t* task,
                                                          realm_sync_progress_func_t notifier,
                                                          realm_userdata_t userdata,
                                                          realm_free_userdata_func_t userdata_free) noexcept
{
    auto cb = [notifier, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](uint64_t transferred,
                                                                                           uint64_t transferrable) {
        notifier(userdata.get(), transferred, transferrable);
    };
    auto token = (*task)->register_download_progress_notifier(std::move(cb));
    return new realm_async_open_task_progress_notification_token_t{(*task), token};
}

RLM_API realm_sync_session_t* realm_sync_session_get(const realm_t* realm) noexcept
{
    if (auto session = (*realm)->sync_session()) {
        return new realm_sync_session_t(std::move(session));
    }

    return nullptr;
}

RLM_API realm_sync_session_state_e realm_sync_session_get_state(const realm_sync_session_t* session) noexcept
{
    return realm_sync_session_state_e((*session)->state());
}

RLM_API realm_sync_connection_state_e
realm_sync_session_get_connection_state(const realm_sync_session_t* session) noexcept
{
    return realm_sync_connection_state_e((*session)->connection_state());
}

RLM_API realm_user_t* realm_sync_session_get_user(const realm_sync_session_t* session) noexcept
{
    return new realm_user_t((*session)->user());
}

RLM_API const char* realm_sync_session_get_partition_value(const realm_sync_session_t* session) noexcept
{
    return (*session)->config().partition_value.c_str();
}

RLM_API const char* realm_sync_session_get_file_path(const realm_sync_session_t* session) noexcept
{
    return (*session)->path().c_str();
}

RLM_API void realm_sync_session_pause(realm_sync_session_t* session) noexcept
{
    (*session)->pause();
}

RLM_API void realm_sync_session_resume(realm_sync_session_t* session) noexcept
{
    (*session)->resume();
}

RLM_API bool realm_sync_immediately_run_file_actions(realm_app_t* realm_app, const char* sync_path,
                                                     bool* did_run) noexcept
{
    return wrap_err([&]() {
        *did_run = (*realm_app)->sync_manager()->immediately_run_file_actions(sync_path);
        return true;
    });
}

RLM_API realm_sync_session_connection_state_notification_token_t*
realm_sync_session_register_connection_state_change_callback(realm_sync_session_t* session,
                                                             realm_sync_connection_state_changed_func_t callback,
                                                             realm_userdata_t userdata,
                                                             realm_free_userdata_func_t userdata_free) noexcept
{
    std::function<realm::SyncSession::ConnectionStateChangeCallback> cb =
        [callback, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](auto old_state, auto new_state) {
            callback(userdata.get(), realm_sync_connection_state_e(old_state),
                     realm_sync_connection_state_e(new_state));
        };
    auto token = (*session)->register_connection_change_callback(std::move(cb));
    return new realm_sync_session_connection_state_notification_token_t{(*session), token};
}

RLM_API realm_sync_session_connection_state_notification_token_t* realm_sync_session_register_progress_notifier(
    realm_sync_session_t* session, realm_sync_progress_func_t notifier, realm_sync_progress_direction_e direction,
    bool is_streaming, realm_userdata_t userdata, realm_free_userdata_func_t userdata_free) noexcept
{
    std::function<realm::SyncSession::ProgressNotifierCallback> cb =
        [notifier, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](uint64_t transferred,
                                                                                     uint64_t transferrable) {
            notifier(userdata.get(), transferred, transferrable);
        };
    auto token = (*session)->register_progress_notifier(std::move(cb), SyncSession::ProgressDirection(direction),
                                                        is_streaming);
    return new realm_sync_session_connection_state_notification_token_t{(*session), token};
}

RLM_API void realm_sync_session_wait_for_download_completion(realm_sync_session_t* session,
                                                             realm_sync_wait_for_completion_func_t done,
                                                             realm_userdata_t userdata,
                                                             realm_free_userdata_func_t userdata_free) noexcept
{
    util::UniqueFunction<void(std::error_code)> cb =
        [done, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](std::error_code e) {
            if (e) {
                std::string error_code_message;
                realm_sync_error_code_t error = to_capi(e, error_code_message);
                done(userdata.get(), &error);
            }
            else {
                done(userdata.get(), nullptr);
            }
        };
    (*session)->wait_for_download_completion(std::move(cb));
}

RLM_API void realm_sync_session_wait_for_upload_completion(realm_sync_session_t* session,
                                                           realm_sync_wait_for_completion_func_t done,
                                                           realm_userdata_t userdata,
                                                           realm_free_userdata_func_t userdata_free) noexcept
{
    util::UniqueFunction<void(std::error_code)> cb =
        [done, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](std::error_code e) {
            if (e) {
                std::string error_code_message;
                realm_sync_error_code_t error = to_capi(e, error_code_message);
                done(userdata.get(), &error);
            }
            else {
                done(userdata.get(), nullptr);
            }
        };
    (*session)->wait_for_upload_completion(std::move(cb));
}

RLM_API void realm_sync_session_handle_error_for_testing(const realm_sync_session_t* session, int error_code,
                                                         int error_category, const char* error_message, bool is_fatal)
{
    REALM_ASSERT(session);
    realm_sync_error_code_t sync_error{static_cast<realm_sync_error_category_e>(error_category), error_code,
                                       error_message};
    std::error_code err;
    sync_error_to_error_code(sync_error, &err);
    SyncSession::OnlyForTesting::handle_error(*session->get(), {err, error_message, is_fatal});
}

} // namespace realm::c_api
