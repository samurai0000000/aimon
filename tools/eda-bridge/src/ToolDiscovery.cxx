/*
 * ToolDiscovery.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "ToolDiscovery.hxx"
#include "ProcessLauncher.hxx"
#include "Version.hxx"
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace aimon {

namespace {

std::string trimFirstLine(const std::string& text) {
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) return line;
    }
    return "";
}

std::string findInPath(const std::vector<std::string>& names) {
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return "";
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::istringstream iss(pathEnv);
    std::string dir;
    while (std::getline(iss, dir, sep)) {
        if (dir.empty()) continue;
        for (const auto& name : names) {
            fs::path p = fs::path(dir) / name;
            std::error_code ec;
            if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
                return p.string();
            }
        }
    }
    return "";
}

} // anonymous namespace

DiscoveredToolInfo ToolDiscovery::discoverKiCad(const std::string& customBinaryPath,
                                                const std::string& customSymbolDir,
                                                const std::string& customFootprintDir) {
    DiscoveredToolInfo info;
    info.name = "kicad";
    info.symbolLibDir = customSymbolDir;
    info.footprintLibDir = customFootprintDir;

    if (info.symbolLibDir.empty()) {
        if (const char* envSym = std::getenv("KICAD8_SYMBOL_DIR")) {
            info.symbolLibDir = envSym;
        }
    }
    if (info.footprintLibDir.empty()) {
        if (const char* envFp = std::getenv("KICAD8_FOOTPRINT_DIR")) {
            info.footprintLibDir = envFp;
        }
    }

    std::string bin = customBinaryPath;
    if (bin.empty()) {
        std::vector<std::string> candidates = {
#ifdef _WIN32
            "C:\\Program Files\\KiCad\\8.0\\bin\\kicad-cli.exe",
            "C:\\Program Files\\KiCad\\9.0\\bin\\kicad-cli.exe",
#endif
            "/usr/bin/kicad-cli",
            "/usr/local/bin/kicad-cli"
        };
        for (const auto& c : candidates) {
            std::error_code ec;
            if (fs::exists(c, ec)) {
                bin = c;
                break;
            }
        }
        if (bin.empty()) {
            bin = findInPath({"kicad-cli", "kicad-cli.exe"});
        }
    }

    if (!bin.empty()) {
        std::error_code ec;
        if (fs::exists(bin, ec)) {
            ProcessResult pr = ProcessLauncher::run(bin, {"--version"}, 5000);
            if (pr.launched && pr.exitCode == 0) {
                info.available = true;
                info.binaryPath = bin;
                info.version = trimFirstLine(pr.stdoutText.empty() ? pr.stderrText : pr.stdoutText);
                if (info.version.empty()) info.version = "8.0";
                return info;
            }
        }
    }

    // Native C++17 KiCadSchematicEditor is always built-in even if kicad-cli is not installed
    info.available = true;
    info.binaryPath = bin.empty() ? "builtin:KiCadSchematicEditor" : bin;
    info.version = std::string("KiCad-8-SExp-Engine-") + AIMON_VERSION_STRING;
    return info;
}

DiscoveredToolInfo ToolDiscovery::discoverFreeCad(const std::string& customBinaryPath) {
    DiscoveredToolInfo info;
    info.name = "freecad";

    std::string bin = customBinaryPath;
    if (bin.empty()) {
        std::vector<std::string> candidates = {
#ifdef _WIN32
            "C:\\Program Files\\FreeCAD 1.0\\bin\\FreeCADCmd.exe",
            "C:\\Program Files\\FreeCAD 0.21\\bin\\FreeCADCmd.exe",
#endif
            "/usr/bin/FreeCADCmd",
            "/usr/bin/freecadcmd",
            "/usr/local/bin/FreeCADCmd"
        };
        for (const auto& c : candidates) {
            std::error_code ec;
            if (fs::exists(c, ec)) {
                bin = c;
                break;
            }
        }
        if (bin.empty()) {
            bin = findInPath({"FreeCADCmd", "freecadcmd", "FreeCADCmd.exe"});
        }
    }

    if (!bin.empty()) {
        std::error_code ec;
        if (fs::exists(bin, ec)) {
            ProcessResult pr = ProcessLauncher::run(bin, {"--version"}, 5000);
            if (pr.launched && pr.exitCode == 0) {
                info.available = true;
                info.binaryPath = bin;
                info.version = trimFirstLine(pr.stdoutText.empty() ? pr.stderrText : pr.stdoutText);
                if (info.version.empty()) info.version = "1.0";
                return info;
            }
        }
    }

    info.available = true;
    info.binaryPath = bin.empty() ? "builtin:FreeCadRoboticsEngine" : bin;
    info.version = std::string("FreeCAD-Robotics-") + AIMON_VERSION_STRING;
    return info;
}

DiscoveredToolInfo ToolDiscovery::discoverEasyEda(const std::string& customBinaryPath) {
    DiscoveredToolInfo info;
    info.name = "easyeda";
    info.available = true;
    info.binaryPath = customBinaryPath.empty() ? "builtin:EasyEdaConverter" : customBinaryPath;
    info.version = std::string("EasyEDA-Pro-Bridge-") + AIMON_VERSION_STRING;
    return info;
}

} // namespace aimon

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
