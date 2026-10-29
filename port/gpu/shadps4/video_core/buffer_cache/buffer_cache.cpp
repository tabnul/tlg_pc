// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <array>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>
#include "tlg_copy.h"
#include "tlg_settings.h"
#include "tlg_toggles.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    // tlg: the batch limit is the graphics setting readback_batch (default off: since the fence
    // flushes and the side queue, batching costs 2-5 FPS, 2026-10-23); this variable forces a size.
    if (const char* forced = std::getenv("TLG_BUFFER_READBACK_BATCH_MB")) {
        char* end{};
        const u64 mib = std::strtoull(forced, &end, 10);
        if (end != forced && *end == '\0' && mib != 0) {
            readback_batch_limit = std::clamp<u64>(mib, 1, 256) * 1_MB;
        }
        readback_batch_forced = true;
    }
    if (const char* diag = std::getenv("TLG_BUFFER_READBACK_STATS")) {
        readback_stats_enabled = *diag == '1';
    }
    if (const char* hot = std::getenv("TLG_RB_HOT")) {
        hot_readbacks_enabled = *hot != '0';
    }
    if (readback_batch_limit || readback_stats_enabled) {
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        readback_report_ns.store(static_cast<u64>(now_ns), std::memory_order_relaxed);
        std::printf("GPU: coherent buffer readback batching %s (limit %llu MiB)%s\n",
                    readback_batch_limit ? "enabled" : "disabled",
                    static_cast<unsigned long long>(readback_batch_limit >> 20),
                    readback_stats_enabled ? "; statistics enabled" : "");
    }

    // tlg: TLG_MAX_BUFFER_MB lowers the driver's buffer size limit as the arenas see it
    // (diagnostics: reproduces AMD on Windows 2048, RADV 4095 on any GPU).
    u64 max_buffer = instance.GetMaxBufferSize();
    if (const char* forced = std::getenv("TLG_MAX_BUFFER_MB")) {
        if (const u64 mb = std::strtoull(forced, nullptr, 10)) {
            max_buffer = std::min<u64>(max_buffer, mb << 20);
        }
    }
    // tlg: with a limit under two full pages (AMD on Windows 2 GiB, RADV ~4 GiB, Intel), pages
    // are the smallest size and an arena joins as many as the limit allows: one inFAMOUS request
    // of ~2.4 GiB (the cutscene before the graffiti) crossed three 1 GiB pages and stopped the
    // game ("cannot span more than two VA arena pages", iss_pc #18).
    if ((u64{2} << arena_page_bits) > max_buffer) {
        arena_page_bits = MIN_ARENA_PAGE_BITS;
    }
    arena_page_size = u64{1} << arena_page_bits;
    max_arena_pages = std::clamp<u64>(max_buffer >> arena_page_bits, 2, 16);
    num_arena_pages = u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits);
    std::printf("GPU: buffer arenas of %llu MiB, up to %llu pages (driver buffer limit %llu MiB)\n",
                static_cast<unsigned long long>(arena_page_size >> 20),
                static_cast<unsigned long long>(max_arena_pages),
                static_cast<unsigned long long>(max_buffer >> 20));
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    // Zero-initialized; Windows commits its pages on first touch, so only ranges with written
    // bindings cost real memory.
    exact_write_pages = std::make_unique<std::atomic<u64>[]>(ExactPageWords);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
}

BufferCache::~BufferCache() = default;

// tlg phase 4, default off: the false-share skip never showed a gain beyond noise (condor,
// pulse, top-5 slowest scenes, 2026-10-29). TLG_RB_FALSESHARE=1 re-enables it for reference.
static bool TlgFalseShareOn() {
    static const bool on = [] {
        const char* env = std::getenv("TLG_RB_FALSESHARE");
        return env && env[0] == '1';
    }();
    return on;
}

void BufferCache::SetExactWritePages(VAddr addr, u64 size) {
    if (size == 0 || !TlgFalseShareOn()) {
        return;
    }
    u64 first = addr >> ExactPageShift;
    const u64 last = (addr + size - 1) >> ExactPageShift;
    while (first <= last) {
        const u64 lo = first & 63;
        const u64 hi = std::min<u64>(63, lo + (last - first));
        const u64 mask = (hi == 63 ? ~u64{0} : ((u64{1} << (hi + 1)) - 1)) & ~((u64{1} << lo) - 1);
        exact_write_pages[(first >> 6) & (ExactPageWords - 1)].fetch_or(mask);
        first += hi - lo + 1;
    }
}

void BufferCache::ClearExactWritePagesInner(VAddr addr, u64 size) {
    // Only pages fully inside the range: an edge page may share bytes with a neighboring
    // island that was not downloaded, and a stale set bit only costs a missed skip.
    u64 first = (addr + (u64{1} << ExactPageShift) - 1) >> ExactPageShift;
    const u64 end = (addr + size) >> ExactPageShift;
    while (first < end) {
        const u64 lo = first & 63;
        const u64 hi = std::min<u64>(63, lo + (end - 1 - first));
        const u64 mask = (hi == 63 ? ~u64{0} : ((u64{1} << (hi + 1)) - 1)) & ~((u64{1} << lo) - 1);
        exact_write_pages[(first >> 6) & (ExactPageWords - 1)].fetch_and(~mask);
        first += hi - lo + 1;
    }
}

bool BufferCache::AnyExactWritePage(VAddr addr, u64 size) const {
    if (size == 0) {
        return false;
    }
    u64 first = addr >> ExactPageShift;
    const u64 last = (addr + size - 1) >> ExactPageShift;
    while (first <= last) {
        const u64 lo = first & 63;
        const u64 hi = std::min<u64>(63, lo + (last - first));
        const u64 mask = (hi == 63 ? ~u64{0} : ((u64{1} << (hi + 1)) - 1)) & ~((u64{1} << lo) - 1);
        if (exact_write_pages[(first >> 6) & (ExactPageWords - 1)].load() & mask) {
            return true;
        }
        first += hi - lo + 1;
    }
    return false;
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    // tlg phase 4 (default off, TLG_RB_FALSESHARE=1 enables): a write fault whose pages hold no
    // not-yet-downloaded GPU bytes (tracker bits left over from page- or window-granular
    // marking) skips the round trip through the GPU thread. The unmark checks the exact-page
    // bitmap under the region lock, so a binding being marked right now is never lost: it
    // sets its bitmap pages before it takes that lock.
    if (TlgFalseShareOn() && !AnyExactWritePage(device_addr, size)) {
        const bool clean = memory_tracker->UnmarkGpuIfClean(
            device_addr, size,
            [this](VAddr sub_addr, u64 sub_size) { return !AnyExactWritePage(sub_addr, sub_size); });
        if (clean) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
            if (readback_stats_enabled) {
                readback_false_share_skips.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        // A written binding landed on these pages between the check and the lock: the normal
        // path below still sees its tracker bits and flushes.
    }
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// tlg: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in the test scene, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const TlgCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const TlgCopy::Item& item) {
    if (scheduler.IsRecordingDeferred() && !TlgToggle::Disabled(TlgToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
        return;
    }
    TlgCopy::QueueCopy(item);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("TLG_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || TlgToggle::Disabled(TlgToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const auto flush_request = [this, device_addr, size, is_write] {
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        // tlg: the width is tunable; each fault pays a near-fixed sync cost, so a wider
        // window that prefetches neighboring islands saves later faults (TLG_RB_WINDOW KiB).
        static const u64 WindowSize = [] {
            const char* env = std::getenv("TLG_RB_WINDOW");
            const u64 kib = env ? std::strtoull(env, nullptr, 10) : 512;
            return std::bit_ceil(std::clamp<u64>(kib, 64, 8192)) * 1024;
        }();
        // tlg phase 1 (TLG_RB_EXACTWRITE=0 disables): a write fault downloads only the GPU
        // islands overlapping the written pages, not the whole window. The CPU needs coherence
        // only there, and a narrow gather waits for the producer of those bytes instead of the
        // youngest write anywhere in the window. Read faults keep the prefetch window.
        static const bool exact_write = [] {
            const char* env = std::getenv("TLG_RB_EXACTWRITE");
            return !env || env[0] != '0';
        }();
        const u64 window_size = exact_write && is_write ? TRACKER_BYTES_PER_PAGE : WindowSize;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, window_size), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + window_size,
                            Common::AlignUp(device_addr + size, TRACKER_BYTES_PER_PAGE)),
            arena_end);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

void BufferCache::ReleaseReadProtection(VAddr device_addr, u64 size) {
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    u64 copy_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    VAddr download_addr = device_addr;
    u64 download_size = size;

    if (readback_stats_enabled) {
        readback_requests.fetch_add(1, std::memory_order_relaxed);
    }

    // tlg: land a finished background readback first. One on this same arena must land now,
    // even if that blocks: its islands are no longer in gpu_modified_ranges, so the gather
    // below would miss them and the unmark at the end would then hide stale bytes.
    if (async_active) {
        ResolveAsyncReadback(arena);
    }

    // A CPU fault must see every preceding GPU write before it resumes, so the wait itself
    // cannot be deferred. At 60 FPS, however, the first fault can download all exact dirty
    // buffer islands in this sparse arena with one submission. Later faults then find clean
    // pages instead of serializing behind the increasingly long open-scene GPU queue.
    //
    // The cap is based on exact copied bytes, not the arena's virtual size. If a title really
    // dirties more than the configured limit, retain the conservative request window rather
    // than allocate an unexpectedly large staging buffer. No dirty data is ever discarded.
    const u64 batch_limit =
        readback_batch_forced
            ? readback_batch_limit
            : u64(TlgSettings::Get().readback_batch.load(std::memory_order_relaxed)) * 1_MB;
    if (batch_limit) {
        u64 candidate_bytes = 0;
        bool over_limit = false;
        gpu_modified_ranges.ForEachInRange(arena_base, arena->size_bytes,
                                            [&](VAddr start, VAddr end) {
            if (over_limit) {
                return;
            }
            constexpr u64 align = 64;
            const u64 range_bytes = Common::AlignUp(end - start, align);
            if (range_bytes > batch_limit - candidate_bytes) {
                over_limit = true;
                return;
            }
            candidate_bytes += range_bytes;
        });
        if (!over_limit && candidate_bytes != 0) {
            download_addr = arena_base;
            download_size = arena->size_bytes;
            if (readback_stats_enabled) {
                readback_full_arena_batches.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (over_limit && readback_stats_enabled) {
            readback_limit_fallbacks.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // tlg: an island with no recorded GPU write since RAM last received its bytes would
    // download what RAM already holds (site g:c4c630: a third of its downloads), so the
    // wait and the copy are skipped. TLG_RB_STALESKIP=0 restores the old behavior.
    static const bool stale_skip = [] {
        const char* env = std::getenv("TLG_RB_STALESKIP");
        return !env || env[0] != '0';
    }();
    u64 stale_bytes = 0;
    const auto add_download = [&](VAddr start, VAddr end) {
        const u64 new_offset = start - arena_base;
        const u64 new_size = end - start;
        if (stale_skip && arena->UnchangedSinceDownload(new_offset, new_size)) {
            stale_bytes += new_size;
            return;
        }
        copies.push_back(vk::BufferCopy{
            .srcOffset = new_offset,
            .dstOffset = total_size_bytes,
            .size = new_size,
        });
        // Align up to avoid cache conflicts
        constexpr u64 align = 64ULL;
        constexpr u64 mask = ~(align - 1ULL);
        total_size_bytes += (new_size + align - 1) & mask;
        copy_size_bytes += new_size;
    };
    memory_tracker->ForEachDownloadRange<false>(download_addr, download_size,
                                                 [&](u64 address, u64 size) {
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });

    // tlg: hot readbacks (see buffer_cache.h). A hot region skips the pages whose CPU bit is
    // set: the guest may have written them since, and its bytes are the right ones.
    const size_t requested_copies = copies.size();
    boost::container::small_vector<std::pair<VAddr, u64>, 16> hot_runs;
    if (hot_readbacks_enabled) {
        const auto now = std::chrono::steady_clock::now();
        const VAddr arena_end = arena_base + arena->size_bytes;
        boost::container::small_vector<std::pair<VAddr, u64>, MaxHotReadbacks> regions;
        bool known = false;
        for (auto& hot : hot_readbacks) {
            if (hot.addr == device_addr && hot.size == size) {
                hot.last_request = now;
                known = true;
            } else if (now - hot.last_request <= HotReadbackLifetime && hot.addr >= arena_base &&
                       hot.addr + hot.size <= arena_end) {
                regions.emplace_back(hot.addr, hot.size);
            }
        }
        if (!known) {
            if (hot_readbacks.size() < MaxHotReadbacks) {
                hot_readbacks.push_back({device_addr, size, now});
            } else {
                *std::ranges::min_element(hot_readbacks, {}, &HotReadback::last_request) = {
                    device_addr, size, now};
            }
        }
        // Only when this readback copies anyway: an empty one costs no sync to share.
        if (total_size_bytes == 0) {
            regions.clear();
        }
        for (const auto& [region_addr, region_size] : regions) {
            const size_t first_copy = copies.size();
            const u64 total_before = total_size_bytes;
            const u64 copy_before = copy_size_bytes;
            const size_t runs_before = hot_runs.size();
            u64 skipped = 0;
            memory_tracker->ForEachDownloadRange<false>(region_addr, region_size,
                                                         [&](u64 address, u64 range_size) {
                VAddr run_start = address;
                const VAddr range_end = address + range_size;
                const auto flush_run = [&](VAddr end) {
                    if (end > run_start) {
                        gpu_modified_ranges.ForEachInRange(run_start, end - run_start, add_download);
                        hot_runs.emplace_back(run_start, end - run_start);
                    }
                };
                for (VAddr page = address; page < range_end;) {
                    const VAddr next = std::min<VAddr>(
                        Common::AlignDown(page, TRACKER_BYTES_PER_PAGE) + TRACKER_BYTES_PER_PAGE,
                        range_end);
                    if (memory_tracker->IsRegionCpuModified(page, next - page)) {
                        flush_run(page);
                        run_start = next;
                        skipped += next - page;
                    }
                    page = next;
                }
                flush_run(range_end);
            });
            if (copy_size_bytes - copy_before > MaxHotBytes) {
                // Not downloaded now: its ranges stay in the modified set.
                copies.resize(first_copy);
                total_size_bytes = total_before;
                copy_size_bytes = copy_before;
                hot_runs.resize(runs_before);
                continue;
            }
            for (size_t i = runs_before; i < hot_runs.size(); ++i) {
                gpu_modified_ranges.Subtract(hot_runs[i].first, hot_runs[i].second);
            }
            if (readback_stats_enabled) {
                readback_hot_regions.fetch_add(copies.size() != first_copy,
                                               std::memory_order_relaxed);
                readback_hot_bytes.fetch_add(copy_size_bytes - copy_before,
                                             std::memory_order_relaxed);
                readback_hot_skipped.fetch_add(skipped, std::memory_order_relaxed);
            }
        }
    }
    if (total_size_bytes == 0) {
        // Every island was stale-skipped: RAM is already right, but the pages must still be
        // unprotected or the same fault fires forever.
        if (stale_bytes != 0) {
            memory_tracker->UnmarkRegionAsGpuModified(download_addr, download_size);
            ClearExactWritePagesInner(download_addr, download_size);
            if (readback_stats_enabled) {
                readback_stale_bytes.fetch_add(stale_bytes, std::memory_order_relaxed);
            }
        }
        if (readback_stats_enabled) {
            readback_empty_requests.fetch_add(1, std::memory_order_relaxed);
        }
        ReportReadbackStats();
        return;
    }
    if (readback_stats_enabled) {
        readback_gpu_completions.fetch_add(1, std::memory_order_relaxed);
        readback_copy_ranges.fetch_add(copies.size(), std::memory_order_relaxed);
        readback_copy_bytes.fetch_add(copy_size_bytes, std::memory_order_relaxed);
        readback_staging_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    // tlg: when the last GPU write to this data has finished, the copy runs on the second
    // queue and waits only for itself. The open command buffer and every submission still in
    // flight are left alone; upstream finishes the whole GPU queue for each readback.
    u64 last_write = 0;
    for (const auto& copy : copies) {
        last_write = std::max(last_write, arena->LastWrite(copy.srcOffset, copy.size));
    }
    bool write_done = last_write != 0 && scheduler.IsFree(last_write);
    if (readback_stats_enabled) {
        if (last_write == 0) {
            readback_writes_unknown.fetch_add(1, std::memory_order_relaxed);
        } else if (write_done) {
            readback_writes_done.fetch_add(1, std::memory_order_relaxed);
        } else if (last_write < scheduler.CurrentTick()) {
            readback_writes_submitted.fetch_add(1, std::memory_order_relaxed);
        } else {
            readback_writes_recording.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const auto wait_start = std::chrono::steady_clock::now();
    // tlg: a last write that is submitted but not finished only needs a wait for its own
    // tick, not a Finish() of the whole queue behind it; after the wait the side-queue copy
    // is as safe as for a finished write. TLG_RB_TICKWAIT=0 restores the old full finish.
    static const bool tick_wait = [] {
        const char* env = std::getenv("TLG_RB_TICKWAIT");
        return !env || env[0] != '0';
    }();
    if (tick_wait && !write_done && last_write != 0 && last_write < scheduler.CurrentTick() &&
        !TlgToggle::Disabled2(TlgToggle::ReadbackSideQueue)) {
        scheduler.Wait(last_write);
        write_done = true;
    }
    const bool side = write_done && !TlgToggle::Disabled2(TlgToggle::ReadbackSideQueue) &&
                      CopyOnReadbackQueue(arena, download.buffer, copies, last_write);
    if (!side) {
        runtime.CopyBuffer(arena, download.buffer, copies);
        scheduler.Finish();
    }
    if (readback_stats_enabled) {
        const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - wait_start)
                                            .count());
        (side ? readback_side_ns : readback_finish_ns).fetch_add(ns, std::memory_order_relaxed);
        if (side) {
            readback_side_copies.fetch_add(1, std::memory_order_relaxed);
        }
    }

    download.buffer->Invalidate(download.offset, download.size);
    for (size_t i = 0; i < copies.size(); ++i) {
        const auto& copy = copies[i];
        const VAddr dst_addr = arena_base + copy.srcOffset;
        const u8* src = download.mapped + (copy.dstOffset - download.offset);
        if (i < requested_copies) {
            memory->TryWriteBacking(std::bit_cast<u8*>(dst_addr), src, copy.size);
            continue;
        }
        // Hot data: checked again page by page under the region lock, so a guest write since
        // the gather is neither overwritten nor raced (SotC: a size the game had just read from
        // a file, replaced by stale GPU bytes, asked for a 59 GiB allocation).
        const u64 skipped = memory_tracker->WriteUnlessCpuModified(
            dst_addr, copy.size, [&](VAddr part_addr, u64 part_size) {
                memory->TryWriteBacking(std::bit_cast<u8*>(part_addr), src + (part_addr - dst_addr),
                                        part_size);
            });
        if (skipped && readback_stats_enabled) {
            readback_hot_skipped.fetch_add(skipped, std::memory_order_relaxed);
        }
    }
    memory_tracker->UnmarkRegionAsGpuModified(download_addr, download_size);
    ClearExactWritePagesInner(download_addr, download_size);
    for (const auto& [run_addr, run_size] : hot_runs) {
        memory_tracker->UnmarkRegionAsGpuModified(run_addr, run_size);
        ClearExactWritePagesInner(run_addr, run_size);
    }
    // Every island in the window was written back (or stale-skipped), so its fully covered
    // 64 KiB groups are current at their write ticks.
    arena->NoteDownloaded(download_addr - arena_base, download_size);
    if (stale_bytes != 0 && readback_stats_enabled) {
        readback_stale_bytes.fetch_add(stale_bytes, std::memory_order_relaxed);
    }
    // tlg: prefetch the arena's remaining dirty islands in the background (TLG_RB_ASYNC=1).
    // Default OFF: measured -20% on the condor route 2026-10-29 - the copy's GPU-side wait on
    // the youngest producer tick occupies the side queue and every later readback queues
    // behind it. Kept for reference only.
    static const bool async_enabled = [] {
        const char* env = std::getenv("TLG_RB_ASYNC");
        return env && env[0] == '1';
    }();
    if (async_enabled && !async_active && !TlgToggle::Disabled2(TlgToggle::ReadbackSideQueue)) {
        IssueAsyncReadback(arena);
    }
    ReportReadbackStats();
}

bool BufferCache::CopyOnReadbackQueue(const Buffer* arena, const Buffer* dst,
                                      std::span<const vk::BufferCopy> copies, u64 last_write) {
    const vk::Queue queue = instance.GetReadbackQueue();
    if (!queue) {
        return false;
    }
    const vk::Device device = instance.GetDevice();
    if (!readback_pool) {
        auto [pool_result, pool] = device.createCommandPoolUnique({
            .flags = vk::CommandPoolCreateFlagBits::eTransient |
                     vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
        });
        auto [fence_result, fence] = device.createFenceUnique({});
        if (pool_result != vk::Result::eSuccess || fence_result != vk::Result::eSuccess) {
            return false;
        }
        auto [alloc_result, cmdbufs] = device.allocateCommandBuffers({
            .commandPool = *pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        });
        if (alloc_result != vk::Result::eSuccess) {
            return false;
        }
        readback_pool = std::move(pool);
        readback_fence = std::move(fence);
        readback_cmdbuf = cmdbufs.front();
    }

    const vk::CommandBufferBeginInfo begin_info{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    if (readback_cmdbuf.begin(begin_info) != vk::Result::eSuccess) {
        return false;
    }
    readback_cmdbuf.copyBuffer(arena->Handle(), dst->Handle(), static_cast<u32>(copies.size()),
                               copies.data());
    const vk::MemoryBarrier2 host_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    readback_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &host_barrier,
    });
    if (readback_cmdbuf.end() != vk::Result::eSuccess) {
        return false;
    }

    // The wait on the main timeline is already satisfied; it makes the writes visible here.
    const vk::Semaphore timeline = scheduler.GetWorkSemaphore()->Handle();
    const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
    const vk::TimelineSemaphoreSubmitInfo timeline_si{
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &last_write,
    };
    const vk::SubmitInfo submit_info{
        .pNext = &timeline_si,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &timeline,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &readback_cmdbuf,
    };
    const vk::Result submit_result = queue.submit(submit_info, *readback_fence);
    ASSERT_MSG(submit_result == vk::Result::eSuccess, "Readback submit failed: {}",
               vk::to_string(submit_result));
    const vk::Result wait_result =
        device.waitForFences(*readback_fence, true, std::numeric_limits<u64>::max());
    ASSERT_MSG(wait_result == vk::Result::eSuccess, "Readback fence wait failed: {}",
               vk::to_string(wait_result));
    const vk::Result reset_result = device.resetFences(*readback_fence);
    ASSERT_MSG(reset_result == vk::Result::eSuccess, "Readback fence reset failed: {}",
               vk::to_string(reset_result));
    return true;
}

void BufferCache::ResolveAsyncReadback(const Buffer* arena) {
    const vk::Device device = instance.GetDevice();
    if (arena != async_arena) {
        // Another arena faulted: only land the copy when it is already done.
        if (device.getFenceStatus(*async_fence) != vk::Result::eSuccess) {
            return;
        }
    } else {
        // The fault needs pages this copy owns, so it must land now. The producer tick was
        // submitted before the copy went out, so the fence always signals without our help.
        const vk::Result wait_result =
            device.waitForFences(*async_fence, true, std::numeric_limits<u64>::max());
        ASSERT_MSG(wait_result == vk::Result::eSuccess, "Async readback fence wait failed: {}",
                   vk::to_string(wait_result));
    }
    const vk::Result reset_result = device.resetFences(*async_fence);
    ASSERT_MSG(reset_result == vk::Result::eSuccess, "Async readback fence reset failed: {}",
               vk::to_string(reset_result));
    async_staging->Invalidate(0, AsyncStagingBytes);
    for (const auto& copy : async_copies) {
        const u8* src = async_staging->mapped_data.data() + copy.staging_offset;
        // Checked page by page: a guest write since the gather wins over the GPU bytes.
        memory_tracker->WriteUnlessCpuModified(
            copy.addr, copy.size, [&](VAddr part_addr, u64 part_size) {
                memory->TryWriteBacking(std::bit_cast<u8*>(part_addr),
                                        src + (part_addr - copy.addr), part_size);
            });
        memory_tracker->UnmarkRegionAsGpuModified(copy.addr, copy.size);
        ClearExactWritePagesInner(copy.addr, copy.size);
    }
    async_copies.clear();
    async_arena = nullptr;
    async_active = false;
}

void BufferCache::IssueAsyncReadback(const Buffer* arena) {
    const vk::Queue queue = instance.GetReadbackQueue();
    if (!queue || !readback_pool) {
        // The pool exists once the synchronous side-queue path has run; until then the scene
        // has produced no side copies and a prefetch has nothing proven to gain.
        return;
    }
    // Gather the islands whose producer is already submitted. A recording tick cannot be
    // used: this thread is the one that will submit it, and the copy's GPU-side wait on it
    // would then never be satisfied once a same-arena fault blocks on the fence. An unknown
    // producer (0) stays with the synchronous path, which treats it conservatively.
    const VAddr arena_base = arena->cpu_addr;
    const u64 current_tick = scheduler.CurrentTick();
    boost::container::small_vector<vk::BufferCopy, 16> copies;
    u64 total = 0;
    u64 wait_value = 0;
    gpu_modified_ranges.ForEachInRange(arena_base, arena->size_bytes, [&](VAddr start, VAddr end) {
        constexpr u64 align = 64;
        const u64 island_size = end - start;
        const u64 aligned = Common::AlignUp(island_size, align);
        if (copies.size() >= 128 || aligned > AsyncStagingBytes - total) {
            return;
        }
        const u64 lw = arena->LastWrite(start - arena_base, island_size);
        if (lw == 0 || lw >= current_tick) {
            return;
        }
        copies.push_back(vk::BufferCopy{
            .srcOffset = start - arena_base,
            .dstOffset = total,
            .size = island_size,
        });
        wait_value = std::max(wait_value, lw);
        total += aligned;
    });
    if (copies.empty()) {
        return;
    }
    const vk::Device device = instance.GetDevice();
    if (!async_fence) {
        auto [fence_result, fence] = device.createFenceUnique({});
        if (fence_result != vk::Result::eSuccess) {
            return;
        }
        auto [alloc_result, cmdbufs] = device.allocateCommandBuffers({
            .commandPool = *readback_pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        });
        if (alloc_result != vk::Result::eSuccess) {
            return;
        }
        async_fence = std::move(fence);
        async_cmdbuf = cmdbufs.front();
        async_staging = std::make_unique<Buffer>(instance, 0, AsyncStagingBytes,
                                                 MemoryType::HostCached,
                                                 "tlg async readback staging");
    }
    const vk::CommandBufferBeginInfo begin_info{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    if (async_cmdbuf.begin(begin_info) != vk::Result::eSuccess) {
        return;
    }
    async_cmdbuf.copyBuffer(arena->Handle(), async_staging->Handle(),
                            static_cast<u32>(copies.size()), copies.data());
    const vk::MemoryBarrier2 host_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    async_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &host_barrier,
    });
    if (async_cmdbuf.end() != vk::Result::eSuccess) {
        return;
    }
    // The producer tick is waited for on the GPU; the submit returns at once.
    const vk::Semaphore timeline = scheduler.GetWorkSemaphore()->Handle();
    const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
    const vk::TimelineSemaphoreSubmitInfo timeline_si{
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &wait_value,
    };
    const vk::SubmitInfo submit_info{
        .pNext = &timeline_si,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &timeline,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &async_cmdbuf,
    };
    if (queue.submit(submit_info, *async_fence) != vk::Result::eSuccess) {
        return;
    }
    async_copies.clear();
    for (const auto& copy : copies) {
        // Out of gpu_modified_ranges so no later gather downloads it twice; the pages stay
        // tracker-protected until the writeback, so reads still fault and resolve first.
        gpu_modified_ranges.Subtract(arena_base + copy.srcOffset, copy.size);
        async_copies.push_back({arena_base + copy.srcOffset, copy.size, copy.dstOffset});
    }
    async_arena = arena;
    async_active = true;
}

void BufferCache::ReportReadbackStats() {
    if (!readback_stats_enabled) {
        return;
    }
    const u64 now_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count());
    u64 report_ns = readback_report_ns.load(std::memory_order_relaxed);
    if (now_ns - report_ns < 5'000'000'000ULL ||
        !readback_report_ns.compare_exchange_strong(report_ns, now_ns,
                                                    std::memory_order_relaxed)) {
        return;
    }
    const u64 requests = readback_requests.exchange(0, std::memory_order_relaxed);
    const u64 empty_requests = readback_empty_requests.exchange(0, std::memory_order_relaxed);
    const u64 full_batches = readback_full_arena_batches.exchange(0, std::memory_order_relaxed);
    const u64 fallbacks = readback_limit_fallbacks.exchange(0, std::memory_order_relaxed);
    const u64 completions = readback_gpu_completions.exchange(0, std::memory_order_relaxed);
    const u64 ranges = readback_copy_ranges.exchange(0, std::memory_order_relaxed);
    const u64 copy_bytes = readback_copy_bytes.exchange(0, std::memory_order_relaxed);
    const u64 staging_bytes = readback_staging_bytes.exchange(0, std::memory_order_relaxed);
    std::printf("Buffer readbacks (5 s): requests %llu (%llu empty), full-arena batches %llu, "
                "limit fallbacks %llu, GPU completions %llu, ranges %llu, copied %.2f MiB, "
                "staging %.2f MiB; hot regions %llu (%.2f MiB, %.2f MiB CPU-written skipped)\n",
                static_cast<unsigned long long>(requests),
                static_cast<unsigned long long>(empty_requests),
                static_cast<unsigned long long>(full_batches),
                static_cast<unsigned long long>(fallbacks),
                static_cast<unsigned long long>(completions),
                static_cast<unsigned long long>(ranges), copy_bytes / 1048576.0,
                staging_bytes / 1048576.0,
                static_cast<unsigned long long>(readback_hot_regions.exchange(0)),
                readback_hot_bytes.exchange(0) / 1048576.0,
                readback_hot_skipped.exchange(0) / 1048576.0);
    std::printf("Buffer readback stale skips (5 s): %.2f MiB not re-downloaded, %llu write "
                "faults resolved without a flush\n",
                readback_stale_bytes.exchange(0, std::memory_order_relaxed) / 1048576.0,
                static_cast<unsigned long long>(
                    readback_false_share_skips.exchange(0, std::memory_order_relaxed)));
    std::printf("Buffer readback waits (5 s): %.1f ms in Finish, %.1f ms in %llu side copies; "
                "last GPU write done %llu, submitted %llu, still "
                "recording %llu, unknown %llu; fence flushes EOP %llu, EOS %llu, WRITE_DATA %llu, "
                "RELEASE_MEM %llu, compute WRITE_DATA %llu\n",
                readback_finish_ns.exchange(0, std::memory_order_relaxed) / 1e6,
                readback_side_ns.exchange(0, std::memory_order_relaxed) / 1e6,
                static_cast<unsigned long long>(readback_side_copies.exchange(0)),
                static_cast<unsigned long long>(readback_writes_done.exchange(0)),
                static_cast<unsigned long long>(readback_writes_submitted.exchange(0)),
                static_cast<unsigned long long>(readback_writes_recording.exchange(0)),
                static_cast<unsigned long long>(readback_writes_unknown.exchange(0)),
                static_cast<unsigned long long>(
                    TlgStats::fence_flushes[TlgStats::FenceEop].exchange(0)),
                static_cast<unsigned long long>(
                    TlgStats::fence_flushes[TlgStats::FenceEos].exchange(0)),
                static_cast<unsigned long long>(
                    TlgStats::fence_flushes[TlgStats::FenceWriteData].exchange(0)),
                static_cast<unsigned long long>(
                    TlgStats::fence_flushes[TlgStats::FenceReleaseMem].exchange(0)),
                static_cast<unsigned long long>(
                    TlgStats::fence_flushes[TlgStats::FenceComputeWriteData].exchange(0)));
    std::fflush(stdout);
}

namespace {
// tlg: TLG_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = TlgStats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("TLG_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferObtain);
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // tlg: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !TlgToggle::Disabled(TlgToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    if (is_written) {
        // Before SynchronizeMemory flags the tracker pages: a write fault that sees a GPU
        // tracker bit must also see these bits, or it would skip the flush (phase 4).
        SetExactWritePages(device_addr, size);
    }
    const u64 uploaded_before = TlgStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            TlgStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
        // tlg: Runtime::AccessBuffer notes the tick the write is recorded in; this covers a
        // write that does not pass through it.
        arena->NoteWrite(arena->Offset(device_addr), size, scheduler.CurrentTick());
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferForImage);
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!TlgToggle::Disabled(TlgToggle::DeferredUploads)) {
        // tlg: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const TlgCopy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                TlgCopy::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    // Arenas cover contiguous runs of pages: when both ends are in the same one, so is the rest.
    if (const Buffer* arena = address_space[first_page]; arena && arena == address_space[last_page]) {
        return arena;
    }
    // tlg: the pages the new arena covers: the request plus every arena it touches (an arena may
    // join several pages, up to the driver's buffer limit).
    u64 lo = first_page, hi = last_page;
    for (u64 page = first_page; page <= last_page; ++page) {
        if (const Buffer* arena = address_space[page]) {
            lo = std::min<u64>(lo, arena->cpu_addr >> arena_page_bits);
            hi = std::max<u64>(hi, ((arena->cpu_addr + arena->size_bytes) >> arena_page_bits) - 1);
        }
    }
    const u64 num_pages = hi - lo + 1;
    // Over the driver's limit: the old code built such arenas too (two merged arenas could
    // exceed it) and drivers accepted them, so warn instead of stopping.
    if (num_pages > max_arena_pages) {
        LOG_WARNING(Render, "Arena of {} pages of {} MiB exceeds the driver buffer limit "
                    "(request {:#x}+{} MiB)", num_pages, arena_page_size >> 20,
                    first_block << block_shift,
                    ((last_block - first_block + 1) << block_shift) >> 20);
    }

    bool any_arena = false;
    for (u64 page = lo; page <= hi; ++page) {
        any_arena |= address_space[page] != nullptr;
    }
    const u64 base_addr = lo << arena_page_bits;
    const u64 total_size = num_pages << arena_page_bits;
    auto* new_arena = &arenas.emplace_back(instance, base_addr, total_size, MemoryType::Sparse);
    if (any_arena) {
        LOG_WARNING(Render, "Migrating arena ({} pages of {} MiB)", num_pages, arena_page_size >> 20);
        const u64 base_block = base_addr >> block_shift;
        const u64 end_block = (base_addr + total_size) >> block_shift;
        auto* bind = BindsForArena(new_arena);
        resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
            const u64 start = std::max(base_block, backing.start);
            const u64 end = std::min(end_block, backing.end);
            bind->binds.push_back(vk::SparseMemoryBind{
                .resourceOffset = (start - base_block) << block_shift,
                .size = (end - start) << block_shift,
                .memory = backing.memory,
                .memoryOffset = backing.offset + ((start - backing.start) << block_shift),
            });
        });
    }
    for (u64 page = lo; page <= hi; ++page) {
        address_space[page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }
    TlgStats::Timer timer{TlgStats::t_resident};

    const vk::MemoryAllocateInfo alloc_info = {
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    const auto device_memory = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));
    TlgStats::arena_resident_bytes.fetch_add(alloc_info.allocationSize, std::memory_order_relaxed);

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            copies.emplace_back(total_size_bytes, addr, size);
            total_size_bytes += size;
        },
        [&] { src_buffer = UploadCopies(arena, copies, total_size_bytes); });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    // Resolve before launching host copies: a copy-thread fault could deadlock submission.
    for (const auto& copy : copies) {
        texture_cache.ResolveReadbacks(copy.dstOffset, copy.size, true, ReadbackSource::BufferUpload);
    }
    TlgStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // tlg: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!TlgToggle::Disabled(TlgToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!TlgToggle::Disabled(TlgToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            TlgCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        TlgCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // tlg: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !TlgToggle::Disabled(TlgToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // tlg: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
