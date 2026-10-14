/*
 * TestInsightEngine.cxx
 *
 * Qualification tests for InsightEngine:
 * - Golden vector parity for extractPromptFeatures against test/fixtures/prompt_features.json
 * - Claude human-prompt extraction rules (§2 item 11)
 * - Antigravity gen_metadata protobuf decoding and identity verification
 * - SQLite schema creation, POSIX 0600 permissions, incremental scan cursors
 * - Programmatic and workflow-coaching detectors across synthetic multi-platform fixtures
 * - Before/after experiment comparator (compareExperiment)
 * - Dataset export (SQLite, CSV, JSONL, anonymize_projects) and weekly Markdown digest
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include "InsightEngine.hxx"
#include "HistoryStore.hxx"
#include "AgentTelemetryDb.hxx"
#include "ClaudeUsageStore.hxx"

// CppUTest headers must come last.
#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/TestHarness.h>

using namespace aimon;
namespace fs = std::filesystem;

#ifndef AIMON_SOURCE_DIR
#error "AIMON_SOURCE_DIR must be defined by the build"
#endif

static std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static std::string makeTempDir() {
    char tmpl[] = "/tmp/aimon_insight_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    return dir ? std::string(dir) : std::string();
}

static void appendVarint(std::vector<uint8_t>& buf, uint64_t val) {
    while (val >= 0x80) {
        buf.push_back(static_cast<uint8_t>((val & 0x7F) | 0x80));
        val >>= 7;
    }
    buf.push_back(static_cast<uint8_t>(val & 0x7F));
}

static void appendTag(std::vector<uint8_t>& buf, uint32_t fieldNum, uint8_t wireType) {
    appendVarint(buf, (static_cast<uint64_t>(fieldNum) << 3) | (wireType & 0x07));
}

static void appendBytesField(std::vector<uint8_t>& buf, uint32_t fieldNum, const std::vector<uint8_t>& payload) {
    appendTag(buf, fieldNum, 2);
    appendVarint(buf, payload.size());
    buf.insert(buf.end(), payload.begin(), payload.end());
}

static std::vector<uint8_t> buildGenBlob(uint64_t modelEnum,
                                         uint64_t inTok,
                                         uint64_t outTok,
                                         uint64_t thinkTok,
                                         uint64_t respTok,
                                         uint64_t cacheTok,
                                         uint64_t tsSec,
                                         uint64_t ctxTok,
                                         uint64_t ctxLim) {
    std::vector<uint8_t> f1_f4;
    appendTag(f1_f4, 1, 0); appendVarint(f1_f4, modelEnum);
    appendTag(f1_f4, 2, 0); appendVarint(f1_f4, inTok);
    appendTag(f1_f4, 3, 0); appendVarint(f1_f4, outTok);
    appendTag(f1_f4, 5, 0); appendVarint(f1_f4, cacheTok);
    appendTag(f1_f4, 6, 0); appendVarint(f1_f4, 24);
    appendTag(f1_f4, 9, 0); appendVarint(f1_f4, thinkTok);
    appendTag(f1_f4, 10, 0); appendVarint(f1_f4, respTok);

    std::vector<uint8_t> f1_f9_f4;
    appendTag(f1_f9_f4, 1, 0); appendVarint(f1_f9_f4, tsSec);

    std::vector<uint8_t> f1_f9_f8;
    appendTag(f1_f9_f8, 1, 0); appendVarint(f1_f9_f8, 1);
    appendTag(f1_f9_f8, 2, 0); appendVarint(f1_f9_f8, 500000000ULL);

    std::vector<uint8_t> f1_f9_f10;
    appendTag(f1_f9_f10, 1, 0); appendVarint(f1_f9_f10, ctxTok);
    appendTag(f1_f9_f10, 4, 0); appendVarint(f1_f9_f10, ctxLim);

    std::vector<uint8_t> f1_f9;
    appendBytesField(f1_f9, 4, f1_f9_f4);
    appendBytesField(f1_f9, 8, f1_f9_f8);
    appendBytesField(f1_f9, 10, f1_f9_f10);

    std::vector<uint8_t> f1_f11;
    appendTag(f1_f11, 1, 0); appendVarint(f1_f11, 4);
    appendTag(f1_f11, 2, 0); appendVarint(f1_f11, 250000000ULL);

    std::vector<uint8_t> f1_f15;
    appendTag(f1_f15, 2, 0); appendVarint(f1_f15, 64000);

    std::vector<uint8_t> f1;
    appendTag(f1, 3, 0); appendVarint(f1, modelEnum);
    appendBytesField(f1, 4, f1_f4);
    appendBytesField(f1, 9, f1_f9);
    appendBytesField(f1, 11, f1_f11);
    appendBytesField(f1, 15, f1_f15);

    std::string uuidStr = "360883b4-dc2a-4ed4-99d4-c8fe55808c4d";
    std::vector<uint8_t> uuidBytes(uuidStr.begin(), uuidStr.end());

    std::vector<uint8_t> root;
    appendBytesField(root, 1, f1);
    appendBytesField(root, 4, uuidBytes);
    return root;
}

TEST_GROUP(InsightEngineGroup) {
    std::string dir;

    void setup() {
        dir = makeTempDir();
        CHECK_TRUE(!dir.empty());
    }

    void teardown() {
        std::string cmd = "rm -rf '" + dir + "'";
        int rc = std::system(cmd.c_str());
        (void)rc;
    }
};

TEST(InsightEngineGroup, ExtractPromptFeaturesMatchesGoldenVectors) {
    std::string fixturePath = std::string(AIMON_SOURCE_DIR) + "/test/fixtures/prompt_features.json";
    nlohmann::json cases = nlohmann::json::parse(readFile(fixturePath));
    CHECK_TRUE(cases.is_array());
    CHECK_TRUE(cases.size() >= 6);

    for (const auto& tc : cases) {
        std::string name = tc["name"].get<std::string>();
        std::string text = tc["input"].get<std::string>();
        const auto& exp = tc["expected"];

        PromptFeatures pf = InsightEngine::extractPromptFeatures(text);
        LONGS_EQUAL_TEXT(exp["chars"].get<int64_t>(), pf.chars, name.c_str());
        LONGS_EQUAL_TEXT(exp["words"].get<int64_t>(), pf.words, name.c_str());
        LONGS_EQUAL_TEXT(exp["lines"].get<int64_t>(), pf.lines, name.c_str());
        LONGS_EQUAL_TEXT(exp["prose_chars"].get<int64_t>(), pf.proseChars, name.c_str());
        LONGS_EQUAL_TEXT(exp["prose_words"].get<int64_t>(), pf.proseWords, name.c_str());
        LONGS_EQUAL_TEXT(exp["fenced_chars"].get<int64_t>(), pf.fencedChars, name.c_str());
        LONGS_EQUAL_TEXT(exp["fenced_block_count"].get<int64_t>(), pf.fencedBlockCount, name.c_str());
        LONGS_EQUAL_TEXT(exp["paste_chars"].get<int64_t>(), pf.pasteChars, name.c_str());
        LONGS_EQUAL_TEXT(exp["mixed_paste"].get<int>(), pf.mixedPaste, name.c_str());
        LONGS_EQUAL_TEXT(exp["file_path_count"].get<int64_t>(), pf.filePathCount, name.c_str());
        LONGS_EQUAL_TEXT(exp["bullet_count"].get<int64_t>(), pf.bulletCount, name.c_str());
        LONGS_EQUAL_TEXT(exp["has_code_block"].get<int>(), pf.hasCodeBlock, name.c_str());
        LONGS_EQUAL_TEXT(exp["has_file_ref"].get<int>(), pf.hasFileRef, name.c_str());
        LONGS_EQUAL_TEXT(exp["has_constraints"].get<int>(), pf.hasConstraints, name.c_str());
    }
}

TEST(InsightEngineGroup, ExtractClaudeUserPromptFeaturesRules) {
    PromptFeatures pf;
    bool isInterrupted = false;

    // 1. Sidechain user record -> excluded
    nlohmann::json sidechain = {
        {"type", "user"},
        {"isSidechain", true},
        {"message", {{"role", "user"}, {"content", "Subagent task prompt"}}}
    };
    CHECK_FALSE(InsightEngine::extractClaudeUserPromptFeatures(sidechain, pf, isInterrupted));

    // 2. Meta slash command -> excluded
    nlohmann::json metaCmd = {
        {"type", "user"},
        {"isMeta", true},
        {"message", {{"role", "user"}, {"content", "<command-name>/clear</command-name>"}}}
    };
    CHECK_FALSE(InsightEngine::extractClaudeUserPromptFeatures(metaCmd, pf, isInterrupted));

    // 3. Tool result array -> excluded
    nlohmann::json toolRes = {
        {"type", "user"},
        {"message", {{"role", "user"}, {"content", nlohmann::json::array({{{"type", "tool_result"}, {"content", "ok"}}})}}}
    };
    CHECK_FALSE(InsightEngine::extractClaudeUserPromptFeatures(toolRes, pf, isInterrupted));

    // 4. Request interrupted marker -> excluded as prompt, sets outIsInterrupted = true
    nlohmann::json interrupted = {
        {"type", "user"},
        {"message", {{"role", "user"}, {"content", nlohmann::json::array({{{"type", "text"}, {"text", "[Request interrupted by user for tool use]"}}})}}}
    };
    CHECK_FALSE(InsightEngine::extractClaudeUserPromptFeatures(interrupted, pf, isInterrupted));
    CHECK_TRUE(isInterrupted);

    // 5. Array with <ide_opened_file> + real prose -> strips IDE tag and extracts prose
    nlohmann::json ideAndProse = {
        {"type", "user"},
        {"message", {{"role", "user"}, {"content", nlohmann::json::array({
            {{"type", "text"}, {"text", "<ide_opened_file>The user opened /path/to/foo.cxx in the IDE.</ide_opened_file>"}},
            {{"type", "text"}, {"text", "fix it"}}
        })}}}
    };
    CHECK_TRUE(InsightEngine::extractClaudeUserPromptFeatures(ideAndProse, pf, isInterrupted));
    CHECK_FALSE(isInterrupted);
    LONGS_EQUAL(6, pf.proseChars);
    LONGS_EQUAL(2, pf.proseWords);
}

TEST(InsightEngineGroup, DecodeGenMetadataProtobufAndIdentity) {
    auto validBlob = buildGenBlob(1404, 500, 300, 120, 180, 2000, 1760000000ULL, 45000, 256000);
    GenerationRecord rec;
    CHECK_TRUE(InsightEngine::decodeGenMetadata(validBlob.data(), validBlob.size(), rec));
    LONGS_EQUAL(1404, rec.modelEnum);
    LONGS_EQUAL(500, rec.inputTokens);
    LONGS_EQUAL(300, rec.outputTokens);
    LONGS_EQUAL(120, rec.thinkingTokens);
    LONGS_EQUAL(180, rec.responseTokens);
    LONGS_EQUAL(2000, rec.cacheReadTokens);
    LONGS_EQUAL(24, rec.f1F4F6);
    LONGS_EQUAL(1760000000LL, rec.ts);
    DOUBLES_EQUAL(1500.0, rec.ttftMs, 0.1);
    DOUBLES_EQUAL(4250.0, rec.durationMs, 0.1);
    LONGS_EQUAL(45000, rec.contextTokens);
    LONGS_EQUAL(256000, rec.contextLimit);
    LONGS_EQUAL(64000, rec.maxOutputTokens);
    LONGS_EQUAL(1, rec.validated);

    // Broken identity: outputTokens (301) != thinkingTokens (120) + responseTokens (180)
    auto invalidBlob = buildGenBlob(1404, 500, 301, 120, 180, 2000, 1760000000ULL, 45000, 256000);
    GenerationRecord badRec;
    CHECK_TRUE(InsightEngine::decodeGenMetadata(invalidBlob.data(), invalidBlob.size(), badRec));
    LONGS_EQUAL(0, badRec.validated);
}

TEST(InsightEngineGroup, SchemaPermissionsAndFullPipeline) {
    std::string insightsDb = dir + "/insights.db";
    std::string historyDb = dir + "/history.db";
    std::string telemetryDb = dir + "/telemetry.db";
    std::string claudeDir = dir + "/claude";
    std::string agyRoot = dir + "/antigravity-ide";

    fs::create_directories(claudeDir + "/projects/-workspace-aimon");
    fs::create_directories(agyRoot + "/conversations");
    fs::create_directories(agyRoot + "/brain/360883b4-dc2a-4ed4-99d4-c8fe55808c4d/.system_generated/logs");

    time_t nowSec = std::time(nullptr);
    int64_t recentTs = static_cast<int64_t>(nowSec) - 3600;
    int64_t olderTs = static_cast<int64_t>(nowSec) - 10 * 86400;

    // 1. Seed history.db (Claude usage + quota samples including Antigravity bucket & credits)
    {
        ClaudeUsageStore cstore;
        CHECK_TRUE(cstore.open(historyDb));
        std::vector<ClaudeUsageRow> rows;
        // High cache hit session vs collapsed cache hit session (>40k tokens)
        for (int i = 0; i < 6; ++i) {
            ClaudeUsageRow r;
            r.account = "selfso";
            r.sessionId = "sess_good";
            r.messageId = "m_good_" + std::to_string(i);
            r.requestId = "r_good_" + std::to_string(i);
            r.timestamp = olderTs + i * 60;
            r.model = "claude-opus-5-5";
            r.input = 1000;
            r.output = 800;
            r.cacheRead = 20000;
            r.cacheWrite5m = 500;
            rows.push_back(r);
        }
        // Collapsed cache session + subagent fanout + retry thrash
        for (int i = 0; i < 10; ++i) {
            ClaudeUsageRow r;
            r.account = "selfso";
            r.sessionId = "sess_bad";
            r.messageId = "m_bad_" + std::to_string(i);
            r.requestId = "r_bad_" + std::to_string(i);
            r.timestamp = recentTs + i * 30;
            r.model = "claude-opus-5-5";
            r.input = 12000;
            r.output = 1500;
            r.cacheRead = 500;
            r.cacheWrite5m = 8000;
            r.sidechain = (i >= 3);
            rows.push_back(r);
        }
        CHECK_TRUE(cstore.ingest(rows).ok);
        cstore.close();

        HistoryStore hstore;
        CHECK_TRUE(hstore.open(historyDb));
        AggregateStatus st;
        st.lastUpdated = std::chrono::system_clock::from_time_t(nowSec);
        st.antigravity.isRunning = true;
        st.antigravity.availablePromptCredits = 420;
        ModelQuota mq;
        mq.modelId = "MODEL_PLACEHOLDER_M26";
        mq.modelName = "Claude Opus 4.6 (Thinking)";
        mq.remainingFraction = 0.20;
        st.antigravity.models.push_back(mq);
        st.cursor.isAuthenticated = true;
        st.cursor.fastRequestsUsed = 120;
        st.cursor.fastRequestsLimit = 500;
        CursorDailyCategorySpend cdcs;
        cdcs.dayStr = "2026-10-14";
        cdcs.dayMs = recentTs * 1000;
        cdcs.category = "composer";
        cdcs.spendCents = 1450.0;
        cdcs.cycleId = "2026-10-01";
        st.cursor.dailyCategorySpends.push_back(cdcs);
        CHECK_TRUE(hstore.recordSnapshot(st));
        hstore.close();
    }

    // 2. Seed telemetry.db with sessions and tool calls
    {
        sqlite3* tdb = nullptr;
        CHECK_EQUAL(SQLITE_OK, sqlite3_open(telemetryDb.c_str(), &tdb));
        sqlite3_exec(tdb,
            "CREATE TABLE agent_sessions ("
            "session_id TEXT PRIMARY KEY, conversation_id TEXT, agent_type TEXT, "
            "workspace_path TEXT, model_name TEXT, start_timestamp INTEGER, end_timestamp INTEGER, "
            "status TEXT, total_turns INTEGER, total_prompt_tokens INTEGER, total_comp_tokens INTEGER, "
            "total_tool_calls INTEGER, total_errors INTEGER, avg_turn_ms REAL);"
            "CREATE TABLE agent_lifecycle_events ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER, session_id TEXT, "
            "agent_type TEXT, event_type TEXT, step_index INTEGER, tool_name TEXT, "
            "duration_ms REAL, status TEXT, details_json TEXT);",
            nullptr, nullptr, nullptr);

        std::string insSessSql =
            "INSERT INTO agent_sessions VALUES ('sess_bad', 'conv_bad', 'claude_code', "
            "'/workspace/aimon', 'claude-opus-5-5', " + std::to_string(recentTs) + ", " +
            std::to_string(recentTs + 300) + ", 'ACTIVE', 6, 72000, 9000, 6, 6, 250.0);";
        sqlite3_exec(tdb, insSessSql.c_str(), nullptr, nullptr, nullptr);

        for (int i = 0; i < 6; ++i) {
            std::string evSql =
                "INSERT INTO agent_lifecycle_events (timestamp, session_id, agent_type, event_type, "
                "step_index, tool_name, duration_ms, status, details_json) VALUES (" +
                std::to_string(recentTs + i * 30 + 5) + ", 'sess_bad', 'claude_code', 'TOOL_CALL', " +
                std::to_string(i * 2 + 1) + ", 'Bash', 120.0, 'ERROR', '{\"tool_name\":\"Bash\"}');";
            sqlite3_exec(tdb, evSql.c_str(), nullptr, nullptr, nullptr);
        }
        sqlite3_close(tdb);
    }

    // 3. Seed Claude JSONL transcript
    {
        std::string jsonlPath = claudeDir + "/projects/-workspace-aimon/sess_bad.jsonl";
        std::ofstream out(jsonlPath);
        for (int i = 0; i < 6; ++i) {
            std::string promptText = (i < 4)
                ? "fix it\\nerror: foo.cxx:10:4: undeclared\\nerror: bar.cxx:12:1: fail"
                : "fix it";
            out << "{\"type\":\"user\",\"sessionId\":\"sess_bad\",\"timestamp\":\"2026-10-14T04:00:0" << i << "Z\","
                << "\"message\":{\"role\":\"user\",\"content\":\"" << promptText << "\"}}\n";
            const char* toolName = (i == 4) ? "Read" : "Edit";
            out << "{\"type\":\"assistant\",\"sessionId\":\"sess_bad\",\"requestId\":\"r_bad_" << i << "\","
                << "\"timestamp\":\"2026-10-14T04:00:1" << i << "Z\","
                << "\"message\":{\"id\":\"m_bad_" << i << "\",\"model\":\"claude-opus-5-5\","
                << "\"content\":[{\"type\":\"tool_use\",\"name\":\"" << toolName << "\",\"input\":{\"file_path\":\"/workspace/aimon/src/Foo.cxx\"}}],"
                << "\"usage\":{\"input_tokens\":12000,\"output_tokens\":1500,\"cache_read_input_tokens\":500,\"cache_creation_input_tokens\":8000}}}\n";
        }
    }

    // 4. Seed Antigravity conversation SQLite DB + transcript.jsonl
    {
        std::string convDbPath = agyRoot + "/conversations/360883b4-dc2a-4ed4-99d4-c8fe55808c4d.db";
        sqlite3* cdb = nullptr;
        CHECK_EQUAL(SQLITE_OK, sqlite3_open(convDbPath.c_str(), &cdb));
        sqlite3_exec(cdb,
            "CREATE TABLE gen_metadata(idx INTEGER, data BLOB, size INTEGER);"
            "CREATE TABLE steps(idx INTEGER, step_type INTEGER, size INTEGER);",
            nullptr, nullptr, nullptr);

        for (int i = 0; i < 6; ++i) {
            auto blob = buildGenBlob(1404, 8000, 4000, 3500, 500, 10000, recentTs + i * 60, 180000 + i * 10000, 200000);
            sqlite3_stmt* st = nullptr;
            sqlite3_prepare_v2(cdb, "INSERT INTO gen_metadata(idx, data, size) VALUES (?, ?, ?);", -1, &st, nullptr);
            sqlite3_bind_int(st, 1, i);
            sqlite3_bind_blob(st, 2, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 3, static_cast<int>(blob.size()));
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        sqlite3_exec(cdb,
            "INSERT INTO steps VALUES (0, 14, 100), (1, 8, 500), (2, 5, 800), (3, 21, 400), (4, 17, 200), (5, 15, 600);",
            nullptr, nullptr, nullptr);
        sqlite3_close(cdb);

        std::string agyTrans = agyRoot + "/brain/360883b4-dc2a-4ed4-99d4-c8fe55808c4d/.system_generated/logs/transcript.jsonl";
        std::ofstream tout(agyTrans);
        tout << "{\"step_index\":0,\"type\":\"USER_INPUT\",\"created_at\":\"2026-10-14T04:00:00Z\",\"content\":\"fix it\"}\n";
        tout << "{\"step_index\":1,\"type\":\"PLANNER_RESPONSE\",\"created_at\":\"2026-10-14T04:00:05Z\",\"tool_calls\":[{\"name\":\"replace_file_content\"}]}\n";

        std::string sumDbPath = agyRoot + "/conversation_summaries.db";
        sqlite3* sdb = nullptr;
        CHECK_EQUAL(SQLITE_OK, sqlite3_open(sumDbPath.c_str(), &sdb));
        sqlite3_exec(sdb,
            "CREATE TABLE conversation_summaries(conversation_id TEXT, workspace_uris TEXT);"
            "INSERT INTO conversation_summaries VALUES ('360883b4-dc2a-4ed4-99d4-c8fe55808c4d', '[\"file:///workspace/aimon\"]');",
            nullptr, nullptr, nullptr);
        sqlite3_close(sdb);
    }

    // 5. Seed Cursor chats/*/meta.json and Remote-SSH checkpoints/*/metadata.json
    std::string cursorHome = dir + "/cursor_home";
    std::string cursorSrv = dir + "/cursor_server";
    {
        fs::create_directories(cursorHome + "/chats/chat_1");
        std::ofstream mout(cursorHome + "/chats/chat_1/meta.json");
        mout << "{\"chatId\":\"chat_1\",\"cwd\":\"/workspace/netmon\",\"createdAt\":" << (recentTs * 1000) << "}\n";

        fs::create_directories(cursorSrv + "/data/User/globalStorage/anysphere.cursor-commits/checkpoints/cp_1");
        std::ofstream cpout(cursorSrv + "/data/User/globalStorage/anysphere.cursor-commits/checkpoints/cp_1/metadata.json");
        cpout << "{\"agentRequestId\":\"cp_req_1\",\"workspaceId\":\"netmon\",\"startTrackingDateUnixMilliseconds\":"
              << (recentTs * 1000) << ",\"fileSizeBytes\":4096,\"requestFiles\":[\"a.cxx\",\"b.cxx\"]}\n";
    }

    InsightEngine engine;
    AimonConfig cfg;
    cfg.insights.enabled = true;
    cfg.insights.dbPath = insightsDb;
    cfg.insights.coachingMinSampleSize = 1;
    cfg.insights.longSessionHours = 1;
    cfg.insights.idleGapSeconds = 60;
    cfg.insights.dripPromptsPerHour = 2;
    cfg.insights.burnRateAlertFraction = 0.01;
    ClaudeAccountConfig cac;
    cac.name = "selfso";
    cac.configDir = claudeDir;
    cac.spendLimitUsd = 0.01;
    cac.cycleResetDay = 1;
    cfg.claude.accounts.push_back(cac);
    engine.setConfig(cfg);
    CHECK_TRUE(engine.open(insightsDb));

    struct stat stBuf;
    CHECK_EQUAL(0, stat(insightsDb.c_str(), &stBuf));
    LONGS_EQUAL(0600, stBuf.st_mode & 0777);

    // Ingest live status with Cursor category spend
    AggregateStatus liveSt;
    liveSt.lastUpdated = std::chrono::system_clock::from_time_t(nowSec);
    liveSt.cursor.isAuthenticated = true;
    CursorDailyCategorySpend cdcs;
    cdcs.dayStr = "2026-10-14";
    cdcs.dayMs = recentTs * 1000;
    cdcs.category = "composer";
    cdcs.spendCents = 1450.0;
    cdcs.cycleId = "2026-10-01";
    liveSt.cursor.dailyCategorySpends.push_back(cdcs);
    CHECK_TRUE(engine.ingestLiveStatus(liveSt));

    // Rebuild twice and verify idempotency + transcript scan cursor behavior
    CHECK_TRUE(engine.rebuild(historyDb, telemetryDb, {claudeDir}, {agyRoot}, cursorHome, cursorSrv, false));
    {
        sqlite3* vdb = nullptr;
        CHECK_EQUAL(SQLITE_OK, sqlite3_open(insightsDb.c_str(), &vdb));
        auto queryInt = [&](const char* q) -> int {
            sqlite3_stmt* st = nullptr;
            int val = -1;
            if (sqlite3_prepare_v2(vdb, q, -1, &st, nullptr) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW) val = sqlite3_column_int(st, 0);
                sqlite3_finalize(st);
            }
            return val;
        };
        CHECK_TRUE(queryInt("SELECT COUNT(*) FROM ins_prompts WHERE platform='claude';") >= 3);
        CHECK_TRUE(queryInt("SELECT COUNT(*) FROM ins_prompts WHERE platform='antigravity';") >= 1);
        LONGS_EQUAL(1, queryInt("SELECT is_first_in_session FROM ins_prompts WHERE platform='antigravity' AND turn_index=0;"));
        LONGS_EQUAL(1, queryInt("SELECT COUNT(*) FROM ins_tool_calls WHERE step_index=4 AND status='ERROR';"));
        LONGS_EQUAL(1, queryInt("SELECT COUNT(*) FROM ins_tool_calls WHERE tool_name='VIEW_FILE' AND status='OK';"));
        LONGS_EQUAL(1, queryInt("SELECT COUNT(*) FROM ins_prompts WHERE platform='claude' AND reprompt_within_120s=1;"));
        LONGS_EQUAL(0, queryInt("SELECT reprompt_within_120s FROM ins_prompts WHERE platform='claude' AND turn_index=2;"));
        sqlite3_close(vdb);
    }
    auto rep1 = engine.getUsageReport("", 30, "project");
    CHECK_TRUE(rep1.contains("rows"));
    CHECK_TRUE(rep1["rows"].is_array());
    size_t count1 = rep1["rows"].size();
    CHECK_TRUE(count1 > 0);

    CHECK_TRUE(engine.rebuild(historyDb, telemetryDb, {claudeDir}, {agyRoot}, cursorHome, cursorSrv, false));
    auto rep2 = engine.getUsageReport("", 30, "project");
    LONGS_EQUAL(static_cast<long>(count1), static_cast<long>(rep2["rows"].size()));

    // Verify findings exist and include platforms_skipped and full detector suite
    auto ins = engine.getInsights("", 30, "");
    CHECK_TRUE(ins.contains("findings"));
    CHECK_TRUE(ins["findings"].is_array());
    CHECK_TRUE(ins["findings"].size() > 0);
    CHECK_TRUE(ins.contains("platforms_skipped"));
    CHECK_TRUE(ins["platforms_skipped"].is_object());

    std::set<std::string> seenDetectors;
    for (const auto& f : ins["findings"]) {
        seenDetectors.insert(f.value("detector", ""));
    }
    CHECK_TRUE(seenDetectors.count("cost_by_project") > 0);
    CHECK_TRUE(seenDetectors.count("time_of_day") > 0);
    CHECK_TRUE(seenDetectors.count("quota_pressure") > 0);
    CHECK_TRUE(seenDetectors.count("burn_rate_forecast") > 0);
    CHECK_TRUE(seenDetectors.count("drip_vs_batch") > 0);

    auto insWarn = engine.getInsights("", 30, "warn");
    CHECK_TRUE(insWarn["findings"].size() > 0);
    for (const auto& f : insWarn["findings"]) {
        std::string sev = f.value("severity", "");
        CHECK_TRUE(sev == "warn" || sev == "crit");
    }

    // Verify compareExperiment works
    auto cmp = engine.compareExperiment("cache_hit_collapse", "claude", recentTs - 3600);
    CHECK_TRUE(cmp.contains("detector"));
    CHECK_TRUE(cmp.contains("verdict"));
    CHECK_TRUE(cmp.contains("before"));
    CHECK_TRUE(cmp.contains("after"));

    // Verify describeDataset
    auto desc = InsightEngine::describeDataset();
    CHECK_TRUE(desc.contains("tables"));
    CHECK_TRUE(desc.contains("sample_queries"));

    // Verify exportDataset (sqlite, csv, jsonl) with 0600 permissions
    std::string exportDir = dir + "/export";
    auto expSqlite = engine.exportDataset(exportDir + "/sqlite", "sqlite", 30, true);
    CHECK_TRUE(expSqlite.contains("files"));
    CHECK_TRUE(expSqlite["files"].size() > 0);
    for (const auto& f : expSqlite["files"]) {
        struct stat fst;
        CHECK_EQUAL(0, stat(f["path"].get<std::string>().c_str(), &fst));
        LONGS_EQUAL(0600, fst.st_mode & 0777);
    }

    // Verify CSV export and non-anonymized export (including comma separation and RFC 4180 quote escaping)
    auto expCsv = engine.exportDataset(exportDir + "/csv", "csv", 30, false);
    CHECK_TRUE(expCsv.contains("files"));
    CHECK_TRUE(expCsv["files"].size() >= 8);
    {
        std::ifstream fin(exportDir + "/csv/ins_findings.csv");
        std::string csvContent((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
        CHECK_TRUE(csvContent.find("id,computed_ts,window_days,detector,platform") != std::string::npos);
        CHECK_TRUE(csvContent.find("\"\"") != std::string::npos);
    }

    // Verify getUsageReport group_by modes
    for (const char* gb : {"day", "model", "project", "session"}) {
        auto rep = engine.getUsageReport("claude", 30, gb);
        CHECK_TRUE(rep.contains("rows"));
        CHECK_TRUE(rep["rows"].size() > 0);
    }

    // Verify getInsights filtering by severity
    auto insCrit = engine.getInsights("claude", 30, "crit");
    CHECK_TRUE(insCrit.contains("findings"));
    for (const auto& f : insCrit["findings"]) {
        STRCMP_EQUAL("crit", f.value("severity", "").c_str());
    }

    // Verify weekly digest
    std::string digest = engine.generateWeeklyDigest(7);
    CHECK_TRUE(!digest.empty());
    CHECK_TRUE(digest.find("# AI Usage & Workflow Digest") != std::string::npos);
    // Ensure raw prompt text ("undeclared") never leaks into digest
    CHECK_EQUAL(std::string::npos, digest.find("undeclared"));

    // Verify resetCursors rebuild path
    CHECK_TRUE(engine.rebuild(historyDb, telemetryDb, {claudeDir}, {agyRoot}, cursorHome, cursorSrv, true));
}

TEST(InsightEngineGroup, MultiCyclePricingAndBurnRateForecastScoping) {
    std::string insightsDb = dir + "/insights_cycle.db";
    std::string historyDb = dir + "/history_cycle.db";
    std::string telemetryDb = dir + "/telemetry_cycle.db";
    std::string claudeDir = dir + "/claude_cycle";
    fs::create_directories(claudeDir);

    time_t nowSec = std::time(nullptr);
    ClaudeCycle curCycle{};
    claudeCycleBounds(static_cast<int64_t>(nowSec), 1, curCycle);

    // Ensure timestamps fall strictly inside previous cycle and current cycle
    int64_t prevCycleTs = curCycle.startEpoch - 5 * 86400;
    int64_t curCycleTs = curCycle.startEpoch + 60;
    if (curCycleTs >= static_cast<int64_t>(nowSec)) {
        curCycleTs = curCycle.startEpoch;
    }

    {
        ClaudeUsageStore cstore;
        CHECK_TRUE(cstore.open(historyDb));
        std::vector<ClaudeUsageRow> rows;

        // 1. Previous billing cycle: massive spend ($800 on opus-5-5: 16M output tokens @ $50/MTok)
        ClaudeUsageRow prevRow;
        prevRow.account = "selfso";
        prevRow.sessionId = "sess_prev_cycle";
        prevRow.messageId = "m_prev_1";
        prevRow.requestId = "r_prev_1";
        prevRow.timestamp = prevCycleTs;
        prevRow.model = "claude-opus-5-5";
        prevRow.input = 0;
        prevRow.output = 16000000;
        rows.push_back(prevRow);

        // 2. Current billing cycle: sonnet-5-5 with 1B cache read tokens ($100 at read_nano=100, NOT $500)
        ClaudeUsageRow curSonnet;
        curSonnet.account = "selfso";
        curSonnet.sessionId = "sess_cur_cycle";
        curSonnet.messageId = "m_cur_1";
        curSonnet.requestId = "r_cur_1";
        curSonnet.timestamp = curCycleTs;
        curSonnet.model = "claude-sonnet-5-5";
        curSonnet.input = 0;
        curSonnet.output = 0;
        curSonnet.cacheRead = 1000000000LL;
        rows.push_back(curSonnet);

        // 3. Current billing cycle: haiku-5-5 upper-tier context (>100k tokens -> read_nano=50 -> $5 for 100M)
        ClaudeUsageRow curHaikuTier2;
        curHaikuTier2.account = "selfso";
        curHaikuTier2.sessionId = "sess_cur_cycle";
        curHaikuTier2.messageId = "m_cur_2";
        curHaikuTier2.requestId = "r_cur_2";
        curHaikuTier2.timestamp = curCycleTs + 10;
        curHaikuTier2.model = "claude-haiku-5-5-20251001";
        curHaikuTier2.input = 200000; // context > 100k threshold -> upper tier (input_nano=500 -> $0.10)
        curHaikuTier2.output = 0;
        curHaikuTier2.cacheRead = 100000000LL; // read_nano=50 -> $5.00
        rows.push_back(curHaikuTier2);

        CHECK_TRUE(cstore.ingest(rows).ok);
        cstore.close();
    }

    InsightEngine engine;
    AimonConfig cfg;
    cfg.insights.enabled = true;
    cfg.insights.dbPath = insightsDb;
    cfg.insights.burnRateAlertFraction = 0.05; // Trigger forecast alert at 5% of $1000 ($50)
    ClaudeAccountConfig cac;
    cac.name = "selfso";
    cac.configDir = claudeDir;
    cac.spendLimitUsd = 1000.0;
    cac.cycleResetDay = 1;
    cfg.claude.accounts.push_back(cac);
    engine.setConfig(cfg);
    CHECK_TRUE(engine.open(insightsDb));
    CHECK_TRUE(engine.rebuild(historyDb, telemetryDb, {claudeDir}, {}, "", "", false));

    auto ins = engine.getInsights("claude", 30, "");
    CHECK_TRUE(ins.contains("findings"));
    bool foundBurnRate = false;
    for (const auto& f : ins["findings"]) {
        if (f.value("detector", "") != "burn_rate_forecast") continue;
        foundBurnRate = true;
        double cycleSpend = f["evidence"].value("est_spend_usd", 0.0);
        int64_t cycleStartTs = f["evidence"].value("cycle_start_epoch", static_cast<int64_t>(0));
        int64_t cycleEndTs = f["evidence"].value("cycle_end_epoch", static_cast<int64_t>(0));
        // Current cycle spend must be ~$105.10 ($100 sonnet cache read + $5.10 haiku upper tier),
        // excluding the $800 previous-cycle spend and NOT $505 from opus fallback pricing.
        DOUBLES_EQUAL(105.10, cycleSpend, 0.05);
        LONGS_EQUAL(curCycle.startEpoch, cycleStartTs);
        LONGS_EQUAL(curCycle.endEpoch, cycleEndTs);
    }
    CHECK_TRUE(foundBurnRate);
}

TEST(InsightEngineGroup, CursorHashedWorkspaceAndNestedChatAttribution) {
    std::string insightsDb = dir + "/insights_cursor.db";
    std::string historyDb = dir + "/history_cursor.db";
    std::string telemetryDb = dir + "/telemetry_cursor.db";
    std::string cursorHome = dir + "/cursor_home_hash";
    std::string cursorSrv = dir + "/cursor_srv_hash";

    time_t nowSec = std::time(nullptr);
    int64_t recentMs = (static_cast<int64_t>(nowSec) - 1800) * 1000;

    // 1. Nested ~/.cursor/chats/<workspaceHash>/<chatUuid>/meta.json with real project path
    fs::create_directories(cursorHome + "/chats/349fcc52d97998f88142ca5a0f7fe253/uuid-1");
    {
        std::ofstream out(cursorHome + "/chats/349fcc52d97998f88142ca5a0f7fe253/uuid-1/meta.json");
        out << "{\"chatId\":\"uuid-1\",\"cwd\":\"/workspace/meshmon\",\"createdAt\":" << recentMs << "}\n";
    }
    // 2. Nested chat whose cwd is bare HOME or hex hash -> should map to "unattributed"
    fs::create_directories(cursorHome + "/chats/df134b4cef1398da994a852b6380a0bc/uuid-2");
    {
        std::ofstream out(cursorHome + "/chats/df134b4cef1398da994a852b6380a0bc/uuid-2/meta.json");
        out << "{\"chatId\":\"uuid-2\",\"createdAt\":" << recentMs << "}\n";
    }

    // 3. Checkpoints referencing known hash (349fcc52...) and unknown hash (df134b4c...)
    fs::create_directories(cursorSrv + "/data/User/globalStorage/anysphere.cursor-commits/checkpoints/cp_known");
    {
        std::ofstream out(cursorSrv + "/data/User/globalStorage/anysphere.cursor-commits/checkpoints/cp_known/metadata.json");
        out << "{\"agentRequestId\":\"req_k\",\"workspaceId\":\"349fcc52d97998f88142ca5a0f7fe253\","
            << "\"startTrackingDateUnixMilliseconds\":" << recentMs << ",\"fileSizeBytes\":1024,\"requestFiles\":[\"a.cxx\"]}\n";
    }
    fs::create_directories(cursorSrv + "/data/User/globalStorage/anysphere.cursor-commits/checkpoints/cp_unknown");
    {
        std::ofstream out(cursorSrv + "/data/User/globalStorage/anysphere.cursor-commits/checkpoints/cp_unknown/metadata.json");
        out << "{\"agentRequestId\":\"req_u\",\"workspaceId\":\"df134b4cef1398da994a852b6380a0bc\","
            << "\"startTrackingDateUnixMilliseconds\":" << recentMs << ",\"fileSizeBytes\":2048,\"requestFiles\":[\"b.cxx\"]}\n";
    }

    InsightEngine engine;
    CHECK_TRUE(engine.open(insightsDb));
    CHECK_TRUE(engine.rebuild(historyDb, telemetryDb, {}, {}, cursorHome, cursorSrv, false));

    auto rep = engine.getUsageReport("cursor", 30, "project");
    CHECK_TRUE(rep.contains("rows"));
    std::set<std::string> projects;
    for (const auto& r : rep["rows"]) {
        std::string p = r.value("key", "");
        projects.insert(p);
        // Raw 32-char hashes must never appear as project names
        CHECK_TRUE(p != "349fcc52d97998f88142ca5a0f7fe253");
        CHECK_TRUE(p != "df134b4cef1398da994a852b6380a0bc");
    }
    CHECK_TRUE(projects.count("meshmon") > 0);
    CHECK_TRUE(projects.count("unattributed") > 0);
}

TEST(InsightEngineGroup, StepMappingWireTypesAndCompareExperimentVerdicts) {
    // 1. Verify mapAntigravityStepType for all known and unknown codes
    STRCMP_EQUAL("CODE_ACTION", InsightEngine::mapAntigravityStepType(5).c_str());
    STRCMP_EQUAL("GREP_SEARCH", InsightEngine::mapAntigravityStepType(7).c_str());
    STRCMP_EQUAL("VIEW_FILE", InsightEngine::mapAntigravityStepType(8).c_str());
    STRCMP_EQUAL("LIST_DIRECTORY", InsightEngine::mapAntigravityStepType(9).c_str());
    STRCMP_EQUAL("USER_INPUT", InsightEngine::mapAntigravityStepType(14).c_str());
    STRCMP_EQUAL("PLANNER_RESPONSE", InsightEngine::mapAntigravityStepType(15).c_str());
    STRCMP_EQUAL("ERROR_MESSAGE", InsightEngine::mapAntigravityStepType(17).c_str());
    STRCMP_EQUAL("RUN_COMMAND", InsightEngine::mapAntigravityStepType(21).c_str());
    STRCMP_EQUAL("CHECKPOINT", InsightEngine::mapAntigravityStepType(23).c_str());
    STRCMP_EQUAL("SEARCH_WEB", InsightEngine::mapAntigravityStepType(33).c_str());
    STRCMP_EQUAL("MCP_TOOL", InsightEngine::mapAntigravityStepType(38).c_str());
    STRCMP_EQUAL("CODE_ACKNOWLEDGEMENT", InsightEngine::mapAntigravityStepType(83).c_str());
    STRCMP_EQUAL("EPHEMERAL_MESSAGE", InsightEngine::mapAntigravityStepType(90).c_str());
    STRCMP_EQUAL("GENERATE_IMAGE", InsightEngine::mapAntigravityStepType(91).c_str());
    STRCMP_EQUAL("CONVERSATION_HISTORY", InsightEngine::mapAntigravityStepType(98).c_str());
    STRCMP_EQUAL("KNOWLEDGE_ARTIFACTS", InsightEngine::mapAntigravityStepType(99).c_str());
    STRCMP_EQUAL("SYSTEM_MESSAGE", InsightEngine::mapAntigravityStepType(101).c_str());
    STRCMP_EQUAL("GENERIC", InsightEngine::mapAntigravityStepType(132).c_str());
    STRCMP_EQUAL("ASK_QUESTION", InsightEngine::mapAntigravityStepType(138).c_str());
    STRCMP_EQUAL("STEP_999", InsightEngine::mapAntigravityStepType(999).c_str());

    // 2. Verify decodeGenMetadata edge cases (null/empty, model enums)
    GenerationRecord rec;
    CHECK_FALSE(InsightEngine::decodeGenMetadata(nullptr, 0, rec));
    for (uint64_t mEnum : {290ULL, 333ULL, 334ULL, 342ULL, 1018ULL, 1026ULL, 1208ULL, 9999ULL}) {
        auto b = buildGenBlob(mEnum, 100, 50, 20, 30, 0, 1760000000ULL, 1000, 200000);
        GenerationRecord mr;
        CHECK_TRUE(InsightEngine::decodeGenMetadata(b.data(), b.size(), mr));
        LONGS_EQUAL(static_cast<int64_t>(mEnum), mr.modelEnum);
        LONGS_EQUAL(1, mr.validated);
    }

    // 3. Verify compareExperiment verdicts (improved, regressed, insufficient_data)
    std::string insightsDb = dir + "/insights_cmp.db";
    InsightEngine engine;
    AimonConfig cfg;
    cfg.insights.coachingMinSampleSize = 3;
    engine.setConfig(cfg);
    CHECK_TRUE(engine.open(insightsDb));

    int64_t splitTs = static_cast<int64_t>(std::time(nullptr)) - 3600;
    sqlite3* db = nullptr;
    CHECK_EQUAL(SQLITE_OK, sqlite3_open(insightsDb.c_str(), &db));
    for (int i = 0; i < 6; ++i) {
        int64_t bTs = splitTs - (i + 1) * 300;
        int64_t aTs = splitTs + (i + 1) * 300;
        std::string sql =
            "INSERT INTO ins_requests (platform, account, session_id, project, request_id, message_id, ts, model, speed, "
            "is_sidechain, input_tokens, output_tokens, cache_read, cache_write_5m, cache_write_1h, context_tokens, compacted, thinking_tokens, web_search_reqs, est_cost_nano) VALUES "
            "('claude', 'selfso', 'sb_" + std::to_string(i) + "', 'aimon', 'rb_" + std::to_string(i) + "', 'mb_" + std::to_string(i) + "', " +
            std::to_string(bTs) + ", 'claude-opus-5-5', 'standard', 1, 90000, 2000, 10000, 0, 0, 100000, 0, 0, 0, 1000000),"
            "('claude', 'selfso', 'sa_" + std::to_string(i) + "', 'aimon', 'ra_" + std::to_string(i) + "', 'ma_" + std::to_string(i) + "', " +
            std::to_string(aTs) + ", 'claude-sonnet-5-5', 'standard', 0, 10000, 2000, 90000, 0, 0, 100000, 0, 0, 0, 100000);"
            "INSERT INTO ins_prompts (platform, session_id, turn_index, ts, project, chars, words, lines, prose_chars, prose_words, "
            "fenced_chars, fenced_block_count, paste_chars, mixed_paste, file_path_count, bullet_count, is_sidechain, has_code_block, "
            "has_file_ref, has_constraints, is_first_in_session, next_tool_calls, next_tool_errors, next_edits, next_Read_before_Edit, "
            "reprompt_within_120s, interrupted_turn, turn_tokens, turn_cost_nano) VALUES "
            "('claude', 'sb_" + std::to_string(i) + "', 0, " + std::to_string(bTs) +
            ", 'aimon', 10, 2, 1, 10, 2, 0, 0, 800, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 10000, 1000000),"
            "('claude', 'sa_" + std::to_string(i) + "', 0, " + std::to_string(aTs) +
            ", 'aimon', 200, 35, 4, 200, 35, 0, 0, 0, 0, 2, 2, 0, 1, 1, 1, 1, 4, 0, 2, 1, 0, 0, 2000, 100000);";
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    }
    sqlite3_close(db);

    for (const char* det : {"cache_efficiency", "cache_hit_collapse", "subagent_share",
                            "subagent_fanout_overhead", "short_prompt_rate"}) {
        auto res = engine.compareExperiment(det, "claude", splitTs);
        STRCMP_EQUAL("improved", res.value("verdict", "").c_str());
    }

    auto insuff = engine.compareExperiment("cache_hit_collapse", "cursor", splitTs);
    STRCMP_EQUAL("insufficient_data", insuff.value("verdict", "").c_str());

    // 4. Verify isOpen() before/after close, bullet variants, paste detectors, file path tokens,
    //    protobuf wireType 1/5 + truncations, and CSV double-quote escaping
    CHECK_TRUE(engine.isOpen());
    engine.close();
    CHECK_FALSE(engine.isOpen());

    PromptFeatures bpf = InsightEngine::extractPromptFeatures("- a\n* b\n+ c\n-\td\n*\te\n+\tf\n1. g\n2)\th");
    LONGS_EQUAL(8, bpf.bulletCount);

    PromptFeatures tsSpace = InsightEngine::extractPromptFeatures(
        "check this:\n2026-10-14 12:00:01 line1\n2026-10-14 12:00:02 line2\n2026-10-14 12:00:03 line3");
    CHECK_TRUE(tsSpace.pasteChars > 0);

    PromptFeatures tsT = InsightEngine::extractPromptFeatures(
        "check this:\n2026-10-14T12:00:01 line1\n2026-10-14T12:00:02 line2\n2026-10-14T12:00:03 line3");
    CHECK_TRUE(tsT.pasteChars > 0);

    PromptFeatures pyPaste = InsightEngine::extractPromptFeatures(
        "trace:\nFile \"a.py\", line 1\nFile \"b.py\", line 2\nFile \"c.py\", line 3");
    CHECK_TRUE(pyPaste.pasteChars > 0);

    PromptFeatures jsPaste = InsightEngine::extractPromptFeatures(
        "trace:\nat foo (a.js:1:1)\nat bar (b.js:2:2)\nat baz (c.js:3:3)");
    CHECK_TRUE(jsPaste.pasteChars > 0);

    PromptFeatures notePaste = InsightEngine::extractPromptFeatures(
        "build:\na.cxx:1: note: candidate 1\nb.cxx:2: note: candidate 2\nc.cxx:3: note: candidate 3");
    CHECK_TRUE(notePaste.pasteChars > 0);
    PromptFeatures noteNonPaste = InsightEngine::extractPromptFeatures(
        "note: this is prose\nnote: still prose\nnote: no line number");
    LONGS_EQUAL(0, noteNonPaste.pasteChars);

    PromptFeatures indentPaste = InsightEngine::extractPromptFeatures(
        "    int a = 1;\n    int b = 2;\n    int c = 3;\n    int d = 4;");
    CHECK_TRUE(indentPaste.pasteChars > 0);
    LONGS_EQUAL(1, indentPaste.mixedPaste);
    LONGS_EQUAL(0, noteNonPaste.mixedPaste);

    LONGS_EQUAL(1, InsightEngine::extractPromptFeatures("a_b.cxx").filePathCount);
    LONGS_EQUAL(1, InsightEngine::extractPromptFeatures("a-b.cxx").filePathCount);
    LONGS_EQUAL(1, InsightEngine::extractPromptFeatures("a+b.cxx").filePathCount);
    LONGS_EQUAL(1, InsightEngine::extractPromptFeatures("dir/file.unknown").filePathCount);
    LONGS_EQUAL(0, InsightEngine::extractPromptFeatures("file.unknown").filePathCount);
    LONGS_EQUAL(0, InsightEngine::extractPromptFeatures("bad$char.cxx").filePathCount);

    PromptFeatures kwStart = InsightEngine::extractPromptFeatures("must pass all tests");
    LONGS_EQUAL(1, kwStart.hasConstraints);
    PromptFeatures kwEnd = InsightEngine::extractPromptFeatures("check this must");
    LONGS_EQUAL(1, kwEnd.hasConstraints);
    PromptFeatures kwPrefix = InsightEngine::extractPromptFeatures("mustard on sandwich");
    LONGS_EQUAL(0, kwPrefix.hasConstraints);

    // Unclosed XML tag and CRLF after XML tag in Claude prompt extraction
    PromptFeatures crlfPf;
    bool intr = false;
    nlohmann::json crlfPrompt = {
        {"type", "user"},
        {"message", {{"role", "user"}, {"content", "<ide_opened_file>foo</ide_opened_file>\r\ncheck src/Main.cxx"}}}
    };
    CHECK_TRUE(InsightEngine::extractClaudeUserPromptFeatures(crlfPrompt, crlfPf, intr));
    LONGS_EQUAL(1, crlfPf.filePathCount);

    nlohmann::json unclosedXml = {
        {"type", "user"},
        {"message", {{"role", "user"}, {"content", "hello <system-reminder>unclosed tag"}}}
    };
    CHECK_TRUE(InsightEngine::extractClaudeUserPromptFeatures(unclosedXml, crlfPf, intr));
    LONGS_EQUAL(6, crlfPf.proseChars);

    // Protobuf wireType 1 (64-bit) and wireType 5 (32-bit) and truncated wire types
    std::vector<uint8_t> wtBlob = buildGenBlob(1404, 100, 50, 20, 30, 0, 1760000000ULL, 1000, 200000);
    appendTag(wtBlob, 10, 1); // wireType 1 (8 bytes)
    for (int i = 0; i < 8; ++i) wtBlob.push_back(0);
    appendTag(wtBlob, 11, 5); // wireType 5 (4 bytes)
    for (int i = 0; i < 4; ++i) wtBlob.push_back(0);
    GenerationRecord wtRec;
    CHECK_TRUE(InsightEngine::decodeGenMetadata(wtBlob.data(), wtBlob.size(), wtRec));

    // Truncated wireType 5 (only 2 bytes instead of 4)
    std::vector<uint8_t> truncWt5 = wtBlob;
    truncWt5.pop_back();
    truncWt5.pop_back();
    CHECK_FALSE(InsightEngine::decodeGenMetadata(truncWt5.data(), truncWt5.size(), wtRec));
}

int main(int argc, char** argv) {
    return CommandLineTestRunner::RunAllTests(argc, argv);
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
