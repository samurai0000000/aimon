/*
 * InsightEngine.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_INSIGHT_ENGINE_HXX
#define AIMON_INSIGHT_ENGINE_HXX

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include "ConfigManager.hxx"
#include "Models.hxx"

namespace aimon {

struct PromptFeatures {
    int64_t chars = 0;
    int64_t words = 0;
    int64_t lines = 0;
    int64_t proseChars = 0;
    int64_t proseWords = 0;
    int64_t fencedChars = 0;
    int64_t fencedBlockCount = 0;
    int64_t pasteChars = 0;
    int mixedPaste = 0;
    int64_t filePathCount = 0;
    int64_t bulletCount = 0;
    int isSidechain = 0;
    int hasCodeBlock = 0;
    int hasFileRef = 0;
    int hasConstraints = 0;
    int isFirstInSession = 0;

    nlohmann::json toJson() const {
        return {
            {"chars", chars},
            {"words", words},
            {"lines", lines},
            {"prose_chars", proseChars},
            {"prose_words", proseWords},
            {"fenced_chars", fencedChars},
            {"fenced_block_count", fencedBlockCount},
            {"paste_chars", pasteChars},
            {"mixed_paste", mixedPaste},
            {"file_path_count", filePathCount},
            {"bullet_count", bulletCount},
            {"is_sidechain", isSidechain},
            {"has_code_block", hasCodeBlock},
            {"has_file_ref", hasFileRef},
            {"has_constraints", hasConstraints},
            {"is_first_in_session", isFirstInSession}
        };
    }
};

struct GenerationRecord {
    std::string conversationId;
    std::string sessionId;
    int64_t idx = 0;
    int64_t ts = 0;
    int64_t modelEnum = 0;
    int64_t inputTokens = 0;
    int64_t outputTokens = 0;
    int64_t thinkingTokens = 0;
    int64_t responseTokens = 0;
    int64_t cacheReadTokens = 0;
    int64_t f1F4F6 = 0;
    int64_t contextTokens = 0;
    int64_t contextLimit = 0;
    int64_t maxOutputTokens = 0;
    double ttftMs = 0.0;
    double durationMs = 0.0;
    int validated = 1;
};

struct InsightFinding {
    std::string detector;
    std::string platform;
    std::string scope;
    std::string scopeId;
    std::string severity;
    std::string confidence;
    int64_t n = 0;
    double metric = 0.0;
    std::string summary;
    nlohmann::json evidenceJson;
    std::string suggestedExperiment;
    int64_t experimentStart = 0;
    std::string experimentMetric;
    int64_t firstSeen = 0;
    std::string status = "active";

    nlohmann::json toJson() const {
        return {
            {"detector", detector},
            {"platform", platform},
            {"scope", scope},
            {"scope_id", scopeId},
            {"severity", severity},
            {"confidence", confidence},
            {"n", n},
            {"metric", metric},
            {"summary", summary},
            {"evidence", evidenceJson},
            {"suggested_experiment", suggestedExperiment},
            {"experiment_start", experimentStart},
            {"experiment_metric", experimentMetric},
            {"first_seen", firstSeen},
            {"status", status}
        };
    }
};

class InsightEngine {
public:
    InsightEngine();
    ~InsightEngine();

    bool open(const std::string& dbPath = "");
    void close();
    bool isOpen() const;

    void setConfig(const AimonConfig& cfg);
    const AimonConfig& getConfig() const { return _config; }

    bool ingestLiveStatus(const AggregateStatus& status);

    bool rebuild(const std::string& historyDbPath,
                 const std::string& telemetryDbPath,
                 const std::vector<std::string>& claudeConfigDirs,
                 const std::vector<std::string>& antigravityRoots,
                 const std::string& cursorHomeDir = "",
                 const std::string& cursorServerDir = "",
                 bool fullResetCursors = false);

    bool rebuildFromConfig(const std::string& historyDbPath,
                           const std::string& telemetryDbPath,
                           bool fullResetCursors = false);

    bool runDetectors(int days = 30);

    nlohmann::json getInsights(const std::string& platform = "",
                               int days = 30,
                               const std::string& detector = "") const;

    nlohmann::json getUsageReport(const std::string& platform = "",
                                  int days = 30,
                                  const std::string& groupBy = "project") const;

    nlohmann::json compareExperiment(const std::string& detector,
                                     const std::string& platform = "",
                                     int64_t splitEpoch = 0) const;

    nlohmann::json exportDataset(const std::string& outputDir,
                                 const std::string& format = "sqlite",
                                 int days = 30,
                                 bool anonymizeProjects = false) const;

    static nlohmann::json describeDataset();

    std::string generateWeeklyDigest(int days = 7) const;

    // Pure functions exposed for unit and RapidCheck property testing
    static PromptFeatures extractPromptFeatures(const std::string& text);
    static bool extractClaudeUserPromptFeatures(const nlohmann::json& lineObj,
                                                PromptFeatures& outFeatures,
                                                bool& outIsInterrupted);
    static bool decodeGenMetadata(const uint8_t* data, size_t len, GenerationRecord& out);
    static std::string mapAntigravityStepType(int stepType);
    static std::string hashFilePath(const std::string& path);
    static std::string decodeProjectSlug(const std::string& slug);
    static std::string basenameOnly(const std::string& path);

private:
    bool initSchema();
    void pruneOldRecords(int64_t cutoffEpoch);
    void insertFinding(const InsightFinding& f);

    mutable std::mutex _mutex;
    sqlite3* _db = nullptr;
    std::string _dbPath;
    AimonConfig _config;
};

} // namespace aimon

#endif // AIMON_INSIGHT_ENGINE_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
