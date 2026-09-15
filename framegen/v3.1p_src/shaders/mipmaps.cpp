#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "v3_1p/shaders/mipmaps.hpp"
#include "adreno_b10_mipmaps.hpp"
#include "common/utils.hpp"
#include "core/image.hpp"
#include "core/commandbuffer.hpp"

#include <utility>
#include <cstddef>
#include <cstdint>
#include <iostream>

using namespace LSFG_3_1P::Shaders;

Mipmaps::Mipmaps(Vulkan& vk,
        Core::Image inImg_0, Core::Image inImg_1)
        : inImg_0(std::move(inImg_0)), inImg_1(std::move(inImg_1)) {
    // create resources
    this->shaderModule = vk.shaders.getShader(vk.device, "p_mipmaps",
        { { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
          { 1, VK_DESCRIPTOR_TYPE_SAMPLER },
          { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
          { 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE } });
    this->pipeline = vk.shaders.getPipeline(vk.device, "p_mipmaps");
#ifdef LSFGVK_ADRENO_B10_MIPMAPS
    // Only enable the split path if ShaderPool actually installed the B10
    // head. A fail-closed transform rejection keeps the original p_mipmaps
    // shader active and must remain entirely inside the baseline LSFG path.
    this->b10Enabled = vk.shaders.isB10MipmapsHeadActive();
#endif
    this->buffer = vk.resources.getBuffer(vk.device);
    this->sampler = vk.resources.getSampler(vk.device);
    for (size_t fc = 0; fc < 2; fc++)
        this->descriptorSets.at(fc) = Core::DescriptorSet(
            vk.device, vk.descriptorPool, this->shaderModule);

    // create outputs
    const VkExtent2D flowExtent{
        .width = static_cast<uint32_t>(
            static_cast<float>(this->inImg_0.getExtent().width) / vk.flowScale),
        .height = static_cast<uint32_t>(
            static_cast<float>(this->inImg_0.getExtent().height) / vk.flowScale)
    };
    for (size_t i = 0; i < 7; i++)
        this->outImgs.at(i) = Core::Image(vk.device,
            { flowExtent.width >> i, flowExtent.height >> i },
            VK_FORMAT_R8_UNORM);

#ifdef LSFGVK_ADRENO_B10_MIPMAPS
    if (this->b10Enabled) {
        const uint32_t groupsX = (flowExtent.width + 63U) >> 6U;
        const uint32_t groupsY = (flowExtent.height + 63U) >> 6U;
        const VkExtent2D scratchExtent{groupsX * 32U, groupsY * 32U};
        this->b10Scratch = Core::Image(vk.device, scratchExtent, VK_FORMAT_R32_SFLOAT);
        std::cerr << "lsfg-vk: candidate-b10-mipmaps runtime_enabled=1"
                  << " flow=" << flowExtent.width << 'x' << flowExtent.height
                  << " dispatch=" << groupsX << 'x' << groupsY
                  << " scratch=" << scratchExtent.width << 'x' << scratchExtent.height
                  << "\n";
        this->b10TailShaderModule = vk.shaders.getShader(vk.device,
            "p_mipmaps_b10_tail", { { 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE } });
        this->b10TailPipeline = vk.shaders.getPipeline(vk.device, "p_mipmaps_b10_tail");
        this->b10TailDescriptorSet = Core::DescriptorSet(
            vk.device, vk.descriptorPool, this->b10TailShaderModule);
        this->b10TailDescriptorSet.update(vk.device)
            .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->b10Scratch)
            .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs.at(2))
            .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs.at(3))
            .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs.at(4))
            .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs.at(5))
            .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs.at(6))
            .build();
    }
#endif

    // hook up shaders
    for (size_t fc = 0; fc < 2; fc++) {
        auto update = this->descriptorSets.at(fc).update(vk.device)
            .add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, this->buffer)
            .add(VK_DESCRIPTOR_TYPE_SAMPLER, this->sampler)
            .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                (fc % 2 == 0) ? this->inImg_0 : this->inImg_1);
#ifdef LSFGVK_ADRENO_B10_MIPMAPS
        if (this->b10Enabled) {
            // p_mipmaps head still has the original 7-storage-image layout.
            // Binding 5 (the old u2 slot) is repurposed as the unquantized
            // R32F scratch. Bindings 6..9 are statically unused after the
            // fail-closed head transform but remain valid descriptors.
            update
                .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs.at(0))
                .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs.at(1))
                .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->b10Scratch)
                .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs.at(3))
                .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs.at(4))
                .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs.at(5))
                .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs.at(6))
                .build();
            continue;
        }
#endif
        update.add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs).build();
    }
}

void Mipmaps::Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount) {
    const auto flowExtent = this->outImgs.at(0).getExtent();
    const uint32_t threadsX = (flowExtent.width + 63) >> 6;
    const uint32_t threadsY = (flowExtent.height + 63) >> 6;

#ifdef LSFGVK_ADRENO_B10_MIPMAPS
    if (this->b10Enabled) {
        Utils::BarrierBuilder preHead(buf);
        preHead
            .addW2R((frameCount % 2 == 0) ? this->inImg_0 : this->inImg_1)
            .addR2W(this->outImgs)
            .addR2W(this->b10Scratch);
        preHead.build();

        this->pipeline.bind(buf);
        this->descriptorSets.at(frameCount % 2).bind(buf, this->pipeline);
        buf.dispatch(threadsX, threadsY, 1);

        // Inter-dispatch image dependency replaces the original first TGSM
        // barrier. The compact tail reproduces u2..u6 with four internal
        // workgroup barriers instead of the original five-barrier 32x32 tree.
        Utils::BarrierBuilder(buf).addW2R(this->b10Scratch).build();
        this->b10TailPipeline.bind(buf);
        this->b10TailDescriptorSet.bind(buf, this->b10TailPipeline);
        buf.dispatch(threadsX, threadsY, 1);
        return;
    }
#endif

    // Preserve the original path exactly for all devices that do not activate
    // a validated B10 head, even when B10 support is compiled in.
    Utils::BarrierBuilder(buf)
        .addW2R((frameCount % 2 == 0) ? this->inImg_0 : this->inImg_1)
        .addR2W(this->outImgs)
        .build();

    this->pipeline.bind(buf);
    this->descriptorSets.at(frameCount % 2).bind(buf, this->pipeline);
    buf.dispatch(threadsX, threadsY, 1);
}
