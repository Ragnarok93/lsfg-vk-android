#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#ifdef __ANDROID__
#include <android/log.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

constexpr std::uintmax_t kMaxDiagnosticBytes = 4u * 1024u * 1024u;

std::filesystem::path diagnosticLogPath() {
#ifdef __ANDROID__
    const char* config = std::getenv("LSFG_CONFIG");
    if (config == nullptr || *config == '\0') return {};
    return std::filesystem::path(config).parent_path() / "diagnostics.log";
#else
    return {};
#endif
}

void appendRollingDiagnostic(const std::string& message) {
#ifdef __ANDROID__
    const auto path = diagnosticLogPath();
    if (path.empty()) return;

    try {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        ec.clear();
        const auto bytes = std::filesystem::exists(path, ec) && !ec
            ? std::filesystem::file_size(path, ec)
            : 0u;
        if (!ec && bytes >= kMaxDiagnosticBytes) {
            const auto rotated = std::filesystem::path(path.string() + ".1");
            std::filesystem::remove(rotated, ec);
            ec.clear();
            std::filesystem::rename(path, rotated, ec);
            if (ec) {
                ec.clear();
                std::ofstream truncate(path, std::ios::trunc);
            }
        }

        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::ofstream out(path, std::ios::app);
        if (out) out << "timestamp_ms=" << nowMs << ' ' << message << '\n';
    } catch (...) {
        // Diagnostics must always fail open and must never affect Vulkan-layer loading.
    }
#else
    (void)message;
#endif
}

void logLifecycle(const std::string& message) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "LSFG", "%s", message.c_str());
#endif
    std::cerr << message << '\n';
    appendRollingDiagnostic(message);
}

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

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
#endif
void onLsfgLayerLoaded() {
    logLifecycle("LSFG: layer loaded [capability-detection]");
#ifdef __ANDROID__
    logLifecycle(
        std::string("LSFG_DIAG event=layer-load cmdline=") + processCmdline()
        + " pid=" + std::to_string(getpid())
        + " tid=" + std::to_string(static_cast<long>(syscall(SYS_gettid)))
        + " module=" + ownModule());
#endif
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor))
#endif
void onLsfgLayerUnloaded() {
#ifdef __ANDROID__
    logLifecycle(
        std::string("LSFG_DIAG event=layer-unload cmdline=") + processCmdline()
        + " pid=" + std::to_string(getpid())
        + " tid=" + std::to_string(static_cast<long>(syscall(SYS_gettid)))
        + " module=" + ownModule());
#else
    logLifecycle("LSFG_DIAG event=layer-unload");
#endif
}

} // namespace
