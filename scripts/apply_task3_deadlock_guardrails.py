#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding='utf-8')


def write(path, text):
    (ROOT / path).write_text(text, encoding='utf-8')


def once(path, old, new):
    text = read(path)
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{path}: expected exactly one match, got {count}: {old[:120]!r}')
    write(path, text.replace(old, new, 1))


# Wrapper: bounded host waits/acquires. A timeout propagates as a Vulkan error;
# hooks.cpp drops the LSFG context so subsequent presents fall through natively.
once('src/context.cpp',
'''#ifdef __ANDROID__\nnamespace {\n\nVkImageSubresourceRange colorSubresourceRange() {''',
'''#ifdef __ANDROID__\nnamespace {\n\nuint64_t runtimeWaitTimeoutNs() {\n    constexpr uint64_t defaultMs = 250;\n    constexpr uint64_t maxMs = 5000;\n    const char* raw = std::getenv("LSFG_VK_WAIT_TIMEOUT_MS");\n    if (raw == nullptr || *raw == '\\0')\n        return defaultMs * 1000000ULL;\n    char* end = nullptr;\n    const unsigned long long parsed = std::strtoull(raw, &end, 10);\n    if (end == raw || *end != '\\0' || parsed == 0)\n        return defaultMs * 1000000ULL;\n    const uint64_t boundedMs = parsed > maxMs ? maxMs : static_cast<uint64_t>(parsed);\n    return boundedMs * 1000000ULL;\n}\n\nVkImageSubresourceRange colorSubresourceRange() {''')
text = read('src/context.cpp')
if 'UINT64_MAX' not in text:
    raise RuntimeError('src/context.cpp: expected unbounded waits before task3')
write('src/context.cpp', text.replace('UINT64_MAX', 'runtimeWaitTimeoutNs()'))

# Swapchain capacity + exact blit capability probing. Preserve the established
# path unchanged when all probes pass; otherwise create the game's original
# swapchain without LSFG wrapping.
once('src/hooks.cpp',
'''    VkResult myvkCreateSwapchainKHR(\n            VkDevice device,''',
'''    bool supportsBidirectionalBlit(VkPhysicalDevice physicalDevice,\n            VkFormat sharedFormat, VkFormat swapchainFormat) {\n        auto getFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(\n            Layer::ovkGetInstanceProcAddr(layerInstance, "vkGetPhysicalDeviceFormatProperties"));\n        if (getFormatProperties == nullptr)\n            return false;\n\n        VkFormatProperties sharedProperties{};\n        VkFormatProperties swapchainProperties{};\n        getFormatProperties(physicalDevice, sharedFormat, &sharedProperties);\n        getFormatProperties(physicalDevice, swapchainFormat, &swapchainProperties);\n        constexpr VkFormatFeatureFlags required =\n            VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;\n        return (sharedProperties.optimalTilingFeatures & required) == required\n            && (swapchainProperties.optimalTilingFeatures & required) == required;\n    }\n\n    VkResult myvkCreateSwapchainKHR(\n            VkDevice device,''')
once('src/hooks.cpp',
'''        VkSwapchainCreateInfoKHR createInfo = *pCreateInfo;\n        const uint32_t maxImages = surfaceCapabilities.maxImageCount == 0\n            ? UINT32_MAX : surfaceCapabilities.maxImageCount;\n        // LSFG needs one additional image available while the game's acquired\n        // image is being transformed. Queue-family numbering is unrelated and\n        // is vendor-defined, so it must not influence swapchain sizing.\n        createInfo.minImageCount = pCreateInfo->minImageCount + 1;\n        if (createInfo.minImageCount > maxImages) {\n            createInfo.minImageCount = maxImages;\n            Utils::logLimitN("swapCount", 10,\n                "Requested image count (" +\n                    std::to_string(pCreateInfo->minImageCount) + ") "\n                "exceeds maximum allowed (" +\n                    std::to_string(maxImages) + "). "\n                "Continuing with maximum allowed image count. "\n                "This might lead to performance degradation.");\n        } else {\n            Utils::resetLimitN("swapCount");\n        }\n\n        createInfo.imageUsage |= requiredTransferUsage;''',
'''        VkSwapchainCreateInfoKHR createInfo = *pCreateInfo;\n        const uint32_t requiredHeadroom = static_cast<uint32_t>(\n            std::max(1, activeConf.multiplier - 1));\n        const uint32_t maxImageCount = surfaceCapabilities.maxImageCount;\n        if (pCreateInfo->minImageCount > UINT32_MAX - requiredHeadroom) {\n            std::cerr << "lsfg-vk: init stage=swapchain-insufficient-headroom minImageCount="\n                      << pCreateInfo->minImageCount\n                      << " maxImageCount=" << maxImageCount\n                      << " requiredHeadroom=" << requiredHeadroom\n                      << "; preserving original swapchain\\n";\n            return Layer::ovkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);\n        }\n        const uint32_t requiredImageCount = pCreateInfo->minImageCount + requiredHeadroom;\n        std::cerr << "lsfg-vk: init stage=swapchain-capacity minImageCount="\n                  << pCreateInfo->minImageCount\n                  << " maxImageCount=" << maxImageCount\n                  << " requiredHeadroom=" << requiredHeadroom\n                  << " multiplier=" << activeConf.multiplier << "\\n";\n        if (maxImageCount != 0 && requiredImageCount > maxImageCount) {\n            std::cerr << "lsfg-vk: init stage=swapchain-insufficient-headroom minImageCount="\n                      << pCreateInfo->minImageCount\n                      << " maxImageCount=" << maxImageCount\n                      << " requiredHeadroom=" << requiredHeadroom\n                      << "; preserving original swapchain\\n";\n            return Layer::ovkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);\n        }\n        createInfo.minImageCount = requiredImageCount;\n        Utils::resetLimitN("swapCount");\n\n        const VkFormat sharedFormat = activeConf.hdr\n            ? VK_FORMAT_R8G8B8A8_UNORM\n            : VK_FORMAT_R16G16B16A16_SFLOAT;\n        if (!supportsBidirectionalBlit(\n                deviceInfo.physicalDevice, sharedFormat, pCreateInfo->imageFormat)) {\n            std::cerr << "lsfg-vk: init stage=blit-format-unsupported sharedFormat="\n                      << sharedFormat << " swapchainFormat=" << pCreateInfo->imageFormat\n                      << "; preserving original swapchain\\n";\n            return Layer::ovkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);\n        }\n\n        createInfo.imageUsage |= requiredTransferUsage;''')
once('src/hooks.cpp',
'''            Utils::logLimitN("swapPresent", 5,\n                "An error occurred while presenting the swapchain:\\n"\n                "- " + std::string(e.what()));\n            return VK_ERROR_INITIALIZATION_FAILED;''',
'''            Utils::logLimitN("swapPresent", 5,\n                "An error occurred while presenting the swapchain; degrading to native presentation:\\n"\n                "- " + std::string(e.what()));\n            swapchains.erase(*pPresentInfo->pSwapchains);\n            std::cerr << "lsfg-vk: runtime stage=context-degraded-bypass reason=present-error\\n";\n            return VK_ERROR_INITIALIZATION_FAILED;''')

# Both framegen implementations use the same bounded fence policy. The public
# completion check lets lifecycle code avoid vkDeviceWaitIdle entirely.
for prefix in ('v3.1', 'v3.1p'):
    hdr = f'framegen/{prefix}_include/v3_1{ "p" if prefix.endswith("p") else "" }/context.hpp'
    once(hdr,
'''        void present(Vulkan& vk,\n            int inSem, const std::vector<int>& outSem);\n''',
'''        void present(Vulkan& vk,\n            int inSem, const std::vector<int>& outSem);\n\n        /// Wait only for this context's submitted fences, using the bounded\n        /// LSFG_VK_WAIT_TIMEOUT_MS budget. Returns false on timeout.\n        bool waitForCompletion(Vulkan& vk);\n''')

    cpp = f'framegen/{prefix}_src/context.cpp'
    text = read(cpp)
    if '#include <cstdlib>' not in text:
        text = text.replace('#include <cstdint>\n', '#include <cstdint>\n#include <cstdlib>\n', 1)
    marker = f'using namespace LSFG_3_1{ "P" if prefix.endswith("p") else "" };\n'
    helper = marker + '''\nnamespace {\nuint64_t framegenWaitTimeoutNs() {\n    constexpr uint64_t defaultMs = 250;\n    constexpr uint64_t maxMs = 5000;\n    const char* raw = std::getenv("LSFG_VK_WAIT_TIMEOUT_MS");\n    if (raw == nullptr || *raw == '\\0')\n        return defaultMs * 1000000ULL;\n    char* end = nullptr;\n    const unsigned long long parsed = std::strtoull(raw, &end, 10);\n    if (end == raw || *end != '\\0' || parsed == 0)\n        return defaultMs * 1000000ULL;\n    const uint64_t boundedMs = parsed > maxMs ? maxMs : static_cast<uint64_t>(parsed);\n    return boundedMs * 1000000ULL;\n}\n}\n'''
    if 'framegenWaitTimeoutNs()' not in text:
        if marker not in text:
            raise RuntimeError(f'{cpp}: namespace marker missing')
        text = text.replace(marker, helper, 1)
    if 'UINT64_MAX' not in text:
        raise RuntimeError(f'{cpp}: expected unbounded slot wait')
    text = text.replace('UINT64_MAX', 'framegenWaitTimeoutNs()')
    insert_before = '\n#ifdef __ANDROID__\n\n#include <android/hardware_buffer.h>'
    method = '''\nbool Context::waitForCompletion(Vulkan& vk) {\n    for (auto& renderData : this->data) {\n        if (!renderData.shouldWait)\n            continue;\n        for (auto& fence : renderData.completionFences) {\n            if (!fence.wait(vk.device, framegenWaitTimeoutNs()))\n                return false;\n        }\n        renderData.shouldWait = false;\n    }\n    return true;\n}\n'''
    if insert_before not in text:
        raise RuntimeError(f'{cpp}: Android constructor marker missing')
    text = text.replace(insert_before, method + insert_before, 1)
    write(cpp, text)

# Lifecycle: bounded per-context fence completion. On a timeout, keep resources
# alive rather than destroying in-flight objects; wrapper-side degradation will
# stop new LSFG work and continue native presentation.
for prefix, ns in (('v3.1', 'LSFG_3_1'), ('v3.1p', 'LSFG_3_1P')):
    path = f'framegen/{prefix}_src/lsfg.cpp'
    text = read(path)
    old_delete = '''    vkDeviceWaitIdle(device->device.handle());\n    contexts.erase(it);'''
    new_delete = '''    if (!it->second.waitForCompletion(*device)) {\n        std::cerr << "lsfg-vk: framegen teardown timed out; retaining context resources for safe bypass\\n";\n        return;\n    }\n    contexts.erase(it);'''
    if text.count(old_delete) != 1:
        raise RuntimeError(f'{path}: deleteContext idle pattern mismatch')
    text = text.replace(old_delete, new_delete, 1)
    old_finalize = '''    vkDeviceWaitIdle(device->device.handle());\n    contexts.clear();\n    device.reset();\n    instance.reset();'''
    new_finalize = '''    for (auto& [id, context] : contexts) {\n        (void)id;\n        if (!context.waitForCompletion(*device)) {\n            std::cerr << "lsfg-vk: framegen finalize timed out; retaining Vulkan resources for safe bypass\\n";\n            return;\n        }\n    }\n    contexts.clear();\n    device.reset();\n    instance.reset();'''
    if text.count(old_finalize) != 1:
        raise RuntimeError(f'{path}: finalize idle pattern mismatch')
    text = text.replace(old_finalize, new_finalize, 1)
    old_wait = '''void ''' + ns + '''::waitIdle() {\n    if (!device.has_value()) return;\n    vkDeviceWaitIdle(device->device.handle());\n}'''
    new_wait = '''void ''' + ns + '''::waitIdle() {\n    if (!device.has_value()) return;\n    for (auto& [id, context] : contexts) {\n        (void)id;\n        if (!context.waitForCompletion(*device))\n            throw LSFG::vulkan_error(VK_TIMEOUT, "Framegen completion wait timed out");\n    }\n}'''
    if text.count(old_wait) != 1:
        raise RuntimeError(f'{path}: Android waitIdle pattern mismatch')
    text = text.replace(old_wait, new_wait, 1)
    if 'vkDeviceWaitIdle' in text:
        raise RuntimeError(f'{path}: vkDeviceWaitIdle remains after task3')
    if '#include <iostream>' not in text:
        text = text.replace('#include <functional>\n', '#include <functional>\n#include <iostream>\n', 1)
    write(path, text)

# Bootstrap artifacts from Tasks 2/3 must not survive the generated source commit.
for cleanup in (
    '.github/task2-bootstrap-trigger',
    'scripts/apply_task2_ahb_transport.py',
    'scripts/apply_task3_deadlock_guardrails.py',
    '.github/workflows/task3-guardrails-bootstrap.yml',
    '.github/task3-bootstrap-trigger',
    '.github/workflows/task1-compile-diagnostics.yml',
):
    target = ROOT / cleanup
    if target.exists():
        target.unlink()
