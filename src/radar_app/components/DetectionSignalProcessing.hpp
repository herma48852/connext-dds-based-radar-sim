#pragma once

#include <span>

namespace radar::app {

constexpr bool receive_aperture_online(int active_elements) noexcept {
    return active_elements > 0;
}

// Invoke emit(bin, amplitude) for each local maximum above the detection
// threshold. A completely dark receive aperture can still contain synthesized
// thermal noise for the A-scope, but it cannot produce reportable detections.
template <typename Emit>
void for_each_cfar_detection(std::span<const float> magnitude,
                             bool aperture_online,
                             float threshold,
                             Emit&& emit) {
    if (!aperture_online || magnitude.size() < 3)
        return;

    for (std::size_t i = 1; i + 1 < magnitude.size(); ++i) {
        if (magnitude[i] > threshold &&
            magnitude[i] >= magnitude[i - 1] &&
            magnitude[i] > magnitude[i + 1]) {
            emit(static_cast<int>(i), magnitude[i]);
        }
    }
}

} // namespace radar::app
