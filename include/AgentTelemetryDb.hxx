/*
 * AgentTelemetryDb.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_AGENTTELEMETRYDB_HXX
#define AIMON_AGENTTELEMETRYDB_HXX

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include "ConfigManager.hxx"

namespace aimon {

struct AgentLifecycleEvent {
    int64_t     timestamp = 0;
    std::string sessionId;
    std::string agentType;
    std::string eventType;
    int         stepIndex = 0;
    std::string toolName;
    double      durationMs = 0.0;
    std::string status = "OK";
    nlohmann::json detailsJson;
};

struct AgentTelemetrySample {
    int64_t timestamp = 0;
    int     activeAgents = 0;
    double  promptTokensSec = 0.0;
    double  compTokensSec = 0.0;
    double  toolCallsSec = 0.0;
    double  errorRatePct = 0.0;
    double  avgTurnLatencyMs = 0.0;
    double  p95TurnLatencyMs = 0.0;
};

struct AgentSessionRecord {
    std::string sessionId;
    std::string convId;
    std::string agentType;
    std::string workspace;
    std::string model;
    int64_t     startTimestamp = 0;
    int64_t     endTimestamp = 0;
    std::string status = "RUNNING";
    int         totalTurns = 0;
    int         promptTokens = 0;
    int         compTokens = 0;
    int         toolCalls = 0;
    int         errors = 0;
    double      avgTurnMs = 0.0;
};

struct GatewayDiagnosticLog {
    int64_t     id = 0;
    int64_t     timestampMs = 0;
    std::string requestId;
    std::string level;          // "INFO", "WARN", "ERROR"
    std::string category;       // "mobile_http", "router_decision", "platform_forward", "discovery", "transcript", "prompt", "hook"
    std::string conversationId;
    std::string provider;
    std::string summary;
    nlohmann::json detailsJson;
};

class AgentTelemetryDb {
public:
    static AgentTelemetryDb &getInstance();

    static std::string scrubSecrets(const std::string &input);
    static bool isValidSessionId(const std::string &id);

    bool open(const std::string &dbPath = "");
    void close();
    bool isOpen() const;

    // Diagnostics & Security-Redacted Trace Logging
    void setDiagnosticsConfig(const DiagnosticsConfig &cfg);
    DiagnosticsConfig getDiagnosticsConfig() const;

    bool logDiagnostic(const std::string &level,
                       const std::string &category,
                       const std::string &conversationId,
                       const std::string &provider,
                       const std::string &summary,
                       const nlohmann::json &details = nlohmann::json::object(),
                       const std::string &requestId = "",
                       int maxRetainedRows = -1);

    size_t clearDiagnostics(const std::string &conversationId = "",
                            const std::string &category = "",
                            int olderThanHours = 0);

    nlohmann::json getStorageStats() const;
    nlohmann::json purgeDebugData(bool vacuum = true);

    nlohmann::json queryDiagnostics(int limit = 100,
                                    const std::string &conversationId = "",
                                    const std::string &category = "",
                                    const std::string &minLevel = "",
                                    const std::string &requestId = "");

    nlohmann::json querySessionRecord(const std::string &sessionId);
    nlohmann::json queryTrace(const std::string &targetId, int limit = 500);

    // Session Management
    bool recordSessionStart(const std::string &sessionId,
                            const std::string &convId,
                            const std::string &agentType,
                            const std::string &workspace,
                            const std::string &model,
                            int64_t timestamp = 0);

    bool recordSessionEnd(const std::string &sessionId,
                          const std::string &status,
                          int totalTurns, int promptTokens, int compTokens,
                          int toolCalls, int errors, double avgTurnMs,
                          int64_t timestamp = 0);

    bool updateSessionStats(const std::string &sessionId,
                            const std::string &status,
                            int totalTurns, int promptTokens, int compTokens,
                            int toolCalls, int errors, double avgTurnMs,
                            int64_t startTimestamp = 0,
                            int64_t endTimestamp = 0);

    // Update status and end time without replacing accumulated counters.
    bool touchSession(const std::string &sessionId,
                      const std::string &status,
                      int64_t endTimestamp = 0);

    // Event & Sample Ingestion
    bool insertEvent(const AgentLifecycleEvent &event);
    bool insertEventsBatch(const std::vector<AgentLifecycleEvent> &events);
    bool insertSample(const AgentTelemetrySample &sample);
    bool insertSamplesBatch(const std::vector<AgentTelemetrySample> &samples);

    // High-Performance Queries
    nlohmann::json queryOverview(int windowHours = 24);
    nlohmann::json queryTimeseries(const std::string &window = "24h", int maxPoints = 300);
    nlohmann::json queryActivityTimeline(const std::string &window = "24h", int maxSessions = 50);
    nlohmann::json querySessions(int limit = 50, const std::string &status = "");
    nlohmann::json querySessionEvents(const std::string &sessionId);
    nlohmann::json queryToolStats(int windowHours = 24);
    double query95thPercentileLatency(int64_t startEpoch, int64_t endEpoch);

    // Maintenance, Rollup & Pruning
    size_t rollupAndPrune(int rawRetentionDays = 30);
    int64_t getDatabaseSizeBytes() const;

private:
    AgentTelemetryDb();
    ~AgentTelemetryDb();
    AgentTelemetryDb(const AgentTelemetryDb &) = delete;
    AgentTelemetryDb &operator=(const AgentTelemetryDb &) = delete;

    bool initSchema();
    void prepareStatements();
    void finalizeStatements();

    std::string   _dbPath;
    sqlite3      *_db;
    mutable std::mutex _mutex;
    DiagnosticsConfig _diagConfig;

    // Prepared statements
    sqlite3_stmt *_stmtInsertEvent;
    sqlite3_stmt *_stmtInsertSample;
    sqlite3_stmt *_stmtInsertSession;
    sqlite3_stmt *_stmtUpdateSession;
    sqlite3_stmt *_stmtUpsertSession;
};

} // namespace aimon

#endif /* AIMON_AGENTTELEMETRYDB_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
