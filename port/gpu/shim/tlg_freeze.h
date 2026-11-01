// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: freeze report support (src/runtime_freeze.c). Frame counters the report reads, and the
// blocking calls of the GPU library marked in the runtime's per-thread wait slots through hooks
// the loader sets at boot (tlggpu_set_wait_hooks; unset: nothing is marked).

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace TlgFreeze {
using Begin = void (*)(const char* kind, const void* object, const char* name, std::uintptr_t frame);
using End = void (*)();
inline std::atomic<Begin> begin_hook{nullptr};
inline std::atomic<End> end_hook{nullptr};

/// Flip requests from the game, flips handed to the presenter, presents done, vblank ticks.
inline std::atomic<std::uint64_t> flips_submitted{0}, flips_shown{0}, presents{0}, vblanks{0};
/// Page-protection faults being handled right now (any thread), and handled so far.
inline std::atomic<std::uint32_t> faults_in_progress{0};
inline std::atomic<std::uint64_t> faults_handled{0};
/// The game executable's address range (tlggpu_set_guest_image; 0 until the loader sets it).
inline std::atomic<std::uintptr_t> guest_base{0};
inline std::atomic<std::uint64_t> guest_size{0};

/// Marks the calling thread as blocked in `kind` until the scope ends.
class WaitScope {
public:
    WaitScope(const char* kind, const void* object = nullptr, const char* name = nullptr,
              std::uintptr_t frame = 0) {
        if (const Begin begin = begin_hook.load(std::memory_order_relaxed)) {
            begin(kind, object, name, frame);
            active = true;
        }
    }
    ~WaitScope() {
        if (active) {
            end_hook.load(std::memory_order_relaxed)();
        }
    }
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;

private:
    bool active = false;
};

/// tlg: prints a SLOW: line for a wait of 50 ms or more (tlggpu.cpp).
void NoteSlow(const char* what, std::uintptr_t address, double ms);

/// A marked wait that also reports itself when it took long. Diagnostics.
class SlowScope {
public:
    explicit SlowScope(const char* what_, std::uintptr_t address_ = 0)
        : wait{what_, reinterpret_cast<const void*>(address_)}, what{what_}, address{address_},
          start{std::chrono::steady_clock::now()} {}
    ~SlowScope() {
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start).count();
        if (ms >= 50.0) {
            NoteSlow(what, address, ms);
        }
    }
    SlowScope(const SlowScope&) = delete;
    SlowScope& operator=(const SlowScope&) = delete;

private:
    WaitScope wait;
    const char* what;
    std::uintptr_t address;
    std::chrono::steady_clock::time_point start;
};
} // namespace TlgFreeze
