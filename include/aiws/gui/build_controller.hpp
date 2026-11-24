#pragma once

#include "aiws/processing_core.hpp"
#include "aiws/workspace.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace aiws::gui{

// The owner calls start, cancel and shutdown from the GUI thread.
class BuildController{
public:
    struct Result{
        std::size_t id{0};
        std::unique_ptr<ProcessingCore> core;
        std::string error;
        bool cancelled{false};
    };

    using Started = std::function<void(std::size_t)>;
    using Completed = std::function<void(Result)>;

    BuildController(Started started, Completed completed);
    ~BuildController();
    BuildController(const BuildController&) = delete;
    BuildController& operator=(const BuildController&) = delete;

    void start(Workspace snapshot, std::size_t id, int delay_ms);
    void cancel();
    void shutdown() noexcept;

private:
    struct Request{
        Workspace snapshot;
        std::size_t id;
        int delay_ms;
        std::shared_ptr<std::atomic_bool> cancelled;
    };

    void run();
    Result build(const Request& request);
    void stop_and_join(bool simulate_silent_shutdown) noexcept;

    const Started started_;
    const Completed completed_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<Request> pending_;
    std::shared_ptr<std::atomic_bool> active_token_;
    bool stopping_{false};
    std::thread worker_;
};

} // namespace aiws::gui
