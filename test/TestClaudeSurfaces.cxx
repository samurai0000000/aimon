/*
 * TestClaudeSurfaces.cxx
 *
 * Qualification tests for how Claude usage is exposed: the MCP tool and its
 * text, the combined status, the history samples, the MQTT messages and the
 * status JSON. Statuses are built directly; no network and no broker.
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "HistoryStore.hxx"
#include "McpServer.hxx"
#include "Models.hxx"
#include "MqttPublisher.hxx"
#include "StateStore.hxx"

// CppUTest's memory leak macros redefine `new`; its headers must come last.
#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/TestHarness.h>

using namespace aimon;
using json = nlohmann::json;

static ClaudeWindowTotals window(const std::string& label, int64_t tokens, int64_t costNano, int64_t messages) {
    ClaudeWindowTotals w;
    w.label = label;
    w.messages = messages;
    w.input = tokens / 10;
    w.output = tokens / 10;
    w.cacheRead = tokens - 4 * (tokens / 10);
    w.cacheWrite5m = tokens / 10;
    w.cacheWrite1h = tokens / 10;
    w.costNano = costNano;
    return w;
}

static ClaudeAccountStatus makeAccount(const std::string& name, ClaudeTier tier) {
    ClaudeAccountStatus a;
    a.name = name;
    a.tier = tier;
    a.rawSubscriptionType = tier == ClaudeTier::Enterprise ? "enterprise" : "personal-fixture";
    a.rawRateLimitTier = "tier-fixture";
    a.hasData = true;
    a.windowDays = 30;
    a.last5h = window("last_5h", 1500000, 2500000000LL, 12);
    a.last7d = window("last_7d", 9800000, 100000000000LL, 90);
    a.window = window("window", 32200000, 174540000000LL, 400);
    a.today = window("today", 500000, 1000000000LL, 5);
    ClaudeModelUsage m1;
    m1.model = "claude-sonnet-5-5";
    m1.messages = 300;
    m1.input = 1000000; m1.output = 1000000; m1.cacheRead = 17000000; m1.cacheWrite5m = 500000; m1.cacheWrite1h = 500000;
    m1.costNano = 51050000000LL;
    ClaudeModelUsage m2;
    m2.model = "claude-haiku-4-5-20251001";
    m2.messages = 100;
    m2.input = 100; m2.output = 200; m2.cacheRead = 300;
    m2.costNano = 1250000000LL;
    a.models = {m1, m2};
    ClaudeDailyPoint d;
    d.dayStr = "2026-10-07";
    d.dayMs = 1791331200000LL;
    d.messages = 10; d.tokens = 1000; d.costNano = 45690000000LL;
    a.daily = {d};
    a.rowsInStore = 400;
    a.filesScanned = 11;
    return a;
}

static ClaudeStatus makeStatus() {
    ClaudeStatus s;
    s.enabled = true;
    s.pricing.sourceUrl = "https://example.invalid/pricing.md";
    s.pricing.fetchedAtEpoch = 1791439200;      // 2026-10-08T06:00:00Z
    s.pricing.stale = false;
    s.pricing.modelsLoaded = 20;
    s.pricing.versionCount = 2;
    ClaudeAccountStatus a = makeAccount("work", ClaudeTier::Enterprise);
    a.cycleConfigured = true;
    a.cycle = window("cycle", 20000000, 58810000000LL, 200);
    a.spendLimitUsd = 500.0;
    a.estPctOfLimit = 11.762;
    a.cycleResetIso = "2026-11-01T00:00:00Z";
    s.accounts = {a};
    return s;
}

static std::string callTool(McpServer& server, const std::string& tool, const std::string& profile = "") {
    json req = {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/call"},
                {"params", {{"name", tool}, {"arguments", json::object()}}}};
    json res = server.handleMessage(req, profile);
    CHECK_TRUE(res.contains("result"));
    CHECK_TRUE(res["result"].contains("content"));
    return res["result"]["content"][0]["text"].get<std::string>();
}

static bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

TEST_GROUP(ClaudeMcp) {
    McpServer* server = nullptr;
    void setup() override { server = new McpServer(StateStore::getInstance()); }
    void teardown() override {
        delete server;
        StateStore::getInstance().updateClaude(ClaudeStatus());     // leave the singleton clean
    }
    std::string textFor(const ClaudeStatus& s) {
        StateStore::getInstance().updateClaude(s);
        return callTool(*server, "check_claude_usage");
    }
};

TEST(ClaudeMcp, ToolIsListedUnderEveryProfileWithoutArguments) {
    for (const char* profile : {"", "core", "embedded", "network", "mesh", "all"}) {
        json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", json::object()}};
        json res = server->handleMessage(req, profile);
        bool found = false;
        for (const auto& t : res["result"]["tools"]) {
            if (t["name"] == "check_claude_usage") {
                found = true;
                CHECK_TRUE(t["description"].get<std::string>().find("estimated") != std::string::npos);
                CHECK_TRUE(t["inputSchema"]["properties"].empty());
                CHECK_FALSE(t["inputSchema"].contains("required"));
            }
        }
        CHECK_TRUE(found);
    }
}

TEST(ClaudeMcp, CallIsNotRefusedUnderAnyProfile) {
    StateStore::getInstance().updateClaude(makeStatus());
    for (const char* profile : {"core", "embedded", "network", "mesh", "all"}) {
        std::string text = callTool(*server, "check_claude_usage", profile);
        CHECK_TRUE(contains(text, "Claude Code Usage"));
    }
}

TEST(ClaudeMcp, EnterpriseTextShowsCycleAgainstTheLimitAndEveryWindow) {
    std::string text = textFor(makeStatus());
    CHECK_TRUE(contains(text, "### Claude Code Usage (estimated)"));
    CHECK_TRUE(contains(text, "Account `work`"));
    CHECK_TRUE(contains(text, "Enterprise"));
    CHECK_TRUE(contains(text, "enterprise / tier-fixture"));
    CHECK_TRUE(contains(text, "$58.81 of $500.00 (11.8%)"));
    CHECK_TRUE(contains(text, "2026-11-01T00:00:00Z"));
    CHECK_TRUE(contains(text, "$2.50 (5 h)"));
    CHECK_TRUE(contains(text, "$100.00 (7 d)"));
    CHECK_TRUE(contains(text, "$174.54 (30 d)"));
    CHECK_TRUE(contains(text, "1.50M (5 h)"));
    CHECK_TRUE(contains(text, "32.20M (30 d)"));
    CHECK_TRUE(contains(text, "| `claude-sonnet-5-5` | 300 | 20.00M | $51.05 |"));
    CHECK_TRUE(contains(text, "20 models"));
    CHECK_TRUE(contains(text, "2026-10-08 06:00 UTC"));
    CHECK_TRUE(contains(text, "can differ from the billing page"));
    CHECK_FALSE(contains(text, "STALE"));
}

TEST(ClaudeMcp, NoFigureIsEverCalledBilledOrCharged) {
    for (ClaudeTier tier : {ClaudeTier::Enterprise, ClaudeTier::Personal, ClaudeTier::Unknown}) {
        ClaudeStatus s = makeStatus();
        s.accounts[0].tier = tier;
        std::string text = textFor(s);
        CHECK_FALSE(contains(text, "billed"));
        CHECK_FALSE(contains(text, "charged"));
        CHECK_TRUE(contains(text, "est."));
        CHECK_TRUE(contains(text, "Est."));
    }
}

TEST(ClaudeMcp, PersonalAndUnknownTiersAreLabelledByWhatTheDollarsMean) {
    ClaudeStatus s = makeStatus();
    s.accounts[0].tier = ClaudeTier::Personal;
    s.accounts[0].rawSubscriptionType = "personal-fixture";
    s.accounts[0].cycleConfigured = false;
    s.accounts[0].spendLimitUsd = 0.0;
    std::string personal = textFor(s);
    CHECK_TRUE(contains(personal, "Personal"));
    CHECK_TRUE(contains(personal, "personal-fixture / tier-fixture"));
    CHECK_TRUE(contains(personal, "Est. API-equivalent value"));
    CHECK_FALSE(contains(personal, " of $"));                       // no dollar budget for a subscription plan

    s.accounts[0].tier = ClaudeTier::Unknown;
    std::string unknown = textFor(s);
    CHECK_TRUE(contains(unknown, "Unknown"));
    CHECK_TRUE(contains(unknown, "Est. value"));
}

TEST(ClaudeMcp, CycleWithoutALimitShowsTheCycleCostOnly) {
    ClaudeStatus s = makeStatus();
    s.accounts[0].spendLimitUsd = 0.0;
    s.accounts[0].estPctOfLimit = 0.0;
    std::string text = textFor(s);
    CHECK_TRUE(contains(text, "$58.81"));
    CHECK_FALSE(contains(text, "$58.81 of"));
}

TEST(ClaudeMcp, UnpricedModelsStalePricesAndProblemsAreStatedPlainly) {
    ClaudeStatus s = makeStatus();
    s.pricing.stale = true;
    s.pricing.quarantinedCount = 2;
    s.pricing.error = "fetch failed: HTTP 503";
    s.accounts[0].unpricedModels = {"claude-future-9"};
    s.accounts[0].window.unpricedMessages = 3;
    s.accounts[0].errorMessage = "cannot aggregate usage: disk I/O error";
    s.accounts[0].warning = "credentials file not found";
    s.accounts[0].skippedLines = 4;
    std::string text = textFor(s);
    CHECK_TRUE(contains(text, "STALE"));
    CHECK_TRUE(contains(text, "2 models quarantined"));
    CHECK_TRUE(contains(text, "fetch failed: HTTP 503"));
    CHECK_TRUE(contains(text, "`claude-future-9`"));
    CHECK_TRUE(contains(text, "no cost"));
    CHECK_TRUE(contains(text, "cannot aggregate usage: disk I/O error"));
    CHECK_TRUE(contains(text, "credentials file not found"));
    CHECK_TRUE(contains(text, "4 lines skipped"));
}

TEST(ClaudeMcp, EmptyDisabledAndAccountWithoutDataCases) {
    ClaudeStatus off;                                                // enabled = false
    CHECK_TRUE(contains(textFor(off), "disabled"));

    ClaudeStatus none;
    none.enabled = true;
    CHECK_TRUE(contains(textFor(none), "No Claude accounts"));

    ClaudeStatus s = makeStatus();
    s.accounts[0].hasData = false;
    s.accounts[0].errorMessage = "Claude config directory not found: ~/fixture";
    std::string text = textFor(s);
    CHECK_TRUE(contains(text, "no Claude Code usage found"));
    CHECK_TRUE(contains(text, "Claude config directory not found: ~/fixture"));
}

TEST(ClaudeMcp, SeveralAccountsAreAllListed) {
    ClaudeStatus s = makeStatus();
    s.accounts.push_back(makeAccount("personal", ClaudeTier::Personal));
    std::string text = textFor(s);
    CHECK_TRUE(contains(text, "Account `work`"));
    CHECK_TRUE(contains(text, "Account `personal`"));
}

TEST(ClaudeMcp, CombinedStatusIncludesTheClaudeSectionAfterTheOthers) {
    StateStore::getInstance().updateClaude(makeStatus());
    std::string text = callTool(*server, "get_combined_ai_status");
    size_t cursor = text.find("Cursor Usage");
    size_t claude = text.find("Claude Code Usage");
    CHECK_TRUE(cursor != std::string::npos);
    CHECK_TRUE(claude != std::string::npos);
    CHECK_TRUE(claude > cursor);
}

TEST(ClaudeMcp, MarkdownInModelNamesCannotBreakTheTable) {
    ClaudeStatus s = makeStatus();
    s.accounts[0].models[0].model = "claude|evil`name\n# heading";
    std::string text = textFor(s);
    CHECK_FALSE(contains(text, "claude|evil"));
    CHECK_FALSE(contains(text, "\n# heading"));
}

//------------------------------------------------------------------------
// Status JSON
//------------------------------------------------------------------------

TEST_GROUP(ClaudeJson) {};

TEST(ClaudeJson, AggregateStatusCarriesTheClaudeSectionWithExactIntegers) {
    AggregateStatus agg;
    agg.claude = makeStatus();
    json j = agg.toJson();
    CHECK_TRUE(j.contains("claude"));
    const json& a = j["claude"]["accounts"][0];
    STRCMP_EQUAL("work", a["name"].get<std::string>().c_str());
    STRCMP_EQUAL("enterprise", a["tier"].get<std::string>().c_str());
    CHECK_TRUE(a["window"]["est_cost_nano"].get<int64_t>() == 174540000000LL);
    DOUBLES_EQUAL(174.54, a["window"]["est_cost_usd"].get<double>(), 1e-9);
    CHECK_TRUE(a["window"]["total_tokens"].get<int64_t>() == 32200000);
    CHECK_TRUE(a["cycle_configured"].get<bool>());
    DOUBLES_EQUAL(500.0, a["spend_limit_usd"].get<double>(), 1e-9);
    STRCMP_EQUAL("2026-11-01T00:00:00Z", a["cycle_reset_iso"].get<std::string>().c_str());
    CHECK_TRUE(a["models"][0]["est_cost_nano"].get<int64_t>() == 51050000000LL);
    CHECK_TRUE(j["claude"]["pricing"]["version_count"].get<int>() == 2);
    LONGS_EQUAL(1, static_cast<long>(a["daily"].size()));
    CHECK_FALSE(a.dump().find("accessToken") != std::string::npos);
}

TEST(ClaudeJson, DisabledStatusIsStillWellFormed) {
    json j = AggregateStatus().toJson();
    CHECK_TRUE(j["claude"]["enabled"].is_boolean());
    CHECK_FALSE(j["claude"]["enabled"].get<bool>());
    CHECK_TRUE(j["claude"]["accounts"].is_array());
    CHECK_TRUE(j["claude"]["accounts"].empty());
}

//------------------------------------------------------------------------
// History
//------------------------------------------------------------------------

TEST_GROUP(ClaudeHistory) {
    std::string dbPath;
    HistoryStore* store = nullptr;
    void setup() override {
        char tmpl[] = "/tmp/aimon_surfaces_test_XXXXXX";
        char* d = mkdtemp(tmpl);
        CHECK_TRUE(d != nullptr);
        dbPath = std::string(d) + "/history.db";
        store = new HistoryStore();
        CHECK_TRUE(store->open(dbPath));
    }
    void teardown() override {
        delete store;
        std::string cmd = "rm -rf '" + dbPath.substr(0, dbPath.rfind('/')) + "'";
        int rc = std::system(cmd.c_str());
        (void)rc;
    }
    AggregateStatus snapshot(const ClaudeStatus& c, time_t at) {
        AggregateStatus s;
        s.claude = c;
        s.lastUpdated = std::chrono::system_clock::from_time_t(at);
        return s;
    }
    int countFor(const std::string& provider, const std::string& key) {
        int n = 0;
        for (const auto& r : store->queryRecentRecords(1000)) {
            if (r.provider == provider && r.metricKey == key) {
                ++n;
            }
        }
        return n;
    }
};

TEST(ClaudeHistory, RecordsEstimatedCostAndTokensPerAccount) {
    ClaudeStatus c = makeStatus();
    c.accounts.push_back(makeAccount("personal", ClaudeTier::Personal));
    CHECK_TRUE(store->recordSnapshot(snapshot(c, 1791439200)));
    LONGS_EQUAL(1, countFor("claude:work", "est_cost_usd"));
    LONGS_EQUAL(1, countFor("claude:work", "total_tokens"));
    LONGS_EQUAL(1, countFor("claude:personal", "est_cost_usd"));
    bool sawCost = false;
    for (const auto& r : store->queryRecentRecords(1000)) {
        if (r.provider == "claude:work" && r.metricKey == "est_cost_usd") {
            DOUBLES_EQUAL(174.54, r.metricValue, 1e-9);
            DOUBLES_EQUAL(500.0, r.metricLimit, 1e-9);           // the configured limit rides along
            sawCost = true;
        }
        if (r.provider == "claude:work" && r.metricKey == "total_tokens") {
            DOUBLES_EQUAL(32200000.0, r.metricValue, 1e-9);
        }
    }
    CHECK_TRUE(sawCost);
}

TEST(ClaudeHistory, UnchangedValuesAreNotRecordedAgainWithinTheHour) {
    ClaudeStatus c = makeStatus();
    CHECK_TRUE(store->recordSnapshot(snapshot(c, 1791439200)));
    CHECK_TRUE(store->recordSnapshot(snapshot(c, 1791439260)));
    LONGS_EQUAL(1, countFor("claude:work", "est_cost_usd"));
    c.accounts[0].window.costNano += 10000000;                     // $0.01 more
    CHECK_TRUE(store->recordSnapshot(snapshot(c, 1791439320)));
    LONGS_EQUAL(2, countFor("claude:work", "est_cost_usd"));
}

TEST(ClaudeHistory, DisabledOrEmptyClaudeRecordsNothing) {
    CHECK_TRUE(store->recordSnapshot(snapshot(ClaudeStatus(), 1791439200)));
    ClaudeStatus c = makeStatus();
    c.accounts[0].hasData = false;
    CHECK_TRUE(store->recordSnapshot(snapshot(c, 1791439300)));
    LONGS_EQUAL(0, countFor("claude:work", "est_cost_usd"));
}

//------------------------------------------------------------------------
// MQTT
//------------------------------------------------------------------------

static const MqttMessage* find(const std::vector<MqttMessage>& v, const std::string& topic) {
    for (const auto& m : v) {
        if (m.topic == topic) {
            return &m;
        }
    }
    return nullptr;
}

TEST_GROUP(ClaudeMqtt) {};

TEST(ClaudeMqtt, StateMessagesCarryEstimatesAndTier) {
    auto msgs = MqttPublisher::buildClaudeState("aimon", makeStatus());
    const MqttMessage* cost = find(msgs, "aimon/claude/work/est_cost_window_usd/state");
    CHECK_TRUE(cost != nullptr);
    STRCMP_EQUAL("174.54", cost->payload.c_str());
    STRCMP_EQUAL("58.81", find(msgs, "aimon/claude/work/est_cost_cycle_usd/state")->payload.c_str());
    STRCMP_EQUAL("11.8", find(msgs, "aimon/claude/work/est_pct_of_limit/state")->payload.c_str());
    STRCMP_EQUAL("1500000", find(msgs, "aimon/claude/work/tokens_5h/state")->payload.c_str());
    STRCMP_EQUAL("9800000", find(msgs, "aimon/claude/work/tokens_7d/state")->payload.c_str());
    STRCMP_EQUAL("32200000", find(msgs, "aimon/claude/work/tokens_window/state")->payload.c_str());
    STRCMP_EQUAL("enterprise", find(msgs, "aimon/claude/work/tier/state")->payload.c_str());
    json attrs = json::parse(find(msgs, "aimon/claude/work/attributes")->payload);
    STRCMP_EQUAL("enterprise", attrs["raw_subscription_type"].get<std::string>().c_str());
    CHECK_FALSE(attrs["prices_stale"].get<bool>());
    CHECK_TRUE(find(msgs, "aimon/claude/work/attributes")->payload.find("Token") == std::string::npos);
}

TEST(ClaudeMqtt, CycleMetricsOnlyWhenConfiguredAndNothingForEmptyAccounts) {
    ClaudeStatus s = makeStatus();
    s.accounts[0].cycleConfigured = false;
    auto msgs = MqttPublisher::buildClaudeState("aimon", s);
    CHECK_TRUE(find(msgs, "aimon/claude/work/est_cost_cycle_usd/state") == nullptr);
    CHECK_TRUE(find(msgs, "aimon/claude/work/est_pct_of_limit/state") == nullptr);
    CHECK_TRUE(find(msgs, "aimon/claude/work/est_cost_window_usd/state") != nullptr);

    s.accounts[0].spendLimitUsd = 0.0;
    s.accounts[0].cycleConfigured = true;
    CHECK_TRUE(find(MqttPublisher::buildClaudeState("aimon", s), "aimon/claude/work/est_pct_of_limit/state") == nullptr);

    ClaudeStatus off;
    CHECK_TRUE(MqttPublisher::buildClaudeState("aimon", off).empty());
    s = makeStatus();
    s.accounts[0].hasData = false;
    CHECK_TRUE(MqttPublisher::buildClaudeState("aimon", s).empty());
}

TEST(ClaudeMqtt, DiscoveryNamesSayEstimateAndIdsAreUnique) {
    ClaudeStatus s = makeStatus();
    s.accounts.push_back(makeAccount("work-2", ClaudeTier::Personal));
    s.accounts.push_back(makeAccount("work 2", ClaudeTier::Personal));     // same slug as "work-2"
    auto msgs = MqttPublisher::buildClaudeDiscovery("aimon", "homeassistant", s);
    CHECK_TRUE(!msgs.empty());
    std::vector<std::string> ids;
    std::vector<std::string> stateTopics;
    for (const auto& m : msgs) {
        CHECK_TRUE(m.topic.rfind("homeassistant/sensor/aimon/", 0) == 0);
        json p = json::parse(m.payload);
        ids.push_back(p["unique_id"].get<std::string>());
        stateTopics.push_back(p["state_topic"].get<std::string>());
        CHECK_TRUE(p["unique_id"].get<std::string>().rfind("aimon_claude_", 0) == 0);
        STRCMP_EQUAL("aimon_quota_monitor", p["device"]["identifiers"][0].get<std::string>().c_str());
        std::string name = p["name"].get<std::string>();
        if (name.find("Cost") != std::string::npos) {
            CHECK_TRUE(name.find("Est.") != std::string::npos);
            STRCMP_EQUAL("$", p["unit_of_measurement"].get<std::string>().c_str());
        }
    }
    std::vector<std::string> sortedIds = ids;
    std::sort(sortedIds.begin(), sortedIds.end());
    CHECK_TRUE(std::adjacent_find(sortedIds.begin(), sortedIds.end()) == sortedIds.end());   // no duplicates
    std::sort(stateTopics.begin(), stateTopics.end());
    CHECK_TRUE(std::adjacent_find(stateTopics.begin(), stateTopics.end()) == stateTopics.end());
    // every discovered state topic is one that the state messages actually use
    auto state = MqttPublisher::buildClaudeState("aimon", s);
    for (const auto& t : stateTopics) {
        CHECK_TRUE(find(state, t) != nullptr);
    }
}

TEST(ClaudeMqtt, TopicsUseOnlySafeCharactersWhateverTheAccountName) {
    ClaudeStatus s = makeStatus();
    s.accounts[0].name = "we/ird+na#me";
    for (const auto& m : MqttPublisher::buildClaudeState("aimon", s)) {
        std::string tail = m.topic.substr(std::string("aimon/claude/").size());
        std::string account = tail.substr(0, tail.find('/'));
        CHECK_TRUE(account.find_first_of("+#/ ") == std::string::npos);
        CHECK_TRUE(!account.empty());
    }
}

int main(int ac, char** av) {
    // StateStore is a process-wide singleton that keeps the string and vector
    // capacity of whatever status it last held. Give it generous capacity
    // before CppUTest starts counting allocations, so tests that load and then
    // clear a status do not look like leaks.
    ClaudeStatus warm;
    warm.enabled = true;
    warm.pricing.sourceUrl = std::string(512, 'u');
    warm.pricing.error = std::string(512, 'e');
    for (int i = 0; i < 6; ++i) {
        ClaudeAccountStatus a;
        a.name = std::string(64, 'n');
        warm.accounts.push_back(a);
    }
    StateStore::getInstance().updateClaude(warm);
    StateStore::getInstance().updateClaude(ClaudeStatus());
    return CommandLineTestRunner::RunAllTests(ac, av);
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
