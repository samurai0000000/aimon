/*
 * TestClaudeAccount.cxx
 *
 * Qualification tests for Claude tier detection (credentials handling) and
 * for the Claude configuration: defaults, validation, JSON and libconfig
 * round trips. Fixtures use fake tier strings and fake tokens only.
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ClaudeAccount.hxx"
#include "ConfigManager.hxx"
#include "Models.hxx"

// CppUTest's memory leak macros redefine `new`; its headers must come last.
#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/TestHarness.h>

using namespace aimon;

static const char* const kSecretAccess = "SECRET-ACCESS-TOKEN-XYZ";
static const char* const kSecretRefresh = "SECRET-REFRESH-TOKEN-XYZ";

static std::string makeTempDir() {
    char tmpl[] = "/tmp/aimon_account_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    return dir ? std::string(dir) : std::string();
}

static void writeFile(const std::string& path, const std::string& data, mode_t mode = 0600) {
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << data;
    }
    chmod(path.c_str(), mode);
}

static std::string credentials(const std::string& subscription, const std::string& rateTier = "tier-fixture") {
    nlohmann::json j = {{"claudeAiOauth", {{"accessToken", kSecretAccess}, {"refreshToken", kSecretRefresh},
                                           {"expiresAt", 1}, {"scopes", {"user:profile"}},
                                           {"subscriptionType", subscription}, {"rateLimitTier", rateTier}}}};
    return j.dump();
}

static void assertNoSecrets(const ClaudeTierInfo& info) {
    const std::string all = info.rawSubscriptionType + "|" + info.rawRateLimitTier + "|" + info.warning;
    CHECK_TRUE(all.find("SECRET") == std::string::npos);
    CHECK_TRUE(all.find("accessToken") == std::string::npos);
}

TEST_GROUP(ClaudeTier) {
    std::string dir;
    void setup() { dir = makeTempDir(); CHECK_TRUE(!dir.empty()); }
    void teardown() {
        std::string cmd = "rm -rf '" + dir + "'";
        int rc = std::system(cmd.c_str());
        (void)rc;
    }
    ClaudeTierInfo detectWith(const std::string& body, mode_t mode = 0600) {
        writeFile(dir + "/.credentials.json", body, mode);
        ClaudeTierInfo info = detectClaudeTier(dir);
        assertNoSecrets(info);
        return info;
    }
};

TEST(ClaudeTier, EnterpriseIsDetectedAndTokensNeverAppear) {
    ClaudeTierInfo info = detectWith(credentials("enterprise", "default_fixture"));
    CHECK_TRUE(info.tier == ClaudeTier::Enterprise);
    STRCMP_EQUAL("enterprise", info.rawSubscriptionType.c_str());
    STRCMP_EQUAL("default_fixture", info.rawRateLimitTier.c_str());
    STRCMP_EQUAL("", info.warning.c_str());
}

TEST(ClaudeTier, EnterpriseMatchIsCaseInsensitive) {
    CHECK_TRUE(detectWith(credentials("Enterprise")).tier == ClaudeTier::Enterprise);
    CHECK_TRUE(detectWith(credentials("ENTERPRISE")).tier == ClaudeTier::Enterprise);
}

TEST(ClaudeTier, AnyOtherSubscriptionIsPersonalWithItsRawString) {
    ClaudeTierInfo info = detectWith(credentials("personal-fixture-plan"));
    CHECK_TRUE(info.tier == ClaudeTier::Personal);
    STRCMP_EQUAL("personal-fixture-plan", info.rawSubscriptionType.c_str());
}

TEST(ClaudeTier, EmptyOrAbsentSubscriptionIsUnknown) {
    ClaudeTierInfo empty = detectWith(credentials(""));
    CHECK_TRUE(empty.tier == ClaudeTier::Unknown);
    CHECK_TRUE(!empty.warning.empty());
    ClaudeTierInfo absent = detectWith("{\"claudeAiOauth\":{\"accessToken\":\"SECRET-ACCESS-TOKEN-XYZ\"}}");
    CHECK_TRUE(absent.tier == ClaudeTier::Unknown);
    CHECK_TRUE(!absent.warning.empty());
}

TEST(ClaudeTier, MissingFileIsUnknownWithAWarning) {
    ClaudeTierInfo info = detectClaudeTier(dir + "/does-not-exist");
    CHECK_TRUE(info.tier == ClaudeTier::Unknown);
    CHECK_TRUE(!info.warning.empty());
    ClaudeTierInfo noFile = detectClaudeTier(dir);
    CHECK_TRUE(noFile.tier == ClaudeTier::Unknown);
}

TEST(ClaudeTier, MalformedAndWronglyShapedFilesAreUnknownNotFatal) {
    const char* bodies[] = {
        "", "{", "not json at all", "[]", "null", "{\"claudeAiOauth\":\"SECRET-ACCESS-TOKEN-XYZ\"}",
        "{\"claudeAiOauth\":[]}", "{\"claudeAiOauth\":{\"subscriptionType\":5}}",
        "{\"claudeAiOauth\":{\"subscriptionType\":null,\"rateLimitTier\":{}}}",
        "{\"other\":{\"subscriptionType\":\"enterprise\"}}"};
    for (const char* b : bodies) {
        ClaudeTierInfo info = detectWith(b);
        CHECK_TRUE(info.tier == ClaudeTier::Unknown);
        CHECK_TRUE(!info.warning.empty());
    }
}

TEST(ClaudeTier, OversizedCredentialsFileIsRejected) {
    std::string body = credentials("enterprise");
    body.insert(body.size() - 1, std::string(70 * 1024, ' '));
    ClaudeTierInfo info = detectWith(body);
    CHECK_TRUE(info.tier == ClaudeTier::Unknown);
    CHECK_TRUE(info.warning.find("large") != std::string::npos);
}

TEST(ClaudeTier, GroupOrWorldReadableFileIsUnknownWithAWarning) {
    ClaudeTierInfo group = detectWith(credentials("enterprise"), 0640);
    CHECK_TRUE(group.tier == ClaudeTier::Unknown);
    CHECK_TRUE(group.warning.find("600") != std::string::npos);
    ClaudeTierInfo world = detectWith(credentials("enterprise"), 0604);
    CHECK_TRUE(world.tier == ClaudeTier::Unknown);
    ClaudeTierInfo owner = detectWith(credentials("enterprise"), 0400);
    CHECK_TRUE(owner.tier == ClaudeTier::Enterprise);   // stricter than 0600 is fine
}

TEST(ClaudeTier, UnprintableOrOverlongStringsAreRejected) {
    CHECK_TRUE(detectWith(credentials(std::string("enter\x01prise"))).tier == ClaudeTier::Unknown);
    CHECK_TRUE(detectWith(credentials(std::string(200, 'a'))).tier == ClaudeTier::Unknown);
    CHECK_TRUE(detectWith(credentials("enterprise", std::string(200, 'b'))).tier == ClaudeTier::Unknown);
    CHECK_TRUE(detectWith(credentials("enter\xc3\xa9prise")).tier == ClaudeTier::Unknown);   // non-ASCII
}

TEST(ClaudeTier, TildePathsExpandAgainstHome) {
    const char* oldHome = std::getenv("HOME");
    std::string saved = oldHome ? oldHome : "";
    setenv("HOME", dir.c_str(), 1);
    mkdir((dir + "/cfg").c_str(), 0700);
    writeFile(dir + "/cfg/.credentials.json", credentials("enterprise"));
    ClaudeTierInfo info = detectClaudeTier("~/cfg");
    if (oldHome) setenv("HOME", saved.c_str(), 1); else unsetenv("HOME");
    CHECK_TRUE(info.tier == ClaudeTier::Enterprise);
}

TEST(ClaudeTier, FileIsNeverModified) {
    std::string path = dir + "/.credentials.json";
    writeFile(path, credentials("enterprise"));
    struct stat before;
    stat(path.c_str(), &before);
    detectClaudeTier(dir);
    struct stat after;
    stat(path.c_str(), &after);
    LONGS_EQUAL(static_cast<long>(before.st_mtime), static_cast<long>(after.st_mtime));
    LONGS_EQUAL(static_cast<long>(before.st_size), static_cast<long>(after.st_size));
    LONGS_EQUAL(0600, static_cast<long>(after.st_mode & 0777));
}

//------------------------------------------------------------------------
// Configuration
//------------------------------------------------------------------------

TEST_GROUP(ClaudeConfigGroup) {
    std::string dir;
    std::string savedEnv;
    bool hadEnv = false;
    void setup() {
        dir = makeTempDir();
        const char* e = std::getenv("CLAUDE_CONFIG_DIR");
        hadEnv = e != nullptr;
        savedEnv = e ? e : "";
        unsetenv("CLAUDE_CONFIG_DIR");
    }
    void teardown() {
        if (hadEnv) setenv("CLAUDE_CONFIG_DIR", savedEnv.c_str(), 1); else unsetenv("CLAUDE_CONFIG_DIR");
        std::string cmd = "rm -rf '" + dir + "'";
        int rc = std::system(cmd.c_str());
        (void)rc;
    }
    static ClaudeAccountConfig acct(const std::string& name, const std::string& cfgDir) {
        ClaudeAccountConfig a;
        a.name = name;
        a.configDir = cfgDir;
        return a;
    }
    bool valid(const ClaudeConfig& c) {
        std::string err;
        bool ok = c.validate(err);
        CHECK_EQUAL(ok, err.empty());
        return ok;
    }
};

TEST(ClaudeConfigGroup, DefaultAccountUsesTheEnvironmentThenHome) {
    ClaudeConfig c;
    auto accounts = c.resolvedAccounts();
    LONGS_EQUAL(1, static_cast<long>(accounts.size()));
    STRCMP_EQUAL("default", accounts[0].name.c_str());
    STRCMP_EQUAL("~/.claude", accounts[0].configDir.c_str());
    setenv("CLAUDE_CONFIG_DIR", "/tmp/aimon-claude-env-fixture", 1);
    STRCMP_EQUAL("/tmp/aimon-claude-env-fixture", c.resolvedAccounts()[0].configDir.c_str());
    // explicit accounts ignore the environment variable
    c.accounts.push_back(acct("work", "/tmp/aimon-explicit-fixture"));
    STRCMP_EQUAL("/tmp/aimon-explicit-fixture", c.resolvedAccounts()[0].configDir.c_str());
    LONGS_EQUAL(1, static_cast<long>(c.resolvedAccounts().size()));
}

TEST(ClaudeConfigGroup, ValidationAcceptsSensibleConfigurations) {
    ClaudeConfig c;
    CHECK_TRUE(valid(c));
    c.accounts = {acct("work", "~/a"), acct("personal", "~/b")};
    c.accounts[0].spendLimitUsd = 500;
    c.accounts[0].cycleResetDay = 1;
    c.accounts[1].cycleResetDay = 28;
    c.pricingUrl = "https://example.invalid/pricing.md";
    CHECK_TRUE(valid(c));
}

TEST(ClaudeConfigGroup, ValidationRejectsBadValues) {
    auto base = []() {
        ClaudeConfig c;
        c.accounts = {acct("work", "~/a")};
        return c;
    };
    ClaudeConfig c;
    c = base(); c.windowDays = 0;            CHECK_FALSE(valid(c));
    c = base(); c.windowDays = 366;          CHECK_FALSE(valid(c));
    c = base(); c.retentionDays = 29;        CHECK_FALSE(valid(c));
    c = base(); c.retentionDays = 3651;      CHECK_FALSE(valid(c));
    c = base(); c.pricingRefreshHours = 0;   CHECK_FALSE(valid(c));
    c = base(); c.pricingRefreshHours = 169; CHECK_FALSE(valid(c));
    c = base(); c.pricingUrl = "http://example.invalid/p.md"; CHECK_FALSE(valid(c));
    c = base(); c.accounts[0].name = "";                CHECK_FALSE(valid(c));
    c = base(); c.accounts[0].name = "has space";       CHECK_FALSE(valid(c));
    c = base(); c.accounts[0].name = std::string(65, 'a'); CHECK_FALSE(valid(c));
    c = base(); c.accounts[0].configDir = "";           CHECK_FALSE(valid(c));
    c = base(); c.accounts[0].spendLimitUsd = -1;       CHECK_FALSE(valid(c));
    c = base(); c.accounts[0].spendLimitUsd = std::nan(""); CHECK_FALSE(valid(c));
    c = base(); c.accounts[0].spendLimitUsd = 1e12;     CHECK_FALSE(valid(c));
    c = base(); c.accounts[0].cycleResetDay = -1;       CHECK_FALSE(valid(c));
    c = base(); c.accounts[0].cycleResetDay = 29;       CHECK_FALSE(valid(c));
}

TEST(ClaudeConfigGroup, DuplicateNamesAndSharedDirectoriesAreRejected) {
    ClaudeConfig c;
    c.accounts = {acct("work", "~/a"), acct("work", "~/b")};
    CHECK_FALSE(valid(c));
    c.accounts = {acct("work", "~/a"), acct("personal", "~/a")};
    CHECK_FALSE(valid(c));
    c.accounts = {acct("work", "~/a"), acct("personal", "~/a/")};
    CHECK_FALSE(valid(c));
    c.accounts = {acct("work", "~/a"), acct("personal", "~/x/../a")};
    CHECK_FALSE(valid(c));
    c.accounts = {acct("work", "~/a"), acct("personal", "~/ab")};
    CHECK_TRUE(valid(c));
}

TEST(ClaudeConfigGroup, JsonRoundTripKeepsEveryField) {
    AimonConfig a;
    a.claude.enabled = false;
    a.claude.windowDays = 14;
    a.claude.retentionDays = 90;
    a.claude.pricingUrl = "https://example.invalid/p.md";
    a.claude.pricingRefreshHours = 6;
    a.claude.accounts = {acct("work", "~/a"), acct("personal", "~/b")};
    a.claude.accounts[0].spendLimitUsd = 500.5;
    a.claude.accounts[0].cycleResetDay = 15;

    AimonConfig b;
    b.fromJson(a.toJson());
    CHECK_FALSE(b.claude.enabled);
    LONGS_EQUAL(14, b.claude.windowDays);
    LONGS_EQUAL(90, b.claude.retentionDays);
    STRCMP_EQUAL("https://example.invalid/p.md", b.claude.pricingUrl.c_str());
    LONGS_EQUAL(6, b.claude.pricingRefreshHours);
    LONGS_EQUAL(2, static_cast<long>(b.claude.accounts.size()));
    STRCMP_EQUAL("work", b.claude.accounts[0].name.c_str());
    STRCMP_EQUAL("~/a", b.claude.accounts[0].configDir.c_str());
    DOUBLES_EQUAL(500.5, b.claude.accounts[0].spendLimitUsd, 1e-9);
    LONGS_EQUAL(15, b.claude.accounts[0].cycleResetDay);
    STRCMP_EQUAL("personal", b.claude.accounts[1].name.c_str());
}

TEST(ClaudeConfigGroup, JsonWithWrongTypesIsIgnoredNotFatal) {
    AimonConfig a;
    nlohmann::json j = {{"claude", {{"enabled", "yes"}, {"window_days", "x"}, {"retention_days", 1.5},
                                    {"pricing_url", 7}, {"accounts", {1, "a", {{"name", 5}, {"config_dir", {}},
                                                                                 {"spend_limit_usd", "5"},
                                                                                 {"cycle_reset_day", "x"}}}}}}};
    a.fromJson(j);
    CHECK_TRUE(a.claude.enabled);
    LONGS_EQUAL(30, a.claude.windowDays);
    LONGS_EQUAL(400, a.claude.retentionDays);
    LONGS_EQUAL(1, static_cast<long>(a.claude.accounts.size()));   // the object entry only, with defaults
    STRCMP_EQUAL("", a.claude.accounts[0].name.c_str());
    std::string err;
    CHECK_FALSE(a.claude.validate(err));                            // and validation catches it
    AimonConfig b;
    b.fromJson(nlohmann::json{{"claude", 5}});                      // non-object section ignored
    LONGS_EQUAL(30, b.claude.windowDays);
}

TEST(ClaudeConfigGroup, LibConfigRoundTripAndIntegerSpendLimit) {
    ConfigManager cm;
    AimonConfig& cfg = cm.getConfig();
    cfg.claude.windowDays = 21;
    cfg.claude.retentionDays = 120;
    cfg.claude.pricingRefreshHours = 12;
    cfg.claude.accounts = {acct("work", "~/a"), acct("personal", "~/b")};
    cfg.claude.accounts[0].spendLimitUsd = 500;
    cfg.claude.accounts[0].cycleResetDay = 1;
    std::string path = dir + "/aimon.cfg";
    CHECK_TRUE(cm.saveLibConfig(path));

    ConfigManager reloaded;
    CHECK_TRUE(reloaded.loadLibConfig(path));
    const ClaudeConfig& c = reloaded.getConfig().claude;
    LONGS_EQUAL(21, c.windowDays);
    LONGS_EQUAL(120, c.retentionDays);
    LONGS_EQUAL(12, c.pricingRefreshHours);
    LONGS_EQUAL(2, static_cast<long>(c.accounts.size()));
    STRCMP_EQUAL("personal", c.accounts[1].name.c_str());
    DOUBLES_EQUAL(500.0, c.accounts[0].spendLimitUsd, 1e-9);
    LONGS_EQUAL(1, c.accounts[0].cycleResetDay);

    // a hand-written file may use an integer for the spend limit
    writeFile(dir + "/hand.cfg",
              "claude = { accounts = ( { name = \"work\"; config_dir = \"~/a\"; spend_limit_usd = 500; "
              "cycle_reset_day = 1; } ); };\n");
    ConfigManager hand;
    CHECK_TRUE(hand.loadLibConfig(dir + "/hand.cfg"));
    DOUBLES_EQUAL(500.0, hand.getConfig().claude.accounts[0].spendLimitUsd, 1e-9);
}

int main(int ac, char** av) {
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
