// Copyright (c) 2020-2021 Sultim Tsyrendashiev
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "Framebuffers.h"

using namespace RTGL1;

#include "Swapchain.h"
#include "Utils.h"
#include "CmdLabel.h"

static_assert(MAX_FRAMES_IN_FLIGHT == FRAMEBUFFERS_HISTORY_LENGTH, "Framebuffers class logic must be changed if history length is not equal to max frames in flight");

FramebufferImageIndex Framebuffers::FrameIndexToFBIndex(FramebufferImageIndex framebufferImageIndex, uint32_t frameIndex)
{
    assert(frameIndex < FRAMEBUFFERS_HISTORY_LENGTH);
    assert(framebufferImageIndex >= 0 && framebufferImageIndex < ShFramebuffers_Count);

    // if framubuffer with given index can be swapped,
    // use one that is currently in use
    if (ShFramebuffers_Bindings[framebufferImageIndex] != ShFramebuffers_BindingsSwapped[framebufferImageIndex])
    {
        return (FramebufferImageIndex)(framebufferImageIndex + frameIndex);
    }

    return framebufferImageIndex;
}

Framebuffers::Framebuffers(
    VkDevice _device, 
    std::shared_ptr<MemoryAllocator> _allocator, 
    std::shared_ptr<CommandBufferManager> _cmdManager,
    std::shared_ptr<SamplerManager> _samplerManager)
: 
    device(_device),
    allocator(std::move(_allocator)),
    cmdManager(std::move(_cmdManager)),
    samplerManager(std::move(_samplerManager)),
    descSetLayout(VK_NULL_HANDLE),
    descPool(VK_NULL_HANDLE),
    descSets{}
{
    images.resize(ShFramebuffers_Count);
    imageMemories.resize(ShFramebuffers_Count);
    imageViews.resize(ShFramebuffers_Count);

    CreateDescriptors();
}

Framebuffers::~Framebuffers()
{
    DestroyImages();
    
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
}

void Framebuffers::CreateDescriptors()
{
    VkResult r;

    const uint32_t allBindingsCount = ShFramebuffers_Count * 2;
    const uint32_t samplerBindingOffset = ShFramebuffers_Count;

    std::vector<VkDescriptorSetLayoutBinding> bindings(allBindingsCount);
    uint32_t bndCount = 0;

    // gimage2D
    for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
    {
        VkDescriptorSetLayoutBinding &bnd = bindings[bndCount];

        // after swapping bindings, cur will become prev, and prev - cur
        bnd.binding = ShFramebuffers_Bindings[i];
        bnd.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bnd.descriptorCount = 1;
        bnd.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT;

        bndCount++;
    }

    // gsampler2D
    for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
    {
        VkDescriptorSetLayoutBinding &bnd = bindings[bndCount];

        if (ShFramebuffers_Sampler_Bindings[i] == FB_SAMPLER_INVALID_BINDING)
        {
            continue;
        }

        // after swapping bindings, cur will become prev, and prev - cur
        bnd.binding = ShFramebuffers_Sampler_Bindings[i];
        bnd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bnd.descriptorCount = 1;
        bnd.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        bndCount++;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = bndCount;
    layoutInfo.pBindings = bindings.data();

    r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "Framebuffers Desc set layout");

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = allBindingsCount * FRAMEBUFFERS_HISTORY_LENGTH;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = FRAMEBUFFERS_HISTORY_LENGTH;

    r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL, "Framebuffers Desc pool");

    for (uint32_t i = 0; i < FRAMEBUFFERS_HISTORY_LENGTH; i++)
    {
        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &descSetLayout;

        r = vkAllocateDescriptorSets(device, &allocInfo, &descSets[i]);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, descSets[i], VK_OBJECT_TYPE_DESCRIPTOR_SET, "Framebuffers Desc set");
    }
}

void Framebuffers::OnSwapchainCreate(const Swapchain *pSwapchain)
{
    CreateImages(pSwapchain->GetWidth(), pSwapchain->GetHeight());
}

void Framebuffers::OnSwapchainDestroy()
{
    DestroyImages();
}

void RTGL1::Framebuffers::BarrierOne(VkCommandBuffer cmd, uint32_t frameIndex, FramebufferImageIndex framebufferImageIndex)
{
    FramebufferImageIndex fs[] = { framebufferImageIndex };
    BarrierMultiple(cmd, frameIndex, fs);
}

void Framebuffers::PresentToSwapchain(
    VkCommandBuffer cmd, uint32_t frameIndex, 
    const std::shared_ptr<Swapchain> &swapchain,
    FramebufferImageIndex framebufferImageIndex, 
    uint32_t srcWidth, uint32_t srcHeight, VkImageLayout srcLayout)
{
    CmdLabel label(cmd, "Present to swapchain");


    framebufferImageIndex = FrameIndexToFBIndex(framebufferImageIndex, frameIndex);

    swapchain->BlitForPresent(
        cmd, images[framebufferImageIndex],
        srcWidth, srcHeight, srcLayout);
}

VkDescriptorSet Framebuffers::GetDescSet(uint32_t frameIndex) const
{
    return descSets[frameIndex];
}

VkDescriptorSetLayout Framebuffers::GetDescSetLayout() const
{
    return descSetLayout;
}

VkImageView Framebuffers::GetImageView(FramebufferImageIndex framebufferImageIndex, uint32_t frameIndex) const
{
    framebufferImageIndex = FrameIndexToFBIndex(framebufferImageIndex, frameIndex);
    return imageViews[framebufferImageIndex];
}

void Framebuffers::CreateImages(uint32_t width, uint32_t height)
{
    VkResult r;

    VkCommandBuffer cmd = cmdManager->StartGraphicsCmd();

    for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
    {
        VkFormat format = ShFramebuffers_Formats[i];
        FramebufferImageFlags flags = ShFramebuffers_Flags[i];

        VkExtent3D extent;
    
        extent = { width, height, 1 };

        if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_FORCE_SIZE_HALF)
        {
            extent.width = (width + 1) / 2;
            extent.height = (height + 1) / 2;
        }
                
        if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_FORCE_SIZE_THIRD)
        {
            extent.width = (width + 2) / 3;
            extent.height = (height + 2) / 3;
        }

        extent.width  = std::max(1u, extent.width); 
        extent.height = std::max(1u, extent.height);

        // create image
        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = extent;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = 
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT; 
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (flags & FB_IMAGE_FLAGS_FRAMEBUF_FLAGS_IS_ATTACHMENT)
        {
            imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }

        r = vkCreateImage(device, &imageInfo, nullptr, &images[i]);
        VK_CHECKERROR(r);

        // allocate dedicated memory
        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, images[i], &memReqs);

        imageMemories[i] = allocator->AllocDedicated(memReqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        r = vkBindImageMemory(device, images[i], imageMemories[i], 0);
        VK_CHECKERROR(r);

        // create image view
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {};
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        viewInfo.image = images[i];
        r = vkCreateImageView(device, &viewInfo, nullptr, &imageViews[i]);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, images[i], VK_OBJECT_TYPE_IMAGE, ShFramebuffers_DebugNames[i]);
        SET_DEBUG_NAME(device, imageViews[i], VK_OBJECT_TYPE_IMAGE_VIEW, ShFramebuffers_DebugNames[i]);

        // to general layout
        Utils::BarrierImage(
            cmd, images[i],
            0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }

    // image creation happens rarely
    cmdManager->Submit(cmd);
    cmdManager->WaitGraphicsIdle();

    UpdateDescriptors();

    NotifySubscribersAboutResize(width, height);
}

void Framebuffers::UpdateDescriptors()
{
    VkSampler nearestSampler = samplerManager->GetSampler(RG_SAMPLER_FILTER_NEAREST, RG_SAMPLER_ADDRESS_MODE_REPEAT, RG_SAMPLER_ADDRESS_MODE_REPEAT);

    const uint32_t allBindingsCount = ShFramebuffers_Count * 2;
    const uint32_t samplerBindingOffset = ShFramebuffers_Count;

    std::vector<VkDescriptorImageInfo> imageInfos(allBindingsCount);

    // gimage2D
    for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
    {
        imageInfos[i].sampler = VK_NULL_HANDLE;
        imageInfos[i].imageView = imageViews[i];
        imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // gsampler2D
    for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
    {
        imageInfos[samplerBindingOffset + i].sampler = nearestSampler;
        imageInfos[samplerBindingOffset + i].imageView = imageViews[i];
        imageInfos[samplerBindingOffset + i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    std::vector<VkWriteDescriptorSet> writes(allBindingsCount * FRAMEBUFFERS_HISTORY_LENGTH);
    uint32_t wrtCount = 0;

    for (uint32_t k = 0; k < FRAMEBUFFERS_HISTORY_LENGTH; k++)
    {
        // gimage2D
        for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
        {
            auto &wrt = writes[wrtCount];

            wrt.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wrt.dstSet = descSets[k];
            wrt.dstBinding = k == 0 ?
                ShFramebuffers_Bindings[i] :
                ShFramebuffers_BindingsSwapped[i];
            wrt.dstArrayElement = 0;
            wrt.descriptorCount = 1;
            wrt.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            wrt.pImageInfo = &imageInfos[i];

            wrtCount++;
        }

        // gsampler2D
        for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
        {
            auto &wrt = writes[wrtCount];

            uint32_t dstBinding = k == 0 ?
                ShFramebuffers_Sampler_Bindings[i] :
                ShFramebuffers_Sampler_BindingsSwapped[i];

            if (dstBinding == FB_SAMPLER_INVALID_BINDING)
            {
                continue;
            }

            wrt.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wrt.dstSet = descSets[k];
            wrt.dstBinding = dstBinding;
            wrt.dstArrayElement = 0;
            wrt.descriptorCount = 1;
            wrt.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wrt.pImageInfo = &imageInfos[samplerBindingOffset + i];

            wrtCount++;
        }
    }

    vkUpdateDescriptorSets(device, wrtCount, writes.data(), 0, nullptr);
}

void Framebuffers::DestroyImages()
{
    for (auto &i : images)
    {
        if (i != VK_NULL_HANDLE)
        {
            vkDestroyImage(device, i, nullptr);
            i = VK_NULL_HANDLE;
        }
    }

    for (auto &m : imageMemories)
    {
        if (m != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, m, nullptr);
            m = VK_NULL_HANDLE;
        }
    }

    for (auto &v : imageViews)
    {
        if (v != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, v, nullptr);
            v = VK_NULL_HANDLE;
        }
    }
}

void Framebuffers::NotifySubscribersAboutResize(uint32_t width, uint32_t height)
{
    for (auto &ws : subscribers)
    {
        if (auto s = ws.lock())
        {
            s->OnFramebuffersSizeChange(width, height);
        }
    }
}

void Framebuffers::Subscribe(std::shared_ptr<IFramebuffersDependency> subscriber)
{
    subscribers.emplace_back(subscriber);
}

void Framebuffers::Unsubscribe(const IFramebuffersDependency *subscriber)
{
    subscribers.remove_if([subscriber] (const std::weak_ptr<IFramebuffersDependency> &ws)
    {
        if (const auto s = ws.lock())
        {
            return s.get() == subscriber;
        }

        return true;
    });
}
