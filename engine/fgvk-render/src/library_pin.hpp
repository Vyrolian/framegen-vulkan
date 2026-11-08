/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

namespace fgvk::layer {

/// Keeps the DSO that contains this function mapped for the rest of the process, so a later
/// dlclose by the Vulkan loader cannot unmap code that a detached thread still runs. Once per
/// process; every call returns the same result. A failed pin is logged once to stderr. Returns
/// true when the library is pinned.
bool pinLayerLibrary() noexcept;

}
