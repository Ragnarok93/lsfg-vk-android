#include "android_diagnostics.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#ifdef __ANDROID__
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace LSFG::AndroidDiagnostics {
namespace {

#ifdef __ANDROID__
void appendExistingNativeArtifact(const std::string& message) noexcept {
    const char* configPath = std::getenv("LSFG_CONFIG");
    if (configPath == nullptr || *configPath == '\0') return;

    try {
        std::string path(configPath);
        const auto slash = path.find_last_of('/');
        if (slash == std::string::npos) return;
        path.resize(slash + 1);
        path += "diagnostics.log";

        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) return;

        std::string line = message;
        line.push_back('\n');
        const char* cursor = line.data();
        size_t remaining = line.size();
        while (remaining > 0) {
            const ssize_t written = write(fd, cursor, remaining);
            if (written <= 0) break;
            cursor += written;
            remaining -= static_cast<size_t>(written);
        }
        close(fd);
    } catch (...) {
        // Diagnostics must never affect LSFG execution or Vulkan fail-open.
    }
}
#endif

#ifdef __ANDROID__
std::string processCmdline() {
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    std::string value((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (char& c : value) {
        if (c == '\0') c = ' ';
    }
    while (!value.empty() && value.back() == ' ') value.pop_back();
    return value.empty() ? std::string("unknown") : value;
}

std::string ownModule() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&ownModule), &info) != 0 && info.dli_fname)
        return info.dli_fname;
    return "unknown";
}
#endif

} // namespace

void logNativeDiagnostic(const std::string& message) noexcept {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "LSFG", "%s", message.c_str());
    appendExistingNativeArtifact(message);
#endif
    std::cerr << message << '\n';
}

} // namespace LSFG::AndroidDiagnostics

namespace {

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
#endif
void onLsfgLayerLoaded() {
    LSFG::AndroidDiagnostics::logNativeDiagnostic("LSFG: layer loaded [capability-detection]");
#ifdef __ANDROID__
    LSFG::AndroidDiagnostics::logNativeDiagnostic(
        std::string("LSFG_DIAG process cmdline=") + LSFG::AndroidDiagnostics::processCmdline()
        + " pid=" + std::to_string(getpid())
        + " tid=" + std::to_string(static_cast<long>(syscall(SYS_gettid)))
        + " module=" + LSFG::AndroidDiagnostics::ownModule());
#endif
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor))
#endif
void onLsfgLayerUnloaded() {
#ifdef __ANDROID__
    LSFG::AndroidDiagnostics::logNativeDiagnostic(
        std::string("LSFG_DIAG unload cmdline=") + LSFG::AndroidDiagnostics::processCmdline()
        + " pid=" + std::to_string(getpid())
        + " tid=" + std::to_string(static_cast<long>(syscall(SYS_gettid)))
        + " module=" + LSFG::AndroidDiagnostics::ownModule());
#else
    LSFG::AndroidDiagnostics::logNativeDiagnostic("LSFG_DIAG unload");
#endif
}

} // namespace
