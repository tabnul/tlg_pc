/* Hold counts at the 30 FPS pace (src/input_timing.h). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../src/input_timing.h"

/* Feeds the game's own count (+1 per update while held) and returns the published count. */
static int32_t hold(TlgInputHold *h, uint32_t *raw, float dt) {
    ++*raw;
    return tlg_input_hold_step(h, *raw, dt);
}

int main(void) {
    TlgInputHold h = {0};
    uint32_t raw = 0;
    /* 30 FPS: the game's own counts. */
    for (int32_t i = 1; i <= 100; ++i) assert(hold(&h, &raw, 1.0f / 30.0f) == i);
    /* Release resets. */
    assert(tlg_input_hold_step(&h, 0, 1.0f / 30.0f) == 0);
    raw = 0;
    /* 60 FPS: 1,1,2,2,3,3... (a press is 1 at once). */
    const int32_t sixty[] = {1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6};
    for (unsigned i = 0; i < sizeof(sixty) / sizeof(sixty[0]); ++i) assert(hold(&h, &raw, 1.0f / 60.0f) == sixty[i]);
    /* Cross >4 needs the same 133 ms at 60 FPS as 4 updates at 30 FPS: count 5 after 9 updates. */
    TlgInputHold c = {0}; uint32_t craw = 0; int updates = 0;
    while (hold(&c, &craw, 1.0f / 60.0f) <= 4) ++updates;
    assert(updates == 8);
    /* Variable frame rate (45 FPS): the count follows the elapsed time. */
    TlgInputHold v = {0}; uint32_t vraw = 0; int32_t last = 0;
    for (int i = 0; i < 45; ++i) last = hold(&v, &vraw, 1.0f / 45.0f);
    assert(last == 30);
    /* A repeated update (same raw count) or a bad dt does not age the hold. */
    TlgInputHold r = {0};
    assert(tlg_input_hold_step(&r, 1, 1.0f / 60.0f) == 1);
    assert(tlg_input_hold_step(&r, 1, 1.0f / 60.0f) == 1);
    assert(tlg_input_hold_step(&r, 2, -1.0f) == 1);
    assert(tlg_input_hold_step(&r, 3, NAN) == 1);
    assert(tlg_input_hold_step(&r, 4, INFINITY) == 1);
    /* A count that went down is a new press. */
    assert(tlg_input_hold_step(&r, 1, 1.0f) == 1);
    /* No overflow. */
    TlgInputHold big = {0};
    assert(tlg_input_hold_step(&big, 1, 1.0f) == 1);
    assert(tlg_input_hold_step(&big, 2, 3.0e38f) == INT32_MAX);
    /* Stubs: the trampoline keeps the update's first 8 bytes and returns after them. */
    unsigned char update[16] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56}, stubs[TLG_INPUT_STUB_SIZE];
    tlg_input_build_stubs(stubs, update, 0x1111, 0x2222);
    uintptr_t back; memcpy(&back, stubs + TLG_INPUT_ENTRY_SIZE + 6, 8);
    assert(!memcmp(stubs, update, 8) && back == (uintptr_t)(update + 8));
    unsigned char branch[8];
    assert(tlg_input_rel_branch(branch, 0x1000, 0x2000, sizeof(branch), 0) && branch[0] == 0xe9 && branch[7] == 0x90);
    assert(!tlg_input_rel_branch(branch, 0, (uintptr_t)1 << 40, sizeof(branch), 1));
    puts("input timing: ok");
    return 0;
}
