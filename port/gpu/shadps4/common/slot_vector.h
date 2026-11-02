// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <numeric>
#include <utility>
#include <vector>
#include "common/assert.h"
#include "common/types.h"
#include "tlg_freeze.h"

namespace Common {

struct SlotId {
    static constexpr u32 INVALID_INDEX = std::numeric_limits<u32>::max();

    SlotId() noexcept = default;
    constexpr SlotId(u32 index) noexcept : index(index) {}

    constexpr auto operator<=>(const SlotId&) const noexcept = default;

    constexpr explicit operator bool() const noexcept {
        return index != INVALID_INDEX;
    }

    u32 index = INVALID_INDEX;
};

// tlg: shim/slot_storage.cpp.
namespace SlotStorage {
void* Reserve(std::size_t bytes);
void Commit(void* base, std::size_t bytes);
void Release(void* base, std::size_t bytes);
bool StableEnabled(); // TLG_SLOT_STABLE=0: the block moves when the list grows (the old way)
bool LockEnabled();   // TLG_SLOT_LOCK=0: no lock around the free list and the bits (the old way)
} // namespace SlotStorage

template <class T>
class SlotVector {
    constexpr static std::size_t InitialCapacity = 2048;
    // tlg: address space only (texture cache high so far: 26k images in 30 s).
    constexpr static std::size_t MaxCapacity = std::size_t(1) << 21;

public:
    template <typename ValueType, typename Pointer, typename Reference>
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = ValueType;
        using difference_type = std::ptrdiff_t;
        using pointer = Pointer;
        using reference = Reference;

        Iterator(SlotVector& vector_, SlotId index_) : vector(vector_), slot(index_) {
            AdvanceToValid();
        }

        reference operator*() const {
            return vector[slot];
        }

        pointer operator->() const {
            return &vector[slot];
        }

        Iterator& operator++() {
            ++slot.index;
            AdvanceToValid();
            return *this;
        }

        Iterator operator++(int) {
            Iterator temp = *this;
            ++(*this);
            return temp;
        }

        bool operator==(const Iterator& other) const {
            return slot == other.slot;
        }

        bool operator!=(const Iterator& other) const {
            return !(*this == other);
        }

    private:
        void AdvanceToValid() {
            while (slot < vector.values_capacity && !vector.ReadStorageBit(slot.index)) {
                ++slot.index;
            }
        }

        SlotVector& vector;
        SlotId slot;
    };

    using iterator = Iterator<T, T*, T&>;
    using const_iterator = Iterator<const T, const T*, const T&>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    SlotVector() {
        stable = SlotStorage::StableEnabled();
        if (stable) {
            // tlg: the block, the bits and the free list never move, so references and unlocked
            // reads stay good while the list grows.
            values = static_cast<Entry*>(SlotStorage::Reserve(MaxCapacity * sizeof(Entry)));
            stored_bitset.reserve((MaxCapacity + 63) / 64);
            free_list.reserve(MaxCapacity);
        }
        Reserve(InitialCapacity);
    }

    ~SlotVector() noexcept {
        std::size_t index = 0;
        for (u64 bits : stored_bitset) {
            for (std::size_t bit = 0; bits; ++bit, bits >>= 1) {
                if ((bits & 1) != 0) {
                    values[index + bit].object.~T();
                }
            }
            index += 64;
        }
        if (stable) {
            SlotStorage::Release(values, MaxCapacity * sizeof(Entry));
        } else {
            delete[] values;
        }
    }

    [[nodiscard]] T& operator[](SlotId id) noexcept {
        ValidateIndex(id);
        return values[id.index].object;
    }

    [[nodiscard]] const T& operator[](SlotId id) const noexcept {
        ValidateIndex(id);
        return values[id.index].object;
    }

    bool is_allocated(SlotId id) const {
        // spartanport: reject out-of-range IDs before reading the allocation bitset.
        return (id.index / 64) < stored_bitset.size() && ReadStorageBit(id.index);
    }

    template <typename... Args>
    SlotId insert(Args&&... args) noexcept {
        u32 index;
        {
            const auto lock = ListLock();
            index = FreeValueIndex();
        }
        new (&values[index].object) T(std::forward<Args>(args)...);
        const auto lock = ListLock();
        SetStorageBit(index);

        return SlotId{index};
    }

    void erase(SlotId id) noexcept {
        values[id.index].object.~T();
        const auto lock = ListLock();
        free_list.push_back(id.index);
        ResetStorageBit(id.index);
    }

    std::size_t size() const noexcept {
        return values_capacity - free_list.size();
    }

    iterator begin() noexcept {
        return iterator(*this, 0);
    }

    const_iterator begin() const noexcept {
        return const_iterator(*this, 0);
    }

    const_iterator cbegin() const noexcept {
        return begin();
    }

    iterator end() noexcept {
        return iterator(*this, values_capacity);
    }

    const_iterator end() const noexcept {
        return const_iterator(*this, values_capacity);
    }

    const_iterator cend() const noexcept {
        return end();
    }

    reverse_iterator rbegin() noexcept {
        return reverse_iterator(end());
    }

    const_reverse_iterator rbegin() const noexcept {
        return const_reverse_iterator(end());
    }

    const_reverse_iterator crbegin() const noexcept {
        return rbegin();
    }

    reverse_iterator rend() noexcept {
        return reverse_iterator(begin());
    }

    const_reverse_iterator rend() const noexcept {
        return const_reverse_iterator(begin());
    }

    const_reverse_iterator crend() const noexcept {
        return rend();
    }

private:
    struct NonTrivialDummy {
        NonTrivialDummy() noexcept {}
    };

    union Entry {
        Entry() noexcept : dummy{} {}
        ~Entry() noexcept {}

        NonTrivialDummy dummy;
        T object;
    };

    // tlg: insert and erase run on different threads: the erase comes from a deferred operation,
    // which holds no texture cache lock. T's constructor and destructor stay outside.
    [[nodiscard]] std::unique_lock<std::mutex> ListLock() noexcept {
        std::unique_lock<std::mutex> lock{list_mutex, std::defer_lock};
        if (SlotStorage::LockEnabled() && !lock.try_lock()) {
            TlgFreeze::SlowScope slow{"image list lock"};
            lock.lock();
        }
        return lock;
    }

    void SetStorageBit(u32 index) noexcept {
        stored_bitset[index / 64] |= u64(1) << (index % 64);
    }

    void ResetStorageBit(u32 index) noexcept {
        stored_bitset[index / 64] &= ~(u64(1) << (index % 64));
    }

    bool ReadStorageBit(u32 index) const noexcept {
        return ((stored_bitset[index / 64] >> (index % 64)) & 1) != 0;
    }

    void ValidateIndex([[maybe_unused]] SlotId id) const noexcept {
        DEBUG_ASSERT(id);
        DEBUG_ASSERT(id.index / 64 < stored_bitset.size());
        DEBUG_ASSERT(((stored_bitset[id.index / 64] >> (id.index % 64)) & 1) != 0);
    }

    [[nodiscard]] u32 FreeValueIndex() noexcept {
        if (free_list.empty()) {
            Reserve(values_capacity ? (values_capacity << 1) : 1);
        }

        const u32 free_index = free_list.back();
        free_list.pop_back();
        return free_index;
    }

    void Reserve(std::size_t new_capacity) noexcept {
        if (stable) {
            ASSERT_MSG(new_capacity <= MaxCapacity, "SlotVector full at {} entries", values_capacity);
            SlotStorage::Commit(values, new_capacity * sizeof(Entry));
        } else {
            MoveTo(new_capacity);
        }

        stored_bitset.resize((new_capacity + 63) / 64);

        const std::size_t old_free_size = free_list.size();
        free_list.resize(old_free_size + (new_capacity - values_capacity));
        const std::size_t new_free_size = free_list.size();
        std::iota(free_list.rbegin(), free_list.rbegin() + new_free_size - old_free_size,
                  static_cast<u32>(values_capacity));

        values_capacity = new_capacity;
    }

    void MoveTo(std::size_t new_capacity) noexcept {
        Entry* const new_values = new Entry[new_capacity];
        std::size_t index = 0;
        for (u64 bits : stored_bitset) {
            for (std::size_t bit = 0; bits; ++bit, bits >>= 1) {
                const std::size_t i = index + bit;
                if ((bits & 1) == 0) {
                    continue;
                }
                T& old_value = values[i].object;
                new (&new_values[i].object) T(std::move(old_value));
                old_value.~T();
            }
            index += 64;
        }
        delete[] values;
        values = new_values;
    }

    Entry* values = nullptr;
    std::size_t values_capacity = 0;

    std::vector<u64> stored_bitset;
    std::vector<u32> free_list;

    std::mutex list_mutex;
    bool stable = false;
};

} // namespace Common

template <>
struct std::hash<Common::SlotId> {
    std::size_t operator()(const Common::SlotId& id) const noexcept {
        return std::hash<u32>{}(id.index);
    }
};
