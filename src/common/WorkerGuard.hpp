#pragma once

#include <atomic>
#include <exception>
#include <utility>

#include "Log.hpp"

namespace radar {

inline std::atomic<bool>& worker_failure_flag() {
    static std::atomic<bool> failed{false};
    return failed;
}

inline bool worker_failure_requested() {
    return worker_failure_flag().load(std::memory_order_acquire);
}

template <typename F>
void run_worker_guarded(const char* name, F&& fn) noexcept {
    try {
        std::forward<F>(fn)();
    } catch (const std::exception& e) {
        RADAR_LOG << "[" << name << "] worker exception: " << e.what() << "\n";
        worker_failure_flag().store(true, std::memory_order_release);
    } catch (...) {
        RADAR_LOG << "[" << name << "] worker exception: unknown exception\n";
        worker_failure_flag().store(true, std::memory_order_release);
    }
}

} // namespace radar
