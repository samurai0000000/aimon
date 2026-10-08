/*
 * TestSanitizerCanary.cxx
 *
 * Deliberately broken program. It is built and run only by
 * `make test SANITIZE=1 SANITIZE_CANARY=1` and must make that run fail:
 * a heap buffer overflow (AddressSanitizer) followed by signed integer
 * overflow (UBSan). If this program exits 0 the sanitizers are not active.
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <climits>
#include <cstdio>

int main(int argc, char** argv) {
    (void)argv;
    int* buffer = new int[4];
    volatile int index = 3 + argc;      // argc >= 1, so index >= 4: out of range
    buffer[index] = 1;                  // heap-buffer-overflow
    delete[] buffer;

    volatile int big = INT_MAX;
    volatile int wrapped = big + argc;  // signed integer overflow
    std::printf("canary survived: %d\n", wrapped);
    return 0;
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
