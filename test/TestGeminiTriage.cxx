/*
 * TestGeminiTriage.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include <chrono>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <nlohmann/json.hpp>

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

#include "GeminiTriage.hxx"

#include <CppUTest/TestHarness.h>
#include <CppUTest/CommandLineTestRunner.h>

using namespace aimon;

TEST_GROUP(GeminiTriage_Unit) {
    void setup() override {
    }

    void teardown() override {
    }
};

TEST(GeminiTriage_Unit, DormancyCheck_DisabledOrEmptyKey) {
    GeminiConfig cfg;
    cfg.enabled = false;
    cfg.apiKey = "some-key";

    GeminiTriage triageDisabled(cfg);
    CHECK_FALSE(triageDisabled.isEnabled());

    auto start = std::chrono::steady_clock::now();
    TriageReport report = triageDisabled.analyzeCrash("/bin/true", "", "log text", 139, 11);
    auto end = std::chrono::steady_clock::now();

    CHECK_FALSE(report.success);
    STRCMP_EQUAL("Gemini triage disabled or API key empty", report.diagnosisText.c_str());
    int durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    CHECK_TRUE(durationMs < 50);

    cfg.enabled = true;
    cfg.apiKey = ""; // Empty key
    GeminiTriage triageNoKey(cfg);
    CHECK_FALSE(triageNoKey.isEnabled());

    TriageReport report2 = triageNoKey.analyzeCrash("/bin/true", "", "log text", 139, 11);
    CHECK_FALSE(report2.success);
}

TEST(GeminiTriage_Unit, GdbOutputParser_ValidFrame) {
    std::string sampleGdb =
        "#0  0x00007ffff7c8b00b in raise () from /lib/x86_64-linux-gnu/libc.so.6\n"
        "#1  0x00007ffff7c6a859 in abort () from /lib/x86_64-linux-gnu/libc.so.6\n"
        "#2  0x000055555555e123 in crashHandler (sig=11) at src/ProcessMonitor.cxx:142\n"
        "#3  <signal handler called>\n";

    std::string faultingFile;
    int faultingLine = 0;
    std::string loc = GeminiTriage::parseGdbOutput(sampleGdb, faultingFile, faultingLine);

    STRCMP_EQUAL("src/ProcessMonitor.cxx:142", loc.c_str());
    STRCMP_EQUAL("src/ProcessMonitor.cxx", faultingFile.c_str());
    LONGS_EQUAL(142, faultingLine);
}

TEST(GeminiTriage_Unit, GdbOutputParser_EmptyOrMalformed) {
    std::string faultingFile;
    int faultingLine = 0;
    std::string loc = GeminiTriage::parseGdbOutput("invalid gdb output with no frames", faultingFile, faultingLine);

    STRCMP_EQUAL("", loc.c_str());
    STRCMP_EQUAL("", faultingFile.c_str());
    LONGS_EQUAL(0, faultingLine);
}

TEST(GeminiTriage_Unit, JsonPayloadSerialization) {
    GeminiConfig cfg;
    cfg.enabled = true;
    cfg.apiKey = "test-key";
    cfg.model = "gemini-2.5-flash";
    cfg.temperature = 0.2;
    cfg.maxTokens = 1024;

    GeminiTriage triage(cfg);
    std::string prompt = "Diagnose crash in ProcessMonitor";
    std::string payload = triage.buildJsonPayload(prompt);

    nlohmann::json j = nlohmann::json::parse(payload);
    CHECK_TRUE(j.contains("contents"));
    CHECK_TRUE(j["contents"][0]["parts"][0]["text"].get<std::string>() == prompt);
    CHECK_TRUE(j.contains("generationConfig"));
    LONGS_EQUAL(1024, j["generationConfig"]["maxOutputTokens"].get<int>());
}

TEST(GeminiTriage_Unit, JsonResponseParser_ValidModelResponse) {
    GeminiConfig cfg;
    GeminiTriage triage(cfg);

    std::string sampleResponse = R"({
        "candidates": [
            {
                "content": {
                    "parts": [
                        {
                            "text": "ROOT_CAUSE_CATEGORY: SEGFAULT\nDIAGNOSIS: Null pointer dereference in ProcessMonitor.\nSUGGESTED_FIX:\n```diff\n--- a/src/ProcessMonitor.cxx\n+++ b/src/ProcessMonitor.cxx\n@@ -10,1 +10,2 @@\n+if (!ptr) return;\n```\n"
                        }
                    ]
                }
            }
        ]
    })";

    TriageReport report;
    bool parsed = triage.parseJsonResponse(sampleResponse, report);
    CHECK_TRUE(parsed);
    STRCMP_EQUAL("SEGFAULT", report.rootCauseCategory.c_str());
    CHECK_TRUE(report.diagnosisText.find("Null pointer dereference") != std::string::npos);
    CHECK_TRUE(report.suggestedFixDiff.find("+if (!ptr) return;") != std::string::npos);
}

TEST(GeminiTriage_Unit, JsonResponseParser_MalformedJson) {
    GeminiConfig cfg;
    GeminiTriage triage(cfg);

    TriageReport report;
    bool parsed = triage.parseJsonResponse("{ broken json ...", report);
    CHECK_FALSE(parsed);
}

TEST(GeminiTriage_Unit, IncidentLogWriter_FileCreation) {
    std::string tempDir = "/tmp/aimon_test_incidents_" + std::to_string(getpid());
    std::filesystem::remove_all(tempDir);

    GeminiConfig cfg;
    cfg.incidentLogDir = tempDir;
    GeminiTriage triage(cfg);

    TriageReport report;
    report.timestampEpoch = 1727330000;
    report.rootCauseCategory = "SEGFAULT";
    report.faultingFile = "src/ProcessMonitor.cxx";
    report.faultingLine = 142;
    report.diagnosisText = "Null pointer dereference in crash handler.";
    report.suggestedFixDiff = "+if (!ptr) return;";
    report.success = true;

    bool written = triage.writeIncidentLog(report, tempDir);
    CHECK_TRUE(written);

    std::string expectedFile = tempDir + "/incident_1727330000.md";
    CHECK_TRUE(std::filesystem::exists(expectedFile));

    std::ifstream ifs(expectedFile);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    CHECK_TRUE(content.find("ROOT_CAUSE_CATEGORY: SEGFAULT") != std::string::npos ||
               content.find("Root Cause Category**: SEGFAULT") != std::string::npos);
    CHECK_TRUE(content.find("Null pointer dereference") != std::string::npos);

    std::filesystem::remove_all(tempDir);
}

// ---------------------------------------------------------------------------
// Integrated Scope: Ephemeral Loopback Mock HTTP Server
// ---------------------------------------------------------------------------

class EphemeralHttpMockServer {
public:
    EphemeralHttpMockServer()
        : _listenFd(-1), _port(0), _running(false), _responseCode(200), _responseBody("{}"), _contentType("application/json") {}

    ~EphemeralHttpMockServer() {
        stop();
    }

    void setResponse(int code, const std::string& body, const std::string& contentType = "application/json") {
        _responseCode = code;
        _responseBody = body;
        _contentType = contentType;
    }

    bool start() {
        _listenFd = socket(AF_INET, SOCK_STREAM, 0);
        if (_listenFd < 0) {
            return false;
        }

        int opt = 1;
        setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // Kernel picks ephemeral port

        if (bind(_listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(_listenFd);
            _listenFd = -1;
            return false;
        }

        socklen_t len = sizeof(addr);
        if (getsockname(_listenFd, (struct sockaddr*)&addr, &len) < 0) {
            close(_listenFd);
            _listenFd = -1;
            return false;
        }
        _port = ntohs(addr.sin_port);

        if (listen(_listenFd, 5) < 0) {
            close(_listenFd);
            _listenFd = -1;
            return false;
        }

        _running.store(true);
        _worker = std::make_unique<std::thread>([this]() {
            while (_running.load()) {
                struct timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 20000; // 20ms

                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(_listenFd, &rfds);

                int rc = select(_listenFd + 1, &rfds, nullptr, nullptr, &tv);
                if (rc > 0 && FD_ISSET(_listenFd, &rfds)) {
                    struct sockaddr_in clientAddr;
                    socklen_t clientLen = sizeof(clientAddr);
                    int cfd = accept(_listenFd, (struct sockaddr*)&clientAddr, &clientLen);
                    if (cfd >= 0) {
                        char buf[1024];
                        ssize_t n = read(cfd, buf, sizeof(buf) - 1);
                        (void)n;
                        std::string resp = "HTTP/1.1 " + std::to_string(_responseCode) + " OK\r\n"
                                           "Content-Type: " + _contentType + "\r\n"
                                           "Content-Length: " + std::to_string(_responseBody.size()) + "\r\n"
                                           "Connection: close\r\n\r\n" + _responseBody;
                        ssize_t written = write(cfd, resp.data(), resp.size());
                        (void)written;
                        close(cfd);
                    }
                }
            }
        });

        return true;
    }

    void stop() {
        _running.store(false);
        if (_worker && _worker->joinable()) {
            _worker->join();
        }
        _worker.reset();
        if (_listenFd >= 0) {
            close(_listenFd);
            _listenFd = -1;
        }
    }

    int getPort() const {
        return _port;
    }

private:
    int _listenFd;
    int _port;
    std::atomic<bool> _running;
    int _responseCode;
    std::string _responseBody;
    std::string _contentType;
    std::unique_ptr<std::thread> _worker;
};

TEST_GROUP(GeminiTriage_Integrated) {
    void setup() override {
    }

    void teardown() override {
    }
};

TEST(GeminiTriage_Integrated, LiveMockServer_SuccessResponse) {
    EphemeralHttpMockServer mockServer;
    nlohmann::json respJson = {
        {"candidates", nlohmann::json::array({
            {
                {"content", {
                    {"parts", nlohmann::json::array({
                        {
                            {"text", "ROOT_CAUSE_CATEGORY: SEGFAULT\nDIAGNOSIS: Memory access violation.\nSUGGESTED_FIX:\n```diff\n- oldCode();\n+ newCode();\n```\n"}
                        }
                    })}
                }}
            }
        })}
    };
    mockServer.setResponse(200, respJson.dump());
    CHECK_TRUE(mockServer.start());

    GeminiConfig cfg;
    cfg.enabled = true;
    cfg.apiKey = "mock-api-key";
    cfg.model = "test";

    GeminiTriage triage(cfg);
    triage.setTestEndpoint("http://127.0.0.1:" + std::to_string(mockServer.getPort()));

    TriageReport report = triage.analyzeCrash("/bin/true", "", "log output", 139, 11);
    CHECK_TRUE(report.success);
    STRCMP_EQUAL("SEGFAULT", report.rootCauseCategory.c_str());
    CHECK_TRUE(report.diagnosisText.find("Memory access violation") != std::string::npos);
    CHECK_TRUE(report.suggestedFixDiff.find("+ newCode();") != std::string::npos);

    mockServer.stop();
}

TEST(GeminiTriage_Integrated, LiveMockServer_Http429RateLimit) {
    EphemeralHttpMockServer mockServer;
    mockServer.setResponse(429, "{\"error\": {\"message\": \"Resource exhausted\"}}");
    CHECK_TRUE(mockServer.start());

    GeminiConfig cfg;
    cfg.enabled = true;
    cfg.apiKey = "mock-api-key";
    cfg.model = "test";

    GeminiTriage triage(cfg);
    triage.setTestEndpoint("http://127.0.0.1:" + std::to_string(mockServer.getPort()));

    TriageReport report = triage.analyzeCrash("/bin/true", "", "log output", 139, 11);
    CHECK_FALSE(report.success);
    CHECK_TRUE(report.diagnosisText.find("HTTP 429") != std::string::npos);

    mockServer.stop();
}

TEST(GeminiTriage_Integrated, LiveMockServer_Http500InternalError) {
    EphemeralHttpMockServer mockServer;
    mockServer.setResponse(500, "Internal Server Error", "text/plain");
    CHECK_TRUE(mockServer.start());

    GeminiConfig cfg;
    cfg.enabled = true;
    cfg.apiKey = "mock-api-key";
    cfg.model = "test";

    GeminiTriage triage(cfg);
    triage.setTestEndpoint("http://127.0.0.1:" + std::to_string(mockServer.getPort()));

    TriageReport report = triage.analyzeCrash("/bin/true", "", "log output", 139, 11);
    CHECK_FALSE(report.success);
    CHECK_TRUE(report.diagnosisText.find("HTTP 500") != std::string::npos);

    mockServer.stop();
}

TEST(GeminiTriage_Integrated, LiveMockServer_ConnectionRefused) {
    // Acquire and immediately close a port to ensure connection refused
    int tempFd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(tempFd, (struct sockaddr*)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(tempFd, (struct sockaddr*)&addr, &len);
    int closedPort = ntohs(addr.sin_port);
    close(tempFd);

    GeminiConfig cfg;
    cfg.enabled = true;
    cfg.apiKey = "mock-api-key";
    cfg.model = "test";

    GeminiTriage triage(cfg);
    triage.setTestEndpoint("http://127.0.0.1:" + std::to_string(closedPort));

    TriageReport report = triage.analyzeCrash("/bin/true", "", "log output", 139, 11);
    CHECK_FALSE(report.success);
    CHECK_TRUE(report.diagnosisText.find("connection error") != std::string::npos);
}

int main(int argc, char** argv) {
    // Warm up httplib thread-local regex structures before CppUTest memory tracking begins
    {
        EphemeralHttpMockServer warmupServer;
        warmupServer.setResponse(200, "{}");
        if (warmupServer.start()) {
            httplib::Client cli("127.0.0.1", warmupServer.getPort());
            cli.Get("/warmup?test=1");
            warmupServer.stop();
        }
    }

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
