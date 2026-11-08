/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "library_pin.hpp"

#include <dlfcn.h>
#include <link.h>

#include <iostream>
#include <mutex>

// The Vulkan loader dlcloses the layer when the instance goes away. A thread that the layer
// leaves running (the pacing writer, the D7 stall watchdog) executes this DSO's code, so an
// unmapped DSO turns its next wake-up into SIGSEGV. An RTLD_NOLOAD reopen takes one more
// reference, and RTLD_NODELETE keeps the mapping through the last dlclose. The handle is never
// closed.

namespace fgvk::layer {
namespace {

std::once_flag pinOnceFlag;
bool pinned{};
void* pinHandle{};

void logFailure(const char* reason) noexcept {
    std::cerr << "framegen-vulkan: layer pin failed: " << reason << '\n';
}

void pinOnce() noexcept {
    Dl_info info{};
    void* extra{};
    if (dladdr1(reinterpret_cast<void*>(&pinLayerLibrary), &info, &extra, RTLD_DL_LINKMAP) == 0 || !extra) {
        logFailure("dladdr found no object");
        return;
    }
    // The program image has an empty name and is never unloaded, and dlopen cannot name it by
    // path (dladdr reports argv[0] for it). It needs no pin.
    if (static_cast<const link_map*>(extra)->l_name[0] == '\0') {
        pinned = true;
        return;
    }
    pinHandle = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
    if (!pinHandle) {
        const char* error = dlerror();
        logFailure(error ? error : "dlopen returned no handle");
        return;
    }
    pinned = true;
}

}

bool pinLayerLibrary() noexcept {
    try {
        std::call_once(pinOnceFlag, pinOnce);
    } catch (...) {
        return false;
    }
    return pinned;
}

}
