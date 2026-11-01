// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <deque>
#include <mutex>
#include <type_traits>

#include "common/types.h"
#include "tlg_freeze.h"

namespace Common {

/// tlg: TLG_GC_FIX bit 1 (on by default). ForEachItemBelow never honoured a callback's stop
/// request (the trait compared std::invoke_result, not its ::type, with bool), so every collector
/// pass walked the whole queue. TLG_GC_FIX=0 keeps the old behaviour for A/B runs.
inline bool lru_stop_early = true;

/// tlg: Touch comes from the GPU threads without the texture cache lock, Free and Insert from any
/// thread that holds it. TLG_LRU_LOCK=0 = the old way.
inline const bool lru_locked = [] {
    const char* env = std::getenv("TLG_LRU_LOCK");
    return !env || env[0] != '0';
}();

template <typename ObjectType, typename TickType>
class LeastRecentlyUsedCache {
    struct Item {
        ObjectType obj;
        TickType tick;
        Item* next{};
        Item* prev{};
        bool live{};
    };

public:
    LeastRecentlyUsedCache() : first_item{}, last_item{} {}
    ~LeastRecentlyUsedCache() = default;

    size_t Insert(ObjectType obj, TickType tick) {
        const auto lock = Lock();
        const auto new_id = Build();
        auto& item = item_pool[new_id];
        item.live = true;
        item.obj = obj;
        item.tick = tick;
        Attach(item);
        return new_id;
    }

    void Touch(size_t id, TickType tick) {
        const auto lock = Lock();
        auto& item = item_pool[id];
        // A late touch of a freed image must not put it back in the list.
        if (lru_locked && !item.live) {
            return;
        }
        if (item.tick >= tick) {
            return;
        }
        item.tick = tick;
        if (&item == last_item) {
            return;
        }
        Detach(item);
        Attach(item);
    }

    void Free(size_t id) {
        const auto lock = Lock();
        auto& item = item_pool[id];
        item.live = false;
        Detach(item);
        item.prev = nullptr;
        item.next = nullptr;
        free_items.push_back(id);
    }

    template <typename Func>
    void ForEachItemBelow(TickType tick, Func&& func) {
        static constexpr bool RETURNS_BOOL =
            std::is_same_v<std::invoke_result_t<Func, ObjectType>, bool>;
        // The callback frees items and may wait for other threads, so it runs outside the lock.
        // Items live in a deque and only a Touch can move one meanwhile: the walk then ends early
        // at a newer item, which the next pass picks up.
        auto lock = Lock();
        Item* iterator = first_item;
        while (iterator) {
            if (static_cast<s64>(tick) - static_cast<s64>(iterator->tick) < 0) {
                return;
            }
            Item* next = iterator->next;
            const ObjectType obj = iterator->obj;
            if (lock.owns_lock()) {
                lock.unlock();
            }
            if constexpr (RETURNS_BOOL) {
                if (func(obj) && lru_stop_early) {
                    return;
                }
            } else {
                func(obj);
            }
            lock = Lock();
            iterator = next;
        }
    }

private:
    [[nodiscard]] std::unique_lock<std::mutex> Lock() {
        std::unique_lock<std::mutex> lock{mutex, std::defer_lock};
        if (lru_locked && !lock.try_lock()) {
            TlgFreeze::SlowScope slow{"lru lock"};
            lock.lock();
        }
        return lock;
    }

    size_t Build() {
        if (free_items.empty()) {
            const size_t item_id = item_pool.size();
            auto& item = item_pool.emplace_back();
            item.next = nullptr;
            item.prev = nullptr;
            return item_id;
        }
        const size_t item_id = free_items.front();
        free_items.pop_front();
        auto& item = item_pool[item_id];
        item.next = nullptr;
        item.prev = nullptr;
        return item_id;
    }

    void Attach(Item& item) {
        if (!first_item) {
            first_item = &item;
        }
        if (!last_item) {
            last_item = &item;
        } else {
            item.prev = last_item;
            last_item->next = &item;
            item.next = nullptr;
            last_item = &item;
        }
    }

    void Detach(Item& item) {
        if (item.prev) {
            item.prev->next = item.next;
        }
        if (item.next) {
            item.next->prev = item.prev;
        }
        if (&item == first_item) {
            first_item = item.next;
            if (first_item) {
                first_item->prev = nullptr;
            }
        }
        if (&item == last_item) {
            last_item = item.prev;
            if (last_item) {
                last_item->next = nullptr;
            }
        }
    }

    std::deque<Item> item_pool;
    std::deque<size_t> free_items;
    Item* first_item{};
    Item* last_item{};
    std::mutex mutex;
};

} // namespace Common
