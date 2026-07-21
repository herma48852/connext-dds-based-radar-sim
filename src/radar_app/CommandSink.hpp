#pragma once
// Minimal command target shared by the production DDS console and UI tests.
// Keeping the panel layer dependent on this interface lets logic-only ImGui
// smoke tests exercise the real controls without creating DDS entities.

#include <cstdint>

namespace radar::app {

class CommandSink {
public:
    virtual ~CommandSink() = default;

    virtual void send(int32_t type, double center = 0.0, double width = 0.0,
                      const char* params = "", int32_t priority = 3) = 0;
};

} // namespace radar::app
