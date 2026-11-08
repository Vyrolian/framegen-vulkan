/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "helpers/image_prefix.hpp"
#include "helpers/delta_scratch.hpp"
#include "helpers/temporal_phases.hpp"
#include "helpers/timestamp_upload_cache.hpp"
#include "helpers/completed_frame_profile.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    void require(const bool condition, const std::string& message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    bool approximatelyEqual(const float lhs, const float rhs) {
        return std::abs(lhs - rhs) < 0.000001F;
    }

    size_t applyTimestamps(std::vector<float>& cache,
            const std::vector<float>& timestamps) {
        size_t writes = 0;
        for (size_t i = 0; i < timestamps.size(); ++i) {
            fgvk::backend::uploadChangedTimestamp(
                cache, i, timestamps.at(i),
                [&writes](const float) { ++writes; }
            );
        }
        return writes;
    }

    void testStableAndChangingCounts() {
        std::vector<float> cache{0.25F, 0.5F, 0.75F};

        require(applyTimestamps(cache, {0.5F}) == 1,
            "3-output to 1-output transition did not upload once");
        require(applyTimestamps(cache, {0.5F}) == 0,
            "stable 1-output timestamp uploaded again");
        require(applyTimestamps(cache, {0.25F, 0.5F, 0.75F}) == 1,
            "1-output to 3-output transition uploaded the wrong count");
        require(applyTimestamps(cache, {1.0F / 3.0F, 2.0F / 3.0F}) == 2,
            "3-output to 2-output transition did not refresh both outputs");
        require(applyTimestamps(cache, {0.5F}) == 1,
            "2-output to 1-output transition did not refresh output zero");
    }

    void testFailedWriteDoesNotCommit() {
        std::vector<float> cache{0.5F};
        try {
            fgvk::backend::uploadChangedTimestamp(
                cache, 0, 0.25F,
                [](const float) { throw std::runtime_error("upload failed"); }
            );
            throw std::runtime_error("failed upload unexpectedly returned");
        } catch (const std::runtime_error& error) {
            require(std::string{error.what()} == "upload failed",
                "test caught the wrong upload exception");
        }

        require(approximatelyEqual(cache.at(0), 0.5F),
            "failed upload changed the cached timestamp");
        size_t writes = 0;
        require(fgvk::backend::uploadChangedTimestamp(
            cache, 0, 0.25F,
            [&writes](const float) { ++writes; }
        ), "retry after failed upload was skipped");
        require(writes == 1 && approximatelyEqual(cache.at(0), 0.25F),
            "successful retry did not commit the timestamp");
    }

    void testCommandPhasesPreserveEveryBinding() {
        for (size_t frame = 0; frame < 120; ++frame) {
            const auto phase = frame % fgvk::backend::commandPhaseCount;
            require(phase % 2 == frame % 2, "cached commands select the wrong source image");
            for (const size_t count : fgvk::backend::alphaHistoryCounts)
                require(phase % count == frame % count,
                    "cached commands select the wrong temporal history");
        }
        require(fgvk::backend::commandPhaseCount == 6,
            "model history changed; update the six-phase hardware qualification");
    }

    void testDeltaScratchPreservesLiveInputs() {
        const std::vector<int> gamma0{10, 11, 12};
        const std::vector<int> gamma1{20, 21, 22, 23};
        for (const size_t multiplier : {size_t{1}, size_t{2}}) {
            const auto scratch = fgvk::backend::deltaScratch(gamma0, gamma1, multiplier);
            require(scratch.primary.size() + scratch.spare.size() == 2 * multiplier,
                "Delta1 scratch does not cover every storage binding");
            require(scratch.primary.size() >= multiplier,
                "Delta1 second-half scratch is incomplete");
            require(scratch.primary.data() == gamma0.data(),
                "Delta1 did not reuse Gamma0 images");
            for (const auto& image : scratch.spare)
                require(&image >= gamma1.data() + multiplier,
                    "Delta1 scratch overwrites a still-live Gamma1 input");
        }
        const auto rejects = [](const std::vector<int>& a,
                const std::vector<int>& b, const size_t m) {
            try { static_cast<void>(fgvk::backend::deltaScratch(a, b, m)); }
            catch (const std::invalid_argument&) { return true; }
            return false;
        };
        require(rejects({10, 11}, gamma1, 2), "short Gamma0 scratch was accepted");
        require(rejects(gamma0, {20, 21}, 2), "missing Gamma1 spare was accepted");
        require(rejects(gamma0, gamma1, 0) && rejects(gamma0, gamma1, 3),
            "unsupported scratch multiplier was accepted");
        require(rejects(gamma0, gamma0, 1), "aliased scratch inputs were accepted");
    }

    void testRequiredImagePrefixIsExactAndFailsClosed() {
        const std::vector<int> images{10, 20, 30, 40};
        const auto prefix = fgvk::backend::requiredPrefix(images, 2, "test");
        require(prefix.size() == 2 && prefix.front() == 10 && prefix.back() == 20,
            "required image prefix did not preserve the exact descriptor width");

        try {
            static_cast<void>(fgvk::backend::requiredPrefix(images, 5, "test"));
            throw std::runtime_error("oversized image prefix unexpectedly succeeded");
        } catch (const std::invalid_argument& error) {
            require(std::string{error.what()} == "test image set is too small",
                "required image prefix reported the wrong failure");
        }
    }
}

int main() {
    try {
        using fgvk::backend::CompletedFrameProfile;
        using fgvk::backend::FrameProfile;
        size_t reads{};
        const auto read = [&] { ++reads; return FrameProfile{.gpuSpanUs = 2000}; };
        CompletedFrameProfile off;
        off.collectAfterCompletion(1, true, read);
        require(reads == 0 && !off.take(), "disabled GPU collection read a query");
        CompletedFrameProfile stream(true);
        stream.collectAfterCompletion(0, true, read);
        stream.collectAfterCompletion(1, false, read);
        require(reads == 0, "history or startup fabricated a generated batch");
        stream.collectAfterCompletion(2, true, read);
        auto sample = stream.take();
        require(reads == 1 && sample && sample->gpuSpanUs == 2000 && !stream.take(),
            "completed profile was lost or consumed twice");
        stream.collectAfterCompletion(2, true, read);
        require(reads == 1 && !stream.take(), "a retry duplicated a completed GPU batch");
        stream.collectAfterCompletion(3, true, read);
        stream.collectAfterCompletion(4, true, read);
        require(reads == 3 && stream.take() && !stream.take(), "profile storage did not stay bounded");
        try {
            stream.collectAfterCompletion(5, true, [&]() -> FrameProfile {
                ++reads; throw std::runtime_error("query unavailable");
            });
        } catch (const std::runtime_error&) {}
        stream.collectAfterCompletion(6, true, read);
        require(reads == 4 && !stream.take(), "failed diagnostic readback was retried indefinitely");
        testStableAndChangingCounts();
        testFailedWriteDoesNotCommit();
        testRequiredImagePrefixIsExactAndFailsClosed();
        testDeltaScratchPreservesLiveInputs();
        testCommandPhasesPreserveEveryBinding();
    } catch (const std::exception& error) {
        std::cerr << "Backend hot-path test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
