////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include <realm/object-store/sync/sync_manager.hpp>

#include <realm/object-store/sync/impl/sync_client.hpp>
#include <realm/object-store/sync/impl/sync_file.hpp>
#include <realm/object-store/sync/impl/sync_metadata.hpp>
#include <realm/object-store/sync/sync_session.hpp>
#include <realm/object-store/sync/sync_user.hpp>
#include <realm/object-store/sync/app.hpp>
#include <realm/object-store/util/uuid.hpp>

#include <realm/util/sha_crypto.hpp>
#include <realm/util/hex_dump.hpp>

using namespace realm;
using namespace realm::_impl;

SyncClientTimeouts::SyncClientTimeouts()
    : connect_timeout(sync::Client::default_connect_timeout)
    , connection_linger_time(sync::Client::default_connection_linger_time)
    , ping_keepalive_period(sync::Client::default_ping_keepalive_period)
    , pong_keepalive_timeout(sync::Client::default_pong_keepalive_timeout)
    , fast_reconnect_limit(sync::Client::default_fast_reconnect_limit)
{
}

SyncManager::SyncManager() = default;

void SyncManager::configure(std::shared_ptr<app::App> app, const std::string& sync_route,
                            const SyncClientConfig& config)
{
    struct UserCreationData {
        std::string identity;
        std::string refresh_token;
        std::string access_token;
        std::string provider_type;
        std::vector<SyncUserIdentity> identities;
        SyncUser::State state;
        std::string device_id;
    };

    std::vector<UserCreationData> users_to_add;
    {
        // Locking the mutex here ensures that it is released before locking m_user_mutex
        util::CheckedLockGuard lock(m_mutex);
        m_app = app;
        m_sync_route = sync_route;
        m_config = std::move(config);
        if (m_sync_client)
            return;

        // create a new logger - if the logger_factory is updated later, a new
        // logger will be created at that time.
        do_make_logger();

        {
            util::CheckedLockGuard lock(m_file_system_mutex);

            // Set up the file manager.
            if (m_file_manager) {
                // Changing the base path for tests requires calling reset_for_testing()
                // first, and otherwise isn't supported
                REALM_ASSERT(m_file_manager->base_path() == m_config.base_file_path);
            }
            else {
                m_file_manager = std::make_unique<SyncFileManager>(m_config.base_file_path, app->config().app_id);
            }

            // Set up the metadata manager, and perform initial loading/purging work.
            if (m_metadata_manager || m_config.metadata_mode == MetadataMode::NoMetadata) {
                return;
            }

            bool encrypt = m_config.metadata_mode == MetadataMode::Encryption;
            m_metadata_manager = std::make_unique<SyncMetadataManager>(m_file_manager->metadata_path(), encrypt,
                                                                       m_config.custom_encryption_key);

            REALM_ASSERT(m_metadata_manager);

            // Perform our "on next startup" actions such as deleting Realm files
            // which we couldn't delete immediately due to them being in use
            std::vector<SyncFileActionMetadata> completed_actions;
            SyncFileActionMetadataResults file_actions = m_metadata_manager->all_pending_actions();
            for (size_t i = 0; i < file_actions.size(); i++) {
                auto file_action = file_actions.get(i);
                if (run_file_action(file_action)) {
                    completed_actions.emplace_back(std::move(file_action));
                }
            }
            for (auto& action : completed_actions) {
                action.remove();
            }

            // Load persisted users into the users map.
            SyncUserMetadataResults users = m_metadata_manager->all_unmarked_users();
            for (size_t i = 0; i < users.size(); i++) {
                auto user_data = users.get(i);
                auto refresh_token = user_data.refresh_token();
                auto access_token = user_data.access_token();
                auto device_id = user_data.device_id();
                if (!refresh_token.empty() && !access_token.empty()) {
                    users_to_add.push_back(UserCreationData{user_data.identity(), std::move(refresh_token),
                                                            std::move(access_token), user_data.provider_type(),
                                                            user_data.identities(), user_data.state(), device_id});
                }
            }

            // Delete any users marked for death.
            std::vector<SyncUserMetadata> dead_users;
            SyncUserMetadataResults users_to_remove = m_metadata_manager->all_users_marked_for_removal();
            dead_users.reserve(users_to_remove.size());
            for (size_t i = 0; i < users_to_remove.size(); i++) {
                auto user = users_to_remove.get(i);
                // FIXME: delete user data in a different way? (This deletes a logged-out user's data as soon as the
                // app launches again, which might not be how some apps want to treat their data.)
                try {
                    m_file_manager->remove_user_realms(user.identity(), user.realm_file_paths());
                    dead_users.emplace_back(std::move(user));
                }
                catch (util::File::AccessError const&) {
                    continue;
                }
            }
            for (auto& user : dead_users) {
                user.remove();
            }
        }
    }
    {
        util::CheckedLockGuard lock(m_user_mutex);
        for (auto& user_data : users_to_add) {
            auto& identity = user_data.identity;
            auto& provider_type = user_data.provider_type;
            auto user =
                std::make_shared<SyncUser>(user_data.refresh_token, identity, provider_type, user_data.access_token,
                                           user_data.state, user_data.device_id, this);
            user->update_identities(user_data.identities);
            m_users.emplace_back(std::move(user));
        }
    }
}

bool SyncManager::immediately_run_file_actions(const std::string& realm_path)
{
    util::CheckedLockGuard lock(m_file_system_mutex);
    if (!m_metadata_manager) {
        return false;
    }
    if (auto metadata = m_metadata_manager->get_file_action_metadata(realm_path)) {
        if (run_file_action(*metadata)) {
            metadata->remove();
            return true;
        }
    }
    return false;
}

// Perform a file action. Returns whether or not the file action can be removed.
bool SyncManager::run_file_action(SyncFileActionMetadata& md)
{
    switch (md.action()) {
        case SyncFileActionMetadata::Action::DeleteRealm:
            // Delete all the files for the given Realm.
            m_file_manager->remove_realm(md.original_name());
            return true;
        case SyncFileActionMetadata::Action::BackUpThenDeleteRealm:
            // Copy the primary Realm file to the recovery dir, and then delete the Realm.
            auto new_name = md.new_name();
            auto original_name = md.original_name();
            if (!util::File::exists(original_name)) {
                // The Realm file doesn't exist anymore.
                return true;
            }
            if (new_name && !util::File::exists(*new_name) &&
                m_file_manager->copy_realm_file(original_name, *new_name)) {
                // We successfully copied the Realm file to the recovery directory.
                bool did_remove = m_file_manager->remove_realm(original_name);
                // if the copy succeeded but not the delete, then running BackupThenDelete
                // a second time would fail, so change this action to just delete the originall file.
                if (did_remove) {
                    return true;
                }
                md.set_action(SyncFileActionMetadata::Action::DeleteRealm);
                return false;
            }
            return false;
    }
    return false;
}

void SyncManager::reset_for_testing()
{
    {
        util::CheckedLockGuard lock(m_file_system_mutex);
        m_metadata_manager = nullptr;
    }

    {
        // Destroy all the users.
        util::CheckedLockGuard lock(m_user_mutex);
        for (auto& user : m_users) {
            user->detach_from_sync_manager();
        }
        m_users.clear();
        m_current_user = nullptr;
    }

    {
        util::CheckedLockGuard lock(m_mutex);
        // Stop the client. This will abort any uploads that inactive sessions are waiting for.
        if (m_sync_client)
            m_sync_client->stop();
    }

    {
        util::CheckedLockGuard lock(m_session_mutex);
        // Callers of `SyncManager::reset_for_testing` should ensure there are no existing sessions
        // prior to calling `reset_for_testing`.
        bool no_sessions = !do_has_existing_sessions();
        REALM_ASSERT_RELEASE(no_sessions);

        // Destroy any inactive sessions.
        // FIXME: We shouldn't have any inactive sessions at this point! Sessions are expected to
        // remain inactive until their final upload completes, at which point they are unregistered
        // and destroyed. Our call to `sync::Client::stop` above aborts all uploads, so all sessions
        // should have already been destroyed.
        m_sessions.clear();
    }

    {
        util::CheckedLockGuard lock(m_mutex);
        // Destroy the client now that we have no remaining sessions.
        m_sync_client = nullptr;

        // Reset even more state.
        m_config = {};
        m_logger_ptr.reset();
        m_sync_route = "";
    }

    {
        util::CheckedLockGuard lock(m_file_system_mutex);
        if (m_file_manager)
            util::try_remove_dir_recursive(m_file_manager->base_path());
        m_file_manager = nullptr;
    }
}

void SyncManager::set_log_level(util::Logger::Level level) noexcept
{
    util::CheckedLockGuard lock(m_mutex);
    m_config.log_level = level;
    // Update the level threshold in the already created logger
    if (m_logger_ptr) {
        m_logger_ptr->set_level_threshold(level);
    }
}

void SyncManager::set_logger_factory(SyncClientConfig::LoggerFactory factory)
{
    util::CheckedLockGuard lock(m_mutex);
    m_config.logger_factory = std::move(factory);

    if (m_sync_client)
        throw std::logic_error("Cannot set the logger_factory after creating the sync client");

    // Create a new logger using the new factory
    do_make_logger();
}

void SyncManager::do_make_logger()
{
    if (m_config.logger_factory) {
        m_logger_ptr = m_config.logger_factory(m_config.log_level);
    }
    else {
        // recreate the logger as a StderrLogger, even if it was created before...
        m_logger_ptr = std::make_shared<util::StderrLogger>(m_config.log_level);
    }
}

const std::shared_ptr<util::Logger>& SyncManager::get_logger() const
{
    util::CheckedLockGuard lock(m_mutex);
    return m_logger_ptr;
}

void SyncManager::set_user_agent(std::string user_agent)
{
    util::CheckedLockGuard lock(m_mutex);
    m_config.user_agent_application_info = std::move(user_agent);
}

void SyncManager::set_timeouts(SyncClientTimeouts timeouts)
{
    util::CheckedLockGuard lock(m_mutex);
    m_config.timeouts = timeouts;
}

void SyncManager::reconnect() const
{
    util::CheckedLockGuard lock(m_session_mutex);
    for (auto& it : m_sessions) {
        it.second->handle_reconnect();
    }
}

util::Logger::Level SyncManager::log_level() const noexcept
{
    util::CheckedLockGuard lock(m_mutex);
    return m_config.log_level;
}

bool SyncManager::perform_metadata_update(util::FunctionRef<void(SyncMetadataManager&)> update_function) const
{
    util::CheckedLockGuard lock(m_file_system_mutex);
    if (!m_metadata_manager) {
        return false;
    }
    update_function(*m_metadata_manager);
    return true;
}

std::shared_ptr<SyncUser> SyncManager::get_user(const std::string& user_id, std::string refresh_token,
                                                std::string access_token, const std::string provider_type,
                                                std::string device_id)
{
    util::CheckedLockGuard lock(m_user_mutex);
    auto it = std::find_if(m_users.begin(), m_users.end(), [user_id, provider_type](const auto& user) {
        return user->identity() == user_id && user->provider_type() == provider_type &&
               user->state() != SyncUser::State::Removed;
    });
    if (it == m_users.end()) {
        // No existing user.
        auto new_user =
            std::make_shared<SyncUser>(std::move(refresh_token), user_id, provider_type, std::move(access_token),
                                       SyncUser::State::LoggedIn, device_id, this);
        m_users.emplace(m_users.begin(), new_user);
        {
            util::CheckedLockGuard lock(m_file_system_mutex);
            // m_current_user is normally set very indirectly via the metadata manger
            if (!m_metadata_manager)
                m_current_user = new_user;
        }
        return new_user;
    }
    else { // LoggedOut => LoggedIn
        auto user = *it;
        REALM_ASSERT(user->state() != SyncUser::State::Removed);
        user->update_state_and_tokens(SyncUser::State::LoggedIn, std::move(access_token), std::move(refresh_token));
        return user;
    }
}

std::vector<std::shared_ptr<SyncUser>> SyncManager::all_users()
{
    util::CheckedLockGuard lock(m_user_mutex);
    m_users.erase(std::remove_if(m_users.begin(), m_users.end(),
                                 [](auto& user) {
                                     bool should_remove = (user->state() == SyncUser::State::Removed);
                                     if (should_remove) {
                                         user->detach_from_sync_manager();
                                     }
                                     return should_remove;
                                 }),
                  m_users.end());
    return m_users;
}

std::shared_ptr<SyncUser> SyncManager::get_user_for_identity(std::string const& identity) const noexcept
{
    auto is_active_user = [identity](auto& el) {
        return el->identity() == identity;
    };
    auto it = std::find_if(m_users.begin(), m_users.end(), is_active_user);
    return it == m_users.end() ? nullptr : *it;
}

std::shared_ptr<SyncUser> SyncManager::get_current_user() const
{
    util::CheckedLockGuard lock(m_user_mutex);

    if (m_current_user)
        return m_current_user;
    util::CheckedLockGuard fs_lock(m_file_system_mutex);
    if (!m_metadata_manager)
        return nullptr;

    auto cur_user_ident = m_metadata_manager->get_current_user_identity();
    return cur_user_ident ? get_user_for_identity(*cur_user_ident) : nullptr;
}

void SyncManager::log_out_user(const std::string& user_id)
{
    util::CheckedLockGuard lock(m_user_mutex);

    // Move this user to the end of the vector
    if (m_users.size() > 1) {
        auto it = std::find_if(m_users.begin(), m_users.end(), [user_id](const auto& user) {
            return user->identity() == user_id;
        });

        if (it != m_users.end())
            std::rotate(it, it + 1, m_users.end());
    }

    util::CheckedLockGuard fs_lock(m_file_system_mutex);
    bool was_active = (m_current_user && m_current_user->identity() == user_id) ||
                      (m_metadata_manager && m_metadata_manager->get_current_user_identity() == user_id);
    if (!was_active)
        return;

    // Set the current active user to the next logged in user, or null if none
    for (auto& user : m_users) {
        if (user->state() == SyncUser::State::LoggedIn) {
            if (m_metadata_manager)
                m_metadata_manager->set_current_user_identity(user->identity());
            m_current_user = user;
            return;
        }
    }

    if (m_metadata_manager)
        m_metadata_manager->set_current_user_identity("");
    m_current_user = nullptr;
}

void SyncManager::set_current_user(const std::string& user_id)
{
    util::CheckedLockGuard lock(m_user_mutex);

    m_current_user = get_user_for_identity(user_id);
    util::CheckedLockGuard fs_lock(m_file_system_mutex);
    if (m_metadata_manager)
        m_metadata_manager->set_current_user_identity(user_id);
}

void SyncManager::remove_user(const std::string& user_id)
{
    util::CheckedLockGuard lock(m_user_mutex);
    auto user = get_user_for_identity(user_id);
    if (!user)
        return;
    user->set_state(SyncUser::State::Removed);

    util::CheckedLockGuard fs_lock(m_file_system_mutex);
    if (!m_metadata_manager)
        return;

    for (size_t i = 0; i < m_metadata_manager->all_unmarked_users().size(); i++) {
        auto metadata = m_metadata_manager->all_unmarked_users().get(i);
        if (user->identity() == metadata.identity()) {
            metadata.mark_for_removal();
        }
    }
}

void SyncManager::delete_user(const std::string& user_id)
{
    util::CheckedLockGuard lock(m_user_mutex);
    // Avoid itterating over m_users twice by not calling `get_user_for_identity`.
    auto it = std::find_if(m_users.begin(), m_users.end(), [&user_id](auto& user) {
        return user->identity() == user_id;
    });
    auto user = it == m_users.end() ? nullptr : *it;

    if (!user)
        return;

    // Deletion should happen immediately, not when we do the cleanup
    // task on next launch.
    m_users.erase(it);
    user->detach_from_sync_manager();

    if (m_current_user && m_current_user->identity() == user->identity())
        m_current_user = nullptr;

    util::CheckedLockGuard fs_lock(m_file_system_mutex);
    if (!m_metadata_manager)
        return;

    auto users = m_metadata_manager->all_unmarked_users();
    for (size_t i = 0; i < users.size(); i++) {
        auto metadata = users.get(i);
        if (user->identity() == metadata.identity()) {
            m_file_manager->remove_user_realms(metadata.identity(), metadata.realm_file_paths());
            metadata.remove();
            break;
        }
    }
}

SyncManager::~SyncManager() NO_THREAD_SAFETY_ANALYSIS
{
    // Grab the current sessions under a lock so we can shut them down. We have to
    // release the lock before calling them as shutdown_and_wait() will call
    // back into us.
    decltype(m_sessions) current_sessions;
    {
        util::CheckedLockGuard lk(m_session_mutex);
        m_sessions.swap(current_sessions);
    }

    for (auto& [_, session] : current_sessions) {
        session->detach_from_sync_manager();
    }

    {
        util::CheckedLockGuard lk(m_user_mutex);
        for (auto& user : m_users) {
            user->detach_from_sync_manager();
        }
    }

    {
        util::CheckedLockGuard lk(m_mutex);
        // Stop the client. This will abort any uploads that inactive sessions are waiting for.
        if (m_sync_client)
            m_sync_client->stop();
    }
}

std::shared_ptr<SyncUser> SyncManager::get_existing_logged_in_user(const std::string& user_id) const
{
    util::CheckedLockGuard lock(m_user_mutex);
    auto user = get_user_for_identity(user_id);
    return user && user->state() == SyncUser::State::LoggedIn ? user : nullptr;
}

struct UnsupportedBsonPartition : public std::logic_error {
    UnsupportedBsonPartition(std::string msg)
        : std::logic_error(msg)
    {
    }
};

static std::string string_from_partition(const std::string& partition)
{
    bson::Bson partition_value = bson::parse(partition);
    switch (partition_value.type()) {
        case bson::Bson::Type::Int32:
            return util::format("i_%1", static_cast<int32_t>(partition_value));
        case bson::Bson::Type::Int64:
            return util::format("l_%1", static_cast<int64_t>(partition_value));
        case bson::Bson::Type::String:
            return util::format("s_%1", static_cast<std::string>(partition_value));
        case bson::Bson::Type::ObjectId:
            return util::format("o_%1", static_cast<ObjectId>(partition_value).to_string());
        case bson::Bson::Type::Uuid:
            return util::format("u_%1", static_cast<UUID>(partition_value).to_string());
        case bson::Bson::Type::Null:
            return "null";
        default:
            throw UnsupportedBsonPartition(util::format("Unsupported partition key value: '%1'. Only int, string "
                                                        "UUID and ObjectId types are currently supported.",
                                                        partition_value.to_string()));
    }
}

std::string SyncManager::path_for_realm(const SyncConfig& config, util::Optional<std::string> custom_file_name) const
{
    auto user = config.user;
    REALM_ASSERT(user);
    std::string path;
    {
        util::CheckedLockGuard lock(m_file_system_mutex);
        REALM_ASSERT(m_file_manager);

        // Attempt to make a nicer filename which will ease debugging when
        // locating files in the filesystem.
        auto file_name = [&]() -> std::string {
            if (custom_file_name) {
                return *custom_file_name;
            }
            if (config.flx_sync_requested) {
                REALM_ASSERT_DEBUG(config.partition_value.empty());
                return "flx_sync_default";
            }
            return string_from_partition(config.partition_value);
        }();
        path = m_file_manager->realm_file_path(user->identity(), user->local_identity(), file_name,
                                               config.partition_value);
    }
    // Report the use of a Realm for this user, so the metadata can track it for clean up.
    perform_metadata_update([&](const auto& manager) {
        auto metadata = manager.get_or_make_user_metadata(user->identity(), user->provider_type());
        metadata->add_realm_file_path(path);
    });
    return path;
}

std::string SyncManager::recovery_directory_path(util::Optional<std::string> const& custom_dir_name) const
{
    util::CheckedLockGuard lock(m_file_system_mutex);
    REALM_ASSERT(m_file_manager);
    return m_file_manager->recovery_directory_path(custom_dir_name);
}

std::vector<std::shared_ptr<SyncSession>> SyncManager::get_all_sessions() const
{
    util::CheckedLockGuard lock(m_session_mutex);
    std::vector<std::shared_ptr<SyncSession>> sessions;
    for (auto& [_, session] : m_sessions) {
        if (auto external_reference = session->existing_external_reference())
            sessions.push_back(std::move(external_reference));
    }
    return sessions;
}

std::shared_ptr<SyncSession> SyncManager::get_existing_active_session(const std::string& path) const
{
    util::CheckedLockGuard lock(m_session_mutex);
    if (auto session = get_existing_session_locked(path)) {
        if (auto external_reference = session->existing_external_reference())
            return external_reference;
    }
    return nullptr;
}

std::shared_ptr<SyncSession> SyncManager::get_existing_session_locked(const std::string& path) const
{
    auto it = m_sessions.find(path);
    return it == m_sessions.end() ? nullptr : it->second;
}

std::shared_ptr<SyncSession> SyncManager::get_existing_session(const std::string& path) const
{
    util::CheckedLockGuard lock(m_session_mutex);
    if (auto session = get_existing_session_locked(path))
        return session->external_reference();

    return nullptr;
}

std::shared_ptr<SyncSession> SyncManager::get_session(std::shared_ptr<DB> db, const RealmConfig& config)
{
    auto& client = get_sync_client(); // Throws
    auto path = db->get_path();
    REALM_ASSERT_EX(path == config.path, path, config.path);
    REALM_ASSERT(config.sync_config);

    util::CheckedUniqueLock lock(m_session_mutex);
    if (auto session = get_existing_session_locked(path)) {
        config.sync_config->user->register_session(session);
        return session->external_reference();
    }

    auto shared_session = SyncSession::create(client, std::move(db), config, this);
    m_sessions[path] = shared_session;

    // Create the external reference immediately to ensure that the session will become
    // inactive if an exception is thrown in the following code.
    auto external_reference = shared_session->external_reference();
    // unlocking m_session_mutex here prevents a deadlock for synchronous network
    // transports such as the unit test suite, in the case where the log in request is
    // denied by the server: Active -> WaitingForAccessToken -> handle_refresh(401
    // error) -> user.log_out() -> unregister_session (locks m_session_mutex again)
    lock.unlock();
    config.sync_config->user->register_session(std::move(shared_session));

    return external_reference;
}

bool SyncManager::has_existing_sessions()
{
    util::CheckedLockGuard lock(m_session_mutex);
    return do_has_existing_sessions();
}

bool SyncManager::do_has_existing_sessions()
{
    return std::any_of(m_sessions.begin(), m_sessions.end(), [](auto& element) {
        return element.second->existing_external_reference();
    });
}

void SyncManager::wait_for_sessions_to_terminate()
{
    auto& client = get_sync_client(); // Throws
    client.wait_for_session_terminations();
}

void SyncManager::unregister_session(const std::string& path)
{
    util::CheckedUniqueLock lock(m_session_mutex);
    auto it = m_sessions.find(path);
    if (it == m_sessions.end()) {
        // The session may already be unregistered. This always happens in the
        // SyncManager destructor, and can also happen due to multiple threads
        // tearing things down at once.
        return;
    }

    // Sync session teardown calls this function, so we need to be careful with
    // locking here. We need to unlock `m_session_mutex` before we do anything
    // which could result in a re-entrant call or we'll deadlock, which in this
    // function means unlocking before we destroy a `shared_ptr<SyncSession>`
    // (either the external reference or internal reference versions).
    // The external reference version will only be the final reference if
    // another thread drops a reference while we're in this function.
    // Dropping the final internal reference does not appear to ever actually
    // result in a recursive call to this function at the time this comment was
    // written, but releasing the lock in that case as well is still safer.

    if (auto existing_session = it->second->existing_external_reference()) {
        // We got here because the session entered the inactive state, but
        // there's still someone referencing it so we should leave it be. This
        // can happen if the user was logged out, or if all Realms using the
        // session were destroyed but the SDK user is holding onto the session.

        // Explicit unlock so that `existing_session`'s destructor runs after
        // the unlock for the reasons noted above
        lock.unlock();
        return;
    }

    // Remove the session from the map while holding the lock, but then defer
    // destroying it until after we unlock the mutex for the reasons noted above.
    auto session = m_sessions.extract(it);
    lock.unlock();
}

void SyncManager::enable_session_multiplexing()
{
    util::CheckedLockGuard lock(m_mutex);
    if (m_config.multiplex_sessions)
        return; // Already enabled, we can ignore

    if (m_sync_client)
        throw std::logic_error("Cannot enable session multiplexing after creating the sync client");

    m_config.multiplex_sessions = true;
}

SyncClient& SyncManager::get_sync_client() const
{
    util::CheckedLockGuard lock(m_mutex);
    if (!m_sync_client)
        m_sync_client = create_sync_client(); // Throws
    return *m_sync_client;
}

std::unique_ptr<SyncClient> SyncManager::create_sync_client() const
{
    return std::make_unique<SyncClient>(m_logger_ptr, m_config, weak_from_this());
}

util::Optional<SyncAppMetadata> SyncManager::app_metadata() const
{
    util::CheckedLockGuard lock(m_file_system_mutex);
    if (!m_metadata_manager) {
        return util::none;
    }
    return m_metadata_manager->get_app_metadata();
}

void SyncManager::close_all_sessions()
{
    // log_out() will call unregister_session(), which requires m_session_mutex,
    // so we need to iterate over them without holding the lock.
    decltype(m_sessions) sessions;
    {
        util::CheckedLockGuard lk(m_session_mutex);
        m_sessions.swap(sessions);
    }

    for (auto& [_, session] : sessions) {
        session->force_close();
    }

    get_sync_client().wait_for_session_terminations();
}
