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
    double  approvalWaitMs = 0.0;
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

class AgentTelemetryDb {
public:
    static AgentTelemetryDb &getInstance();

    bool open(const std::string &dbPath = "");
    void close();
    bool isOpen() const;

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
