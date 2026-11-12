// tlg: GPU-side assertion failures stop the port with exit code 23, or the more specific
// code a failing site set first (24 = out of memory, 25 = GPU device lost).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"

static std::atomic<int> stop_code{23};

void assert_set_stop_code(int code) {
    stop_code.store(code, std::memory_order_relaxed);
}

void assert_fail_impl() {
    const int code = stop_code.load(std::memory_order_relaxed);
    std::fflush(stdout);
    std::fprintf(stderr, "STOP: GPU library assertion failed, exit code %d (%s; see GPU log above)\n",
                 code, code == 24 ? "out of memory" : code == 25 ? "GPU device lost" : "graphics check");
    std::_Exit(code);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
