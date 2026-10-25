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
#include <ctime>
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
#include "ClaudeCollector.hxx"
#include "ClaudeUsageStore.hxx"
#include "PriceCatalog.hxx"
#include "McpServer.hxx"
#include "MqttPublisher.hxx"
#include "WebServer.hxx"
#include "DynamicToolRegistry.hxx"
#include "TcpGateway.hxx"
#include "NcursesConsole.hxx"
#include "Version.hxx"
#include "AgentTelemetryDb.hxx"
#include "AgentHookHandler.hxx"
#include "MobileGateway.hxx"
#include "InsightEngine.hxx"
#include "EdaBridge.hxx"
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
              << "  hook [cursor|claude|antigravity]  Process IDE/CLI lifecycle hook event from stdin\n"
              << "  diag [sessions|session|logs|trace] Run local gateway & session diagnostics CLI\n"
              << "  insights [subcommand]  Run usage insights CLI (findings|report|rebuild|export|digest|compare)\n\n"
              << "Options:\n"
              << "  --config <path>        Custom path to configuration file (default: ~/.config/aimon/config.json)\n"
              << "  --profile <profile>    Default MCP tool profile (core, embedded, network, mesh, mail, eda, all)\n"
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
    std::string optProfile;
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

    if (command == "hook") {
        return AgentHookHandler::run(argc - argIdx + 1, &argv[argIdx - 1], std::cin, std::cout);
    }

    if (command == "diag") {
        std::string subcmd = "sessions";
        if (argIdx < argc && argv[argIdx][0] != '-') {
            subcmd = argv[argIdx++];
        }
        std::string targetId;
        if ((subcmd == "session" || subcmd == "trace") && argIdx < argc && argv[argIdx][0] != '-') {
            targetId = argv[argIdx++];
        }

        int limit = 100;
        std::string cidFilter;
        std::string reqFilter;
        std::string catFilter;
        std::string lvlFilter;
        std::string providerFilter;
        std::string customDbPath;

        while (argIdx < argc) {
            std::string arg = argv[argIdx++];
            if (arg == "--limit" && argIdx < argc) {
                limit = std::stoi(argv[argIdx++]);
            } else if ((arg == "--cid" || arg == "--id") && argIdx < argc) {
                cidFilter = argv[argIdx++];
                if (targetId.empty()) targetId = cidFilter;
            } else if (arg == "--req" && argIdx < argc) {
                reqFilter = argv[argIdx++];
                if (targetId.empty()) targetId = reqFilter;
            } else if (arg == "--category" && argIdx < argc) {
                catFilter = argv[argIdx++];
            } else if (arg == "--level" && argIdx < argc) {
                lvlFilter = argv[argIdx++];
            } else if (arg == "--provider" && argIdx < argc) {
                providerFilter = argv[argIdx++];
            } else if (arg == "--db" && argIdx < argc) {
                customDbPath = argv[argIdx++];
            } else if (arg == "--help" || arg == "-h") {
                printUsage(argv[0]);
                return 0;
            } else if (targetId.empty() && !arg.empty() && arg[0] != '-') {
                targetId = arg;
            }
        }

        auto& db = AgentTelemetryDb::getInstance();
        if (!db.open(customDbPath)) {
            std::cerr << "[aimon diag] Failed to open telemetry database.\n";
            return 1;
        }

        if (subcmd == "logs") {
            std::cout << db.queryDiagnostics(limit, cidFilter, catFilter, lvlFilter, reqFilter).dump(2) << "\n";
            return 0;
        } else if (subcmd == "trace") {
            if (targetId.empty()) {
                std::cerr << "[aimon diag] Usage: aimon diag trace <request_id_or_conversation_id>\n";
                return 1;
            }
            std::cout << db.queryTrace(targetId, limit > 0 ? limit : 500).dump(2) << "\n";
            return 0;
        } else if (subcmd == "sessions") {
            std::cout << WebServer::diagnoseSessions(providerFilter).dump(2) << "\n";
            return 0;
        } else if (subcmd == "session") {
            if (targetId.empty() || !AgentTelemetryDb::isValidSessionId(targetId)) {
                std::cerr << "[aimon diag] Invalid or missing session ID.\n";
                return 1;
            }
            auto res = WebServer::inspectSession(targetId);
            std::cout << res.dump(2) << "\n";
            return res.value("ok", true) ? 0 : 1;
        } else {
            std::cerr << "[aimon diag] Unknown subcommand: " << subcmd << "\n";
            return 1;
        }
    }

    if (command == "insights") {
        std::string subcmd = "findings";
        if (argIdx < argc && argv[argIdx][0] != '-') {
            subcmd = argv[argIdx++];
        }
        std::string insPlatform;
        std::string insGroupBy = "project";
        std::string insMinSev;
        std::string insFormat = "jsonl";
        std::string insOutDir;
        std::string insDetector = "cache_hit_collapse";
        int insDays = (subcmd == "digest") ? 7 : ((subcmd == "export") ? 90 : 30);
        int64_t insSplitEpoch = 0;
        bool insFull = false;
        bool insAnon = false;

        if (subcmd == "compare" && argIdx < argc && argv[argIdx][0] != '-') {
            insDetector = argv[argIdx++];
        }

        while (argIdx < argc) {
            std::string arg = argv[argIdx++];
            if (arg == "--config" && argIdx < argc) {
                customConfigPath = argv[argIdx++];
            } else if (arg == "--platform" && argIdx < argc) {
                insPlatform = argv[argIdx++];
            } else if (arg == "--days" && argIdx < argc) {
                insDays = std::stoi(argv[argIdx++]);
            } else if (arg == "--group-by" && argIdx < argc) {
                insGroupBy = argv[argIdx++];
            } else if (arg == "--min-severity" && argIdx < argc) {
                insMinSev = argv[argIdx++];
            } else if (arg == "--format" && argIdx < argc) {
                insFormat = argv[argIdx++];
            } else if (arg == "--dir" && argIdx < argc) {
                insOutDir = argv[argIdx++];
            } else if (arg == "--detector" && argIdx < argc) {
                insDetector = argv[argIdx++];
            } else if (arg == "--split-epoch" && argIdx < argc) {
                insSplitEpoch = std::stoll(argv[argIdx++]);
            } else if (arg == "--full" || arg == "--reset-cursors") {
                insFull = true;
            } else if (arg == "--anonymize") {
                insAnon = true;
            } else if (arg == "--help" || arg == "-h") {
                printUsage(argv[0]);
                return 0;
            }
        }

        ConfigManager configMgr;
        configMgr.load(customConfigPath);
        AimonConfig& cfg = configMgr.getConfig();

        InsightEngine engine;
        engine.setConfig(cfg);
        if (!engine.open(cfg.insights.dbPath)) {
            std::cerr << "[aimon insights] Failed to open " << cfg.insights.dbPath << "\n";
            return 1;
        }

        std::string histPath = cfg.history.dbPath.empty() ?
            (PathUtils::getAimonConfigDir() + "/history.db") : cfg.history.dbPath;
        std::string telPath = PathUtils::getAimonConfigDir() + "/telemetry.db";

        if (subcmd == "rebuild") {
            bool ok = engine.rebuildFromConfig(histPath, telPath, insFull);
            if (!ok) {
                std::cerr << "[aimon insights] Rebuild failed.\n";
                return 1;
            }
            auto rep = engine.getUsageReport("", 30, "project");
            auto ins = engine.getInsights("", 30, "");
            std::cout << nlohmann::json({
                {"ok", true},
                {"db_path", PathUtils::expandHome(cfg.insights.dbPath)},
                {"projects_30d", rep["rows"].size()},
                {"active_findings_30d", ins["findings"].size()}
            }).dump(2) << "\n";
            return 0;
        } else if (subcmd == "findings") {
            std::cout << engine.getInsights(insPlatform, insDays, insMinSev).dump(2) << "\n";
            return 0;
        } else if (subcmd == "report") {
            std::cout << engine.getUsageReport(insPlatform, insDays, insGroupBy).dump(2) << "\n";
            return 0;
        } else if (subcmd == "export") {
            std::cout << engine.exportDataset(insOutDir, insFormat, insDays, insAnon).dump(2) << "\n";
            return 0;
        } else if (subcmd == "digest") {
            std::cout << engine.generateWeeklyDigest(insDays);
            return 0;
        } else if (subcmd == "compare") {
            std::cout << engine.compareExperiment(insDetector, insPlatform, insSplitEpoch).dump(2) << "\n";
            return 0;
        } else if (subcmd == "describe") {
            std::cout << InsightEngine::describeDataset().dump(2) << "\n";
            return 0;
        } else {
            std::cerr << "[aimon insights] Unknown subcommand: " << subcmd << "\n";
            return 1;
        }
    }

    while (argIdx < argc) {
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
        } else if (arg == "--profile" && argIdx < argc) {
            optProfile = argv[argIdx++];
        } else if (arg.rfind("--profile=", 0) == 0) {
            optProfile = arg.substr(10);
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

    InsightEngine insightEngine;
    insightEngine.setConfig(cfg);
    if (cfg.insights.enabled) {
        insightEngine.open(cfg.insights.dbPath);
    }
    int64_t lastInsightRebuildEpoch = 0;

    AgentTelemetryDb::getInstance().open();
    AgentTelemetryDb::getInstance().setDiagnosticsConfig(cfg.diagnostics);
    AgentTelemetryDb::getInstance().rollupAndPrune(
        cfg.diagnostics.telemetryRetentionDays > 0 ? cfg.diagnostics.telemetryRetentionDays : 14);
    int64_t lastTelemetryPruneEpoch = static_cast<int64_t>(std::time(nullptr));
    MobileGateway::getInstance().init(PathUtils::getAimonConfigDir() + "/mobile_devices.json");


    AntigravityCollector agCollector(cfg.antigravity);
    CursorCollector crCollector(cfg.cursor);
    if (cfg.history.enabled) {
        crCollector.setHistoryStore(&historyStore);
    }

    // Claude Code usage: the daemon reads the local transcripts itself. Declared
    // before the poller thread and destroyed after it (the collector joins its
    // price refresh thread in its destructor).
    ClaudeUsageStore claudeStore;
    std::unique_ptr<PriceCatalog> claudePrices;
    std::unique_ptr<ClaudeCollector> claudeCollector;
    if (cfg.claude.enabled) {
        std::string claudeError;
        if (!cfg.claude.validate(claudeError)) {
            std::cerr << "[aimon] Claude collector disabled: " << claudeError << std::endl;
        } else if (!claudeStore.open(cfg.history.dbPath)) {
            std::cerr << "[aimon] Claude collector disabled: cannot open " << cfg.history.dbPath << std::endl;
        } else {
            claudePrices = std::make_unique<PriceCatalog>(
                PathUtils::getAimonConfigDir() + "/claude_prices.json",
                cfg.claude.pricingUrl.empty() ? std::string(PriceCatalog::kDefaultUrl) : cfg.claude.pricingUrl,
                PriceCatalog::httpFetcher());
            claudePrices->loadCache();
            claudeCollector = std::make_unique<ClaudeCollector>(cfg.claude, claudeStore, *claudePrices);
            claudeCollector->startPriceRefresh();
        }
    }
    std::unique_ptr<MqttPublisher> mqttPublisher;

    if (cfg.mqtt.enabled) {
        mqttPublisher = std::make_unique<MqttPublisher>(cfg.mqtt);
        mqttPublisher->start();
    }

    EdaBridge edaBridge(cfg.eda);
    int64_t lastEdaProbeEpoch = 0;
    std::mutex pollMutex;

    auto pollOnce = [&]() {
        std::lock_guard<std::mutex> pollLock(pollMutex);
        AntigravityStatus ag = agCollector.fetchStatus();
        CursorStatus cr = crCollector.fetchStatus();
        ClaudeStatus cl;
        int64_t nowEpoch = static_cast<int64_t>(std::time(nullptr));
        if (claudeCollector) {
            cl = claudeCollector->fetchStatus(nowEpoch);
        }

        EdaBridgeConfig liveEdaCfg = edaBridge.getConfig();
        if (liveEdaCfg.enabled) {
            int probeIntSec = std::max(5, liveEdaCfg.healthPollIntervalSec);
            if (nowEpoch - lastEdaProbeEpoch >= probeIntSec) {
                lastEdaProbeEpoch = nowEpoch;
                edaBridge.probeNow();
            }
        }

        AggregateStatus status;
        status.antigravity = ag;
        status.cursor = cr;
        status.claude = cl;
        status.eda = edaBridge.getStatus();
        status.lastUpdated = std::chrono::system_clock::now();

        stateStore.update(status);
        agCollector.syncTranscriptTelemetry();

        if (nowEpoch - lastTelemetryPruneEpoch >= 21600) {
            lastTelemetryPruneEpoch = nowEpoch;
            int retDays = AgentTelemetryDb::getInstance().getDiagnosticsConfig().telemetryRetentionDays;
            AgentTelemetryDb::getInstance().rollupAndPrune(retDays > 0 ? retDays : 14);
        }

        if (cfg.history.enabled) {
            historyStore.recordSnapshot(status);
        }

        if (cfg.insights.enabled) {
            insightEngine.ingestLiveStatus(status);
            int intervalSec = std::max(60, cfg.insights.refreshIntervalSec);
            if (nowEpoch - lastInsightRebuildEpoch >= intervalSec) {
                lastInsightRebuildEpoch = nowEpoch;
                std::string histPath = cfg.history.dbPath.empty() ?
                    (PathUtils::getAimonConfigDir() + "/history.db") : cfg.history.dbPath;
                std::string telPath = PathUtils::getAimonConfigDir() + "/telemetry.db";
                insightEngine.rebuildFromConfig(histPath, telPath, false);
            }
        }

        if (mqttPublisher) {
            mqttPublisher->publishState(status);
        }
    };

    if (command == "mcp") {
        // Initial synchronous poll for stdio MCP mode
        pollOnce();

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
        edaBridge.attachTcpGateway(&tcpGateway);
        if (cfg.gateway.enabled) {
            tcpGateway.start(cfg.gateway.host, cfg.gateway.port);
        }

        McpServer mcpServer(stateStore, &dynamicRegistry, &tcpGateway);
        mcpServer.setEdaBridge(&edaBridge);
        if (cfg.insights.enabled) {
            mcpServer.setInsightEngine(&insightEngine);
        }
        if (!optProfile.empty()) {
            mcpServer.setDefaultProfile(optProfile);
        }
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

    // Web or Daemon mode: bind TCP Gateway and WebServer before initial pollOnce()
    // so reconnecting SSE MCP clients never experience connection refused during startup.
    DynamicToolRegistry dynamicRegistry;
    TcpGateway tcpGateway(dynamicRegistry);
    edaBridge.attachTcpGateway(&tcpGateway);

    McpServer mcpServer(stateStore, &dynamicRegistry, &tcpGateway);
    mcpServer.setEdaBridge(&edaBridge);
    if (cfg.insights.enabled) {
        mcpServer.setInsightEngine(&insightEngine);
    }
    if (!optProfile.empty()) {
        mcpServer.setDefaultProfile(optProfile);
    }
    WebServer webServer(stateStore, historyStore, cfg.web, [&]() {
        pollOnce();
    }, &mcpServer, &tcpGateway);
    webServer.setEdaBridge(&edaBridge);
    webServer.setConfigManager(&configMgr);
    webServer.setGatewayConfig(cfg.gateway);
    if (cfg.insights.enabled) {
        webServer.setInsightEngine(&insightEngine);
    }

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

    // Initial synchronous poll now that listening sockets are bound
    pollOnce();

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
        console->init();
    } else {
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
