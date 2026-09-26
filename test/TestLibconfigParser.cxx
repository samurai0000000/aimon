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
        validFile << "supervisor: {\n"
                  << "    enabled = true;\n"
                  << "    probe_interval_sec = 5;\n"
                  << "    probe_timeout_sec = 2;\n"
                  << "    crash_loop_window_sec = 60;\n"
                  << "    crash_loop_max_retries = 5;\n"
                  << "    backoff_initial_sec = 1;\n"
                  << "    backoff_max_sec = 30;\n"
                  << "    max_db_quarantine_versions = 5;\n"
                  << "    safe_mode_port = 3889;\n"
                  << "    prev_binary_path = \"/usr/local/bin/aimon.prev\";\n"
                  << "};\n"
                  << "services: (\n"
                  << "    {\n"
                  << "        name = \"meshmon\";\n"
                  << "        host = \"127.0.0.1\";\n"
                  << "        port = 3884;\n"
                  << "        secondary_port = 0;\n"
                  << "        probe_interval_ms = 5000;\n"
                  << "        probe_timeout_ms = 2000;\n"
                  << "        max_restart_retries = 5;\n"
                  << "        restart_window_sec = 60;\n"
                  << "        start_cmd = \"systemctl --user start meshmon\";\n"
                  << "        stop_cmd = \"systemctl --user stop meshmon\";\n"
                  << "        log_file = \"/tmp/meshmon.log\";\n"
                  << "    },\n"
                  << "    {\n"
                  << "        name = \"meshroom\";\n"
                  << "        host = \"127.0.0.1\";\n"
                  << "        port = 3886;\n"
                  << "        secondary_port = 0;\n"
                  << "        probe_interval_ms = 5000;\n"
                  << "        probe_timeout_ms = 2000;\n"
                  << "        max_restart_retries = 5;\n"
                  << "        restart_window_sec = 60;\n"
                  << "        start_cmd = \"systemctl --user start meshroom\";\n"
                  << "        stop_cmd = \"systemctl --user stop meshroom\";\n"
                  << "        log_file = \"/tmp/meshroom.log\";\n"
                  << "    }\n"
                  << ");\n"
                  << "gemini: {\n"
                  << "    enabled = false;\n"
                  << "    api_key = \"\";\n"
                  << "    model = \"gemini-2.5-flash\";\n"
                  << "    max_tokens = 2048;\n"
                  << "    temperature = 0.2;\n"
                  << "    prompt_template = \"Analyze crash report:\";\n"
                  << "    incident_log_dir = \"/tmp/incidents\";\n"
                  << "};\n";
        validFile.close();

        std::ofstream syntaxErrFile(_syntaxErrorCfgPath);
        syntaxErrFile << "supervisor: {\n"
                      << "    enabled = true;\n"
                      << "    probe_interval_sec = ;\n"
                      << "};\n";
        syntaxErrFile.close();

        std::ofstream typeMismatchFile(_typeMismatchCfgPath);
        typeMismatchFile << "supervisor: {\n"
                         << "    safe_mode_port = \"not_a_number\";\n"
                         << "};\n";
        typeMismatchFile.close();

        std::ofstream minimalFile(_minimalCfgPath);
        minimalFile << "web: {\n"
                    << "    port = 8080;\n"
                    << "};\n";
        minimalFile.close();
    }

    void teardown() override {
        unsetenv("GEMINI_API_KEY");
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
    CHECK_TRUE(cfg.supervisor.enabled);
    CHECK_EQUAL(5, cfg.supervisor.probeIntervalSec);
    CHECK_EQUAL(2, cfg.supervisor.probeTimeoutSec);
    CHECK_EQUAL(60, cfg.supervisor.crashLoopWindowSec);
    CHECK_EQUAL(5, cfg.supervisor.crashLoopMaxRetries);
    CHECK_EQUAL(1, cfg.supervisor.backoffInitialSec);
    CHECK_EQUAL(30, cfg.supervisor.backoffMaxSec);
    CHECK_EQUAL(5, cfg.supervisor.maxDbQuarantineVersions);
    CHECK_EQUAL(3889, cfg.supervisor.safeModePort);
    CHECK_EQUAL("/usr/local/bin/aimon.prev", cfg.supervisor.prevBinaryPath);

    CHECK_EQUAL(2, static_cast<int>(cfg.services.size()));
    CHECK_EQUAL("meshmon", cfg.services[0].name);
    CHECK_EQUAL("127.0.0.1", cfg.services[0].host);
    CHECK_EQUAL(3884, cfg.services[0].port);
    CHECK_EQUAL(0, cfg.services[0].secondaryPort);
    CHECK_EQUAL(5000, cfg.services[0].probeIntervalMs);
    CHECK_EQUAL(2000, cfg.services[0].probeTimeoutMs);
    CHECK_EQUAL(5, cfg.services[0].maxRestartRetries);
    CHECK_EQUAL(60, cfg.services[0].restartWindowSec);
    CHECK_EQUAL("systemctl --user start meshmon", cfg.services[0].startCmd);
    CHECK_EQUAL("systemctl --user stop meshmon", cfg.services[0].stopCmd);
    CHECK_EQUAL("/tmp/meshmon.log", cfg.services[0].logFile);

    CHECK_EQUAL("meshroom", cfg.services[1].name);
    CHECK_EQUAL(3886, cfg.services[1].port);

    CHECK_FALSE(cfg.gemini.enabled);
    CHECK_EQUAL("", cfg.gemini.apiKey);
    CHECK_EQUAL("gemini-2.5-flash", cfg.gemini.model);
    CHECK_EQUAL(2048, cfg.gemini.maxTokens);
    DOUBLES_EQUAL(0.2, cfg.gemini.temperature, 0.001);
    CHECK_EQUAL("Analyze crash report:", cfg.gemini.promptTemplate);
    CHECK_EQUAL("/tmp/incidents", cfg.gemini.incidentLogDir);
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
    CHECK_TRUE(err.find("safe_mode_port") != std::string::npos);
}

TEST(LibconfigParser_Unit, MissingRequiredFieldsFallback) {
    ConfigManager cm;
    bool success = cm.loadLibConfig(_minimalCfgPath);
    CHECK_TRUE(success);

    const AimonConfig& cfg = cm.getConfig();
    // Default fallback values
    CHECK_TRUE(cfg.supervisor.enabled);
    CHECK_EQUAL(5, cfg.supervisor.probeIntervalSec);
    CHECK_EQUAL(3889, cfg.supervisor.safeModePort);
    CHECK_EQUAL(2048, cfg.gemini.maxTokens);
    DOUBLES_EQUAL(0.2, cfg.gemini.temperature, 0.001);
    CHECK_EQUAL(8080, cfg.web.port);
}

TEST(LibconfigParser_Unit, EnvVariableFallback) {
    setenv("GEMINI_API_KEY", "secret-test-token-777", 1);
    ConfigManager cm;
    bool success = cm.loadLibConfig(_validCfgPath);
    CHECK_TRUE(success);
    CHECK_EQUAL("secret-test-token-777", cm.getConfig().gemini.apiKey);
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
    cfg1.supervisor.enabled = true;
    cfg1.supervisor.safeModePort = 9999;
    cfg1.supervisor.backoffMaxSec = 75;
    cfg1.gemini.enabled = true;
    cfg1.gemini.apiKey = "custom-api-key";
    cfg1.gemini.temperature = 0.85;

    SupervisedServiceConfig svc1;
    svc1.name = "svc-alpha";
    svc1.host = "10.0.0.1";
    svc1.port = 4001;
    svc1.secondaryPort = 4002;
    svc1.probeIntervalMs = 12345;
    svc1.probeTimeoutMs = 3456;
    svc1.maxRestartRetries = 9;
    svc1.restartWindowSec = 120;
    svc1.startCmd = "launch alpha";
    svc1.stopCmd = "kill alpha";
    svc1.logFile = "/var/log/alpha.log";
    cfg1.services.push_back(svc1);

    SupervisedServiceConfig svc2;
    svc2.name = "svc-beta";
    svc2.host = "10.0.0.2";
    svc2.port = 5001;
    svc2.startCmd = "launch beta";
    svc2.stopCmd = "kill beta";
    svc2.logFile = "/var/log/beta.log";
    cfg1.services.push_back(svc2);

    bool saveOk = cm1.saveLibConfig(path1);
    CHECK_TRUE(saveOk);

    ConfigManager cm2;
    bool loadOk = cm2.loadLibConfig(path1);
    CHECK_TRUE(loadOk);

    const AimonConfig& cfg2 = cm2.getConfig();
    CHECK_EQUAL(cfg1.supervisor.safeModePort, cfg2.supervisor.safeModePort);
    CHECK_EQUAL(cfg1.supervisor.backoffMaxSec, cfg2.supervisor.backoffMaxSec);
    CHECK_TRUE(cfg2.gemini.enabled);
    CHECK_EQUAL(cfg1.gemini.apiKey, cfg2.gemini.apiKey);
    DOUBLES_EQUAL(0.85, cfg2.gemini.temperature, 0.001);

    CHECK_EQUAL(2, static_cast<int>(cfg2.services.size()));
    CHECK_EQUAL("svc-alpha", cfg2.services[0].name);
    CHECK_EQUAL("10.0.0.1", cfg2.services[0].host);
    CHECK_EQUAL(4001, cfg2.services[0].port);
    CHECK_EQUAL(4002, cfg2.services[0].secondaryPort);
    CHECK_EQUAL(12345, cfg2.services[0].probeIntervalMs);
    CHECK_EQUAL(3456, cfg2.services[0].probeTimeoutMs);
    CHECK_EQUAL(9, cfg2.services[0].maxRestartRetries);
    CHECK_EQUAL(120, cfg2.services[0].restartWindowSec);
    CHECK_EQUAL("launch alpha", cfg2.services[0].startCmd);
    CHECK_EQUAL("kill alpha", cfg2.services[0].stopCmd);
    CHECK_EQUAL("/var/log/alpha.log", cfg2.services[0].logFile);

    CHECK_EQUAL("svc-beta", cfg2.services[1].name);
    CHECK_EQUAL("10.0.0.2", cfg2.services[1].host);
    CHECK_EQUAL(5001, cfg2.services[1].port);

    // Save from cm2 to path2 and re-verify
    bool save2Ok = cm2.saveLibConfig(path2);
    CHECK_TRUE(save2Ok);

    ConfigManager cm3;
    bool load2Ok = cm3.loadLibConfig(path2);
    CHECK_TRUE(load2Ok);
    CHECK_EQUAL(cfg2.supervisor.safeModePort, cm3.getConfig().supervisor.safeModePort);
}

TEST(LibconfigParser_Integrated, RealAimonCfgParity) {
    std::string path = _tempDir + "/fleet.cfg";
    ConfigManager cm;
    AimonConfig& cfg = cm.getConfig();

    const std::vector<std::pair<std::string, int>> baselineServices = {
        {"meshmon", 3884},
        {"meshroom", 3886},
        {"meshroof", 3887},
        {"meshpump", 3888},
        {"netmon", 3882}
    };

    cfg.services.clear();
    for (const auto& entry : baselineServices) {
        SupervisedServiceConfig svc;
        svc.name = entry.first;
        svc.host = "127.0.0.1";
        svc.port = entry.second;
        svc.probeIntervalMs = 5000;
        svc.probeTimeoutMs = 2000;
        svc.maxRestartRetries = 5;
        svc.restartWindowSec = 60;
        svc.startCmd = "systemctl --user start " + entry.first;
        svc.stopCmd = "systemctl --user stop " + entry.first;
        svc.logFile = "~/.local/state/aimon/" + entry.first + ".log";
        cfg.services.push_back(svc);
    }

    CHECK_TRUE(cm.saveLibConfig(path));

    ConfigManager reloaded;
    CHECK_TRUE(reloaded.loadLibConfig(path));
    const auto& reloadedSvcs = reloaded.getConfig().services;
    CHECK_EQUAL(5, static_cast<int>(reloadedSvcs.size()));

    for (size_t i = 0; i < baselineServices.size(); ++i) {
        CHECK_EQUAL(baselineServices[i].first, reloadedSvcs[i].name);
        CHECK_EQUAL(baselineServices[i].second, reloadedSvcs[i].port);
        CHECK_EQUAL("systemctl --user start " + baselineServices[i].first, reloadedSvcs[i].startCmd);
    }
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
