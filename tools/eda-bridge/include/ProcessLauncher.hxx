/*
 * ProcessLauncher.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_EDA_BRIDGE_PROCESS_LAUNCHER_HXX
#define AIMON_EDA_BRIDGE_PROCESS_LAUNCHER_HXX

#include <string>
#include <vector>

namespace aimon {

struct ProcessResult {
    bool launched = false;
    int exitCode = -1;
    bool timedOut = false;
    bool truncated = false;
    int durationMs = 0;
    std::string stdoutText;
    std::string stderrText;
    std::string errorMessage;
};

class ProcessLauncher {
public:
    static constexpr size_t kDefaultMaxOutputBytes = 4 * 1024 * 1024; // 4 MiB

    static ProcessResult run(const std::string& executable,
                             const std::vector<std::string>& args,
                             int timeoutMs = 60000,
                             size_t maxOutputBytes = kDefaultMaxOutputBytes,
                             const std::string& workingDir = "");
};

} // namespace aimon

#endif // AIMON_EDA_BRIDGE_PROCESS_LAUNCHER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
