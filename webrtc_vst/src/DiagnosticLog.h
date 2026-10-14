#pragma once
#include <cstdio>
#include <string>

namespace webrtc_vst {
// Optional worker-thread diagnostics must not mutate the host's shared cout
// formatting state. A single stdio write also keeps each plugin line together.
inline void writeDiagnosticLine(std::string line) {
    line.push_back('\n');
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}
} // namespace webrtc_vst
