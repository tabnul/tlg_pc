// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <source_location>
#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/string_literal.h"
#include "common/types.h"
#include "tlg_perf_stats.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Frontend {
enum class WindowSystemType : u8;
class WindowSDL;
} // namespace Frontend

namespace Vulkan {

constexpr u32 TargetVulkanApiVersion = VK_API_VERSION_1_3;

vk::SurfaceKHR CreateSurface(vk::Instance instance, const Frontend::WindowSDL& emu_window);

vk::UniqueInstance CreateInstance(Frontend::WindowSystemType window_type, bool enable_validation,
                                  bool enable_crash_diagnostic);

vk::UniqueDebugUtilsMessengerEXT CreateDebugCallback(vk::Instance instance);

template <typename T>
concept VulkanHandleType = vk::isVulkanHandleType<T>::value;

template <VulkanHandleType HandleType>
void SetObjectName(vk::Device device, const HandleType& handle, std::string_view debug_name) {
    const vk::DebugUtilsObjectNameInfoEXT name_info = {
        .objectType = HandleType::objectType,
        .objectHandle = reinterpret_cast<u64>(static_cast<typename HandleType::NativeType>(handle)),
        .pObjectName = debug_name.data(),
    };
    auto result = device.setDebugUtilsObjectNameEXT(name_info);
    if (result != vk::Result::eSuccess) {
        LOG_DEBUG(Render_Vulkan, "Could not set object debug name: {}", vk::to_string(result));
    }
}

template <VulkanHandleType HandleType, typename... Args>
void SetObjectName(vk::Device device, const HandleType& handle, const char* format,
                   const Args&... args) {
    const std::string debug_name = fmt::vformat(format, fmt::make_format_args(args...));
    SetObjectName(device, handle, debug_name);
}

// tlg: a failed check names its caller and stops with a code per kind (assert_set_stop_code).
inline void CheckFailed(vk::Result r, const char* what, const std::source_location& loc) {
    if (r == vk::Result::eErrorOutOfHostMemory || r == vk::Result::eErrorOutOfDeviceMemory ||
        r == vk::Result::eErrorOutOfPoolMemory) {
        assert_set_stop_code(24);
    } else if (r == vk::Result::eErrorDeviceLost) {
        assert_set_stop_code(25);
    }
    const char* file = loc.file_name();
    for (const char* p = file; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            file = p + 1;
        }
    }
    LOG_CRITICAL(Render, "Vulkan call failed at {}:{} ({}){}{}: vk::Result={}", file, loc.line(),
                 loc.function_name(), *what ? ": failed to " : "", what, vk::to_string(r));
    if (r == vk::Result::eErrorOutOfHostMemory || r == vk::Result::eErrorOutOfDeviceMemory ||
        r == vk::Result::eErrorOutOfPoolMemory) {
        // tlg (issue #79): what the process, the driver and the arenas held when it stopped.
        TlgPerfStats::MemoryStopReport();
    }
    assert_fail_impl();
}

template <StringLiteral msg = "">
static void Check(vk::Result r, const std::source_location loc = std::source_location::current()) {
    if (r != vk::Result::eSuccess) [[unlikely]] {
        CheckFailed(r, msg.value, loc);
    }
}

template <StringLiteral msg = "", typename T>
static T Check(vk::ResultValue<T> r,
               const std::source_location loc = std::source_location::current()) {
    if (r.result != vk::Result::eSuccess) [[unlikely]] {
        CheckFailed(r.result, msg.value, loc);
    }
    return std::move(r.value);
}

} // namespace Vulkan
