/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "fence_ring.hpp"
#include "helpers/temporal_phases.hpp"
#include "helpers/black_image.hpp"
#include "helpers/frame_color_policy.hpp"
#include "fgvk-backend/fgvk.hpp"
#include "extraction/dll_reader.hpp"
#include "extraction/shader_registry.hpp"
#include "helpers/limits.hpp"
#include "helpers/managed_shader.hpp"
#include "helpers/timestamp_upload_cache.hpp"
#include "helpers/utils.hpp"
#include "fgvk-common/helpers/errors.hpp"
#include "fgvk-common/helpers/pointers.hpp"
#include "fgvk-common/helpers/file_descriptors.hpp"
#include "fgvk-common/vulkan/buffer.hpp"
#include "fgvk-common/vulkan/command_buffer.hpp"
#include "fgvk-common/vulkan/fence.hpp"
#include "fgvk-common/vulkan/external_image.hpp"
#include "fgvk-common/vulkan/gpu_clock.hpp"
#include "fgvk-common/vulkan/image.hpp"
#include "fgvk-common/vulkan/timeline_semaphore.hpp"
#include "fgvk-common/vulkan/timestamp_query_pool.hpp"
#include "fgvk-common/vulkan/vulkan.hpp"
#include "shaderchains/alpha0.hpp"
#include "shaderchains/alpha1.hpp"
#include "shaderchains/beta0.hpp"
#include "shaderchains/beta1.hpp"
#include "shaderchains/color_conversion.hpp"
#include "shaderchains/delta0.hpp"
#include "shaderchains/delta1.hpp"
#include "shaderchains/gamma0.hpp"
#include "shaderchains/gamma1.hpp"
#include "shaderchains/generate.hpp"
#include "shaderchains/mipmaps.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>
#include "helpers/completed_frame_profile.hpp"

#ifdef FGVK_TESTING_RENDERDOC
#include <renderdoc_app.h>
#include <dlfcn.h>
#endif

using namespace fgvk;
using namespace fgvk::backend;

namespace fgvk::backend {
    struct FrameProfilingState {
        FrameProfilingState(const vk::Vulkan& vk, const size_t outputs, const bool collectCompleted) :
            // Three prepass boundaries, batch completion, and three per output.
            // Round up for the pair-sized pool; readback excludes the spare slot.
            queries(vk, static_cast<uint32_t>(((4 + outputs * 3 + 1) / 2) * 2)),
            completed(collectCompleted) {}
        vk::TimestampQueryPool queries;
        FrameProfile timings;
        size_t generatedCount{};
        CompletedFrameProfile completed;
    };

    using ProfileClock = std::chrono::steady_clock;
    double profileMicroseconds(const ProfileClock::time_point start) {
        return std::chrono::duration<double, std::micro>(ProfileClock::now() - start).count();
    }

    /// Non-draining split boundaries: four per command phase, read only after the batch fence signals.
    struct SplitProfilingState {
        // Each phase's prepass resets its pool, so reads must precede that phase's next batch.
        std::array<std::optional<vk::TimestampQueryPool>, commandPhaseCount> pools;
        vk::CalibratedClock clock;
        ProfileClock::time_point calibratedAt{};
        // Set after a read failure; recorded commands remain valid but are no longer sampled.
        bool failed{};
        // Keep every completed generated batch for the diagnostics log (see enableSplitProfile).
        bool retainCompleted{};
        std::deque<SplitFrameProfile> completed;
        // The newest completed batch, whatever its kind; the pacing predictor reads it.
        std::optional<CompletedBatchEnd> latest;
    };

    /// GP1 diagnostics: one BOTTOM_OF_PIPE timestamp after each serial step of a batch, read only after
    /// the batch fence signalled. Window statistics cover generated batches, so every step has a sample there.
    struct PassStepProfilingState {
        // The prepass has 10 steps and the first generated output at most 33 with overlap, plus the batch start.
        static constexpr uint32_t queryCount = 64;
        std::array<std::optional<vk::TimestampQueryPool>, commandPhaseCount> pools;
        // Names recorded with each phase's commands. Query k ends step k - 1: the prepass steps come
        // first, then the generated steps of the first output.
        struct PhaseNames {
            std::vector<std::string> prepass;
            std::vector<std::string> generated;
        };
        std::array<PhaseNames, commandPhaseCount> names;
        bool overlap{};
        // Set after a failed read; recorded commands remain valid but are no longer sampled.
        bool failed{};
        std::vector<std::string> stepNames;
        std::vector<std::vector<int64_t>> samples;
        size_t batches{};
        std::vector<PassStepSummary> completed;
    };

    /// Writes one timestamp per recorded step into a phase's pool. Default-constructed marks write nothing,
    /// so the unprofiled recording is unchanged.
    class PassStepMarks {
    public:
        PassStepMarks() = default;
        PassStepMarks(const vk::TimestampQueryPool& queries, VkCommandBuffer cmd,
                uint32_t first, std::vector<std::string>& nameList)
            : pool(&queries), commandBuffer(cmd), next(first), names(&nameList) {}

        explicit operator bool() const { return this->pool != nullptr; }

        /// Batch start: query zero, before the first step.
        void start() const {
            if (this->pool)
                this->pool->write(this->commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
        }
        /// Ends a step after the commands recorded since the previous mark.
        void mark(std::string_view name) {
            if (!this->pool)
                return;
            if (this->next >= PassStepProfilingState::queryCount)
                throw std::logic_error("GP1 pass-step timestamps exceed the query pool");
            this->pool->write(this->commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, this->next++);
            this->names->emplace_back(name);
        }
    private:
        const vk::TimestampQueryPool* pool{};
        VkCommandBuffer commandBuffer{};
        uint32_t next{};
        std::vector<std::string>* names{};
    };

    /// Names one recorded run from its member labels: each label once, in order, joined with '+'.
    template<typename Label>
    std::string passRunName(std::span<const Label> labels, size_t begin, size_t end) {
        std::string name;
        for (size_t i = begin; i < end; ++i) {
            if (i > begin && labels[i] == labels[i - 1])
                continue;
            if (!name.empty())
                name += '+';
            name += labels[i];
        }
        return name;
    }

    error::error(const std::string& msg, const std::exception& inner)
        : std::runtime_error(msg + "\n- " + inner.what()) {}
    error::error(const std::string& msg)
        : std::runtime_error(msg) {}
    error::~error() = default;

    /// instance class
    class InstanceImpl {
    public:
        /// create an instance
        /// (see fgvk documentation)
        InstanceImpl(vk::PhysicalDeviceSelector selectPhysicalDevice,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision);

        /// get the Vulkan instance
        /// @return the Vulkan instance
        [[nodiscard]] const auto& getVulkan() const { return this->vk; }
        /// get the shader registry
        /// @return the shader registry
        [[nodiscard]] const auto& getShaderRegistry() const { return this->shaders; }
#ifdef FGVK_TESTING_RENDERDOC
        /// get the RenderDoc API
        /// @return the RenderDoc API
        [[nodiscard]] const auto& getRenderDocAPI() const { return this->renderdoc; }
#endif
        // Movable, non-copyable, custom destructor
        InstanceImpl(const InstanceImpl&) = delete;
        InstanceImpl& operator=(const InstanceImpl&) = delete;
        InstanceImpl(InstanceImpl&&) = default;
        InstanceImpl& operator=(InstanceImpl&&) = default;
        ~InstanceImpl();
    private:
        vk::Vulkan vk;
        ShaderRegistry shaders;

#ifdef FGVK_TESTING_RENDERDOC
        std::optional<RENDERDOC_API_1_6_0> renderdoc;
#endif
    };

    /// context class
    class ContextImpl {
    public:
        /// create a context
        /// (see fgvk documentation)
        ContextImpl(const InstanceImpl& instance,
            ls::FileDescriptorScope& sourceFds, ls::FileDescriptorScope& destFds,
            ls::FileDescriptorScope& syncFd, VkExtent2D extent, FrameEncoding encoding, float flow, bool perf);

        // Borrowed scratch and cached descriptors require stable context owners.
        ContextImpl(const ContextImpl&) = delete;
        ContextImpl& operator=(const ContextImpl&) = delete;
        ContextImpl(ContextImpl&&) = delete;
        ContextImpl& operator=(ContextImpl&&) = delete;

        /// schedule frames
        /// (see fgvk documentation)
        void scheduleFrames();

        void enableFrameProfiling(bool collectCompleted);
        [[nodiscard]] std::optional<FrameProfile> takeCompletedFrameProfile();
        [[nodiscard]] FrameProfile readFrameProfile() const;

        void enableSplitProfile(bool retainCompleted);
        [[nodiscard]] std::vector<SplitFrameProfile> takeCompletedSplitProfiles();
        [[nodiscard]] bool splitProfileEnabled() const { return this->split && !this->split->failed; }
        void enablePassStepProfile();
        [[nodiscard]] std::vector<PassStepSummary> takePassStepSummaries();
        [[nodiscard]] std::optional<CompletedBatchEnd> latestBatchEnd() const {
            return this->split && !this->split->failed ? this->split->latest : std::nullopt;
        }
        [[nodiscard]] std::optional<bool> previousBatchPending() const;
        void pollSplitProfiles();

        /// schedule a variable number of frames at explicit timestamps
        void scheduleFrames(std::span<const float> timestamps);

        /// update temporal history without generating output frames
        /// (see fgvk documentation)
        void scheduleFrameHistory();

        /// wait for all of this context's in-flight work
        [[nodiscard]] bool waitForIdle(uint64_t timeoutNs) const;
    private:
        std::pair<vk::Image, vk::Image> sourceImages;
        std::vector<vk::Image> destImages;
        vk::Image blackImage;

        vk::TimelineSemaphore syncSemaphore; // imported
        vk::TimelineSemaphore prepassSemaphore;
        size_t idx{1};
        size_t fidx{0}; // real frame index

        // Per-phase recordings are reused every commandPhaseCount batches, and
        // waiting for batch n - inFlightDepth proves that they are idle.
        static constexpr size_t inFlightDepth = 3;
        static_assert(commandPhaseCount >= inFlightDepth);

        // Descriptor bindings repeat every lcm(2 source slots, 3 history slots).
        // Record lazily, once per phase and output; prepareWork serializes reuse.
        // Declared before recordings so query pools outlive referring commands.
        std::unique_ptr<FrameProfilingState> profiling;
        // Split boundaries share the recording lifetime rule above; each in-flight
        // slot remembers which frame and phase it ran, so its pool is read correctly.
        struct SplitBatch {
            uint64_t frame{};
            size_t phase{};
            bool generated{};
            bool valid{};
        };
        std::unique_ptr<SplitProfilingState> split;
        std::array<SplitBatch, inFlightDepth> splitBatches{};
        void recordSplitBatch(bool generated);
        void collectSplitProfiles(const std::array<bool, inFlightDepth>& signalled);
        // GP1 pass-step timestamps; needs split boundaries and shares their batch bookkeeping.
        std::unique_ptr<PassStepProfilingState> passSteps;
        void collectPassSteps(size_t phase);
        using RecordedCommands = std::array<std::optional<vk::CommandBuffer>, commandPhaseCount>;
        std::vector<RecordedCommands> cmdbufs;
        RecordedCommands prepassDependency;
        RecordedCommands sourceRelease;
        std::vector<vk::Fence> cmdbufFences; // one per in-flight batch slot
        std::array<bool, inFlightDepth> fenceScheduled{};
        std::vector<float> constantBufferTimestamps;

        /// fence of the batch that uses the current frame index
        [[nodiscard]] VkFence currentFence() const {
            return this->cmdbufFences.at(this->fidx % inFlightDepth).handle();
        }
        /// true from a batch's submission until prepareWork observes its fence
        [[nodiscard]] bool hasScheduledWork() const {
            return std::ranges::any_of(this->fenceScheduled, std::identity{});
        }

        void prepareWork(bool drainAll);
        const vk::CommandBuffer& recordPrepass();
        const vk::CommandBuffer& recordSourceRelease();
        void schedulePrepass(VkFence completionFence);

        Ctx ctx;

        // PQ conversion works in private linear scRGB images. Both are absent,
        // and every conversion stage is skipped, unless requiresPqConversion().
        std::optional<std::pair<vk::Image, vk::Image>> workingSourceImages;
        std::optional<std::vector<vk::Image>> workingDestImages;
        std::optional<ColorConversion> sourceColorConversion;
        std::optional<ColorConversion> destColorConversion;

        Mipmaps mipmaps;
        std::array<Alpha0, 7> alpha0;
        std::array<Alpha1, 7> alpha1;
        Beta0 beta0;
        Beta1 beta1;
        struct Pass {
            std::vector<Gamma0> gamma0;
            std::vector<Gamma1> gamma1;

            std::vector<Delta0> delta0;
            std::vector<Delta1> delta1;
            ls::lazy<Generate> generate;
        };
        std::vector<Pass> passes;
    };
}

Instance::Instance(
        const DevicePicker& devicePicker,
        const std::filesystem::path& shaderDllPath,
        bool allowLowPrecision) {
    const auto selectFunc = [&devicePicker](const vk::VulkanInstanceFuncs funcs,
            const std::vector<VkPhysicalDevice>& devices) {
        for (const auto& device : devices) {
            // check if the physical device supports VK_EXT_pci_bus_info
            uint32_t ext_count{};
            funcs.EnumerateDeviceExtensionProperties(device, nullptr, &ext_count, VK_NULL_HANDLE);

            std::vector<VkExtensionProperties> extensions(ext_count);
            funcs.EnumerateDeviceExtensionProperties(device, nullptr, &ext_count, extensions.data());

            const bool has_pci_ext = std::ranges::find_if(extensions,
                [](const VkExtensionProperties& ext) {
                    return std::string(std::to_array(ext.extensionName).data())
                        == VK_EXT_PCI_BUS_INFO_EXTENSION_NAME;
                }) != extensions.end();

            // then fetch all available properties
            VkPhysicalDevicePCIBusInfoPropertiesEXT pciInfo{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT
            };
            VkPhysicalDeviceProperties2 props{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                .pNext = has_pci_ext ? &pciInfo : nullptr
            };
            funcs.GetPhysicalDeviceProperties2(device, &props);

            std::array<char, 256> devname = std::to_array(props.properties.deviceName);
            devname.at(255) = '\0'; // ensure null-termination

            if (devicePicker(
                std::string(devname.data()),
                { backend::to_hex_id(props.properties.vendorID),
                  backend::to_hex_id(props.properties.deviceID) },
                has_pci_ext ? std::optional<std::string>{
                    std::to_string(pciInfo.pciBus) + ":" +
                    std::to_string(pciInfo.pciDevice) + "." +
                    std::to_string(pciInfo.pciFunction)
                } : std::nullopt
            ))
                return device;
        }

        throw ls::vulkan_error("no suitable physical device found");
    };

    this->m_impl = std::make_unique<InstanceImpl>(
        selectFunc, shaderDllPath, allowLowPrecision
    );
}

namespace {
    constexpr std::array<std::string_view, 3> alpha0Steps{"alpha0.s0", "alpha0.s1", "alpha0.s2"};
    constexpr std::array<std::string_view, 4> beta1Steps{"beta1.0", "beta1.1", "beta1.2", "beta1.3"};
    constexpr std::array<std::string_view, 7> gamma0Steps{
        "gamma0.0", "gamma0.1", "gamma0.2", "gamma0.3", "gamma0.4", "gamma0.5", "gamma0.6"};
    constexpr std::array<std::string_view, 7> gamma1Steps{
        "gamma1.0", "gamma1.1", "gamma1.2", "gamma1.3", "gamma1.4", "gamma1.5", "gamma1.6"};
    constexpr std::array<std::string_view, 3> delta0Steps{"delta0.0", "delta0.1", "delta0.2"};
    constexpr std::array<std::string_view, 3> delta1Steps{"delta1.0", "delta1.1", "delta1.2"};

    constexpr auto previousWorkFenceTimeout = std::chrono::milliseconds(250);
    constexpr uint64_t previousWorkFenceTimeoutNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            previousWorkFenceTimeout
        ).count()
    );
    constexpr size_t maximumRetiredContextCount = 2;
    // Diagnostic split samples retained for the render layer; the oldest is dropped first.
    constexpr size_t splitQueueCapacity = 8;

    /// wait on a batch fence with the time left in a budget shared by several waits
    /// @returns true if the fence signalled
    bool waitWithBudget(const vk::Fence& fence, const vk::Vulkan& vulkan, uint64_t& remainingNs) {
        const auto started = std::chrono::steady_clock::now();
        const bool signalled = fence.wait(vulkan, remainingNs);
        const auto spentNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
        remainingNs = spentNs >= remainingNs ? 0 : remainingNs - spentNs;
        return signalled;
    }

    /// The calibrated-timestamp entry point, but only when the backend device can
    /// pair its timestamp counter with CLOCK_MONOTONIC. Null means unavailable.
    PFN_vkGetCalibratedTimestampsKHR splitCalibrationFunction(const vk::Vulkan& vulkan) {
        const char* extension = vulkan.calibratedTimestampsExtension();
        const auto queryDomains = vulkan.fi().GetPhysicalDeviceCalibrateableTimeDomainsKHR;
        if (!extension || !queryDomains)
            return nullptr;
        uint32_t count{};
        if (queryDomains(vulkan.physdev(), &count, nullptr) != VK_SUCCESS || count == 0)
            return nullptr;
        std::vector<VkTimeDomainKHR> domains(count);
        if (queryDomains(vulkan.physdev(), &count, domains.data()) != VK_SUCCESS)
            return nullptr;
        const auto offered = [&domains](const VkTimeDomainKHR domain) {
            return std::ranges::find(domains, domain) != domains.end();
        };
        if (!offered(VK_TIME_DOMAIN_DEVICE_KHR) || !offered(VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR))
            return nullptr;
        const bool khr = std::string_view(extension) == VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
        return reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(vulkan.fi().GetDeviceProcAddr(
            vulkan.dev(), khr ? "vkGetCalibratedTimestampsKHR" : "vkGetCalibratedTimestampsEXT"));
    }

    /// Diagnostic only: one line per process, for the first split step that could not start.
    void reportSplitUnavailable(const std::string_view reason, const std::string_view detail = {}) {
        static std::atomic<bool> reported{false};
        if (reported.exchange(true))
            return;
        std::cerr << "framegen-vulkan: GPU split profile unavailable; reason=" << reason;
        if (!detail.empty())
            std::cerr << "; " << detail;
        std::cerr << '\n';
    }

    /// Diagnostic only: one line per process, for the first pass-step read that failed.
    void reportPassStepUnavailable(const std::string_view detail) {
        static std::atomic<bool> reported{false};
        if (reported.exchange(true))
            return;
        std::cerr << "framegen-vulkan: GPU pass-step profile unavailable; reason=queries; " << detail << '\n';
    }

    // Generated batches per pass-step summary; the render layer consumes each summary once.
    constexpr size_t passWindowBatches = 240;
    constexpr size_t passSummaryCapacity = 512;

    /// Nearest-rank percentile of an ascending sample list, in nanoseconds.
    int64_t nearestRankNs(const std::vector<int64_t>& sorted, const size_t percent) {
        if (sorted.empty())
            return 0;
        const size_t rank = (percent * sorted.size() + 99) / 100;
        return sorted.at(std::clamp<size_t>(rank, 1, sorted.size()) - 1);
    }

    /// find the cache file path
    std::filesystem::path findCacheFilePath() {
        const char* xdgCacheHome = std::getenv("XDG_CACHE_HOME");
        if (xdgCacheHome && *xdgCacheHome != '\0')
            return std::filesystem::path(xdgCacheHome) / "fgvk_pipeline_cache.bin";

        const char* home = std::getenv("HOME");
        if (home && *home != '\0')
            return std::filesystem::path(home) / ".cache" / "fgvk_pipeline_cache.bin";

        return{"/tmp/fgvk_pipeline_cache.bin"};
    }
    /// create a Vulkan instance
    vk::Vulkan createVulkanInstance(vk::PhysicalDeviceSelector selectPhysicalDevice) {
        try {
            return{
                "fgvk", vk::version{2, 0, 0},
                "fgvk-engine", vk::version{2, 0, 0},
                selectPhysicalDevice,
                false, std::nullopt,
                findCacheFilePath()
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to initialize Vulkan", e);
        }
    }
    /// build a shader registry
    ShaderRegistry createShaderRegistry(vk::Vulkan& vk,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision) {
        std::shared_ptr<const backend::DllResourceArchive> archive;

        try {
            archive = backend::loadDllResourceArchive(shaderDllPath);
        } catch (const std::exception& e) {
            throw backend::error("Unable to parse Lossless Scaling DLL", e);
        }

        try {
            if (allowLowPrecision && !vk.supportsFP16())
                throw ls::error("LSFG FP16 requested but shaderFloat16 is unavailable on the selected GPU; select FP32");
            const bool fp16 = allowLowPrecision;
            auto registry = backend::buildShaderRegistry(
                vk, fp16, *archive, shaderDllPath
            );
            std::clog << "framegen-vulkan: model DLL inspected: component=lsfg"
                      << "; dll_sha256=" << archive->fileSha256
                      << "; resource_layout_sha256="
                      << archive->resourceLayoutSha256
                      << "; resource_count=" << archive->resources.size()
                      << "; precision=" << (registry.is_fp16 ? "fp16" : "fp32")
                      << "; compatibility=structural-and-vulkan" << '\n';
            return registry;
        } catch (const std::exception& e) {
            throw backend::error("Unable to build shader registry", e);
        }
    }
#ifdef FGVK_TESTING_RENDERDOC
    /// load RenderDoc integration
    std::optional<RENDERDOC_API_1_6_0> loadRenderDocIntegration() {
        void* module = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
        if (!module)
            return std::nullopt;

        auto renderdocGetAPI = reinterpret_cast<pRENDERDOC_GetAPI>(
            dlsym(module, "RENDERDOC_GetAPI"));
        if (!renderdocGetAPI)
            return std::nullopt;

        RENDERDOC_API_1_6_0* api{};
        renderdocGetAPI(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&api));
        if (!api)
            return std::nullopt;

        return *api;
    }
#endif
}

InstanceImpl::InstanceImpl(vk::PhysicalDeviceSelector selectPhysicalDevice,
            const std::filesystem::path& shaderDllPath,
            bool allowLowPrecision)
        : vk(createVulkanInstance(selectPhysicalDevice)),
        shaders(createShaderRegistry(this->vk, shaderDllPath,
            allowLowPrecision && vk.supportsFP16())) {
#ifdef FGVK_TESTING_RENDERDOC
    this->renderdoc = loadRenderDocIntegration();
#endif
    std::clog << "framegen-vulkan: robustImageAccess2 "
        << (vk.supportsRobustImageAccess2() ? "enabled" : "unavailable")
        << '\n';
    vk.persistPipelineCache(); // will silently fail
}

Context& Instance::openContext(std::pair<int, int> sourceFds, const std::vector<int>& destFds,
        int syncFd, uint32_t width, uint32_t height,
        const FrameEncoding encoding, float flow, bool perf) {
    // Take the complete batch before any operation that can fail. Individual
    // import wrappers consume their descriptor even on failure; unattempted
    // descriptors remain here until construction succeeds or unwinds.
    const std::array sourceDescriptors{sourceFds.first, sourceFds.second};
    ls::FileDescriptorScope sourceScope{sourceDescriptors};
    ls::FileDescriptorScope destinationScope{destFds};
    ls::FileDescriptorScope syncScope{{&syncFd, 1}};
    this->collectRetiredContexts();
    if (this->m_retiredContexts.size() >= maximumRetiredContextCount) {
        throw backend::error(
            "Too many frame-generation contexts are still retiring"
        );
    }
    const VkExtent2D extent{ width, height };
    auto context = std::make_unique<ContextImpl>(*this->m_impl,
        sourceScope, destinationScope, syncScope,
        extent, encoding, flow, perf
    );
    if (usesHdrModel(encoding)) {
        std::clog << "framegen-vulkan: HDR generation: encoding="
            << (requiresPqConversion(encoding) ? "hdr10-pq" : "scrgb-linear")
            << " pq_conversion=" << requiresPqConversion(encoding) << '\n';
    }
    auto* result = this->m_contexts.emplace_back(std::move(context)).get();
    return *result;
}

namespace {
    /// import source images
    std::pair<vk::Image, vk::Image> importSourceImages(const vk::Vulkan& vk,
            ls::FileDescriptorScope& sourceFds,
            VkExtent2D extent, VkFormat format) {
        try {
            return {
                vk::Image(vk, extent, format,
                    sourceExchangeImageUsage, sourceFds.take()),
                vk::Image(vk, extent, format,
                    sourceExchangeImageUsage, sourceFds.take())
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to import destination images", e);
        }
    }
    /// import destination images
    std::vector<vk::Image> importDestinationImages(const vk::Vulkan& vk,
            ls::FileDescriptorScope& destFds,
            VkExtent2D extent, VkFormat format) {
        try {
            std::vector<vk::Image> destImages;
            destImages.reserve(destFds.size());

            for (size_t index = 0; index < destFds.size(); ++index)
                destImages.emplace_back(vk, extent, format,
                    destinationExchangeImageUsage, destFds.take());

            return destImages;
        } catch (const std::exception& e) {
            throw backend::error("Unable to import destination images", e);
        }
    }
    /// create a black image
    vk::Image createBlackImage(const vk::Vulkan& vk) {
        try {
            return{vk,
                { .width = 4, .height = 4 }, VK_FORMAT_R8G8B8A8_UNORM,
                backend::blackImageUsage
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to create black image", e);
        }
    }
    /// import timeline semaphore
    vk::TimelineSemaphore importTimelineSemaphore(const vk::Vulkan& vk, int syncFd) {
        try {
            return{vk, 0, syncFd};
        } catch (const std::exception& e) {
            throw backend::error("Unable to import timeline semaphore", e);
        }
    }
    /// create prepass semaphore
    vk::TimelineSemaphore createPrepassSemaphore(const vk::Vulkan& vk) {
        try {
            return{vk, 0};
        } catch (const std::exception& e) {
            throw backend::error("Unable to create prepass semaphore", e);
        }
    }
    /// create one fence per in-flight batch slot
    std::vector<vk::Fence> createCmdbufFences(const vk::Vulkan& vulkan, const size_t count) {
        std::vector<vk::Fence> fences;
        fences.reserve(count);
        for (size_t i = 0; i < count; ++i)
            fences.emplace_back(vulkan);
        return fences;
    }
    /// create context data
    bool usesHighPrecisionModel(const FrameEncoding encoding) {
        return encoding != FrameEncoding::Sdr8;
    }

    VkFormat transportFormat(const FrameEncoding encoding) {
        if (encoding == FrameEncoding::Sdr8)
            return VK_FORMAT_R8G8B8A8_UNORM;
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    }

    std::optional<std::pair<vk::Image, vk::Image>> createWorkingSourceImages(
            const Ctx& ctx, const VkExtent2D extent, const FrameEncoding encoding) {
        if (!requiresPqConversion(encoding))
            return std::nullopt;

        return std::make_optional(std::pair{
            vk::Image(ctx.vk, extent, ctx.imageMemoryPool, VK_FORMAT_R16G16B16A16_SFLOAT),
            vk::Image(ctx.vk, extent, ctx.imageMemoryPool, VK_FORMAT_R16G16B16A16_SFLOAT)
        });
    }

    std::optional<std::vector<vk::Image>> createWorkingDestImages(
            const Ctx& ctx, const VkExtent2D extent,
            const FrameEncoding encoding, const size_t count) {
        if (!requiresPqConversion(encoding))
            return std::nullopt;

        std::vector<vk::Image> images;
        images.reserve(count);
        for (size_t i = 0; i < count; ++i)
            images.emplace_back(ctx.vk, extent, ctx.imageMemoryPool, VK_FORMAT_R16G16B16A16_SFLOAT);
        return images;
    }

    const std::pair<vk::Image, vk::Image>& selectModelSourceImages(
            const std::pair<vk::Image, vk::Image>& transportImages,
            const std::optional<std::pair<vk::Image, vk::Image>>& workingImages) {
        return workingImages ? *workingImages : transportImages;
    }

    const std::vector<vk::Image>& selectModelDestImages(
            const std::vector<vk::Image>& transportImages,
            const std::optional<std::vector<vk::Image>>& workingImages) {
        return workingImages ? *workingImages : transportImages;
    }

    std::optional<ColorConversion> createSourceColorConversion(
            const Ctx& ctx,
            const std::pair<vk::Image, vk::Image>& transportImages,
            const std::optional<std::pair<vk::Image, vk::Image>>& workingImages) {
        if (!workingImages)
            return std::nullopt;
        return std::optional<ColorConversion>(std::in_place,
            ctx, ctx.shaders.get().hdr10_pq_to_scrgb,
            transportImages, workingImages.value()
        );
    }

    std::optional<ColorConversion> createDestColorConversion(
            const Ctx& ctx,
            const std::optional<std::vector<vk::Image>>& workingImages,
            const std::vector<vk::Image>& transportImages) {
        if (!workingImages)
            return std::nullopt;
        return std::optional<ColorConversion>(std::in_place,
            ctx, ctx.shaders.get().scrgb_to_hdr10_pq,
            workingImages.value(), transportImages
        );
    }

    Ctx createCtx(const InstanceImpl& instance, VkExtent2D extent,
            const FrameEncoding encoding, float flow, bool perf, size_t count) {
        const auto& vk = instance.getVulkan();
        const auto& shaders = instance.getShaderRegistry();

        try {
            std::vector<vk::Buffer> constantBuffers{};
            constantBuffers.reserve(count);

            for (size_t i = 0; i < count; ++i)
                constantBuffers.emplace_back(vk,
                    backend::getDefaultConstantBuffer(
                        i, count, usesHdrModel(encoding), flow
                    )
                );

            return {
                .vk = std::ref(vk),
                .shaders = std::ref(shaders),
                .imageMemoryPool{std::make_shared<vk::ImageMemoryPool>(vk)},
                .pool{vk, backend::calculateDescriptorPoolLimits(
                    count, perf, requiresPqConversion(encoding)
                )},
                .constantBuffer{vk, backend::getDefaultConstantBuffer(
                    0, 1, usesHdrModel(encoding), flow
                )},
                .constantBuffers{std::move(constantBuffers)},
                .bnbSampler{vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, false},
                .bnwSampler{vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_COMPARE_OP_NEVER, true},
                .eabSampler{vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_COMPARE_OP_ALWAYS, false},
                .sourceExtent = extent,
                .flowExtent = VkExtent2D {
                    .width = static_cast<uint32_t>(static_cast<float>(extent.width) / flow),
                    .height = static_cast<uint32_t>(static_cast<float>(extent.height) / flow)
                },
                .highPrecision = usesHighPrecisionModel(encoding),
                .hdr = usesHdrModel(encoding),
                .flow = flow,
                .perf = perf,
                .count = count
            };
        } catch (const std::exception& e) {
            throw backend::error("Unable to create context", e);
        }
    }
}

ContextImpl::ContextImpl(const InstanceImpl& instance,
            ls::FileDescriptorScope& sourceFds, ls::FileDescriptorScope& destFds,
            ls::FileDescriptorScope& syncFd, VkExtent2D extent, const FrameEncoding encoding, float flow, bool perf) :
        sourceImages(importSourceImages(instance.getVulkan(), sourceFds,
            extent, transportFormat(encoding))),
        destImages(importDestinationImages(instance.getVulkan(), destFds,
            extent, transportFormat(encoding))),
        blackImage(createBlackImage(instance.getVulkan())),
        syncSemaphore(importTimelineSemaphore(instance.getVulkan(), syncFd.take())),
        prepassSemaphore(createPrepassSemaphore(instance.getVulkan())),
        cmdbufs(destFds.size() + 1),
        cmdbufFences(createCmdbufFences(instance.getVulkan(), inFlightDepth)),
        ctx(createCtx(instance, extent, encoding, flow, perf, destFds.size())),
        workingSourceImages(createWorkingSourceImages(ctx, extent, encoding)),
        workingDestImages(createWorkingDestImages(
            ctx, extent, encoding, destImages.size()
        )),
        sourceColorConversion(createSourceColorConversion(
            ctx, sourceImages, workingSourceImages
        )),
        destColorConversion(createDestColorConversion(
            ctx, workingDestImages, destImages
        )),
        mipmaps(ctx, selectModelSourceImages(sourceImages, workingSourceImages)),
        alpha0{
            Alpha0(ctx, mipmaps.getImages().at(0)),
            Alpha0(ctx, mipmaps.getImages().at(1)),
            Alpha0(ctx, mipmaps.getImages().at(2)),
            Alpha0(ctx, mipmaps.getImages().at(3)),
            Alpha0(ctx, mipmaps.getImages().at(4)),
            Alpha0(ctx, mipmaps.getImages().at(5)),
            Alpha0(ctx, mipmaps.getImages().at(6))
        },
        alpha1{
            Alpha1(ctx, alphaHistoryCounts.at(0), alpha0.at(0).getImages()),
            Alpha1(ctx, alphaHistoryCounts.at(1), alpha0.at(1).getImages()),
            Alpha1(ctx, alphaHistoryCounts.at(2), alpha0.at(2).getImages()),
            Alpha1(ctx, alphaHistoryCounts.at(3), alpha0.at(3).getImages()),
            Alpha1(ctx, alphaHistoryCounts.at(4), alpha0.at(4).getImages()),
            Alpha1(ctx, alphaHistoryCounts.at(5), alpha0.at(5).getImages()),
            Alpha1(ctx, alphaHistoryCounts.at(6), alpha0.at(6).getImages())
        },
        beta0(ctx, alpha1.at(0).getImages()),
        beta1(ctx, beta0.getImages(), alpha0.at(0).getImages()) {
    const auto& modelSourceImages = selectModelSourceImages(
        this->sourceImages, this->workingSourceImages
    );
    const auto& modelDestImages = selectModelDestImages(
        this->destImages, this->workingDestImages
    );

    // Generated outputs run sequentially on the same compute queue. Keep
    // descriptors/uniforms per output, but share the intermediate images.
    // Compute dependencies order the previous readers and writers.
    // build main passes
    for (size_t i = 0; i < destImages.size(); ++i) {
        auto& pass = this->passes.emplace_back();

        pass.gamma0.reserve(7);
        pass.gamma1.reserve(7);
        pass.delta0.reserve(3);
        pass.delta1.reserve(3);
        for (size_t j = 0; j < 7; j++) {
            if (j == 0) { // first pass has no prior data
                pass.gamma0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    this->blackImage,
                    i == 0 ? nullptr : &this->passes.front().gamma0.at(j)
                );
                pass.gamma1.emplace_back(ctx, i,
                    pass.gamma0.at(j).getImages(),
                    this->alpha0.at(6 - j).getImages(),
                    this->blackImage,
                    this->beta1.getImages().at(5),
                    i == 0 ? nullptr : &this->passes.front().gamma1.at(j)
                );
            } else { // other passes use prior data
                pass.gamma0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    pass.gamma1.at(j - 1).getImage(),
                    i == 0 ? nullptr : &this->passes.front().gamma0.at(j)
                );
                pass.gamma1.emplace_back(ctx, i,
                    pass.gamma0.at(j).getImages(),
                    this->alpha0.at(6 - j).getImages(),
                    pass.gamma1.at(j - 1).getImage(),
                    this->beta1.getImages().at(6 - j),
                    i == 0 ? nullptr : &this->passes.front().gamma1.at(j)
                );
            }

            if (j == 4) { // first special pass has no prior data
                pass.delta0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    pass.gamma0.at(j).getImages(),
                    pass.gamma1.at(j).getTempImages0(),
                    this->blackImage,
                    pass.gamma1.at(j - 1).getImage()
                );
                pass.delta1.emplace_back(ctx, i,
                    pass.gamma0.at(j).getImages(),
                    pass.gamma1.at(j).getTempImages0(),
                    pass.gamma1.at(j).getTempImages1(),
                    this->blackImage,
                    this->beta1.getImages().at(6 - j),
                    this->blackImage,
                    i == 0 ? nullptr : &this->passes.front().delta1.at(j - 4),
                    this->alpha0.at(7 - j).getTemporaryImages0(),
                    this->alpha0.at(7 - j).getTemporaryImages1()
                );
            } else if (j > 4) { // further passes do
                pass.delta0.emplace_back(ctx, i,
                    this->alpha1.at(6 - j).getImages(),
                    pass.gamma0.at(j).getImages(),
                    pass.gamma1.at(j).getTempImages0(),
                    pass.delta1.at(j - 5).getImage0(),
                    pass.gamma1.at(j - 1).getImage()
                );
                pass.delta1.emplace_back(ctx, i,
                    pass.gamma0.at(j).getImages(),
                    pass.gamma1.at(j).getTempImages0(),
                    pass.gamma1.at(j).getTempImages1(),
                    pass.delta1.at(j - 5).getImage0(),
                    this->beta1.getImages().at(6 - j),
                    pass.delta1.at(j - 5).getImage1(),
                    i == 0 ? nullptr : &this->passes.front().delta1.at(j - 4),
                    this->alpha0.at(7 - j).getTemporaryImages0(),
                    this->alpha0.at(7 - j).getTemporaryImages1()
                );
            }
        }

        pass.generate.emplace(ctx, i,
            modelSourceImages,
            pass.gamma1.at(6).getImage(),
            pass.delta1.at(2).getImage0(),
            pass.delta1.at(2).getImage1(),
            modelDestImages.at(i)
        );
    }

    // initialize all images
    std::vector<VkImage> images{};
    images.push_back(this->sourceImages.first.handle());
    images.push_back(this->sourceImages.second.handle());
    for (const auto& image : this->destImages)
        images.push_back(image.handle());
    if (this->workingSourceImages) {
        images.push_back(this->workingSourceImages->first.handle());
        images.push_back(this->workingSourceImages->second.handle());
    }
    if (this->workingDestImages) {
        for (const auto& image : *this->workingDestImages)
            images.push_back(image.handle());
    }
    mipmaps.prepare(images);
    for (size_t i = 0; i < 7; ++i) {
        alpha0.at(i).prepare(images);
        alpha1.at(i).prepare(images);
    }
    beta0.prepare(images);
    beta1.prepare(images);
    // Every output references the same scratch images: initialize once.
    if (!this->passes.empty()) {
        const auto& pass = this->passes.front();
        for (size_t i = 0; i < 7; ++i) {
            pass.gamma0.at(i).prepare(images);
            pass.gamma1.at(i).prepare(images);

            if (i < 4) continue;
            pass.delta1.at(i - 4).prepare(images);
        }
    }

    std::vector<vk::Barrier> barriers{};
    barriers.reserve(images.size());

    for (const auto& image : images) {
        barriers.emplace_back(vk::Barrier {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1
            }
        });
    }

    const vk::CommandBuffer cmdbuf{ctx.vk};
    cmdbuf.begin(ctx.vk);
    cmdbuf.insertBarriers(ctx.vk, barriers);
    backend::initializeBlackImage(ctx.vk.get().df(), cmdbuf.handle(), this->blackImage.handle());
    // The exporter may overwrite either source before the first scheduled
    // frame. Leave both aliases in the documented external GENERAL state.
    for (const auto image : {this->sourceImages.first.handle(), this->sourceImages.second.handle()})
        vk::releaseExternalImage(ctx.vk, cmdbuf.handle(), image, 0,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    cmdbuf.end(ctx.vk);
    cmdbuf.submit(ctx.vk); // wait for completion

    this->constantBufferTimestamps.reserve(this->ctx.constantBuffers.size());
    for (size_t i = 0; i < this->ctx.constantBuffers.size(); ++i) {
        this->constantBufferTimestamps.push_back(
            backend::getDefaultConstantBuffer(
                i, this->ctx.constantBuffers.size(),
                this->ctx.hdr, this->ctx.flow
            ).timestamp
        );
    }
}

void Instance::scheduleFrames(Context& context) { // NOLINT (static)
#ifdef FGVK_TESTING_RENDERDOC
    const auto& impl = this->m_impl;
    if (impl->getRenderDocAPI()) {
        impl->getRenderDocAPI()->StartFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(impl->getVulkan().inst()),
            nullptr);
    }
#endif
    try {
        context.scheduleFrames();
    } catch (const std::exception& e) {
        throw backend::error("Unable to schedule frames", e);
    }
#ifdef FGVK_TESTING_RENDERDOC
    if (impl->getRenderDocAPI()) {
        impl->getVulkan().df().DeviceWaitIdle(impl->getVulkan().dev());
        impl->getRenderDocAPI()->EndFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(impl->getVulkan().inst()),
            nullptr);
    }
#endif
}

void Instance::enableFrameProfiling(Context& context, const bool collectCompleted) { // NOLINT (static)
    context.enableFrameProfiling(collectCompleted);
}

std::optional<FrameProfile> Instance::takeCompletedFrameProfile(Context& context) { // NOLINT (static)
    return context.takeCompletedFrameProfile();
}

FrameProfile Instance::readFrameProfile(const Context& context) const { // NOLINT (static)
    return context.readFrameProfile();
}

void Instance::enableSplitProfile(Context& context, bool retainCompleted) { // NOLINT (static)
    context.enableSplitProfile(retainCompleted);
}

std::vector<SplitFrameProfile> Instance::takeCompletedSplitProfiles(Context& context) { // NOLINT (static)
    return context.takeCompletedSplitProfiles();
}

void Instance::enablePassStepProfile(Context& context) { // NOLINT (static)
    context.enablePassStepProfile();
}

std::vector<PassStepSummary> Instance::takePassStepSummaries(Context& context) { // NOLINT (static)
    return context.takePassStepSummaries();
}

bool Instance::splitProfileEnabled(const Context& context) const { // NOLINT (static)
    return context.splitProfileEnabled();
}

std::optional<CompletedBatchEnd> Instance::latestBatchEnd(const Context& context) const { // NOLINT (static)
    return context.latestBatchEnd();
}

std::optional<bool> Instance::previousBatchPending(const Context& context) const { // NOLINT (static)
    return context.previousBatchPending();
}

void Instance::pollSplitProfiles(Context& context) { // NOLINT (static)
    context.pollSplitProfiles();
}

void Context::enableSplitProfile(const bool retainCompleted) {
    if (this->fidx != 0 || this->hasScheduledWork() || this->split)
        throw backend::error("Split profiling requires a fresh unscheduled context");
    const auto& vulkan = this->ctx.vk.get();
    const auto calibrate = splitCalibrationFunction(vulkan);
    if (!calibrate) {
        reportSplitUnavailable("calibration");
        return;
    }
    try {
        auto state = std::make_unique<SplitProfilingState>();
        state->retainCompleted = retainCompleted;
        for (auto& pool : state->pools)
            pool.emplace(vulkan, 4);
        const auto& first = *state->pools.front();
        state->clock = vk::CalibratedClock(vulkan.dev(), calibrate,
            first.timestampValidBits(), first.timestampPeriodNanoseconds());
        if (!state->clock.recalibrate()) {
            reportSplitUnavailable("calibration");
            return;
        }
        state->calibratedAt = ProfileClock::now();
        this->split = std::move(state);
    } catch (const std::exception& error) {
        reportSplitUnavailable("queries", error.what());
    }
}

std::vector<SplitFrameProfile> Context::takeCompletedSplitProfiles() {
    if (!this->split)
        return {};
    std::vector<SplitFrameProfile> profiles(this->split->completed.begin(), this->split->completed.end());
    this->split->completed.clear();
    return profiles;
}

void Context::enablePassStepProfile() {
    if (this->fidx != 0 || this->hasScheduledWork() || this->passSteps)
        throw backend::error("Pass-step profiling requires a fresh unscheduled context");
    // Without split boundaries there is no fence-ordered reader; the split path has reported why.
    if (!this->split || this->split->failed)
        return;
    const auto& vulkan = this->ctx.vk.get();
    auto state = std::make_unique<PassStepProfilingState>();
    state->overlap = backendOverlapEnabled();
    for (auto& pool : state->pools)
        pool.emplace(vulkan, PassStepProfilingState::queryCount);
    this->passSteps = std::move(state);
}

std::vector<PassStepSummary> Context::takePassStepSummaries() {
    if (!this->passSteps)
        return {};
    auto summaries = std::move(this->passSteps->completed);
    this->passSteps->completed.clear();
    return summaries;
}

void Context::collectPassSteps(const size_t phase) {
    auto& state = *this->passSteps;
    if (state.failed)
        return;
    const auto& names = state.names.at(phase);
    const size_t stepCount = names.prepass.size() + names.generated.size();
    if (stepCount == 0)
        return;
    const auto& pool = *state.pools.at(phase);
    std::array<uint64_t, PassStepProfilingState::queryCount> ticks{};
    try {
        // The batch fence has signalled, so every query it wrote is available. A query that is not ready is no sample.
        if (!pool.tryReadTimestamps(std::span{ticks}.first(stepCount + 1)))
            return;
        if (state.stepNames.empty()) {
            state.stepNames = names.prepass;
            state.stepNames.insert(state.stepNames.end(), names.generated.begin(), names.generated.end());
            state.samples.assign(stepCount, {});
            for (auto& samples : state.samples)
                samples.reserve(passWindowBatches);
        } else if (state.stepNames.size() != stepCount) {
            return;
        }
        for (size_t step = 0; step < stepCount; ++step) {
            const double microseconds = pool.elapsedMicroseconds(ticks.at(step), ticks.at(step + 1));
            state.samples.at(step).push_back(static_cast<int64_t>(std::llround(microseconds * 1000.0)));
        }
        if (++state.batches < passWindowBatches)
            return;
        for (size_t step = 0; step < state.samples.size(); ++step) {
            auto& samples = state.samples.at(step);
            std::ranges::sort(samples);
            state.completed.push_back(PassStepSummary{
                .name = state.stepNames.at(step),
                .step = static_cast<uint32_t>(step),
                .batches = samples.size(),
                .p50Ns = nearestRankNs(samples, 50),
                .p95Ns = nearestRankNs(samples, 95),
                .overlap = state.overlap,
            });
            samples.clear();
        }
        state.batches = 0;
        if (state.completed.size() > passSummaryCapacity)
            state.completed.erase(state.completed.begin(),
                state.completed.begin() + static_cast<std::ptrdiff_t>(state.completed.size() - passSummaryCapacity));
    } catch (const std::exception& error) {
        state.failed = true;
        reportPassStepUnavailable(error.what());
    }
}

void Context::recordSplitBatch(const bool generated) {
    if (!this->split)
        return;
    this->splitBatches.at(this->fidx % inFlightDepth) = SplitBatch{
        .frame = static_cast<uint64_t>(this->fidx),
        .phase = this->fidx % commandPhaseCount,
        .generated = generated,
        .valid = true,
    };
}

void Context::collectSplitProfiles(const std::array<bool, inFlightDepth>& signalled) {
    auto& state = *this->split;
    if (state.failed)
        return;

    // Refresh the device/host pair at most once per second. A failed call keeps the previous pair.
    const auto now = ProfileClock::now();
    if (now - state.calibratedAt >= std::chrono::seconds(1)) {
        state.calibratedAt = now;
        static_cast<void>(state.clock.recalibrate());
    }
    for (size_t slot = 0; slot < inFlightDepth; ++slot) {
        auto& batch = this->splitBatches.at(slot);
        // Only a slot whose fence this prepareWork observed signalled is read.
        if (!signalled.at(slot) || !batch.valid)
            continue;
        batch.valid = false;
        if (this->passSteps && batch.generated)
            this->collectPassSteps(batch.phase);
        try {
            std::array<uint64_t, 4> ticks{};
            if (!state.pools.at(batch.phase)->tryReadTimestamps(ticks))
                continue;
            const auto batchEndNs = state.clock.monotonicNanoseconds(ticks[3]);
            // Slots are read in slot order, not batch order: keep the newest batch. Zero means uncalibrated.
            if (batchEndNs > 0 && (!state.latest || batch.frame > state.latest->backendFrameIndex))
                state.latest = CompletedBatchEnd{.backendFrameIndex = batch.frame, .batchEndNs = batchEndNs};
            if (!batch.generated || !state.retainCompleted)
                continue;
            state.completed.push_back(SplitFrameProfile{
                .backendFrameIndex = batch.frame,
                .fgStartNs = state.clock.monotonicNanoseconds(ticks[0]),
                .prepassEndNs = state.clock.monotonicNanoseconds(ticks[1]),
                .generatedEndNs = state.clock.monotonicNanoseconds(ticks[2]),
                .batchEndNs = batchEndNs,
            });
            if (state.completed.size() > splitQueueCapacity)
                state.completed.pop_front();
        } catch (const std::exception& error) {
            state.failed = true;
            reportSplitUnavailable("queries", error.what());
            return;
        }
    }
}

void Context::enableFrameProfiling(const bool collectCompleted) {
    if (this->fidx != 0 || this->hasScheduledWork() || this->profiling)
        throw backend::error("Frame profiling requires a fresh unscheduled context");
    this->profiling = std::make_unique<FrameProfilingState>(this->ctx.vk, this->destImages.size(), collectCompleted);
}

std::optional<FrameProfile> Context::takeCompletedFrameProfile() {
    return this->profiling ? this->profiling->completed.take() : std::nullopt;
}

FrameProfile Context::readFrameProfile() const {
    if (!this->profiling || !this->hasScheduledWork() || this->profiling->generatedCount == 0)
        throw backend::error("No generated batch is available for frame profiling");
    const auto& state = *this->profiling;
    const auto ticks = state.queries.timestamps(static_cast<uint32_t>(4 + state.generatedCount * 3));
    auto result = state.timings;
    result.backendFrameIndex = this->fidx - 1;
    result.gpuPrepassUs = state.queries.elapsedMicroseconds(ticks.at(0), ticks.at(2));
    result.gpuInputConversionUs = this->sourceColorConversion
        ? state.queries.elapsedMicroseconds(ticks.at(0), ticks.at(1)) : 0.0;
    result.gpuMotionEstimationUs = state.queries.elapsedMicroseconds(
        ticks.at(this->sourceColorConversion ? 1 : 0), ticks.at(2));
    result.gpuGeneratedUs.reserve(state.generatedCount);
    result.gpuSynthesisUs.reserve(state.generatedCount);
    result.gpuOutputConversionUs.reserve(state.generatedCount);
    for (size_t i = 0; i < state.generatedCount; ++i) {
        const size_t begin = 4 + i * 3;
        result.gpuGeneratedUs.push_back(state.queries.elapsedMicroseconds(ticks.at(begin), ticks.at(begin + 2)));
        result.gpuSynthesisUs.push_back(state.queries.elapsedMicroseconds(
            ticks.at(begin), ticks.at(begin + (this->destColorConversion ? 1 : 2))));
        result.gpuOutputConversionUs.push_back(this->destColorConversion
            ? state.queries.elapsedMicroseconds(ticks.at(begin + 1), ticks.at(begin + 2)) : 0.0);
    }
    result.gpuSpanUs = state.queries.elapsedMicroseconds(ticks.front(), ticks.at(3));
    result.timestampValidBits = state.queries.timestampValidBits();
    result.timestampPeriodNs = state.queries.timestampPeriodNanoseconds();
    return result;
}

void Instance::scheduleFrames(Context& context, std::span<const float> timestamps) { // NOLINT (static)
    if (timestamps.empty())
        throw backend::error("At least one interpolation timestamp is required");
#ifdef FGVK_TESTING_RENDERDOC
    const auto& impl = this->m_impl;
    if (impl->getRenderDocAPI()) {
        impl->getRenderDocAPI()->StartFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(impl->getVulkan().inst()),
            nullptr);
    }
#endif
    try {
        context.scheduleFrames(timestamps);
    } catch (const std::exception& e) {
        throw backend::error("Unable to schedule frames", e);
    }
#ifdef FGVK_TESTING_RENDERDOC
    if (impl->getRenderDocAPI()) {
        impl->getVulkan().df().DeviceWaitIdle(impl->getVulkan().dev());
        impl->getRenderDocAPI()->EndFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(impl->getVulkan().inst()),
            nullptr);
    }
#endif
}

void Instance::scheduleFrameHistory(Context& context) { // NOLINT (static)
    context.scheduleFrameHistory();
}

bool Instance::contextReady(const Context& context) const {
    return context.waitForIdle(0);
}

void Context::prepareWork(const bool drainAll) {
    const auto start = this->profiling ? ProfileClock::now() : ProfileClock::time_point{};
    // Profiling drains every batch so that its readback sees completed queries.
    const bool waitAll = drainAll || this->profiling != nullptr;
    const size_t current = this->fidx % inFlightDepth;
    std::array<bool, inFlightDepth> signalled{};
    if (!waitScheduledFences(this->fenceScheduled, inFlightDepth, current, waitAll,
            previousWorkFenceTimeoutNs,
            [this](const size_t slot, uint64_t& remainingNs) {
                return waitWithBudget(this->cmdbufFences.at(slot), this->ctx.vk, remainingNs);
            },
            signalled)) {
        std::cerr << "framegen-vulkan: backend work fence timed out after "
                  << previousWorkFenceTimeout.count() << " ms; "
                     "aborting frame scheduling\n";
        throw backend::error("Timeout waiting for previous frame to complete");
    }
    if (this->split)
        this->collectSplitProfiles(signalled);
    if (this->profiling) {
        const double previousWaitUs = profileMicroseconds(start);
        // The fence waits above prove completion before queries are reused.
        // Diagnostic readback must never change scheduling/recovery. It still
        // reads the scheduled flags, so they are cleared only after it runs.
        try {
            this->profiling->completed.collectAfterCompletion(this->fidx,
                this->hasScheduledWork() && this->profiling->generatedCount > 0, [&] {
                const auto readStarted = ProfileClock::now();
                auto completed = this->readFrameProfile();
                completed.cpuReadbackUs = profileMicroseconds(readStarted);
                return completed;
            });
        } catch (const std::exception& error) {
            std::cerr << "framegen-vulkan: GPU pacing profile unavailable; reason=query-readback; "
                      << error.what() << '\n';
        }
        this->profiling->timings.cpuPreviousWaitUs = previousWaitUs;
        this->profiling->timings.cpuGenerationSubmitUs = 0.0;
    }
    for (size_t slot = 0; slot < inFlightDepth; ++slot) {
        if (signalled.at(slot))
            this->fenceScheduled.at(slot) = false;
    }
}

const vk::CommandBuffer& Context::recordPrepass() {
    auto& recorded = this->cmdbufs.at(0).at(this->fidx % commandPhaseCount);
    if (!recorded) {
        vk::CommandBuffer cmdbuf(ctx.vk);
        cmdbuf.begin(ctx.vk, 0);

        // A pipeline barrier's first scope includes every earlier submission on
        // this queue, so this batch's prepass starts only after all previous
        // batches' work. This replaces the CPU wait that used to serialize the
        // single-instance pyramid and history images.
        const VkMemoryBarrier batchOrder{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        ctx.vk.get().df().CmdPipelineBarrier(cmdbuf.handle(),
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 1, &batchOrder, 0, nullptr, 0, nullptr);

        if (this->split) {
            // Reset this phase's pool before its first split boundary of the batch.
            const auto& pool = this->split->pools.at(this->fidx % commandPhaseCount).value();
            pool.reset(cmdbuf.handle());
            pool.write(cmdbuf.handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
        }
        if (this->profiling) {
            this->profiling->queries.reset(cmdbuf.handle());
            this->profiling->queries.write(cmdbuf.handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
        }
        // GP1: reset this phase's step pool before its first write, then mark the batch start.
        PassStepMarks marks;
        if (this->passSteps) {
            const auto phase = this->fidx % commandPhaseCount;
            auto& names = this->passSteps->names.at(phase);
            names.prepass.clear();
            const auto& pool = *this->passSteps->pools.at(phase);
            pool.reset(cmdbuf.handle());
            marks = PassStepMarks(pool, cmdbuf.handle(), 1, names.prepass);
            marks.start();
        }

        const std::array sourceAcquires{
            vk::externalImageAcquire(this->sourceImages.first.handle(), ctx.vk.get().queueFamilyIndex(), VK_ACCESS_SHADER_READ_BIT),
            vk::externalImageAcquire(this->sourceImages.second.handle(), ctx.vk.get().queueFamilyIndex(), VK_ACCESS_SHADER_READ_BIT),
        };
        ctx.vk.get().df().CmdPipelineBarrier(cmdbuf.handle(),
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(sourceAcquires.size()), sourceAcquires.data());

        // Recorded once per phase, never per frame: the SDR recording has no stage here.
        if (this->sourceColorConversion)
            this->sourceColorConversion->render(ctx.vk, cmdbuf, this->fidx);
        if (this->profiling)
            this->profiling->queries.write(cmdbuf.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);
        this->mipmaps.render(ctx.vk, cmdbuf, this->fidx);
        marks.mark("mipmaps");
        // Pyramid levels share no writable images. Record one dependency per
        // stage, allowing the seven levels to fill the GPU together.
        std::vector<ManagedDispatch> batch;
        batch.reserve(this->alpha0.size());
        for (size_t stage = 0; stage < 3; ++stage) {
            batch.clear();
            for (size_t i = 0; i < 7; ++i)
                batch.push_back(this->alpha0.at(6 - i).getDispatch(stage));
            ManagedShader::dispatchIndependent(ctx.vk, cmdbuf, batch);
            marks.mark(alpha0Steps.at(stage));
        }
        batch.clear();
        for (size_t i = 0; i < 7; ++i)
            batch.push_back(this->alpha1.at(6 - i).getDispatch(this->fidx));
        ManagedShader::dispatchIndependent(ctx.vk, cmdbuf, batch);
        marks.mark("alpha1");
        this->beta0.render(ctx.vk, cmdbuf, this->fidx);
        marks.mark("beta0");
        if (!marks) {
            this->beta1.render(ctx.vk, cmdbuf);
        } else if (backendOverlapEnabled()) {
            const auto steps = this->beta1.dispatches();
            ManagedShader::dispatchGroups(ctx.vk, cmdbuf, steps, [&](size_t begin, size_t end) {
                marks.mark(passRunName<std::string_view>(beta1Steps, begin, end));
            });
        } else {
            const auto steps = this->beta1.dispatches();
            for (size_t set = 0; set < steps.size(); ++set) {
                steps.at(set).shader.get().dispatch(ctx.vk, cmdbuf, steps.at(set).extent);
                marks.mark(beta1Steps.at(set));
            }
        }

        if (this->profiling)
            this->profiling->queries.write(cmdbuf.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 2);
        if (this->split)
            this->split->pools.at(this->fidx % commandPhaseCount).value().write(
                cmdbuf.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);

        cmdbuf.end(ctx.vk);
        recorded.emplace(std::move(cmdbuf));
    }
    return *recorded;
}

const vk::CommandBuffer& Context::recordSourceRelease() {
    auto& recorded = this->sourceRelease.at(this->fidx % commandPhaseCount);
    if (!recorded) {
        vk::CommandBuffer command(ctx.vk);
        command.begin(ctx.vk, 0);
        // This command follows the last generated output in the same submission, so a
        // bottom-of-pipe boundary here marks that output's completion for any output count.
        if (this->split)
            this->split->pools.at(this->fidx % commandPhaseCount).value().write(
                command.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 2);
        const std::array releases{
            vk::externalImageRelease(this->sourceImages.first.handle(), ctx.vk.get().queueFamilyIndex(), VK_ACCESS_SHADER_READ_BIT),
            vk::externalImageRelease(this->sourceImages.second.handle(), ctx.vk.get().queueFamilyIndex(), VK_ACCESS_SHADER_READ_BIT),
        };
        ctx.vk.get().df().CmdPipelineBarrier(command.handle(),
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(releases.size()), releases.data());
        // Include the final ownership handoff in the profiled batch span.
        if (this->profiling)
            this->profiling->queries.write(command.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 3);
        if (this->split)
            this->split->pools.at(this->fidx % commandPhaseCount).value().write(
                command.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 3);
        command.end(ctx.vk);
        recorded.emplace(std::move(command));
    }
    return *recorded;
}

void Context::schedulePrepass(const VkFence completionFence) {
    const auto& cmdbuf = this->recordPrepass();
    const std::array commands{cmdbuf.handle(), this->recordSourceRelease().handle()};
    // Preserve the completed fence while allocation/recording can still fail.
    // From the first submit onward, retain the context even if a later submit
    // fails before the final fence can be signalled.
    this->cmdbufFences.at(this->fidx % inFlightDepth).reset(this->ctx.vk);
    this->fenceScheduled.at(this->fidx % inFlightDepth) = true;
    const auto start = this->profiling ? ProfileClock::now() : ProfileClock::time_point{};
    vk::CommandBuffer::submitCommands(this->ctx.vk,
        std::span{commands}.first(completionFence != VK_NULL_HANDLE ? 2 : 1),
        {}, this->syncSemaphore.handle(), this->idx,
        {}, this->prepassSemaphore.handle(), this->idx,
        completionFence
    );
    if (this->profiling)
        this->profiling->timings.cpuPrepassSubmitUs = profileMicroseconds(start);

    this->idx++;
}

void Context::scheduleFrames() {
    this->scheduleFrames({});
}

void Context::scheduleFrames(std::span<const float> timestamps) {
    const bool explicitTimestamps = !timestamps.empty();
    const size_t generatedFrameCount = explicitTimestamps
        ? timestamps.size()
        : this->destImages.size();
    if (generatedFrameCount > this->destImages.size())
        throw backend::error("Too many interpolation timestamps for this context");
    if (generatedFrameCount == 0) {
        this->scheduleFrameHistory();
        return;
    }

    float previousTimestamp = 0.0F;
    if (explicitTimestamps) {
        for (const float timestamp : timestamps) {
            if (timestamp <= previousTimestamp || timestamp >= 1.0F)
                throw backend::error(
                    "Interpolation timestamps must be strictly increasing between 0 and 1"
                );
            previousTimestamp = timestamp;
        }
    }

    // A changed timestamp rewrites a constant buffer that in-flight batches may
    // still read, so drain them all first. Unchanged timestamps write nothing.
    bool drainAll = false;
    if (explicitTimestamps) {
        for (size_t i = 0; i < timestamps.size(); ++i)
            drainAll = drainAll || timestamps[i] != this->constantBufferTimestamps.at(i);
    }
    this->prepareWork(drainAll);
    this->recordSplitBatch(true);
    if (this->profiling)
        this->profiling->generatedCount = generatedFrameCount;

    // Every generated pass has its own uniform buffer. prepareWork drained all
    // in-flight batches before any changed timestamp, so this upload cannot
    // race with a batch that still reads the old value.
    if (explicitTimestamps) {
        for (size_t i = 0; i < timestamps.size(); ++i) {
            backend::uploadChangedTimestamp(
                this->constantBufferTimestamps, i, timestamps[i],
                [this, i](const float timestamp) {
                    auto constants = backend::getDefaultConstantBuffer(
                        0, 1, this->ctx.hdr, this->ctx.flow
                    );
                    constants.timestamp = timestamp;
                    this->ctx.constantBuffers.at(i).write(
                        this->ctx.vk, constants
                    );
                }
            );
        }
    }

    // Finish fallible allocation/recording before submitting any work. A cold
    // cache failure must not leave a prepass without its completion fence.
    const auto& sourceReleaseCommand = this->recordSourceRelease();
    // GP1 names the generated steps after this phase's prepass, so record the prepass first.
    if (this->passSteps)
        static_cast<void>(this->recordPrepass());
    for (size_t i = 0; i < generatedFrameCount; i++) {
        auto& recorded = this->cmdbufs.at(i + 1).at(this->fidx % commandPhaseCount);
        if (!recorded) {
            vk::CommandBuffer cmdbuf(ctx.vk);
            cmdbuf.begin(ctx.vk, 0);

            if (this->profiling)
                this->profiling->queries.write(cmdbuf.handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, static_cast<uint32_t>(4 + i * 3));

            // Every generated output is fully overwritten. Discarding its old
            // contents permits taking ownership without a consumer release.
            const VkImageMemoryBarrier discard{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = this->destImages.at(i).handle(),
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            ctx.vk.get().df().CmdPipelineBarrier(cmdbuf.handle(),
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &discard);

            // GP1 times the first output only; later outputs record unmarked.
            PassStepMarks marks;
            if (this->passSteps && i == 0) {
                const auto phase = this->fidx % commandPhaseCount;
                auto& names = this->passSteps->names.at(phase);
                names.generated.clear();
                const auto& pool = *this->passSteps->pools.at(phase);
                marks = PassStepMarks(pool, cmdbuf.handle(), 1 + static_cast<uint32_t>(names.prepass.size()),
                    names.generated);
            }
            const auto& pass = this->passes.at(i);
            // GP2 (FGVK_BACKEND_OVERLAP=1): Gamma0(j+1) joins the Delta work of level j when the
            // independence helper accepts it; otherwise the order is today's. Order never changes.
            const bool overlap = backendOverlapEnabled();
            std::array<bool, 7> gamma0Recorded{};
            for (size_t j = 0; j < 7; j++) {
                if (!gamma0Recorded.at(j)) {
                    pass.gamma0.at(j).render(ctx.vk, cmdbuf, this->fidx);
                    marks.mark(gamma0Steps.at(j));
                }
                pass.gamma1.at(j).render(ctx.vk, cmdbuf);
                marks.mark(gamma1Steps.at(j));

                if (j < 4) continue;
                if (overlap && j < 6) {
                    auto batch = pass.delta0.at(j - 4).dispatches(this->fidx);
                    const auto delta1 = pass.delta1.at(j - 4).dispatches();
                    const auto gamma0 = pass.gamma0.at(j + 1).dispatches(this->fidx);
                    batch.insert(batch.end(), delta1.begin(), delta1.end());
                    batch.insert(batch.end(), gamma0.begin(), gamma0.end());
                    if (marks) {
                        // Labels follow the batch order: two Delta0, the Delta1 dispatches, then Gamma0(j+1).
                        std::vector<std::string> labels;
                        labels.reserve(batch.size());
                        for (size_t k = 0; k < 2; ++k)
                            labels.push_back("delta0." + std::to_string(j - 4));
                        for (size_t k = 0; k < delta1.size(); ++k)
                            labels.push_back("delta1." + std::to_string(j - 4));
                        labels.push_back("gamma0." + std::to_string(j + 1));
                        ManagedShader::dispatchGroups(ctx.vk, cmdbuf, batch,
                            [&](size_t begin, size_t end) {
                                marks.mark(passRunName<std::string>(labels, begin, end));
                            });
                    } else {
                        ManagedShader::dispatchGroups(ctx.vk, cmdbuf, batch);
                    }
                    gamma0Recorded.at(j + 1) = true;
                } else {
                    pass.delta0.at(j - 4).render(ctx.vk, cmdbuf, this->fidx);
                    marks.mark(delta0Steps.at(j - 4));
                    pass.delta1.at(j - 4).render(ctx.vk, cmdbuf);
                    marks.mark(delta1Steps.at(j - 4));
                }
            }
            pass.generate->render(ctx.vk, cmdbuf, this->fidx);
            marks.mark("generate");
            if (this->profiling)
                this->profiling->queries.write(cmdbuf.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, static_cast<uint32_t>(5 + i * 3));
            if (this->destColorConversion)
                this->destColorConversion->render(ctx.vk, cmdbuf, i);

            vk::releaseExternalImage(ctx.vk, cmdbuf.handle(), this->destImages.at(i).handle(),
                VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

            if (this->profiling)
                this->profiling->queries.write(cmdbuf.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, static_cast<uint32_t>(6 + i * 3));

            cmdbuf.end(ctx.vk);
            recorded.emplace(std::move(cmdbuf));
        }
    }

    if (generatedFrameCount == 1) {
        const auto& prepass = this->recordPrepass();
        auto& recordedDependency = this->prepassDependency.at(this->fidx % commandPhaseCount);
        if (!recordedDependency) {
            // Keep this dependency separate from the phase recordings so the
            // multiple-output path retains its existing barriers and overlap.
            vk::CommandBuffer dependency(ctx.vk);
            dependency.begin(ctx.vk, 0);
            const VkMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            };
            ctx.vk.get().df().CmdPipelineBarrier(dependency.handle(),
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &barrier, 0, nullptr, 0, nullptr);
            dependency.end(ctx.vk);
            recordedDependency.emplace(std::move(dependency));
        }
        const std::array commands{
            prepass.handle(), recordedDependency->handle(),
            this->cmdbufs.at(1).at(this->fidx % commandPhaseCount)->handle(),
            sourceReleaseCommand.handle(),
        };
        // Finish fallible recording before resetting the completion fence.
        this->cmdbufFences.at(this->fidx % inFlightDepth).reset(this->ctx.vk);
        this->fenceScheduled.at(this->fidx % inFlightDepth) = true;
        const auto start = this->profiling ? ProfileClock::now() : ProfileClock::time_point{};
        vk::CommandBuffer::submitCommands(this->ctx.vk, commands,
            {}, this->syncSemaphore.handle(), this->idx,
            {}, this->syncSemaphore.handle(), this->idx + 1, this->currentFence());
        if (this->profiling) {
            this->profiling->timings.cpuPrepassSubmitUs = 0.0;
            this->profiling->timings.cpuGenerationSubmitUs = profileMicroseconds(start);
        }
        this->idx += 2;
    } else {
        this->schedulePrepass(VK_NULL_HANDLE);
        const auto start = this->profiling ? ProfileClock::now() : ProfileClock::time_point{};
        for (size_t i = 0; i < generatedFrameCount; i++) {
            const auto& cmdbuf = *this->cmdbufs.at(i + 1).at(this->fidx % commandPhaseCount);
            const std::array commands{cmdbuf.handle(), sourceReleaseCommand.handle()};
            vk::CommandBuffer::submitCommands(this->ctx.vk,
                std::span{commands}.first(i == generatedFrameCount - 1 ? 2 : 1),
                {}, this->prepassSemaphore.handle(), this->idx - 1,
                {}, this->syncSemaphore.handle(), this->idx + i,
                i == generatedFrameCount - 1 ? this->currentFence() : VK_NULL_HANDLE);
        }
        if (this->profiling)
            this->profiling->timings.cpuGenerationSubmitUs = profileMicroseconds(start);
        this->idx += generatedFrameCount;
    }
    this->fidx++;
}

void Context::scheduleFrameHistory() {
    this->prepareWork(false);
    this->recordSplitBatch(false);
    if (this->profiling)
        this->profiling->generatedCount = 0;
    this->schedulePrepass(this->currentFence());
    this->fidx++;
}

bool Context::waitForIdle(const uint64_t timeoutNs) const {
    // One budget covers every scheduled batch, so timeoutNs == 0 only polls.
    return pollScheduledFences(this->fenceScheduled, inFlightDepth, timeoutNs,
        [this](const size_t slot, uint64_t& remainingNs) {
            return waitWithBudget(this->cmdbufFences.at(slot), this->ctx.vk, remainingNs);
        });
}

void Context::pollSplitProfiles() {
    if (!this->split || this->split->failed)
        return;
    // prepareWork observes only the slot it reuses (three batches back). One non-blocking
    // status query per scheduled slot finds newer completed batches; nothing waits.
    std::array<bool, inFlightDepth> signalled{};
    for (size_t slot = 0; slot < inFlightDepth; ++slot)
        signalled.at(slot) = this->fenceScheduled.at(slot) && this->splitBatches.at(slot).valid
            && this->cmdbufFences.at(slot).signalled(this->ctx.vk);
    this->collectSplitProfiles(signalled);
}

std::optional<bool> Context::previousBatchPending() const {
    // Every scheduled batch increments fidx, so the last one owns the slot before it.
    if (this->fidx == 0) return std::nullopt;
    const size_t slot = (this->fidx - 1) % inFlightDepth;
    // A slot prepareWork already observed complete is no longer scheduled.
    if (!this->fenceScheduled.at(slot)) return false;
    return !this->cmdbufFences.at(slot).signalled(this->ctx.vk);
}

void Instance::collectRetiredContexts() {
    std::erase_if(this->m_retiredContexts,
        [](const std::unique_ptr<ContextImpl>& context) {
            try {
                return context->waitForIdle(0);
            } catch (const std::exception& e) {
                std::cerr << "framegen-vulkan: unable to poll a retired backend context: "
                          << e.what() << '\n';
                return false;
            }
        });
}

void Instance::closeContext(const Context& context) {
    this->collectRetiredContexts();
    auto it = std::ranges::find_if(this->m_contexts,
        [context = &context](const std::unique_ptr<ContextImpl>& ctx) {
            return ctx.get() == context;
        });
    if (it == this->m_contexts.end())
        throw backend::error("attempted to close unknown context",
            std::runtime_error("no such context"));

    try {
        if ((*it)->waitForIdle(previousWorkFenceTimeoutNs)) {
            this->m_contexts.erase(it);
            return;
        }
    } catch (const std::exception& e) {
        std::cerr << "framegen-vulkan: unable to wait for backend context retirement: "
                  << e.what() << '\n';
    }

    // Never issue a device-wide indefinite wait while the game is replacing a
    // swapchain. Keep the exceptional in-flight context alive and reclaim it
    // on a later open/close once its own completion fence signals.
    std::cerr << "framegen-vulkan: backend context did not retire within "
              << previousWorkFenceTimeout.count() << " ms; "
                 "deferring resource destruction\n";
    this->m_retiredContexts.push_back(std::move(*it));
    this->m_contexts.erase(it);
}

Instance::~Instance() = default;

// leaking shenanigans

namespace {
    bool leaking{false}; // NOLINT (global variable)
}

InstanceImpl::~InstanceImpl() {
    if (!leaking) return;

    try {
        new vk::Vulkan(std::move(this->vk));
    } catch (...) {
        std::cerr << "framegen-vulkan: failed to leak Vulkan instance\n";
    }

}

void backend::makeLeaking() {
    leaking = true;
}
