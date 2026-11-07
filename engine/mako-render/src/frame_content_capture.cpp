/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "frame_content_capture.hpp"
#include "present_diagnostics.hpp"
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace mako::layer;
namespace {
uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
template<class T> uint64_t handleId(T value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(value);
    else return static_cast<uint64_t>(value);
}
VkImageMemoryBarrier barrier(VkImage image, VkImageLayout before, VkImageLayout after,
        VkAccessFlags source, VkAccessFlags destination) {
    return {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=source, .dstAccessMask=destination,
        .oldLayout=before, .newLayout=after,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image, .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
}
}

std::shared_ptr<FrameContentCapture> FrameContentCapture::create(uint64_t context) noexcept {
    const char* enabled=std::getenv("MAKO_PACING_PROFILE_CONTENT");
    if (!enabled || std::string_view(enabled)!="1" || std::endian::native!=std::endian::little) return {};
    try { return std::shared_ptr<FrameContentCapture>(new FrameContentCapture(context)); }
    catch (...) { return {}; }
}
FrameContentCapture::FrameContentCapture(uint64_t id):context(id) {
    const char* requested = std::getenv("MAKO_PACING_PROFILE_CONTENT_SOURCES");
    const auto limit = sourceLimit(requested ? std::string_view(requested) : std::string_view{});
    if (!limit) throw std::runtime_error("invalid MAKO_PACING_PROFILE_CONTENT_SOURCES (expected 2..4096)");
    maxSources = *limit;
    const char* temporary=std::getenv("TMPDIR");
    std::filesystem::path directory=temporary && *temporary ? temporary : "/tmp";
    if (!directory.is_absolute()) directory="/tmp";
    path=(directory/("mako-content-"+std::to_string(geteuid())+"-"+std::to_string(getpid())+"-"+std::to_string(id)+".bin")).string();
    trigger=path+".trigger";
    announce("state=armed trigger="+trigger+" path="+path+" source_limit="+std::to_string(maxSources));
}
FrameContentCapture::~FrameContentCapture() { if(fd>=0) ::close(fd); }
void FrameContentCapture::announce(const std::string& fields) noexcept {
    try {
        const auto line="MAKO Renderer: present diagnostics: operation=pacing-profile-content version=1 pid="+
            std::to_string(getpid())+" context="+std::to_string(context)+" "+fields+"\n";
        present_diagnostics::writePacingProfileRecords(line);
        std::cerr<<line;
    } catch (...) {}
}
void FrameContentCapture::write(const void* data, size_t size) {
    auto* bytes=static_cast<const unsigned char*>(data);
    while(size) {
        const auto n=::write(fd,bytes,size);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) throw std::runtime_error("content capture write failed");
        bytes+=n; size-=static_cast<size_t>(n);
    }
}
void FrameContentCapture::beginSource(const vk::Vulkan& vk, VkFormat format, uint64_t ordinal,
        uint64_t backend, VkQueue caller, VkQueue present) noexcept {
    active=false;
    if(disabled || batches>=maxSources || pending[0] || pending[1]) return;
    try {
        if(fd<0) {
            const auto now=std::chrono::steady_clock::now();
            if(now<pollAfter) return;
            pollAfter=now+std::chrono::seconds(1);
            struct stat status{};
            if(::lstat(trigger.c_str(),&status)!=0 || !S_ISREG(status.st_mode) || status.st_uid!=geteuid()) return;
            if(!vk.supportsOptimalTilingFormatFeatures(format,VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
               !vk.supportsOptimalTilingFormatFeatures(VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_FEATURE_BLIT_DST_BIT))
                throw std::runtime_error("content capture blit unsupported");
            // No diagnostic GPU allocation before the explicit warm-scene trigger.
            reduced.emplace(vk,VkExtent2D{width,height},VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            const std::vector<uint8_t> zeroes(imageBytes*2);
            staging.emplace(vk,zeroes.data(),zeroes.size(),VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
            if(fd<0) throw std::runtime_error("content capture file unavailable");
            announce("state=recording path="+path+" sources="+std::to_string(maxSources)+" width=192 height=108 format=rgba8");
        }
        source=ordinal; backendFrame=backend; callerQueue=handleId(caller); privateQueue=handleId(present);
        ++batches; active=true;
    } catch(const std::exception& error) {
        disabled=true; announce(std::string("state=unavailable reason=")+error.what());
    }
}
void FrameContentCapture::record(const vk::Vulkan& vk, VkCommandBuffer command, VkImage image,
        VkExtent2D extent, uint32_t imageIndex, bool generated) noexcept {
    const size_t slot=generated ? 1 : 0;
    if(!active || pending[slot] || !extent.width || !extent.height) return;
    // No allocations, waits, submissions, or fallible operations while recording.
    const std::array before{
        barrier(image,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT),
        barrier(reduced->handle(),VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT)};
    vk.df().CmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,nullptr,0,nullptr,before.size(),before.data());
    const VkImageBlit region{
        .srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .srcOffsets={{0,0,0},{static_cast<int32_t>(extent.width),static_cast<int32_t>(extent.height),1}},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstOffsets={{0,0,0},{width,height,1}}};
    vk.df().CmdBlitImage(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,reduced->handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region,VK_FILTER_NEAREST);
    const std::array after{
        barrier(image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT),
        barrier(reduced->handle(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT)};
    vk.df().CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,0,nullptr,0,nullptr,after.size(),after.data());
    const VkBufferImageCopy copy{.bufferOffset=slot*imageBytes,
        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={width,height,1}};
    vk.df().CmdCopyImageToBuffer(command,reduced->handle(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        staging->handle(),1,&copy);
    const VkBufferMemoryBarrier visible{.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=staging->handle(),.offset=slot*imageBytes,.size=imageBytes};
    vk.df().CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,0,nullptr,1,&visible,0,nullptr);
    records[slot]={0x31544e4f434b414dULL,maxSources > 128 ? 2u : 1u,context,source,backendFrame,generated,imageIndex,
        handleId(image),nowNs(),0,handleId(vk.queue()),callerQueue,privateQueue,width,height,imageBytes,batches};
    pending[slot]=true;
}
void FrameContentCapture::completed(const vk::Vulkan& vk) noexcept {
    if(!pending[0] && !pending[1]) return;
    if(!disabled) try {
        const auto completedAt=nowNs();
        const auto bytes=staging->read(vk);
        for(size_t slot=0;slot<2;++slot) if(pending[slot]) {
            records[slot][9]=completedAt;
            write(records[slot].data(),sizeof(records[slot]));
            write(bytes.data()+slot*imageBytes,imageBytes);
        }
        if(batches>=maxSources) announce("state=complete path="+path+" sources="+std::to_string(batches));
    } catch(const std::exception& error) {
        disabled=true; announce(std::string("state=unavailable reason=")+error.what());
    }
    pending={}; active=false;
}
