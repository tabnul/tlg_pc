/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TLG_INPUT_TIMING_H
#define TLG_INPUT_TIMING_H
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Button and stick hold counts at any frame rate (01.03 eboot). Based on b3ndoi's 60 FPS input
 * timing for EU 01.00.
 *
 * CharacterSensorExt::update adds 1 to each held button's count per update, unlike the seconds
 * timers next to it, which add dt. The boy's animation graph and the guide scripts compare these
 * counts with limits made for 30 updates per second (Cross >4, Circle/R2 >6, R1 >10, the guide's
 * Circle >10), so at 60 FPS a hold is recognised in half the time, at 45 FPS in two thirds.
 *
 * The update publishes a snapshot for the graph (memcpy of the live block, inside the sensor
 * lock). Right after that memcpy the snapshot's button counts and stick hold ages are replaced
 * with 30 Hz-equivalent counts from the update's dt. The live counts, axes, bits, seconds timers
 * and stamina stay native. At 30 FPS the counts are the game's own.
 */
enum {
    TLG_INPUT_UPDATE = 0x72d170,         /* CharacterSensorExt::update(sensor, dt) */
    TLG_INPUT_COPY = 0x72edb0,           /* call memcpy(sensor+0x38b0, sensor+0xd20, 0x268) */
    TLG_INPUT_ENTRY_SIZE = 8,            /* push rbp; mov rbp,rsp; push r15; push r14 */
    TLG_INPUT_COPY_SIZE = 5,
    TLG_INPUT_BUTTONS = 17,
    TLG_INPUT_LIVE = 0xd20,
    TLG_INPUT_LIVE_COUNTS = 0xd38,
    TLG_INPUT_REMAP_COUNTS = 0xe60,
    TLG_INPUT_SNAPSHOT = 0x38b0,
    TLG_INPUT_SNAPSHOT_COUNTS = 0x38c8,
    TLG_INPUT_SNAPSHOT_REMAP = 0x39f0,
    TLG_INPUT_SNAPSHOT_SIZE = 0x268,
    TLG_INPUT_STICKS = 6,
    TLG_INPUT_STICK_LIVE = 0x50,
    TLG_INPUT_STICK_SNAPSHOT = 0x2be0,
    TLG_INPUT_STICK_STRIDE = 0x1f0,
    TLG_INPUT_STICK_AGE = 0x1e0,
    TLG_INPUT_TRAMPOLINE = 0,
    TLG_INPUT_ENTRY_RELAY = 32,
    TLG_INPUT_COPY_RELAY = 64,
    TLG_INPUT_STUB_SIZE = 78
};

/* The code the hook relies on, as FNV-1a 64 of the exact bytes (tests/test_input_timing.py
 * recomputes them from out/eboot.elf). Any difference, a patch included, leaves the game as is.
 */
typedef struct { uint32_t address, size; uint64_t fnv; } TlgInputCode;
static const TlgInputCode tlg_input_code[] = {
    {0x72d170, 0x38, 0xb69798ce68801fcbull},  /* prologue, r13 = sensor, dt saved */
    {0x72e953, 0x07, 0x9956f6a4ca4ad10dull},  /* r14 = sensor + 0xd20 */
    {0x72e9f0, 0x68, 0x1df2663d51a53436ull},  /* the hold count loop (+1 per update) */
    {0x72eb08, 0x07, 0x41910aea506cd6afull},  /* rbx = sensor + 0x2be0 */
    {0x72ebe4, 0x6b, 0x5b43c1976bb59c0aull},  /* the six stick snapshot copies */
    {0x72ec58, 0x15d, 0x3a60a6d7e528fd47ull}, /* snapshot memcpy: arguments through the call */
    {0x7303d0, 0x08, 0xfd18b9a3686afb6aull},  /* stick update: hold age load */
    {0x731638, 0x0b, 0x9fed0f8e750fb647ull},  /* +1 */
    {0x731750, 0x08, 0x97085dbebca684b8ull},  /* store */
    {0x731fb5, 0x11, 0x59acbe625b961d8dull},  /* stick snapshot copy takes the age along */
};

static inline uint64_t tlg_input_fnv(const unsigned char *bytes, size_t size) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

typedef struct { uint32_t previous; double ticks; } TlgInputHold;

static inline int32_t tlg_input_hold_step(TlgInputHold *hold, uint32_t raw, float dt) {
    if (!raw) { hold->previous = 0; hold->ticks = 0; return 0; }
    /* A new press is count 1 at every rate; after that the count follows the elapsed time in
     * 30 Hz steps (60 Hz: 1,1,2,2,3,3...). A repeated update or dt 0 does not age a hold, and
     * NaN, infinite or negative dt cannot overflow the count.
     */
    if (!hold->previous || raw < hold->previous) hold->ticks = 1.0;
    else if (raw > hold->previous && dt > 0.0f && dt <= 3.402823466e38f) {
        hold->ticks += 30.0 * (double)dt;
        if (hold->ticks > INT32_MAX) hold->ticks = INT32_MAX;
    }
    hold->previous = raw;
    double rounded = hold->ticks + 0.00001; /* float(1/60) accumulated roundoff */
    return rounded >= INT32_MAX ? INT32_MAX : (int32_t)rounded;
}

static inline void tlg_input_abs_jump(unsigned char *out, uintptr_t target) {
    static const unsigned char jump[] = {0xff,0x25,0,0,0,0};
    memcpy(out, jump, sizeof(jump)); memcpy(out + sizeof(jump), &target, sizeof(target));
}
/* Trampoline (the update's first 8 bytes, then back), and the two relays to the C hooks. */
static inline void tlg_input_build_stubs(unsigned char *out, const unsigned char *update,
                                         uintptr_t entry, uintptr_t copy) {
    memset(out, 0xcc, TLG_INPUT_STUB_SIZE);
    memcpy(out, update, TLG_INPUT_ENTRY_SIZE);
    tlg_input_abs_jump(out + TLG_INPUT_ENTRY_SIZE, (uintptr_t)(update + TLG_INPUT_ENTRY_SIZE));
    tlg_input_abs_jump(out + TLG_INPUT_ENTRY_RELAY, entry);
    tlg_input_abs_jump(out + TLG_INPUT_COPY_RELAY, copy);
}
static inline int tlg_input_rel_branch(unsigned char *out, uintptr_t from,
                                       uintptr_t to, size_t size, int call) {
    int64_t delta = (int64_t)to - (int64_t)(from + 5);
    if (size < 5 || delta < INT32_MIN || delta > INT32_MAX) return 0;
    int32_t relative = (int32_t)delta;
    out[0] = call ? 0xe8 : 0xe9;
    memcpy(out + 1, &relative, sizeof(relative));
    memset(out + 5, 0x90, size - 5);
    return 1;
}

#ifdef _WIN32
#define TLG_INPUT_ABI __attribute__((sysv_abi))
typedef void (TLG_INPUT_ABI *TlgInputUpdate)(void *, float);
void tlg_input_configure(TlgInputUpdate original);
void TLG_INPUT_ABI tlg_input_update(void *sensor, float dt);
void *TLG_INPUT_ABI tlg_input_copy(void *destination, const void *source, size_t size);
#endif
#endif
