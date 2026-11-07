/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "native_copy_profile.hpp"
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <unistd.h>

using namespace mako::layer::present_diagnostics;
namespace {
size_t creates{}, destroys{}, resets{}, writes{}, reads{}, calibrations{};
bool gpuComplete{};
VkResult createResult = VK_SUCCESS, queryResult = VK_SUCCESS;
std::array<bool, 10> available{};
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
VkResult VKAPI_CALL create(VkDevice, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks*, VkQueryPool* pool) {
    ++creates;
    expect(info->queryType == VK_QUERY_TYPE_TIMESTAMP && info->queryCount == 10, "unbounded/incorrect query pool");
    if (createResult == VK_SUCCESS) *pool = reinterpret_cast<VkQueryPool>(1);
    return createResult;
}
void VKAPI_CALL destroy(VkDevice, VkQueryPool, const VkAllocationCallbacks*) { ++destroys; }
void VKAPI_CALL reset(VkCommandBuffer, VkQueryPool, uint32_t first, uint32_t count) {
    expect(first == 0 && count == 10, "query reset changed capacity");
    ++resets;
    available = {};
}
void VKAPI_CALL write(VkCommandBuffer, VkPipelineStageFlagBits stage, VkQueryPool, uint32_t query) {
    expect(query < 10 && stage == (query % 2 ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT), "incorrect GPU timestamp boundary");
    ++writes;
    available[query] = true;
}
VkResult VKAPI_CALL results(VkDevice, VkQueryPool, uint32_t first, uint32_t count, size_t size, void* data, VkDeviceSize stride, VkQueryResultFlags flags) {
    expect(gpuComplete, "queries read before existing completion proof");
    expect(first == 0 && count <= 10 && size == count * 16 && stride == 16 &&
        flags == (VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT), "blocking or incorrectly sized query read");
    ++reads;
    for (size_t i = 0; i < count; ++i) {
        const std::array<uint64_t, 2> value{100 + i * 10, available[i] ? 1U : 0U};
        std::memcpy(static_cast<char*>(data) + i * stride, value.data(), sizeof(value));
    }
    return queryResult;
}
VkResult VKAPI_CALL calibrate(VkDevice, uint32_t count, const VkCalibratedTimestampInfoKHR* info, uint64_t* values, uint64_t* deviation) {
    expect(count == 2 && info[0].timeDomain == VK_TIME_DOMAIN_DEVICE_KHR &&
        info[1].timeDomain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR, "mixed calibration domains");
    ++calibrations;
    values[0] = 200;
    values[1] = 1000000000;
    *deviation = 100;
    return VK_SUCCESS;
}
}
int main() {
    const auto path = std::filesystem::path("/tmp") / ("mako-native-copy-profile-test-" + std::to_string(getpid()) + ".log");
    setenv("MAKO_PACING_PROFILE_TRACE", "1", 1);
    setenv("MAKO_PACING_PROFILE_LOG", path.c_str(), 1);
    initializePacingProfileLog();
    const NativeCopyProfile::Functions functions{create, destroy, reset, write, results, calibrate};
    const auto command = reinterpret_cast<VkCommandBuffer>(2);
    const auto queue = reinterpret_cast<VkQueue>(3);
    {
        NativeCopyProfile profile({}, functions, 64, 1.0F, 77);
        expect(profile.ready() && creates == 1, "query pool initialization failed");
        profile.beginCopy(command, queue, 1, false);
        expect(writes == 0, "unarmed observer recorded GPU commands");
        expect(profile.beginSource(9, 3, {}), "source not admitted");
        profile.beginCopy(command, queue, 1, false);
        profile.endCopy(command, false);
        profile.endCopy(command, false);
        for (uint32_t i = 0; i < 4; ++i) {
            profile.beginCopy(command, queue, i + 2, true, i);
            profile.endCopy(command, true, i);
        }
        expect(resets == 1 && writes == 10 && calibrations == 1, "missing or duplicate query writes");
        expect(!profile.beginSource(10, 4, {}), "in-flight pool reused without completion");
        profile.beginCopy(command, queue, 1, false);
        profile.endCopy(command, false);
        profile.beginCopy(command, queue, 2, true);
        profile.endCopy(command, true);
        expect(writes == 10 && resets == 1, "a rejected source wrote stale in-flight queries");
        available[9] = false;
        queryResult = VK_NOT_READY;
        gpuComplete = true; // Models the owner's existing final-copy fence.
        profile.completed();
        profile.completed();
        expect(reads == 1 && calibrations == 2, "GPU samples repeated or added polling");
        expect(profile.beginSource(11, 4, {}), "completed pool could not be reused");
        profile.beginCopy(command, queue, 1, false);
        profile.endCopy(command, false);
        queryResult = VK_ERROR_DEVICE_LOST;
        profile.completed();
        expect(reads == 2, "query error changed collection behavior");
    }
    expect(destroys == 1, "query pool not retired exactly once");
    {
        NativeCopyProfile unsupported({}, functions, 0, 1.0F, 78);
        expect(!unsupported.ready() && creates == 1, "unsupported timestamps allocated a query pool");
        NativeCopyProfile invalid({}, functions, 64, std::numeric_limits<float>::quiet_NaN(), 78);
        expect(!invalid.ready() && creates == 1, "invalid period accepted");
        createResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        NativeCopyProfile failed({}, functions, 64, 1.0F, 78);
        expect(!failed.ready(), "failed initialization marked ready");
    }
    std::ifstream input(path);
    const std::string log{std::istreambuf_iterator<char>(input), {}};
    expect(log.find("source=9 generation=3") != std::string::npos &&
        log.find("source=10 ") == std::string::npos && log.find("source=11 generation=4") != std::string::npos &&
        log.find("query_result=1 available=0") != std::string::npos &&
        log.find("query_result=-4 available=0") != std::string::npos &&
        log.find("before_result=0") != std::string::npos && log.find("after_result=0") != std::string::npos,
        "GPU logger lost resource/source identity, query failures or calibration evidence");
    std::filesystem::remove(path);
    std::cout << "native copy GPU profile tests passed\n";
}
