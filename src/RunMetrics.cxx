/*
 * RunMetrics.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "RunMetrics.hxx"
#include <algorithm>
#include <chrono>
#include <set>

namespace aimon {

nlohmann::json QuotaDelta::toJson() const {
    return {
        {"cursor_fast_requests_delta", cursorFastRequestsDelta},
        {"cursor_spend_usd_delta", cursorSpendUsdDelta}
    };
}

QuotaDelta QuotaDelta::fromJson(const nlohmann::json& j) {
    QuotaDelta q;
    q.cursorFastRequestsDelta = j.value("cursor_fast_requests_delta", 0);
    q.cursorSpendUsdDelta = j.value("cursor_spend_usd_delta", 0.0);
    return q;
}

nlohmann::json PhaseSegment::toJson() const {
    return {
        {"seq", seq},
        {"checkpoint_id", checkpointId},
        {"phase", phase},
        {"actor", actor},
        {"start_epoch", startEpoch},
        {"end_epoch", endEpoch},
        {"duration_seconds", durationSeconds},
        {"summary", summary}
    };
}

PhaseSegment PhaseSegment::fromJson(const nlohmann::json& j) {
    PhaseSegment s;
    s.seq = j.value("seq", 0);
    s.checkpointId = j.value("checkpoint_id", "");
    s.phase = j.value("phase", "");
    s.actor = j.value("actor", "");
    s.startEpoch = j.value("start_epoch", (int64_t)0);
    s.endEpoch = j.value("end_epoch", (int64_t)0);
    s.durationSeconds = j.value("duration_seconds", 0);
    s.summary = j.value("summary", "");
    return s;
}

nlohmann::json RunMetrics::toJson() const {
    nlohmann::json wf = nlohmann::json::array();
    for (const auto& seg : waterfall) {
        wf.push_back(seg.toJson());
    }
    return {
        {"run_id", runId},
        {"total_duration_seconds", totalDurationSeconds},
        {"execution_duration_seconds", executionDurationSeconds},
        {"review_duration_seconds", reviewDurationSeconds},
        {"interlock_duration_seconds", interlockDurationSeconds},
        {"recovery_duration_seconds", recoveryDurationSeconds},
        {"human_gating_ratio", humanGatingRatio},
        {"autonomous_ratio", autonomousRatio},
        {"total_checkpoints", totalCheckpoints},
        {"recovery_count", recoveryCount},
        {"total_commands", totalCommands},
        {"successful_commands", successfulCommands},
        {"failed_commands", failedCommands},
        {"command_success_rate", commandSuccessRate},
        {"quota_delta", quotaDelta.toJson()},
        {"waterfall", wf}
    };
}

RunMetrics RunMetrics::fromJson(const nlohmann::json& j) {
    RunMetrics m;
    m.runId = j.value("run_id", "");
    m.totalDurationSeconds = j.value("total_duration_seconds", (int64_t)0);
    m.executionDurationSeconds = j.value("execution_duration_seconds", (int64_t)0);
    m.reviewDurationSeconds = j.value("review_duration_seconds", (int64_t)0);
    m.interlockDurationSeconds = j.value("interlock_duration_seconds", (int64_t)0);
    m.recoveryDurationSeconds = j.value("recovery_duration_seconds", (int64_t)0);
    m.humanGatingRatio = j.value("human_gating_ratio", 0.0);
    m.autonomousRatio = j.value("autonomous_ratio", 0.0);
    m.totalCheckpoints = j.value("total_checkpoints", 0);
    m.recoveryCount = j.value("recovery_count", 0);
    m.totalCommands = j.value("total_commands", 0);
    m.successfulCommands = j.value("successful_commands", 0);
    m.failedCommands = j.value("failed_commands", 0);
    m.commandSuccessRate = j.value("command_success_rate", 0.0);
    if (j.contains("quota_delta") && j["quota_delta"].is_object()) {
        m.quotaDelta = QuotaDelta::fromJson(j["quota_delta"]);
    }
    if (j.contains("waterfall") && j["waterfall"].is_array()) {
        for (const auto& item : j["waterfall"]) {
            m.waterfall.push_back(PhaseSegment::fromJson(item));
        }
    }
    return m;
}

RunMetrics RunMetricsAnalyzer::analyze(const RunMetadata& metadata,
                                      const std::vector<TranscriptEvent>& events,
                                      const AggregateStatus& currentStatus,
                                      int64_t nowEpoch) {
    if (nowEpoch <= 0) {
        nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    RunMetrics metrics;
    metrics.runId = metadata.runId;

    int64_t runStart = metadata.startEpoch > 0 ? metadata.startEpoch :
                      (!events.empty() ? events[0].tsEpoch : nowEpoch);
    int64_t runEnd = metadata.endEpoch > 0 ? metadata.endEpoch : nowEpoch;
    metrics.totalDurationSeconds = std::max<int64_t>(0, runEnd - runStart);

    // Compute QuotaDelta
    if (metadata.initialQuota.is_object() && !metadata.initialQuota.empty()) {
        int initCr = metadata.initialQuota.value("cursor_fast_requests", currentStatus.cursor.fastRequestsUsed);
        double initSpend = metadata.initialQuota.value("cursor_total_spend_usd", currentStatus.cursor.totalSpendUsd);

        metrics.quotaDelta.cursorFastRequestsDelta = std::max(0, currentStatus.cursor.fastRequestsUsed - initCr);
        metrics.quotaDelta.cursorSpendUsdDelta = std::max(0.0, currentStatus.cursor.totalSpendUsd - initSpend);
    }

    std::set<std::string> seenCheckpoints;
    bool inRecoveryMode = false;
    int64_t prevEpoch = runStart;

    for (size_t i = 0; i < events.size(); ++i) {
        const auto& ev = events[i];
        int64_t curEpoch = ev.tsEpoch > 0 ? ev.tsEpoch : prevEpoch;
        int duration = std::max<int>(0, static_cast<int>(curEpoch - prevEpoch));

        PhaseSegment seg;
        seg.seq = ev.seq;
        seg.checkpointId = ev.checkpointId;
        seg.startEpoch = prevEpoch;
        seg.endEpoch = curEpoch;
        seg.durationSeconds = duration;
        seg.actor = ev.actor;

        if (ev.kind == "start") {
            seg.phase = "setup";
            seg.summary = ev.notes.empty() ? "Run start" : ev.notes;
            prevEpoch = curEpoch;
            metrics.waterfall.push_back(seg);
            continue;
        }

        if (ev.kind == "result") {
            if (!ev.checkpointId.empty()) {
                seenCheckpoints.insert(ev.checkpointId);
            }
            if (inRecoveryMode) {
                seg.phase = "recovery";
                inRecoveryMode = false;
            } else {
                seg.phase = "execution";
            }
            if (ev.exitCodes.is_array() && !ev.exitCodes.empty()) {
                for (const auto& code : ev.exitCodes) {
                    metrics.totalCommands++;
                    if (code.is_number_integer() && code.get<int>() == 0) {
                        metrics.successfulCommands++;
                    } else {
                        metrics.failedCommands++;
                    }
                }
            } else if (ev.commands.is_array()) {
                for (size_t c = 0; c < ev.commands.size(); ++c) {
                    metrics.totalCommands++;
                    metrics.successfulCommands++;
                }
            }
            seg.summary = ev.notes.empty() ? ("Executed " + ev.checkpointId) : ev.notes;
        } else if (ev.kind == "review") {
            if (ev.decision == "recover") {
                seg.phase = "recovery";
                inRecoveryMode = true;
                metrics.recoveryCount++;
            } else {
                seg.phase = "review";
            }
            seg.summary = ev.decision + ": " + (ev.notes.empty() ? ev.proposalNext : ev.notes);
        } else if (ev.kind == "interlock") {
            seg.phase = "interlock";
            seg.summary = "Awaiting human approval: " + ev.proposalNext;
        } else if (ev.kind == "interlock_resolved") {
            seg.phase = "interlock";
            seg.summary = "Human operator " + ev.decision + " (" + ev.notes + ")";
        } else if (ev.kind == "end") {
            seg.phase = "setup";
            seg.summary = "Run finished (" + ev.notes + ")";
        } else {
            seg.phase = "execution";
            seg.summary = ev.notes;
        }

        metrics.waterfall.push_back(seg);
        prevEpoch = curEpoch;
    }

    // In-progress run: if run is still running, add active trailing segment
    if (metadata.endEpoch == 0 && prevEpoch < nowEpoch) {
        PhaseSegment activeSeg;
        activeSeg.seq = static_cast<int>(events.size()) + 1;
        activeSeg.startEpoch = prevEpoch;
        activeSeg.endEpoch = nowEpoch;
        activeSeg.durationSeconds = std::max<int>(0, static_cast<int>(nowEpoch - prevEpoch));

        if (!events.empty()) {
            const auto& lastEv = events.back();
            activeSeg.checkpointId = lastEv.checkpointId;
            if (lastEv.kind == "interlock" || (lastEv.kind == "review" && lastEv.decision == "wait_human")) {
                activeSeg.phase = "interlock";
                activeSeg.actor = "human";
                activeSeg.summary = "Awaiting human operator approval";
            } else if (lastEv.kind == "result") {
                activeSeg.phase = "review";
                activeSeg.actor = metadata.initiatorId.empty() ? "cursor" : metadata.initiatorId;
                activeSeg.summary = "Reviewer evaluating checkpoint " + lastEv.checkpointId;
            } else {
                activeSeg.phase = inRecoveryMode ? "recovery" : "execution";
                activeSeg.actor = metadata.executorId.empty() ? "gemini" : metadata.executorId;
                activeSeg.summary = "Executor working on next checkpoint";
            }
        } else {
            activeSeg.phase = "setup";
            activeSeg.actor = "aimon";
            activeSeg.summary = "Initializing execution run";
        }
        metrics.waterfall.push_back(activeSeg);
    }

    // Aggregate phase durations
    for (const auto& seg : metrics.waterfall) {
        if (seg.phase == "execution") {
            metrics.executionDurationSeconds += seg.durationSeconds;
        } else if (seg.phase == "review") {
            metrics.reviewDurationSeconds += seg.durationSeconds;
        } else if (seg.phase == "interlock") {
            metrics.interlockDurationSeconds += seg.durationSeconds;
        } else if (seg.phase == "recovery") {
            metrics.recoveryDurationSeconds += seg.durationSeconds;
        }
    }

    metrics.totalCheckpoints = static_cast<int>(seenCheckpoints.size());
    if (metrics.totalCommands > 0) {
        metrics.commandSuccessRate = static_cast<double>(metrics.successfulCommands) / metrics.totalCommands;
    } else {
        metrics.commandSuccessRate = 1.0;
    }

    if (metrics.totalDurationSeconds > 0) {
        metrics.humanGatingRatio = static_cast<double>(metrics.interlockDurationSeconds) / metrics.totalDurationSeconds;
        metrics.autonomousRatio = static_cast<double>(metrics.executionDurationSeconds + metrics.reviewDurationSeconds) / metrics.totalDurationSeconds;
    }

    return metrics;
}

} // namespace aimon

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
