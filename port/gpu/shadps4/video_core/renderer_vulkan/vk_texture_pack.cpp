// SPDX-FileCopyrightText: Copyright 2026 TLG PC port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unordered_set>
#include <fstream>
#include <xxhash.h>
#include "tlg_settings.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_texture_pack.h"
#include "video_core/texture_cache/image_view.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

namespace {

constexpr u64 Budget = 16ull << 30; ///< host VRAM for pack images; later loads are refused
constexpr u64 UploadPerFrame = 256ull << 20;
constexpr u32 MaxLevels = 14;
constexpr u32 EvictSeconds = 30; ///< pack images not bound for this long are freed

bool IsSrgb(vk::Format format) {
    switch (format) {
    case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc2SrgbBlock:
    case vk::Format::eBc3SrgbBlock:
    case vk::Format::eBc7SrgbBlock:
        return true;
    default:
        return false;
    }
}

bool IsColorBc(vk::Format format) {
    switch (format) {
    case vk::Format::eBc1RgbUnormBlock:
    case vk::Format::eBc1RgbaUnormBlock:
    case vk::Format::eBc2UnormBlock:
    case vk::Format::eBc3UnormBlock:
    case vk::Format::eBc7UnormBlock:
        return true;
    default:
        return IsSrgb(format);
    }
}

/// Formats a pack image keeps as the game has them (no sRGB view): BC5 normal maps, BC4 single channel.
bool IsDataBc(vk::Format format) {
    return format == vk::Format::eBc5SnormBlock || format == vk::Format::eBc5UnormBlock ||
           format == vk::Format::eBc4UnormBlock;
}

// DDS from ps4-texturepack-generator pack_encode.py (texconv): DX10 header, BC7 (color), BC5 (normal maps) or BC4
// (single channel), mips packed from level 0.
constexpr size_t DdsHeader = 4 + 124 + 20;
constexpr u32 DxgiBc4Unorm = 80, DxgiBc5Unorm = 83, DxgiBc5Snorm = 84;

} // namespace

TexturePack::TexturePack(const Instance& instance_, Scheduler& scheduler_,
                         StagingBufferPool& staging_)
    : instance{instance_}, scheduler{scheduler_}, staging{staging_},
      worker{[this](std::stop_token stop) { Worker(stop); }} {
    const VkImageCreateInfo sample{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                   .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                                   .imageType = VK_IMAGE_TYPE_2D,
                                   .format = VK_FORMAT_BC7_UNORM_BLOCK,
                                   .extent = {256, 256, 1},
                                   .mipLevels = 1,
                                   .arrayLayers = 1,
                                   .samples = VK_SAMPLE_COUNT_1_BIT,
                                   .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VmaPoolCreateInfo pool_ci{};
    if (vmaFindMemoryTypeIndexForImageInfo(instance.GetAllocator(), &sample, &alloc,
                                           &pool_ci.memoryTypeIndex) != VK_SUCCESS ||
        vmaCreatePool(instance.GetAllocator(), &pool_ci, &pool) != VK_SUCCESS) {
        pool = VK_NULL_HANDLE; // the default pools still work, only without giving memory back
    }
}

TexturePack::~TexturePack() {
    worker.request_stop();
    wake.notify_all();
    if (pool) {
        scheduler.Finish(); // the GPU is done with every pack image
        scheduler.PopPendingOperations(); // runs the deferred frees of evicted ones
        entries.clear();
        std::scoped_lock lock{mutex};
        queue.clear();
        done.clear();
        vmaDestroyPool(instance.GetAllocator(), pool);
    }
}

u64 TexturePack::Key(const VideoCore::Image& image) {
    const auto& info = image.info;
    if (!info.props.is_block || info.props.is_volume || info.resources.layers != 1 ||
        info.num_samples > 1 || !info.guest_address ||
        !(IsColorBc(info.pixel_format) || IsDataBc(info.pixel_format))) {
        return 0;
    }
    const u32 size = info.mips_layout[0].size;
    if (size == 0 || size > info.guest_size) {
        return 0;
    }
    return XXH3_64bits(reinterpret_cast<const void*>(info.guest_address), size);
}

void TexturePack::Switch(int pack) {
    // Images still in flight are released once the GPU reaches the current tick.
    scheduler.DeferOperation([old = std::make_shared<decltype(entries)>(std::move(entries))] {});
    entries = {};
    {
        std::scoped_lock lock{mutex};
        queue.clear();
        done.clear();
    }
    resident_bytes = 0;
    hits = misses = refused = 0;
    last_report = now;
    active = pack;
    ++generation;
    const std::string dir = TlgSettings::TexturePackDir(pack);
    if (dir.empty()) {
        std::printf("Texture pack: off\n");
        return;
    }
    std::ifstream index{dir + "/index.txt"};
    std::string line;
    while (std::getline(index, line)) {
        unsigned long long key = 0;
        int path_start = 0;
        auto entry = std::make_shared<Entry>();
        // The path is the rest of the line: some game file names contain spaces.
        if (std::sscanf(line.c_str(), "%llx %u %u %n", &key, &entry->width, &entry->height,
                        &path_start) == 3 &&
            path_start > 0 && size_t(path_start) < line.size()) {
            entry->path = dir + "/" + line.substr(path_start);
            if (entry->path.back() == '\r') {
                entry->path.pop_back();
            }
            entry->pack = pack;
            entries.emplace(key, std::move(entry));
        }
    }
    std::printf("Texture pack: %s, %zu entries\n", dir.c_str(), entries.size());
}

void TexturePack::Worker(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::shared_ptr<Entry> entry;
        {
            std::unique_lock lock{mutex};
            wake.wait(lock, stop, [this] { return !queue.empty(); });
            if (queue.empty()) {
                continue;
            }
            entry = std::move(queue.front());
            queue.pop_front();
        }
        std::ifstream file{entry->path, std::ios::binary | std::ios::ate};
        if (file) {
            entry->file.resize(size_t(file.tellg()));
            file.seekg(0);
            file.read(reinterpret_cast<char*>(entry->file.data()), entry->file.size());
        }
        std::scoped_lock lock{mutex};
        done.push_back(std::move(entry));
    }
}

void TexturePack::Tick() {
    const int pack = TlgSettings::Get().texture_pack.load();
    if (pack != active) {
        Switch(pack);
    }
    const u32 seconds = u32(std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - start)
                                .count());
    if (seconds != now) {
        now = seconds;
        Evict();
        if (now % 10 == 0 && std::getenv("TLG_TEXPACK_VMA")) {
            // Whether freed pack images give memory back or VMA keeps the blocks.
            VmaTotalStatistics stats{};
            vmaCalculateStatistics(instance.GetAllocator(), &stats);
            std::printf("Texture pack: VMA %llu MB allocated in %llu MB blocks, pack %llu MB\n",
                        (unsigned long long)(stats.total.statistics.allocationBytes >> 20),
                        (unsigned long long)(stats.total.statistics.blockBytes >> 20),
                        (unsigned long long)(resident_bytes >> 20));
        }
    }
    std::deque<std::shared_ptr<Entry>> ready;
    {
        std::scoped_lock lock{mutex};
        u64 bytes = 0;
        while (!done.empty() && bytes < UploadPerFrame) {
            if (done.front()->pack == active) { // loads of a pack switched away are dropped
                bytes += done.front()->file.size();
                ready.push_back(std::move(done.front()));
            }
            done.pop_front();
        }
    }
    for (auto& entry : ready) {
        Upload(*entry);
    }
    if (!ready.empty()) {
        ++generation;
    }
    // Once a minute: the counts are for comparing packs, not for following every frame.
    if (active && now >= last_report + 60) {
        last_report = now;
        std::printf("Texture pack: %llu sampled from the pack, %llu without entry, %llu refused, "
                    "%llu MB resident\n",
                    (unsigned long long)hits, (unsigned long long)misses,
                    (unsigned long long)refused, (unsigned long long)(resident_bytes >> 20));
    }
}

void TexturePack::Evict() {
    bool evicted = false;
    for (auto& [key, entry] : entries) {
        if (entry->state != Entry::State::Ready || now - entry->last_used < EvictSeconds) {
            continue;
        }
        // Freed once the GPU is done with the frames that still sample it; loads again on use.
        scheduler.DeferOperation(
            [old = std::make_shared<std::pair<VideoCore::UniqueImage, decltype(entry->views)>>(
                 std::move(entry->image), std::move(entry->views))] {});
        entry->views.clear();
        entry->state = Entry::State::Unloaded;
        resident_bytes -= entry->bytes;
        evicted = true;
    }
    if (evicted) {
        ++generation; // memoized bindings must not keep the freed views
    }
}

void TexturePack::Upload(Entry& entry) {
    entry.state = Entry::State::Failed;
    const auto& file = entry.file;
    u32 levels = 1;
    u32 dxgi = 0;
    if (file.size() > DdsHeader) {
        std::memcpy(&levels, file.data() + 28, 4);
        std::memcpy(&dxgi, file.data() + 128, 4);
    }
    entry.format = dxgi == DxgiBc5Snorm   ? vk::Format::eBc5SnormBlock
                   : dxgi == DxgiBc5Unorm ? vk::Format::eBc5UnormBlock
                   : dxgi == DxgiBc4Unorm ? vk::Format::eBc4UnormBlock
                                          : vk::Format::eBc7UnormBlock;
    const u32 block_bytes = entry.format == vk::Format::eBc4UnormBlock ? 8 : 16;
    levels = std::clamp(levels, 1u, MaxLevels);
    std::array<vk::BufferImageCopy, MaxLevels> copies{};
    u64 total = 0;
    for (u32 m = 0; m < levels; ++m) {
        const u32 w = std::max(entry.width >> m, 1u), h = std::max(entry.height >> m, 1u);
        copies[m] = {.bufferOffset = total,
                     .imageSubresource = {vk::ImageAspectFlagBits::eColor, m, 0, 1},
                     .imageExtent = {w, h, 1}};
        total += u64((w + 3) / 4) * ((h + 3) / 4) * block_bytes;
    }
    if (file.size() < DdsHeader + total || resident_bytes + total > Budget) {
        std::printf("Texture pack: cannot load %s\n", entry.path.c_str());
        entry.file = {};
        return;
    }
    entry.image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    entry.image.Create(vk::ImageCreateInfo{
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = entry.format,
        .extent = {entry.width, entry.height, 1},
        .mipLevels = levels,
        .arrayLayers = 1,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
    }, pool);
    const auto buffer = staging.Request(total, VideoCore::MemoryType::HostUncached);
    std::memcpy(buffer.mapped, file.data() + DdsHeader, total);
    buffer.Flush();
    for (u32 m = 0; m < levels; ++m) {
        copies[m].bufferOffset += buffer.offset;
    }
    scheduler.EndRendering();
    scheduler.Record([image = vk::Image(entry.image), src = buffer.buffer->Handle(), copies,
                      levels](vk::CommandBuffer cmd) {
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, levels, 0, 1};
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = image,
            .subresourceRange = range,
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        cmd.copyBufferToImage(src, image, vk::ImageLayout::eTransferDstOptimal, levels,
                              copies.data());
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eCopy;
        barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
        barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
    entry.levels = levels;
    entry.bytes = total;
    entry.last_used = now;
    entry.file = {};
    entry.state = Entry::State::Ready;
    resident_bytes += total;
    static const bool log = std::getenv("TLG_TEXPACK_LOG");
    if (log) {
        std::printf("Texture pack: loaded %s\n", entry.path.c_str());
    }
}

vk::ImageView TexturePack::Sample(VideoCore::Image& image, const VideoCore::ImageViewInfo& info) {
    if (!active) {
        return {};
    }
    if (image.pack_key == ~0ull) {
        image.pack_key = Key(image); // guest memory holds the uploaded data (refreshed before)
    }
    if (!image.pack_key) {
        return {};
    }
    const auto it = entries.find(image.pack_key);
    if (it == entries.end()) {
        ++misses;
        static const bool log = std::getenv("TLG_TEXPACK_LOG");
        static std::unordered_set<u64> logged;
        if (log && image.info.size.width >= 256 && logged.insert(image.pack_key).second) {
            std::printf("Texture pack: miss %016llx %ux%u format %d bytes %u\n",
                        (unsigned long long)image.pack_key, image.info.size.width,
                        image.info.size.height, int(image.info.pixel_format),
                        image.info.mips_layout[0].size);
        }
        return {};
    }
    Entry& entry = *it->second;
    entry.last_used = now;
    if (entry.state == Entry::State::Unloaded) {
        entry.state = Entry::State::Loading;
        std::scoped_lock lock{mutex};
        queue.push_back(it->second);
        wake.notify_one();
    }
    // Whole-texture 2D views only: a partial mip range or LOD clamp has no pack counterpart.
    if (entry.state != Entry::State::Ready || info.type != AmdGpu::ImageType::Color2D ||
        info.range.base.level != 0 || info.range.extent.levels < image.info.resources.levels ||
        info.min_lod != 0 ||
        (IsDataBc(entry.format) ? info.format != entry.format : !IsColorBc(info.format))) {
        refused += entry.state == Entry::State::Ready;
        return {};
    }
    ++hits;
    auto mapping = info.mapping;
    if (info.format == vk::Format::eBc1RgbUnormBlock ||
        info.format == vk::Format::eBc1RgbSrgbBlock) {
        mapping.a = vk::ComponentSwizzle::eOne;
    }
    static const bool tint = std::getenv("TLG_TEXPACK_TINT"); // debug: pack colour textures get R and B swapped
    if (tint && !IsDataBc(entry.format)) {
        std::swap(mapping.r, mapping.b);
    }
    const auto key = std::make_pair(IsSrgb(info.format), mapping);
    for (const auto& [view_key, view] : entry.views) {
        if (view_key == key) {
            return *view;
        }
    }
    auto view = Check(instance.GetDevice().createImageViewUnique({
        .image = entry.image,
        .viewType = vk::ImageViewType::e2D,
        .format = IsDataBc(entry.format) ? entry.format
                  : key.first                ? vk::Format::eBc7SrgbBlock
                                             : vk::Format::eBc7UnormBlock,
        .components = mapping,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, entry.levels, 0, 1},
    }));
    const vk::ImageView result = *view;
    entry.views.emplace_back(key, std::move(view));
    return result;
}

void TexturePack::Touch(const VideoCore::Image& image) {
    if (const auto it = entries.find(image.pack_key); it != entries.end()) {
        it->second->last_used = now;
    }
}

} // namespace Vulkan
