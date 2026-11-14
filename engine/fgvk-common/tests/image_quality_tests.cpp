/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "fgvk-common/quality/image_quality.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }

    template<typename Callable>
    void expectInvalidArgument(Callable&& callable, const std::string_view message) {
        try {
            callable();
        } catch (const std::invalid_argument&) {
            return;
        }
        expect(false, message);
    }
}

int main() {
    const auto catalog = fgvk::quality::qualitySceneCatalog();
    expect(catalog.size() == 5,
        "the quality catalog must expose all procedural workload families");
    std::set<std::string> sceneNames;
    for (const auto& descriptor : catalog) {
        expect(!descriptor.name.empty() && !descriptor.description.empty(),
            "every quality scene needs a stable name and useful description");
        expect(sceneNames.insert(std::string(descriptor.name)).second,
            "quality scene names must be unique");
        expect(fgvk::quality::qualitySceneFromName(descriptor.name) == descriptor.kind,
            "scene-name parsing must round-trip through the catalog");
        expect(fgvk::quality::qualitySceneName(descriptor.kind) == descriptor.name,
            "scene-kind formatting must round-trip through the catalog");

        const auto catalogScene = fgvk::quality::makeImageQualityRegressionScene(
            descriptor.kind, 0.5F
        );
        const auto repeatedScene = fgvk::quality::makeImageQualityRegressionScene(
            descriptor.kind, 0.5F
        );
        expect(catalogScene.width == 321 && catalogScene.height == 181,
            "every temporal scene must keep odd, non-workgroup-aligned dimensions");
        expect(catalogScene.previous == repeatedScene.previous &&
                catalogScene.current == repeatedScene.current &&
                catalogScene.reference == repeatedScene.reference &&
                catalogScene.focusMask == repeatedScene.focusMask &&
                catalogScene.detailMask == repeatedScene.detailMask,
            "every procedural quality scene must be deterministic");
        expect(catalogScene.previous != catalogScene.current,
            "every temporal scene needs visible endpoint motion");
        expect(std::ranges::count(catalogScene.focusMask, uint8_t{1}) > 500,
            "every temporal scene needs a meaningful motion focus region");
        expect(std::ranges::count(catalogScene.detailMask, uint8_t{1}) > 50,
            "every temporal scene needs explicit fine-detail coverage");
        const auto catalogPerfect = fgvk::quality::evaluateImageQuality(
            catalogScene, catalogScene.reference
        );
        expect(fgvk::quality::passesImageQualityRegression(catalogPerfect),
            "every ideal temporal reference must pass its quality guardrails");
    }
    expect(!fgvk::quality::qualitySceneFromName("unknown-scene").has_value(),
        "unknown scene names must be rejected without an implicit fallback");
    expect(fgvk::quality::imageQualityThresholds(
            fgvk::quality::QualitySceneKind::MotionBoundary
        ).maximumFocusMeanAbsoluteError == 0.075,
        "the legacy motion-boundary focus threshold must remain stable");
    expect(fgvk::quality::imageQualityThresholds(
            fgvk::quality::QualitySceneKind::Traffic
        ).maximumFocusMeanAbsoluteError == 0.11 &&
            fgvk::quality::imageQualityThresholds(
                fgvk::quality::QualitySceneKind::HudDisocclusion
            ).maximumFocusMeanAbsoluteError == 0.14,
        "large-disocclusion scenes must use their calibrated focus guardrails");
    const auto scene = fgvk::quality::makeAmdImageQualityRegressionScene();
    const auto repeated = fgvk::quality::makeAmdImageQualityRegressionScene();
    expect(scene.width == 321 && scene.height == 181,
        "the regression must keep odd, non-workgroup-aligned dimensions");
    expect(scene.previous == repeated.previous && scene.current == repeated.current &&
            scene.reference == repeated.reference,
        "the synthetic regression scene must be deterministic");
    expect(std::ranges::count(scene.focusMask, uint8_t{1}) > 1000,
        "the scene must include a meaningful motion/disocclusion focus region");
    expect(std::ranges::count(scene.detailMask, uint8_t{1}) > 100,
        "the scene must include thin-detail coverage");

    const auto perfect = fgvk::quality::evaluateImageQuality(scene, scene.reference);
    expect(fgvk::quality::passesImageQualityRegression(perfect),
        "the reference midpoint must pass its own quality guardrails");
    expect(perfect.meanAbsoluteError == 0.0 &&
            perfect.focusMeanAbsoluteError == 0.0 &&
            perfect.detailMeanAbsoluteError == 0.0,
        "the reference midpoint must score zero error");

    const auto duplicatedEndpoint = fgvk::quality::evaluateImageQuality(
        scene, scene.current
    );
    expect(!fgvk::quality::passesImageQualityRegression(duplicatedEndpoint),
        "a duplicated endpoint must fail the midpoint ghosting guardrails");

    auto blendedEndpoints = scene.previous;
    for (size_t byte = 0; byte < blendedEndpoints.size(); ++byte) {
        blendedEndpoints[byte] = static_cast<uint8_t>(
            (static_cast<uint16_t>(scene.previous[byte]) + scene.current[byte]) / 2
        );
    }
    const auto doubleImage = fgvk::quality::evaluateImageQuality(
        scene, blendedEndpoints
    );
    expect(!fgvk::quality::passesImageQualityRegression(doubleImage),
        "a blended double image must fail the midpoint ghosting guardrails");

    auto corrupted = scene.reference;
    for (size_t pixel = 0; pixel < scene.focusMask.size(); ++pixel) {
        if (scene.focusMask[pixel] == 0)
            continue;
        const size_t offset = pixel * 4;
        corrupted[offset] = 255;
        corrupted[offset + 1] = 0;
        corrupted[offset + 2] = 255;
    }
    const auto corruption = fgvk::quality::evaluateImageQuality(scene, corrupted);
    expect(!fgvk::quality::passesImageQualityRegression(corruption),
        "large focus-region corruption must fail the regression");

    expect(fgvk::quality::flowScaleOnePreset.flowScale == 1.0F &&
            fgvk::quality::flowScaleOnePreset.multiplier == 2 &&
            !fgvk::quality::flowScaleOnePreset.performanceMode,
        "the quality preset must use full-resolution flow and the quality shader path");
    const float inverseFlow = 1.0F / fgvk::quality::flowScaleOnePreset.flowScale;
    expect(static_cast<uint32_t>(static_cast<float>(scene.width) / inverseFlow) ==
            scene.width &&
            static_cast<uint32_t>(static_cast<float>(scene.height) / inverseFlow) ==
            scene.height,
        "Flow Scale 1.0 must preserve the exact odd source extent");

    const auto explicitLegacy = fgvk::quality::makeImageQualityRegressionScene(
        fgvk::quality::QualitySceneKind::MotionBoundary, 0.5F
    );
    expect(scene.previous == explicitLegacy.previous &&
            scene.current == explicitLegacy.current &&
            scene.reference == explicitLegacy.reference &&
            scene.focusMask == explicitLegacy.focusMask &&
            scene.detailMask == explicitLegacy.detailMask,
        "the legacy AMD entry point must remain the motion-boundary midpoint");

    expectInvalidArgument([] {
        static_cast<void>(fgvk::quality::makeImageQualityRegressionScene(
            fgvk::quality::QualitySceneKind::Traffic, 0.0F
        ));
    }, "temporal endpoints must not be accepted as interpolation targets");
    const auto temporalA = fgvk::quality::makeImageQualityRegressionScene(
        fgvk::quality::QualitySceneKind::Traffic, 321, 181, 0.0F, 0.25F, 0.5F);
    const auto temporalB = fgvk::quality::makeImageQualityRegressionScene(
        fgvk::quality::QualitySceneKind::Traffic, 321, 181, 0.25F, 0.5F, 0.5F);
    const auto temporalAgain = fgvk::quality::makeImageQualityRegressionScene(
        fgvk::quality::QualitySceneKind::Traffic, 321, 181, 0.0F, 0.25F, 0.5F);
    expect(temporalA.current == temporalB.previous,
        "adjacent temporal pairs must share the exact endpoint pixels");
    expect(temporalA.reference == temporalAgain.reference &&
        temporalA.current == temporalAgain.current,
        "controlled content must be deterministic");
    expect(temporalA.previous != temporalA.current &&
        temporalA.reference != temporalA.previous && temporalA.reference != temporalA.current,
        "temporal references must contain actual intermediate motion");
    const auto legacy = fgvk::quality::makeImageQualityRegressionScene(
        fgvk::quality::QualitySceneKind::Traffic);
    const auto temporalLegacy = fgvk::quality::makeImageQualityRegressionScene(
        fgvk::quality::QualitySceneKind::Traffic, 321, 181, 0, 1, 0.5F);
    expect(legacy.previous == temporalLegacy.previous && legacy.current == temporalLegacy.current &&
        legacy.reference == temporalLegacy.reference && legacy.focusMask == temporalLegacy.focusMask,
        "explicit scene times must preserve the existing quality contract");
    expectInvalidArgument([] {
        static_cast<void>(fgvk::quality::makeImageQualityRegressionScene(
            fgvk::quality::QualitySceneKind::Traffic, 20000, 20000, 0, 1, 0.5F));
    }, "bounded scene generation must reject oversized allocations");
    expectInvalidArgument([] {
        static_cast<void>(fgvk::quality::makeImageQualityRegressionScene(
            fgvk::quality::QualitySceneKind::Traffic, 321, 181, -0.1F, 1, 0.5F));
    }, "temporal scenes must reject invalid source times");

    std::cout << "Procedural image-quality regression tests passed\n";
    return 0;
}
