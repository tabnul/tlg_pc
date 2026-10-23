/* Windows access-violation dispatch shared with the native regression test. */
#ifndef TLG_FAULT_DISPATCH_WIN_H
#define TLG_FAULT_DISPATCH_WIN_H
#ifdef _WIN32
#include <windows.h>
#include "runtime.h"

static inline LONG tlg_dispatch_gpu_fault(EXCEPTION_POINTERS *info, int gpu_enabled,
                                         int (*gpu_fault)(void *, void *)) {
    /* Speculative workers must abandon a protected guest read before GPU tracking attempts
     * a synchronous readback. The GPU command thread may be waiting for that worker (or a
     * programs_mutex reader it holds), so queuing a GPU command here creates a circular wait.
     * Resume outside the exception dispatcher, as in the Linux signal handler. */
    if (runtime_fault_recover) {
        unsigned long long *buffer = *runtime_fault_recover;
        runtime_fault_recover = NULL;
        CONTEXT *c = info->ContextRecord;
        c->Rsp = ((c->Rsp - 128) & ~(DWORD64)15) - 40;
        c->Rcx = (DWORD64)(uintptr_t)buffer;
        c->Rdx = 1;
        c->Rip = (DWORD64)(uintptr_t)tlg_longjmp;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (gpu_enabled && gpu_fault(info, (void *)info->ExceptionRecord->ExceptionInformation[1])) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
#endif
