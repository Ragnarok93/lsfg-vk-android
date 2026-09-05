#!/usr/bin/env python3
import argparse
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]


def replace_once(text, old, new, label):
    if old not in text:
        raise RuntimeError(f"missing patch anchor: {label}")
    return text.replace(old, new, 1)


def patch_tests():
    p = ROOT / "tests/android_wsi_loader_bridge_test.py"
    s = p.read_text()
    anchor = 'class AndroidWsiLoaderBridgeContract(unittest.TestCase):\n'
    addition = '''class AndroidWsiLoaderBridgeContract(unittest.TestCase):\n    def test_provenance_wrappers_are_opt_in(self):\n        source = (ROOT / "src/android_wsi_loader_bridge.cpp").read_text()\n        self.assertIn("LSFG_WSI_PROVENANCE", source)\n        self.assertIn("if (!provenanceEnabled()) return resolved;", source)\n\n'''
    if 'test_provenance_wrappers_are_opt_in' not in s:
        s = replace_once(s, anchor, addition, "wsi test class")
    p.write_text(s)

    p = ROOT / "tests/android_runtime_stability_test.py"
    s = p.read_text()
    insert = '''\n    def test_android_config_watcher_is_present_thread_syscall_free_after_arm(self):\n        hooks = (ROOT / "src/hooks.cpp").read_text()\n        start = hooks.index("bool changed(const std::string& configFile) noexcept")\n        end = hooks.index("private:", start)\n        changed_body = hooks[start:end]\n        self.assertIn("changed_.exchange(false", changed_body)\n        self.assertNotIn("::read(", changed_body)\n        self.assertNotIn("last_write_time", changed_body)\n        self.assertIn("void run() noexcept", hooks)\n        self.assertIn("::poll(", hooks)\n\n'''
    marker = '\nif __name__ == "__main__":\n'
    if 'test_android_config_watcher_is_present_thread_syscall_free_after_arm' not in s:
        s = replace_once(s, marker, insert + marker, "runtime test footer")
    p.write_text(s)

    p = ROOT / "tests/android_a6xx_transport_test.py"
    s = p.read_text()
    insert = '''\n    def test_external_semaphore_fd_diagnostics_cover_sync_fd_without_forcing_it(self):\n        hooks = (ROOT / "src/hooks.cpp").read_text()\n        device = (ROOT / "framegen/src/core/device.cpp").read_text()\n        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", hooks)\n        self.assertIn("game-external-semaphore-fd", hooks)\n        self.assertIn("VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT", device)\n        self.assertIn("framegen-external-semaphore-fd", device)\n        self.assertIn("probeOpaqueFdExternalSemaphore", hooks)\n        self.assertIn("probeOpaqueFdExternalSemaphore", device)\n\n'''
    if 'test_external_semaphore_fd_diagnostics_cover_sync_fd_without_forcing_it' not in s:
        s = replace_once(s, marker, insert + marker, "a6xx test footer")
    p.write_text(s)


def patch_source():
    # 1) Production WSI hook resolution must not add diagnostic wrappers unless explicitly requested.
    p = ROOT / "src/android_wsi_loader_bridge.cpp"
    s = p.read_text()
    if '#include <cstdlib>' not in s:
        s = replace_once(s, '#include <cstring>\n', '#include <cstring>\n#include <cstdlib>\n', 'cstdlib include')
    provenance = '''\nbool provenanceEnabled() {\n    static const bool enabled = [] {\n        const char* value = std::getenv("LSFG_WSI_PROVENANCE");\n        return value != nullptr && (\n            std::strcmp(value, "1") == 0 ||\n            std::strcmp(value, "true") == 0 ||\n            std::strcmp(value, "TRUE") == 0);\n    }();\n    return enabled;\n}\n\n'''
    if 'bool provenanceEnabled()' not in s:
        s = replace_once(s, 'std::unordered_set<std::string> acquisitionKeys;\n\n',
                         'std::unordered_set<std::string> acquisitionKeys;\n' + provenance,
                         'provenance function')
    gate_anchor = '    if (pName == nullptr || resolved == nullptr) return resolved;\n\n'
    gate = gate_anchor + '    if (!provenanceEnabled()) return resolved;\n\n'
    if 'if (!provenanceEnabled()) return resolved;' not in s:
        s = replace_once(s, gate_anchor, gate, 'provenance gate')
    p.write_text(s)

    # 2) Move inotify draining/fallback stat polling completely off the present thread.
    p = ROOT / "src/hooks.cpp"
    s = p.read_text()
    if '#include <atomic>' not in s:
        s = replace_once(s, '#include <algorithm>\n', '#include <algorithm>\n#include <atomic>\n', 'atomic include')
    if '#include <poll.h>' not in s:
        s = replace_once(s, '#include <sys/inotify.h>\n', '#include <sys/inotify.h>\n#include <poll.h>\n', 'poll include')

    new_watcher = r'''    class AndroidConfigWatcher {
    public:
        ~AndroidConfigWatcher() {
            stop();
        }

        bool changed(const std::string& configFile) noexcept {
            if (configFile.empty())
                return false;
            if (configFile != configFile_)
                arm(configFile);
            return changed_.exchange(false, std::memory_order_acq_rel);
        }

    private:
        void arm(const std::string& configFile) noexcept {
            stop();
            configFile_ = configFile;
            const std::filesystem::path path(configFile_);
            directory_ = path.parent_path().string();
            filename_ = path.filename().string();

            fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            if (fd_ >= 0) {
                wd_ = ::inotify_add_watch(fd_, directory_.c_str(),
                    IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_ATTRIB |
                    IN_MOVE_SELF | IN_DELETE_SELF);
            }

            std::error_code ec;
            fallbackTimestamp_ = std::filesystem::last_write_time(configFile_, ec);
            fallbackTimestampValid_ = !ec;
            stopRequested_.store(false, std::memory_order_release);
            worker_ = std::thread(&AndroidConfigWatcher::run, this);
        }

        void run() noexcept {
            bool useInotify = fd_ >= 0 && wd_ >= 0;
            auto nextFallbackPoll = RuntimeOutputStats::Clock::now() + std::chrono::seconds(1);
            alignas(struct inotify_event) char buffer[4096];

            while (!stopRequested_.load(std::memory_order_acquire)) {
                if (useInotify) {
                    struct pollfd pfd { fd_, POLLIN, 0 };
                    const int pollResult = ::poll(&pfd, 1, 250);
                    if (pollResult > 0 && (pfd.revents & POLLIN)) {
                        for (;;) {
                            const ssize_t length = ::read(fd_, buffer, sizeof(buffer));
                            if (length < 0) {
                                if (errno == EAGAIN || errno == EWOULDBLOCK)
                                    break;
                                useInotify = false;
                                break;
                            }
                            if (length == 0)
                                break;

                            size_t offset = 0;
                            while (offset < static_cast<size_t>(length)) {
                                const auto* event = reinterpret_cast<const struct inotify_event*>(buffer + offset);
                                const bool matchingName = event->len > 0 && filename_ == event->name;
                                if (matchingName && (event->mask & (
                                        IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE |
                                        IN_ATTRIB | IN_MOVE_SELF | IN_DELETE_SELF))) {
                                    changed_.store(true, std::memory_order_release);
                                }
                                if (event->mask & IN_IGNORED)
                                    useInotify = false;
                                offset += sizeof(struct inotify_event) + event->len;
                            }
                        }
                    } else if (pollResult < 0 && errno != EINTR) {
                        useInotify = false;
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                if (!useInotify && RuntimeOutputStats::Clock::now() >= nextFallbackPoll) {
                    nextFallbackPoll = RuntimeOutputStats::Clock::now() + std::chrono::seconds(1);
                    std::error_code ec;
                    const auto timestamp = std::filesystem::last_write_time(configFile_, ec);
                    const bool valid = !ec;
                    if (valid != fallbackTimestampValid_
                            || (valid && timestamp != fallbackTimestamp_)) {
                        changed_.store(true, std::memory_order_release);
                    }
                    fallbackTimestamp_ = timestamp;
                    fallbackTimestampValid_ = valid;
                }
            }
        }

        void stop() noexcept {
            stopRequested_.store(true, std::memory_order_release);
            if (worker_.joinable())
                worker_.join();
            if (fd_ >= 0 && wd_ >= 0)
                ::inotify_rm_watch(fd_, wd_);
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = -1;
            wd_ = -1;
            configFile_.clear();
            directory_.clear();
            filename_.clear();
            changed_.store(false, std::memory_order_release);
        }

        int fd_{-1};
        int wd_{-1};
        std::string configFile_;
        std::string directory_;
        std::string filename_;
        std::filesystem::file_time_type fallbackTimestamp_{};
        bool fallbackTimestampValid_{false};
        std::atomic<bool> changed_{false};
        std::atomic<bool> stopRequested_{false};
        std::thread worker_;
    };

'''
    pattern = re.compile(r'    class AndroidConfigWatcher \{.*?^    AndroidConfigWatcher androidConfigWatcher;\n', re.S | re.M)
    match = pattern.search(s)
    if not match:
        raise RuntimeError('missing AndroidConfigWatcher block')
    s = s[:match.start()] + new_watcher + '    AndroidConfigWatcher androidConfigWatcher;\n' + s[match.end():]

    # External semaphore property helper + exact game-device diagnostics.
    helper_anchor = '    bool supportsOpaqueFdExternalSemaphore(VkPhysicalDevice physicalDevice) {\n'
    if 'queryExternalSemaphoreProperties' not in s:
        helper = '''    VkExternalSemaphoreProperties queryExternalSemaphoreProperties(\n            VkPhysicalDevice physicalDevice, VkExternalSemaphoreHandleTypeFlagBits handleType) {\n        VkExternalSemaphoreProperties properties{\n            .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,\n        };\n        auto getExternalSemaphoreProperties =\n            reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(\n                Layer::ovkGetInstanceProcAddr(layerInstance,\n                    "vkGetPhysicalDeviceExternalSemaphoreProperties"));\n        if (getExternalSemaphoreProperties == nullptr) {\n            getExternalSemaphoreProperties =\n                reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(\n                    Layer::ovkGetInstanceProcAddr(layerInstance,\n                        "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR"));\n        }\n        if (getExternalSemaphoreProperties == nullptr)\n            return properties;\n        const VkPhysicalDeviceExternalSemaphoreInfo info{\n            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,\n            .handleType = handleType,\n        };\n        getExternalSemaphoreProperties(physicalDevice, &info, &properties);\n        return properties;\n    }\n\n'''
        s = replace_once(s, helper_anchor, helper + helper_anchor, 'external semaphore helper')

    # Replace the duplicated property query in the OPAQUE support function with helper use.
    opaque_pattern = re.compile(r'''        auto getExternalSemaphoreProperties =.*?        getExternalSemaphoreProperties\(physicalDevice, &info, &properties\);\n''', re.S)
    opaque_replacement = '''        const auto properties = queryExternalSemaphoreProperties(\n            physicalDevice, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);\n'''
    m = opaque_pattern.search(s, s.index('bool supportsOpaqueFdExternalSemaphore'))
    if not m:
        raise RuntimeError('missing opaque semaphore property query')
    s = s[:m.start()] + opaque_replacement + s[m.end():]

    diag_anchor = '#else\n        const bool androidAhbSupported = true;\n        const bool androidExternalSemaphoreFdSupported = false;\n#endif\n'
    if 'game-external-semaphore-fd' not in s:
        diag = '''#ifdef __ANDROID__\n        const bool externalSemaphoreFdExtension = supportsDeviceExtension(physicalDevice,\n            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);\n        const auto opaqueFdProperties = queryExternalSemaphoreProperties(physicalDevice,\n            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);\n        const auto syncFdProperties = queryExternalSemaphoreProperties(physicalDevice,\n            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);\n        const bool getSemaphoreFdProc = Layer::ovkGetDeviceProcAddr(*pDevice, "vkGetSemaphoreFdKHR") != nullptr;\n        const bool importSemaphoreFdProc = Layer::ovkGetDeviceProcAddr(*pDevice, "vkImportSemaphoreFdKHR") != nullptr;\n        std::cerr << "lsfg-vk: game-external-semaphore-fd"\n                  << " extension=" << (externalSemaphoreFdExtension ? 1 : 0)\n                  << " opaqueFeatures=0x" << std::hex << opaqueFdProperties.externalSemaphoreFeatures\n                  << " opaqueCompatible=0x" << opaqueFdProperties.compatibleHandleTypes\n                  << " opaqueExportFromImported=0x" << opaqueFdProperties.exportFromImportedHandleTypes\n                  << " syncFeatures=0x" << syncFdProperties.externalSemaphoreFeatures\n                  << " syncCompatible=0x" << syncFdProperties.compatibleHandleTypes\n                  << " syncExportFromImported=0x" << syncFdProperties.exportFromImportedHandleTypes << std::dec\n                  << " getSemaphoreFdKHR=" << (getSemaphoreFdProc ? 1 : 0)\n                  << " importSemaphoreFdKHR=" << (importSemaphoreFdProc ? 1 : 0) << '\\n';\n#endif\n'''
        s = replace_once(s, diag_anchor, diag_anchor + diag, 'game semaphore diagnostics')
    p.write_text(s)

    # 3) Mirror exact OPAQUE_FD/SYNC_FD telemetry on the framegen logical-device side.
    p = ROOT / "framegen/src/core/device.cpp"
    s = p.read_text()
    frame_helper_anchor = 'bool probeOpaqueFdExternalSemaphore(VkPhysicalDevice physicalDevice) {\n'
    if 'queryExternalSemaphoreProperties(VkPhysicalDevice physicalDevice' not in s:
        helper = '''VkExternalSemaphoreProperties queryExternalSemaphoreProperties(VkPhysicalDevice physicalDevice,\n        VkExternalSemaphoreHandleTypeFlagBits handleType) {\n    VkExternalSemaphoreProperties properties{\n        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,\n    };\n#ifdef __ANDROID__\n    if (vkGetPhysicalDeviceExternalSemaphoreProperties == nullptr)\n        return properties;\n    const VkPhysicalDeviceExternalSemaphoreInfo info{\n        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,\n        .handleType = handleType,\n    };\n    vkGetPhysicalDeviceExternalSemaphoreProperties(physicalDevice, &info, &properties);\n#else\n    (void)physicalDevice;\n    (void)handleType;\n#endif\n    return properties;\n}\n\n'''
        s = replace_once(s, frame_helper_anchor, helper + frame_helper_anchor, 'framegen semaphore helper')

    old_probe = '''    if (vkGetPhysicalDeviceExternalSemaphoreProperties == nullptr)\n        return false;\n    const VkPhysicalDeviceExternalSemaphoreInfo info{\n        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,\n        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,\n    };\n    VkExternalSemaphoreProperties properties{\n        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,\n    };\n    vkGetPhysicalDeviceExternalSemaphoreProperties(physicalDevice, &info, &properties);\n'''
    if old_probe in s:
        s = replace_once(s, old_probe,
            '''    const auto properties = queryExternalSemaphoreProperties(physicalDevice,\n        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);\n''', 'framegen opaque probe')

    backend_anchor = '    std::cerr << "lsfg-vk: backend-init apiVersion="\n'
    if 'framegen-external-semaphore-fd' not in s:
        diag = '''    const bool externalSemaphoreFdExtension = hasExtension(availableExtensions,\n        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);\n    const auto opaqueFdProperties = queryExternalSemaphoreProperties(physicalDevice,\n        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);\n    const auto syncFdProperties = queryExternalSemaphoreProperties(physicalDevice,\n        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);\n    std::cerr << "lsfg-vk: framegen-external-semaphore-fd"\n              << " extension=" << (externalSemaphoreFdExtension ? 1 : 0)\n              << " opaqueFeatures=0x" << std::hex << opaqueFdProperties.externalSemaphoreFeatures\n              << " opaqueCompatible=0x" << opaqueFdProperties.compatibleHandleTypes\n              << " opaqueExportFromImported=0x" << opaqueFdProperties.exportFromImportedHandleTypes\n              << " syncFeatures=0x" << syncFdProperties.externalSemaphoreFeatures\n              << " syncCompatible=0x" << syncFdProperties.compatibleHandleTypes\n              << " syncExportFromImported=0x" << syncFdProperties.exportFromImportedHandleTypes << std::dec << '\\n';\n\n'''
        s = replace_once(s, backend_anchor, diag + backend_anchor, 'framegen semaphore diagnostics')

    volk_anchor = '    volkLoadDevice(handle);\n\n'
    if 'framegen-external-semaphore-fd-procs' not in s:
        procdiag = '''    std::cerr << "lsfg-vk: framegen-external-semaphore-fd-procs"\n              << " getSemaphoreFdKHR=" << (vkGetSemaphoreFdKHR != nullptr ? 1 : 0)\n              << " importSemaphoreFdKHR=" << (vkImportSemaphoreFdKHR != nullptr ? 1 : 0) << '\\n';\n\n'''
        s = replace_once(s, volk_anchor, volk_anchor + procdiag, 'framegen proc diagnostics')
    p.write_text(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tests-only', action='store_true')
    ap.add_argument('--source-only', action='store_true')
    args = ap.parse_args()
    if not args.source_only:
        patch_tests()
    if not args.tests_only:
        patch_source()


if __name__ == '__main__':
    main()
