/*
 * TestInsightSurfaces.cxx
 *
 * Qualification tests for Usage Insights MCP tools (get_usage_insights,
 * get_usage_report, export_usage_dataset, describe_usage_dataset,
 * compare_experiment) and WebServer REST endpoints (/api/v1/insights,
 * /api/v1/insights/report, /api/v1/insights/digest, /api/v1/insights/compare,
 * /api/v1/insights/export).
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "ClaudeUsageStore.hxx"
#include "ConfigManager.hxx"
#include "HistoryStore.hxx"
#include "InsightEngine.hxx"
#include "McpServer.hxx"
#include "StateStore.hxx"
#include "WebServer.hxx"

// CppUTest's memory leak macros redefine `new`; its headers must come last.
#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/TestHarness.h>

namespace fs = std::filesystem;
using namespace aimon;
using json = nlohmann::json;

namespace {

static std::string makeTempDir() {
    std::string tmpl = "/tmp/aimon_insight_surf_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* res = mkdtemp(buf.data());
    return res ? std::string(res) : "";
}

static json callMcpTool(McpServer& server, const std::string& toolName,
                        const json& args = json::object(),
                        const std::string& profile = "") {
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 42},
        {"method", "tools/call"},
        {"params", {
            {"name", toolName},
            {"arguments", args}
        }}
    };
    return server.handleMessage(req, profile);
}

} // namespace

TEST_GROUP(InsightSurfacesGroup) {
    std::string dir;
    InsightEngine* engine = nullptr;
    HistoryStore* hstore = nullptr;
    McpServer* mcp = nullptr;

    void setup() override {
        dir = makeTempDir();
        CHECK_TRUE(!dir.empty());

        std::string histPath = dir + "/history.db";
        std::string insPath = dir + "/insights.db";
        std::string claudeDir = dir + "/claude";
        fs::create_directories(claudeDir + "/projects/-home-samurai-work-aimon");

        int64_t nowSec = static_cast<int64_t>(std::time(nullptr));
        ClaudeUsageStore cstore;
        CHECK_TRUE(cstore.open(histPath));
        std::vector<ClaudeUsageRow> rows;
        for (int i = 0; i < 6; ++i) {
            ClaudeUsageRow r;
            r.account = "selfso";
            r.sessionId = "sess_surf";
            r.messageId = "m_surf_" + std::to_string(i);
            r.requestId = "r_surf_" + std::to_string(i);
            r.timestamp = nowSec - 1800 + i * 30;
            r.model = "claude-opus-5-5";
            r.input = 15000;
            r.output = 1200;
            r.cacheRead = 500;
            r.cacheWrite5m = 5000;
            rows.push_back(r);
        }
        CHECK_TRUE(cstore.ingest(rows).ok);
        cstore.close();

        hstore = new HistoryStore();
        CHECK_TRUE(hstore->open(histPath));

        engine = new InsightEngine();
        AimonConfig cfg;
        cfg.insights.enabled = true;
        cfg.insights.dbPath = insPath;
        cfg.insights.coachingMinSampleSize = 3;
        engine->setConfig(cfg);
        CHECK_TRUE(engine->open(insPath));
        CHECK_TRUE(engine->rebuild(histPath, "", {claudeDir}, {}, "", "", false));

        mcp = new McpServer(StateStore::getInstance());
        mcp->setInsightEngine(engine);
    }

    void teardown() override {
        delete mcp;
        if (engine) {
            engine->close();
            delete engine;
        }
        if (hstore) {
            hstore->close();
            delete hstore;
        }
        std::string cmd = "rm -rf '" + dir + "'";
        int rc = std::system(cmd.c_str());
        (void)rc;
    }
};

TEST(InsightSurfacesGroup, McpToolsListedAndCallableAcrossProfiles) {
    static constexpr const char* kExpectedTools[] = {
        "get_usage_insights",
        "get_usage_report",
        "export_usage_dataset",
        "describe_usage_dataset",
        "compare_experiment"
    };

    for (const char* profile : {"", "core", "embedded", "network", "mesh", "all"}) {
        json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", json::object()}};
        json res = mcp->handleMessage(req, profile);
        CHECK_TRUE(res.contains("result"));
        CHECK_TRUE(res["result"].contains("tools"));

        for (const char* expected : kExpectedTools) {
            bool found = false;
            for (const auto& t : res["result"]["tools"]) {
                if (t["name"] == expected) {
                    found = true;
                    break;
                }
            }
            CHECK_TEXT(found, expected);
        }
    }

    // Call get_usage_insights
    json resIns = callMcpTool(*mcp, "get_usage_insights", {{"days", 30}}, "core");
    CHECK_TRUE(resIns.contains("result"));
    std::string txtIns = resIns["result"]["content"][0]["text"].get<std::string>();
    json parsedIns = json::parse(txtIns);
    CHECK_TRUE(parsedIns.contains("findings"));
    CHECK_TRUE(parsedIns["findings"].is_array());
    CHECK_TRUE(parsedIns["findings"].size() > 0);

    // Call get_usage_report
    json resRep = callMcpTool(*mcp, "get_usage_report", {{"group_by", "project"}, {"days", 30}}, "core");
    CHECK_TRUE(resRep.contains("result"));
    json parsedRep = json::parse(resRep["result"]["content"][0]["text"].get<std::string>());
    CHECK_TRUE(parsedRep.contains("rows"));
    CHECK_TRUE(parsedRep["rows"].size() > 0);

    // Call describe_usage_dataset
    json resDesc = callMcpTool(*mcp, "describe_usage_dataset", json::object(), "core");
    CHECK_TRUE(resDesc.contains("result"));
    json parsedDesc = json::parse(resDesc["result"]["content"][0]["text"].get<std::string>());
    CHECK_TRUE(parsedDesc.contains("tables"));
    CHECK_TRUE(parsedDesc.contains("sample_queries"));

    // Call compare_experiment
    json resCmp = callMcpTool(*mcp, "compare_experiment", {{"detector", "cache_hit_collapse"}, {"platform", "claude"}}, "core");
    CHECK_TRUE(resCmp.contains("result"));
    json parsedCmp = json::parse(resCmp["result"]["content"][0]["text"].get<std::string>());
    CHECK_TRUE(parsedCmp.contains("verdict"));

    // Call export_usage_dataset
    std::string expOut = dir + "/mcp_export";
    json resExp = callMcpTool(*mcp, "export_usage_dataset",
                              {{"output_dir", expOut}, {"format", "jsonl"}, {"days", 30}, {"anonymize_projects", true}},
                              "core");
    CHECK_TRUE(resExp.contains("result"));
    json parsedExp = json::parse(resExp["result"]["content"][0]["text"].get<std::string>());
    CHECK_TRUE(parsedExp.contains("files"));
    CHECK_TRUE(parsedExp["files"].size() >= 8);
}

TEST(InsightSurfacesGroup, WebApiInsightsEndpointsReturnStructuredJson) {
    WebConfig wcfg;
    wcfg.host = "127.0.0.1";
    wcfg.port = 0;
    wcfg.endpointsEnabled = true;

    WebServer server(StateStore::getInstance(), *hstore, wcfg, nullptr, mcp, nullptr);
    server.setInsightEngine(engine);
    CHECK_TRUE(server.start(true));
    int port = server.getPort();
    CHECK_TRUE(port > 0);

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);

    auto r1 = cli.Get("/api/v1/insights?days=30");
    CHECK_TRUE(r1 != nullptr);
    LONGS_EQUAL(200, r1->status);
    json j1 = json::parse(r1->body);
    CHECK_TRUE(j1.contains("findings"));
    CHECK_TRUE(j1["findings"].is_array());

    auto r2 = cli.Get("/api/v1/insights/report?group_by=project&days=30");
    CHECK_TRUE(r2 != nullptr);
    LONGS_EQUAL(200, r2->status);
    json j2 = json::parse(r2->body);
    CHECK_TRUE(j2.contains("rows"));
    CHECK_TRUE(j2["rows"].size() > 0);

    auto r3 = cli.Get("/api/v1/insights/digest?days=7");
    CHECK_TRUE(r3 != nullptr);
    LONGS_EQUAL(200, r3->status);
    json j3 = json::parse(r3->body);
    CHECK_TRUE(j3.contains("markdown"));
    CHECK_TRUE(j3["markdown"].get<std::string>().find("# AI Usage & Workflow Digest") != std::string::npos);

    auto r4 = cli.Get("/api/v1/insights/compare?detector=cache_hit_collapse&platform=claude");
    CHECK_TRUE(r4 != nullptr);
    LONGS_EQUAL(200, r4->status);
    json j4 = json::parse(r4->body);
    CHECK_TRUE(j4.contains("verdict"));

    json expBody = {{"output_dir", dir + "/web_export"}, {"format", "csv"}, {"days", 30}, {"anonymize_projects", true}};
    auto r5 = cli.Post("/api/v1/insights/export", expBody.dump(), "application/json");
    CHECK_TRUE(r5 != nullptr);
    LONGS_EQUAL(200, r5->status);
    json j5 = json::parse(r5->body);
    CHECK_TRUE(j5.contains("files"));
    CHECK_TRUE(j5["files"].size() >= 8);

    server.stop();
}

int main(int argc, char** argv) {
    // Disable CppUTest's non-thread-safe global new/delete linked-list tracker
    // across 72-thread cpp-httplib ThreadPool workers (ASan/LSan verifies leaks).
    MemoryLeakWarningPlugin::turnOffNewDeleteOverloads();

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
