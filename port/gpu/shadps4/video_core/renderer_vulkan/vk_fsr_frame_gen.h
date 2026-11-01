// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>

#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"

struct FfxVkFsr3_3_1_6FrameGenerationContext;
struct VmaAllocation_T;

namespace Vulkan {

class Instance;
class Scheduler;

/// tlg: AMD FSR 3 frame generation, through UniversalScalers-Vulkan by @FireBurn. It only takes 16-bit color and
/// float depth, so the frame and the depth pass through copies owned here.
class FsrFrameGen {
public:
    FsrFrameGen(const Instance& instance, Scheduler& scheduler);
    /// The GPU must be done with its work: call scheduler.Finish() first.
    ~FsrFrameGen();

    /// Records the image between the last frame and this one into output. Both stay in General.
    bool Record(vk::Image frame, vk::Image output, u32 width, u32 height,
                const TemporalUpscaler::FgInputs& inputs, bool reset, bool hdr, float frame_ms);

private:
    struct Image {
        vk::Image image{};
        VmaAllocation_T* allocation{};
    };
    bool Create(u32 width, u32 height, u32 new_render_width, u32 new_render_height);
    void Destroy();
    bool MakeImage(Image& image, vk::Format format, u32 width, u32 height,
                   vk::ImageUsageFlags usage);

    const Instance& instance;
    Scheduler& scheduler;
    FfxVkFsr3_3_1_6FrameGenerationContext* context{};
    Image color{}, depth{}, result{};
    vk::Buffer depth_buffer{};
    VmaAllocation_T* depth_buffer_allocation{};
    u32 display_width{}, display_height{}, render_width{}, render_height{};
    u64 frame_id{};
    struct Submitted {
        u64 frame_id;
        u64 tick;
    };
    std::deque<Submitted> submitted;
};

} // namespace Vulkan
