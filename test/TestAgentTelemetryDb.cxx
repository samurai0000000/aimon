/*
 * TestAgentTelemetryDb.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cassert>
#include <iostream>
#include <filesystem>
#include <thread>
#include <vector>
#include "AgentTelemetryDb.hxx"
#include "PathUtils.hxx"

namespace fs = std::filesystem;
using namespace aimon;

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
    assert(db.recordSessionStart("sess-001", "conv-100", "antigravity", "/home/samurai/work/aimon", "Antigravity", now - 60));

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
        s.approvalWaitMs = 0.0;
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

int main() {
    std::cout << "=== Starting TestAgentTelemetryDb ===" << std::endl;
    testBasicOperations();
    testPercentileAndRollup();
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
