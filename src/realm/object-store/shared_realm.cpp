////////////////////////////////////////////////////////////////////////////
//
// Copyright 2015 Realm Inc.
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

#include <realm/object-store/shared_realm.hpp>

#include <realm/object-store/impl/collection_notifier.hpp>
#include <realm/object-store/impl/realm_coordinator.hpp>
#include <realm/object-store/impl/transact_log_handler.hpp>

#include <realm/object-store/audit.hpp>
#include <realm/object-store/binding_context.hpp>
#include <realm/object-store/list.hpp>
#include <realm/object-store/object.hpp>
#include <realm/object-store/object_schema.hpp>
#include <realm/object-store/object_store.hpp>
#include <realm/object-store/results.hpp>
#include <realm/object-store/schema.hpp>
#include <realm/object-store/thread_safe_reference.hpp>

#include <realm/object-store/util/scheduler.hpp>

#include <realm/db.hpp>
#include <realm/util/fifo_helper.hpp>
#include <realm/util/file.hpp>
#include <realm/util/scope_exit.hpp>

#if REALM_ENABLE_SYNC
#include <realm/object-store/sync/impl/sync_file.hpp>
#include <realm/object-store/sync/sync_manager.hpp>
#include <realm/object-store/sync/sync_user.hpp>
#include <realm/object-store/sync/sync_session.hpp>

#include <realm/sync/config.hpp>
#include <realm/sync/history.hpp>
#include <realm/sync/noinst/client_history_impl.hpp>
#include <realm/history.hpp>
#endif

#include <thread>

using namespace realm;
using namespace realm::_impl;

namespace {
class CountGuard {
public:
    CountGuard(size_t& count)
        : m_count(count)
    {
        ++m_count;
    }
    ~CountGuard()
    {
        --m_count;
    }

private:
    size_t& m_count;
};
} // namespace

Realm::Realm(Config config, util::Optional<VersionID> version, std::shared_ptr<_impl::RealmCoordinator> coordinator,
             MakeSharedTag)
    : m_config(std::move(config))
    , m_frozen_version(version)
    , m_scheduler(m_config.scheduler)
{
    if (version) {
        m_auto_refresh = false;
        REALM_ASSERT(*version != VersionID());
    }
    else if (!coordinator->get_cached_schema(m_schema, m_schema_version, m_schema_transaction_version)) {
        m_transaction = coordinator->begin_read();
        read_schema_from_group_if_needed();
        coordinator->cache_schema(m_schema, m_schema_version, m_schema_transaction_version);
        m_transaction = nullptr;
    }
    m_coordinator = std::move(coordinator);
}

Realm::~Realm()
{
    if (m_transaction) {
        // Wait for potential syncing to finish
        m_transaction->prepare_for_close();
        call_completion_callbacks();
    }

    if (m_coordinator) {
        m_coordinator->unregister_realm(this);
    }
}

Group& Realm::read_group()
{
    return transaction();
}

Transaction& Realm::transaction()
{
    verify_open();
    if (!m_transaction)
        begin_read(m_frozen_version.value_or(VersionID{}));
    return *m_transaction;
}

Transaction& Realm::transaction() const
{
    // one day we should change the way we use constness
    Realm* nc_realm = const_cast<Realm*>(this);
    return nc_realm->transaction();
}

std::shared_ptr<Transaction> Realm::transaction_ref()
{
    return m_transaction;
}

std::shared_ptr<Transaction> Realm::duplicate() const
{
    return m_coordinator->begin_read(read_transaction_version(), is_frozen());
}

std::shared_ptr<DB>& Realm::Internal::get_db(Realm& realm)
{
    return realm.m_coordinator->m_db;
}

void Realm::Internal::begin_read(Realm& realm, VersionID version_id)
{
    realm.begin_read(version_id);
}

void Realm::begin_read(VersionID version_id)
{
    REALM_ASSERT(!m_transaction);
    m_transaction = m_coordinator->begin_read(version_id, bool(m_frozen_version));
    add_schema_change_handler();
    read_schema_from_group_if_needed();
}

SharedRealm Realm::get_shared_realm(Config config)
{
    auto coordinator = RealmCoordinator::get_coordinator(config.path);
    return coordinator->get_realm(std::move(config), util::none);
}

SharedRealm Realm::get_frozen_realm(Config config, VersionID version)
{
    auto coordinator = RealmCoordinator::get_coordinator(config.path);
    return coordinator->get_realm(std::move(config), version);
}

SharedRealm Realm::get_shared_realm(ThreadSafeReference ref, std::shared_ptr<util::Scheduler> scheduler)
{
    if (!scheduler)
        scheduler = util::Scheduler::make_default();
    SharedRealm realm = ref.resolve<std::shared_ptr<Realm>>(nullptr);
    REALM_ASSERT(realm);
    auto& config = realm->config();
    auto coordinator = RealmCoordinator::get_coordinator(config.path);
    if (auto realm = coordinator->get_cached_realm(config, scheduler))
        return realm;
    realm->m_scheduler = scheduler;
    coordinator->bind_to_context(*realm);
    return realm;
}

#if REALM_ENABLE_SYNC
std::shared_ptr<AsyncOpenTask> Realm::get_synchronized_realm(Config config)
{
    auto coordinator = RealmCoordinator::get_coordinator(config.path);
    return coordinator->get_synchronized_realm(std::move(config));
}

std::shared_ptr<SyncSession> Realm::sync_session() const
{
    return m_coordinator->sync_session();
}

sync::SubscriptionSet Realm::get_latest_subscription_set()
{
    if (!m_config.sync_config || !m_config.sync_config->flx_sync_requested) {
        throw std::runtime_error("Flexible sync is not enabled");
    }
    // If there is a subscription store, then return the active set
    auto flx_sub_store = m_coordinator->sync_session()->get_flx_subscription_store();
    REALM_ASSERT(flx_sub_store);
    return flx_sub_store->get_latest();
}

sync::SubscriptionSet Realm::get_active_subscription_set()
{
    if (!m_config.sync_config || !m_config.sync_config->flx_sync_requested) {
        throw std::runtime_error("Flexible sync is not enabled");
    }
    // If there is a subscription store, then return the active set
    auto flx_sub_store = m_coordinator->sync_session()->get_flx_subscription_store();
    REALM_ASSERT(flx_sub_store);
    return flx_sub_store->get_active();
}
#endif

void Realm::set_schema(Schema const& reference, Schema schema)
{
    m_dynamic_schema = false;
    schema.copy_keys_from(reference, m_config.schema_subset_mode);
    m_schema = std::move(schema);
    notify_schema_changed();
}

void Realm::read_schema_from_group_if_needed()
{
    if (m_config.immutable()) {
        REALM_ASSERT(m_transaction);
        if (m_schema.empty()) {
            m_schema_version = ObjectStore::get_schema_version(*m_transaction);
            m_schema = ObjectStore::schema_from_group(*m_transaction);
        }
        return;
    }

    Group& group = read_group();
    auto current_version = transaction().get_version_of_current_transaction().version;
    if (m_schema_transaction_version == current_version)
        return;

    m_schema_transaction_version = current_version;
    m_schema_version = ObjectStore::get_schema_version(group);
    auto schema = ObjectStore::schema_from_group(group);

    if (m_coordinator)
        m_coordinator->cache_schema(schema, m_schema_version, m_schema_transaction_version);

    if (m_dynamic_schema) {
        if (m_schema == schema) {
            // The structure of the schema hasn't changed. Bring the table column indices up to date.
            m_schema.copy_keys_from(schema, SchemaSubsetMode::Strict);
        }
        else {
            // The structure of the schema has changed, so replace our copy of the schema.
            m_schema = std::move(schema);
        }
    }
    else {
        ObjectStore::verify_valid_external_changes(m_schema.compare(schema, m_config.schema_mode));
        m_schema.copy_keys_from(schema, m_config.schema_subset_mode);
    }
    notify_schema_changed();
}

bool Realm::reset_file(Schema& schema, std::vector<SchemaChange>& required_changes)
{
    // FIXME: this does not work if multiple processes try to open the file at
    // the same time, or even multiple threads if there is not any external
    // synchronization. The latter is probably fixable, but making it
    // multi-process-safe requires some sort of multi-process exclusive lock
    m_transaction = nullptr;
    m_coordinator->delete_and_reopen();

    m_schema = ObjectStore::schema_from_group(read_group());
    m_schema_version = ObjectStore::get_schema_version(read_group());
    required_changes = m_schema.compare(schema, m_config.schema_mode);
    m_coordinator->clear_schema_cache_and_set_schema_version(m_schema_version);
    return false;
}

bool Realm::schema_change_needs_write_transaction(Schema& schema, std::vector<SchemaChange>& changes,
                                                  uint64_t version)
{
    if (version == m_schema_version && changes.empty())
        return false;

    switch (m_config.schema_mode) {
        case SchemaMode::Automatic:
            if (version < m_schema_version && m_schema_version != ObjectStore::NotVersioned)
                throw InvalidSchemaVersionException(m_schema_version, version, false);
            return true;

        case SchemaMode::Immutable:
            if (version != m_schema_version)
                throw InvalidSchemaVersionException(m_schema_version, version, true);
            REALM_FALLTHROUGH;
        case SchemaMode::ReadOnly:
            ObjectStore::verify_compatible_for_immutable_and_readonly(changes);
            return m_schema_version == ObjectStore::NotVersioned;

        case SchemaMode::SoftResetFile:
            if (m_schema_version == ObjectStore::NotVersioned)
                return true;
            if (m_schema_version == version && !ObjectStore::needs_migration(changes))
                return true;
            REALM_FALLTHROUGH;
        case SchemaMode::HardResetFile:
            reset_file(schema, changes);
            return true;

        case SchemaMode::AdditiveDiscovered:
        case SchemaMode::AdditiveExplicit: {
            bool will_apply_index_changes = version > m_schema_version;
            if (ObjectStore::verify_valid_additive_changes(changes, will_apply_index_changes))
                return true;
            return version != m_schema_version;
        }

        case SchemaMode::Manual:
            if (version < m_schema_version && m_schema_version != ObjectStore::NotVersioned)
                throw InvalidSchemaVersionException(m_schema_version, version, false);
            if (version == m_schema_version) {
                ObjectStore::verify_no_changes_required(changes);
                REALM_UNREACHABLE(); // changes is non-empty so above line always throws
            }
            return true;
    }
    REALM_COMPILER_HINT_UNREACHABLE();
}

Schema Realm::get_full_schema()
{
    if (!m_config.immutable())
        do_refresh();

    // If the user hasn't specified a schema previously then m_schema is always
    // the full schema if it's been read
    if (m_dynamic_schema && !m_schema.empty())
        return m_schema;

    // Otherwise we may have a subset of the file's schema, so we need to get
    // the complete thing to calculate what changes to make
    Schema actual_schema;
    uint64_t actual_version;
    uint64_t version = -1;
    bool got_cached = m_coordinator->get_cached_schema(actual_schema, actual_version, version);
    if (!got_cached || version != transaction().get_version_of_current_transaction().version)
        return ObjectStore::schema_from_group(read_group());
    return actual_schema;
}

void Realm::set_schema_subset(Schema schema)
{
    REALM_ASSERT(m_dynamic_schema);
    REALM_ASSERT(m_schema_version != ObjectStore::NotVersioned);

    std::vector<SchemaChange> changes = m_schema.compare(schema, m_config.schema_mode);
    switch (m_config.schema_mode) {
        case SchemaMode::Automatic:
        case SchemaMode::SoftResetFile:
        case SchemaMode::HardResetFile:
            ObjectStore::verify_no_migration_required(changes);
            break;

        case SchemaMode::Immutable:
        case SchemaMode::ReadOnly:
            ObjectStore::verify_compatible_for_immutable_and_readonly(changes);
            break;

        case SchemaMode::AdditiveDiscovered:
        case SchemaMode::AdditiveExplicit:
            ObjectStore::verify_valid_additive_changes(changes);
            break;

        case SchemaMode::Manual:
            ObjectStore::verify_no_changes_required(changes);
            break;
    }

    set_schema(m_schema, std::move(schema));
}

void Realm::update_schema(Schema schema, uint64_t version, MigrationFunction migration_function,
                          DataInitializationFunction initialization_function, bool in_transaction)
{
    uint64_t validation_mode = SchemaValidationMode::Basic;
#if REALM_ENABLE_SYNC
    if (auto sync_config = m_config.sync_config) {
        validation_mode |=
            sync_config->flx_sync_requested ? SchemaValidationMode::SyncFLX : SchemaValidationMode::SyncPBS;
    }
#endif
    if (m_config.schema_mode == SchemaMode::AdditiveExplicit) {
        validation_mode |= SchemaValidationMode::RejectEmbeddedOrphans;
    }

    schema.validate(static_cast<SchemaValidationMode>(validation_mode));

    bool was_in_read_transaction = is_in_read_transaction();
    Schema actual_schema = get_full_schema();

    // Frozen Realms never modify the schema on disk and we just need to verify
    // that the requested schema is a subset of what actually exists
    if (m_frozen_version) {
        ObjectStore::verify_valid_external_changes(schema.compare(actual_schema, m_config.schema_mode, true));
        set_schema(actual_schema, std::move(schema));
        return;
    }

    std::vector<SchemaChange> required_changes = actual_schema.compare(schema, m_config.schema_mode);
    if (!schema_change_needs_write_transaction(schema, required_changes, version)) {
        if (!was_in_read_transaction)
            m_transaction = nullptr;
        set_schema(actual_schema, std::move(schema));
        return;
    }
    // Either the schema version has changed or we need to do non-migration changes

    // Cancel the write transaction if we exit this function before committing it
    auto cleanup = util::make_scope_exit([&]() noexcept {
        // When in_transaction is true, caller is responsible to cancel the transaction.
        if (!in_transaction && is_in_transaction())
            cancel_transaction();
        if (!was_in_read_transaction)
            m_transaction = nullptr;
    });

    if (!in_transaction) {
        transaction().promote_to_write();

        // Beginning the write transaction may have advanced the version and left
        // us with nothing to do if someone else initialized the schema on disk
        if (m_new_schema) {
            actual_schema = *m_new_schema;
            required_changes = actual_schema.compare(schema, m_config.schema_mode);
            if (!schema_change_needs_write_transaction(schema, required_changes, version)) {
                cancel_transaction();
                cache_new_schema();
                set_schema(actual_schema, std::move(schema));
                return;
            }
        }
        cache_new_schema();
    }

    schema.copy_keys_from(actual_schema, m_config.schema_subset_mode);

    uint64_t old_schema_version = m_schema_version;
    bool additive = m_config.schema_mode == SchemaMode::AdditiveDiscovered ||
                    m_config.schema_mode == SchemaMode::AdditiveExplicit ||
                    m_config.schema_mode == SchemaMode::ReadOnly;
    if (migration_function && !additive) {
        auto wrapper = [&] {
            auto config = m_config;
            config.schema_mode = SchemaMode::ReadOnly;
            config.schema = util::none;
            // Don't go through the normal codepath for opening a Realm because
            // we're using a mismatched config
            auto old_realm = std::make_shared<Realm>(std::move(config), none, m_coordinator, MakeSharedTag{});
            // block autorefresh for the old realm
            old_realm->m_auto_refresh = false;
            migration_function(old_realm, shared_from_this(), m_schema);
        };

        // migration function needs to see the target schema on the "new" Realm
        std::swap(m_schema, schema);
        std::swap(m_schema_version, version);
        m_in_migration = true;
        auto restore = util::make_scope_exit([&]() noexcept {
            std::swap(m_schema, schema);
            std::swap(m_schema_version, version);
            m_in_migration = false;
        });

        ObjectStore::apply_schema_changes(transaction(), version, m_schema, m_schema_version, m_config.schema_mode,
                                          required_changes, m_config.automatically_handle_backlinks_in_migrations,
                                          wrapper);
    }
    else {
        ObjectStore::apply_schema_changes(transaction(), m_schema_version, schema, version, m_config.schema_mode,
                                          required_changes, m_config.automatically_handle_backlinks_in_migrations);
        REALM_ASSERT_DEBUG(additive ||
                           (required_changes = ObjectStore::schema_from_group(read_group()).compare(schema)).empty());
    }

    if (initialization_function && old_schema_version == ObjectStore::NotVersioned) {
        // Initialization function needs to see the latest schema
        uint64_t temp_version = ObjectStore::get_schema_version(read_group());
        std::swap(m_schema, schema);
        std::swap(m_schema_version, temp_version);
        auto restore = util::make_scope_exit([&]() noexcept {
            std::swap(m_schema, schema);
            std::swap(m_schema_version, temp_version);
        });
        initialization_function(shared_from_this());
    }

    m_schema = std::move(schema);
    m_new_schema = ObjectStore::schema_from_group(read_group());
    m_schema_version = ObjectStore::get_schema_version(read_group());
    m_dynamic_schema = false;
    m_coordinator->clear_schema_cache_and_set_schema_version(version);

    if (!in_transaction) {
        m_coordinator->commit_write(*this);
        cache_new_schema();
    }

    notify_schema_changed();
}

void Realm::rename_property(Schema schema, StringData object_type, StringData old_name, StringData new_name)
{
    ObjectStore::rename_property(read_group(), schema, object_type, old_name, new_name);
}

void Realm::add_schema_change_handler()
{
    if (m_config.immutable())
        return;
    m_transaction->set_schema_change_notification_handler([&] {
        m_new_schema = ObjectStore::schema_from_group(read_group());
        m_schema_version = ObjectStore::get_schema_version(read_group());
        if (m_dynamic_schema) {
            m_schema = *m_new_schema;
        }
        else {
            m_schema.copy_keys_from(*m_new_schema, m_config.schema_subset_mode);
        }
        notify_schema_changed();
    });
}

void Realm::cache_new_schema()
{
    if (is_closed()) {
        return;
    }

    auto new_version = transaction().get_version_of_current_transaction().version;
    if (m_new_schema)
        m_coordinator->cache_schema(std::move(*m_new_schema), m_schema_version, new_version);
    else
        m_coordinator->advance_schema_cache(m_schema_transaction_version, new_version);
    m_schema_transaction_version = new_version;
    m_new_schema = util::none;
}

void Realm::translate_schema_error()
{
    // Read the new (incompatible) schema without changing our read transaction
    auto new_schema = ObjectStore::schema_from_group(*m_coordinator->begin_read());

    // Should always throw
    ObjectStore::verify_valid_external_changes(m_schema.compare(new_schema, m_config.schema_mode, true));

    // Something strange happened so just rethrow the old exception
    throw;
}

void Realm::notify_schema_changed()
{
    if (m_binding_context) {
        m_binding_context->schema_did_change(m_schema);
    }
}

static void check_can_create_write_transaction(const Realm* realm)
{
    if (realm->config().immutable() || realm->config().read_only()) {
        throw InvalidTransactionException("Can't perform transactions on read-only Realms.");
    }
    if (realm->is_frozen()) {
        throw InvalidTransactionException("Can't perform transactions on a frozen Realm");
    }
    if (!realm->is_closed() && realm->get_number_of_versions() > realm->config().max_number_of_active_versions) {
        throw InvalidTransactionException(
            util::format("Number of active versions (%1) in the Realm exceeded the limit of %2",
                         realm->get_number_of_versions(), realm->config().max_number_of_active_versions));
    }
}

void Realm::verify_thread() const
{
    if (m_scheduler && !m_scheduler->is_on_thread())
        throw IncorrectThreadException();
}

void Realm::verify_in_write() const
{
    if (!is_in_transaction()) {
        throw InvalidTransactionException("Cannot modify managed objects outside of a write transaction.");
    }
}

void Realm::verify_open() const
{
    if (is_closed()) {
        throw ClosedRealmException();
    }
}

bool Realm::verify_notifications_available(bool throw_on_error) const
{
    if (is_frozen()) {
        if (throw_on_error)
            throw InvalidTransactionException(
                "Notifications are not available on frozen lists since they do not change.");
        return false;
    }
    if (config().immutable()) {
        if (throw_on_error)
            throw InvalidTransactionException("Cannot create asynchronous query for immutable Realms");
        return false;
    }
    if (is_in_transaction()) {
        if (throw_on_error)
            throw InvalidTransactionException("Cannot create asynchronous query while in a write transaction");
        return false;
    }

    return true;
}

VersionID Realm::read_transaction_version() const
{
    verify_thread();
    verify_open();
    REALM_ASSERT(m_transaction);
    return m_transaction->get_version_of_current_transaction();
}

uint_fast64_t Realm::get_number_of_versions() const
{
    verify_open();
    return m_coordinator->get_number_of_versions();
}

bool Realm::is_in_transaction() const noexcept
{
    return !m_config.immutable() && !is_closed() && m_transaction &&
           transaction().get_transact_stage() == DB::transact_Writing;
}

bool Realm::is_in_async_transaction() const noexcept
{
    return !m_config.immutable() && !is_closed() && m_transaction && m_transaction->is_async();
}

util::Optional<VersionID> Realm::current_transaction_version() const
{
    util::Optional<VersionID> ret;
    if (m_transaction) {
        ret = m_transaction->get_version_of_current_transaction();
    }
    else if (m_frozen_version) {
        ret = m_frozen_version;
    }
    return ret;
}

// Get the version of the latest snapshot
util::Optional<DB::version_type> Realm::latest_snapshot_version() const
{
    util::Optional<DB::version_type> ret;
    if (m_transaction) {
        ret = m_transaction->get_version_of_latest_snapshot();
    }
    return ret;
}

void Realm::enable_wait_for_change()
{
    m_coordinator->enable_wait_for_change();
}

bool Realm::wait_for_change()
{
    if (m_frozen_version) {
        return false;
    }
    return m_transaction ? m_coordinator->wait_for_change(transaction_ref()) : false;
}

void Realm::wait_for_change_release()
{
    m_coordinator->wait_for_change_release();
}

bool Realm::has_pending_async_work() const
{
    verify_thread();
    return !m_async_commit_q.empty() || !m_async_write_q.empty() || (m_transaction && m_transaction->is_async());
}

void Realm::run_writes_on_proper_thread()
{
    m_scheduler->invoke([self = shared_from_this()] {
        self->run_writes();
    });
}

void Realm::call_completion_callbacks()
{
    if (m_is_running_async_commit_completions) {
        return;
    }

    CountGuard sending_completions(m_is_running_async_commit_completions);
    auto error = m_transaction->get_commit_exception();
    auto completions = std::move(m_async_commit_q);
    m_async_commit_q.clear();
    for (auto& cb : completions) {
        if (!cb.when_completed)
            continue;
        if (m_async_exception_handler) {
            try {
                cb.when_completed(error);
            }
            catch (...) {
                m_async_exception_handler(cb.handle, std::current_exception());
            }
        }
        else {
            cb.when_completed(error);
        }
    }
}

void Realm::run_async_completions()
{
    call_completion_callbacks();
    check_pending_write_requests();
}

void Realm::check_pending_write_requests()
{
    if (!m_async_write_q.empty()) {
        if (m_transaction->is_async()) {
            run_writes_on_proper_thread();
        }
        else {
            m_coordinator->async_request_write_mutex(*this);
        }
    }
}

void Realm::end_current_write(bool check_pending)
{
    if (!m_transaction) {
        return;
    }
    m_transaction->async_complete_writes([self = shared_from_this(), this]() mutable {
        m_scheduler->invoke([self = std::move(self), this]() mutable {
            run_async_completions();
            self.reset();
        });
    });
    if (check_pending && m_async_commit_q.empty()) {
        check_pending_write_requests();
    }
}

void Realm::run_writes()
{
    if (!m_transaction) {
        // Realm might have been closed
        return;
    }
    if (m_transaction->is_synchronizing()) {
        // Wait for the synchronization complete callback before we run more
        // writes as we can't add commits while in that state
        return;
    }

    CountGuard running_writes(m_is_running_async_writes);
    int run_limit = 20; // max number of commits without full sync to disk
    // this is tricky
    //  - each pending call may itself add other async writes
    //  - the 'run' will terminate as soon as a commit without grouping is requested
    while (!m_async_write_q.empty() && m_transaction) {
        // We might have made a sync commit and thereby given up the write lock
        if (!m_transaction->holds_write_mutex()) {
            return;
        }

        do_begin_transaction();

        auto write_desc = std::move(m_async_write_q.front());
        m_async_write_q.pop_front();

        // prevent any calls to commit/cancel during a simple notification
        m_notify_only = write_desc.notify_only;
        m_async_commit_barrier_requested = false;
        auto prev_version = m_transaction->get_version();
        try {
            write_desc.writer();
        }
        catch (const std::exception&) {
            if (m_transaction) {
                transaction::cancel(*m_transaction, m_binding_context.get());
            }
            m_notify_only = false;

            if (m_async_exception_handler) {
                m_async_exception_handler(write_desc.handle, std::current_exception());
                continue;
            }
            end_current_write();
            throw;
        }

        // if we've merely delivered a notification, the full transaction will follow later
        // and terminate with a call to async commit or async cancel
        if (m_notify_only) {
            m_notify_only = false;
            return;
        }

        // Realm may have been closed in the write function
        if (!m_transaction) {
            return;
        }

        auto new_version = m_transaction->get_version();
        // if we've run the full transaction, there is follow up work to do:
        if (new_version > prev_version) {
            // A commit was done during callback
            --run_limit;
            if (run_limit <= 0)
                break;
        }
        else {
            if (m_transaction->get_transact_stage() == DB::transact_Writing) {
                // Still in writing stage - we make a rollback
                transaction::cancel(transaction(), m_binding_context.get());
            }
        }
        if (m_async_commit_barrier_requested)
            break;
    }

    end_current_write();
}

auto Realm::async_begin_transaction(util::UniqueFunction<void()>&& the_write_block, bool notify_only) -> AsyncHandle
{
    verify_thread();
    check_can_create_write_transaction(this);
    if (m_is_running_async_commit_completions) {
        throw InvalidTransactionException(
            "Can't begin a write transaction from inside a commit completion callback.");
    }
    if (!m_scheduler->can_invoke()) {
        throw InvalidTransactionException(
            "Cannot schedule async transaction. Make sure you are running from inside a run loop.");
    }
    REALM_ASSERT(the_write_block);

    // make sure we have a (at least a) read transaction
    transaction();
    auto handle = m_async_commit_handle++;
    m_async_write_q.push_back({std::move(the_write_block), notify_only, handle});

    if (!m_is_running_async_writes && !m_transaction->is_async() &&
        m_transaction->get_transact_stage() != DB::transact_Writing) {
        m_coordinator->async_request_write_mutex(*this);
    }
    return handle;
}

auto Realm::async_commit_transaction(util::UniqueFunction<void(std::exception_ptr)>&& completion, bool allow_grouping)
    -> AsyncHandle
{
    verify_thread();
    if (m_is_running_async_commit_completions) {
        throw InvalidTransactionException(
            "Can't commit a write transaction from inside a commit completion callback.");
    }
    if (!is_in_transaction()) {
        throw InvalidTransactionException("Can't commit a non-existing write transaction");
    }

    m_transaction->promote_to_async();
    REALM_ASSERT(m_transaction->holds_write_mutex());
    REALM_ASSERT(!m_notify_only);
    // auditing is not supported
    REALM_ASSERT(!audit_context());
    // grab a version lock on current version, push it along with the done block
    // do in-buffer-cache commit_transaction();
    auto handle = m_async_commit_handle++;
    m_async_commit_q.push_back({std::move(completion), handle});
    try {
        m_coordinator->commit_write(*this, /* commit_to_disk: */ false);
    }
    catch (...) {
        // If the exception happened before the commit, we need to roll back the
        // transaction and remove the completion handler from the queue
        if (is_in_transaction()) {
            // Exception happened before the commit, so roll back the transaction
            // and remove the completion handler from the queue
            cancel_transaction();
            auto it = std::find_if(m_async_commit_q.begin(), m_async_commit_q.end(), [=](auto& e) {
                return e.handle == handle;
            });
            if (it != m_async_commit_q.end()) {
                m_async_commit_q.erase(it);
            }
        }
        else if (m_transaction) {
            end_current_write(false);
        }
        throw;
    }

    if (m_is_running_async_writes) {
        // we're called from with the callback loop and it will take care of releasing lock
        // if applicable, and of triggering followup runs of callbacks
        if (!allow_grouping) {
            m_async_commit_barrier_requested = true;
        }
    }
    else {
        // we're called from outside the callback loop so we have to take care of
        // releasing any lock and of keeping callbacks coming.
        if (allow_grouping) {
            run_writes();
        }
        else {
            end_current_write(false);
        }
    }
    return handle;
}

bool Realm::async_cancel_transaction(AsyncHandle handle)
{
    verify_thread();
    auto compare = [handle](auto& elem) {
        return elem.handle == handle;
    };

    auto it1 = std::find_if(m_async_write_q.begin(), m_async_write_q.end(), compare);
    if (it1 != m_async_write_q.end()) {
        m_async_write_q.erase(it1);
        return true;
    }
    auto it2 = std::find_if(m_async_commit_q.begin(), m_async_commit_q.end(), compare);
    if (it2 != m_async_commit_q.end()) {
        // Just delete the callback. It is important that we know
        // that there are still commits pending.
        it2->when_completed = nullptr;
        return true;
    }
    return false;
}

void Realm::begin_transaction()
{
    verify_thread();
    check_can_create_write_transaction(this);

    if (is_in_transaction()) {
        throw InvalidTransactionException("The Realm is already in a write transaction");
    }

    // Any of the callbacks to user code below could drop the last remaining
    // strong reference to `this`
    auto retain_self = shared_from_this();

    // make sure we have a read transaction
    read_group();

    do_begin_transaction();
}

void Realm::do_begin_transaction()
{
    CountGuard sending_notifications(m_is_sending_notifications);
    try {
        m_coordinator->promote_to_write(*this);
    }
    catch (_impl::UnsupportedSchemaChange const&) {
        translate_schema_error();
    }
    cache_new_schema();

    if (m_transaction && !m_transaction->has_unsynced_commits()) {
        call_completion_callbacks();
    }
}

void Realm::commit_transaction()
{
    check_can_create_write_transaction(this);
    verify_thread();

    if (!is_in_transaction()) {
        throw InvalidTransactionException("Can't commit a non-existing write transaction");
    }

    DB::VersionID prev_version = transaction().get_version_of_current_transaction();
    if (auto audit = audit_context()) {
        audit->prepare_for_write(prev_version);
    }

    m_coordinator->commit_write(*this, /* commit_to_disk */ true);
    cache_new_schema();

    // Realm might have been closed
    if (m_transaction) {
        // Any previous async commits got flushed along with the sync commit
        call_completion_callbacks();
        // If we have pending async writes we need to rerequest the write mutex
        check_pending_write_requests();
    }
    if (auto audit = audit_context()) {
        audit->record_write(prev_version, transaction().get_version_of_current_transaction());
    }
}

void Realm::cancel_transaction()
{
    check_can_create_write_transaction(this);
    verify_thread();

    if (m_is_running_async_commit_completions) {
        throw InvalidTransactionException(
            "Can't cancel a write transaction from inside a commit completion callback.");
    }
    if (!is_in_transaction()) {
        throw InvalidTransactionException("Can't cancel a non-existing write transaction");
    }

    transaction::cancel(transaction(), m_binding_context.get());

    if (m_transaction && !m_is_running_async_writes) {
        if (m_async_write_q.empty()) {
            end_current_write();
        }
        else {
            check_pending_write_requests();
        }
    }
}

void Realm::invalidate()
{
    verify_open();
    verify_thread();

    if (m_is_sending_notifications) {
        // This was originally because closing the Realm during notification
        // sending would break things, but we now support that. However, it's a
        // breaking change so we keep the old behavior for now.
        return;
    }

    if (is_in_transaction()) {
        cancel_transaction();
    }

    do_invalidate();
}

void Realm::do_invalidate()
{
    if (!m_config.immutable() && m_transaction) {
        m_transaction->prepare_for_close();
        call_completion_callbacks();
        transaction().close();
    }

    m_transaction = nullptr;
    m_async_write_q.clear();
    m_async_commit_q.clear();
}

bool Realm::compact()
{
    verify_thread();
    verify_open();

    if (m_config.immutable() || m_config.read_only()) {
        throw InvalidTransactionException("Can't compact a read-only Realm");
    }
    if (is_in_transaction()) {
        throw InvalidTransactionException("Can't compact a Realm within a write transaction");
    }

    verify_open();
    m_transaction = nullptr;
    return m_coordinator->compact();
}

void Realm::convert(const Config& config, bool merge_into_existing)
{
    verify_thread();

#if REALM_ENABLE_SYNC
    auto src_is_flx_sync = m_config.sync_config && m_config.sync_config->flx_sync_requested;
    auto dst_is_flx_sync = config.sync_config && config.sync_config->flx_sync_requested;
    auto dst_is_pbs_sync = config.sync_config && !config.sync_config->flx_sync_requested;

    if (dst_is_flx_sync && !src_is_flx_sync) {
        throw std::logic_error(
            "Realm cannot be converted to a flexible sync realm unless flexible sync is already enabled");
    }
    if (dst_is_pbs_sync && src_is_flx_sync) {
        throw std::logic_error(
            "Realm cannot be converted from a flexible sync realm to a partition based sync realm");
    }

#endif

    if (merge_into_existing && util::File::exists(config.path)) {
        auto destination_realm = Realm::get_shared_realm(config);
        destination_realm->begin_transaction();
        auto destination = destination_realm->transaction_ref();
        m_transaction->copy_to(destination);
        destination_realm->commit_transaction();
        return;
    }

    if (config.encryption_key.size() && config.encryption_key.size() != 64) {
        throw InvalidEncryptionKeyException();
    }

    try {
        auto& tr = transaction();
        auto repl = tr.get_replication();
        bool src_is_sync = repl && repl->get_history_type() == Replication::hist_SyncClient;
        bool dst_is_sync = config.sync_config || config.force_sync_history;

        if (dst_is_sync) {
            m_coordinator->write_copy(config.path, config.encryption_key.data());
            if (!src_is_sync) {
#if REALM_ENABLE_SYNC
                DBOptions options;
                if (config.encryption_key.size()) {
                    options.encryption_key = config.encryption_key.data();
                }
                auto db = DB::create(make_in_realm_history(), config.path, options);
                db->create_new_history(sync::make_client_replication());
#endif
            }
        }
        else {
            tr.write(config.path, config.encryption_key.data());
        }
    }
    catch (...) {
        _impl::translate_file_exception(config.path);
    }
}

OwnedBinaryData Realm::write_copy()
{
    verify_thread();
    BinaryData buffer = read_group().write_to_mem();

    // Since OwnedBinaryData does not have a constructor directly taking
    // ownership of BinaryData, we have to do this to avoid copying the buffer
    return OwnedBinaryData(std::unique_ptr<char[]>((char*)buffer.data()), buffer.size());
}

void Realm::notify()
{
    if (is_closed() || is_in_transaction() || is_frozen()) {
        return;
    }

    verify_thread();

    // Any of the callbacks to user code below could drop the last remaining
    // strong reference to `this`
    auto retain_self = shared_from_this();

    if (m_binding_context) {
        m_binding_context->before_notify();
        if (is_closed() || is_in_transaction()) {
            return;
        }
    }

    if (!m_coordinator->can_advance(*this)) {
        CountGuard sending_notifications(m_is_sending_notifications);
        m_coordinator->process_available_async(*this);
        return;
    }

    if (m_binding_context) {
        m_binding_context->changes_available();

        // changes_available() may have advanced the read version, and if
        // so we don't need to do anything further
        if (!m_coordinator->can_advance(*this))
            return;
    }

    CountGuard sending_notifications(m_is_sending_notifications);
    if (m_auto_refresh) {
        if (m_transaction) {
            try {
                m_coordinator->advance_to_ready(*this);
            }
            catch (_impl::UnsupportedSchemaChange const&) {
                translate_schema_error();
            }
            if (!is_closed())
                cache_new_schema();
        }
        else {
            if (m_binding_context) {
                m_binding_context->did_change({}, {});
            }
            if (!is_closed()) {
                m_coordinator->process_available_async(*this);
            }
        }
    }
}

bool Realm::refresh()
{
    verify_thread();
    return do_refresh();
}

bool Realm::do_refresh()
{
    // Frozen Realms never change.
    if (is_frozen()) {
        return false;
    }

    if (m_config.immutable()) {
        throw std::logic_error("Can't refresh a read-only Realm.");
    }

    // can't be any new changes if we're in a write transaction
    if (is_in_transaction()) {
        return false;
    }
    // don't advance if we're already in the process of advancing as that just
    // makes things needlessly complicated
    if (m_is_sending_notifications) {
        return false;
    }

    // Any of the callbacks to user code below could drop the last remaining
    // strong reference to `this`
    auto retain_self = shared_from_this();

    CountGuard sending_notifications(m_is_sending_notifications);
    if (m_binding_context) {
        m_binding_context->before_notify();
    }
    if (m_transaction) {
        try {
            bool version_changed = m_coordinator->advance_to_latest(*this);
            if (is_closed())
                return false;
            cache_new_schema();
            return version_changed;
        }
        catch (_impl::UnsupportedSchemaChange const&) {
            translate_schema_error();
        }
    }

    // No current read transaction, so just create a new one
    read_group();
    m_coordinator->process_available_async(*this);
    return true;
}

void Realm::set_auto_refresh(bool auto_refresh)
{
    if (is_frozen() && auto_refresh) {
        throw std::logic_error("Auto-refresh cannot be enabled for frozen Realms.");
    }
    m_auto_refresh = auto_refresh;
}


bool Realm::can_deliver_notifications() const noexcept
{
    if (m_config.immutable() || !m_config.automatic_change_notifications) {
        return false;
    }

    if (!m_scheduler || !m_scheduler->can_invoke()) {
        return false;
    }

    return true;
}

uint64_t Realm::get_schema_version(const Realm::Config& config)
{
    auto coordinator = RealmCoordinator::get_coordinator(config.path);
    auto version = coordinator->get_schema_version();
    if (version == ObjectStore::NotVersioned)
        version = ObjectStore::get_schema_version(coordinator->get_realm(config, util::none)->read_group());
    return version;
}


bool Realm::is_frozen() const
{
    bool result = bool(m_frozen_version);
    REALM_ASSERT_DEBUG((result && m_transaction) ? m_transaction->is_frozen() : true);
    return result;
}

SharedRealm Realm::freeze()
{
    read_group(); // Freezing requires a read transaction
    return m_coordinator->freeze_realm(*this);
}

void Realm::copy_schema_from(const Realm& source)
{
    REALM_ASSERT(is_frozen());
    REALM_ASSERT(m_frozen_version == source.read_transaction_version());
    m_schema = source.m_schema;
    m_schema_version = source.m_schema_version;
    m_schema_transaction_version = m_frozen_version->version;
    m_dynamic_schema = false;
}

void Realm::close()
{
    if (is_closed()) {
        return;
    }
    if (m_coordinator) {
        m_coordinator->unregister_realm(this);
    }

    do_invalidate();

    m_binding_context = nullptr;
    m_coordinator = nullptr;
    m_scheduler = nullptr;
    m_config = {};
}

void Realm::delete_files(const std::string& realm_file_path, bool* did_delete_realm)
{
    try {
        auto lock_successful = DB::call_with_lock(realm_file_path, [=](auto const& path) {
            DB::delete_files(path, did_delete_realm);
        });
        if (!lock_successful) {
            throw DeleteOnOpenRealmException(realm_file_path);
        }
    }
    catch (const util::File::NotFound&) {
        // Thrown only if the parent directory of the lock file does not exist,
        // which obviously indicates that we didn't need to delete anything
        if (did_delete_realm) {
            *did_delete_realm = false;
        }
    }
}

AuditInterface* Realm::audit_context() const noexcept
{
    return m_coordinator ? m_coordinator->audit_context() : nullptr;
}

MismatchedConfigException::MismatchedConfigException(StringData message, StringData path)
    : std::logic_error(util::format(message.data(), path))
{
}

MismatchedRealmException::MismatchedRealmException(StringData message)
    : std::logic_error(message.data())
{
}
