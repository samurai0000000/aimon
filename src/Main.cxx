/*
 * Main.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <csignal>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>
#include "ConfigManager.hxx"
#include "PathUtils.hxx"
#include "Models.hxx"
#include "StateStore.hxx"
#include "HistoryStore.hxx"
#include "AntigravityCollector.hxx"
#include "CursorCollector.hxx"
#include "McpServer.hxx"
#include "MqttPublisher.hxx"
#include "WebServer.hxx"
#include "DynamicToolRegistry.hxx"
#include "TcpGateway.hxx"
#include "AgentMessageBus.hxx"
#include "NcursesConsole.hxx"
#include "AgentRunner.hxx"
#include "CollabOrchestrator.hxx"
#include "Version.hxx"
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

using namespace aimon;

static std::atomic<bool> g_shutdown(false);
static std::condition_variable g_cv;
static std::mutex g_cvMutex;
static NcursesConsole* g_console = nullptr;

static void signalHandler(int sig) {
    (void)sig;
    g_shutdown = true;
    if (g_console) {
        g_console->shutdown();
    }
    g_cv.notify_all();
}

static void printUsage(const char* progName) {
    std::cout << "aimon: Unified AI Quota & Subscription Monitor (v"
              << AIMON_VERSION_STRING << ")\n"
              << "Built: " << AIMON_WHOAMI << "@" << AIMON_HOSTNAME
              << " " << AIMON_DATE << "\n\n"
              << "Usage:\n"
              << "  " << progName << " [command] [options]\n\n"
              << "Commands:\n"
              << "  daemon                 Run full background daemon (Web + Poller + MQTT) [Default]\n"
              << "  web                    Run embedded web dashboard server\n"
              << "  mcp                    Run as stdio JSON-RPC 2.0 MCP server for AI agents\n"
              << "  collab <subcmd> [...]  Inter-agent collaboration controls (run, step, auto, status, abort)\n\n"
              << "Options:\n"
              << "  --config <path>        Custom path to configuration file (default: ~/.config/aimon/config.json)\n"
              << "  --host <host>          Host interface for web dashboard (default: 0.0.0.0)\n"
              << "  --port <port>          Port for web dashboard (default: 3883)\n"
              << "  --mqtt-enable          Enable MQTT publishing to Home Assistant\n"
              << "  --mqtt-broker <host>   MQTT broker hostname (default: localhost)\n"
              << "  --mqtt-port <port>     MQTT broker port (default: 1883)\n"
              << "  --cursor-token <token> Set Cursor authentication token\n"
              << "  --gateway-port <port>  Port for TCP Tool Gateway (default: 3885)\n"
              << "  --gateway-disable      Disable TCP Tool Gateway\n"
              << "  --no-ncurses           Disable split-screen ncurses terminal interface\n"
              << "  --version, -v          Display version and build metadata\n"
              << "  --help, -h             Display this help message\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::string command = "daemon";
    std::string customConfigPath;
    std::string optHost;
    int optPort = 0;
    bool optMqttEnable = false;
    bool optMqttExplicit = false;
    std::string optMqttBroker;
    int optMqttPort = 0;
    std::string optCursorToken;
    int optGatewayPort = 0;
    bool optGatewayDisable = false;
    bool optNoNcurses = false;

    int argIdx = 1;
    if (argIdx < argc && argv[argIdx][0] != '-') {
        command = argv[argIdx++];
    }

    while (argIdx < argc) {
        if (command == "collab") {
            break;
        }
        std::string arg = argv[argIdx++];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--version" || arg == "-v") {
            std::cout << "aimon version " << AIMON_VERSION_STRING << "\n"
                      << "Built: " << AIMON_WHOAMI << "@" << AIMON_HOSTNAME
                      << " " << AIMON_DATE << "\n";
            return 0;
        } else if (arg == "--config" && argIdx < argc) {
            customConfigPath = argv[argIdx++];
        } else if (arg == "--host" && argIdx < argc) {
            optHost = argv[argIdx++];
        } else if (arg == "--port" && argIdx < argc) {
            optPort = std::stoi(argv[argIdx++]);
        } else if (arg == "--mqtt-enable") {
            optMqttEnable = true;
            optMqttExplicit = true;
        } else if (arg == "--mqtt-disable") {
            optMqttEnable = false;
            optMqttExplicit = true;
        } else if (arg == "--mqtt-broker" && argIdx < argc) {
            optMqttBroker = argv[argIdx++];
        } else if (arg == "--mqtt-port" && argIdx < argc) {
            optMqttPort = std::stoi(argv[argIdx++]);
        } else if (arg == "--cursor-token" && argIdx < argc) {
            optCursorToken = argv[argIdx++];
        } else if (arg == "--gateway-port" && argIdx < argc) {
            optGatewayPort = std::stoi(argv[argIdx++]);
        } else if (arg == "--gateway-disable") {
            optGatewayDisable = true;
        } else if (arg == "--no-ncurses") {
            optNoNcurses = true;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (command == "--version" || command == "-v") {
        std::cout << "aimon version " << AIMON_VERSION_STRING << "\n"
                  << "Built: " << AIMON_WHOAMI << "@" << AIMON_HOSTNAME
                  << " " << AIMON_DATE << "\n";
        return 0;
    }

    if (command == "collab") {
        ConfigManager configMgr;
        configMgr.load(customConfigPath);
        int port = (optPort > 0) ? optPort : configMgr.getConfig().web.port;
        std::string host = optHost.empty() ? "127.0.0.1" : optHost;

        std::string subCmd = (argIdx < argc) ? argv[argIdx++] : "status";
        httplib::Client cli(host, port);
        cli.set_connection_timeout(5, 0);

        if (subCmd == "status") {
            auto res = cli.Get("/api/collaboration/status");
            if (!res || res->status != 200) {
                std::cerr << "Failed to connect to aimon daemon at " << host << ":" << port << "\n";
                return 1;
            }
            std::cout << res->body << "\n";
            return 0;
        } else if (subCmd == "run") {
            std::string planFile = (argIdx < argc) ? argv[argIdx++] : "";
            std::string initId = (argIdx < argc) ? argv[argIdx++] : "agent-antigravity-builder";
            std::string revId = (argIdx < argc) ? argv[argIdx++] : "agent-cursor-windows";
            if (planFile.empty()) {
                std::cerr << "Usage: " << argv[0] << " collab run <plan_file> [initiator] [reviewer]\n";
                return 1;
            }
            nlohmann::json reqBody = {
                {"plan_file", planFile},
                {"initiator_id", initId},
                {"reviewer_id", revId}
            };
            auto res = cli.Post("/api/collaboration/run", reqBody.dump(), "application/json");
            if (!res) {
                std::cerr << "Failed to connect to aimon daemon at " << host << ":" << port << "\n";
                return 1;
            }
            std::cout << res->body << "\n";
            return (res->status == 200 ? 0 : 1);
        } else if (subCmd == "step") {
            auto res = cli.Post("/api/collaboration/step", "{}", "application/json");
            if (!res) {
                std::cerr << "Failed to connect to aimon daemon at " << host << ":" << port << "\n";
                return 1;
            }
            std::cout << res->body << "\n";
            return (res->status == 200 ? 0 : 1);
        } else if (subCmd == "auto") {
            std::string mode = (argIdx < argc) ? argv[argIdx++] : "on";
            bool enable = (mode == "on" || mode == "1" || mode == "true");
            nlohmann::json reqBody = {{"enabled", enable}};
            auto res = cli.Post("/api/collaboration/auto", reqBody.dump(), "application/json");
            if (!res) {
                std::cerr << "Failed to connect to aimon daemon at " << host << ":" << port << "\n";
                return 1;
            }
            std::cout << res->body << "\n";
            return (res->status == 200 ? 0 : 1);
        } else if (subCmd == "abort") {
            auto res = cli.Post("/api/collaboration/abort", "{}", "application/json");
            if (!res) {
                std::cerr << "Failed to connect to aimon daemon at " << host << ":" << port << "\n";
                return 1;
            }
            std::cout << res->body << "\n";
            return (res->status == 200 ? 0 : 1);
        } else {
            // Treat subCmd as planFile to start collaboration
            std::string planFile = subCmd;
            std::string initId = (argIdx < argc) ? argv[argIdx++] : "agent-antigravity-builder";
            std::string revId = (argIdx < argc) ? argv[argIdx++] : "agent-cursor-windows";
            nlohmann::json reqBody = {
                {"plan_file", planFile},
                {"initiator_id", initId},
                {"reviewer_id", revId}
            };
            auto res = cli.Post("/api/collaboration/start", reqBody.dump(), "application/json");
            if (!res) {
                std::cerr << "Failed to connect to aimon daemon at " << host << ":" << port << "\n";
                return 1;
            }
            std::cout << res->body << "\n";
            return (res->status == 200 ? 0 : 1);
        }
    }

    if (command != "daemon" && command != "web" && command != "mcp") {
        std::cerr << "Unknown command: " << command << "\n\n";
        printUsage(argv[0]);
        return 1;
    }

    // Load configuration
    ConfigManager configMgr;
    configMgr.load(customConfigPath);
    AimonConfig& cfg = configMgr.getConfig();

    // Apply CLI overrides
    if (!optHost.empty()) cfg.web.host = optHost;
    if (optPort > 0) cfg.web.port = optPort;
    if (optMqttExplicit) cfg.mqtt.enabled = optMqttEnable;
    if (!optMqttBroker.empty()) cfg.mqtt.broker = optMqttBroker;
    if (optMqttPort > 0) cfg.mqtt.port = optMqttPort;
    if (!optCursorToken.empty()) cfg.cursor.accessToken = optCursorToken;
    if (optGatewayPort > 0) cfg.gateway.port = optGatewayPort;
    if (optGatewayDisable) cfg.gateway.enabled = false;

    StateStore& stateStore = StateStore::getInstance();
    HistoryStore historyStore;
    if (cfg.history.enabled) {
        historyStore.open(cfg.history.dbPath);
    }

    AntigravityCollector agCollector(cfg.antigravity);
    CursorCollector crCollector(cfg.cursor);
    std::unique_ptr<MqttPublisher> mqttPublisher;

    if (cfg.mqtt.enabled) {
        mqttPublisher = std::make_unique<MqttPublisher>(cfg.mqtt);
        mqttPublisher->start();
    }

    auto pollOnce = [&]() {
        AntigravityStatus ag = agCollector.fetchStatus();
        CursorStatus cr = crCollector.fetchStatus();

        AggregateStatus status;
        status.antigravity = ag;
        status.cursor = cr;
        status.lastUpdated = std::chrono::system_clock::now();

        stateStore.update(status);

        if (cfg.history.enabled) {
            historyStore.recordSnapshot(status);
        }

        if (mqttPublisher) {
            mqttPublisher->publishState(status);
        }
    };

    // Initial synchronous poll
    pollOnce();

    if (command == "mcp") {
        // Start background polling thread
        std::thread pollerThread([&]() {
            while (!g_shutdown) {
                std::unique_lock<std::mutex> lock(g_cvMutex);
                int waitSec = cfg.polling.baseIntervalSec;
                if (g_cv.wait_for(lock, std::chrono::seconds(waitSec), [] { return g_shutdown.load(); })) {
                    break;
                }
                pollOnce();
            }
        });

        DynamicToolRegistry dynamicRegistry;
        TcpGateway tcpGateway(dynamicRegistry);
        if (cfg.gateway.enabled) {
            tcpGateway.start(cfg.gateway.host, cfg.gateway.port);
        }

        McpServer mcpServer(stateStore, &dynamicRegistry, &tcpGateway);
        tcpGateway.setToolsChangedCallback([&]() {
            mcpServer.notifyToolsListChanged();
        });

        mcpServer.run();

        if (cfg.gateway.enabled) {
            tcpGateway.stop();
        }

        g_shutdown = true;
        g_cv.notify_all();
        if (pollerThread.joinable()) {
            pollerThread.join();
        }
        return 0;
    }

    // Web or Daemon mode
    DynamicToolRegistry dynamicRegistry;
    TcpGateway tcpGateway(dynamicRegistry);

    McpServer mcpServer(stateStore, &dynamicRegistry, &tcpGateway);
    WebServer webServer(stateStore, historyStore, cfg.web, [&]() {
        pollOnce();
    }, &mcpServer);

    AgentRunner agentRunner(cfg.collaboration);
    CollabOrchestrator collabOrchestrator(agentRunner, configMgr);
    collabOrchestrator.start();
    webServer.setCollabOrchestrator(&collabOrchestrator);

    mcpServer.setNotificationBroadcaster([&](const std::string& notif) {
        webServer.broadcastSseNotification(notif);
    });

    tcpGateway.setToolsChangedCallback([&]() {
        mcpServer.notifyToolsListChanged();
    });

    if (cfg.gateway.enabled) {
        if (!tcpGateway.start(cfg.gateway.host, cfg.gateway.port)) {
            std::cerr << "[aimon] Warning: failed to start TCP Gateway on "
                      << cfg.gateway.host << ":" << cfg.gateway.port << std::endl;
        }
    }

    if (!webServer.start(true)) {
        std::cerr << "[aimon] Error: failed to start web server" << std::endl;
        tcpGateway.stop();
        return 1;
    }

    std::cout << "[aimon] Running in " << command << " mode (PID: " << getpid() << ")" << std::endl;

    bool isTty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    const char* termEnv = std::getenv("TERM");
    if (termEnv && std::string(termEnv) == "dumb") {
        isTty = false;
    }
    if (optNoNcurses) {
        isTty = false;
    }

    std::unique_ptr<NcursesConsole> console;
    if (isTty && (command == "daemon" || command == "web")) {
        console = std::make_unique<NcursesConsole>(stateStore);
        g_console = console.get();
        console->setShutdownCallback([&]() {
            g_shutdown = true;
            g_cv.notify_all();
        });

        AgentMessageBus::getInstance().setReplyCallback([&](const std::string& sessId,
                                                            const std::string& msgId,
                                                            const std::string& reply) {
            if (console) {
                console->logAgentReply(sessId, msgId, reply);
            }
        });

        console->setCollabOrchestrator(&collabOrchestrator);
        console->init();
    } else {
        AgentMessageBus::getInstance().setReplyCallback([&](const std::string& sessId,
                                                            const std::string& msgId,
                                                            const std::string& reply) {
            std::cout << "\n[Agent (" << sessId.substr(0, 8) << ") ref: " << msgId << "]:\n"
                      << reply << "\n" << std::endl;
        });
        std::cout << "[aimon] Press Ctrl+C to stop." << std::endl;
    }

    // Background poller thread
    std::thread pollerThread([&]() {
        while (!g_shutdown) {
            std::unique_lock<std::mutex> lock(g_cvMutex);

            AggregateStatus current = stateStore.getStatus();
            int waitSec = cfg.polling.baseIntervalSec;
            if (!current.antigravity.isRunning && !current.cursor.isAuthenticated) {
                waitSec = cfg.polling.idleIntervalSec;
            }

            if (g_cv.wait_for(lock, std::chrono::seconds(waitSec), [] { return g_shutdown.load(); })) {
                break;
            }

            pollOnce();
            if (console) {
                console->updateHeader();
            }
        }
    });

    if (console) {
        console->run();
    } else {
        while (!g_shutdown) {
            std::unique_lock<std::mutex> lock(g_cvMutex);
            g_cv.wait(lock, [] { return g_shutdown.load(); });
        }
    }

    g_shutdown = true;
    g_cv.notify_all();

    if (pollerThread.joinable()) {
        pollerThread.join();
    }

    if (console) {
        g_console = nullptr;
        console->shutdown();
    }

    std::cout << "\n[aimon] Shutting down cleanly..." << std::endl;
    collabOrchestrator.stop();
    tcpGateway.stop();
    webServer.stop();
    if (mqttPublisher) {
        mqttPublisher->stop();
    }
    historyStore.close();

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
