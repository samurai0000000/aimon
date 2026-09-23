/*
 * AgentTelemetryDb.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "AgentTelemetryDb.hxx"
#include "PathUtils.hxx"
#include <iostream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <algorithm>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace aimon {

AgentTelemetryDb &AgentTelemetryDb::getInstance() {
    static AgentTelemetryDb instance;
    return instance;
}

AgentTelemetryDb::AgentTelemetryDb()
    : _dbPath("")
    , _db(nullptr)
    , _stmtInsertEvent(nullptr)
    , _stmtInsertSample(nullptr)
    , _stmtInsertSession(nullptr)
    , _stmtUpdateSession(nullptr) {
}

AgentTelemetryDb::~AgentTelemetryDb() {
    close();
}

bool AgentTelemetryDb::open(const std::string &dbPath) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_db != nullptr) {
        return true;
    }

    if (!dbPath.empty()) {
        _dbPath = PathUtils::expandHome(dbPath);
    } else {
        _dbPath = PathUtils::expandHome("~/.config/aimon/telemetry.db");
    }

    try {
        fs::path p(_dbPath);
        if (p.has_parent_path()) {
            PathUtils::ensureDirectoryExists(p.parent_path().string());
        }
    } catch (const std::exception &ex) {
        std::cerr << "AgentTelemetryDb: Failed to create directory: " << ex.what() << std::endl;
    }

    int rc = sqlite3_open(_dbPath.c_str(), &_db);
    if (rc != SQLITE_OK) {
        std::cerr << "AgentTelemetryDb: Failed to open SQLite database " << _dbPath
                  << ": " << sqlite3_errmsg(_db) << std::endl;
        if (_db) {
            sqlite3_close(_db);
            _db = nullptr;
        }
        return false;
    }

    char *err = nullptr;
    sqlite3_exec(_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    sqlite3_exec(_db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);

    if (!initSchema()) {
        std::cerr << "AgentTelemetryDb: Failed to initialize schemas" << std::endl;
        close();
        return false;
    }

    prepareStatements();
    return true;
}

void AgentTelemetryDb::close() {
    std::lock_guard<std::mutex> lock(_mutex);
    finalizeStatements();
    if (_db != nullptr) {
        sqlite3_close(_db);
        _db = nullptr;
    }
}

bool AgentTelemetryDb::isOpen() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _db != nullptr;
}

bool AgentTelemetryDb::initSchema() {
    const char *ddlSessions =
        "CREATE TABLE IF NOT EXISTS agent_sessions ("
        "  session_id          TEXT PRIMARY KEY,"
        "  conversation_id     TEXT NOT NULL,"
        "  agent_type          TEXT NOT NULL,"
        "  workspace_path      TEXT NOT NULL,"
        "  model_name          TEXT NOT NULL,"
        "  start_timestamp     INTEGER NOT NULL,"
        "  end_timestamp       INTEGER DEFAULT 0,"
        "  status              TEXT NOT NULL,"
        "  total_turns         INTEGER DEFAULT 0,"
        "  total_prompt_tokens INTEGER DEFAULT 0,"
        "  total_comp_tokens   INTEGER DEFAULT 0,"
        "  total_tool_calls    INTEGER DEFAULT 0,"
        "  total_errors        INTEGER DEFAULT 0,"
        "  avg_turn_ms         REAL DEFAULT 0.0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_sessions_ts ON agent_sessions(start_timestamp DESC);"
        "CREATE INDEX IF NOT EXISTS idx_sessions_conv ON agent_sessions(conversation_id);";

    const char *ddlEvents =
        "CREATE TABLE IF NOT EXISTS agent_lifecycle_events ("
        "  id                  INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp           INTEGER NOT NULL,"
        "  session_id          TEXT NOT NULL,"
        "  agent_type          TEXT NOT NULL,"
        "  event_type          TEXT NOT NULL,"
        "  step_index          INTEGER DEFAULT 0,"
        "  tool_name           TEXT DEFAULT '',"
        "  duration_ms         REAL DEFAULT 0.0,"
        "  status              TEXT DEFAULT 'OK',"
        "  details_json        TEXT DEFAULT '{}'"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_events_lookup ON agent_lifecycle_events(session_id, timestamp);"
        "CREATE INDEX IF NOT EXISTS idx_events_tool ON agent_lifecycle_events(tool_name, timestamp);"
        "CREATE INDEX IF NOT EXISTS idx_events_type ON agent_lifecycle_events(event_type, timestamp);";

    const char *ddlSamples =
        "CREATE TABLE IF NOT EXISTS agent_telemetry_samples ("
        "  timestamp           INTEGER NOT NULL,"
        "  active_agents       INTEGER NOT NULL,"
        "  prompt_tokens_sec   REAL NOT NULL,"
        "  comp_tokens_sec     REAL NOT NULL,"
        "  tool_calls_sec      REAL NOT NULL,"
        "  error_rate_pct      REAL NOT NULL,"
        "  avg_turn_latency_ms REAL NOT NULL,"
        "  p95_turn_latency_ms REAL NOT NULL,"
        "  approval_wait_ms    REAL NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_samples_ts ON agent_telemetry_samples(timestamp);";

    const char *ddlRollups =
        "CREATE TABLE IF NOT EXISTS agent_hourly_rollups ("
        "  hour_timestamp      INTEGER NOT NULL,"
        "  agent_type          TEXT NOT NULL,"
        "  model_name          TEXT NOT NULL,"
        "  total_sessions      INTEGER NOT NULL,"
        "  total_turns         INTEGER NOT NULL,"
        "  total_prompt_tokens INTEGER NOT NULL,"
        "  total_comp_tokens   INTEGER NOT NULL,"
        "  total_tool_calls    INTEGER NOT NULL,"
        "  total_errors        INTEGER NOT NULL,"
        "  avg_turn_ms         REAL NOT NULL,"
        "  p95_turn_ms         REAL NOT NULL,"
        "  max_turn_ms         REAL NOT NULL,"
        "  avg_approval_wait_ms REAL NOT NULL,"
        "  PRIMARY KEY (hour_timestamp, agent_type, model_name)"
        ");";

    char *err = nullptr;
    if (sqlite3_exec(_db, ddlSessions, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "AgentTelemetryDb: DDL error (sessions): " << (err ? err : "") << std::endl;
        if (err) sqlite3_free(err);
        return false;
    }

    if (sqlite3_exec(_db, ddlEvents, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "AgentTelemetryDb: DDL error (events): " << (err ? err : "") << std::endl;
        if (err) sqlite3_free(err);
        return false;
    }

    if (sqlite3_exec(_db, ddlSamples, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "AgentTelemetryDb: DDL error (samples): " << (err ? err : "") << std::endl;
        if (err) sqlite3_free(err);
        return false;
    }

    if (sqlite3_exec(_db, ddlRollups, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "AgentTelemetryDb: DDL error (rollups): " << (err ? err : "") << std::endl;
        if (err) sqlite3_free(err);
        return false;
    }

    return true;
}

void AgentTelemetryDb::prepareStatements() {
    const char *sqlEvent =
        "INSERT INTO agent_lifecycle_events ("
        "  timestamp, session_id, agent_type, event_type, step_index, tool_name, duration_ms, status, details_json"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_prepare_v2(_db, sqlEvent, -1, &_stmtInsertEvent, nullptr);

    const char *sqlSample =
        "INSERT INTO agent_telemetry_samples ("
        "  timestamp, active_agents, prompt_tokens_sec, comp_tokens_sec, tool_calls_sec,"
        "  error_rate_pct, avg_turn_latency_ms, p95_turn_latency_ms, approval_wait_ms"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_prepare_v2(_db, sqlSample, -1, &_stmtInsertSample, nullptr);

    const char *sqlInsertSession =
        "INSERT OR REPLACE INTO agent_sessions ("
        "  session_id, conversation_id, agent_type, workspace_path, model_name, start_timestamp, status"
        ") VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_prepare_v2(_db, sqlInsertSession, -1, &_stmtInsertSession, nullptr);

    const char *sqlUpdateSession =
        "UPDATE agent_sessions SET "
        "  end_timestamp = ?, status = ?, total_turns = ?, total_prompt_tokens = ?,"
        "  total_comp_tokens = ?, total_tool_calls = ?, total_errors = ?, avg_turn_ms = ? "
        "WHERE session_id = ?;";
    sqlite3_prepare_v2(_db, sqlUpdateSession, -1, &_stmtUpdateSession, nullptr);
}

void AgentTelemetryDb::finalizeStatements() {
    if (_stmtInsertEvent) {
        sqlite3_finalize(_stmtInsertEvent);
        _stmtInsertEvent = nullptr;
    }
    if (_stmtInsertSample) {
        sqlite3_finalize(_stmtInsertSample);
        _stmtInsertSample = nullptr;
    }
    if (_stmtInsertSession) {
        sqlite3_finalize(_stmtInsertSession);
        _stmtInsertSession = nullptr;
    }
    if (_stmtUpdateSession) {
        sqlite3_finalize(_stmtUpdateSession);
        _stmtUpdateSession = nullptr;
    }
}

bool AgentTelemetryDb::recordSessionStart(const std::string &sessionId,
                                         const std::string &convId,
                                         const std::string &agentType,
                                         const std::string &workspace,
                                         const std::string &model,
                                         int64_t timestamp) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db || !_stmtInsertSession) return false;

    int64_t ts = (timestamp > 0) ? timestamp : static_cast<int64_t>(time(nullptr));

    sqlite3_reset(_stmtInsertSession);
    sqlite3_bind_text(_stmtInsertSession, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(_stmtInsertSession, 2, convId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(_stmtInsertSession, 3, agentType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(_stmtInsertSession, 4, workspace.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(_stmtInsertSession, 5, model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(_stmtInsertSession, 6, ts);
    sqlite3_bind_text(_stmtInsertSession, 7, "RUNNING", -1, SQLITE_TRANSIENT);

    return sqlite3_step(_stmtInsertSession) == SQLITE_DONE;
}

bool AgentTelemetryDb::recordSessionEnd(const std::string &sessionId,
                                       const std::string &status,
                                       int totalTurns, int promptTokens, int compTokens,
                                       int toolCalls, int errors, double avgTurnMs,
                                       int64_t timestamp) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db || !_stmtUpdateSession) return false;

    int64_t ts = (timestamp > 0) ? timestamp : static_cast<int64_t>(time(nullptr));

    sqlite3_reset(_stmtUpdateSession);
    sqlite3_bind_int64(_stmtUpdateSession, 1, ts);
    sqlite3_bind_text(_stmtUpdateSession, 2, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(_stmtUpdateSession, 3, totalTurns);
    sqlite3_bind_int(_stmtUpdateSession, 4, promptTokens);
    sqlite3_bind_int(_stmtUpdateSession, 5, compTokens);
    sqlite3_bind_int(_stmtUpdateSession, 6, toolCalls);
    sqlite3_bind_int(_stmtUpdateSession, 7, errors);
    sqlite3_bind_double(_stmtUpdateSession, 8, avgTurnMs);
    sqlite3_bind_text(_stmtUpdateSession, 9, sessionId.c_str(), -1, SQLITE_TRANSIENT);

    return sqlite3_step(_stmtUpdateSession) == SQLITE_DONE;
}

bool AgentTelemetryDb::updateSessionStats(const std::string &sessionId,
                                        const std::string &status,
                                        int totalTurns, int promptTokens, int compTokens,
                                        int toolCalls, int errors, double avgTurnMs,
                                        int64_t startTimestamp,
                                        int64_t endTimestamp) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) return false;

    const char *sql =
        "UPDATE agent_sessions SET "
        "  status = ?, total_turns = ?, total_prompt_tokens = ?, total_comp_tokens = ?,"
        "  total_tool_calls = ?, total_errors = ?, avg_turn_ms = ?, "
        "  start_timestamp = CASE WHEN ? > 0 THEN ? ELSE start_timestamp END, "
        "  end_timestamp = CASE WHEN ? > 0 THEN ? ELSE end_timestamp END "
        "WHERE session_id = ?;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, totalTurns);
    sqlite3_bind_int(stmt, 3, promptTokens);
    sqlite3_bind_int(stmt, 4, compTokens);
    sqlite3_bind_int(stmt, 5, toolCalls);
    sqlite3_bind_int(stmt, 6, errors);
    sqlite3_bind_double(stmt, 7, avgTurnMs);
    sqlite3_bind_int64(stmt, 8, startTimestamp);
    sqlite3_bind_int64(stmt, 9, startTimestamp);
    sqlite3_bind_int64(stmt, 10, endTimestamp);
    sqlite3_bind_int64(stmt, 11, endTimestamp);
    sqlite3_bind_text(stmt, 12, sessionId.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool AgentTelemetryDb::insertEvent(const AgentLifecycleEvent &event) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db || !_stmtInsertEvent) return false;

    int64_t ts = (event.timestamp > 0) ? event.timestamp : static_cast<int64_t>(time(nullptr));
    std::string detailsStr = event.detailsJson.empty() ? "{}" : event.detailsJson.dump();

    sqlite3_reset(_stmtInsertEvent);
    sqlite3_bind_int64(_stmtInsertEvent, 1, ts);
    sqlite3_bind_text(_stmtInsertEvent, 2, event.sessionId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(_stmtInsertEvent, 3, event.agentType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(_stmtInsertEvent, 4, event.eventType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(_stmtInsertEvent, 5, event.stepIndex);
    sqlite3_bind_text(_stmtInsertEvent, 6, event.toolName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(_stmtInsertEvent, 7, event.durationMs);
    sqlite3_bind_text(_stmtInsertEvent, 8, event.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(_stmtInsertEvent, 9, detailsStr.c_str(), -1, SQLITE_TRANSIENT);

    return sqlite3_step(_stmtInsertEvent) == SQLITE_DONE;
}

bool AgentTelemetryDb::insertEventsBatch(const std::vector<AgentLifecycleEvent> &events) {
    if (events.empty()) return true;

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db || !_stmtInsertEvent) return false;

    sqlite3_exec(_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    for (const auto &event : events) {
        int64_t ts = (event.timestamp > 0) ? event.timestamp : static_cast<int64_t>(time(nullptr));
        std::string detailsStr = event.detailsJson.empty() ? "{}" : event.detailsJson.dump();

        sqlite3_reset(_stmtInsertEvent);
        sqlite3_bind_int64(_stmtInsertEvent, 1, ts);
        sqlite3_bind_text(_stmtInsertEvent, 2, event.sessionId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(_stmtInsertEvent, 3, event.agentType.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(_stmtInsertEvent, 4, event.eventType.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(_stmtInsertEvent, 5, event.stepIndex);
        sqlite3_bind_text(_stmtInsertEvent, 6, event.toolName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(_stmtInsertEvent, 7, event.durationMs);
        sqlite3_bind_text(_stmtInsertEvent, 8, event.status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(_stmtInsertEvent, 9, detailsStr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(_stmtInsertEvent);
    }
    sqlite3_exec(_db, "COMMIT;", nullptr, nullptr, nullptr);
    return true;
}

bool AgentTelemetryDb::insertSample(const AgentTelemetrySample &sample) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db || !_stmtInsertSample) return false;

    int64_t ts = (sample.timestamp > 0) ? sample.timestamp : static_cast<int64_t>(time(nullptr));

    sqlite3_reset(_stmtInsertSample);
    sqlite3_bind_int64(_stmtInsertSample, 1, ts);
    sqlite3_bind_int(_stmtInsertSample, 2, sample.activeAgents);
    sqlite3_bind_double(_stmtInsertSample, 3, sample.promptTokensSec);
    sqlite3_bind_double(_stmtInsertSample, 4, sample.compTokensSec);
    sqlite3_bind_double(_stmtInsertSample, 5, sample.toolCallsSec);
    sqlite3_bind_double(_stmtInsertSample, 6, sample.errorRatePct);
    sqlite3_bind_double(_stmtInsertSample, 7, sample.avgTurnLatencyMs);
    sqlite3_bind_double(_stmtInsertSample, 8, sample.p95TurnLatencyMs);
    sqlite3_bind_double(_stmtInsertSample, 9, sample.approvalWaitMs);

    return sqlite3_step(_stmtInsertSample) == SQLITE_DONE;
}

bool AgentTelemetryDb::insertSamplesBatch(const std::vector<AgentTelemetrySample> &samples) {
    if (samples.empty()) return true;

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db || !_stmtInsertSample) return false;

    sqlite3_exec(_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    for (const auto &sample : samples) {
        int64_t ts = (sample.timestamp > 0) ? sample.timestamp : static_cast<int64_t>(time(nullptr));

        sqlite3_reset(_stmtInsertSample);
        sqlite3_bind_int64(_stmtInsertSample, 1, ts);
        sqlite3_bind_int(_stmtInsertSample, 2, sample.activeAgents);
        sqlite3_bind_double(_stmtInsertSample, 3, sample.promptTokensSec);
        sqlite3_bind_double(_stmtInsertSample, 4, sample.compTokensSec);
        sqlite3_bind_double(_stmtInsertSample, 5, sample.toolCallsSec);
        sqlite3_bind_double(_stmtInsertSample, 6, sample.errorRatePct);
        sqlite3_bind_double(_stmtInsertSample, 7, sample.avgTurnLatencyMs);
        sqlite3_bind_double(_stmtInsertSample, 8, sample.p95TurnLatencyMs);
        sqlite3_bind_double(_stmtInsertSample, 9, sample.approvalWaitMs);
        sqlite3_step(_stmtInsertSample);
    }
    sqlite3_exec(_db, "COMMIT;", nullptr, nullptr, nullptr);
    return true;
}

json AgentTelemetryDb::queryOverview(int windowHours) {
    std::lock_guard<std::mutex> lock(_mutex);
    json overview = json::object();
    if (!_db) return overview;

    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t startTs = now - (windowHours * 3600);

    // Active & total sessions
    const char *sqlSessions =
        "SELECT "
        "  COUNT(CASE WHEN status = 'RUNNING' THEN 1 END) AS active_count,"
        "  COUNT(*) AS total_sessions,"
        "  SUM(total_turns) AS total_turns,"
        "  SUM(total_prompt_tokens) AS total_prompt,"
        "  SUM(total_comp_tokens) AS total_comp,"
        "  SUM(total_tool_calls) AS total_tools,"
        "  SUM(total_errors) AS total_errors,"
        "  AVG(CASE WHEN avg_turn_ms > 0 THEN avg_turn_ms ELSE NULL END) AS overall_avg_turn_ms "
        "FROM agent_sessions WHERE (end_timestamp >= ? OR (end_timestamp = 0 AND start_timestamp >= ?));";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sqlSessions, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, startTs);
        sqlite3_bind_int64(stmt, 2, startTs);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            overview["active_sessions"] = sqlite3_column_int(stmt, 0);
            overview["total_sessions"] = sqlite3_column_int(stmt, 1);
            overview["total_turns"] = sqlite3_column_int(stmt, 2);
            overview["total_prompt_tokens"] = sqlite3_column_int64(stmt, 3);
            overview["total_comp_tokens"] = sqlite3_column_int64(stmt, 4);
            overview["total_tool_calls"] = sqlite3_column_int(stmt, 5);
            overview["total_errors"] = sqlite3_column_int(stmt, 6);
            overview["avg_turn_latency_ms"] = sqlite3_column_double(stmt, 7);
        }
        sqlite3_finalize(stmt);
    }

    // Direct event counts in window
    const char *sqlEvents =
        "SELECT "
        "  SUM(CASE WHEN event_type IN ('USER_TURN', 'THINKING') THEN 1 ELSE 0 END),"
        "  SUM(CASE WHEN event_type = 'TOOL_CALL' THEN 1 ELSE 0 END),"
        "  SUM(CASE WHEN status = 'ERROR' THEN 1 ELSE 0 END),"
        "  AVG(CASE WHEN duration_ms > 0 THEN duration_ms ELSE NULL END) "
        "FROM agent_lifecycle_events WHERE timestamp >= ?;";

    if (sqlite3_prepare_v2(_db, sqlEvents, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, startTs);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int evTurns = sqlite3_column_int(stmt, 0);
            int evTools = sqlite3_column_int(stmt, 1);
            int evErrors = sqlite3_column_int(stmt, 2);
            double evLatency = sqlite3_column_double(stmt, 3);

            if (evTools > 0 || evTurns > 0) {
                overview["total_turns"] = evTurns;
                overview["total_tool_calls"] = evTools;
                overview["total_errors"] = evErrors;
                if (evLatency > 0.0) {
                    overview["avg_turn_latency_ms"] = evLatency;
                }
            }
        }
        sqlite3_finalize(stmt);
    }

    int totalTools = overview.value("total_tool_calls", 0);
    int totalErrors = overview.value("total_errors", 0);
    overview["error_rate_pct"] = (totalTools > 0) ? (static_cast<double>(totalErrors) * 100.0 / totalTools) : 0.0;
    overview["db_size_bytes"] = getDatabaseSizeBytes();

    return overview;
}

json AgentTelemetryDb::queryTimeseries(const std::string &window, int maxPoints) {
    std::lock_guard<std::mutex> lock(_mutex);
    json result = json::object();
    result["window"] = window;
    result["series"] = json::object();

    if (!_db) return result;

    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t durationSec = 86400; // default 24h

    if (window == "1h") durationSec = 3600;
    else if (window == "24h") durationSec = 86400;
    else if (window == "7d") durationSec = 7 * 86400;
    else if (window == "30d") durationSec = 30 * 86400;
    else if (window == "1y") durationSec = 365 * 86400;

    int64_t startEpoch = now - durationSec;
    result["start_time"] = startEpoch;
    result["end_time"] = now;

    if (maxPoints <= 0) maxPoints = 120;
    int64_t bucketWidth = std::max<int64_t>(10, durationSec / maxPoints);

    std::string sql =
        "SELECT "
        "  (timestamp / " + std::to_string(bucketWidth) + ") * " + std::to_string(bucketWidth) + " AS bucket_ts,"
        "  COUNT(DISTINCT session_id) AS active_agents,"
        "  SUM(CASE WHEN event_type = 'TOOL_CALL' THEN 1 ELSE 0 END) * 1.0 / " + std::to_string(bucketWidth) + " AS tool_calls_sec,"
        "  SUM(CASE WHEN status = 'ERROR' THEN 1 ELSE 0 END) * 100.0 / MAX(1, COUNT(*)) AS error_rate_pct,"
        "  AVG(CASE WHEN duration_ms > 0 THEN duration_ms ELSE NULL END) AS avg_turn_latency_ms,"
        "  MAX(duration_ms) AS p95_turn_latency_ms "
        "FROM agent_lifecycle_events "
        "WHERE timestamp >= ? AND timestamp <= ? "
        "GROUP BY bucket_ts ORDER BY bucket_ts ASC;";

    json timestamps = json::array();
    json activeAgents = json::array();
    json promptTokens = json::array();
    json compTokens = json::array();
    json toolCalls = json::array();
    json errorRate = json::array();
    json avgLatency = json::array();
    json p95Latency = json::array();
    json approvalWait = json::array();

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, startEpoch);
        sqlite3_bind_int64(stmt, 2, now);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            timestamps.push_back(sqlite3_column_int64(stmt, 0));
            activeAgents.push_back(sqlite3_column_int(stmt, 1));
            promptTokens.push_back(0.0);
            compTokens.push_back(0.0);
            toolCalls.push_back(sqlite3_column_double(stmt, 2));
            errorRate.push_back(sqlite3_column_double(stmt, 3));
            avgLatency.push_back(sqlite3_column_double(stmt, 4));
            p95Latency.push_back(sqlite3_column_double(stmt, 5));
            approvalWait.push_back(0.0);
        }
        sqlite3_finalize(stmt);
    }

    result["points_count"] = timestamps.size();
    result["series"]["timestamps"] = timestamps;
    result["series"]["active_agents"] = activeAgents;
    result["series"]["prompt_tokens_sec"] = promptTokens;
    result["series"]["comp_tokens_sec"] = compTokens;
    result["series"]["tool_calls_sec"] = toolCalls;
    result["series"]["error_rate_pct"] = errorRate;
    result["series"]["avg_turn_latency_ms"] = avgLatency;
    result["series"]["p95_turn_latency_ms"] = p95Latency;
    result["series"]["approval_wait_ms"] = approvalWait;

    return result;
}

json AgentTelemetryDb::querySessions(int limit, const std::string &status) {
    std::lock_guard<std::mutex> lock(_mutex);
    json sessions = json::array();
    if (!_db) return sessions;

    std::string sql =
        "SELECT session_id, conversation_id, agent_type, workspace_path, model_name,"
        "       start_timestamp, end_timestamp, status, total_turns, total_prompt_tokens,"
        "       total_comp_tokens, total_tool_calls, total_errors, avg_turn_ms "
        "FROM agent_sessions ";
    if (!status.empty()) {
        sql += "WHERE status = ? ";
    }
    sql += "ORDER BY start_timestamp DESC LIMIT ?;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        int bindIdx = 1;
        if (!status.empty()) {
            sqlite3_bind_text(stmt, bindIdx++, status.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int(stmt, bindIdx, limit > 0 ? limit : 50);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            json s = json::object();
            s["session_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            s["conversation_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            s["agent_type"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            s["workspace_path"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            s["model_name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            s["start_timestamp"] = sqlite3_column_int64(stmt, 5);
            s["end_timestamp"] = sqlite3_column_int64(stmt, 6);
            s["status"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
            s["total_turns"] = sqlite3_column_int(stmt, 8);
            s["total_prompt_tokens"] = sqlite3_column_int64(stmt, 9);
            s["total_comp_tokens"] = sqlite3_column_int64(stmt, 10);
            s["total_tool_calls"] = sqlite3_column_int(stmt, 11);
            s["total_errors"] = sqlite3_column_int(stmt, 12);
            s["avg_turn_ms"] = sqlite3_column_double(stmt, 13);
            sessions.push_back(s);
        }
        sqlite3_finalize(stmt);
    }

    return sessions;
}

json AgentTelemetryDb::querySessionEvents(const std::string &sessionId) {
    std::lock_guard<std::mutex> lock(_mutex);
    json events = json::array();
    if (!_db) return events;

    const char *sql =
        "SELECT id, timestamp, session_id, agent_type, event_type, step_index,"
        "       tool_name, duration_ms, status, details_json "
        "FROM agent_lifecycle_events "
        "WHERE session_id = ? ORDER BY timestamp ASC, id ASC;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sessionId.c_str(), -1, SQLITE_TRANSIENT);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            json e = json::object();
            e["id"] = sqlite3_column_int64(stmt, 0);
            e["timestamp"] = sqlite3_column_int64(stmt, 1);
            e["session_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            e["agent_type"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            e["event_type"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            e["step_index"] = sqlite3_column_int(stmt, 5);
            e["tool_name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            e["duration_ms"] = sqlite3_column_double(stmt, 7);
            e["status"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));

            const char *rawJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
            if (rawJson && rawJson[0] != '\0') {
                try {
                    e["details"] = json::parse(rawJson);
                } catch (...) {
                    e["details"] = json::object();
                }
            } else {
                e["details"] = json::object();
            }
            events.push_back(e);
        }
        sqlite3_finalize(stmt);
    }

    return events;
}

json AgentTelemetryDb::queryToolStats(int windowHours) {
    std::lock_guard<std::mutex> lock(_mutex);
    json tools = json::array();
    if (!_db) return tools;

    int64_t startTs = static_cast<int64_t>(time(nullptr)) - (windowHours * 3600);

    const char *sql =
        "SELECT tool_name, "
        "       COUNT(*) AS invocations,"
        "       SUM(CASE WHEN status = 'ERROR' THEN 1 ELSE 0 END) AS error_count,"
        "       AVG(duration_ms) AS avg_duration_ms,"
        "       MAX(duration_ms) AS max_duration_ms "
        "FROM agent_lifecycle_events "
        "WHERE timestamp >= ? AND tool_name != '' AND tool_name IS NOT NULL "
        "GROUP BY tool_name ORDER BY invocations DESC;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, startTs);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            json t = json::object();
            t["tool_name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            t["invocations"] = sqlite3_column_int(stmt, 1);
            t["error_count"] = sqlite3_column_int(stmt, 2);
            t["avg_duration_ms"] = sqlite3_column_double(stmt, 3);
            t["max_duration_ms"] = sqlite3_column_double(stmt, 4);

            int inv = t["invocations"].get<int>();
            int err = t["error_count"].get<int>();
            t["error_rate_pct"] = (inv > 0) ? (static_cast<double>(err) * 100.0 / inv) : 0.0;
            tools.push_back(t);
        }
        sqlite3_finalize(stmt);
    }

    return tools;
}

double AgentTelemetryDb::query95thPercentileLatency(int64_t startEpoch, int64_t endEpoch) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) return 0.0;

    const char *sql =
        "SELECT duration_ms FROM agent_lifecycle_events "
        "WHERE timestamp >= ? AND timestamp <= ? AND event_type = 'TURN_END' AND duration_ms > 0 "
        "ORDER BY duration_ms ASC;";

    std::vector<double> latencies;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, startEpoch);
        sqlite3_bind_int64(stmt, 2, endEpoch);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            latencies.push_back(sqlite3_column_double(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }

    if (latencies.empty()) return 0.0;
    size_t idx = static_cast<size_t>(std::floor(latencies.size() * 0.95));
    if (idx >= latencies.size()) idx = latencies.size() - 1;

    return latencies[idx];
}

size_t AgentTelemetryDb::rollupAndPrune(int rawRetentionDays) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) return 0;

    if (rawRetentionDays <= 0) rawRetentionDays = 30;
    int64_t cutoff = static_cast<int64_t>(time(nullptr)) - (rawRetentionDays * 86400);

    // 1. Rollup raw events into hourly rollups
    const char *sqlRollup =
        "INSERT OR REPLACE INTO agent_hourly_rollups "
        "SELECT (timestamp / 3600) * 3600 AS hour_ts, agent_type, "
        "       'default' AS model_name, "
        "       COUNT(DISTINCT session_id) AS total_sessions, "
        "       SUM(CASE WHEN event_type = 'TURN_END' THEN 1 ELSE 0 END) AS total_turns, "
        "       0 AS total_prompt_tokens, "
        "       0 AS total_comp_tokens, "
        "       SUM(CASE WHEN event_type = 'TOOL_POST_USE' THEN 1 ELSE 0 END) AS total_tool_calls, "
        "       SUM(CASE WHEN status = 'ERROR' THEN 1 ELSE 0 END) AS total_errors, "
        "       COALESCE(AVG(CASE WHEN event_type = 'TURN_END' THEN duration_ms ELSE NULL END), 0.0), "
        "       COALESCE(MAX(duration_ms), 0.0), "
        "       COALESCE(MAX(duration_ms), 0.0), "
        "       COALESCE(AVG(CASE WHEN event_type = 'APPROVAL_WAIT' THEN duration_ms ELSE NULL END), 0.0) "
        "FROM agent_lifecycle_events "
        "WHERE timestamp < ? "
        "GROUP BY hour_ts, agent_type;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sqlRollup, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, cutoff);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // 2. Prune raw rows past cutoff
    size_t deletedRows = 0;
    const char *sqlPruneEvents = "DELETE FROM agent_lifecycle_events WHERE timestamp < ?;";
    if (sqlite3_prepare_v2(_db, sqlPruneEvents, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, cutoff);
        sqlite3_step(stmt);
        deletedRows += sqlite3_changes(_db);
        sqlite3_finalize(stmt);
    }

    const char *sqlPruneSamples = "DELETE FROM agent_telemetry_samples WHERE timestamp < ?;";
    if (sqlite3_prepare_v2(_db, sqlPruneSamples, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, cutoff);
        sqlite3_step(stmt);
        deletedRows += sqlite3_changes(_db);
        sqlite3_finalize(stmt);
    }

    return deletedRows;
}

int64_t AgentTelemetryDb::getDatabaseSizeBytes() const {
    if (_dbPath.empty()) return 0;
    try {
        if (fs::exists(_dbPath)) {
            return static_cast<int64_t>(fs::file_size(_dbPath));
        }
    } catch (...) {}
    return 0;
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
