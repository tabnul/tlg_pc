// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>

namespace Vulkan::SceneResolution {
struct Size {
    uint32_t width = 1920, height = 1080;
    bool operator==(const Size&) const = default;
};
/// tlg: the game's frame size: TLG_RESOLUTION_PATCH (run.py) or the shipped 1920x1080.
inline Size Native() {
    static const Size native = [] {
        Size size;
        if (const char* patch = std::getenv("TLG_RESOLUTION_PATCH")) {
            unsigned w = 0, h = 0;
            if (std::sscanf(patch, "%ux%u", &w, &h) == 2 && w && h && w < 32768 && h < 32768) {
                size = {w, h};
            }
        }
        return size;
    }();
    return native;
}
/// The step a scene dimension keeps so that Logical() is exact: the shaders convert FragCoord
/// with 1920 / logical width (emit_spirv_context_get_set.cpp), and half resolution halves it.
constexpr uint32_t Step(uint32_t native, uint32_t shipped) {
    const uint32_t step = native / std::gcd(native, shipped);
    return step % 2 ? step * 2 : step;
}
inline Size ForPreset(int preset, Size output = {}) {
    // Even dimensions for half-resolution effects; match the preset labels exactly.
    constexpr std::array<double, 5> scales{1.0, 1.5, 1.7, 2.0, 3.0};
    const double scale = scales[std::clamp(preset, 0, 4)];
    const Size native = Native();
    const auto round = [scale](uint32_t size, uint32_t step) {
        return uint32_t(std::max(1l, std::lround(size / scale / step)) * step);
    };
    return {round(output.width, Step(native.width, 1920)),
            round(output.height, Step(native.height, 1080))};
}
/// A scene size in the shipped 1920x1080 coordinates the FragCoord conversion expects.
inline Size Logical(Size size) {
    const Size native = Native();
    return {uint32_t(uint64_t(size.width) * 1920 / native.width),
            uint32_t(uint64_t(size.height) * 1080 / native.height)};
}
/// The FragCoord conversion constant: per axis a 13-bit logical size and a 3-bit sample phase,
/// the point in the reduced pixel (eighths) a draw samples. Center unless the draw is jittered:
/// screen-space dither then walks the native pixels a reduced pixel covers (FragPhase).
constexpr uint32_t CenterPhase = 4;
constexpr uint32_t Pack(Size size, uint32_t phase_x = CenterPhase,
                        uint32_t phase_y = CenterPhase) {
    return size.width | (phase_x << 13) | (size.height << 16) | (phase_y << 29);
}
/// The sample phase of a draw shifted by jitter (pixels; content moves by +jitter).
inline uint32_t FragPhase(float jitter) {
    return uint32_t(std::clamp(std::lround((0.5f - jitter) * 8.0f), 0l, 7l));
}
constexpr Size Unpack(uint32_t packed) {
    return packed ? Size{packed & 0x1fffu, (packed >> 16) & 0x1fffu} : Size{};
}
// A proxy and the guest-size image hold the same logical surface. Native shader reads
// need a resolve after proxy writes; native writes invalidate the cached proxy.
struct Coherence {
    bool valid = false, dirty = false;
    void ProxyWrite() { valid = dirty = true; }
    void CopiedToProxy() { valid = true; dirty = false; }
    void Resolved() { dirty = false; }
    void NativeWrite() { valid = dirty = false; }
};
} // namespace Vulkan::SceneResolution
