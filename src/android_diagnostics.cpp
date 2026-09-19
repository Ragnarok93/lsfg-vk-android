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
