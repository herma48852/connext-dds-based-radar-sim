#pragma once
// ============================================================================
// Thread-safe console logging. Component worker threads wrote to std::cout
// unsynchronized; the 2026-07-20 TSan run flagged the resulting race on
// iostream internals (TrackManager::update_loop vs HmiUi::housekeeping_loop).
// RADAR_LOG builds the line in a local stream and emits it under one mutex,
// so concurrent threads can never interleave inside the stream object.
// Every line is flushed: the last lines before a fatal signal are evidence.
//
// Usage:  RADAR_LOG << "[Tag] field=" << value << "\n";   // one line per stmt
// ============================================================================

#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>

namespace radar {

class LogLine {
public:
    LogLine() = default;
    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;
    ~LogLine() {
        std::lock_guard lk(mutex());
        std::cout << stream_.str() << std::flush;
    }
    template <typename T>
    LogLine& operator<<(T&& v) {
        stream_ << std::forward<T>(v);
        return *this;
    }
    // Stream manipulators (std::endl & co.): overloaded function templates
    // cannot bind to the deduced overload above — take them explicitly.
    LogLine& operator<<(std::ostream& (*m)(std::ostream&)) {
        m(stream_);
        return *this;
    }

private:
    static std::mutex& mutex() {
        static std::mutex m;
        return m;
    }
    std::ostringstream stream_;
};

} // namespace radar

#define RADAR_LOG ::radar::LogLine{}
