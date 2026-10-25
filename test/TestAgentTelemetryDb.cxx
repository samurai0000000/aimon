/*
 * TestAgentTelemetryDb.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>
#include "AgentTelemetryDb.hxx"
#include "AgentHookHandler.hxx"
#include "PathUtils.hxx"

namespace fs = std::filesystem;
using namespace aimon;
using json = nlohmann::json;

static void testBasicOperations() {
    std::cout << "[TestAgentTelemetryDb] Running testBasicOperations..." << std::endl;
    std::string testDbPath = "/tmp/test_aimon_telemetry_basic.db";
    if (fs::exists(testDbPath)) {
        fs::remove(testDbPath);
    }

    auto &db = AgentTelemetryDb::getInstance();
    assert(db.open(testDbPath));
    assert(db.isOpen());

    int64_t now = static_cast<int64_t>(time(nullptr));

    // 1. Session start
    assert(db.recordSessionStart("sess-001", "conv-100", "antigravity", "/workspace/aimon", "Antigravity", now - 60));

    // 2. Insert lifecycle events
    AgentLifecycleEvent ev1;
    ev1.timestamp = now - 50;
    ev1.sessionId = "sess-001";
    ev1.agentType = "antigravity";
    ev1.eventType = "TOOL_PRE_USE";
    ev1.stepIndex = 1;
    ev1.toolName = "view_file";
    ev1.durationMs = 0.0;
    ev1.status = "OK";
    assert(db.insertEvent(ev1));

    AgentLifecycleEvent ev2;
    ev2.timestamp = now - 45;
    ev2.sessionId = "sess-001";
    ev2.agentType = "antigravity";
    ev2.eventType = "TOOL_POST_USE";
    ev2.stepIndex = 1;
    ev2.toolName = "view_file";
    ev2.durationMs = 25.0;
    ev2.status = "OK";
    assert(db.insertEvent(ev2));

    AgentLifecycleEvent ev3;
    ev3.timestamp = now - 30;
    ev3.sessionId = "sess-001";
    ev3.agentType = "antigravity";
    ev3.eventType = "TURN_END";
    ev3.stepIndex = 1;
    ev3.durationMs = 1200.0;
    ev3.status = "OK";
    assert(db.insertEvent(ev3));

    // 3. Batch insert samples
    std::vector<AgentTelemetrySample> samples;
    for (int i = 0; i < 10; ++i) {
        AgentTelemetrySample s;
        s.timestamp = now - (10 - i) * 5;
        s.activeAgents = 1;
        s.promptTokensSec = 15.0 + i;
        s.compTokensSec = 8.0 + i;
        s.toolCallsSec = 1.0;
        s.errorRatePct = 0.0;
        s.avgTurnLatencyMs = 1200.0;
        s.p95TurnLatencyMs = 1500.0;
        samples.push_back(s);
    }
    assert(db.insertSamplesBatch(samples));

    // 4. Session end
    assert(db.recordSessionEnd("sess-001", "COMPLETED", 1, 500, 250, 1, 0, 1200.0, now));

    // 5. Query overview
    auto overview = db.queryOverview(24);
    assert(overview.contains("total_sessions"));
    assert(overview["total_sessions"] == 1);
    assert(overview["total_turns"] == 1);
    assert(overview["total_prompt_tokens"] == 500);
    assert(overview["total_comp_tokens"] == 250);
    assert(overview["total_tool_calls"] == 1);

    // 6. Query sessions
    auto sessions = db.querySessions(10);
    assert(sessions.size() == 1);
    assert(sessions[0]["session_id"] == "sess-001");
    assert(sessions[0]["status"] == "COMPLETED");

    // 7. Query session events
    auto events = db.querySessionEvents("sess-001");
    assert(events.size() == 3);
    assert(events[0]["event_type"] == "TOOL_PRE_USE");
    assert(events[1]["event_type"] == "TOOL_POST_USE");
    assert(events[2]["event_type"] == "TURN_END");

    // 8. Query tool stats
    auto tools = db.queryToolStats(24);
    assert(tools.size() >= 1);
    assert(tools[0]["tool_name"] == "view_file");
    assert(tools[0]["invocations"] == 2);

    // 9. Query timeseries
    auto ts = db.queryTimeseries("1h", 100);
    assert(ts.contains("series"));
    assert(ts["series"]["timestamps"].is_array());
    assert(!ts["series"]["timestamps"].empty());

    db.close();
    fs::remove(testDbPath);
    std::cout << "[TestAgentTelemetryDb] testBasicOperations passed!" << std::endl;
}

static void testPercentileAndRollup() {
    std::cout << "[TestAgentTelemetryDb] Running testPercentileAndRollup..." << std::endl;
    std::string testDbPath = "/tmp/test_aimon_telemetry_rollup.db";
    if (fs::exists(testDbPath)) {
        fs::remove(testDbPath);
    }

    auto &db = AgentTelemetryDb::getInstance();
    assert(db.open(testDbPath));

    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t oldTs = now - (35 * 86400); // 35 days ago (should be pruned)

    // Insert old events
    std::vector<AgentLifecycleEvent> oldEvents;
    for (int i = 0; i < 20; ++i) {
        AgentLifecycleEvent ev;
        ev.timestamp = oldTs + i * 60;
        ev.sessionId = "old-sess-1";
        ev.agentType = "cursor";
        ev.eventType = (i % 2 == 0) ? "TOOL_POST_USE" : "TURN_END";
        ev.stepIndex = i;
        ev.durationMs = 100.0 + i * 50.0;
        ev.status = "OK";
        ev.toolName = "replace_file_content";
        oldEvents.push_back(ev);
    }
    assert(db.insertEventsBatch(oldEvents));

    // Test 95th percentile
    double p95 = db.query95thPercentileLatency(oldTs, oldTs + 3600);
    assert(p95 > 0.0);

    // Test rollup and prune (retention 30 days)
    size_t deleted = db.rollupAndPrune(30);
    assert(deleted == 20);

    db.close();
    fs::remove(testDbPath);
    std::cout << "[TestAgentTelemetryDb] testPercentileAndRollup passed!" << std::endl;
}

static void testClaudeSeriesIsSeparate() {
    std::cout << "[TestAgentTelemetryDb] Running testClaudeSeriesIsSeparate..." << std::endl;
    std::string testDbPath = "/tmp/test_aimon_telemetry_claude.db";
    if (fs::exists(testDbPath)) {
        fs::remove(testDbPath);
    }

    auto &db = AgentTelemetryDb::getInstance();
    assert(db.open(testDbPath));

    int64_t now = static_cast<int64_t>(time(nullptr));
    assert(db.recordSessionStart("claude-sess-1", "claude-sess-1", "claude", "/workspace/a", "claude-sonnet-5-5", now - 1000));
    assert(db.recordSessionStart("agy-sess-1", "agy-sess-1", "antigravity", "/workspace/b", "Antigravity", now - 1000));

    AgentLifecycleEvent claudeTool;
    claudeTool.timestamp = now - 900;
    claudeTool.sessionId = "claude-sess-1";
    claudeTool.agentType = "claude";
    claudeTool.eventType = "TOOL_CALL";
    claudeTool.stepIndex = 1;
    claudeTool.toolName = "Bash";
    claudeTool.durationMs = 12.0;
    claudeTool.status = "OK";
    assert(db.insertEvent(claudeTool));

    AgentLifecycleEvent agyTool = claudeTool;
    agyTool.sessionId = "agy-sess-1";
    agyTool.agentType = "antigravity";
    assert(db.insertEvent(agyTool));

    auto timeline = db.queryActivityTimeline("1h", 50);
    int claudeActive = 0;
    int agyActive = 0;
    int peak = 0;
    for (const auto &b : timeline["buckets"]) {
        claudeActive += b["claude_active"].get<int>();
        agyActive += b["antigravity_active"].get<int>();
        peak = std::max(peak, b["active_agents"].get<int>());
    }
    // The Claude session must be counted under its own series and not as Antigravity.
    assert(claudeActive >= 1);
    assert(agyActive >= 1);
    assert(peak >= 2);
    std::cout << "[TestAgentTelemetryDb] testClaudeSeriesIsSeparate passed (claude=" << claudeActive
              << " antigravity=" << agyActive << " peak=" << peak << ")" << std::endl;
    db.close();
    fs::remove(testDbPath);
}

static void DiagnosticLog_InsertQueryAndFilterByRequestIdAndCategory() {
    std::cout << "[TestAgentTelemetryDb] Running DiagnosticLog_InsertQueryAndFilterByRequestIdAndCategory..." << std::endl;
    std::string testDbPath = "/tmp/test_aimon_diag_filter.db";
    if (fs::exists(testDbPath)) {
        fs::remove(testDbPath);
    }

    auto &db = AgentTelemetryDb::getInstance();
    assert(db.open(testDbPath));

    // Verify default DiagnosticsConfig (enabled = false) writes 0 rows
    db.setDiagnosticsConfig(DiagnosticsConfig{});
    assert(db.logDiagnostic("INFO", "mobile_http", "cid-default-off", "cursor",
                            "Should be suppressed", {{"status", 200}}, "req-off"));
    assert(db.queryDiagnostics(100).empty());

    DiagnosticsConfig d;
    d.enabled = true;
    d.logMobileHttp = true;
    d.logRouterDecisions = true;
    d.logIdleTranscriptPolls = true;
    d.logDiscoveryPolls = true;
    d.logHooks = true;
    db.setDiagnosticsConfig(d);

    assert(db.logDiagnostic("INFO", "mobile_http", "cid-alpha-01", "cursor",
                            "GET /api/agent/conversations", {{"status", 200}}, "req-1001"));
    assert(db.logDiagnostic("INFO", "router_decision", "cid-alpha-01", "cursor",
                            "Selected cli_resume_with_context", {{"has_cli_store", false}}, "req-1001"));
    assert(db.logDiagnostic("WARN", "platform_forward", "cid-alpha-01", "cursor",
                            "Slow CLI response", {{"duration_ms", 4200}}, "req-1001"));
    assert(db.logDiagnostic("ERROR", "transcript", "cid-beta-02", "claude",
                            "Transcript file missing", {{"code", 404}}, "req-2002"));

    auto allLogs = db.queryDiagnostics(100);
    assert(allLogs.size() == 4);

    auto byReq = db.queryDiagnostics(100, "", "", "", "req-1001");
    assert(byReq.size() == 3);

    auto byCid = db.queryDiagnostics(100, "cid-beta-02");
    assert(byCid.size() == 1);
    assert(byCid[0]["category"] == "transcript");
    assert(byCid[0]["level"] == "ERROR");

    auto byCat = db.queryDiagnostics(100, "", "router_decision");
    assert(byCat.size() == 1);
    assert(byCat[0]["summary"] == "Selected cli_resume_with_context");

    auto warnAndAbove = db.queryDiagnostics(100, "", "", "WARN");
    assert(warnAndAbove.size() == 2);

    auto errorOnly = db.queryDiagnostics(100, "", "", "ERROR");
    assert(errorOnly.size() == 1);
    assert(errorOnly[0]["conversation_id"] == "cid-beta-02");

    auto trace = db.queryTrace("req-1001");
    assert(trace["target_id"] == "req-1001");
    assert(trace["trace_events"].is_array());
    assert(trace["trace_events"].size() == 3);
    assert(trace["trace_events"][0]["category"] == "mobile_http");
    assert(trace["trace_events"][1]["category"] == "router_decision");
    assert(trace["trace_events"][2]["category"] == "platform_forward");

    db.close();
    fs::remove(testDbPath);
    std::cout << "[TestAgentTelemetryDb] DiagnosticLog_InsertQueryAndFilterByRequestIdAndCategory passed!" << std::endl;
}

static void DiagnosticLog_EnforcesRingBufferCap() {
    std::cout << "[TestAgentTelemetryDb] Running DiagnosticLog_EnforcesRingBufferCap..." << std::endl;
    std::string testDbPath = "/tmp/test_aimon_diag_ringcap.db";
    if (fs::exists(testDbPath)) {
        fs::remove(testDbPath);
    }

    auto &db = AgentTelemetryDb::getInstance();
    assert(db.open(testDbPath));

    DiagnosticsConfig d;
    d.enabled = true;
    d.logMobileHttp = true;
    d.logRouterDecisions = true;
    d.logIdleTranscriptPolls = true;
    d.logDiscoveryPolls = true;
    d.logHooks = true;
    db.setDiagnosticsConfig(d);

    for (int i = 1; i <= 25; ++i) {
        assert(db.logDiagnostic("INFO", "hook", "cid-cap", "cursor",
                                "Entry " + std::to_string(i),
                                {{"seq", i}}, "req-cap", 10));
    }

    auto logs = db.queryDiagnostics(100, "cid-cap");
    assert(logs.size() == 10);
    // Ordered DESC: newest is 25, oldest retained is 16
    assert(logs.front()["details"]["seq"] == 25);
    assert(logs.back()["details"]["seq"] == 16);

    db.close();
    fs::remove(testDbPath);
    std::cout << "[TestAgentTelemetryDb] DiagnosticLog_EnforcesRingBufferCap passed!" << std::endl;
}

static void AgentHookHandler_CursorTurn1LifecycleAndPermissions() {
    std::cout << "[TestAgentTelemetryDb] Running AgentHookHandler_CursorTurn1LifecycleAndPermissions..." << std::endl;
    std::string testDir = "/tmp/test_aimon_hook_turn1_" + std::to_string(::getpid());
    std::string stateDir = testDir + "/state";
    std::string testDbPath = testDir + "/telemetry.db";
    fs::remove_all(testDir);

    const std::string cid = "0da2a96a-621c-4b59-8e9a-9ece09bfe723";

    {
        DiagnosticsConfig d;
        d.enabled = true;
        d.logMobileHttp = true;
        d.logRouterDecisions = true;
        d.logIdleTranscriptPolls = true;
        d.logDiscoveryPolls = true;
        d.logHooks = true;
        AgentTelemetryDb::getInstance().setDiagnosticsConfig(d);
    }

    // 1. sessionStart
    assert(AgentHookHandler::processPayload({
        {"hook_event_name", "sessionStart"},
        {"conversation_id", cid},
        {"workspace_roots", json::array({"/workspace/aimon"})},
        {"model", "claude-4.6-opus"}
    }, "cursor", "", stateDir, testDbPath));

    // 2. beforeSubmitPrompt
    assert(AgentHookHandler::processPayload({
        {"hook_event_name", "beforeSubmitPrompt"},
        {"conversation_id", cid},
        {"prompt", "Investigate Turn-1 session visibility with token=secret_tok_99"}
    }, "cursor", "", stateDir, testDbPath));

    // 3. afterAgentThought
    assert(AgentHookHandler::processPayload({
        {"hook_event_name", "afterAgentThought"},
        {"conversation_id", cid},
        {"text", "Checking hooks state and subagent transcripts."}
    }, "cursor", "", stateDir, testDbPath));

    // 4. postToolUse (with secret in tool_input and raw tool_output that must NEVER be persisted)
    assert(AgentHookHandler::processPayload({
        {"hook_event_name", "postToolUse"},
        {"conversation_id", cid},
        {"tool_use_id", "toolu_01"},
        {"tool_name", "run_command"},
        {"duration_ms", 42.5},
        {"tool_input", {
            {"command", "curl -H 'Authorization: Bearer eyJSecretToken123' http://127.0.0.1:3883"},
            {"content", "UNSAFE_BULK_FILE_CONTENT_MUST_NOT_BE_SAVED"}
        }},
        {"tool_output", "SENSITIVE_RAW_TOOL_OUTPUT_SECRET_ENV_DUMP"}
    }, "cursor", "", stateDir, testDbPath));

    // 5. subagentStart & subagentStop
    assert(AgentHookHandler::processPayload({
        {"hook_event_name", "subagentStart"},
        {"conversation_id", cid},
        {"subagent_id", "da6ba2af-ccaf-4012-98d7-77cc60335066"}
    }, "cursor", "", stateDir, testDbPath));
    assert(AgentHookHandler::processPayload({
        {"hook_event_name", "subagentStop"},
        {"conversation_id", cid},
        {"subagent_id", "da6ba2af-ccaf-4012-98d7-77cc60335066"}
    }, "cursor", "", stateDir, testDbPath));

    // 6. afterAgentResponse
    assert(AgentHookHandler::processPayload({
        {"hook_event_name", "afterAgentResponse"},
        {"conversation_id", cid},
        {"text", "Turn 1 live synthesis is active."}
    }, "cursor", "", stateDir, testDbPath));

    // 7. stop
    assert(AgentHookHandler::processPayload({
        {"hook_event_name", "stop"},
        {"conversation_id", cid},
        {"input_tokens", 1200},
        {"output_tokens", 350},
        {"duration_ms", 1800.0}
    }, "cursor", "", stateDir, testDbPath));

    // Verify POSIX permissions: 0700 on stateDir, 0600 on <cid>.json, <cid>.jsonl, and telemetry.db
    struct stat st{};
    assert(::stat(stateDir.c_str(), &st) == 0);
    assert((st.st_mode & 0777) == 0700);

    std::string stateJsonPath = stateDir + "/" + cid + ".json";
    std::string stateJsonlPath = stateDir + "/" + cid + ".jsonl";
    assert(fs::exists(stateJsonPath));
    assert(fs::exists(stateJsonlPath));

    assert(::stat(stateJsonPath.c_str(), &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    assert(::stat(stateJsonlPath.c_str(), &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    assert(::stat(testDbPath.c_str(), &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    // Verify <cid>.json state contents
    {
        std::ifstream ifs(stateJsonPath);
        json stateObj = json::parse(ifs);
        assert(stateObj["session_id"] == cid);
        assert(stateObj["conversation_id"] == cid);
        assert(stateObj["workspace"] == "/workspace/aimon");
        assert(stateObj["model"] == "claude-4.6-opus");
        assert(stateObj["status"] == "COMPLETED");
        assert(stateObj["tools"] == 1);
        assert(stateObj["turns"] == 1);
        assert(stateObj["first_prompt"].get<std::string>().find("secret_tok_99") == std::string::npos);
        assert(stateObj["first_prompt"].get<std::string>().find("[REDACTED]") != std::string::npos);
    }

    // Verify <cid>.jsonl lines and absence of tool_output or secrets
    {
        std::ifstream ifs(stateJsonlPath);
        std::vector<json> lines;
        std::string line;
        while (std::getline(ifs, line)) {
            if (!line.empty()) {
                assert(line.find("SENSITIVE_RAW_TOOL_OUTPUT_SECRET_ENV_DUMP") == std::string::npos);
                assert(line.find("UNSAFE_BULK_FILE_CONTENT_MUST_NOT_BE_SAVED") == std::string::npos);
                assert(line.find("eyJSecretToken123") == std::string::npos);
                assert(line.find("secret_tok_99") == std::string::npos);
                lines.push_back(json::parse(line));
            }
        }
        assert(lines.size() == 5);
        assert(lines[0]["role"] == "user");
        assert(lines[1]["role"] == "assistant");
        assert(lines[1]["message"]["content"][0]["type"] == "thinking");
        assert(lines[2]["role"] == "assistant");
        assert(lines[2]["message"]["content"][0]["type"] == "tool_use");
        assert(lines[2]["message"]["content"][0]["name"] == "run_command");
        assert(lines[3]["role"] == "assistant");
        assert(lines[3]["message"]["content"][0]["type"] == "text");
        assert(lines[4]["type"] == "turn_ended");
    }

    // Verify DB records
    auto &db = AgentTelemetryDb::getInstance();
    auto sessRec = db.querySessionRecord(cid);
    assert(sessRec["session_id"] == cid);
    assert(sessRec["status"] == "COMPLETED");
    assert(sessRec["total_tool_calls"] == 1);
    assert(sessRec["total_turns"] == 1);

    auto diagLogs = db.queryDiagnostics(50, cid);
    assert(diagLogs.size() >= 5);

    db.close();
    fs::remove_all(testDir);
    std::cout << "[TestAgentTelemetryDb] AgentHookHandler_CursorTurn1LifecycleAndPermissions passed!" << std::endl;
}

static void AgentHookHandler_RejectsPathTraversalAndMalformedJson() {
    std::cout << "[TestAgentTelemetryDb] Running AgentHookHandler_RejectsPathTraversalAndMalformedJson..." << std::endl;
    std::string testDir = "/tmp/test_aimon_hook_security_" + std::to_string(::getpid());
    std::string stateDir = testDir + "/state";
    std::string testDbPath = testDir + "/telemetry.db";
    fs::remove_all(testDir);

    assert(!AgentTelemetryDb::isValidSessionId(""));
    assert(!AgentTelemetryDb::isValidSessionId("../../../etc/passwd"));
    assert(!AgentTelemetryDb::isValidSessionId(".."));
    assert(!AgentTelemetryDb::isValidSessionId("abc/def"));
    assert(!AgentTelemetryDb::isValidSessionId("abc\\def"));
    assert(AgentTelemetryDb::isValidSessionId("0da2a96a-621c-4b59-8e9a-9ece09bfe723"));

    {
        DiagnosticsConfig d;
        d.enabled = true;
        d.logMobileHttp = true;
        d.logRouterDecisions = true;
        d.logIdleTranscriptPolls = true;
        d.logDiscoveryPolls = true;
        d.logHooks = true;
        AgentTelemetryDb::getInstance().setDiagnosticsConfig(d);
    }

    // 1. Path traversal attempt must return false and create zero state files
    bool ok = AgentHookHandler::processPayload({
        {"hook_event_name", "sessionStart"},
        {"conversation_id", "../../../etc/passwd"}
    }, "cursor", "", stateDir, testDbPath);
    assert(!ok);
    assert(!fs::exists(stateDir));

    auto &db = AgentTelemetryDb::getInstance();
    auto logs = db.queryDiagnostics(10, "", "hook", "WARN");
    assert(!logs.empty());
    assert(logs[0]["summary"].get<std::string>().find("Rejected invalid session_id") != std::string::npos);

    // 2. Malformed JSON on stdin via AgentHookHandler::run must never throw, outputs "{}\n", and returns 0
    std::istringstream badIn("{not valid json at all!!!");
    std::ostringstream out;
    char arg0[] = "hook";
    char arg1[] = "cursor";
    char* argv[] = {arg0, arg1};
    int rc = AgentHookHandler::run(2, argv, badIn, out, stateDir, testDbPath);
    assert(rc == 0);
    assert(out.str() == "{}\n");

    db.close();
    fs::remove_all(testDir);
    std::cout << "[TestAgentTelemetryDb] AgentHookHandler_RejectsPathTraversalAndMalformedJson passed!" << std::endl;
}

int main() {
    std::cout << "=== Starting TestAgentTelemetryDb ===" << std::endl;
    testBasicOperations();
    testPercentileAndRollup();
    testClaudeSeriesIsSeparate();
    DiagnosticLog_InsertQueryAndFilterByRequestIdAndCategory();
    DiagnosticLog_EnforcesRingBufferCap();
    AgentHookHandler_CursorTurn1LifecycleAndPermissions();
    AgentHookHandler_RejectsPathTraversalAndMalformedJson();
    std::cout << "=== TestAgentTelemetryDb: ALL TESTS PASSED ===" << std::endl;
    return 0;
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
