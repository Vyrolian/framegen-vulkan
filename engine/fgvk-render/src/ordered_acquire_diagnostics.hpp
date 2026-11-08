/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

namespace fgvk::layer {

    /// Builds the one-time ordered acquire policy record, including its prefix
    /// and trailing newline. Timeouts are printed in milliseconds with the
    /// default stream formatting. An absent configured budget prints as
    /// "unbounded". A per-image value of UINT64_MAX prints as unbounded.
    [[nodiscard]] inline std::string formatOrderedAcquirePolicy(
            const uint64_t contextId,
            const std::optional<uint64_t> configuredNs,
            const uint64_t perImageNs,
            const uint64_t recoveryNs,
            const double retryCeilingMs) {
        constexpr uint64_t unboundedNs = std::numeric_limits<uint64_t>::max();
        const bool perImageUnbounded = perImageNs == unboundedNs;
        std::ostringstream record;
        record << "framegen-vulkan: present diagnostics: "
                  "operation=ordered-acquire-policy"
               << " context=" << contextId
               << " configured_timeout_ms=";
        if (configuredNs) {
            record << static_cast<double>(*configuredNs) / 1'000'000.0;
        } else {
            record << "unbounded";
        }
        record << " per_image_timeout_ms="
               << (perImageUnbounded
                       ? 0.0
                       : static_cast<double>(perImageNs) / 1'000'000.0)
               << " per_image_timeout_unbounded="
               << (perImageUnbounded ? 1 : 0)
               << " budget_scope=application-present"
               << " failure_source=explicit-acquire-timeout"
               << " recovery_probe_timeout_ms="
               << static_cast<double>(recoveryNs) / 1'000'000.0
               << " recovery_probe_failure=backoff"
               << " recovery_retry_ceiling_ms=" << retryCeilingMs
               << '\n';
        return record.str();
    }
}
