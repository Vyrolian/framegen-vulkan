/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "helpers/frame_color_policy.hpp"

#include <cstdlib>
#include <iostream>

using namespace fgvk::backend;

int main() {
    for (const auto encoding : {FrameEncoding::Sdr8, FrameEncoding::SdrHighPrecision,
            FrameEncoding::ScRgbLinear, FrameEncoding::Hdr10Pq}) {
        const bool pq = encoding == FrameEncoding::Hdr10Pq;
        if (requiresPqConversion(encoding) != pq ||
                usesHdrModel(encoding) != (encoding == FrameEncoding::ScRgbLinear || pq)) {
            std::cerr << "HDR colour policy changed an encoding's conversion or model constants\n";
            return EXIT_FAILURE;
        }
    }
    std::cout << "frame colour policy tests passed\n";
    return EXIT_SUCCESS;
}
