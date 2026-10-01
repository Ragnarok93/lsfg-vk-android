#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "v3_1p/shaders/alpha.hpp"
#include "common/utils.hpp"
#include "core/commandbuffer.hpp"
#include "core/image.hpp"

#include <utility>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

using namespace LSFG_3_1P::Shaders;

Alpha::Alpha(Vulkan& vk, Core::Image inImg) : inImg(std::move(inImg)) {
    // create resources
    this->shaderModules = {{
        vk.shaders.getShader(vk.device, "p_alpha[0]",
            { { 1, VK_DESCRIPTOR_TYPE_SAMPLER },
              { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
              { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE } }),
        vk.shaders.getShader(vk.device, "p_alpha[1]",
            { { 1, VK_DESCRIPTOR_TYPE_SAMPLER },
              { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
              { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE } }),
        vk.shaders.getShader(vk.device, "p_alpha[2]",
            { { 1, VK_DESCRIPTOR_TYPE_SAMPLER },
              { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
              { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE } }),
        vk.shaders.getShader(vk.device, "p_alpha[3]",
            { { 1, VK_DESCRIPTOR_TYPE_SAMPLER },
              { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
              { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE } })
    }};
    this->pipelines = {{
        vk.shaders.getPipeline(vk.device, "p_alpha[0]"),
        vk.shaders.getPipeline(vk.device, "p_alpha[1]"),
        vk.shaders.getPipeline(vk.device, "p_alpha[2]"),
        vk.shaders.getPipeline(vk.device, "p_alpha[3]")
    }};
    this->sampler = vk.resources.getSampler(vk.device);
    for (size_t i = 0; i < 3; i++)
        this->descriptorSets.at(i) = Core::DescriptorSet(vk.device, vk.descriptorPool, this->shaderModules.at(i));
    for (size_t i = 0; i < 3; i++)
        this->lastDescriptorSet.at(i) = Core::DescriptorSet(vk.device, vk.descriptorPool, this->shaderModules.at(3));

    // create internal images/outputs
    const VkExtent2D extent = this->inImg.getExtent();
    const VkExtent2D halfExtent = {
        .width = (extent.width + 1) >> 1,
        .height = (extent.height + 1) >> 1
    };
    this->tempImg1 = Core::Image(vk.device, halfExtent);
    this->tempImg2 = Core::Image(vk.device, halfExtent);

    const VkExtent2D quarterExtent = {
        .width = (halfExtent.width + 1) >> 1,
        .height = (halfExtent.height + 1) >> 1
    };
    for (size_t i = 0; i < 2; i++) {
        this->tempImgs3.at(i) = Core::Image(vk.device, quarterExtent);
        for (size_t j = 0; j < 3; j++)
            this->outImgs.at(j).at(i) = Core::Image(vk.device, quarterExtent);
    }

    // hook up shaders
    this->descriptorSets.at(0).update(vk.device)
        .add(VK_DESCRIPTOR_TYPE_SAMPLER, this->sampler)
        .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, this->inImg)
        .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->tempImg1)
        .build();
    this->descriptorSets.at(1).update(vk.device)
        .add(VK_DESCRIPTOR_TYPE_SAMPLER, this->sampler)
        .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, this->tempImg1)
        .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->tempImg2)
        .build();
    this->descriptorSets.at(2).update(vk.device)
        .add(VK_DESCRIPTOR_TYPE_SAMPLER, this->sampler)
        .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, this->tempImg2)
        .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->tempImgs3)
        .build();
    for (size_t i = 0; i < 3; i++)
        this->lastDescriptorSet.at(i).update(vk.device)
            .add(VK_DESCRIPTOR_TYPE_SAMPLER, this->sampler)
            .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, this->tempImgs3)
            .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs.at(i))
            .build();
}


void Alpha::PushBarriers(Utils::BarrierBuilder& barriers, uint64_t frameCount, size_t stage) {
    switch (stage) {
    case 0:
        barriers
            .addW2R(this->inImg)
            .addR2W(this->tempImg1);
        break;
    case 1:
        barriers
            .addW2R(this->tempImg1)
            .addR2W(this->tempImg2);
        break;
    case 2:
        barriers
            .addW2R(this->tempImg2)
            .addR2W(this->tempImgs3);
        break;
    case 3:
        barriers
            .addW2R(this->tempImgs3)
            .addR2W(this->outImgs.at(frameCount % 3));
        break;
    default:
        throw std::out_of_range("Alpha stage");
    }
}

void Alpha::BindStagePipeline(const Core::CommandBuffer& buf, size_t stage) {
    this->pipelines.at(stage).bind(buf);
}

void Alpha::DispatchStage(const Core::CommandBuffer& buf, uint64_t frameCount, size_t stage) {
    const auto extent = stage < 2
        ? this->tempImg1.getExtent()
        : this->tempImgs3.at(0).getExtent();
    const uint32_t threadsX = (extent.width + 7) >> 3;
    const uint32_t threadsY = (extent.height + 7) >> 3;

    if (stage < StageCount - 1) {
        this->descriptorSets.at(stage).bind(buf, this->pipelines.at(stage));
    } else {
        this->lastDescriptorSet.at(frameCount % 3).bind(buf, this->pipelines.at(stage));
    }
    buf.dispatch(threadsX, threadsY, 1);
}

void Alpha::SeedHistory(
        const Core::CommandBuffer& buf, uint64_t frameCount) {
    // Stages 0..2 are independent of the temporal slot. Run them once, then
    // write the same coherent current-frame result into all three stage-3
    // history slots. Beta therefore never reads uninitialized temporal images
    // during a Flow graph handoff.
    for (size_t stage = 0; stage + 1 < StageCount; ++stage) {
        Utils::BarrierBuilder barriers(buf);
        this->PushBarriers(barriers, frameCount, stage);
        barriers.build();
        this->BindStagePipeline(buf, stage);
        this->DispatchStage(buf, frameCount, stage);
    }

    for (size_t history = 0; history < 3; ++history) {
        Utils::BarrierBuilder barriers(buf);
        this->PushBarriers(
            barriers, frameCount + history, StageCount - 1);
        barriers.build();
        this->BindStagePipeline(buf, StageCount - 1);
        this->DispatchStage(
            buf, frameCount + history, StageCount - 1);
    }
}

void Alpha::Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount) {
    for (size_t stage = 0; stage < StageCount; ++stage) {
        Utils::BarrierBuilder barriers(buf);
        this->PushBarriers(barriers, frameCount, stage);
        barriers.build();
        this->BindStagePipeline(buf, stage);
        this->DispatchStage(buf, frameCount, stage);
    }
}
