/*
 * Main.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <selfupdate/UpdateInstaller.hxx>
#include "EdaBridgeAppConfig.hxx"
#include "EdaRpcServer.hxx"

#ifdef AIMON_EDA_BRIDGE_WITH_QT
#include <QApplication>
#include "EdaTrayWindow.hxx"
#endif

#ifdef __MINGW32__
extern "C" void _ZSt21ios_base_library_initv() {
    static std::ios_base::Init s_ioinit;
}
#endif

namespace {

std::atomic<bool> gRunning{true};

void handleSignal(int) {
    gRunning.store(false);
}

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --config <path>         Path to persisted JSON config file\n"
              << "  --listen-host <ip>      RPC listen address (default: 0.0.0.0)\n"
              << "  --port <port>           RPC listen port (default: 3892)\n"
              << "  --plugin-port <port>    Loopback GUI plugin relay port on 127.0.0.1 (default: 3893)\n"
              << "  --connect <host:port>   Reverse-connect to aimon TcpGateway (e.g. host:3885)\n"
              << "  --secret <token>        Shared bearer authentication token (or AIMON_EDA_AUTH_SECRET)\n"
              << "  --allowed-root <path>   Workspace sandbox root directory\n"
              << "  --kicad-bin <path>      Custom path to kicad-cli(.exe)\n"
              << "  --freecad-bin <path>    Custom path to FreeCADCmd(.exe)\n"
              << "  --export-eext <path>    Write embedded EasyEDA Pro .eext extension archive and exit\n"
              << "  --deploy-plugins <dir>  Deploy KiCad & FreeCAD plugins under <dir> (%APPDATA%) and exit\n"
              << "  --headless              Run in headless daemon mode without System Tray GUI\n"
              << "  --gui                   Open the aimon System Tray window immediately on startup\n"
              << "  --help                  Show this help message\n";
}

} // anonymous namespace

int main(int argc, char** argv) {
    selfupdate::UpdateInstaller::cleanupStaleBackup();

    std::string configPath = aimon::EdaBridgeAppConfig::defaultConfigFilePath();
    std::string exportEextPath;
    std::string deployPluginsDir;
    bool forceHeadless = false;
    bool forceShowGui = false;

    // Pre-scan for --config, --export-eext, --deploy-plugins, --help
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--export-eext" && i + 1 < argc) {
            exportEextPath = argv[++i];
        } else if (arg == "--deploy-plugins" && i + 1 < argc) {
            deployPluginsDir = argv[++i];
        }
    }

    if (!exportEextPath.empty()) {
        std::string err;
        if (!aimon::EdaBridgeAppConfig::writeEasyEdaEextFile(exportEextPath, err)) {
            std::cerr << "[aimon] Failed to export .eext to " << exportEextPath
                      << ": " << err << "\n";
            return 1;
        }
        std::cout << "[aimon] Exported EasyEDA Pro extension to " << exportEextPath << "\n";
        return 0;
    }

    if (!deployPluginsDir.empty()) {
        std::string kcDir;
        std::string fcDir;
        std::string err1;
        std::string err2;
        bool okKc = aimon::EdaBridgeAppConfig::deployKiCadPlugin(deployPluginsDir, kcDir, err1);
        bool okFc = aimon::EdaBridgeAppConfig::deployFreeCadPlugin(deployPluginsDir, fcDir, err2);
        if (!okKc || !okFc) {
            std::cerr << "[aimon] Plugin deployment error: KiCad=" << (okKc ? "OK" : err1)
                      << " FreeCAD=" << (okFc ? "OK" : err2) << "\n";
            return 1;
        }
        std::cout << "[aimon] Deployed KiCad plugin to " << kcDir
                  << " and FreeCAD plugin to " << fcDir << "\n";
        return 0;
    }

    aimon::EdaBridgeAppSettings settings;
    {
        std::string loadErr;
        aimon::EdaBridgeAppConfig::loadFromFile(configPath, settings, loadErr);
    }

    if (const char* envSec = std::getenv("AIMON_EDA_AUTH_SECRET")) {
        settings.authSecret = envSec;
    }

    std::string connectTarget;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            ++i;
        } else if (arg == "--listen-host" && i + 1 < argc) {
            settings.listenHost = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            settings.rpcPort = std::atoi(argv[++i]);
        } else if (arg == "--plugin-port" && i + 1 < argc) {
            settings.pluginPort = std::atoi(argv[++i]);
        } else if (arg == "--connect" && i + 1 < argc) {
            connectTarget = argv[++i];
        } else if (arg == "--secret" && i + 1 < argc) {
            settings.authSecret = argv[++i];
        } else if (arg == "--allowed-root" && i + 1 < argc) {
            settings.allowedRoot = argv[++i];
        } else if (arg == "--kicad-bin" && i + 1 < argc) {
            settings.kicadBinaryPath = argv[++i];
        } else if (arg == "--freecad-bin" && i + 1 < argc) {
            settings.freecadBinaryPath = argv[++i];
        } else if (arg == "--headless") {
            forceHeadless = true;
        } else if (arg == "--gui") {
            forceShowGui = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

#ifdef _WIN32
    if (settings.autoDeployPlugins) {
        if (const char* appData = std::getenv("APPDATA")) {
            std::string kcDir;
            std::string fcDir;
            std::string err;
            aimon::EdaBridgeAppConfig::deployKiCadPlugin(
                appData, kcDir, err, settings.kicadPluginDir, false);
            aimon::EdaBridgeAppConfig::deployFreeCadPlugin(
                appData, fcDir, err, settings.freecadModDir, false);
        }
        std::error_code ec;
        std::filesystem::path eextOut =
            std::filesystem::path(configPath).parent_path() / "aimon-easyeda-bridge.eext";
        std::string eextErr;
        aimon::EdaBridgeAppConfig::writeEasyEdaEextFile(eextOut.string(), eextErr);
    }
#endif

    aimon::EdaRpcServerOptions opts = aimon::EdaBridgeAppConfig::toServerOptions(settings);

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    aimon::EdaRpcServer server(opts);
    if (!server.start()) {
        std::cerr << "[aimon] Failed to start RPC server on "
                  << opts.listenHost << ":" << opts.port
                  << " or plugin relay on 127.0.0.1:" << opts.pluginPort << "\n";
        return 1;
    }

    std::cout << "[aimon] Listening on " << opts.listenHost << ":" << server.boundPort()
              << " (AppPluginRelay on 127.0.0.1:" << server.boundPluginPort() << ")\n";

    if (!connectTarget.empty()) {
        std::string gwHost = connectTarget;
        int gwPort = settings.gatewayPort;
        size_t colon = connectTarget.rfind(':');
        if (colon != std::string::npos) {
            gwHost = connectTarget.substr(0, colon);
            gwPort = std::atoi(connectTarget.substr(colon + 1).c_str());
        }
        settings.aimonHost = gwHost;
        settings.gatewayPort = gwPort;
        if (server.connectToGateway(gwHost, gwPort)) {
            std::cout << "[aimon] Connected to aimon TcpGateway at "
                      << gwHost << ":" << gwPort << "\n";
        } else {
            std::cerr << "[aimon] Warning: initial gateway connect failed for "
                      << gwHost << ":" << gwPort << "\n";
        }
    } else if (settings.connectionMode == "gateway" ||
               settings.connectionMode == "bidirectional") {
        server.connectToGateway(settings.aimonHost, settings.gatewayPort);
    }

#ifdef AIMON_EDA_BRIDGE_WITH_QT
    if (!forceHeadless) {
        QApplication app(argc, argv);
        QApplication::setApplicationName("aimon");
        QApplication::setApplicationDisplayName("aimon");
        QApplication::setOrganizationName("selfso");
        QApplication::setOrganizationDomain("selfso.com");
        QApplication::setApplicationVersion(QString::fromStdString(settings.version));
        QApplication::setQuitOnLastWindowClosed(false);

        aimon::EdaTrayWindow trayWindow(server, settings, configPath);
        if (!settings.startMinimizedToTray || forceShowGui) {
            trayWindow.showQuotasTab();
        }

        const int rc = app.exec();
        server.stop();
        return rc;
    }
#else
    (void)forceHeadless;
    (void)forceShowGui;
#endif

    while (gRunning.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
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
