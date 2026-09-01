#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding='utf-8')

def write(path, text):
    (ROOT / path).write_text(text, encoding='utf-8')

def once(path, old, new):
    text = read(path)
    n = text.count(old)
    if n != 1:
        raise RuntimeError(f'{path}: expected one match, got {n}: {old[:100]!r}')
    write(path, text.replace(old, new, 1))

# Fix the advertised-extension regression caught during generated-code review.
once('framegen/src/core/device.cpp',
'''    const bool hasRobustness2 = hasExtension(enabledExtensions,\n        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);''',
'''    const bool hasRobustness2 = hasExtension(availableExtensions,\n        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);''')

# Game-side AHB allocator is told whether the external allocation will be bound
# directly as storage by framegen or used only as a transfer transport.
once('include/mini/image.hpp', '#include <vulkan/vulkan_core.h>\n',
     '#include <vulkan/vulkan_core.h>\n#include "lsfg_backend.hpp"\n')
once('include/mini/image.hpp',
'''        Image(VkDevice device, VkPhysicalDevice physicalDevice, VkExtent2D extent, VkFormat format,\n            VkImageUsageFlags usage, VkImageAspectFlags aspectFlags);''',
'''        Image(VkDevice device, VkPhysicalDevice physicalDevice, VkExtent2D extent, VkFormat format,\n            VkImageUsageFlags usage, VkImageAspectFlags aspectFlags,\n            LSFG::AhbTransportMode transportMode = LSFG::AhbTransportMode::DirectStorage);''')
once('src/mini/image.cpp',
'''Image::Image(VkDevice device, VkPhysicalDevice physicalDevice,\n        VkExtent2D extent, VkFormat format,\n        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags)\n        : extent(extent), format(format), aspectFlags(aspectFlags) {''',
'''Image::Image(VkDevice device, VkPhysicalDevice physicalDevice,\n        VkExtent2D extent, VkFormat format,\n        VkImageUsageFlags usage, VkImageAspectFlags aspectFlags,\n        LSFG::AhbTransportMode transportMode)\n        : extent(extent), format(format), aspectFlags(aspectFlags) {''')
once('src/mini/image.cpp',
'''        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE\n               | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,''',
'''        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE\n               | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT\n               | (transportMode == LSFG::AhbTransportMode::DirectStorage\n                    ? AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER : 0ULL),''')

# Expose immutable backend probe results to the wrapper after initialize() has
# selected the exact driver but before any AHB allocations are made.
for header in ('framegen/public/lsfg_3_1.hpp', 'framegen/public/lsfg_3_1p.hpp'):
    once(header,
'''    void initialize(const LSFG::DeviceIdentity& identity, VkFormat sharedFormat,\n        bool isHdr, float flowScale, uint64_t generationCount,\n        const std::function<std::vector<uint8_t>(const std::string&)>& loader);\n''',
'''    void initialize(const LSFG::DeviceIdentity& identity, VkFormat sharedFormat,\n        bool isHdr, float flowScale, uint64_t generationCount,\n        const std::function<std::vector<uint8_t>(const std::string&)>& loader);\n\n    /// Return immutable capability/provenance diagnostics for the selected backend.\n    __attribute__((visibility("default")))\n    LSFG::BackendDiagnostics getBackendDiagnostics();\n''')

for source, ns in (('framegen/v3.1_src/lsfg.cpp', 'LSFG_3_1'),
                   ('framegen/v3.1p_src/lsfg.cpp', 'LSFG_3_1P')):
    marker = f'int32_t {ns}::createContext(\n'
    text = read(source)
    idx = text.index(marker)
    getter = f'''LSFG::BackendDiagnostics {ns}::getBackendDiagnostics() {{\n    if (!device.has_value())\n        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");\n    return device->device.getDiagnostics();\n}}\n\n'''
    write(source, text[:idx] + getter + text[idx:])

# Add transport-only bookkeeping to both framegen Context classes.
for header in ('framegen/v3.1_include/v3_1/context.hpp',
               'framegen/v3.1p_include/v3_1p/context.hpp'):
    once(header,
'''    private:\n        Core::Image inImg_0, inImg_1; // inImg_0 is next when fc % 2 == 0\n        uint64_t frameIdx{0};\n''',
'''    private:\n        Core::Image inImg_0, inImg_1; // private shader images in transport-only mode\n#ifdef __ANDROID__\n        bool transportOnly{false};\n        Core::Image sharedInImg_0, sharedInImg_1;\n        std::vector<Core::Image> sharedOutImages;\n#endif\n        uint64_t frameIdx{0};\n''')

helpers = r'''
void add_external_transfer_acquire(std::vector<VkImageMemoryBarrier2>& barriers,
        Vulkan& vk, Core::Image& image, VkImageLayout newLayout,
        VkAccessFlags2 dstAccessMask) {
    if (!image.isExternalShared()) return;
    barriers.emplace_back(VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = dstAccessMask,
        .oldLayout = image.getLayout(),
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .dstQueueFamilyIndex = vk.device.getComputeFamilyIdx(),
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(), .levelCount = 1, .layerCount = 1,
        },
    });
    image.setLayout(newLayout);
}

void add_external_transfer_release(std::vector<VkImageMemoryBarrier2>& barriers,
        Vulkan& vk, Core::Image& image, VkImageLayout oldLayout,
        VkAccessFlags2 srcAccessMask) {
    if (!image.isExternalShared()) return;
    barriers.emplace_back(VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = srcAccessMask,
        .dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        .dstAccessMask = 0,
        .oldLayout = oldLayout,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = vk.device.getComputeFamilyIdx(),
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(), .levelCount = 1, .layerCount = 1,
        },
    });
    image.setLayout(VK_IMAGE_LAYOUT_GENERAL);
}

void add_local_transition(std::vector<VkImageMemoryBarrier2>& barriers,
        Core::Image& image, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
        VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
        VkImageLayout newLayout) {
    barriers.emplace_back(VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask = dstStage,
        .dstAccessMask = dstAccess,
        .oldLayout = image.getLayout(),
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(), .levelCount = 1, .layerCount = 1,
        },
    });
    image.setLayout(newLayout);
}

void copy_same_format(const Core::CommandBuffer& buf,
        Core::Image& src, Core::Image& dst) {
    const VkExtent2D extent = src.getExtent();
    const VkImageCopy region{
        .srcSubresource = { .aspectMask = src.getAspectFlags(), .layerCount = 1 },
        .dstSubresource = { .aspectMask = dst.getAspectFlags(), .layerCount = 1 },
        .extent = { extent.width, extent.height, 1 },
    };
    vkCmdCopyImage(buf.handle(), src.handle(), src.getLayout(),
        dst.handle(), dst.getLayout(), 1, &region);
}
'''

for source in ('framegen/v3.1_src/context.cpp', 'framegen/v3.1p_src/context.cpp'):
    # Insert transport helpers after the existing emit helper, before anonymous namespace closes.
    text = read(source)
    anchor = '} // namespace\n#endif\n\nContext::Context(Vulkan& vk,\n'
    if text.count(anchor) != 1:
        raise RuntimeError(f'{source}: helper anchor mismatch')
    text = text.replace(anchor, helpers + '\n} // namespace\n#endif\n\nContext::Context(Vulkan& vk,\n', 1)

    old_acquire = '''#ifdef __ANDROID__\n    {\n        std::vector<VkImageMemoryBarrier2> acquireBarriers;\n        acquireBarriers.reserve(2);\n        add_external_acquire(acquireBarriers, vk, this->inImg_0, VK_ACCESS_2_SHADER_READ_BIT);\n        add_external_acquire(acquireBarriers, vk, this->inImg_1, VK_ACCESS_2_SHADER_READ_BIT);\n        emit_external_barriers(data.cmdBuffer1, acquireBarriers);\n    }\n#endif\n'''
    new_acquire = '''#ifdef __ANDROID__\n    if (this->transportOnly) {\n        std::vector<VkImageMemoryBarrier2> barriers;\n        barriers.reserve(4);\n        add_external_transfer_acquire(barriers, vk, this->sharedInImg_0,\n            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);\n        add_external_transfer_acquire(barriers, vk, this->sharedInImg_1,\n            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);\n        add_local_transition(barriers, this->inImg_0, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,\n            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,\n            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);\n        add_local_transition(barriers, this->inImg_1, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,\n            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,\n            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);\n        emit_external_barriers(data.cmdBuffer1, barriers);\n        copy_same_format(data.cmdBuffer1, this->sharedInImg_0, this->inImg_0);\n        copy_same_format(data.cmdBuffer1, this->sharedInImg_1, this->inImg_1);\n        barriers.clear();\n        add_local_transition(barriers, this->inImg_0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,\n            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,\n            VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);\n        add_local_transition(barriers, this->inImg_1, VK_PIPELINE_STAGE_2_TRANSFER_BIT,\n            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,\n            VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);\n        add_external_transfer_release(barriers, vk, this->sharedInImg_0,\n            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);\n        add_external_transfer_release(barriers, vk, this->sharedInImg_1,\n            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT);\n        emit_external_barriers(data.cmdBuffer1, barriers);\n    } else {\n        std::vector<VkImageMemoryBarrier2> acquireBarriers;\n        acquireBarriers.reserve(2);\n        add_external_acquire(acquireBarriers, vk, this->inImg_0, VK_ACCESS_2_SHADER_READ_BIT);\n        add_external_acquire(acquireBarriers, vk, this->inImg_1, VK_ACCESS_2_SHADER_READ_BIT);\n        emit_external_barriers(data.cmdBuffer1, acquireBarriers);\n    }\n#endif\n'''
    if text.count(old_acquire) != 1:
        raise RuntimeError(f'{source}: acquire block mismatch')
    text = text.replace(old_acquire, new_acquire, 1)

    old_out_acquire = '''#ifdef __ANDROID__\n        {\n            std::vector<VkImageMemoryBarrier2> acquireBarriers;\n            acquireBarriers.reserve(1);\n            add_external_acquire(acquireBarriers, vk, this->generate.getOutImages().at(pass),\n                VK_ACCESS_2_SHADER_WRITE_BIT);\n            emit_external_barriers(buf2, acquireBarriers);\n        }\n#endif\n'''
    new_out_acquire = '''#ifdef __ANDROID__\n        if (!this->transportOnly) {\n            std::vector<VkImageMemoryBarrier2> acquireBarriers;\n            acquireBarriers.reserve(1);\n            add_external_acquire(acquireBarriers, vk, this->generate.getOutImages().at(pass),\n                VK_ACCESS_2_SHADER_WRITE_BIT);\n            emit_external_barriers(buf2, acquireBarriers);\n        }\n#endif\n'''
    if text.count(old_out_acquire) != 1:
        raise RuntimeError(f'{source}: output acquire mismatch')
    text = text.replace(old_out_acquire, new_out_acquire, 1)

    old_release = '''#ifdef __ANDROID__\n        {\n            std::vector<VkImageMemoryBarrier2> releaseBarriers;\n            releaseBarriers.reserve(pass + 1 == vk.generationCount ? 3 : 1);\n            add_external_release(releaseBarriers, vk, this->generate.getOutImages().at(pass),\n                VK_ACCESS_2_SHADER_WRITE_BIT);\n            if (pass + 1 == vk.generationCount) {\n                add_external_release(releaseBarriers, vk, this->inImg_0, VK_ACCESS_2_SHADER_READ_BIT);\n                add_external_release(releaseBarriers, vk, this->inImg_1, VK_ACCESS_2_SHADER_READ_BIT);\n            }\n            emit_external_barriers(buf2, releaseBarriers);\n        }\n#endif\n'''
    new_release = '''#ifdef __ANDROID__\n        if (this->transportOnly) {\n            auto& localOut = this->generate.getOutImages().at(pass);\n            auto& sharedOut = this->sharedOutImages.at(pass);\n            std::vector<VkImageMemoryBarrier2> barriers;\n            barriers.reserve(2);\n            add_local_transition(barriers, localOut, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,\n                VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,\n                VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);\n            add_external_transfer_acquire(barriers, vk, sharedOut,\n                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT);\n            emit_external_barriers(buf2, barriers);\n            copy_same_format(buf2, localOut, sharedOut);\n            barriers.clear();\n            add_local_transition(barriers, localOut, VK_PIPELINE_STAGE_2_TRANSFER_BIT,\n                VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,\n                VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);\n            add_external_transfer_release(barriers, vk, sharedOut,\n                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT);\n            emit_external_barriers(buf2, barriers);\n        } else {\n            std::vector<VkImageMemoryBarrier2> releaseBarriers;\n            releaseBarriers.reserve(pass + 1 == vk.generationCount ? 3 : 1);\n            add_external_release(releaseBarriers, vk, this->generate.getOutImages().at(pass),\n                VK_ACCESS_2_SHADER_WRITE_BIT);\n            if (pass + 1 == vk.generationCount) {\n                add_external_release(releaseBarriers, vk, this->inImg_0, VK_ACCESS_2_SHADER_READ_BIT);\n                add_external_release(releaseBarriers, vk, this->inImg_1, VK_ACCESS_2_SHADER_READ_BIT);\n            }\n            emit_external_barriers(buf2, releaseBarriers);\n        }\n#endif\n'''
    if text.count(old_release) != 1:
        raise RuntimeError(f'{source}: release block mismatch')
    text = text.replace(old_release, new_release, 1)

    # Replace only the Android AHB constructor; desktop FD constructor remains untouched.
    start = text.index('Context::Context(Vulkan& vk,\n        AHardwareBuffer* in0, AHardwareBuffer* in1,')
    end = text.index('\n#endif // __ANDROID__', start)
    constructor = r'''Context::Context(Vulkan& vk,
        AHardwareBuffer* in0, AHardwareBuffer* in1,
        const std::vector<AHardwareBuffer*>& outN,
        VkExtent2D extent, VkFormat format) {
    this->transportOnly = vk.device.getAhbTransportMode() == LSFG::AhbTransportMode::TransportOnly;
    if (vk.device.getAhbTransportMode() == LSFG::AhbTransportMode::Unsupported)
        throw LSFG::vulkan_error(VK_ERROR_FORMAT_NOT_SUPPORTED,
            "No compatible AHardwareBuffer transport path for framegen format");

    std::vector<Core::Image> outImgs;
    outImgs.reserve(outN.size());
    if (this->transportOnly) {
        this->sharedInImg_0 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, in0);
        this->sharedInImg_1 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, in1);
        this->sharedOutImages.reserve(outN.size());
        for (auto* ahb : outN)
            this->sharedOutImages.emplace_back(vk.device, extent, format,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, ahb);

        this->inImg_0 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        this->inImg_1 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        for (size_t i = 0; i < outN.size(); ++i)
            outImgs.emplace_back(vk.device, extent, format,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    } else {
        // Fast path: exactly the established direct shader-storage AHB binding.
        this->inImg_0 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, in0);
        this->inImg_1 = Core::Image(vk.device, extent, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, in1);
        for (auto* ahb : outN)
            outImgs.emplace_back(vk.device, extent, format,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, ahb);
    }

    for (size_t i = 0; i < 8; i++) {
        auto& data = this->data.at(i);
        data.internalSemaphores.resize(vk.generationCount);
        data.outSemaphores.resize(vk.generationCount);
        data.completionFences.resize(vk.generationCount);
        data.cmdBuffers2.resize(vk.generationCount);
    }

    this->mipmaps = Shaders::Mipmaps(vk, this->inImg_0, this->inImg_1);
    for (size_t i = 0; i < 7; i++)
        this->alpha.at(i) = Shaders::Alpha(vk, this->mipmaps.getOutImages().at(i));
    this->beta = Shaders::Beta(vk, this->alpha.at(0).getOutImages());
    for (size_t i = 0; i < 7; i++) {
        this->gamma.at(i) = Shaders::Gamma(vk,
            this->alpha.at(6 - i).getOutImages(),
            this->beta.getOutImages().at(std::min<size_t>(6 - i, 5)),
            (i == 0) ? std::nullopt : std::make_optional(this->gamma.at(i - 1).getOutImage()));
        if (i < 4) continue;
        this->delta.at(i - 4) = Shaders::Delta(vk,
            this->alpha.at(6 - i).getOutImages(),
            this->beta.getOutImages().at(6 - i),
            (i == 4) ? std::nullopt : std::make_optional(this->gamma.at(i - 1).getOutImage()),
            (i == 4) ? std::nullopt : std::make_optional(this->delta.at(i - 5).getOutImage1()),
            (i == 4) ? std::nullopt : std::make_optional(this->delta.at(i - 5).getOutImage2()));
    }
    this->generate = Shaders::Generate(vk,
        this->inImg_0, this->inImg_1,
        this->gamma.at(6).getOutImage(),
        this->delta.at(2).getOutImage1(),
        this->delta.at(2).getOutImage2(),
        std::move(outImgs));
}
'''
    text = text[:start] + constructor + text[end:]
    write(source, text)

# The wrapper selects AHB allocation usage from framegen's already-probed exact ICD.
text = read('src/context.cpp')
needle = '''    lsfgInitialize(\n        info.identity, format,\n        conf.hdr, 1.0F / conf.flowScale, conf.multiplier - 1,\n        [](const std::string& name) {\n            auto dxbc = Extract::getShader(name);\n            auto spirv = Extract::translateShader(dxbc);\n            return spirv;\n        }\n    );\n\n    // Android path: use AHardwareBuffer-backed images for sharing with framegen.\n'''
replacement = '''    lsfgInitialize(\n        info.identity, format,\n        conf.hdr, 1.0F / conf.flowScale, conf.multiplier - 1,\n        [](const std::string& name) {\n            auto dxbc = Extract::getShader(name);\n            auto spirv = Extract::translateShader(dxbc);\n            return spirv;\n        }\n    );\n\n    const LSFG::BackendDiagnostics backendDiagnostics = conf.performance\n        ? LSFG_3_1P::getBackendDiagnostics()\n        : LSFG_3_1::getBackendDiagnostics();\n    const auto ahbTransportMode = backendDiagnostics.ahbTransportMode;\n    if (ahbTransportMode == LSFG::AhbTransportMode::Unsupported)\n        throw LSFG::vulkan_error(VK_ERROR_FORMAT_NOT_SUPPORTED,\n            "Exact game/framegen ICD has no supported AHB image transport for LSFG format");\n\n    // Android path: use AHardwareBuffer-backed images for sharing with framegen.\n'''
if text.count(needle) != 1:
    raise RuntimeError('src/context.cpp: initialize probe insertion mismatch')
text = text.replace(needle, replacement, 1)
text = text.replace('''        extent, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);''',
                    '''        extent, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,\n        ahbTransportMode);''', 2)
text = text.replace('''            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);''',
                    '''            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,\n            ahbTransportMode);''', 1)
write('src/context.cpp', text)

# Static contract: transport-only must keep shader storage private and direct path gets DATA_BUFFER.
once('tests/android_capability_architecture_test.py',
'''        self.assertIn("VK_IMAGE_USAGE_TRANSFER_DST_BIT", device)\n''',
'''        self.assertIn("VK_IMAGE_USAGE_TRANSFER_DST_BIT", device)\n        quality = (ROOT / "framegen/v3.1_src/context.cpp").read_text(encoding="utf-8")\n        performance = (ROOT / "framegen/v3.1p_src/context.cpp").read_text(encoding="utf-8")\n        for source in (quality, performance):\n            self.assertIn("transportOnly", source)\n            self.assertIn("sharedOutImages", source)\n            self.assertIn("vkCmdCopyImage", source)\n            self.assertIn("VK_IMAGE_USAGE_TRANSFER_SRC_BIT", source)\n            self.assertIn("VK_IMAGE_USAGE_TRANSFER_DST_BIT", source)\n''')

# Self-remove; the temporary workflow removes itself too.
(ROOT / 'scripts/apply_task2_ahb_transport.py').unlink()
wf = ROOT / '.github/workflows/task2-ahb-bootstrap.yml'
if wf.exists(): wf.unlink()
print('Task 2 AHB transport refactor applied')
