#include <iostream>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace {

void logLifecycle(const char* message) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "LSFG", "%s", message);
#endif
    std::cerr << message << '\n';
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
#endif
void onLsfgLayerLoaded() {
    // This fires as soon as the Vulkan loader dlopens the layer, before any
    // Vulkan entry point is intercepted. It deliberately uses Android logcat
    // in addition to stderr because Wine/Bionic launch chains do not always
    // preserve native stderr in GameNative diagnostics.
    logLifecycle("LSFG: layer loaded [capability-detection]");
}

} // namespace
