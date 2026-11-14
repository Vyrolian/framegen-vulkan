/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "model_resources.hpp"
#include "model_resource_validation.hpp"
#include "fgvk-common/helpers/errors.hpp"

#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace {
    struct ResolutionOutcome {
        std::optional<int64_t> offset;
        std::string reason;
    };

    // Protect only setup-time metadata, not shader execution or frame work.
    std::mutex resolutionMutex;
    constexpr size_t maximumDiscoveryResources = 4096U;
}

struct fgvk::backend::ModelResolutionCache {
    // First key: native LSFG FP32/FP16 or DirectX LSFG.
    // Second: mode or all.
    std::map<std::pair<uint32_t, uint32_t>, ResolutionOutcome> selections;
    std::optional<ResolutionOutcome> relocatedLsfg;
    std::optional<ResolutionOutcome> relocatedLsfgDxbc;
};

namespace {
    using namespace fgvk::backend;

    template<typename Action>
    ResolutionOutcome attempt(Action action) {
        try {
            return {.offset = action(), .reason = {}};
        } catch (const ls::error& error) {
            return {.offset = std::nullopt, .reason = error.what()};
        }
    }

    int64_t requireResolution(const ResolutionOutcome& result) {
        if (!result.offset)
            throw ls::error(result.reason);
        return *result.offset;
    }

    template<typename Action>
    ModelResourceSelection cachedResolution(const DllResourceArchive& archive,
            const std::pair<uint32_t, uint32_t> key, Action action) {
        const std::scoped_lock lock(resolutionMutex);
        if (!archive.modelResolutionCache)
            archive.modelResolutionCache = std::make_shared<ModelResolutionCache>();
        auto& cache = *archive.modelResolutionCache;
        auto found = cache.selections.find(key);
        if (found == cache.selections.end())
            found = cache.selections.emplace(key, attempt([&] {
                return action(cache);
            })).first;
        return {requireResolution(found->second)};
    }

    bool validateLsfg(const DllResourceArchive& archive,
            const ModelResourceSelection selection,
            const bool fp16, const bool performance) {
        bool declaresFloat16 = false;
        for (const auto& spec : detail::lsfgShaderSpecs(performance)) {
            const auto id = detail::lsfgResourceId(spec.logicalId, fp16, performance);
            const auto info = detail::validateSpirvComputeShader(
                selection.resource(archive, id), spec.contract,
                "LSFG resource " + std::to_string(selection.resourceId(id)));
            if (selection.idOffset != 0 && !info.exactBindings)
                throw ls::error("relocated LSFG table has incompatible discovery bindings");
            declaresFloat16 = declaresFloat16 || info.declaresFloat16;
        }
        return declaresFloat16;
    }

    void validateLsfgDxbc(const DllResourceArchive& archive,
            const ModelResourceSelection selection, const bool performance) {
        for (const auto& spec : detail::lsfgShaderSpecs(performance)) {
            const auto id = detail::lsfgDxbcResourceId(spec.logicalId, performance);
            detail::validateDxbcResourceBindings(selection.resource(archive, id),
                spec.contract, "LSFG DirectX resource " +
                    std::to_string(selection.resourceId(id)));
        }
    }

    template<typename Validate>
    int64_t discoverTable(const DllResourceArchive& archive,
            const uint32_t anchor, const std::string& family, Validate validate) {
        if (archive.resources.size() > maximumDiscoveryResources)
            throw ls::error(family + " resource discovery exceeds its bounded inventory");
        std::optional<int64_t> selected;
        for (const auto& [id, data] : archive.resources) {
            static_cast<void>(data);
            const int64_t offset = static_cast<int64_t>(id) - anchor;
            if (offset == 0)
                continue; // The caller already inspected the canonical model.
            const auto candidate = attempt([&] {
                validate(ModelResourceSelection{offset});
                return offset;
            });
            if (!candidate.offset)
                continue;
            if (selected)
                throw ls::error("ambiguous relocated " + family + " resource tables");
            selected = offset;
        }
        if (!selected)
            throw ls::error("no supported relocated " + family + " resource table");
        return *selected;
    }

    int64_t discoverLsfg(const DllResourceArchive& archive) {
        // Both precision lanes and both modes must retain their relative
        // positions. A single stage's bindings cannot establish its role.
        const uint32_t anchor = detail::lsfgResourceId(255U, true, false);
        return discoverTable(archive, anchor, "LSFG", [&](const auto selection) {
            for (const bool fp16 : {true, false}) {
                bool declaresFloat16 = false;
                for (const bool performance : {false, true})
                    declaresFloat16 = validateLsfg(archive, selection,
                        fp16, performance) || declaresFloat16;
                if (declaresFloat16 != fp16)
                    throw ls::error("relocated LSFG table has unproven precision order");
            }
        });
    }

    template<typename Discover>
    int64_t relocatedAfterFailure(const ls::error& original,
            std::optional<ResolutionOutcome>& cached, Discover discover) {
        if (!cached)
            cached = attempt(discover);
        if (!cached->offset)
            throw ls::error(std::string(original.what()) + "; " + cached->reason);
        return *cached->offset;
    }
}

uint32_t fgvk::backend::ModelResourceSelection::resourceId(
        const uint32_t canonicalId) const {
    const int64_t base = canonicalId;
    if (this->idOffset < -base ||
            this->idOffset > std::numeric_limits<uint32_t>::max() - base)
        throw ls::error("relocated model resource ID is out of range");
    return static_cast<uint32_t>(base + this->idOffset);
}

const std::vector<uint8_t>& fgvk::backend::ModelResourceSelection::resource(
        const DllResourceArchive& archive, const uint32_t canonicalId) const {
    const uint32_t id = this->resourceId(canonicalId);
    const auto found = archive.resources.find(id);
    if (found == archive.resources.end())
        throw ls::error("model DLL does not contain resource " + std::to_string(id));
    return found->second;
}

fgvk::backend::ModelResourceSelection fgvk::backend::resolveLsfgModelResources(
        const DllResourceArchive& archive, const bool fp16,
        const std::optional<bool> performance) {
    return cachedResolution(archive, {fp16 ? 1U : 0U,
            performance ? (*performance ? 1U : 0U) : 2U}, [&](ModelResolutionCache& cache) {
        try {
            for (const bool mode : {false, true})
                if (!performance || *performance == mode)
                    static_cast<void>(validateLsfg(archive, {}, fp16, mode));
            return int64_t{0};
        } catch (const ls::error& error) {
            return relocatedAfterFailure(error, cache.relocatedLsfg,
                [&] { return discoverLsfg(archive); });
        }
    });
}

fgvk::backend::ModelResourceSelection fgvk::backend::resolveLsfgDxbcModelResources(
        const DllResourceArchive& archive, const std::optional<bool> performance) {
    return cachedResolution(archive, {6U,
            performance ? (*performance ? 1U : 0U) : 2U}, [&](ModelResolutionCache& cache) {
        try {
            for (const bool mode : {false, true})
                if (!performance || *performance == mode)
                    validateLsfgDxbc(archive, {}, mode);
            return int64_t{0};
        } catch (const ls::error& error) {
            return relocatedAfterFailure(error, cache.relocatedLsfgDxbc, [&] {
                return discoverTable(archive, 255U, "LSFG DirectX", [&](const auto selection) {
                    for (const bool mode : {false, true})
                        validateLsfgDxbc(archive, selection, mode);
                });
            });
        }
    });
}
