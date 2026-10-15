/*
 * TestLibconfigParser.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <unistd.h>
#include "ConfigManager.hxx"
#include <CppUTest/TestHarness.h>
#include <CppUTest/CommandLineTestRunner.h>

namespace fs = std::filesystem;
using namespace aimon;

TEST_GROUP(LibconfigParser_Unit) {
    std::string _tempDir;
    std::string _validCfgPath;
    std::string _syntaxErrorCfgPath;
    std::string _typeMismatchCfgPath;
    std::string _minimalCfgPath;

    void setup() override {
        _tempDir = "/tmp/aimon_test_unit_" + std::to_string(getpid());
        fs::create_directories(_tempDir);
        _validCfgPath = _tempDir + "/valid.cfg";
        _syntaxErrorCfgPath = _tempDir + "/syntax_error.cfg";
        _typeMismatchCfgPath = _tempDir + "/type_mismatch.cfg";
        _minimalCfgPath = _tempDir + "/minimal.cfg";

        std::ofstream validFile(_validCfgPath);
        validFile << "polling: {\n"
                  << "    base_interval_sec = 240;\n"
                  << "    idle_interval_sec = 720;\n"
                  << "    active_interval_sec = 90;\n"
                  << "};\n"
                  << "web: {\n"
                  << "    host = \"127.0.0.1\";\n"
                  << "    port = 3883;\n"
                  << "    endpoints_enabled = true;\n"
                  << "};\n"
                  << "mqtt: {\n"
                  << "    enabled = true;\n"
                  << "    broker = \"mqtt.local\";\n"
                  << "    port = 1884;\n"
                  << "    username = \"u1\";\n"
                  << "    password = \"p1\";\n"
                  << "    topic_prefix = \"aimon_test\";\n"
                  << "    discovery_prefix = \"ha_test\";\n"
                  << "    retain = false;\n"
                  << "};\n"
                  << "gateway: {\n"
                  << "    enabled = true;\n"
                  << "    host = \"127.0.0.1\";\n"
                  << "    port = 3885;\n"
                  << "};\n";
        validFile.close();

        std::ofstream syntaxErrFile(_syntaxErrorCfgPath);
        syntaxErrFile << "web: {\n"
                      << "    host = \"127.0.0.1\";\n"
                      << "    port = ;\n"
                      << "};\n";
        syntaxErrFile.close();

        std::ofstream typeMismatchFile(_typeMismatchCfgPath);
        typeMismatchFile << "web: {\n"
                         << "    port = \"not_a_number\";\n"
                         << "};\n";
        typeMismatchFile.close();

        std::ofstream minimalFile(_minimalCfgPath);
        minimalFile << "web: {\n"
                    << "    port = 8080;\n"
                    << "};\n";
        minimalFile.close();
    }

    void teardown() override {
        unsetenv("CURSOR_ACCESS_TOKEN");
        unsetenv("ANTIGRAVITY_CSRF_TOKEN");
        unsetenv("MQTT_BROKER");
        if (fs::exists(_tempDir)) {
            fs::remove_all(_tempDir);
        }
    }
};

TEST(LibconfigParser_Unit, NominalParsing) {
    ConfigManager cm;
    bool success = cm.loadLibConfig(_validCfgPath);
    CHECK_TRUE(success);
    CHECK_EQUAL("", cm.getLastErrorMessage());

    const AimonConfig& cfg = cm.getConfig();
    CHECK_EQUAL(240, cfg.polling.baseIntervalSec);
    CHECK_EQUAL(720, cfg.polling.idleIntervalSec);
    CHECK_EQUAL(90, cfg.polling.activeIntervalSec);

    CHECK_EQUAL("127.0.0.1", cfg.web.host);
    CHECK_EQUAL(3883, cfg.web.port);
    CHECK_TRUE(cfg.web.endpointsEnabled);

    CHECK_TRUE(cfg.mqtt.enabled);
    CHECK_EQUAL("mqtt.local", cfg.mqtt.broker);
    CHECK_EQUAL(1884, cfg.mqtt.port);
    CHECK_EQUAL("u1", cfg.mqtt.username);
    CHECK_EQUAL("p1", cfg.mqtt.password);
    CHECK_EQUAL("aimon_test", cfg.mqtt.topicPrefix);
    CHECK_EQUAL("ha_test", cfg.mqtt.discoveryPrefix);
    CHECK_FALSE(cfg.mqtt.retain);

    CHECK_TRUE(cfg.gateway.enabled);
    CHECK_EQUAL("127.0.0.1", cfg.gateway.host);
    CHECK_EQUAL(3885, cfg.gateway.port);

    nlohmann::json j = cfg.toJson();
    CHECK_FALSE(j.contains("supervisor"));
    CHECK_FALSE(j.contains("services"));
    CHECK_FALSE(j.contains("gemini"));
}

TEST(LibconfigParser_Unit, FaultSyntaxError) {
    ConfigManager cm;
    bool success = cm.loadLibConfig(_syntaxErrorCfgPath);
    CHECK_FALSE(success);
    std::string err = cm.getLastErrorMessage();
    CHECK_TRUE(!err.empty());
    CHECK_TRUE(err.find("Parse error in ") != std::string::npos);
    // Line 2 or 3 where syntax error is located
    CHECK_TRUE(err.find(":2 -") != std::string::npos || err.find(":3 -") != std::string::npos);
}

TEST(LibconfigParser_Unit, FaultTypeMismatch) {
    ConfigManager cm;
    bool success = cm.loadLibConfig(_typeMismatchCfgPath);
    CHECK_FALSE(success);
    std::string err = cm.getLastErrorMessage();
    CHECK_TRUE(!err.empty());
    CHECK_TRUE(err.find("Setting type mismatch") != std::string::npos);
    CHECK_TRUE(err.find("port") != std::string::npos);
}

TEST(LibconfigParser_Unit, MissingRequiredFieldsFallback) {
    ConfigManager cm;
    bool success = cm.loadLibConfig(_minimalCfgPath);
    CHECK_TRUE(success);

    const AimonConfig& cfg = cm.getConfig();
    // Default fallback values
    CHECK_EQUAL(300, cfg.polling.baseIntervalSec);
    CHECK_EQUAL(600, cfg.polling.idleIntervalSec);
    CHECK_EQUAL(120, cfg.polling.activeIntervalSec);
    CHECK_EQUAL(3885, cfg.gateway.port);
    CHECK_EQUAL(8080, cfg.web.port);
}

TEST(LibconfigParser_Unit, EnvVariableFallback) {
    setenv("CURSOR_ACCESS_TOKEN", "secret-cursor-token-777", 1);
    setenv("ANTIGRAVITY_CSRF_TOKEN", "secret-ag-csrf-888", 1);
    setenv("MQTT_BROKER", "env.mqtt.local", 1);
    ConfigManager cm;
    bool success = cm.loadLibConfig(_validCfgPath);
    CHECK_TRUE(success);
    CHECK_EQUAL("secret-cursor-token-777", cm.getConfig().cursor.accessToken);
    CHECK_EQUAL("secret-ag-csrf-888", cm.getConfig().antigravity.csrfToken);
    CHECK_EQUAL("env.mqtt.local", cm.getConfig().mqtt.broker);
}

TEST_GROUP(LibconfigParser_Integrated) {
    std::string _tempDir;

    void setup() override {
        _tempDir = "/tmp/aimon_test_integrated_" + std::to_string(getpid());
        fs::create_directories(_tempDir);
    }

    void teardown() override {
        if (fs::exists(_tempDir)) {
            fs::remove_all(_tempDir);
        }
    }
};

TEST(LibconfigParser_Integrated, RoundTripFidelity) {
    std::string path1 = _tempDir + "/config1.cfg";
    std::string path2 = _tempDir + "/config2.cfg";

    ConfigManager cm1;
    AimonConfig& cfg1 = cm1.getConfig();
    cfg1.web.host = "127.0.0.1";
    cfg1.web.port = 9999;
    cfg1.web.endpointsEnabled = true;
    cfg1.polling.baseIntervalSec = 180;
    cfg1.gateway.host = "127.0.0.1";
    cfg1.gateway.port = 4885;

    ClaudeAccountConfig acct1;
    acct1.name = "acct-alpha";
    acct1.configDir = "/tmp/claude_alpha";
    acct1.spendLimitUsd = 250.5;
    acct1.cycleResetDay = 15;
    cfg1.claude.accounts.push_back(acct1);

    bool saveOk = cm1.saveLibConfig(path1);
    CHECK_TRUE(saveOk);

    ConfigManager cm2;
    bool loadOk = cm2.loadLibConfig(path1);
    CHECK_TRUE(loadOk);

    const AimonConfig& cfg2 = cm2.getConfig();
    CHECK_EQUAL(cfg1.web.host, cfg2.web.host);
    CHECK_EQUAL(cfg1.web.port, cfg2.web.port);
    CHECK_EQUAL(cfg1.web.endpointsEnabled, cfg2.web.endpointsEnabled);
    CHECK_EQUAL(cfg1.polling.baseIntervalSec, cfg2.polling.baseIntervalSec);
    CHECK_EQUAL(cfg1.gateway.port, cfg2.gateway.port);
    CHECK_EQUAL(1, static_cast<int>(cfg2.claude.accounts.size()));
    CHECK_EQUAL("acct-alpha", cfg2.claude.accounts[0].name);
    CHECK_EQUAL("/tmp/claude_alpha", cfg2.claude.accounts[0].configDir);
    DOUBLES_EQUAL(250.5, cfg2.claude.accounts[0].spendLimitUsd, 0.001);
    CHECK_EQUAL(15, cfg2.claude.accounts[0].cycleResetDay);

    nlohmann::json j2 = cfg2.toJson();
    CHECK_FALSE(j2.contains("supervisor"));
    CHECK_FALSE(j2.contains("services"));
    CHECK_FALSE(j2.contains("gemini"));

    // Save from cm2 to path2 and re-verify
    bool save2Ok = cm2.saveLibConfig(path2);
    CHECK_TRUE(save2Ok);

    ConfigManager cm3;
    bool load2Ok = cm3.loadLibConfig(path2);
    CHECK_TRUE(load2Ok);
    CHECK_EQUAL(cfg2.web.port, cm3.getConfig().web.port);
}

TEST(LibconfigParser_Integrated, LegacyConfigKeysIgnoredGracefully) {
    std::string legacyJsonPath = _tempDir + "/legacy.json";
    nlohmann::json legacy = {
        {"web", {{"host", "127.0.0.1"}, {"port", 3883}}},
        {"supervisor", {{"enabled", true}, {"safe_mode_port", 3889}}},
        {"services", nlohmann::json::array({{{"name", "meshmon"}, {"port", 3884}}})},
        {"gemini", {{"enabled", true}, {"model", "gemini-2.5-flash"}}}
    };
    {
        std::ofstream out(legacyJsonPath);
        out << legacy.dump(2);
    }

    ConfigManager cm;
    CHECK_TRUE(cm.load(legacyJsonPath));
    CHECK_EQUAL("127.0.0.1", cm.getConfig().web.host);
    CHECK_EQUAL(3883, cm.getConfig().web.port);
    nlohmann::json roundTrip = cm.getConfig().toJson();
    CHECK_FALSE(roundTrip.contains("supervisor"));
    CHECK_FALSE(roundTrip.contains("services"));
    CHECK_FALSE(roundTrip.contains("gemini"));
}

TEST(LibconfigParser_Integrated, InsightsConfigLibconfigAndJsonParity) {
    std::string cfgPath = _tempDir + "/insights_test.cfg";
    std::string jsonPath = _tempDir + "/insights_test.json";

    ConfigManager cm1;
    AimonConfig& c1 = cm1.getConfig();
    c1.insights.enabled = false;
    c1.insights.dbPath = "/tmp/custom_insights.db";
    c1.insights.retentionDays = 180;
    c1.insights.refreshIntervalSec = 120;
    c1.insights.longContextThresholdTokens = 175000;
    c1.insights.compactContextThresholdTokens = 110000;
    c1.insights.compactContinueRequests = 20;
    c1.insights.subagentShareThreshold = 0.45;
    c1.insights.longSessionHours = 12;
    c1.insights.cacheEfficiencyThreshold = 0.60;
    c1.insights.cacheEfficiencyMinRequests = 25;
    c1.insights.errorRetryStreakThreshold = 4;
    c1.insights.idleGapSeconds = 600;
    c1.insights.burnRateAlertFraction = 0.85;
    c1.insights.shortPromptMaxProseChars = 75;
    c1.insights.dripPromptMaxProseChars = 90;
    c1.insights.dripPromptsPerHour = 30;
    c1.insights.quickRepromptSeconds = 90;
    c1.insights.coachingMinSampleSize = 15;

    CHECK_TRUE(cm1.saveLibConfig(cfgPath));
    ConfigManager cmLib;
    CHECK_TRUE(cmLib.loadLibConfig(cfgPath));
    const InsightsConfig& libIns = cmLib.getConfig().insights;
    CHECK_FALSE(libIns.enabled);
    CHECK_EQUAL("/tmp/custom_insights.db", libIns.dbPath);
    CHECK_EQUAL(180, libIns.retentionDays);
    CHECK_EQUAL(120, libIns.refreshIntervalSec);
    CHECK_EQUAL(175000, libIns.longContextThresholdTokens);
    CHECK_EQUAL(110000, libIns.compactContextThresholdTokens);
    CHECK_EQUAL(20, libIns.compactContinueRequests);
    DOUBLES_EQUAL(0.45, libIns.subagentShareThreshold, 0.001);
    CHECK_EQUAL(12, libIns.longSessionHours);
    DOUBLES_EQUAL(0.60, libIns.cacheEfficiencyThreshold, 0.001);
    CHECK_EQUAL(25, libIns.cacheEfficiencyMinRequests);
    CHECK_EQUAL(4, libIns.errorRetryStreakThreshold);
    CHECK_EQUAL(600, libIns.idleGapSeconds);
    DOUBLES_EQUAL(0.85, libIns.burnRateAlertFraction, 0.001);
    CHECK_EQUAL(75, libIns.shortPromptMaxProseChars);
    CHECK_EQUAL(90, libIns.dripPromptMaxProseChars);
    CHECK_EQUAL(30, libIns.dripPromptsPerHour);
    CHECK_EQUAL(90, libIns.quickRepromptSeconds);
    CHECK_EQUAL(15, libIns.coachingMinSampleSize);

    CHECK_TRUE(cm1.save(jsonPath));
    ConfigManager cmJson;
    CHECK_TRUE(cmJson.load(jsonPath));
    const InsightsConfig& jsIns = cmJson.getConfig().insights;
    CHECK_FALSE(jsIns.enabled);
    CHECK_EQUAL("/tmp/custom_insights.db", jsIns.dbPath);
    CHECK_EQUAL(180, jsIns.retentionDays);
    CHECK_EQUAL(120, jsIns.refreshIntervalSec);
    CHECK_EQUAL(175000, jsIns.longContextThresholdTokens);
    CHECK_EQUAL(110000, jsIns.compactContextThresholdTokens);
    CHECK_EQUAL(20, jsIns.compactContinueRequests);
    DOUBLES_EQUAL(0.45, jsIns.subagentShareThreshold, 0.001);
    CHECK_EQUAL(12, jsIns.longSessionHours);
    DOUBLES_EQUAL(0.60, jsIns.cacheEfficiencyThreshold, 0.001);
    CHECK_EQUAL(25, jsIns.cacheEfficiencyMinRequests);
    CHECK_EQUAL(4, jsIns.errorRetryStreakThreshold);
    CHECK_EQUAL(600, jsIns.idleGapSeconds);
    DOUBLES_EQUAL(0.85, jsIns.burnRateAlertFraction, 0.001);
    CHECK_EQUAL(75, jsIns.shortPromptMaxProseChars);
    CHECK_EQUAL(90, jsIns.dripPromptMaxProseChars);
    CHECK_EQUAL(30, jsIns.dripPromptsPerHour);
    CHECK_EQUAL(90, jsIns.quickRepromptSeconds);
    CHECK_EQUAL(15, jsIns.coachingMinSampleSize);
}

TEST(LibconfigParser_Integrated, EdaConfigLibconfigJsonAndValidationParity) {
    std::string cfgPath = _tempDir + "/eda_test.cfg";
    std::string jsonPath = _tempDir + "/eda_test.json";

    ConfigManager cm1;
    AimonConfig& c1 = cm1.getConfig();
    c1.eda.enabled = true;
    c1.eda.mode = "bidirectional";
    c1.eda.host = "eda-host.local";
    c1.eda.port = 3892;
    c1.eda.pluginPort = 3893;
    c1.eda.authSecret = "test-secret-token";
    c1.eda.healthPollIntervalSec = 45;
    c1.eda.timeoutMs = 75000;
    c1.eda.maxOutputBytes = 8388608;
    c1.eda.requireApprovalForExports = true;
    c1.eda.linuxPathPrefix = "~/work";
    c1.eda.windowsPathPrefix = "Z:\\";

    c1.eda.accelerators.enabled = true;
    c1.eda.accelerators.saPlacementIterations = 25000;
    c1.eda.accelerators.autorouteMaxRipupPasses = 30;
    c1.eda.accelerators.autorouteGridMm = 0.20;
    c1.eda.accelerators.preferJlcpcbBasicESeries = false;
    c1.eda.accelerators.defaultCopperWeightOz = 2.0;
    c1.eda.accelerators.defaultFilamentMaterial = "PA-CF";
    c1.eda.accelerators.defaultGearPressureAngleDeg = 20.0;

    c1.eda.easyeda.enabled = true;
    c1.eda.easyeda.host = "easyeda.local";
    c1.eda.easyeda.port = 3901;
    c1.eda.easyeda.pluginPort = 3902;
    c1.eda.easyeda.executionMode = "live_gui";
    c1.eda.easyeda.customBinaryPath = "C:\\Program Files\\EasyEDA-Pro\\EasyEDA-Pro.exe";
    c1.eda.easyeda.projectsDir = "~/work/pcb/easyeda_custom";
    c1.eda.easyeda.preferJlcpcbBasicParts = false;
    c1.eda.easyeda.minLcscStock = 250;
    c1.eda.easyeda.autoDrcAfterEdit = false;
    c1.eda.easyeda.jlcpcbLayerCount = 4;
    c1.eda.easyeda.timeoutMs = 90000;

    c1.eda.kicad.enabled = true;
    c1.eda.kicad.host = "";
    c1.eda.kicad.port = 0;
    c1.eda.kicad.pluginPort = 0;
    c1.eda.kicad.executionMode = "headless_cli";
    c1.eda.kicad.customBinaryPath = "C:\\Program Files\\KiCad\\8.0\\bin\\kicad-cli.exe";
    c1.eda.kicad.symbolLibDir = "Z:\\libs\\symbols";
    c1.eda.kicad.footprintLibDir = "Z:\\libs\\footprints.pretty";
    c1.eda.kicad.projectsDir = "~/work/pcb/kicad_custom";
    c1.eda.kicad.schematicGridMils = 25;
    c1.eda.kicad.autoErcAfterEdit = false;
    c1.eda.kicad.drcSchematicParity = false;
    c1.eda.kicad.fabTargetHouse = "pcbway";
    c1.eda.kicad.timeoutMs = 80000;

    c1.eda.freecad.enabled = true;
    c1.eda.freecad.host = "freecad.local";
    c1.eda.freecad.port = 3910;
    c1.eda.freecad.pluginPort = 3911;
    c1.eda.freecad.executionMode = "headless_cmd";
    c1.eda.freecad.customBinaryPath = "C:\\Program Files\\FreeCAD 1.0\\bin\\FreeCADCmd.exe";
    c1.eda.freecad.projectsDir = "~/work/robot/cad_custom";
    c1.eda.freecad.enforceTnpGuard = true;
    c1.eda.freecad.defaultPrintToleranceMm = 0.25;
    c1.eda.freecad.minWallThicknessMm = 1.60;
    c1.eda.freecad.maxOverhangDeg = 50.0;
    c1.eda.freecad.meshLinearDeflectionMm = 0.04;
    c1.eda.freecad.meshAngularDeflectionDeg = 4.0;
    c1.eda.freecad.jointSweepStepDeg = 2.5;
    c1.eda.freecad.renderWidth = 1920;
    c1.eda.freecad.renderHeight = 1080;
    c1.eda.freecad.timeoutMs = 150000;

    std::string valErr;
    CHECK_TRUE(c1.eda.validate(valErr));
    CHECK_EQUAL("", valErr);

    CHECK_EQUAL("easyeda.local", c1.eda.resolvedHostFor(c1.eda.easyeda.host));
    CHECK_EQUAL("eda-host.local", c1.eda.resolvedHostFor(c1.eda.kicad.host));
    CHECK_EQUAL(3901, c1.eda.resolvedPortFor(c1.eda.easyeda.port));
    CHECK_EQUAL(3892, c1.eda.resolvedPortFor(c1.eda.kicad.port));
    CHECK_EQUAL(3902, c1.eda.resolvedPluginPortFor(c1.eda.easyeda.pluginPort));
    CHECK_EQUAL(3893, c1.eda.resolvedPluginPortFor(c1.eda.kicad.pluginPort));

    CHECK_TRUE(cm1.saveLibConfig(cfgPath));
    ConfigManager cmLib;
    CHECK_TRUE(cmLib.loadLibConfig(cfgPath));
    const EdaBridgeConfig& libEda = cmLib.getConfig().eda;
    CHECK_TRUE(libEda.enabled);
    CHECK_EQUAL("bidirectional", libEda.mode);
    CHECK_EQUAL("eda-host.local", libEda.host);
    CHECK_EQUAL(3892, libEda.port);
    CHECK_EQUAL(3893, libEda.pluginPort);
    CHECK_EQUAL("test-secret-token", libEda.authSecret);
    CHECK_EQUAL(45, libEda.healthPollIntervalSec);
    CHECK_EQUAL(75000, libEda.timeoutMs);
    CHECK_EQUAL(8388608, libEda.maxOutputBytes);
    CHECK_TRUE(libEda.requireApprovalForExports);
    CHECK_EQUAL("~/work", libEda.linuxPathPrefix);
    CHECK_EQUAL("Z:\\", libEda.windowsPathPrefix);

    CHECK_EQUAL(25000, libEda.accelerators.saPlacementIterations);
    CHECK_EQUAL(30, libEda.accelerators.autorouteMaxRipupPasses);
    DOUBLES_EQUAL(0.20, libEda.accelerators.autorouteGridMm, 0.001);
    CHECK_FALSE(libEda.accelerators.preferJlcpcbBasicESeries);
    DOUBLES_EQUAL(2.0, libEda.accelerators.defaultCopperWeightOz, 0.001);
    CHECK_EQUAL("PA-CF", libEda.accelerators.defaultFilamentMaterial);

    CHECK_EQUAL("easyeda.local", libEda.easyeda.host);
    CHECK_EQUAL(3901, libEda.easyeda.port);
    CHECK_EQUAL(3902, libEda.easyeda.pluginPort);
    CHECK_EQUAL("live_gui", libEda.easyeda.executionMode);
    CHECK_EQUAL(4, libEda.easyeda.jlcpcbLayerCount);
    CHECK_EQUAL(250, libEda.easyeda.minLcscStock);

    CHECK_EQUAL("headless_cli", libEda.kicad.executionMode);
    CHECK_EQUAL(25, libEda.kicad.schematicGridMils);
    CHECK_EQUAL("pcbway", libEda.kicad.fabTargetHouse);
    CHECK_EQUAL("Z:\\libs\\symbols", libEda.kicad.symbolLibDir);

    CHECK_EQUAL("freecad.local", libEda.freecad.host);
    DOUBLES_EQUAL(0.25, libEda.freecad.defaultPrintToleranceMm, 0.001);
    DOUBLES_EQUAL(1.60, libEda.freecad.minWallThicknessMm, 0.001);
    CHECK_EQUAL(1920, libEda.freecad.renderWidth);
    CHECK_EQUAL(1080, libEda.freecad.renderHeight);

    CHECK_TRUE(cm1.save(jsonPath));
    ConfigManager cmJson;
    CHECK_TRUE(cmJson.load(jsonPath));
    const EdaBridgeConfig& jsEda = cmJson.getConfig().eda;
    CHECK_TRUE(jsEda.enabled);
    CHECK_EQUAL("eda-host.local", jsEda.host);
    CHECK_EQUAL(25000, jsEda.accelerators.saPlacementIterations);
    CHECK_EQUAL("PA-CF", jsEda.accelerators.defaultFilamentMaterial);
    CHECK_EQUAL(4, jsEda.easyeda.jlcpcbLayerCount);
    CHECK_EQUAL(25, jsEda.kicad.schematicGridMils);
    DOUBLES_EQUAL(0.25, jsEda.freecad.defaultPrintToleranceMm, 0.001);

    setenv("AIMON_EDA_AUTH_SECRET", "env-override-secret", 1);
    setenv("AIMON_EDA_HOST", "env-eda.local", 1);
    setenv("AIMON_EDA_PORT", "4892", 1);
    cmJson.applyEnvironmentOverrides();
    CHECK_EQUAL("env-override-secret", cmJson.getConfig().eda.authSecret);
    CHECK_EQUAL("env-eda.local", cmJson.getConfig().eda.host);
    CHECK_EQUAL(4892, cmJson.getConfig().eda.port);
    unsetenv("AIMON_EDA_AUTH_SECRET");
    unsetenv("AIMON_EDA_HOST");
    unsetenv("AIMON_EDA_PORT");

    EdaBridgeConfig badPort = c1.eda;
    badPort.port = 70000;
    CHECK_FALSE(badPort.validate(valErr));

    EdaBridgeConfig badMode = c1.eda;
    badMode.mode = "invalid_mode";
    CHECK_FALSE(badMode.validate(valErr));

    EdaBridgeConfig badTraversal = c1.eda;
    badTraversal.linuxPathPrefix = "~/work/../escape";
    CHECK_FALSE(badTraversal.validate(valErr));
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
