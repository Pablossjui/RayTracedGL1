// Copyright (c) 2022 Sultim Tsyrendashiev
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

#pragma once

#include "EffectBase.h"

namespace RTGL1
{

template<typename PUSH_CONST>
struct EffectSimple : public EffectBase
{
    explicit EffectSimple(
        VkDevice device, const char *pShaderName,
        const std::shared_ptr<const Framebuffers> &framebuffers,
        const std::shared_ptr<const GlobalUniform> &uniform,
        const std::shared_ptr<const ShaderManager> &shaderManager)
    :
        EffectBase(device),
        push{},
        isCurrentlyActive(false),
        shaderName(pShaderName)
    {
        VkDescriptorSetLayout setLayouts[] =
        {
            framebuffers->GetDescSetLayout(),
            uniform->GetDescSetLayout(),
        };

        InitBase(shaderManager, setLayouts, push);
    }

protected:
    bool SetupNull()
    {
        isCurrentlyActive = false;
        return false;
    }

    bool Setup(const CommonnlyUsedEffectArguments &args, bool isActive, float transitionDurationIn, float transitionDurationOut)
    {
        bool wasActivePreviously = isCurrentlyActive;
        isCurrentlyActive = isActive;

        // if to start
        if (!wasActivePreviously && isCurrentlyActive)
        {
            push.transitionType = 0;
            push.transitionBeginTime = args.currentTime;
            push.transitionDuration = transitionDurationIn;
        }
        // if to end
        else if (wasActivePreviously && !isCurrentlyActive)
        {
            push.transitionType = 1;
            push.transitionBeginTime = args.currentTime;
            push.transitionDuration = transitionDurationOut;
        }

        return
            isCurrentlyActive ||
            (push.transitionType == 1 && args.currentTime - push.transitionBeginTime <= push.transitionDuration);
    }

public:
    FramebufferImageIndex Apply(const CommonnlyUsedEffectArguments &args, FramebufferImageIndex inputFramebuf)
    {
        VkDescriptorSet descSets[] =
        {
            args.framebuffers->GetDescSet(args.frameIndex),
            args.uniform->GetDescSet(args.frameIndex),
        };

        return Dispatch(args.cmd, args.frameIndex, args.framebuffers, args.width, args.height, inputFramebuf, descSets);
    }

protected:
    bool GetPushConstData(uint8_t(&pData)[128], uint32_t *pDataSize) const override
    {
        static_assert(sizeof(push) <= 128, "");
        memcpy(pData, &push, sizeof(push));
        *pDataSize = sizeof(push);
        return true;
    }

    const char *GetShaderName() const override
    {
        return shaderName;
    }

    // Can be changed by child classes
    PUSH_CONST &GetPush()
    {
        return push.custom;
    }

protected:
    struct
    {
        uint32_t transitionType; // 0 - in, 1 - out
        float transitionBeginTime;                
        float transitionDuration;
        PUSH_CONST custom;
    } push;
private:
    bool isCurrentlyActive;
    const char *shaderName;
};


#define RTGL1_EFFECT_SIMPLE_INHERIT_CONSTRUCTOR(T, SHADER_NAME) \
    T(VkDevice device, const std::shared_ptr<const Framebuffers> &framebuffers, \
    const std::shared_ptr<const GlobalUniform> &uniform, const std::shared_ptr<const ShaderManager> &shaderManager) : \
    EffectSimple(device, SHADER_NAME, framebuffers, uniform, shaderManager) {}

}
