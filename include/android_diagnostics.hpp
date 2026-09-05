#pragma once

#include <string>

namespace LSFG::AndroidDiagnostics {

/// Emit a native LSFG diagnostic line to Android logcat/stderr and, when the
/// GameNative LSFG_CONFIG path is available, append it to diagnostics.log next
/// to conf.toml so the existing in-app LSFG exporter captures child-process
/// events as part of its normal LSFG NATIVE EVENTS section.
void logNativeDiagnostic(const std::string& message) noexcept;

} // namespace LSFG::AndroidDiagnostics
