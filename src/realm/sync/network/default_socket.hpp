#pragma once

#include <random>
#include <system_error>
#include <map>

#include <realm/sync/config.hpp>
#include <realm/sync/socket_provider.hpp>
#include <realm/sync/network/http.hpp>
#include <realm/sync/network/network.hpp>
#include <realm/util/future.hpp>
#include <realm/util/random.hpp>
#include <realm/util/tagged_bool.hpp>

namespace realm::sync::network {
class Service;
} // namespace realm::sync::network

namespace realm::sync::websocket {
using port_type = sync::port_type;

class DefaultSocketProvider : public SyncSocketProvider {
public:
    class Timer : public SyncSocketProvider::Timer {
    public:
        friend class DefaultSocketProvider;

        /// Cancels the timer and destroys the timer instance.
        ~Timer() = default;

        /// Cancel the timer immediately
        void cancel() override
        {
            m_timer.cancel();
        }

    protected:
        Timer(network::Service& service, std::chrono::milliseconds delay, FunctionHandler&& handler)
            : m_timer{service}
        {
            m_timer.async_wait(delay, std::move(handler));
        }

    private:
        network::DeadlineTimer m_timer;
    };

    struct AutoStartTag {
    };

    using AutoStart = util::TaggedBool<AutoStartTag>;
    DefaultSocketProvider(const std::shared_ptr<util::Logger>& logger, const std::string user_agent,
                          AutoStart auto_start = AutoStart{true});

    // Don't allow move or copy constructor
    DefaultSocketProvider(DefaultSocketProvider&&) = delete;

    ~DefaultSocketProvider();

    // Start the event loop if it is not started already. Otherwise, do nothing.
    void start();

    /// Temporary workaround until client shutdown has been updated in a separate PR - these functions
    /// will be handled internally when this happens.
    /// Stops the internal event loop (provided by network::Service)
    void stop(bool wait_for_stop = false) override;

    std::unique_ptr<WebSocketInterface> connect(std::unique_ptr<WebSocketObserver>, WebSocketEndpoint&&) override;

    void post(FunctionHandler&& handler) override
    {
        // Don't post empty handlers onto the event loop
        if (!handler)
            return;
        m_service.post(std::move(handler));
    }

    SyncTimer create_timer(std::chrono::milliseconds delay, FunctionHandler&& handler) override
    {
        return std::unique_ptr<Timer>(new DefaultSocketProvider::Timer(m_service, delay, std::move(handler)));
    }

private:
    enum class State { Starting, Running, Stopping, Stopped };

    /// Block until the state reaches the expected or later state - return true if state matches expected state
    void state_wait_for(std::unique_lock<std::mutex>& lock, State expected_state);
    /// Internal function for updating the state and signaling the wait_for_state condvar
    void do_state_update(std::unique_lock<std::mutex>&, State new_state);
    /// The execution code for the event loop thread
    void event_loop();

    std::shared_ptr<util::Logger> m_logger_ptr;
    network::Service m_service;
    std::mt19937_64 m_random;
    const std::string m_user_agent;
    std::mutex m_mutex;
    uint64_t m_event_loop_generation = 0;
    State m_state;                      // protected by m_mutex
    std::condition_variable m_state_cv; // uses m_mutex
    std::thread m_thread;               // protected by m_mutex
};

/// Class for the Default Socket Provider websockets that allows a simulated
/// http response to be specified for testing.
class DefaultWebSocket : public WebSocketInterface {
public:
    virtual ~DefaultWebSocket() = default;

    virtual void force_handshake_response_for_testing(int status_code, std::string body = "") = 0;

protected:
};

} // namespace realm::sync::websocket
