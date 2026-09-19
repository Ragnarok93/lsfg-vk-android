#include "android_diagnostics.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>

#ifdef __ANDROID__
#include <android/log.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

void logLifecycle(const std::string& message) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "LSFG", "%s", message.c_str());
#endif
    std::cerr << message << '\n';
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
        std::string("LSFG_DIAG process cmdline=") + processCmdline()
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
        std::string("LSFG_DIAG unload cmdline=") + processCmdline()
        + " pid=" + std::to_string(getpid())
        + " tid=" + std::to_string(static_cast<long>(syscall(SYS_gettid)))
        + " module=" + ownModule());
#else
    logLifecycle("LSFG_DIAG unload");
#endif
}

} // namespace

namespace AndroidDiagnostics {

void appendRuntimeLine(const std::string& configFile, const std::string& line) {
#ifdef __ANDROID__
    if (configFile.empty() || line.empty())
        return;

    constexpr std::uintmax_t kMaxRuntimeLogBytes = 8ULL * 1024ULL * 1024ULL;
    constexpr unsigned kFlushEveryLines = 4;

    static std::mutex mutex;
    static std::filesystem::path activePath;
    static std::ofstream stream;
    static std::uintmax_t bytesWritten = 0;
    static unsigned pendingLines = 0;

    std::lock_guard<std::mutex> lock(mutex);
    const std::filesystem::path path =
        std::filesystem::path(configFile).parent_path() / "diagnostics.log";

    auto openPath = [&](bool truncate) {
        if (stream.is_open())
            stream.close();

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        stream.clear();
        stream.open(
            path,
            std::ios::out | (truncate ? std::ios::trunc : std::ios::app));
        if (!stream) {
            activePath.clear();
            bytesWritten = 0;
            pendingLines = 0;
            return false;
        }

        activePath = path;
        if (truncate) {
            bytesWritten = 0;
        } else {
            ec.clear();
            bytesWritten = std::filesystem::file_size(path, ec);
            if (ec)
                bytesWritten = 0;
        }
        pendingLines = 0;
        return true;
    };

    if (!stream.is_open() || activePath != path) {
        std::error_code ec;
        const auto existingSize = std::filesystem::file_size(path, ec);
        const bool truncate =
            !ec && existingSize >= kMaxRuntimeLogBytes;
        if (!openPath(truncate))
            return;
    }

    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string record =
        "timestamp_ms=" + std::to_string(nowMs) + " " + line + "\n";

    if (bytesWritten + record.size() > kMaxRuntimeLogBytes) {
        if (!openPath(true))
            return;
    }

    stream << record;
    if (!stream) {
        stream.close();
        activePath.clear();
        bytesWritten = 0;
        pendingLines = 0;
        return;
    }

    bytesWritten += record.size();
    if (++pendingLines >= kFlushEveryLines) {
        stream.flush();
        pendingLines = 0;
    }
#else
    (void)configFile;
    (void)line;
#endif
}

} // namespace AndroidDiagnostics
