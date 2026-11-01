// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_fsr_frame_gen.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <array>
#include <cstdio>

#include <vk_mem_alloc.h>

#include "ffx_vk_fsr3_3_1_5_bridge.h"

namespace Vulkan {

namespace {

constexpr auto ColorFormat = vk::Format::eR16G16B16A16Sfloat;
constexpr auto DepthFormat = vk::Format::eR32Sfloat;
constexpr vk::ImageUsageFlags ColorUsage =
    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
constexpr vk::ImageUsageFlags ResultUsage = vk::ImageUsageFlagBits::eStorage |
                                            vk::ImageUsageFlagBits::eSampled |
                                            vk::ImageUsageFlagBits::eTransferSrc;

void FullBarrier(vk::CommandBuffer cmdbuf) {
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const vk::MemoryBarrier2 barrier{.srcStageMask = all,
                                     .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                     .dstStageMask = all,
                                     .dstAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                                      vk::AccessFlagBits2::eMemoryWrite};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

void Blit(vk::CommandBuffer cmdbuf, vk::Image src, vk::Image dst, u32 width, u32 height) {
    const vk::ImageSubresourceLayers layers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    const std::array<vk::Offset3D, 2> area{vk::Offset3D{0, 0, 0},
                                           vk::Offset3D{s32(width), s32(height), 1}};
    const vk::ImageBlit region{
        .srcSubresource = layers, .srcOffsets = area, .dstSubresource = layers, .dstOffsets = area};
    cmdbuf.blitImage(src, vk::ImageLayout::eGeneral, dst, vk::ImageLayout::eGeneral, region,
                     vk::Filter::eNearest);
}

FfxVkFsr3_3_1_6FrameGenerationImage Describe(vk::Image image, vk::Format format, u32 width,
                                             u32 height, vk::ImageUsageFlags usage) {
    FfxVkFsr3_3_1_6FrameGenerationImage info{};
    info.image = image;
    info.format = static_cast<VkFormat>(format);
    info.width = width;
    info.height = height;
    info.layout = VK_IMAGE_LAYOUT_GENERAL;
    info.usage = static_cast<VkImageUsageFlags>(usage);
    return info;
}

} // namespace

FsrFrameGen::FsrFrameGen(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

FsrFrameGen::~FsrFrameGen() {
    Destroy();
}

bool FsrFrameGen::MakeImage(Image& image, vk::Format format, u32 width, u32 height,
                            vk::ImageUsageFlags usage) {
    const VkImageCreateInfo info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = usage,
    });
    const VmaAllocationCreateInfo alloc_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage handle{};
    if (vmaCreateImage(instance.GetAllocator(), &info, &alloc_info, &handle, &image.allocation,
                       nullptr) != VK_SUCCESS) {
        return false;
    }
    image.image = vk::Image{handle};
    return true;
}

bool FsrFrameGen::Create(u32 width, u32 height, u32 new_render_width, u32 new_render_height) {
    display_width = width;
    display_height = height;
    render_width = new_render_width;
    render_height = new_render_height;
    if (!MakeImage(color, ColorFormat, width, height, ColorUsage) ||
        !MakeImage(result, ColorFormat, width, height, ResultUsage) ||
        !MakeImage(depth, DepthFormat, render_width, render_height, ColorUsage)) {
        return false;
    }
    const VkBufferCreateInfo buffer_info = static_cast<VkBufferCreateInfo>(vk::BufferCreateInfo{
        .size = vk::DeviceSize{render_width} * render_height * 4,
        .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
    });
    const VmaAllocationCreateInfo alloc_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer buffer{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_info, &alloc_info, &buffer,
                        &depth_buffer_allocation, nullptr) != VK_SUCCESS) {
        return false;
    }
    depth_buffer = vk::Buffer{buffer};
    FfxVkFsr3_3_1_6FrameGenerationCreateInfo info{};
    info.physicalDevice = instance.GetPhysicalDevice();
    info.device = instance.GetDevice();
    info.maxRenderWidth = render_width;
    info.maxRenderHeight = render_height;
    info.displayWidth = width;
    info.displayHeight = height;
    info.colorFormat = static_cast<VkFormat>(ColorFormat);
    return ffxVkFsr3_3_1_6FrameGenerationContextCreate(&info, &context) ==
           FFX_VK_FSR3_3_1_6_FRAMEGEN_OK;
}

void FsrFrameGen::Destroy() {
    if (context) {
        ffxVkFsr3_3_1_6FrameGenerationContextDestroy(context);
        context = nullptr;
    }
    submitted.clear();
    for (Image* image : {&color, &depth, &result}) {
        if (image->image) {
            vmaDestroyImage(instance.GetAllocator(), image->image, image->allocation);
            *image = {};
        }
    }
    if (depth_buffer) {
        vmaDestroyBuffer(instance.GetAllocator(), depth_buffer, depth_buffer_allocation);
        depth_buffer = nullptr;
    }
    display_width = 0;
}

bool FsrFrameGen::Record(vk::Image frame, vk::Image output, u32 width, u32 height,
                         const TemporalUpscaler::FgInputs& inputs, bool reset, bool hdr,
                         float frame_ms) {
    if (inputs.depth_format != vk::Format::eD32Sfloat &&
        inputs.depth_format != vk::Format::eD32SfloatS8Uint) {
        std::printf("Frame generation: FSR needs 32-bit depth, the game has %s\n",
                    vk::to_string(inputs.depth_format).c_str());
        return false;
    }
    if (display_width != width || display_height != height || render_width != inputs.width ||
        render_height != inputs.height) {
        scheduler.Finish();
        Destroy();
        if (!Create(width, height, inputs.width, inputs.height)) {
            std::printf("Frame generation: FSR could not be created\n");
            Destroy();
            return false;
        }
        reset = true;
    }
    // Imported images stay alive until the GPU is past the frame that used them.
    while (!submitted.empty() && scheduler.IsFree(submitted.front().tick)) {
        ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(context, submitted.front().frame_id);
        submitted.pop_front();
    }

    const auto cmdbuf = scheduler.CommandBuffer();
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto to_general = [&](vk::Image image) {
        return vk::ImageMemoryBarrier2{
            .srcStageMask = all,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = all,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    };
    const std::array barriers{to_general(color.image), to_general(depth.image),
                              to_general(result.image)};
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(barriers.size()),
                             .pImageMemoryBarriers = barriers.data()});
    Blit(cmdbuf, frame, color.image, width, height);
    // A depth image can't be copied to a color image directly; a buffer between them can.
    const vk::BufferImageCopy depth_from{
        .imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
        .imageExtent = {inputs.width, inputs.height, 1},
    };
    cmdbuf.copyImageToBuffer(inputs.depth, vk::ImageLayout::eGeneral, depth_buffer, depth_from);
    FullBarrier(cmdbuf);
    const vk::BufferImageCopy depth_to{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {inputs.width, inputs.height, 1},
    };
    cmdbuf.copyBufferToImage(depth_buffer, depth.image, vk::ImageLayout::eGeneral, depth_to);
    FullBarrier(cmdbuf);

    const auto transfer = hdr ? FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_PQ
                              : FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB;
    const float max_luminance = hdr ? 1000.0f : 1.0f;
    constexpr float CameraFar = 3000.0f;
    const auto color_info = Describe(color.image, ColorFormat, width, height, ColorUsage);
    const u64 id = ++frame_id;

    FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prepare{};
    prepare.commandBuffer = cmdbuf;
    prepare.color = color_info;
    prepare.depth = Describe(depth.image, DepthFormat, inputs.width, inputs.height, ColorUsage);
    prepare.motionVectors =
        Describe(inputs.motion, inputs.motion_format, inputs.width, inputs.height,
                 vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);
    prepare.renderWidth = inputs.width;
    prepare.renderHeight = inputs.height;
    prepare.jitterOffsetX = inputs.jitter_x;
    prepare.jitterOffsetY = inputs.jitter_y;
    prepare.motionVectorScaleX = 1.0f;
    prepare.motionVectorScaleY = 1.0f;
    prepare.frameTimeMilliseconds = frame_ms;
    prepare.minLuminance = 0.0f;
    prepare.maxLuminance = max_luminance;
    prepare.transferFunction = transfer;
    prepare.cameraNear = inputs.camera_near;
    prepare.cameraFar = CameraFar;
    prepare.viewSpaceToMeters = 1.0f;
    prepare.cameraVerticalFovRadians = inputs.camera_fov;
    prepare.cameraUp[1] = 1.0f;
    prepare.cameraRight[0] = 1.0f;
    prepare.cameraForward[2] = -1.0f;
    prepare.frameId = id;
    prepare.reset = reset ? VK_TRUE : VK_FALSE;
    if (const auto code = ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(context, &prepare);
        code != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        std::printf("Frame generation: FSR prepare failed (%d)\n", int(code));
        return false;
    }

    FfxVkFsr3_3_1_6FrameGenerationDispatchInfo dispatch{};
    dispatch.commandBuffer = cmdbuf;
    dispatch.color = color_info;
    dispatch.output = Describe(result.image, ColorFormat, width, height, ResultUsage);
    dispatch.displayWidth = width;
    dispatch.displayHeight = height;
    dispatch.frameTimeMilliseconds = frame_ms;
    dispatch.cameraNear = inputs.camera_near;
    dispatch.cameraFar = CameraFar;
    dispatch.viewSpaceToMeters = 1.0f;
    dispatch.cameraVerticalFovRadians = inputs.camera_fov;
    dispatch.minLuminance = 0.0f;
    dispatch.maxLuminance = max_luminance;
    dispatch.transferFunction = transfer;
    dispatch.frameId = id;
    dispatch.reset = reset ? VK_TRUE : VK_FALSE;
    if (const auto code = ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context, &dispatch);
        code != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        std::printf("Frame generation: FSR dispatch failed (%d)\n", int(code));
        return false;
    }
    submitted.push_back({id, scheduler.CurrentTick()});
    FullBarrier(cmdbuf);
    Blit(cmdbuf, result.image, output, width, height);
    return true;
}

} // namespace Vulkan
