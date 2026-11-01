// tlg: memory and switches behind Common::SlotVector, out of the header so that it stays free of
// windows.h. The image lists used to move to a new block each time they grew; here they keep their
// address for the whole run.
#include <cstdlib>

#include "common/assert.h"
#include "common/slot_vector.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace Common::SlotStorage {

namespace {

bool SwitchOn(const char* name) {
    const char* env = std::getenv(name);
    return !env || env[0] != '0';
}

} // namespace

void* Reserve(std::size_t bytes) {
#ifdef _WIN32
    // Top down: the low addresses belong to the guest.
    void* base = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS);
#else
    void* base = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) {
        base = nullptr;
    }
#endif
    ASSERT_MSG(base != nullptr, "Slot storage: no address space for {} bytes", bytes);
    return base;
}

void Commit(void* base, std::size_t bytes) {
#ifdef _WIN32
    const bool ok = VirtualAlloc(base, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    const bool ok = mprotect(base, bytes, PROT_READ | PROT_WRITE) == 0;
#endif
    ASSERT_MSG(ok, "Slot storage: no memory for {} bytes", bytes);
}

void Release(void* base, [[maybe_unused]] std::size_t bytes) {
#ifdef _WIN32
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, bytes);
#endif
}

bool StableEnabled() {
    static const bool enabled = SwitchOn("TLG_SLOT_STABLE");
    return enabled;
}

bool LockEnabled() {
    static const bool enabled = SwitchOn("TLG_SLOT_LOCK");
    return enabled;
}

} // namespace Common::SlotStorage
