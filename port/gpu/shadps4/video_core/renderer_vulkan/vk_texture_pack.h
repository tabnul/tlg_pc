// SPDX-FileCopyrightText: Copyright 2026 TLG PC port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
struct ImageViewInfo;
}

namespace Vulkan {

class Instance;
class Scheduler;
class StagingBufferPool;

/// tlg: host-side texture pack. Pack folders (texpack/<name>/index.txt, built by
/// ps4-texturepack-generator) map the XXH3 of a texture's tiled mip 0 in guest memory to a BC7 DDS with a
/// full mip chain. Sampled-only bindings of a matching image read the pack image instead; guest
/// memory and the game files stay untouched. Settings texture_pack picks the pack (0 = off); a
/// switch drops the loaded images, which load again on demand on a worker thread.
class TexturePack {
public:
    TexturePack(const Instance&, Scheduler&, StagingBufferPool&);
    ~TexturePack();

    /// Key of `image` for the pack lookup (0: never replaced).
    static u64 Key(const VideoCore::Image& image);

    /// Once per frame: applies the setting and uploads finished loads.
    void Tick();
    /// The pack view for a sampled-only binding, or null (no entry, loading, incompatible view).
    vk::ImageView Sample(VideoCore::Image& image, const VideoCore::ImageViewInfo& info);
    /// Marks the pack image of `image` as in use (a memoized binding skipped Sample).
    void Touch(const VideoCore::Image& image);
    /// Changes when views returned by Sample change (pack switched, new image ready).
    u64 Generation() const {
        return generation;
    }

private:
    struct Entry {
        std::string path;
        int pack = 0;
        u32 width = 0, height = 0;
        enum class State { Unloaded, Loading, Ready, Failed } state = State::Unloaded;
        std::vector<u8> file;
        VideoCore::UniqueImage image;
        u32 levels = 0;
        vk::Format format = vk::Format::eBc7UnormBlock; ///< BC5 for normal maps
        u64 bytes = 0;
        u32 last_used = 0; ///< `now` of the last binding; unused images are freed after a while
        std::vector<std::pair<std::pair<bool, vk::ComponentMapping>, vk::UniqueImageView>> views;
    };
    void Switch(int pack);
    void Upload(Entry& entry);
    void Worker(std::stop_token stop);
    void Evict();

    const Instance& instance;
    Scheduler& scheduler;
    StagingBufferPool& staging;
    int active = 0;
    u64 generation = 1;
    u64 resident_bytes = 0;
    u64 hits = 0, misses = 0, refused = 0, last_report = 0;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    u32 now = 0; ///< seconds since start, updated by Tick
    std::unordered_map<u64, std::shared_ptr<Entry>> entries;
    /// Pack images only: freed ones empty whole blocks, which VMA returns to the driver (in the
    /// default pools they left holes in blocks the game still uses). Large images get their own
    /// memory, released at once.
    VmaPool pool = VK_NULL_HANDLE;

    std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<std::shared_ptr<Entry>> queue, done;
    std::jthread worker;
};

} // namespace Vulkan
