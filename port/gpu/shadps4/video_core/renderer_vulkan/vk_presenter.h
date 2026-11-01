// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

#include <deque>

#include <condition_variable>

#include <memory>

#include "core/libraries/videoout/buffer.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/postfx_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

class FsrFrameGen;

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    vk::Format format{}; ///< tlg: FrameFormat() at creation
    u8 id{};
    // tlg: frame generation (setting frame_generation, TLG_FG): the image shown before this frame, made on first use.
    VmaAllocation fg_allocation{};
    vk::Image fg_image{};
    vk::ImageView fg_view{};
    bool fg_ready{false};
};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        swapchain.SetHDR(enable);
        pp_settings.hdr = enable ? 1 : 0;
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address);

    Frame* PrepareBlankFrame(bool present_thread);

    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    /// tlg: a black frame with the loading screen (TlgOverlay::SetLoading) only.
    void PresentLoadingFrame();
    Frame* PrepareLastFrame();

private:
    Frame* GetRenderFrame();

    void RecreateFrame(Frame* frame, u32 width, u32 height);

    void DestroyFgImage(Frame* frame);
    /// tlg: records the generated image for a finished frame, when frame generation is on.
    void RecordFrameGeneration(Frame* frame);
    /// tlg: blits source to the next swapchain image and presents it. Only the frame's last
    /// present signals present_done. False: skipped, the swapchain is being rebuilt.
    bool PresentImage(Frame* frame, vk::Image source, bool last);

    void SetExpectedGameSize(s32 width, s32 height);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    HostPasses::PostProcessingPass pp_pass_8bit; ///< tlg: for 8-bit frames (FrameFormat)
    HostPasses::SmaaPass smaa_pass;  ///< tlg: tlg.ini smaa
    HostPasses::CasPass cas_pass;    ///< tlg: tlg.ini cas
    HostPasses::PostFxTimer postfx_timer; ///< tlg: TLG_POSTFX_PROFILE=1
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    int fg_state{-1}; ///< tlg: DLSS frame generation: -1 not checked yet, 0 not available, 1 available
    int fg_active{}, fg_failed{}; ///< tlg: the mode as last seen; the mode that failed
    bool fg_restart{}; ///< tlg: next evaluate starts over
    std::unique_ptr<FsrFrameGen> fsr_frame_gen;
    u32 fg_width{}, fg_height{};
    vk::Format fg_format{};
    u64 fg_presented{};
    std::chrono::steady_clock::time_point fg_last_flip{};
    float fg_interval_ms{16.6f};
    std::deque<u64> recent_frame_ticks; ///< tlg: TLG_FRAMES_AHEAD bound (PrepareFrame)
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame;
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::condition_variable_any frame_cv;
    std::vector<VAddr> vo_buffers_addr;
};

} // namespace Vulkan
