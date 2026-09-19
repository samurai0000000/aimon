/*
 * TranscriptSink.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_TRANSCRIPT_SINK_HXX
#define AIMON_TRANSCRIPT_SINK_HXX

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace aimon {

struct RunMetadata {
    std::string runId;
    std::string planFile;
    std::string workspace;
    std::string targetAlias;
    std::string initiatorId;
    std::string executorId;
    std::string policyId;
    int64_t startEpoch = 0;
    int64_t endEpoch = 0;
    std::string terminalStatus = "running";
    nlohmann::json initialQuota = nlohmann::json::object();

    nlohmann::json toJson() const;
    static RunMetadata fromJson(const nlohmann::json& j);
};

struct TranscriptEvent {
    int64_t tsEpoch = 0;
    std::string runId;
    int seq = 0;
    std::string actor;
    std::string kind;
    std::string checkpointId;
    nlohmann::json commands = nlohmann::json::array();
    nlohmann::json exitCodes = nlohmann::json::array();
    nlohmann::json artifacts = nlohmann::json::object();
    std::string dmesgExcerpt;
    std::string decision;
    std::string proposalNext;
    std::string notes;

    nlohmann::json toJson() const;
    static TranscriptEvent fromJson(const nlohmann::json& j);
};

class TranscriptSink {
public:
    static TranscriptSink& getInstance();

    TranscriptSink();
    ~TranscriptSink() = default;

    void setBaseDir(const std::string& baseDir);
    std::string getBaseDir() const;

    bool startRun(const RunMetadata& metadata, std::string& outRunId, std::string& outError);
    int appendEvent(TranscriptEvent& event, std::string& outError);
    bool endRun(const std::string& runId, const std::string& terminalStatus, std::string& outError);

    bool getRunMetadata(const std::string& runId, RunMetadata& outMeta) const;
    std::vector<TranscriptEvent> getTranscript(const std::string& runId, int sinceSeq = 0) const;
    std::vector<RunMetadata> listRuns() const;
    std::string getActiveRunId() const;

    std::string getRunDirectory(const std::string& runId) const;

private:
    std::string generateRunId() const;
    void appendToMarkdownMirror(const std::string& runDir, const TranscriptEvent& event);
    void writeInitialMarkdown(const std::string& runDir, const RunMetadata& meta);
    void finalizeMarkdownMirror(const std::string& runDir, const RunMetadata& meta);

    mutable std::mutex _mutex;
    std::string _baseDir;
    std::string _activeRunId;
    int _nextSeq = 1;
};

} // namespace aimon

#endif // AIMON_TRANSCRIPT_SINK_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
