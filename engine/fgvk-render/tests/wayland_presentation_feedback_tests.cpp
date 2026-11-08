/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wayland_presentation_feedback.hpp"
#include "present_diagnostics.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

extern "C" {
wl_display* fgvk_test_wayland_display();
wl_surface* fgvk_test_wayland_surface();
int fgvk_test_wayland_objects();
int fgvk_test_wayland_feedbacks();
void fgvk_test_wayland_mode(int);
void fgvk_test_wayland_commit();
}
using fgvk::layer::WaylandPresentationFeedback;
namespace {
void expect(bool value) { if (!value) std::abort(); }
int lowerCalls{};
VkResult lowerResult=VK_SUCCESS;
const VkPresentInfoKHR* expected{};
VkResult VKAPI_CALL lower(VkQueue, const VkPresentInfoKHR* info) {
    expect(info == expected);
    ++lowerCalls;
    fgvk_test_wayland_commit();
    return lowerResult;
}
}
int main() {
    char directory[]="/tmp/fgvk-wayland-feedback-test-XXXXXX";
    expect(mkdtemp(directory));
    const auto path=std::filesystem::path(directory)/"capture.log";
    setenv("FGVK_PACING_PROFILE_WAYLAND","1",1);
    setenv("FGVK_PACING_PROFILE_LOG",path.c_str(),1);
    uint32_t image=2;
    VkPresentInfoKHR info{.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.swapchainCount=1,.pImageIndices=&image};
    expected=&info;
    for (int mode=0;mode<=5;++mode) {
        fgvk_test_wayland_mode(mode);
        auto observer=WaylandPresentationFeedback::create(fgvk_test_wayland_display(),fgvk_test_wayland_surface(),true);
        expect(static_cast<bool>(observer));
        const int rounds=mode==4 ? 270 : 12;
        for (int i=0;i<rounds;++i) {
            lowerResult=i%2 ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
            expect(WaylandPresentationFeedback::observe(observer.get(),lower,VK_NULL_HANDLE,info,
                static_cast<uint64_t>(100+mode),static_cast<uint64_t>(i/2+1),i%2)==lowerResult);
        }
        if (mode==1 || mode==2) expect(fgvk_test_wayland_feedbacks()==0);
        else if (mode==4) expect(fgvk_test_wayland_feedbacks()==256);
        else expect(fgvk_test_wayland_feedbacks()==rounds);
        observer->stop();
        expect(fgvk_test_wayland_objects()==0);
        const int calls=lowerCalls;
        lowerResult=VK_ERROR_OUT_OF_DATE_KHR;
        expect(WaylandPresentationFeedback::observe(observer.get(),lower,VK_NULL_HANDLE,info,9,9,false)==lowerResult);
        expect(lowerCalls==calls+1);
        observer.reset();
        expect(fgvk_test_wayland_objects()==0);
    }
    // Records reach the file through the writer; the test reads it now, so flush first.
    fgvk::layer::present_diagnostics::flushPacingProfileForTests();
    std::ifstream file(path);
    const std::string contents((std::istreambuf_iterator<char>(file)),{});
    expect(contents.find("event=presented")!=std::string::npos);
    expect(contents.find("event=discarded")!=std::string::npos);
    expect(contents.find("event=abandoned")!=std::string::npos);
    expect(contents.find("event=unavailable")!=std::string::npos);
    for (const auto* field : {"reason=2", "reason=3", "clock_id=1", "sequence=1", "flags=7", "context=100", "generated=1"})
        expect(contents.find(field)!=std::string::npos);
    // Samples are queued, not buffered per second, so this run's few hundred samples are all kept.
    // A gap is only reported when the recorder queue itself is full, which this test never reaches.
    expect(contents.find("pacing-profile-wayland-gap")==std::string::npos);
    std::filesystem::remove_all(directory);
}
