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
#include <iostream>
#include <string>
#include <thread>
#include "EdaRpcServer.hxx"

namespace {

std::atomic<bool> gRunning{true};

void handleSignal(int) {
    gRunning.store(false);
}

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --listen-host <ip>      RPC listen address (default: 0.0.0.0)\n"
              << "  --port <port>           RPC listen port (default: 3892)\n"
              << "  --plugin-port <port>    Loopback GUI plugin relay port on 127.0.0.1 (default: 3893)\n"
              << "  --connect <host:port>   Reverse-connect to aimon TcpGateway (e.g. host:3885)\n"
              << "  --secret <token>        Shared bearer authentication token (or AIMON_EDA_AUTH_SECRET)\n"
              << "  --allowed-root <path>   Workspace sandbox root directory\n"
              << "  --kicad-bin <path>      Custom path to kicad-cli(.exe)\n"
              << "  --freecad-bin <path>    Custom path to FreeCADCmd(.exe)\n"
              << "  --help                  Show this help message\n";
}

} // anonymous namespace

int main(int argc, char** argv) {
    aimon::EdaRpcServerOptions opts;
    if (const char* envSec = std::getenv("AIMON_EDA_AUTH_SECRET")) {
        opts.authSecret = envSec;
    }

    std::string connectTarget;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--listen-host" && i + 1 < argc) {
            opts.listenHost = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            opts.port = std::atoi(argv[++i]);
        } else if (arg == "--plugin-port" && i + 1 < argc) {
            opts.pluginPort = std::atoi(argv[++i]);
        } else if (arg == "--connect" && i + 1 < argc) {
            connectTarget = argv[++i];
        } else if (arg == "--secret" && i + 1 < argc) {
            opts.authSecret = argv[++i];
        } else if (arg == "--allowed-root" && i + 1 < argc) {
            opts.allowedRoot = argv[++i];
        } else if (arg == "--kicad-bin" && i + 1 < argc) {
            opts.kicadBinaryPath = argv[++i];
        } else if (arg == "--freecad-bin" && i + 1 < argc) {
            opts.freecadBinaryPath = argv[++i];
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    aimon::EdaRpcServer server(opts);
    if (!server.start()) {
        std::cerr << "[aimon-eda-bridge] Failed to start RPC server on "
                  << opts.listenHost << ":" << opts.port
                  << " or plugin relay on 127.0.0.1:" << opts.pluginPort << "\n";
        return 1;
    }

    std::cout << "[aimon-eda-bridge] Listening on " << opts.listenHost << ":" << server.boundPort()
              << " (AppPluginRelay on 127.0.0.1:" << server.boundPluginPort() << ")\n";

    if (!connectTarget.empty()) {
        std::string gwHost = connectTarget;
        int gwPort = 3885;
        size_t colon = connectTarget.rfind(':');
        if (colon != std::string::npos) {
            gwHost = connectTarget.substr(0, colon);
            gwPort = std::atoi(connectTarget.substr(colon + 1).c_str());
        }
        if (server.connectToGateway(gwHost, gwPort)) {
            std::cout << "[aimon-eda-bridge] Connected to aimon TcpGateway at "
                      << gwHost << ":" << gwPort << "\n";
        } else {
            std::cerr << "[aimon-eda-bridge] Warning: initial gateway connect failed for "
                      << gwHost << ":" << gwPort << "\n";
        }
    }

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
