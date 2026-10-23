/* Real Windows protection faults: speculative recovery must precede GPU readback. */
#include <assert.h>
#include <stdio.h>
#include "fault_dispatch_win.h"

__thread sigjmp_buf *runtime_fault_recover;
static volatile LONG gpu_calls;
static volatile unsigned char *page;

static int gpu_readback(void *context, void *address) {
    (void)context;
    assert(address == (void *)page);
    ++gpu_calls;
    DWORD old;
    assert(VirtualProtect((void *)page, 4096, PAGE_READWRITE, &old));
    return 1;
}

static LONG CALLBACK dispatch(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2) {
        return tlg_dispatch_gpu_fault(info, 1, gpu_readback);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void speculative_read(void) {
    sigjmp_buf recover;
    if (!sigsetjmp(recover, 0)) {
        runtime_fault_recover = &recover;
        (void)*page;
        assert(!"speculative read must recover, not invoke GPU readback");
    }
    assert(runtime_fault_recover == NULL);
    assert(gpu_calls == 0);
}

int main(void) {
    page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(page);
    *page = 42;
    DWORD old;
    assert(VirtualProtect((void *)page, 4096, PAGE_NOACCESS, &old));
    void *handler = AddVectoredExceptionHandler(1, dispatch);
    assert(handler);
    for (int i = 0; i < 1000; ++i) speculative_read();
    /* Without a speculative recovery point, ordinary GPU tracking must still run. */
    assert(*page == 42);
    assert(gpu_calls == 1);
    assert(RemoveVectoredExceptionHandler(handler));
    assert(VirtualFree((void *)page, 0, MEM_RELEASE));
    puts("PASS: 1000 speculative faults recovered before GPU readback; ordinary GPU fault handled");
    return 0;
}
