/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wayland_presentation_feedback.hpp"
#include "present_diagnostics.hpp"
#include "swapchain/screen_samples.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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
    // X6: without diagnostics the observer still reports the output refresh, and asks for feedback only on the cadence.
    fgvk_test_wayland_mode(0);
    {
        auto observer=WaylandPresentationFeedback::create(fgvk_test_wayland_display(),fgvk_test_wayland_surface(),false);
        expect(static_cast<bool>(observer));
        expect(observer->refreshNs()==0);
        lowerResult=VK_SUCCESS;
        const int calls=lowerCalls;
        for (int i=0;i<480;++i) {
            expect(WaylandPresentationFeedback::observe(observer.get(),lower,VK_NULL_HANDLE,info,7,7,false)==VK_SUCCESS);
            // Present 0's feedback is dispatched by present 1, so the refresh is known from present 1 on.
            expect(observer->refreshNs()==(i==0 ? 0u : 8'000'000u));
            // X7: a non-zero refresh keeps the output's mode rate unused, although the output is bound.
            expect(observer->outputMilliHz()==0);
        }
        expect(lowerCalls==calls+480);
        expect(fgvk_test_wayland_feedbacks()==2); // presents 0 and 240 only
        observer->stop();
        expect(observer->refreshNs()==8'000'000u); // stop keeps the last value
        expect(fgvk_test_wayland_objects()==0);
        observer.reset();
        expect(fgvk_test_wayland_objects()==0);
    }
    // X7: under variable refresh (presented refresh 0) the output's current mode, 165000 mHz, is the rate.
    // Mode 6 names the output in sync_output; mode 7 sends none and uses the only bound output. With one
    // output both give 165000, so this does not test which one wins; uncapped_auto_cap_tests does.
    for (int mode : {6, 7}) {
        fgvk_test_wayland_mode(mode);
        auto observer=WaylandPresentationFeedback::create(fgvk_test_wayland_display(),fgvk_test_wayland_surface(),false);
        expect(static_cast<bool>(observer));
        expect(observer->outputMilliHz()==0);
        lowerResult=VK_SUCCESS;
        const int calls=lowerCalls;
        for (int i=0;i<480;++i) {
            expect(WaylandPresentationFeedback::observe(observer.get(),lower,VK_NULL_HANDLE,info,7,7,false)==VK_SUCCESS);
            // Present 0's feedback is dispatched by present 1, so the output rate is known from present 1 on.
            expect(observer->refreshNs()==0);
            expect(observer->outputMilliHz()==(i==0 ? 0u : 165'000u));
        }
        expect(lowerCalls==calls+480);
        expect(fgvk_test_wayland_feedbacks()==2); // presents 0 and 240 only, as with a known refresh
        observer->stop();
        expect(observer->outputMilliHz()==165'000u); // stop keeps the last value
        expect(fgvk_test_wayland_objects()==0);
        observer.reset();
        expect(fgvk_test_wayland_objects()==0);
    }
    // OS1: an open screen sink asks for feedback on every present, and each presented event reaches it with
    // its context, source, generated flag and CLOCK_MONOTONIC time. Presented event k is dispatched by present k+1.
    {
        fgvk_test_wayland_mode(0);
        auto observer=WaylandPresentationFeedback::create(fgvk_test_wayland_display(),fgvk_test_wayland_surface(),false);
        expect(static_cast<bool>(observer));
        auto ring=std::make_shared<fgvk::layer::ScreenSampleRing>();
        observer->setScreenSink(ring);
        lowerResult=VK_SUCCESS;
        for (int i=0;i<12;++i)
            expect(WaylandPresentationFeedback::observe(observer.get(),lower,VK_NULL_HANDLE,info,
                200,static_cast<uint64_t>(i/2+1),i%2)==VK_SUCCESS);
        expect(fgvk_test_wayland_feedbacks()==12); // every present asks while the sink is open
        uint64_t cursor=0;
        std::array<fgvk::layer::ScreenSample,16> out{};
        const size_t count=ring->read(&cursor,out.data(),out.size());
        expect(count==11); // presents 0 to 10; present 11's event is still pending
        for (size_t k=0;k<count;++k) {
            expect(out[k].context==200 && out[k].source==k/2+1 && out[k].generated==(k%2==1));
            // The fixture's presented time is 10 s plus the commit's sequence times 8 ms.
            expect(out[k].screenNs==10'000'000'000LL+static_cast<int64_t>(k+1)*8'000'000LL);
            expect(out[k].startedNs > 0 && out[k].receivedNs >= out[k].startedNs && out[k].flags == 7);
        }
        observer->stop();
        expect(fgvk_test_wayland_objects()==0);
        observer.reset();
        expect(fgvk_test_wayland_objects()==0);
    }
    // OS1: without an open sink (closed, or its ring gone) only the refresh cadence asks, as before.
    for (int variant=0;variant<2;++variant) {
        fgvk_test_wayland_mode(0);
        auto observer=WaylandPresentationFeedback::create(fgvk_test_wayland_display(),fgvk_test_wayland_surface(),false);
        expect(static_cast<bool>(observer));
        if (variant==0) {
            auto ring=std::make_shared<fgvk::layer::ScreenSampleRing>();
            observer->setScreenSink(ring);
            ring->close();
        } else {
            auto ring=std::make_shared<fgvk::layer::ScreenSampleRing>();
            observer->setScreenSink(ring);
        }
        lowerResult=VK_SUCCESS;
        for (int i=0;i<480;++i)
            expect(WaylandPresentationFeedback::observe(observer.get(),lower,VK_NULL_HANDLE,info,7,7,false)==VK_SUCCESS);
        expect(fgvk_test_wayland_feedbacks()==2); // presents 0 and 240 only
        observer->stop();
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
