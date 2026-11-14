// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "tlg_overlay.h"
#include "tlg_settings.h"
#include "tlg_graphics.h"
#include <thread>

#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_fsr_frame_gen.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_warmup_inbox.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"
#include "tlg_toggles.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <miniz.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <vk_mem_alloc.h>
#ifdef MemoryBarrier
#undef MemoryBarrier // tlg: winnt.h macro (through fmt), clashes with vk::MemoryBarrier
#endif

namespace Vulkan {

/// tlg: TLG_PRESENT_SKIP_CLEAR=0 clears the whole swapchain image before the blit again, and
/// filters the blit linearly, as before. Default on: the clear only runs when the blit leaves
/// part of the swapchain image untouched (letterbox bars). Backport of infamous_pc 028f31c.
static bool SkipPresentClear() {
    static const bool skip = [] {
        const char* env = std::getenv("TLG_PRESENT_SKIP_CLEAR");
        const bool on = !(env && env[0] == '0');
        if (!on) {
            std::printf("GPU: present clears the whole swapchain image (TLG_PRESENT_SKIP_CLEAR=0)\n");
        }
        return on;
    }();
    return skip;
}

/// tlg: the frame (the post-process target, blitted to the swapchain) is 10-bit, so the HDR10
/// PQ signal keeps its precision; the swapchain's 8-bit SDR format banded dark gradients in HDR.
/// TLG_FRAME_10BIT=0, or the live toggle bit Frame10Bit: the swapchain's SDR format, as before.
static vk::Format FrameFormat(const Swapchain& swapchain) {
    static const bool ten_bit = [] {
        const char* env = std::getenv("TLG_FRAME_10BIT");
        return !(env && env[0] == '0');
    }();
    return ten_bit && !TlgToggle::Disabled2(TlgToggle::Frame10Bit)
               ? vk::Format::eA2B10G10R10UnormPack32
               : swapchain.GetSurfaceFormat().format;
}

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

// tlg: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    smaa_pass.Create(device, instance.GetAllocator());
    cas_pass.Create(device, instance.GetAllocator());
    postfx_timer.Create(device, instance.GetPhysicalDevice().getProperties().limits.timestampPeriod);
    pp_pass.Create(device, vk::Format::eA2B10G10R10UnormPack32);
    pp_pass_8bit.Create(device, swapchain.GetSurfaceFormat().format);
    TlgOverlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images);

    const bool binary_cache = EmulatorSettingsImpl::Flag("TLG_PIPELINE_BINARIES", true) &&
                              EmulatorSettings.IsPipelineCacheEnabled();
    const bool capture_only = EmulatorSettingsImpl::Flag("TLG_PIPELINE_BINARY_ISOLATED", false) &&
                              EmulatorSettingsImpl::Flag("TLG_PIPELINE_CAPTURE_ONLY", false);
    if (UseOnDemandPipelines(binary_cache, instance.GetPipelineBinaryGlobalKey().keySize != 0,
                            capture_only)) {
        rasterizer->GetPipelineCache().StartOnDemand();
        return;
    }

    // tlg: by default the cached pipelines load while the game runs (a corner note shows the
    // progress). Otherwise rebuilding them takes up to minutes before the first game frame:
    // show what is happening instead of a black window.
    const char* background_warmup = std::getenv("TLG_BACKGROUND_WARMUP");
    if ((!background_warmup || *background_warmup != '0') &&
        rasterizer->GetPipelineCache().StartBackgroundWarmUp()) {
        return;
    }
    TlgOverlay::SetLoading(true, 0, 0);
    PresentLoadingFrame();
    auto last = std::chrono::steady_clock::now();
    rasterizer->GetPipelineCache().WarmUp([&](u32 done, u32 total) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(250)) {
            return;
        }
        last = now;
        TlgOverlay::SetLoading(true, done, total);
        PresentLoadingFrame();
    });
    TlgOverlay::SetLoading(false, 0, 0);

}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    fsr_frame_gen.reset();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        DestroyFgImage(&frame);
        device.destroyFence(frame.present_done);
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::DestroyFgImage(Frame* frame) {
    if (frame->fg_view) {
        instance.GetDevice().destroyImageView(frame->fg_view);
        frame->fg_view = nullptr;
    }
    if (frame->fg_image) {
        vmaDestroyImage(instance.GetAllocator(), frame->fg_image, frame->fg_allocation);
        frame->fg_image = nullptr;
    }
}

void Presenter::RecordFrameGeneration(Frame* frame) {
    if (fg_state < 0) {
        Dlss* dlss = Dlss::Get();
        fg_state = dlss && dlss->FgAvailable() ? 1 : 0;
        std::printf("Frame generation: %s\n", fg_state ? "available" : "not available");
        TlgSettings::ConfigureDlssFgSupport(fg_state != 0);
    }
    TemporalUpscaler::FgInputs inputs{};
    const bool have_inputs = rasterizer->GetUpscaler().TakeFgInputs(inputs);
    // Live switch. A change frees the old data; on starts without the old frames. A failed mode
    // stays off until the setting changes.
    // tlg: the setting as it may run now: off at a 30 FPS game rate (launch mode, limit, end
    // credits, slow motion scenes), see TlgSettings::EffectiveFrameGeneration.
    const int setting = TlgSettings::EffectiveFrameGeneration();
    if (setting != fg_failed) {
        fg_failed = 0;
    }
    const int mode = setting == fg_failed || (setting == 1 && !fg_state) ? 0 : setting;
    if (mode != fg_active) {
        static constexpr const char* names[] = {"off", "DLSS", "FSR 3.1"};
        std::printf("Frame generation: %s\n", names[mode]);
        fg_active = mode;
        fg_restart = mode != 0;
        if (fg_width || fsr_frame_gen) {
            draw_scheduler.EndRendering();
            draw_scheduler.Finish();
            if (fg_width) {
                Dlss::Get()->FgRelease();
                fg_width = 0;
            }
            fsr_frame_gen.reset();
        }
    }
    if (!have_inputs || !mode) {
        return;
    }
    // Storage writes need a format without sRGB: the default 10-bit frame qualifies.
    if (frame->format != vk::Format::eA2B10G10R10UnormPack32) {
        std::printf("Frame generation: off, needs the 10-bit frame format\n");
        fg_failed = mode;
        return;
    }
    if (!frame->fg_image) {
        const vk::ImageCreateInfo image_info = {
            .imageType = vk::ImageType::e2D,
            .format = frame->format,
            .extent = {frame->width, frame->height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc |
                     vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        };
        const VmaAllocationCreateInfo alloc_info = {
            .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };
        VkImage image{};
        const VkImageCreateInfo unsafe_info = static_cast<VkImageCreateInfo>(image_info);
        if (vmaCreateImage(instance.GetAllocator(), &unsafe_info, &alloc_info, &image,
                           &frame->fg_allocation, nullptr) != VK_SUCCESS) {
            std::printf("Frame generation: off, no memory for the generated image\n");
            fg_failed = mode;
            return;
        }
        frame->fg_image = vk::Image{image};
        frame->fg_view = Check<"create generated frame view">(instance.GetDevice().createImageView({
            .image = frame->fg_image,
            .viewType = vk::ImageViewType::e2D,
            .format = frame->format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    }
    draw_scheduler.EndRendering();
    Dlss* dlss = Dlss::Get();
    if (mode == 2) {
        if (!fsr_frame_gen) {
            fsr_frame_gen = std::make_unique<FsrFrameGen>(instance, draw_scheduler);
        }
    } else if (fg_width != frame->width || fg_height != frame->height ||
               fg_format != frame->format) {
        draw_scheduler.Finish();
        if (!dlss->FgCreate(draw_scheduler.CommandBuffer(), frame->width, frame->height,
                            frame->format)) {
            std::printf("Frame generation: off, NVIDIA refused to create it\n");
            fg_failed = mode;
            return;
        }
        fg_width = frame->width;
        fg_height = frame->height;
        fg_format = frame->format;
    }
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const std::array barriers{
        vk::ImageMemoryBarrier2{
            .srcStageMask = all,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = all,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = frame->image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = all,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = all,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = frame->fg_image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(barriers.size()),
                             .pImageMemoryBarriers = barriers.data()});
    const auto color = vk::ImageAspectFlagBits::eColor;
    const Dlss::FgFrame fg_frame{
        {frame->image, frame->image_view, frame->format, color, frame->width, frame->height},
        {inputs.depth, inputs.depth_view, inputs.depth_format, vk::ImageAspectFlagBits::eDepth,
         inputs.width, inputs.height},
        {inputs.motion, inputs.motion_view, inputs.motion_format, color, inputs.width,
         inputs.height},
        {frame->fg_image, frame->fg_view, frame->format, color, frame->width, frame->height},
        inputs.reset || fg_restart,
        frame->is_hdr,
    };
    if (mode == 2) {
        if (!fsr_frame_gen->Record(frame->image, frame->fg_image, frame->width, frame->height,
                                   inputs, inputs.reset || fg_restart, frame->is_hdr,
                                   fg_interval_ms)) {
            fg_failed = mode;
            return;
        }
    } else if (!dlss->FgEvaluate(cmdbuf, fg_frame)) {
        std::printf("Frame generation: off, evaluation failed\n");
        fg_failed = mode;
        return;
    }
    const vk::MemoryBarrier2 done{.srcStageMask = all,
                                  .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                  .dstStageMask = all,
                                  .dstAccessMask = vk::AccessFlagBits2::eMemoryRead};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
    frame->fg_ready = !inputs.reset && !fg_restart;
    fg_restart = false;
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    DestroyFgImage(frame);
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = FrameFormat(swapchain);
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
    frame->format = format;
    static vk::Format logged{};
    if (format != logged) {
        logged = format;
        std::printf("GPU: frame format %s\n",
                    format == vk::Format::eA2B10G10R10UnormPack32 ? "10-bit" : "8-bit");
    }
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        ASSERT_CODE_MSG(result != vk::Result::eErrorDeviceLost, 25,
                    "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

// tlg: raw copy of an image for TLG_FRAME_DUMP_TRIGGER, written once the GPU is done.
static void DumpRaw(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                    vk::Image image, vk::ImageLayout layout, u32 w, u32 h, const char* name,
                    int index, u32 bytes_per_pixel = 4) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * bytes_per_pixel,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    cmdbuf.copyImageToBuffer(image, layout, buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {w, h, 1}});
    const char* dir = std::getenv("TLG_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s_%03d_%ux%u.raw", dir && *dir ? dir : "out/dump", name,
                  index, w, h);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      size = buffer_ci.size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

// tlg: photo mode screenshot. The frame image (in TransferSrcOptimal) is copied to a host buffer;
// once the GPU is done, a worker thread turns it into 8-bit RGB and writes a lossless PNG to
// <user>/screenshots (TLG_GPU_USER_DIR, else "user"), so the frame itself never waits on the encode.
void Presenter::SaveScreenshot(vk::CommandBuffer cmdbuf, Scheduler& scheduler, vk::Image image,
                               u32 w, u32 h, vk::Format format) {
    const bool ten_bit = format == vk::Format::eA2B10G10R10UnormPack32 ||
                         format == vk::Format::eA2R10G10B10UnormPack32;
    const bool bgr = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb ||
                     format == vk::Format::eA2R10G10B10UnormPack32;
    if (!ten_bit && !bgr && format != vk::Format::eR8G8B8A8Unorm &&
        format != vk::Format::eR8G8B8A8Srgb) {
        std::printf("Screenshot: frame format %s not supported\n", vk::to_string(format).c_str());
        TlgOverlay::Notice("Screenshot failed", 3.0f);
        return;
    }
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        TlgOverlay::Notice("Screenshot failed", 3.0f);
        return;
    }
    cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {w, h, 1}});
    const char* user = std::getenv("TLG_GPU_USER_DIR");
    const std::filesystem::path dir =
        std::filesystem::path(user && user[0] ? user : "user") / "screenshots";
    const std::time_t now = std::time(nullptr);
    char stamp[40];
    std::strftime(stamp, sizeof(stamp), "TLG-%Y%m%d-%H%M%S", std::localtime(&now));
    static int count = 0;
    const std::filesystem::path file =
        dir / (std::string(stamp) + "-" + std::to_string(++count) + ".png");
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      w, h, ten_bit, bgr, dir, file] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        std::vector<u32> pixels(size_t(w) * h);
        std::memcpy(pixels.data(), info.pMappedData, pixels.size() * 4);
        vmaDestroyBuffer(allocator, buffer, allocation);
        std::thread([pixels = std::move(pixels), w, h, ten_bit, bgr, dir, file] {
            std::vector<u8> rgb(size_t(w) * h * 3);
            for (size_t i = 0; i < pixels.size(); ++i) {
                const u32 p = pixels[i];
                u32 r, g, b;
                if (ten_bit) { // 10 bits per channel, red in the low bits (B and R swapped for A2R10)
                    r = ((p >> 0) & 1023) >> 2;
                    g = ((p >> 10) & 1023) >> 2;
                    b = ((p >> 20) & 1023) >> 2;
                } else {
                    r = p & 255;
                    g = (p >> 8) & 255;
                    b = (p >> 16) & 255;
                }
                if (bgr) {
                    std::swap(r, b);
                }
                rgb[i * 3 + 0] = u8(r);
                rgb[i * 3 + 1] = u8(g);
                rgb[i * 3 + 2] = u8(b);
            }
            size_t size = 0;
            void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), int(w), int(h), 3,
                                                                   &size, 6, MZ_FALSE);
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            bool ok = false;
            if (png) {
                std::ofstream out(file, std::ios::binary);
                out.write(static_cast<const char*>(png), std::streamsize(size));
                ok = bool(out);
                mz_free(png);
            }
            std::printf("Screenshot: %s %ux%u %s\n", ok ? "saved" : "FAILED", w, h,
                        file.string().c_str());
            std::fflush(stdout);
            TlgOverlay::Notice(ok ? "Screenshot saved (user/screenshots)" : "Screenshot failed", 3.0f);
        }).detach();
    });
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    // tlg: TLG_FRAME_DUMP_TRIGGER=<file>: when the file exists it is removed and this frame's
    // guest display buffer and presented image are written to TLG_DUMP_DIR (default out/dump)
    // as raw 32-bit pixels (menus and movies included; the upscaler dumps scene frames only).
    static const char* frame_dump_trigger = std::getenv("TLG_FRAME_DUMP_TRIGGER");
    static int frame_dump_index = 0;
    const bool frame_dump = frame_dump_trigger && std::remove(frame_dump_trigger) == 0;
    // tlg: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = display.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    } else {
        auto& image = texture_cache.GetImage(image_id);
        if (frame_dump) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(),
                    vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                    image.info.size.height, "display", frame_dump_index);
        }
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    // tlg: FSR 1, RCAS and its strength come from the in-game menu, every frame.
    TlgSettings::ReloadIfRequested();
    const auto& graphics = TlgSettings::Get();
    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    // tlg: TLG_POSTFX_PROFILE=1 times each filter on the final image.
    const bool timed = postfx_timer.Enabled();
    if (timed) {
        postfx_timer.Begin(cmdbuf);
    }
    // tlg: SMAA 1x on the game's image (its UI included: the game draws it into the same buffer),
    // at the game's resolution, before any scaling.
    vk::Image filtered_image{}; // tlg: the last filter's RGBA16F output, for the frame dump
    if (graphics.smaa) {
        image_view = smaa_pass.Render(cmdbuf, draw_scheduler, image_view, image_size,
                                      !pp_settings.srgb_input);
        filtered_image = smaa_pass.OutputImage();
    }
    // One Sharpening setting (2026-11-07): the AA step's RCAS when DLSS / FSR / TAA ran on this
    // frame, else CAS here on the final picture (Built-in). FSR 1 upscales without its RCAS.
    const bool aa_sharpened = TlgSettings::AaSharpenedRecently();
    const bool use_cas = TlgSettings::Get().sharpen && !aa_sharpened;
    {
        // tlg: one log line whenever the sharpener or its strength changes (user check 11-07).
        static std::string last_sharpening;
        const int pct = int(std::lround(TlgSettings::Get().sharpness.load() * 100.0f));
        char text[128];
        if (!TlgSettings::Get().sharpen) {
            std::snprintf(text, sizeof(text), "off");
        } else if (aa_sharpened) {
            std::snprintf(text, sizeof(text), "%s in the AA step, %d%%, no CAS",
                          TlgSettings::AaSharpenerName(), pct);
        } else {
            std::snprintf(text, sizeof(text), "CAS on the final picture, %d%% (CAS %d%%)", pct,
                          std::min(pct, 100));
        }
        if (last_sharpening != text) {
            last_sharpening = text;
            std::printf("Sharpening: %s\n", text);
        }
    }
    if (timed) {
        postfx_timer.Stamp(cmdbuf, HostPasses::PostFxTimer::Smaa);
    }
    fsr_settings.enable = graphics.fsr1;
    fsr_settings.use_rcas = false;
    TlgSettings::Get().image_width = int(image_size.width);
    TlgSettings::Get().image_height = int(image_size.height);
    TlgSettings::Get().window_width = int(frame->width);
    TlgSettings::Get().window_height = int(frame->height);
    const bool fsr_runs =
        TlgGraphics::FsrPassRuns(fsr_settings.enable, fsr_settings.use_rcas, int(image_size.width),
                                 int(image_size.height), int(frame->width), int(frame->height));
    TlgSettings::Get().rcas_applied = use_cas;
    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr);
    if (timed) {
        postfx_timer.Stamp(cmdbuf, HostPasses::PostFxTimer::FsrRcas);
    }
    // The FSR pass outputs at the window's size.
    const vk::Extent2D filtered_size = fsr_runs ? vk::Extent2D{frame->width, frame->height} : image_size;
    if (use_cas) {
        image_view = cas_pass.Render(cmdbuf, draw_scheduler, image_view, filtered_size,
                                     TlgGraphics::CasFromSharpness(TlgSettings::Get().sharpness));
        filtered_image = cas_pass.OutputImage();
    }
    if (frame_dump && filtered_image && !fsr_runs) {
        // SMAA / CAS output at the game's resolution before scaling: "filtered_*", RGBA16F.
        const auto transfer = [&](vk::ImageLayout from, vk::ImageLayout to) {
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
                .oldLayout = from,
                .newLayout = to,
                .image = filtered_image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        };
        transfer(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal);
        DumpRaw(instance, draw_scheduler, cmdbuf, filtered_image,
                vk::ImageLayout::eTransferSrcOptimal, filtered_size.width, filtered_size.height,
                "filtered", frame_dump_index, 8);
        transfer(vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
    }

    // tlg: the color adjustments run in the post process pass (no extra pass).
    pp_settings.color_adjust = graphics.color_adjust && !frame->is_hdr ? 1u : 0u;
    pp_settings.brightness = graphics.brightness;
    pp_settings.contrast = graphics.contrast;
    pp_settings.saturation = graphics.saturation;
    pp_settings.user_gamma = graphics.color_gamma;
    if (timed) {
        postfx_timer.Stamp(cmdbuf, HostPasses::PostFxTimer::Cas);
    }
    (frame->format == vk::Format::eA2B10G10R10UnormPack32 ? pp_pass : pp_pass_8bit)
        .Render(cmdbuf, image_view, image_size, *frame, pp_settings);
    if (timed) {
        postfx_timer.Stamp(cmdbuf, HostPasses::PostFxTimer::PostProcess);
        postfx_timer.End(graphics.smaa, use_cas, graphics.color_adjust);
    }
    if (frame_dump) {
        // The presented image as the swapchain blit reads it (General after the pass).
        const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                      .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        DumpRaw(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                frame->width, frame->height, "output", frame_dump_index);
        ++frame_dump_index;
    }

    RecordFrameGeneration(frame);

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);

    // tlg: the GPU command thread runs at most TLG_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    // The in-game menu changes it live (TlgSettings::frames_queued; TLG_FRAMES_AHEAD at start).
    const u32 frames_ahead = u32(std::max(0, TlgSettings::Get().frames_queued.load()));
    if (!frames_ahead) {
        recent_frame_ticks.clear();
    } else {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::PresentLoadingFrame() {
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        return;
    }
    swapchain.UpdateVsync();
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    if (!swapchain.AcquireNextImage()) {
        return;
    }
    const vk::Image image = swapchain.Image();
    const vk::Extent2D extent = swapchain.GetExtent();
    const auto cmdbuf = present_scheduler.CommandBuffer();
    const vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };
    const auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to, vk::AccessFlags src,
                             vk::AccessFlags dst) {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
                               vk::ImageMemoryBarrier{
                                   .srcAccessMask = src,
                                   .dstAccessMask = dst,
                                   .oldLayout = from,
                                   .newLayout = to,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange = range,
                               });
    };
    barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {},
            vk::AccessFlagBits::eTransferWrite);
    cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal,
                           vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}},
                           range);
    barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal,
            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eColorAttachmentWrite);
    TlgOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
    barrier(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR,
            vk::AccessFlagBits::eColorAttachmentWrite, {});
    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    present_scheduler.Flush(info);
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        swapchain.Present();
    }
    // A few frames a second during start-up: waiting keeps semaphores and the swapchain simple.
    present_scheduler.Finish();
}

bool Presenter::PresentImage(Frame* frame, vk::Image source, bool last) {
    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            return false;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    if (last) {
        const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
        ASSERT_MSG(reset_result == vk::Result::eSuccess,
                   "Unexpected error resetting present done fence: {}",
                   vk::to_string(reset_result));
    }

    // tlg: the game frame is blitted (letterboxed) straight into the swapchain image.
    const vk::Image swapchain_image = swapchain.Image();
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source,
                .subresourceRange = color_range,
            },
        };
        // tlg: the fit rect covers the whole swapchain image when the window and the game share
        // the aspect (the common case): the black clear then writes only pixels the blit
        // overwrites, so it and its barrier are skipped. A blit that lands texel for texel is
        // exact with nearest filtering (and cheaper) instead of linear.
        const vk::Rect2D fit = FitImage(frame->width, frame->height, extent.width, extent.height);
        const bool full = fit.offset.x == 0 && fit.offset.y == 0 &&
                          fit.extent.width == extent.width && fit.extent.height == extent.height;
        const bool one_to_one = fit.extent.width == u32(frame->width) &&
                                fit.extent.height == u32(frame->height);
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        if (!(full && SkipPresentClear())) {
            const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
            cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black, color_range);
            const vk::MemoryBarrier clear_done{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        }
        cmdbuf.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlit(frame->width, frame->height, fit.extent.width,
                                       fit.extent.height, fit.offset.x, fit.offset.y),
                         one_to_one ? vk::Filter::eNearest : vk::Filter::eLinear);
        // tlg: photo mode screenshot: the finished frame at render resolution, before our
        // overlay (menu, FPS, notices) is drawn over it.
        if (TlgOverlay::TakeScreenshotRequest()) {
            SaveScreenshot(cmdbuf, scheduler, source, u32(frame->width), u32(frame->height),
                           FrameFormat(swapchain));
        }
        // tlg: the settings menu / FPS counter over the frame, at display resolution.
        const bool overlay = TlgOverlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            TlgOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    if (last) {
        info.AddSignal(frame->present_done);
    }
    scheduler.Flush(info);

    // Present to swapchain.
    bool recreate = false;
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        recreate = !swapchain.Present() && window.GetWidth() != 0 && window.GetHeight() != 0;
    }
    if (recreate) {
        // Out of date: rebuild outside the present lock (Create takes the queue lock itself).
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    return true;
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // tlg: a minimized window has no pixels on Windows (0x0); keep the swapchain and skip
    // presenting until the window is restored.
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        free_frame();
        return;
    }

    // Recreate the swapchain if the window was resized.
    swapchain.UpdateVsync();
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    // tlg: frame generation: the generated image now, the real frame half a frame later.
    const auto flip_time = std::chrono::steady_clock::now();
    if (frame->fg_ready && !is_reusing_frame) {
        const float gap_ms =
            std::chrono::duration<float, std::milli>(flip_time - fg_last_flip).count();
        if (gap_ms > 0.0f && gap_ms < 100.0f) {
            fg_interval_ms += (gap_ms - fg_interval_ms) * 0.1f;
        }
        if (PresentImage(frame, frame->fg_image, false)) {
            const auto target = std::chrono::steady_clock::now() +
                                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<float, std::milli>(fg_interval_ms * 0.5f));
            while (std::chrono::steady_clock::now() < target) {
                std::this_thread::yield();
            }
            if (++fg_presented % 600 == 0) {
                std::printf("Frame generation: %llu generated, %.1f ms per game frame\n",
                            static_cast<unsigned long long>(fg_presented), fg_interval_ms);
            }
        }
    }
    if (!is_reusing_frame) {
        fg_last_flip = flip_time;
    }

    if (!PresentImage(frame, frame->image, true)) {
        free_frame();
        return;
    }

    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    frame->fg_ready = false;

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        ASSERT_CODE_MSG(result != vk::Result::eErrorDeviceLost, 25,
                    "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR() || frame->format != FrameFormat(swapchain)) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // no surface (minimized window): keep the last frame size
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
