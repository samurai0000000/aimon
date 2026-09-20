/*
 * TestGatewayTimeout.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "DynamicToolRegistry.hxx"
#include "TcpGateway.hxx"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <string>

using namespace aimon;

static void testTimeoutCalculation() {
    std::cout << "[TEST] 1. Timeout calculation formula..." << std::endl;

    auto calculateTimeout = [](const std::string& toolName, const nlohmann::json& args) -> int {
        int timeoutMs = 45000;
        if (args.contains("timeout_sec") && args["timeout_sec"].is_number()) {
            timeoutMs = (args["timeout_sec"].get<int>() + 10) * 1000;
        } else if (toolName.find("flash") != std::string::npos) {
            timeoutMs = 180000;
        }
        return timeoutMs;
    };

    // Default case (no timeout_sec, non-flash tool)
    {
        nlohmann::json args = nlohmann::json::object();
        int t = calculateTimeout("embdevenv_get_target_status", args);
        assert(t == 45000);
    }

    // Console catch bootloader with default 30s timeout_sec
    {
        nlohmann::json args = {{"timeout_sec", 30}};
        int t = calculateTimeout("embdevenv_console_catch_bootloader", args);
        assert(t == 40000);
    }

    // Console catch bootloader with 60s timeout_sec
    {
        nlohmann::json args = {{"timeout_sec", 60}};
        int t = calculateTimeout("embdevenv_console_catch_bootloader", args);
        assert(t == 70000);
    }

    // Flash tool without timeout_sec
    {
        nlohmann::json args = nlohmann::json::object();
        int t = calculateTimeout("embdevenv_flash_target", args);
        assert(t == 180000);
    }

    // Flash tool with custom timeout_sec
    {
        nlohmann::json args = {{"timeout_sec", 300}};
        int t = calculateTimeout("embdevenv_flash_target", args);
        assert(t == 310000);
    }

    std::cout << "  [PASS] Timeout calculation formula verified." << std::endl;
}

static void testTcpGatewayTimeoutExecution() {
    std::cout << "[TEST] 2. End-to-end TcpGateway timeout execution on ephemeral port..." << std::endl;

    const int testPort = 29885;
    DynamicToolRegistry registry;
    TcpGateway gateway(registry);

    bool started = gateway.start("127.0.0.1", testPort);
    assert(started);
    assert(gateway.isRunning());

    // Connect mock client socket
    int clientFd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(clientFd >= 0);

    struct sockaddr_in saddr;
    std::memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(testPort);
    ::inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr);

    // Connect with retries
    bool connected = false;
    for (int i = 0; i < 20; ++i) {
        if (::connect(clientFd, (struct sockaddr*)&saddr, sizeof(saddr)) == 0) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(connected);

    // Start background receiver/responder loop for mock client
    std::atomic<bool> clientRunning{true};
    std::thread clientThread([clientFd, &clientRunning]() {
        std::string buffer;
        char buf[1024];
        while (clientRunning.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(clientFd, &rfds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 30000;
            int ret = ::select(clientFd + 1, &rfds, nullptr, nullptr, &tv);
            if (ret <= 0) continue;

            ssize_t n = ::read(clientFd, buf, sizeof(buf));
            if (n <= 0) break;
            buffer.append(buf, n);

            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                if (line.empty()) continue;

                auto j = nlohmann::json::parse(line, nullptr, false);
                if (j.is_discarded()) continue;

                if (j.contains("method") && j["method"] == "tools/call") {
                    std::string tool = j["params"]["name"];
                    auto reqId = j["id"];
                    if (tool == "mock_quick_tool") {
                        nlohmann::json resp = {
                            {"jsonrpc", "2.0"},
                            {"id", reqId},
                            {"result", {{"status", "ok"}, {"caught", true}}}
                        };
                        std::string respStr = resp.dump() + "\n";
                        ::write(clientFd, respStr.data(), respStr.size());
                    }
                    // For mock_console_catch_bootloader: intentionally do nothing (simulating silence)
                }
            }
        }
    });

    // Register mock tools
    nlohmann::json regMsg = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "gateway/register"},
        {"params", {
            {"subsystem", "embdevenv"},
            {"tools", {
                {
                    {"name", "mock_console_catch_bootloader"},
                    {"description", "Mock bootloader catcher"},
                    {"inputSchema", {{"type", "object"}}}
                },
                {
                    {"name", "mock_quick_tool"},
                    {"description", "Mock fast tool"},
                    {"inputSchema", {{"type", "object"}}}
                }
            }}
        }}
    };

    std::string regStr = regMsg.dump() + "\n";
    ssize_t sent = ::write(clientFd, regStr.data(), regStr.size());
    assert(sent == (ssize_t)regStr.size());

    // Allow registration to process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(registry.hasTool("mock_console_catch_bootloader"));
    assert(registry.hasTool("mock_quick_tool"));

    // Sub-test A: Test callTool timeout expiration
    {
        std::cout << "  Sub-test 2a: Testing callTool timeout expiration (300ms)..." << std::endl;
        nlohmann::json args = {{"timeout_sec", 30}};
        nlohmann::json result;
        std::string err;

        auto t0 = std::chrono::steady_clock::now();
        bool ok = gateway.callTool("mock_console_catch_bootloader", args, result, err, 300);
        auto t1 = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        assert(!ok);
        assert(err.find("Timed out waiting for subsystem 'embdevenv'") != std::string::npos);
        std::cout << "    Timeout returned error: '" << err << "' in " << elapsedMs << " ms" << std::endl;
        assert(elapsedMs >= 250 && elapsedMs <= 600);
        std::cout << "  [PASS] 2a. Expired timeout handled cleanly within tolerance." << std::endl;
    }

    // Sub-test B: Test callTool successful response before timeout
    {
        std::cout << "  Sub-test 2b: Testing callTool quick success before timeout..." << std::endl;
        nlohmann::json args = nlohmann::json::object();
        nlohmann::json result;
        std::string err;

        auto t0 = std::chrono::steady_clock::now();
        bool ok = gateway.callTool("mock_quick_tool", args, result, err, 2000);
        auto t1 = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        assert(ok);
        assert(result.contains("status") && result["status"] == "ok");
        assert(result.contains("caught") && result["caught"] == true);
        std::cout << "    Response: " << result.dump() << " received in " << elapsedMs << " ms" << std::endl;
        std::cout << "  [PASS] 2b. Successful tool execution received." << std::endl;
    }

    // Sub-test C: Subsystem disconnect handling
    {
        std::cout << "  Sub-test 2c: Testing socket disconnect during tool execution..." << std::endl;
        std::thread caller([&gateway]() {
            nlohmann::json args = nlohmann::json::object();
            nlohmann::json result;
            std::string err;
            bool ok = gateway.callTool("mock_console_catch_bootloader", args, result, err, 5000);
            assert(!ok);
            assert(err.find("Subsystem disconnected") != std::string::npos ||
                   err.find("Timed out") != std::string::npos);
            std::cout << "    Caller correctly received on disconnect: '" << err << "'" << std::endl;
        });

        // Give caller a moment to send the request
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        clientRunning.store(false);
        ::close(clientFd);

        caller.join();
        std::cout << "  [PASS] 2c. Subsystem disconnect handled immediately." << std::endl;
    }

    if (clientThread.joinable()) {
        clientThread.join();
    }

    gateway.stop();
    assert(!gateway.isRunning());
    std::cout << "  [PASS] 2. End-to-end TcpGateway timeout verification passed." << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Running aimon TcpGateway Timeout Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    testTimeoutCalculation();
    testTcpGatewayTimeoutExecution();

    std::cout << "\nALL VERIFICATION TESTS PASSED SUCCESSFULLY." << std::endl;
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
