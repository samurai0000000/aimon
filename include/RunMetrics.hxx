/*
 * RunMetrics.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_RUN_METRICS_HXX
#define AIMON_RUN_METRICS_HXX

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "TranscriptSink.hxx"
#include "Models.hxx"

namespace aimon {

struct QuotaDelta {
    int cursorFastRequestsDelta = 0;
    double cursorSpendUsdDelta = 0.0;

    nlohmann::json toJson() const;
    static QuotaDelta fromJson(const nlohmann::json& j);
};

struct PhaseSegment {
    int seq = 0;
    std::string checkpointId;
    std::string phase;        // "execution", "review", "interlock", "recovery", "setup"
    std::string actor;        // "gemini", "cursor", "human", "aimon"
    int64_t startEpoch = 0;
    int64_t endEpoch = 0;
    int durationSeconds = 0;
    std::string summary;

    nlohmann::json toJson() const;
    static PhaseSegment fromJson(const nlohmann::json& j);
};

struct RunMetrics {
    std::string runId;
    int64_t totalDurationSeconds = 0;
    int64_t executionDurationSeconds = 0;   // On-target commands + diagnostics
    int64_t reviewDurationSeconds = 0;      // Model review and reasoning
    int64_t interlockDurationSeconds = 0;   // Human gating wait time
    int64_t recoveryDurationSeconds = 0;    // Reboot / recovery time

    double humanGatingRatio = 0.0;          // interlockDuration / totalDuration
    double autonomousRatio = 0.0;           // (execution + review) / totalDuration

    int totalCheckpoints = 0;
    int recoveryCount = 0;
    int totalCommands = 0;
    int successfulCommands = 0;
    int failedCommands = 0;
    double commandSuccessRate = 0.0;

    QuotaDelta quotaDelta;
    std::vector<PhaseSegment> waterfall;

    nlohmann::json toJson() const;
    static RunMetrics fromJson(const nlohmann::json& j);
};

class RunMetricsAnalyzer {
public:
    static RunMetrics analyze(const RunMetadata& metadata,
                             const std::vector<TranscriptEvent>& events,
                             const AggregateStatus& currentStatus,
                             int64_t nowEpoch = 0);
};

} // namespace aimon

#endif // AIMON_RUN_METRICS_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
