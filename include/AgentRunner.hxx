/*
 * AgentRunner.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_AGENT_RUNNER_HXX
#define AIMON_AGENT_RUNNER_HXX

#include <string>
#include <cstdint>
#include <cstddef>
#include "ConfigManager.hxx"

namespace aimon {

struct AgentRunSpec {
    std::string agentId;
    std::string planFile;
    int turnNumber = 0;
    std::string role;          // "Initiator" or "Reviewer"
    std::string prompt;
    std::string workspace;
    bool writeEnabled = false;
    int timeoutSec = 300;
    size_t maxOutputBytes = 512 * 1024;
};

struct AgentRunResult {
    bool ok = false;
    int exitCode = -1;
    bool timedOut = false;
    std::string stdoutText;
    std::string stderrText;
    int64_t durationMs = 0;
};

class AgentRunner {
public:
    AgentRunner(const CollaborationConfig& config);
    ~AgentRunner() = default;

    bool isAvailable(const std::string& agentId) const;
    bool checkQuotaOk(const std::string& agentId, std::string& outReason) const;
    AgentRunResult run(const AgentRunSpec& spec) const;

    // Per-runner timeout_seconds when configured, otherwise fallbackSec.
    int timeoutForAgent(const std::string& agentId, int fallbackSec) const;

    // True when a failed run means the CLI itself could not be used (missing
    // binary, expired credentials, quota refusal) rather than the agent doing
    // the work badly. Such failures must not be charged against the session.
    static bool isUnavailabilityFailure(const AgentRunResult& res, std::string& outReason);

    void updateConfig(const CollaborationConfig& config);

private:
    std::string resolveCommand(const AgentRunSpec& spec, bool& outIsScriptBridge) const;
    CollaborationConfig _config;
};

} // namespace aimon

#endif // AIMON_AGENT_RUNNER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
