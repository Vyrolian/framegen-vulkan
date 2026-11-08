/* SPDX-License-Identifier: GPL-3.0-or-later */

// The same source builds two programs. Built with FGVK_LIBRARY_PIN_MODULE it is the module: it
// pins its own DSO, then starts a detached thread that keeps running code inside that DSO. Built
// without it, it is the test executable: it loads the module, calls the export, unloads the module
// and waits. Without the pin the detached thread wakes into unmapped code and dies with SIGSEGV.

#include <chrono>
#include <iostream>
#include <thread>

#ifdef FGVK_LIBRARY_PIN_MODULE

#include "library_pin.hpp"

extern "C" int startPinnedThread();

extern "C" int startPinnedThread() {
    if (!fgvk::layer::pinLayerLibrary()) return 0;
    std::thread([] {
        for (;;) std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }).detach();
    return 1;
}

#else

#include <dlfcn.h>

namespace {
using StartPinnedThread = int (*)();

int fail(const char* message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: fgvk-library-pin-tests <library-pin-module>\n";
        return 2;
    }
    void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        const char* error = dlerror();
        std::cerr << "FAIL: dlopen: " << (error ? error : "unknown error") << '\n';
        return 1;
    }
    const auto start = reinterpret_cast<StartPinnedThread>(dlsym(module, "startPinnedThread"));
    if (!start) {
        dlclose(module);
        return fail("startPinnedThread is missing from the module");
    }
    if (start() != 1) {
        dlclose(module);
        return fail("pinLayerLibrary did not succeed in the module");
    }
    if (dlclose(module) != 0) return fail("dlclose of the module failed");
    // The detached thread wakes every 20 ms. Several wake-ups after the unload expose an unmapped DSO.
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    return 0;
}

#endif
