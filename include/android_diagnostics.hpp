#pragma once

#include <string>

namespace AndroidDiagnostics {

#ifdef __ANDROID__
void appendRuntimeLine(const std::string& configFile, const std::string& line);
#else
inline void appendRuntimeLine(const std::string&, const std::string&) {}
#endif

} // namespace AndroidDiagnostics
