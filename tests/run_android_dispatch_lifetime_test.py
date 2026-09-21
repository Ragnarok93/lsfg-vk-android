#!/usr/bin/env python3
"""Run actual Android dispatch and WSI ownership methods with fake downstream Vulkan.
Requires a C++20 compiler and Vulkan headers; no Android SDK or GPU.
The context shell omits unrelated scheduling/shader dependencies, not ownership logic.
"""
import os
import sys
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

def main():
    with tempfile.TemporaryDirectory(prefix='lsfg-dispatch-test-') as tmp:
        temp = Path(tmp)
        (temp / 'android').mkdir()
        (temp / 'android/hardware_buffer.h').write_text('struct AHardwareBuffer;\n')
        hooks = (ROOT / 'src/hooks.cpp').read_text()
        context = (ROOT / 'src/context.cpp').read_text()
        shell = '''
    class LsContext {
    public:
        struct RenderPassInfo { uint64_t generation{}; std::vector<Mini::Semaphore> queueConsumerSemaphores; };
        struct WsiConsumerResources { uint64_t generation{}; std::vector<Mini::Semaphore> semaphores; };
        std::vector<WsiConsumerResources> wsiConsumersByImage_;
        void transferWsiConsumersToPass(uint32_t, RenderPassInfo&, const char*);
        void retainWsiConsumersForImage(uint32_t, uint64_t, std::vector<Mini::Semaphore>, const char*);
    };
    '''
        transfer = function(context, 'void LsContext::transferWsiConsumersToPass(')
        if '--baseline-lifetime' in sys.argv:
            baseline = subprocess.check_output(['git', 'show',
                '19ae9f06cbda0adadedf2f25c9ca2c8042d9e466:src/context.cpp'], cwd=ROOT, text=True)
            transfer = function(baseline, 'void LsContext::releaseWsiConsumersForImage(')
            transfer = transfer.replace('releaseWsiConsumersForImage(', 'transferWsiConsumersToPass(')
            transfer = transfer.replace('uint32_t imageIndex, const char* reason',
                                        'uint32_t imageIndex, RenderPassInfo&, const char* reason')
        (temp / 'production_lifetime_methods.inc').write_text(
            function(hooks, 'std::shared_ptr<SwapchainState> findSwapchainState(') + '\n' + shell +
            transfer + '\n' +
            function(context, 'void LsContext::retainWsiConsumersForImage('))
        cmd = [os.environ.get('CXX', 'c++'), '-std=c++20', '-D__ANDROID__', '-pthread',
               '-Wno-attributes', '-I' + tmp, '-I' + str(ROOT / 'include'),
               '-I' + str(ROOT / 'framegen/include'), '-I' + str(ROOT / 'framegen/public')]
        if os.environ.get('VULKAN_HEADERS'):
            cmd += ['-I' + os.environ['VULKAN_HEADERS']]
        cmd += [str(ROOT / 'tests/android_dispatch_lifetime_test.cpp'), '-o', str(temp / 'test')]
        subprocess.run(cmd, check=True)
        subprocess.run([str(temp / 'test')], check=True)

if __name__ == "__main__":
    main()
