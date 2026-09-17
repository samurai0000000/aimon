/*
 * TranscriptSink.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "TranscriptSink.hxx"
#include "PathUtils.hxx"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>
#include <algorithm>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace aimon {

nlohmann::json RunMetadata::toJson() const {
    return {
        {"run_id", runId},
        {"plan_file", planFile},
        {"workspace", workspace},
        {"target_alias", targetAlias},
        {"initiator_id", initiatorId},
        {"executor_id", executorId},
        {"policy_id", policyId},
        {"start_epoch", startEpoch},
        {"end_epoch", endEpoch},
        {"terminal_status", terminalStatus}
    };
}

RunMetadata RunMetadata::fromJson(const nlohmann::json& j) {
    RunMetadata m;
    m.runId = j.value("run_id", "");
    m.planFile = j.value("plan_file", "");
    m.workspace = j.value("workspace", "");
    m.targetAlias = j.value("target_alias", "");
    m.initiatorId = j.value("initiator_id", "");
    m.executorId = j.value("executor_id", "");
    m.policyId = j.value("policy_id", "");
    m.startEpoch = j.value("start_epoch", (int64_t)0);
    m.endEpoch = j.value("end_epoch", (int64_t)0);
    m.terminalStatus = j.value("terminal_status", "running");
    return m;
}

nlohmann::json TranscriptEvent::toJson() const {
    return {
        {"ts_epoch", tsEpoch},
        {"run_id", runId},
        {"seq", seq},
        {"actor", actor},
        {"kind", kind},
        {"checkpoint_id", checkpointId},
        {"commands", commands.is_null() ? nlohmann::json::array() : commands},
        {"exit_codes", exitCodes.is_null() ? nlohmann::json::array() : exitCodes},
        {"artifacts", artifacts.is_null() ? nlohmann::json::object() : artifacts},
        {"dmesg_excerpt", dmesgExcerpt},
        {"decision", decision},
        {"proposal_next", proposalNext},
        {"notes", notes}
    };
}

TranscriptEvent TranscriptEvent::fromJson(const nlohmann::json& j) {
    TranscriptEvent e;
    e.tsEpoch = j.value("ts_epoch", (int64_t)0);
    e.runId = j.value("run_id", "");
    e.seq = j.value("seq", 0);
    e.actor = j.value("actor", "");
    e.kind = j.value("kind", "");
    e.checkpointId = j.value("checkpoint_id", "");
    e.commands = j.value("commands", nlohmann::json::array());
    e.exitCodes = j.value("exit_codes", nlohmann::json::array());
    e.artifacts = j.value("artifacts", nlohmann::json::object());
    e.dmesgExcerpt = j.value("dmesg_excerpt", "");
    e.decision = j.value("decision", "");
    e.proposalNext = j.value("proposal_next", "");
    e.notes = j.value("notes", "");
    return e;
}

TranscriptSink& TranscriptSink::getInstance() {
    static TranscriptSink instance;
    return instance;
}

TranscriptSink::TranscriptSink() {
    _baseDir = PathUtils::getAimonConfigDir() + "/runs";
}

void TranscriptSink::setBaseDir(const std::string& baseDir) {
    std::lock_guard<std::mutex> lock(_mutex);
    _baseDir = baseDir;
}

std::string TranscriptSink::getBaseDir() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _baseDir;
}

std::string TranscriptSink::getRunDirectory(const std::string& runId) const {
    return _baseDir + "/" + runId;
}

std::string TranscriptSink::generateRunId() const {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&tt, &tm);

    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    static thread_local std::uniform_int_distribution<uint16_t> dis;
    uint16_t suffix = dis(gen);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d-%H%M%S") << "-"
        << std::hex << std::setfill('0') << std::setw(4) << suffix;
    return oss.str();
}

std::string TranscriptSink::getActiveRunId() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _activeRunId;
}

bool TranscriptSink::startRun(const RunMetadata& metadata, std::string& outRunId, std::string& outError) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_activeRunId.empty()) {
        outError = "Another run is currently active: " + _activeRunId;
        return false;
    }

    RunMetadata meta = metadata;
    if (meta.runId.empty()) {
        meta.runId = generateRunId();
    }
    if (meta.startEpoch == 0) {
        meta.startEpoch = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    meta.terminalStatus = "running";

    std::string runDir = _baseDir + "/" + meta.runId;
    if (!PathUtils::ensureDirectoryExists(runDir)) {
        outError = "Failed to create directory: " + runDir;
        return false;
    }

    // Write run.json
    std::string metaFile = runDir + "/run.json";
    std::ofstream mf(metaFile, std::ios::trunc);
    if (!mf.is_open()) {
        outError = "Failed to write metadata file: " + metaFile;
        return false;
    }
    mf << meta.toJson().dump(2) << std::endl;
    mf.close();

    // Write initial report.md
    writeInitialMarkdown(runDir, meta);

    _activeRunId = meta.runId;
    _nextSeq = 1;

    // Append START event
    TranscriptEvent startEvent;
    startEvent.tsEpoch = meta.startEpoch;
    startEvent.runId = meta.runId;
    startEvent.seq = _nextSeq++;
    startEvent.actor = "aimon";
    startEvent.kind = "start";
    startEvent.notes = "Execution run started for target: " + meta.targetAlias;

    std::string txFile = runDir + "/transcript.jsonl";
    std::ofstream tf(txFile, std::ios::app);
    if (tf.is_open()) {
        tf << startEvent.toJson().dump() << "\n";
        tf.flush();
    }
    appendToMarkdownMirror(runDir, startEvent);

    outRunId = meta.runId;
    return true;
}

int TranscriptSink::appendEvent(TranscriptEvent& event, std::string& outError) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (event.runId.empty()) {
        if (_activeRunId.empty()) {
            outError = "No active run and no run_id specified";
            return -1;
        }
        event.runId = _activeRunId;
    }

    if (event.tsEpoch == 0) {
        event.tsEpoch = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    event.seq = _nextSeq++;

    std::string runDir = _baseDir + "/" + event.runId;
    if (!fs::exists(runDir)) {
        outError = "Run directory not found: " + runDir;
        return -1;
    }

    std::string txFile = runDir + "/transcript.jsonl";
    std::ofstream tf(txFile, std::ios::app);
    if (!tf.is_open()) {
        outError = "Failed to open transcript file for append: " + txFile;
        return -1;
    }
    tf << event.toJson().dump() << "\n";
    tf.flush();
    tf.close();

    appendToMarkdownMirror(runDir, event);

    return event.seq;
}

bool TranscriptSink::endRun(const std::string& runId, const std::string& terminalStatus, std::string& outError) {
    std::lock_guard<std::mutex> lock(_mutex);

    std::string targetRunId = runId.empty() ? _activeRunId : runId;
    if (targetRunId.empty()) {
        outError = "No run_id specified and no active run";
        return false;
    }

    std::string runDir = _baseDir + "/" + targetRunId;
    std::string metaFile = runDir + "/run.json";
    if (!fs::exists(metaFile)) {
        outError = "Run metadata not found: " + metaFile;
        return false;
    }

    RunMetadata meta;
    try {
        std::ifstream mf(metaFile);
        nlohmann::json j;
        mf >> j;
        meta = RunMetadata::fromJson(j);
    } catch (const std::exception& e) {
        outError = std::string("Error reading metadata: ") + e.what();
        return false;
    }

    meta.endEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    meta.terminalStatus = terminalStatus.empty() ? "completed" : terminalStatus;

    std::ofstream mf(metaFile, std::ios::trunc);
    if (mf.is_open()) {
        mf << meta.toJson().dump(2) << std::endl;
        mf.close();
    }

    // Append END event
    TranscriptEvent endEvent;
    endEvent.tsEpoch = meta.endEpoch;
    endEvent.runId = targetRunId;
    endEvent.seq = _nextSeq++;
    endEvent.actor = "aimon";
    endEvent.kind = "end";
    endEvent.decision = meta.terminalStatus;
    endEvent.notes = "Execution run ended with status: " + meta.terminalStatus;

    std::string txFile = runDir + "/transcript.jsonl";
    std::ofstream tf(txFile, std::ios::app);
    if (tf.is_open()) {
        tf << endEvent.toJson().dump() << "\n";
        tf.flush();
        tf.close();
    }
    appendToMarkdownMirror(runDir, endEvent);
    finalizeMarkdownMirror(runDir, meta);

    if (_activeRunId == targetRunId) {
        _activeRunId.clear();
    }

    return true;
}

bool TranscriptSink::getRunMetadata(const std::string& runId, RunMetadata& outMeta) const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string targetRunId = runId.empty() ? _activeRunId : runId;
    if (targetRunId.empty()) {
        return false;
    }

    std::string metaFile = _baseDir + "/" + targetRunId + "/run.json";
    if (!fs::exists(metaFile)) {
        return false;
    }

    try {
        std::ifstream mf(metaFile);
        nlohmann::json j;
        mf >> j;
        outMeta = RunMetadata::fromJson(j);
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<TranscriptEvent> TranscriptSink::getTranscript(const std::string& runId, int sinceSeq) const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<TranscriptEvent> events;

    std::string targetRunId = runId.empty() ? _activeRunId : runId;
    if (targetRunId.empty()) {
        return events;
    }

    std::string txFile = _baseDir + "/" + targetRunId + "/transcript.jsonl";
    if (!fs::exists(txFile)) {
        return events;
    }

    std::ifstream tf(txFile);
    std::string line;
    while (std::getline(tf, line)) {
        if (line.empty()) continue;
        try {
            nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
            if (!j.is_discarded()) {
                TranscriptEvent ev = TranscriptEvent::fromJson(j);
                if (ev.seq > sinceSeq) {
                    events.push_back(ev);
                }
            }
        } catch (...) {}
    }

    return events;
}

std::vector<RunMetadata> TranscriptSink::listRuns() const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<RunMetadata> runs;

    if (!fs::exists(_baseDir)) {
        return runs;
    }

    try {
        for (const auto& entry : fs::directory_iterator(_baseDir)) {
            if (entry.is_directory()) {
                std::string metaFile = entry.path().string() + "/run.json";
                if (fs::exists(metaFile)) {
                    std::ifstream mf(metaFile);
                    nlohmann::json j;
                    mf >> j;
                    runs.push_back(RunMetadata::fromJson(j));
                }
            }
        }
    } catch (...) {}

    std::sort(runs.begin(), runs.end(), [](const RunMetadata& a, const RunMetadata& b) {
        return a.startEpoch > b.startEpoch;
    });

    return runs;
}

void TranscriptSink::writeInitialMarkdown(const std::string& runDir, const RunMetadata& meta) {
    std::string mdFile = runDir + "/report.md";
    std::ofstream mf(mdFile, std::ios::trunc);
    if (!mf.is_open()) return;

    std::time_t tt = meta.startEpoch;
    std::tm tm;
    localtime_r(&tt, &tm);

    mf << "# Execution Run Report: " << meta.runId << "\n\n";
    mf << "- **Target Alias**: `" << meta.targetAlias << "`\n";
    if (!meta.planFile.empty()) {
        mf << "- **Plan File**: `" << meta.planFile << "`\n";
    }
    if (!meta.workspace.empty()) {
        mf << "- **Workspace**: `" << meta.workspace << "`\n";
    }
    if (!meta.initiatorId.empty()) {
        mf << "- **Initiator**: `" << meta.initiatorId << "`\n";
    }
    if (!meta.executorId.empty()) {
        mf << "- **Executor**: `" << meta.executorId << "`\n";
    }
    mf << "- **Started**: " << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "\n";
    mf << "- **Status**: `RUNNING`\n\n";
    mf << "---\n\n";
    mf << "## Event Timeline\n\n";
    mf.close();
}

void TranscriptSink::appendToMarkdownMirror(const std::string& runDir, const TranscriptEvent& event) {
    std::string mdFile = runDir + "/report.md";
    std::ofstream mf(mdFile, std::ios::app);
    if (!mf.is_open()) return;

    std::time_t tt = event.tsEpoch;
    std::tm tm;
    localtime_r(&tt, &tm);

    std::string upperKind = event.kind;
    std::transform(upperKind.begin(), upperKind.end(), upperKind.begin(), ::toupper);

    mf << "### [#" << event.seq << "] " << upperKind << " (" << event.actor << ") &mdash; "
       << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "\n\n";

    if (!event.checkpointId.empty()) {
        mf << "- **Checkpoint**: `" << event.checkpointId << "`\n";
    }
    if (!event.decision.empty()) {
        mf << "- **Decision**: `" << event.decision << "`\n";
    }
    if (!event.proposalNext.empty()) {
        mf << "- **Proposal Next**: " << event.proposalNext << "\n";
    }
    if (event.commands.is_array() && !event.commands.empty()) {
        mf << "- **Commands**:\n";
        for (size_t i = 0; i < event.commands.size(); ++i) {
            std::string cmd = event.commands[i].get<std::string>();
            int code = (event.exitCodes.is_array() && i < event.exitCodes.size()) ? event.exitCodes[i].get<int>() : 0;
            mf << "  - `" << cmd << "` (exit: " << code << ")\n";
        }
    }
    if (!event.dmesgExcerpt.empty()) {
        mf << "- **Kernel/Diagnostic Excerpt**:\n```text\n"
           << event.dmesgExcerpt << "\n```\n";
    }
    if (!event.notes.empty()) {
        mf << "- **Notes**: " << event.notes << "\n";
    }

    mf << "\n";
    mf.close();
}

void TranscriptSink::finalizeMarkdownMirror(const std::string& runDir, const RunMetadata& meta) {
    std::string mdFile = runDir + "/report.md";
    std::ofstream mf(mdFile, std::ios::app);
    if (!mf.is_open()) return;

    std::time_t tt = meta.endEpoch;
    std::tm tm;
    localtime_r(&tt, &tm);

    mf << "---\n\n";
    mf << "### Summary\n\n";
    mf << "- **Finished**: " << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "\n";
    mf << "- **Final Status**: `" << meta.terminalStatus << "`\n\n";
    mf.close();
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
