# framegen-vulkan third-party notices

This document supplements [LICENSE.md](LICENSE.md). It identifies third-party material distributed with MAKO and proprietary software that MAKO can use but does not distribute. Each third-party copyright, license, and trademark remains with its owner.

## Lossless Scaling is not distributed by framegen-vulkan

framegen-vulkan is an independent open-source project. framegen-vulkan source archives, framegen-vulkan packages, and Flatpak extensions do not contain or distribute Lossless Scaling, `Lossless.dll`, or extracted LS1 or LSFG model and shader payloads.

The open framegen-vulkan Scaler works without Lossless Scaling. A user who chooses LSFG frame generation or LS1 scaling must independently obtain a lawful copy of Lossless Scaling and select or permit discovery of its locally installed `Lossless.dll`. The user is responsible for complying with the terms and law applicable to that copy. framegen-vulkan grants no license or other rights in Lossless Scaling or its proprietary resources.

framegen-vulkan reads the user-supplied local DLL at runtime for LSFG or LS1. It may translate shaders for Vulkan or, in experimental builds, convert eligible LSFG shader arithmetic from FP32 to FP16. Extracted and transformed resources stay in process memory. framegen-vulkan does not install, upload, package, or alter the DLL, or save those resources as standalone files.

Lossless Scaling, LS1, and LSFG are used descriptively and remain the property of their respective owner. framegen-vulkan is not affiliated with or endorsed by Lossless Scaling.

## Incorporated Renderer source

The complete notices for the GPL-3.0-or-later lsfg-vk version 2 Renderer lineage, toml++, and Bjoern Hoehrmann's UTF-8 decoder are reproduced in [LICENSE.md](LICENSE.md).

### lsfg-vk Renderer provenance

MAKO Renderer descends directly from PancakeTAS's GPL-3.0-or-later lsfg-vk version 2 tree at upstream commit [`8b0da2661c6f3473a7fccc8ba643880050e71642`](https://github.com/PancakeTAS/lsfg-vk/commit/8b0da2661c6f3473a7fccc8ba643880050e71642). MAKO's experimental predecessor carried that baseline to `v2.0.0-dev28-experimental.25` at [`276030d4925c40038a61ecd66bd49ce777faec8c`](https://github.com/eugeniosegala/lsfg-vk-experimental/commit/276030d4925c40038a61ecd66bd49ce777faec8c), and that Renderer lineage entered this monorepo in [`8ed1cdbe7f5496fade8ad01d85ac3d671957fcac`](https://github.com/eugeniosegala/MAKO/commit/8ed1cdbe7f5496fade8ad01d85ac3d671957fcac).

Copyright in the incorporated portions remains with the respective lsfg-vk copyright holders. The incorporated source files identify themselves as `GPL-3.0-or-later`, and MAKO remains distributed under that license. The MIT License used for lsfg-vk version 1 does not describe MAKO Renderer's version 2 lineage.

## Gamescope surface protocol

[`engine/gamescope-compatibility.json`](engine/gamescope-compatibility.json) owns the protocol revision/hash and compatibility review record; [ongoing maintenance](MAINTENANCE.md) describes how to update it alongside this notice.

MAKO Renderer's optional scaling surface adapter declares the association, swapchain feedback, HDR and version-one/version-two presentation timing requests and events from Valve's [Gamescope swapchain protocol at commit `2b5cf191d1047d6895516a834de8a72ac1c342f3`](https://github.com/ValveSoftware/gamescope/blob/2b5cf191d1047d6895516a834de8a72ac1c342f3/protocol/gamescope-swapchain.xml). This protocol material carries the following MIT notice:

Copyright © 2023 Joshua Ashton for Valve Software

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next paragraph) shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Wayland presentation feedback protocol

MAKO Renderer's optional native Wayland diagnostic observer declares the version-one requests and events of the stable [presentation-time protocol](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/main/stable/presentation-time/presentation-time.xml). The protocol material carries the following MIT notice:

Copyright © 2013-2014 Collabora, Ltd.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next paragraph) shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Experimental LSFG FP16 optimizer

The optional native FP16 experiment statically links official [SPIRV-Tools at commit `33e02568181e3312f49a3cf33df470bf96ef293a`](https://github.com/KhronosGroup/SPIRV-Tools/tree/33e02568181e3312f49a3cf33df470bf96ef293a), copyright © 2015–2025 The Khronos Group Inc. and its contributors, under the Apache License 2.0. Its generated tables use [SPIRV-Headers at commit `2a611a970fdbc41ac2e3e328802aed9985352dca`](https://github.com/KhronosGroup/SPIRV-Headers/tree/2a611a970fdbc41ac2e3e328802aed9985352dca), under the Khronos permissive terms recorded in that repository. The experimental CMake install includes their complete license files under `share/doc/mako-render/spirv-tools/` and `share/doc/mako-render/spirv-headers/`. The checksum-pinned source preparation is owned by `engine/scripts/prepare-lsfg-fp16-tools.sh`; ordinary builds do not include this optimizer. Neither this dependency nor conversion redistributes Lossless Scaling resources.

## Project artwork

framegen-vulkan's project artwork and the provenance limits recorded for it are documented in [ASSET_PROVENANCE.md](ASSET_PROVENANCE.md). Third-party brand assets are not relicensed by framegen-vulkan.
