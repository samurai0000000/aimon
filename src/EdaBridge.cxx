/*
 * EdaBridge.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "EdaBridge.hxx"
#include "EasyEdaConverter.hxx"
#include "TcpGateway.hxx"
#include "PathUtils.hxx"
#include "Version.hxx"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <regex>
#include <set>
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace aimon {

namespace {

bool hasNullOrDotDotSegment(const std::string& p) {
    if (p.empty() || p.size() > 4096) return true;
    if (p.find('\0') != std::string::npos) return true;
    // Normalize backslashes to forward slashes for segment inspection
    std::string norm = p;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    size_t pos = 0;
    while (pos <= norm.size()) {
        size_t next = norm.find('/', pos);
        std::string seg = (next == std::string::npos) ? norm.substr(pos) : norm.substr(pos, next - pos);
        if (seg == "..") {
            return true;
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return false;
}

bool isPathFieldKey(const std::string& key) {
    if (key == "api_path" || key == "method_path" || key == "key_path") {
        return false;
    }
    static constexpr const char* kSuffixes[] = {
        "_path", "_dir", "_file", "_root", "files", "paths"
    };
    if (key == "path" || key == "file" || key == "directory" || key == "project" ||
        key == "output" || key == "input" || key == "schematic" || key == "pcb") {
        return true;
    }
    for (const char* s : kSuffixes) {
        size_t sLen = std::char_traits<char>::length(s);
        if (key.size() >= sLen && key.compare(key.size() - sLen, sLen, s) == 0) {
            return true;
        }
    }
    return false;
}

} // anonymous namespace

EdaBridge::EdaBridge(const EdaBridgeConfig& config)
    : _config(config), _accelerators(config.accelerators) {
    initRuntimeToolsLocked();
}

void EdaBridge::setConfig(const EdaBridgeConfig& config) {
    std::lock_guard<std::mutex> lock(_mutex);
    _config = config;
    _accelerators.setConfig(config.accelerators);
    _hasRemoteClientAcceleratorCaps = false;
    _lastClientCapsProbeFailEpoch = 0;
    _hadRemoteAcceleratorProvider = false;
    initRuntimeToolsLocked();
}

void EdaBridge::setSimulatedLocalCapabilitiesForTest(const AcceleratorCapabilities& caps) {
    std::lock_guard<std::mutex> lock(_mutex);
    _accelerators.setSimulatedCapabilitiesForTest(caps);
    _hasRemoteClientAcceleratorCaps = false;
    _lastClientCapsProbeFailEpoch = 0;
    _hadRemoteAcceleratorProvider = false;
    initRuntimeToolsLocked();
}

EdaBridgeConfig EdaBridge::getConfig() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _config;
}

void EdaBridge::attachGateway(TcpGateway* gateway) {
    std::lock_guard<std::mutex> lock(_mutex);
    _gateway = gateway;
}

void EdaBridge::initRuntimeToolsLocked() {
    auto preserveStats = [&](const std::string& name, EdaToolRuntimeInfo& dst) {
        for (const auto& old : _toolInfos) {
            if (old.name == name) {
                dst.totalCalls = old.totalCalls;
                dst.successCalls = old.successCalls;
                dst.failedCalls = old.failedCalls;
                dst.lastCallDurationMs = old.lastCallDurationMs;
                dst.avgCallDurationMs = old.avgCallDurationMs;
                dst.lastToolCalled = old.lastToolCalled;
                dst.lastCallEpoch = old.lastCallEpoch;
                dst.lastValidationSummary = old.lastValidationSummary;
                break;
            }
        }
    };

    std::vector<EdaToolRuntimeInfo> updated;

    const auto& localCaps = _accelerators.getCapabilities();
    EdaToolRuntimeInfo acc;
    acc.name = "accelerators";
    acc.enabled = _config.accelerators.enabled;
    acc.resolvedHost = "local (aimon)";
    acc.resolvedPort = 0;
    acc.available = _config.accelerators.enabled;
    acc.pluginConnected = _config.accelerators.enabled;
    acc.binaryPath = localCaps.backend + ": " + localCaps.deviceName;
    acc.version = AIMON_VERSION_STRING;
    acc.activeDocument = localCaps.gpuAvailable
        ? (localCaps.deviceName + " (" + std::to_string(localCaps.computeUnits) + " CUs)")
        : ("CPU C++17 (" + std::to_string(localCaps.cpuThreads) + " threads)");
    acc.lastProbeLatencyMs = 0;
    preserveStats("accelerators", acc);
    updated.push_back(acc);

    EdaToolRuntimeInfo ee;
    ee.name = "easyeda";
    ee.enabled = _config.easyeda.enabled;
    ee.resolvedHost = _config.host;
    ee.resolvedPort = _config.port;
    preserveStats("easyeda", ee);
    updated.push_back(ee);

    EdaToolRuntimeInfo kc;
    kc.name = "kicad";
    kc.enabled = _config.kicad.enabled;
    kc.resolvedHost = _config.host;
    kc.resolvedPort = _config.port;
    preserveStats("kicad", kc);
    updated.push_back(kc);

    EdaToolRuntimeInfo fc;
    fc.name = "freecad";
    fc.enabled = _config.freecad.enabled;
    fc.resolvedHost = _config.host;
    fc.resolvedPort = _config.port;
    preserveStats("freecad", fc);
    updated.push_back(fc);

    _toolInfos = std::move(updated);
}

EdaToolRuntimeInfo* EdaBridge::findToolInfoLocked(const std::string& component) {
    for (auto& info : _toolInfos) {
        if (info.name == component) {
            return &info;
        }
    }
    return nullptr;
}

bool EdaBridge::validatePathSafety(const std::string& pathStr,
                                   std::string& outError) {
    outError.clear();
    if (hasNullOrDotDotSegment(pathStr)) {
        outError = "Invalid, empty, or traversal-containing path: " + pathStr;
        return false;
    }
    return true;
}

bool EdaBridge::validateJsonPaths(nlohmann::json& payload,
                                  std::string& outError) {
    outError.clear();
    if (payload.is_object()) {
        for (auto it = payload.begin(); it != payload.end(); ++it) {
            const std::string& key = it.key();
            nlohmann::json& val = it.value();
            if (val.is_string() && isPathFieldKey(key)) {
                std::string s = val.get<std::string>();
                if (!validatePathSafety(s, outError)) {
                    return false;
                }
                if (s == "~" || s.rfind("~/", 0) == 0) {
                    val = PathUtils::expandHome(s);
                }
            } else if (val.is_array() && isPathFieldKey(key)) {
                for (auto& elem : val) {
                    if (elem.is_string()) {
                        std::string s = elem.get<std::string>();
                        if (!validatePathSafety(s, outError)) {
                            return false;
                        }
                        if (s == "~" || s.rfind("~/", 0) == 0) {
                            elem = PathUtils::expandHome(s);
                        }
                    } else if (elem.is_object() || elem.is_array()) {
                        if (!validateJsonPaths(elem, outError)) {
                            return false;
                        }
                    }
                }
            } else if (val.is_object() || val.is_array()) {
                if (!validateJsonPaths(val, outError)) {
                    return false;
                }
            }
        }
    } else if (payload.is_array()) {
        for (auto& elem : payload) {
            if (elem.is_object() || elem.is_array()) {
                if (!validateJsonPaths(elem, outError)) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool EdaBridge::guardFreecadScript(const std::string& scriptCode,
                                   std::string& outReason) {
    outReason.clear();
    if (scriptCode.empty()) {
        outReason = "Empty FreeCAD script";
        return false;
    }
    if (scriptCode.find('\0') != std::string::npos) {
        outReason = "FreeCAD script contains null byte";
        return false;
    }

    auto isIdentChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };

    // Topological Naming Problem (TNP) guard: reject Face1..Face9999, Edge1..Edge9999, Vertex1..Vertex9999
    static constexpr const char* kTnpPrefixes[] = {"Face", "Edge", "Vertex"};
    for (const char* pfx : kTnpPrefixes) {
        size_t pfxLen = std::char_traits<char>::length(pfx);
        size_t pos = 0;
        while ((pos = scriptCode.find(pfx, pos)) != std::string::npos) {
            bool wordStart = (pos == 0) || !isIdentChar(scriptCode[pos - 1]);
            size_t numStart = pos + pfxLen;
            if (wordStart && numStart < scriptCode.size() &&
                scriptCode[numStart] >= '1' && scriptCode[numStart] <= '9') {
                size_t numEnd = numStart + 1;
                while (numEnd < scriptCode.size() &&
                       std::isdigit(static_cast<unsigned char>(scriptCode[numEnd]))) {
                    ++numEnd;
                }
                bool wordEnd = (numEnd == scriptCode.size()) || !isIdentChar(scriptCode[numEnd]);
                if (wordEnd && (numEnd - numStart) <= 4) {
                    std::string matched = scriptCode.substr(pos, numEnd - pos);
                    outReason = "Rejected by FreeCAD TNP guard: brittle topological reference '" + matched +
                                "' detected; use explicit datum planes, named LCS, or CSG primitives instead";
                    return false;
                }
            }
            pos += pfxLen;
        }
    }

    // OS / shell / network escape guard
    static constexpr const char* kForbiddenTokens[] = {
        "subprocess", "os.system", "os.popen", "socket", "urllib", "http.client",
        "shutil.rmtree", "ctypes", "__import__", "eval(", "exec("
    };
    for (const char* token : kForbiddenTokens) {
        if (scriptCode.find(token) != std::string::npos) {
            outReason = "Rejected by FreeCAD sandbox guard: forbidden module or call '" +
                        std::string(token) + "'";
            return false;
        }
    }

    return true;
}

bool EdaBridge::guardFreecadApiCall(const std::string& moduleName,
                                    const std::string& methodName,
                                    std::string& outReason) {
    outReason.clear();
    static constexpr const char* kAllowedModules[] = {
        "FreeCAD", "Part", "PartDesign", "Sketcher", "Mesh", "MeshPart", "Spreadsheet", "TechDraw", "Import"
    };
    bool modAllowed = false;
    for (const char* m : kAllowedModules) {
        if (moduleName == m) {
            modAllowed = true;
            break;
        }
    }
    if (!modAllowed) {
        outReason = "FreeCAD module '" + moduleName + "' is not in the allowed workbench whitelist";
        return false;
    }
    if (methodName.empty() || methodName.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.") != std::string::npos) {
        outReason = "Invalid FreeCAD method identifier '" + methodName + "'";
        return false;
    }
    if (methodName.find("__") != std::string::npos || methodName == "eval" || methodName == "exec") {
        outReason = "Forbidden FreeCAD method '" + methodName + "'";
        return false;
    }
    return true;
}

bool EdaBridge::guardEasyEdaApiCall(const std::string& apiPath,
                                    std::string& outReason) {
    outReason.clear();
    if (apiPath.empty() || apiPath.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.") != std::string::npos) {
        outReason = "Invalid EasyEDA API identifier";
        return false;
    }

    static constexpr const char* kAllowedPrefixes[] = {
        "eda.sch_",
        "eda.pcb_",
        "eda.lib_",
        "eda.dmt_",
        "eda.drc_",
        "eda.sys_Document"
    };
    for (const char* prefix : kAllowedPrefixes) {
        size_t pLen = std::char_traits<char>::length(prefix);
        if (apiPath.rfind(prefix, 0) == 0 && apiPath.size() > pLen) {
            return true;
        }
    }

    outReason = "EasyEDA API '" + apiPath +
                "' is outside the allowed eda.{sch_*,pcb_*,lib_*,dmt_*,drc_*,sys_Document*} whitelist";
    return false;
}

bool EdaBridge::normalizeEasyEdaNetlistArgs(const std::string& apiPath,
                                            nlohmann::json& argsArray,
                                            std::string& outError) {
    outError.clear();
    std::string normPath = apiPath;
    if (normPath.rfind("eda.", 0) != 0 &&
        (normPath.rfind("sch_", 0) == 0 || normPath.rfind("pcb_", 0) == 0)) {
        normPath = "eda." + normPath;
    }
    const bool isGetNetlist = (normPath == "eda.pcb_Net.getNetlist" ||
                               normPath == "eda.sch_Netlist.getNetlist");
    const bool isSetNetlist = (normPath == "eda.pcb_Net.setNetlist" ||
                               normPath == "eda.sch_Netlist.setNetlist");
    if (!isGetNetlist && !isSetNetlist) {
        return true;
    }

    if (!argsArray.is_array()) {
        argsArray = nlohmann::json::array();
    }
    if (argsArray.empty() || argsArray[0].is_null() ||
        (argsArray[0].is_string() && argsArray[0].get<std::string>().empty())) {
        if (argsArray.empty()) {
            argsArray.push_back("JLCEDA");
        } else {
            argsArray[0] = "JLCEDA";
        }
        return true;
    }
    if (!argsArray[0].is_string()) {
        outError = "EasyEDA netlist format argument (args[0]) must be a string";
        return false;
    }
    std::string fmt = argsArray[0].get<std::string>();
    if (fmt == "JLCEDA_PRO" || fmt == "jlceda_pro" || fmt == "jlceda") {
        fmt = "JLCEDA";
        argsArray[0] = fmt;
    }
    static constexpr const char* kValidNetlistFormats[] = {
        "JLCEDA", "EasyEDA", "PADS", "Protel2", "Allegro", "DISA"
    };
    for (const char* validFmt : kValidNetlistFormats) {
        if (fmt == validFmt) {
            return true;
        }
    }
    outError = "Invalid EasyEDA Pro netlist format '" + fmt +
               "' for " + normPath +
               " (allowed: JLCEDA, EasyEDA, PADS, Protel2, Allegro, DISA)";
    return false;
}

std::string EdaBridge::componentForTool(const std::string& toolName) {
    if (EdaAccelerators::isAcceleratorTool(toolName)) {
        return "accelerators";
    }
    if (toolName.rfind("easyeda_", 0) == 0) {
        return "easyeda";
    }
    if (toolName.rfind("kicad_", 0) == 0) {
        return "kicad";
    }
    if (toolName.rfind("freecad_", 0) == 0) {
        return "freecad";
    }
    return "accelerators";
}

void EdaBridge::recordToolCall(const std::string& component,
                               const std::string& toolName,
                               const std::string& targetDocument,
                               int durationMs,
                               bool success,
                               const std::string& summary,
                               const std::string& errorMessage) {
    std::lock_guard<std::mutex> lock(_mutex);
    int64_t nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    EdaToolRuntimeInfo* info = findToolInfoLocked(component);
    if (info) {
        double prevTotal = static_cast<double>(info->totalCalls) * info->avgCallDurationMs;
        info->totalCalls += 1;
        if (success) {
            info->successCalls += 1;
            info->errorMessage.clear();
        } else {
            info->failedCalls += 1;
            if (!errorMessage.empty()) {
                info->errorMessage = errorMessage;
            }
        }
        info->lastCallDurationMs = durationMs;
        info->avgCallDurationMs = (prevTotal + static_cast<double>(durationMs)) /
                                  static_cast<double>(info->totalCalls);
        info->lastToolCalled = toolName;
        info->lastCallEpoch = nowEpoch;
        if (!summary.empty()) {
            info->lastValidationSummary = summary;
        }
        if (!targetDocument.empty() && component != "accelerators") {
            info->activeDocument = targetDocument;
        }
    }

    EdaToolCallLogEntry entry;
    entry.timestampEpoch = nowEpoch;
    entry.component = component;
    entry.toolName = toolName;
    entry.targetDocument = targetDocument;
    entry.durationMs = durationMs;
    entry.success = success;
    entry.summary = summary.empty() ? errorMessage : summary;

    _recentCalls.push_front(entry);
    while (_recentCalls.size() > 50) {
        _recentCalls.pop_back();
    }
}

EdaBridgeStatus EdaBridge::getStatus() const {
    EdaBridgeStatus st;
    EdaBridgeConfig cfg;
    TcpGateway* gw = nullptr;
    AcceleratorCapabilities localCaps;
    bool hasClientCaps = false;
    AcceleratorCapabilities clientCaps;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        st.enabled = _config.enabled;
        st.connected = _connected;
        st.mode = _config.mode;
        st.activeTransport = _activeTransport;
        st.host = _config.host;
        st.port = _config.port;
        st.authConfigured = !_config.authSecret.empty();
        st.healthPollIntervalSec = _config.healthPollIntervalSec;
        st.lastProbeEpoch = _lastProbeEpoch;
        st.lastLatencyMs = _lastLatencyMs;
        st.remotePlatform = _remotePlatform;
        st.tools = _toolInfos;
        st.recentCalls.assign(_recentCalls.begin(), _recentCalls.end());
        st.configSnapshot = _config.toJson(true);
        st.errorMessage = _lastError;

        cfg = _config;
        gw = _gateway;
        localCaps = _accelerators.getCapabilities();
        hasClientCaps = _hasRemoteClientAcceleratorCaps;
        clientCaps = _remoteClientAcceleratorCaps;
    }

    if (cfg.enabled && cfg.accelerators.enabled) {
        DynamicTool bestTool;
        std::string bestAddr;
        nlohmann::json bestClientCaps = nlohmann::json::object();
        bool hasGw = false;
        if ((cfg.mode == "gateway" || cfg.mode == "bidirectional") && gw != nullptr) {
            hasGw = gw->getBestToolProvider("cad_calc_print_orientation_dfm",
                                            bestTool, bestAddr, &bestClientCaps);
        }
        const int64_t localScore = localCaps.capabilityScore;
        const int64_t gwScore = hasGw ? bestTool.capabilityScore : 0;
        const int64_t clientScore =
            ((cfg.mode == "client" || cfg.mode == "bidirectional") && hasClientCaps)
                ? clientCaps.capabilityScore
                : 0;

        for (auto& info : st.tools) {
            if (info.name != "accelerators") continue;
            if (gwScore > localScore && gwScore >= clientScore) {
                AcceleratorCapabilities gwCaps =
                    (bestClientCaps.is_object() && bestClientCaps.contains("accelerators") &&
                     bestClientCaps["accelerators"].is_object())
                        ? AcceleratorCapabilities::fromJson(bestClientCaps["accelerators"])
                        : AcceleratorCapabilities::fromJson(bestClientCaps);
                std::string backend = !gwCaps.backend.empty()
                    ? gwCaps.backend
                    : (bestTool.gpuAccelerated ? "opencl_gpu" : "cpu_cpp17");
                std::string devName = !bestTool.computeDevice.empty()
                    ? bestTool.computeDevice
                    : gwCaps.deviceName;
                info.resolvedHost = bestAddr + " (gateway)";
                info.resolvedPort = 0;
                info.available = true;
                info.pluginConnected = true;
                info.binaryPath = backend + ": " + devName;
                info.activeDocument = devName + " (score " + std::to_string(bestTool.capabilityScore) + ")";
            } else if (clientScore > localScore && clientScore > gwScore) {
                info.resolvedHost = cfg.host + ":" + std::to_string(cfg.port);
                info.resolvedPort = cfg.port;
                info.available = true;
                info.pluginConnected = true;
                info.binaryPath = clientCaps.backend + ": " + clientCaps.deviceName;
                info.activeDocument = clientCaps.deviceName + " (score " +
                                      std::to_string(clientCaps.capabilityScore) + ")";
            } else {
                info.resolvedHost = "local (aimon)";
                info.resolvedPort = 0;
                info.available = cfg.accelerators.enabled;
                info.pluginConnected = cfg.accelerators.enabled;
                info.binaryPath = localCaps.backend + ": " + localCaps.deviceName;
                info.activeDocument = localCaps.gpuAvailable
                    ? (localCaps.deviceName + " (" + std::to_string(localCaps.computeUnits) + " CUs)")
                    : ("CPU C++17 (" + std::to_string(localCaps.cpuThreads) + " threads)");
            }
            break;
        }
    }

    return st;
}

namespace {

bool sendTcpJsonRpcRequest(const std::string& host,
                           int port,
                           const nlohmann::json& request,
                           int timeoutMs,
                           size_t maxOutputBytes,
                           nlohmann::json& outResponse,
                           std::string& outError,
                           int& outErrorCode) {
    outResponse = nlohmann::json();
    outError.clear();
    outErrorCode = 0;

    if (host.empty() || port <= 0 || port > 65535) {
        outError = "Invalid remote bridge host or port";
        return false;
    }

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        outError = "Failed to resolve remote EDA bridge host: " + host;
        return false;
    }

    int sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        ::freeaddrinfo(res);
        outError = "Failed to create TCP socket";
        return false;
    }

    int flags = ::fcntl(sock, F_GETFL, 0);
    if (flags >= 0) ::fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int cRes = ::connect(sock, res->ai_addr, res->ai_addrlen);
    ::freeaddrinfo(res);

    int connectWaitMs = std::min(timeoutMs > 0 ? timeoutMs : 5000, 3000);
    if (cRes < 0) {
        if (errno == EINPROGRESS) {
            struct pollfd pfd{};
            pfd.fd = sock;
            pfd.events = POLLOUT;
            int pr = ::poll(&pfd, 1, connectWaitMs);
            if (pr <= 0) {
                ::close(sock);
                outError = "Timed out connecting to aimon-eda-bridge at " + host + ":" + portStr;
                return false;
            }
            int soErr = 0;
            socklen_t sLen = sizeof(soErr);
            if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &soErr, &sLen) != 0 || soErr != 0) {
                ::close(sock);
                outError = "Connection refused by aimon-eda-bridge at " + host + ":" + portStr;
                return false;
            }
        } else {
            ::close(sock);
            outError = "Failed to connect to aimon-eda-bridge at " + host + ":" + portStr;
            return false;
        }
    }

    std::string line = request.dump() + "\n";
    size_t sent = 0;
    auto startTp = std::chrono::steady_clock::now();
    while (sent < line.size()) {
        struct pollfd pfd{};
        pfd.fd = sock;
        pfd.events = POLLOUT;
        if (::poll(&pfd, 1, 1000) <= 0) {
            ::close(sock);
            outError = "Timed out sending JSON-RPC request to aimon-eda-bridge";
            return false;
        }
        ssize_t n = ::send(sock, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            ::close(sock);
            outError = "Socket write failed while sending to aimon-eda-bridge";
            return false;
        }
        sent += static_cast<size_t>(n);
    }

    std::string recvBuf;
    char chunk[4096];
    int totalTimeout = (timeoutMs > 0) ? timeoutMs : 60000;
    while (true) {
        auto elapsedMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTp).count());
        int remainingMs = totalTimeout - elapsedMs;
        if (remainingMs <= 0) {
            ::close(sock);
            outError = "Timed out waiting for response from aimon-eda-bridge";
            return false;
        }

        struct pollfd pfd{};
        pfd.fd = sock;
        pfd.events = POLLIN | POLLHUP;
        int pr = ::poll(&pfd, 1, std::min(remainingMs, 200));
        if (pr < 0) {
            if (errno == EINTR) continue;
            ::close(sock);
            outError = "poll() error reading from aimon-eda-bridge";
            return false;
        }
        if (pr == 0) continue;

        ssize_t n = ::recv(sock, chunk, sizeof(chunk), 0);
        if (n > 0) {
            recvBuf.append(chunk, static_cast<size_t>(n));
            if (recvBuf.size() > maxOutputBytes) {
                ::close(sock);
                outError = "Response from aimon-eda-bridge exceeded max_output_bytes";
                return false;
            }
            size_t nlPos = recvBuf.find('\n');
            if (nlPos != std::string::npos) {
                std::string respLine = recvBuf.substr(0, nlPos);
                if (!respLine.empty() && respLine.back() == '\r') respLine.pop_back();
                ::close(sock);

                nlohmann::json parsed = nlohmann::json::parse(respLine, nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object()) {
                    outError = "Malformed JSON-RPC response from aimon-eda-bridge";
                    return false;
                }
                if (parsed.contains("error") && !parsed["error"].is_null()) {
                    if (parsed["error"].is_object()) {
                        outErrorCode = parsed["error"].value("code", -32000);
                        outError = parsed["error"].value("message", parsed["error"].dump());
                    } else {
                        outErrorCode = -32000;
                        outError = parsed["error"].dump();
                    }
                    return false;
                }
                outResponse = parsed.value("result", nlohmann::json::object());
                return true;
            }
        } else if (n == 0) {
            ::close(sock);
            outError = "Remote aimon-eda-bridge closed connection before replying";
            return false;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            ::close(sock);
            outError = "Socket recv error from aimon-eda-bridge";
            return false;
        }
    }
}

std::string toLowerAsciiBridge(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

nlohmann::json introspectEasyEdaApiCatalogInBridge(const nlohmann::json& args) {
    struct ApiEntry {
        const char* apiPath;
        const char* ns;
        const char* method;
        const char* signature;
        const char* returnType;
        const char* description;
        nlohmann::json parameters;
    };

    const std::vector<ApiEntry> kCatalog = {
        {
            "eda.pcb_PrimitiveLine.create",
            "pcb_PrimitiveLine",
            "create",
            "create(net: string, layer: TPCB_LayersOfLine, startX: number, startY: number, endX: number, endY: number, lineWidth?: number, locked?: boolean): Promise<IPCB_PrimitiveLine | undefined>",
            "Promise<IPCB_PrimitiveLine | undefined>",
            "Create a copper track segment on a PCB layer (coordinates and lineWidth in mil: 1 mil = 0.0254 mm).",
            nlohmann::json::array({
                {{"name", "net"}, {"type", "string"}, {"optional", false}},
                {{"name", "layer"}, {"type", "TPCB_LayersOfLine"}, {"optional", false}},
                {{"name", "startX"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "startY"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "endX"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "endY"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "lineWidth"}, {"type", "number (mil)"}, {"optional", true}},
                {{"name", "locked"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_PrimitiveLine.getAll",
            "pcb_PrimitiveLine",
            "getAll",
            "getAll(net?: string, layer?: TPCB_LayersOfLine): Promise<IPCB_PrimitiveLine[]>",
            "Promise<IPCB_PrimitiveLine[]>",
            "Retrieve all copper track line primitives on the active PCB canvas.",
            nlohmann::json::array({
                {{"name", "net"}, {"type", "string"}, {"optional", true}},
                {{"name", "layer"}, {"type", "TPCB_LayersOfLine"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_PrimitiveVia.create",
            "pcb_PrimitiveVia",
            "create",
            "create(net: string, x: number, y: number, holeDiameter?: number, diameter?: number, viaType?: EPCB_PrimitiveViaType, padLayer?: number[], locked?: boolean): Promise<IPCB_PrimitiveVia | undefined>",
            "Promise<IPCB_PrimitiveVia | undefined>",
            "Create an interlayer via on the PCB canvas (x, y, holeDiameter, diameter in mil).",
            nlohmann::json::array({
                {{"name", "net"}, {"type", "string"}, {"optional", false}},
                {{"name", "x"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "y"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "holeDiameter"}, {"type", "number (mil)"}, {"optional", true}},
                {{"name", "diameter"}, {"type", "number (mil)"}, {"optional", true}},
                {{"name", "viaType"}, {"type", "EPCB_PrimitiveViaType"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_PrimitiveVia.getAll",
            "pcb_PrimitiveVia",
            "getAll",
            "getAll(net?: string): Promise<IPCB_PrimitiveVia[]>",
            "Promise<IPCB_PrimitiveVia[]>",
            "Retrieve all via primitives from the active PCB document.",
            nlohmann::json::array({
                {{"name", "net"}, {"type", "string"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_PrimitivePad.getAll",
            "pcb_PrimitivePad",
            "getAll",
            "getAll(layerId?: number, net?: string): Promise<IPCB_PrimitivePad[]>",
            "Promise<IPCB_PrimitivePad[]>",
            "Retrieve all component/board pad primitives with net and coordinate metadata.",
            nlohmann::json::array({
                {{"name", "layerId"}, {"type", "number"}, {"optional", true}},
                {{"name", "net"}, {"type", "string"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_PrimitiveComponent.getAll",
            "pcb_PrimitiveComponent",
            "getAll",
            "getAll(layerId?: EPCB_LayerId, locked?: boolean): Promise<IPCB_PrimitiveComponent[]>",
            "Promise<IPCB_PrimitiveComponent[]>",
            "Retrieve all placed PCB footprint components.",
            nlohmann::json::array({
                {{"name", "layerId"}, {"type", "EPCB_LayerId"}, {"optional", true}},
                {{"name", "locked"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_PrimitiveComponent.modify",
            "pcb_PrimitiveComponent",
            "modify",
            "modify(primitiveId: string, property: Partial<IPCB_PrimitiveComponentProperty>): Promise<IPCB_PrimitiveComponent | undefined>",
            "Promise<IPCB_PrimitiveComponent | undefined>",
            "Move, rotate, flip layer, or update designator of a placed PCB footprint.",
            nlohmann::json::array({
                {{"name", "primitiveId"}, {"type", "string"}, {"optional", false}},
                {{"name", "property"}, {"type", "Partial<IPCB_PrimitiveComponentProperty>"}, {"optional", false}}
            })
        },
        {
            "eda.pcb_PrimitivePour.create",
            "pcb_PrimitivePour",
            "create",
            "create(net: string, layer: EPCB_LayerId, polygon: IPCB_Polygon, pourFillMethod?: EPCB_PrimitivePourFillMethod, preserveSmdPad?: boolean, lineWidth?: number, clearance?: number, pourName?: string, hatchedWidth?: number, hatchedSpacing?: number, locked?: boolean): Promise<IPCB_PrimitivePour | undefined>",
            "Promise<IPCB_PrimitivePour | undefined>",
            "Create a copper pour zone attached to a net on TopLayer(1)/BottomLayer(2)/InnerLayer1(15)/InnerLayer2(16). Note: layer must be numeric EPCB_LayerId and polygon must be created via eda.pcb_MathPolygon.createPolygon in closed single-'L' format [x0, y0, 'L', x1, y1, x2, y2, ..., x0, y0].",
            nlohmann::json::array({
                {{"name", "net"}, {"type", "string"}, {"optional", false}},
                {{"name", "layer"}, {"type", "EPCB_LayerId (number: 1=Top, 2=Bottom, 15=Inner1, 16=Inner2)"}, {"optional", false}},
                {{"name", "polygon"}, {"type", "IPCB_Polygon"}, {"optional", false}},
                {{"name", "pourFillMethod"}, {"type", "EPCB_PrimitivePourFillMethod"}, {"optional", true}},
                {{"name", "preserveSmdPad"}, {"type", "boolean"}, {"optional", true}},
                {{"name", "lineWidth"}, {"type", "number (mil)"}, {"optional", true}},
                {{"name", "clearance"}, {"type", "number (mil)"}, {"optional", true}},
                {{"name", "pourName"}, {"type", "string"}, {"optional", true}},
                {{"name", "hatchedWidth"}, {"type", "number (mil)"}, {"optional", true}},
                {{"name", "hatchedSpacing"}, {"type", "number (mil)"}, {"optional", true}},
                {{"name", "locked"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_MathPolygon.createPolygon",
            "pcb_MathPolygon",
            "createPolygon",
            "createPolygon(flatCommands: (number | string)[]): IPCB_Polygon",
            "IPCB_Polygon",
            "Create an EasyEDA Pro MathPolygon object from canvas mil coordinates in closed single-'L' format [x0, y0, 'L', x1, y1, x2, y2, ..., x0, y0].",
            nlohmann::json::array({
                {{"name", "flatCommands"}, {"type", "(number | string)[]"}, {"optional", false}}
            })
        },
        {
            "eda.pcb_Drc.check",
            "pcb_Drc",
            "check",
            "check(strict?: boolean, userInterface?: boolean, includeVerboseError?: boolean): Promise<boolean | any[]>",
            "Promise<boolean | any[]>",
            "Execute EasyEDA Pro PCB Design Rule Check (DRC) and return structured violations.",
            nlohmann::json::array({
                {{"name", "strict"}, {"type", "boolean"}, {"optional", true}},
                {{"name", "userInterface"}, {"type", "boolean"}, {"optional", true}},
                {{"name", "includeVerboseError"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.pcb_ManufactureData.getGerberFile",
            "pcb_ManufactureData",
            "getGerberFile",
            "getGerberFile(fileName?: string): Promise<File | undefined>",
            "Promise<File | undefined>",
            "Generate JLCPCB-ready Gerber and Excellon drill archive from the active PCB.",
            nlohmann::json::array({
                {{"name", "fileName"}, {"type", "string"}, {"optional", true}}
            })
        },
        {
            "eda.sch_PrimitiveComponent.create",
            "sch_PrimitiveComponent",
            "create",
            "create(component: {libraryUuid: string, uuid: string} | ISCH_NetlistComponent, x: number, y: number, subPartName?: string, rotation?: number, mirror?: boolean, addIntoBom?: boolean, addIntoPcb?: boolean): Promise<ISCH_PrimitiveComponent | undefined>",
            "Promise<ISCH_PrimitiveComponent | undefined>",
            "Place a schematic symbol component onto the active schematic sheet.",
            nlohmann::json::array({
                {{"name", "component"}, {"type", "{libraryUuid: string, uuid: string}"}, {"optional", false}},
                {{"name", "x"}, {"type", "number"}, {"optional", false}},
                {{"name", "y"}, {"type", "number"}, {"optional", false}},
                {{"name", "rotation"}, {"type", "number"}, {"optional", true}},
                {{"name", "mirror"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.sch_PrimitiveComponent.getAll",
            "sch_PrimitiveComponent",
            "getAll",
            "getAll(primitiveType?: ESCH_PrimitiveComponentType, allSchematicPages?: boolean): Promise<ISCH_PrimitiveComponent[]>",
            "Promise<ISCH_PrimitiveComponent[]>",
            "Retrieve all schematic components on the active sheet or across all pages.",
            nlohmann::json::array({
                {{"name", "primitiveType"}, {"type", "ESCH_PrimitiveComponentType"}, {"optional", true}},
                {{"name", "allSchematicPages"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.sch_PrimitiveComponent.getAllPinsByPrimitiveId",
            "sch_PrimitiveComponent",
            "getAllPinsByPrimitiveId",
            "getAllPinsByPrimitiveId(primitiveId: string): Promise<ISCH_PrimitiveComponentPin[] | undefined>",
            "Promise<ISCH_PrimitiveComponentPin[] | undefined>",
            "Retrieve all pins (pinNumber, pinName, x, y, rotation, net) of a placed schematic component.",
            nlohmann::json::array({
                {{"name", "primitiveId"}, {"type", "string"}, {"optional", false}}
            })
        },
        {
            "eda.sch_PrimitiveComponent.createNetFlag",
            "sch_PrimitiveComponent",
            "createNetFlag",
            "createNetFlag(identification: 'Power' | 'Ground' | 'AnalogGround' | 'ProtectGround', net: string, x: number, y: number, rotation?: number, mirror?: boolean): Promise<ISCH_PrimitiveComponent | undefined>",
            "Promise<ISCH_PrimitiveComponent | undefined>",
            "Place a schematic power or ground netFlag (VCC, GND, +3V3) at (x, y) in mil.",
            nlohmann::json::array({
                {{"name", "identification"}, {"type", "'Power' | 'Ground' | 'AnalogGround' | 'ProtectGround'"}, {"optional", false}},
                {{"name", "net"}, {"type", "string"}, {"optional", false}},
                {{"name", "x"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "y"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "rotation"}, {"type", "number"}, {"optional", true}},
                {{"name", "mirror"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.sch_PrimitiveComponent.createNetPort",
            "sch_PrimitiveComponent",
            "createNetPort",
            "createNetPort(direction: 'IN' | 'OUT' | 'BI', net: string, x: number, y: number, rotation?: number, mirror?: boolean): Promise<ISCH_PrimitiveComponent | undefined>",
            "Promise<ISCH_PrimitiveComponent | undefined>",
            "Place a schematic net port label at (x, y) in mil.",
            nlohmann::json::array({
                {{"name", "direction"}, {"type", "'IN' | 'OUT' | 'BI'"}, {"optional", false}},
                {{"name", "net"}, {"type", "string"}, {"optional", false}},
                {{"name", "x"}, {"type", "number (mil)"}, {"optional", false}},
                {{"name", "y"}, {"type", "number (mil)"}, {"optional", false}}
            })
        },
        {
            "eda.sch_PrimitiveWire.create",
            "sch_PrimitiveWire",
            "create",
            "create(line: number[] | number[][], net?: string, color?: string, lineWidth?: number, lineType?: ESCH_PrimitiveLineType): Promise<ISCH_PrimitiveWire | undefined>",
            "Promise<ISCH_PrimitiveWire | undefined>",
            "Create a schematic wire polyline and optionally bind a net label.",
            nlohmann::json::array({
                {{"name", "line"}, {"type", "number[] | number[][]"}, {"optional", false}},
                {{"name", "net"}, {"type", "string"}, {"optional", true}}
            })
        },
        {
            "eda.sch_PrimitiveWire.getAll",
            "sch_PrimitiveWire",
            "getAll",
            "getAll(net?: string, allSchematicPages?: boolean): Promise<ISCH_PrimitiveWire[]>",
            "Promise<ISCH_PrimitiveWire[]>",
            "Retrieve all schematic wire primitives on the active sheet or across all pages.",
            nlohmann::json::array({
                {{"name", "net"}, {"type", "string"}, {"optional", true}},
                {{"name", "allSchematicPages"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.sch_Drc.check",
            "sch_Drc",
            "check",
            "check(strict?: boolean, userInterface?: boolean, includeVerboseError?: boolean): Promise<boolean | any[]>",
            "Promise<boolean | any[]>",
            "Run Schematic Electrical Rule Check (ERC) inside EasyEDA Pro.",
            nlohmann::json::array({
                {{"name", "strict"}, {"type", "boolean"}, {"optional", true}},
                {{"name", "userInterface"}, {"type", "boolean"}, {"optional", true}},
                {{"name", "includeVerboseError"}, {"type", "boolean"}, {"optional", true}}
            })
        },
        {
            "eda.lib_Device.getByLcscIds",
            "lib_Device",
            "getByLcscIds",
            "getByLcscIds(lcscIds: string | string[], libraryUuid?: string): Promise<ILIB_DeviceSearchItem[]>",
            "Promise<ILIB_DeviceSearchItem[]>",
            "Resolve LCSC C-numbers (e.g. C25804) to EasyEDA device UUID and libraryUuid.",
            nlohmann::json::array({
                {{"name", "lcscIds"}, {"type", "string | string[]"}, {"optional", false}},
                {{"name", "libraryUuid"}, {"type", "string"}, {"optional", true}}
            })
        },
        {
            "eda.dmt_SelectControl.getCurrentDocumentInfo",
            "dmt_SelectControl",
            "getCurrentDocumentInfo",
            "getCurrentDocumentInfo(): Promise<IDMT_DocumentInfo | undefined>",
            "Promise<IDMT_DocumentInfo | undefined>",
            "Inspect the currently focused schematic or PCB tab in EasyEDA Pro.",
            nlohmann::json::array()
        }
    };

    bool isDescribe = args.contains("describe_api") && args["describe_api"].is_string() &&
                      !args["describe_api"].get<std::string>().empty();
    std::string rawQuery = isDescribe
        ? args["describe_api"].get<std::string>()
        : args.value("search_api", "");
    std::string qLower = toLowerAsciiBridge(rawQuery);

    nlohmann::json matched = nlohmann::json::array();
    for (const auto& entry : kCatalog) {
        std::string pathLower = toLowerAsciiBridge(entry.apiPath);
        std::string sigLower = toLowerAsciiBridge(entry.signature);
        std::string descLower = toLowerAsciiBridge(entry.description);
        bool hit = false;
        if (isDescribe) {
            hit = (pathLower == qLower) || (pathLower.find(qLower) != std::string::npos);
        } else {
            hit = (pathLower.find(qLower) != std::string::npos) ||
                  (sigLower.find(qLower) != std::string::npos) ||
                  (descLower.find(qLower) != std::string::npos);
        }
        if (hit) {
            std::string notes = std::string(entry.description) + " Signature: " + entry.signature +
                                " (1 mil = 0.0254 mm)";
            matched.push_back({
                {"api_path", entry.apiPath},
                {"namespace", entry.ns},
                {"method", entry.method},
                {"signature", entry.signature},
                {"parameters", entry.parameters},
                {"return_type", entry.returnType},
                {"unit_note", "1 mil = 0.0254 mm (EasyEDA Pro native canvas unit is mil)"},
                {"description", entry.description},
                {"notes", notes}
            });
        }
    }

    return {
        {"ok", true},
        {"offline_introspection", true},
        {"mode", isDescribe ? "describe_api" : "search_api"},
        {"query", rawQuery},
        {"matched_count", matched.size()},
        {"matched_apis", matched}
    };
}

} // anonymous namespace

EdaBridgeStatus EdaBridge::probeNow() {
    EdaBridgeConfig cfg;
    TcpGateway* gw = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _lastProbeEpoch = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        cfg = _config;
        gw = _gateway;
        if (!cfg.enabled) {
            _connected = false;
            _activeTransport = "none";
            _lastLatencyMs = -1;
            _lastError = "Remote Windows EDA bridge is disabled (local CPU accelerators active)";
            return getStatus();
        }
    }

    auto startTp = std::chrono::steady_clock::now();
    nlohmann::json statusPayload;
    std::string probeErr;
    int errCode = 0;
    bool ok = false;
    std::string usedTransport = "none";

    if ((cfg.mode == "client" || cfg.mode == "bidirectional") && !cfg.host.empty() && cfg.port > 0) {
        nlohmann::json req = {
            {"jsonrpc", "2.0"},
            {"id", "probe-1"},
            {"method", "eda/status"},
            {"auth_token", cfg.authSecret},
            {"params", nlohmann::json::object()}
        };
        if (sendTcpJsonRpcRequest(cfg.host, cfg.port, req, 3000,
                                  static_cast<size_t>(cfg.maxOutputBytes),
                                  statusPayload, probeErr, errCode)) {
            ok = true;
            usedTransport = "client";
        }
    }

    if (!ok && (cfg.mode == "gateway" || cfg.mode == "bidirectional") && gw != nullptr) {
        nlohmann::json gwArgs = nlohmann::json::object();
        if (!cfg.authSecret.empty()) {
            gwArgs["_auth_token"] = cfg.authSecret;
        }
        std::string gwErr;
        if (gw->callTool("eda_bridge_status", gwArgs, statusPayload, gwErr, 3000)) {
            ok = true;
            usedTransport = "gateway";
            probeErr.clear();
        } else if (probeErr.empty()) {
            probeErr = gwErr;
        }
    }

    int latencyMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTp).count());

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _connected = ok;
        _activeTransport = ok ? usedTransport : "none";
        _lastLatencyMs = ok ? latencyMs : -1;
        _lastError = ok ? "" : probeErr;

        if (ok && statusPayload.is_object()) {
            _remotePlatform = statusPayload.value("platform", _remotePlatform);
            if (statusPayload.contains("accelerator_capabilities") &&
                statusPayload["accelerator_capabilities"].is_object()) {
                _remoteClientAcceleratorCaps = AcceleratorCapabilities::fromJson(
                    statusPayload["accelerator_capabilities"]);
                _hasRemoteClientAcceleratorCaps = (usedTransport == "client");
                _lastClientCapsProbeFailEpoch = 0;
            }
            const auto& localCaps = _accelerators.getCapabilities();
            if (statusPayload.contains("tools") && statusPayload["tools"].is_array()) {
                for (const auto& rt : statusPayload["tools"]) {
                    if (!rt.is_object()) continue;
                    std::string tName = rt.value("name", "");
                    EdaToolRuntimeInfo* info = findToolInfoLocked(tName);
                    if (!info) continue;
                    if (tName == "accelerators") {
                        int64_t remoteScore = rt.value("capability_score", int64_t(0));
                        if (remoteScore <= 0 && _hasRemoteClientAcceleratorCaps) {
                            remoteScore = _remoteClientAcceleratorCaps.capabilityScore;
                        }
                        if (_config.accelerators.enabled && remoteScore > localCaps.capabilityScore) {
                            info->available = rt.value("available", info->available);
                            info->pluginConnected = rt.value("plugin_connected", true);
                            info->binaryPath = rt.value("binary_path", info->binaryPath);
                            info->version = rt.value("version", info->version);
                            info->activeDocument = rt.value("active_document", info->activeDocument);
                            info->resolvedHost = (usedTransport == "client")
                                ? (cfg.host + ":" + std::to_string(cfg.port))
                                : "gateway";
                            info->resolvedPort = (usedTransport == "client") ? cfg.port : 0;
                            info->lastProbeLatencyMs = latencyMs;
                            info->errorMessage.clear();
                        } else {
                            info->resolvedHost = "local (aimon)";
                            info->resolvedPort = 0;
                            info->available = _config.accelerators.enabled;
                            info->pluginConnected = _config.accelerators.enabled;
                            info->binaryPath = localCaps.backend + ": " + localCaps.deviceName;
                            info->activeDocument = localCaps.gpuAvailable
                                ? (localCaps.deviceName + " (" + std::to_string(localCaps.computeUnits) + " CUs)")
                                : ("CPU C++17 (" + std::to_string(localCaps.cpuThreads) + " threads)");
                            info->lastProbeLatencyMs = 0;
                            info->errorMessage.clear();
                        }
                    } else {
                        info->available = rt.value("available", info->available);
                        info->pluginConnected = rt.value("plugin_connected", false);
                        info->binaryPath = rt.value("binary_path", info->binaryPath);
                        info->version = rt.value("version", info->version);
                        info->activeDocument = rt.value("active_document", info->activeDocument);
                        info->lastProbeLatencyMs = latencyMs;
                        info->errorMessage.clear();
                    }
                }
            }
        } else {
            _hasRemoteClientAcceleratorCaps = false;
            const auto& localCaps = _accelerators.getCapabilities();
            for (auto& info : _toolInfos) {
                if (info.name != "accelerators") {
                    info.available = false;
                    info.pluginConnected = false;
                    info.lastProbeLatencyMs = -1;
                } else {
                    info.resolvedHost = "local (aimon)";
                    info.resolvedPort = 0;
                    info.available = _config.accelerators.enabled;
                    info.pluginConnected = _config.accelerators.enabled;
                    info.binaryPath = localCaps.backend + ": " + localCaps.deviceName;
                    info.activeDocument = localCaps.gpuAvailable
                        ? (localCaps.deviceName + " (" + std::to_string(localCaps.computeUnits) + " CUs)")
                        : ("CPU C++17 (" + std::to_string(localCaps.cpuThreads) + " threads)");
                    info.lastProbeLatencyMs = 0;
                }
            }
        }
    }

    return getStatus();
}

namespace {

// Type-safe readers: nlohmann::json::value() throws when the stored type differs from the
// default's type, so tool arguments are read through these helpers instead.
bool jsonFlag(const nlohmann::json& obj, const char* key, bool defVal) {
    if (!obj.is_object()) return defVal;
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) return defVal;
    return it->get<bool>();
}

int jsonInt(const nlohmann::json& obj, const char* key, int defVal) {
    if (!obj.is_object()) return defVal;
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return defVal;
    return static_cast<int>(it->get<double>());
}

double jsonNum(const nlohmann::json& obj, const char* key, double defVal) {
    if (!obj.is_object()) return defVal;
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return defVal;
    return it->get<double>();
}

std::string jsonStr(const nlohmann::json& obj, const char* key, const std::string& defVal) {
    if (!obj.is_object()) return defVal;
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return defVal;
    return it->get<std::string>();
}

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
}

std::string toUpperAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return s;
}

// eda_calc_pcb_autoroute is dispatched to the live pipeline instead of the stateless
// calculator when the caller asks for it explicitly, supplies a pcb_state snapshot, omits
// nets[], or passes nets[] as plain net-name strings.
bool isLiveAutorouteRequest(const nlohmann::json& args) {
    if (!args.is_object()) return false;
    if (jsonFlag(args, "from_active_pcb", false)) return true;
    if (args.contains("pcb_state") && args["pcb_state"].is_object()) return true;
    if (args.contains("apply_to_pcb") || args.contains("engine")) return true;
    auto it = args.find("nets");
    if (it == args.end()) return true;
    if (!it->is_array() || it->empty()) return false;
    for (const auto& n : *it) {
        if (!n.is_string()) return false;
    }
    return true;
}

nlohmann::json compactConnectivityAudit(const nlohmann::json& audit) {
    nlohmann::json out = nlohmann::json::object();
    if (!audit.is_object()) return out;
    for (const char* k : {"all_connected", "total_nets", "fully_routed_nets", "unrouted_nets_count",
                          "dangling_tracks_count", "unassigned_pads_count", "pours_count", "summary"}) {
        if (audit.contains(k)) out[k] = audit[k];
    }
    nlohmann::json unrouted = nlohmann::json::array();
    if (audit.contains("unrouted_nets") && audit["unrouted_nets"].is_array()) {
        for (const auto& u : audit["unrouted_nets"]) {
            if (!u.is_object()) continue;
            unrouted.push_back({
                {"net", jsonStr(u, "net", "")},
                {"island_count", jsonInt(u, "island_count", 0)}
            });
        }
    }
    out["unrouted_nets"] = std::move(unrouted);
    return out;
}

std::set<std::string> unroutedNetNamesOf(const nlohmann::json& audit) {
    std::set<std::string> names;
    if (audit.is_object() && audit.contains("unrouted_nets") && audit["unrouted_nets"].is_array()) {
        for (const auto& u : audit["unrouted_nets"]) {
            std::string n = jsonStr(u, "net", "");
            if (!n.empty()) names.insert(n);
        }
    }
    return names;
}

// Every board-outline segment becomes a zero-width keepout on all copper layers; the router
// inflates it by clearance + half track (or half via), so no route can cross or hug the edge.
nlohmann::json boardOutlineKeepouts(const nlohmann::json& state) {
    nlohmann::json keepouts = nlohmann::json::array();
    if (!state.is_object() || !state.contains("board_outline") || !state["board_outline"].is_array()) {
        return keepouts;
    }
    for (const auto& seg : state["board_outline"]) {
        if (!seg.is_object()) continue;
        bool complete = true;
        for (const char* k : {"x1", "y1", "x2", "y2"}) {
            if (!seg.contains(k) || !seg[k].is_number()) complete = false;
        }
        if (!complete) continue;
        double x1 = seg["x1"].get<double>();
        double y1 = seg["y1"].get<double>();
        double x2 = seg["x2"].get<double>();
        double y2 = seg["y2"].get<double>();
        keepouts.push_back({
            {"x_min", std::min(x1, x2)},
            {"x_max", std::max(x1, x2)},
            {"y_min", std::min(y1, y2)},
            {"y_max", std::max(y1, y2)},
            {"layer", "MultiLayer"}
        });
    }
    return keepouts;
}

} // anonymous namespace

nlohmann::json EdaBridge::runLiveAutoroute(const std::string& toolName,
                                           const nlohmann::json& arguments) {
    const auto startTp = std::chrono::steady_clock::now();
    const bool viaEditPcb = (toolName == "easyeda_edit_pcb");
    const std::string component = viaEditPcb ? "easyeda" : "accelerators";
    nlohmann::json result = {
        {"ok", false},
        {"tool", toolName},
        {"mode", "live_autoroute"}
    };
    auto finish = [&](bool ok, const std::string& summary, const std::string& error) -> nlohmann::json {
        int elapsedMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTp).count());
        result["ok"] = ok;
        result["elapsed_ms"] = elapsedMs;
        if (!summary.empty()) result["summary"] = summary;
        if (!error.empty()) result["error"] = error;
        recordToolCall(component, toolName, "live PCB autoroute", elapsedMs, ok, summary, error);
        return result;
    };

    std::string engine = "auto";
    if (arguments.contains("engine")) {
        if (!arguments["engine"].is_string()) {
            return finish(false, "", "engine must be one of \"auto\", \"easyeda_native\", \"cpp_steiner\"");
        }
        engine = toLowerAscii(arguments["engine"].get<std::string>());
    }
    if (engine != "auto" && engine != "easyeda_native" && engine != "cpp_steiner") {
        return finish(false, "", "engine must be one of \"auto\", \"easyeda_native\", \"cpp_steiner\" (got \"" +
                                 engine + "\")");
    }
    result["engine"] = engine;

    const bool hasState = arguments.contains("pcb_state") && arguments["pcb_state"].is_object();
    const bool applyToPcb = jsonFlag(arguments, "apply_to_pcb", viaEditPcb);
    const bool nativeFirst = (engine == "easyeda_native") ||
                             (engine == "auto" && jsonFlag(arguments, "native_first", false));
    const bool allowDrcViolations = jsonFlag(arguments, "allow_drc_violations", false);
    const std::string pcbUuid = jsonStr(arguments, "pcb_uuid", "");
    result["source"] = hasState ? "pcb_state" : "active_pcb";
    result["apply_to_pcb"] = applyToPcb;

    if (hasState && nativeFirst) {
        return finish(false, "", "engine \"easyeda_native\" / native_first drives the live EasyEDA Pro GUI and "
                                 "cannot run against an offline pcb_state");
    }

    EdaAccelerators accCopy;
    bool accEnabled = true;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        accEnabled = _config.accelerators.enabled;
        accCopy = _accelerators;
    }
    if (!accEnabled) {
        return finish(false, "", "Local CPU accelerators are disabled in configuration "
                                 "(required for the connectivity audit and cpp_steiner routing)");
    }

    auto withPcbUuid = [&pcbUuid](nlohmann::json a) -> nlohmann::json {
        if (!pcbUuid.empty()) a["pcb_uuid"] = pcbUuid;
        return a;
    };

    // ---- 0. easyeda_edit_pcb(autoroute: true): apply any other requested edits first ------
    if (viaEditPcb) {
        static const char* const kEditKeys[] = {
            "outline", "board_outline", "placements", "move_components", "place_components",
            "footprints", "modify_pads", "pad_nets", "delete_primitives", "tracks", "add_tracks",
            "modify_tracks", "modify_components", "resolve_value_templates", "vias", "add_vias",
            "zones", "copper_pours", "pours", "rebuild_pours"
        };
        nlohmann::json editArgs = nlohmann::json::object();
        bool hasEdits = false;
        for (const char* k : kEditKeys) {
            if (arguments.contains(k)) {
                editArgs[k] = arguments[k];
                hasEdits = true;
            }
        }
        if (hasEdits) {
            if (hasState) {
                return finish(false, "", "easyeda_edit_pcb(autoroute: true) with pcb_state cannot be combined "
                                         "with other edit operations; apply the edits first, then autoroute");
            }
            for (const char* k : {"units", "preserve_connections"}) {
                if (arguments.contains(k)) editArgs[k] = arguments[k];
            }
            nlohmann::json editRes = callTool("easyeda_edit_pcb", withPcbUuid(editArgs));
            result["pre_edit"] = editRes;
            if (!jsonFlag(editRes, "ok", false)) {
                return finish(false, "", "Pre-route easyeda_edit_pcb operations failed: " +
                                         jsonStr(editRes, "error", "unknown error"));
            }
        }
    }

    // ---- 1. Optional EasyEDA Pro native auto-router ---------------------------------------
    if (nativeFirst) {
        int natTimeout = std::clamp(jsonInt(arguments, "native_timeout_ms", 120000), 5000, 600000);
        nlohmann::json natRes = callTool("easyeda_native_autoroute",
                                         withPcbUuid({{"timeout_ms", natTimeout}}));
        bool nativeTriggered = jsonFlag(natRes, "triggered", false);
        result["native"] = natRes;
        if (engine == "easyeda_native" && !nativeTriggered) {
            return finish(false, "", "EasyEDA Pro native auto-router could not be triggered: " +
                                     jsonStr(natRes, "error", "DOM and Win32 UI Automation triggers both failed"));
        }
    }

    // ---- 2. PCB state (live, zero LLM tokens) and connectivity audit ----------------------
    auto fetchLiveState = [&](const nlohmann::json& inspArgs, nlohmann::json& outState,
                              std::string& outErr) -> bool {
        outState = callTool("easyeda_inspect_pcb", withPcbUuid(inspArgs));
        if (!outState.is_object() || !jsonFlag(outState, "ok", false)) {
            outErr = jsonStr(outState, "error", "easyeda_inspect_pcb failed");
            return false;
        }
        return true;
    };

    nlohmann::json state;
    if (hasState) {
        state = arguments["pcb_state"];
    } else {
        std::string err;
        if (!fetchLiveState({{"include_raw_primitives", true}}, state, err)) {
            return finish(false, "", "Failed to read the active EasyEDA Pro PCB: " + err);
        }
    }
    std::string units = toLowerAscii(jsonStr(state, "units", "mil"));
    if (units != "mm" && units != "mil") units = "mil";
    if (!state.contains("units")) state["units"] = units;

    // An empty board is never "nothing to route": it means no PCB is loaded (e.g. the Start Page).
    const int statePads = (state.contains("pads") && state["pads"].is_array())
        ? static_cast<int>(state["pads"].size()) : 0;
    if (statePads == 0) {
        const std::string activeDoc = jsonStr(state, "active_document", "");
        if (!activeDoc.empty()) result["active_document"] = activeDoc;
        return finish(false, "", hasState
            ? std::string("pcb_state contains no pads; nothing can be audited or routed")
            : "The active EasyEDA Pro PCB has no pads (active document: " +
              (activeDoc.empty() ? std::string("unknown") : activeDoc) + "); is a PCB document open?");
    }

    bool auditOk = false;
    std::string auditSummary;
    nlohmann::json preAudit = accCopy.calcPcbConnectivityAudit(state, auditOk, auditSummary);

    if (engine == "easyeda_native") {
        int unrouted = jsonInt(preAudit, "unrouted_nets_count", -1);
        result["post_route_audit"] = compactConnectivityAudit(preAudit);
        return finish(unrouted == 0,
                      "EasyEDA Pro native auto-router finished • " + std::to_string(unrouted) +
                      " unrouted nets remain",
                      unrouted == 0 ? "" : "Unrouted nets remain after the native auto-router");
    }
    result["pre_route_audit"] = compactConnectivityAudit(preAudit);

    // ---- 3. Select nets: unrouted per audit, optional filter, fewest islands first --------
    std::set<std::string> netFilter;
    if (arguments.contains("nets") && arguments["nets"].is_array()) {
        for (const auto& n : arguments["nets"]) {
            if (n.is_string() && !n.get<std::string>().empty()) netFilter.insert(n.get<std::string>());
        }
    }
    struct Candidate {
        std::string name;
        int islands = 2;
        double distMm = 0.0;
    };
    std::vector<Candidate> cands;
    const std::set<std::string> preUnrouted = unroutedNetNamesOf(preAudit);
    if (preAudit.contains("unrouted_nets") && preAudit["unrouted_nets"].is_array()) {
        for (const auto& u : preAudit["unrouted_nets"]) {
            std::string name = jsonStr(u, "net", "");
            if (name.empty()) continue;
            if (!netFilter.empty() && netFilter.count(name) == 0) continue;
            Candidate c;
            c.name = name;
            c.islands = jsonInt(u, "island_count", 2);
            if (u.contains("airwire") && u["airwire"].is_object()) {
                c.distMm = jsonNum(u["airwire"], "distance_mm", 0.0);
            }
            cands.push_back(std::move(c));
        }
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
        if (a.islands != b.islands) return a.islands < b.islands;
        return a.distMm < b.distMm;
    });
    nlohmann::json notUnrouted = nlohmann::json::array();
    for (const auto& f : netFilter) {
        if (preUnrouted.count(f) == 0) notUnrouted.push_back(f);
    }
    if (!notUnrouted.empty()) result["requested_nets_not_unrouted"] = notUnrouted;
    if (cands.empty()) {
        result["routed_nets"] = nlohmann::json::array();
        result["added_tracks_count"] = 0;
        result["added_vias_count"] = 0;
        result["committed"] = false;
        return finish(true, "No unrouted nets to route", "");
    }

    // ---- 4. Resolve design rules (explicit > board_design_rules > existing_copper) -------
    auto positiveFromObj = [](const nlohmann::json& obj,
                              std::initializer_list<const char*> keys) -> double {
        if (!obj.is_object()) return 0.0;
        for (const char* k : keys) {
            if (obj.contains(k) && obj[k].is_number()) {
                double v = obj[k].get<double>();
                if (v > 0.0) return v;
            }
        }
        return 0.0;
    };
    const nlohmann::json argRules =
        (arguments.contains("rules") && arguments["rules"].is_object())
            ? arguments["rules"] : nlohmann::json::object();
    const nlohmann::json boardRules =
        (state.contains("board_design_rules") && state["board_design_rules"].is_object())
            ? state["board_design_rules"]
            : ((state.contains("design_rules") && state["design_rules"].is_object())
                   ? state["design_rules"] : nlohmann::json::object());

    const double bareScale = (units == "mil") ? 0.0254 : 1.0;
    double copperMinTrackMm = 0.0;
    if (state.contains("tracks") && state["tracks"].is_array()) {
        for (const auto& t : state["tracks"]) {
            if (!t.is_object()) continue;
            double wMm = positiveFromObj(t, {"width_mm"});
            if (wMm <= 0.0) {
                double wBare = positiveFromObj(t, {"width"});
                if (wBare > 0.0) wMm = wBare * bareScale;
            }
            if (wMm > 0.0 && (copperMinTrackMm <= 0.0 || wMm < copperMinTrackMm)) {
                copperMinTrackMm = wMm;
            }
        }
    }
    double copperMinViaSizeMm = 0.0;
    double copperMinViaDrillMm = 0.0;
    if (state.contains("vias") && state["vias"].is_array()) {
        for (const auto& v : state["vias"]) {
            if (!v.is_object()) continue;
            double vsMm = positiveFromObj(v, {"size_mm", "diameter_mm"});
            if (vsMm <= 0.0) {
                double vsBare = positiveFromObj(v, {"size", "diameter"});
                if (vsBare > 0.0) vsMm = vsBare * bareScale;
            }
            if (vsMm > 0.0 && (copperMinViaSizeMm <= 0.0 || vsMm < copperMinViaSizeMm)) {
                copperMinViaSizeMm = vsMm;
            }
            double vdMm = positiveFromObj(v, {"drill_mm"});
            if (vdMm <= 0.0) {
                double vdBare = positiveFromObj(v, {"drill", "hole_diameter"});
                if (vdBare > 0.0) vdMm = vdBare * bareScale;
            }
            if (vdMm > 0.0 && (copperMinViaDrillMm <= 0.0 || vdMm < copperMinViaDrillMm)) {
                copperMinViaDrillMm = vdMm;
            }
        }
    }

    double expTrackMm = positiveFromObj(arguments, {"track_width_mm"});
    if (expTrackMm <= 0.0) expTrackMm = positiveFromObj(argRules, {"track_width_mm"});
    double expClearMm = positiveFromObj(arguments, {"clearance_mm"});
    if (expClearMm <= 0.0) expClearMm = positiveFromObj(argRules, {"clearance_mm"});
    double expViaSizeMm = positiveFromObj(arguments, {"via_size_mm", "via_diameter_mm"});
    if (expViaSizeMm <= 0.0) expViaSizeMm = positiveFromObj(argRules, {"via_size_mm", "via_diameter_mm"});
    double expViaDrillMm = positiveFromObj(arguments, {"via_drill_mm"});
    if (expViaDrillMm <= 0.0) expViaDrillMm = positiveFromObj(argRules, {"via_drill_mm"});

    double brdTrackMm = positiveFromObj(boardRules, {"track_width_mm"});
    double brdClearMm = positiveFromObj(boardRules, {"clearance_mm"});
    double brdViaSizeMm = positiveFromObj(boardRules, {"via_size_mm", "via_diameter_mm"});
    double brdViaDrillMm = positiveFromObj(boardRules, {"via_drill_mm"});

    const bool hasExplicitRule =
        (expTrackMm > 0.0 || expClearMm > 0.0 || expViaSizeMm > 0.0 || expViaDrillMm > 0.0);
    const bool hasBoardRule =
        (brdTrackMm > 0.0 || brdClearMm > 0.0 || brdViaSizeMm > 0.0 || brdViaDrillMm > 0.0);
    const bool hasExistingCopper = (copperMinTrackMm > 0.0 || copperMinViaSizeMm > 0.0);

    if (!hasExplicitRule && !hasBoardRule && !hasExistingCopper) {
        return finish(false, "",
                      "Cannot determine PCB design rules: board_design_rules is unavailable, "
                      "the board has no existing copper (tracks/vias) to infer rules from, and "
                      "no explicit rules (track_width_mm, clearance_mm, via_size_mm, via_drill_mm) "
                      "were supplied");
    }

    auto r4 = [](double v) { return std::round(v * 1e4) / 1e4; };
    std::string srcTrack = "existing_copper";
    double resolvedTrackMm = (copperMinTrackMm > 0.0) ? copperMinTrackMm : 0.254;
    if (expTrackMm > 0.0) {
        resolvedTrackMm = expTrackMm;
        srcTrack = "explicit";
    } else if (brdTrackMm > 0.0) {
        resolvedTrackMm = brdTrackMm;
        srcTrack = "board_design_rules";
    }

    std::string srcClear = "existing_copper";
    double resolvedClearMm = 0.16;
    if (expClearMm > 0.0) {
        resolvedClearMm = expClearMm;
        srcClear = "explicit";
    } else if (brdClearMm > 0.0) {
        resolvedClearMm = brdClearMm;
        srcClear = "board_design_rules";
    }

    std::string srcViaSize = "existing_copper";
    double resolvedViaSizeMm = (copperMinViaSizeMm > 0.0) ? std::max(copperMinViaSizeMm, 0.61) : 0.61;
    if (expViaSizeMm > 0.0) {
        resolvedViaSizeMm = expViaSizeMm;
        srcViaSize = "explicit";
    } else if (brdViaSizeMm > 0.0) {
        resolvedViaSizeMm = brdViaSizeMm;
        srcViaSize = "board_design_rules";
    }

    std::string srcViaDrill = "existing_copper";
    double resolvedViaDrillMm = (copperMinViaDrillMm > 0.0) ? std::max(copperMinViaDrillMm, 0.305) : 0.305;
    if (expViaDrillMm > 0.0) {
        resolvedViaDrillMm = expViaDrillMm;
        srcViaDrill = "explicit";
    } else if (brdViaDrillMm > 0.0) {
        resolvedViaDrillMm = brdViaDrillMm;
        srcViaDrill = "board_design_rules";
    }

    std::string primaryRuleSource = hasExplicitRule
        ? "explicit"
        : (hasBoardRule ? "board_design_rules" : "existing_copper");
    result["rules_source"] = primaryRuleSource;
    result["rules"] = {
        {"track_width_mm", r4(resolvedTrackMm)},
        {"clearance_mm", r4(resolvedClearMm)},
        {"via_size_mm", r4(resolvedViaSizeMm)},
        {"via_drill_mm", r4(resolvedViaDrillMm)},
        {"sources", {
            {"track_width_mm", srcTrack},
            {"clearance_mm", srcClear},
            {"via_size_mm", srcViaSize},
            {"via_drill_mm", srcViaDrill}
        }}
    };

    // ---- 4b. Build the calcPcbAutoroute request from live primitives ---------------------
    std::map<std::string, nlohmann::json> padsByNet;
    std::map<std::string, std::string> netByUpper;
    nlohmann::json allPads = nlohmann::json::array();
    if (state.contains("pads") && state["pads"].is_array()) {
        for (const auto& p : state["pads"]) {
            if (!p.is_object()) continue;
            allPads.push_back(p);
            std::string n = jsonStr(p, "net", "");
            if (n.empty()) continue;
            auto& bucket = padsByNet[n];
            if (!bucket.is_array()) bucket = nlohmann::json::array();
            bucket.push_back(p);
            netByUpper.emplace(toUpperAscii(n), n);
        }
    }
    nlohmann::json netsReq = nlohmann::json::array();
    nlohmann::json skipped = nlohmann::json::array();
    std::vector<std::string> requestedOrder;
    for (const auto& c : cands) {
        auto it = padsByNet.find(c.name);
        if (it == padsByNet.end()) {
            auto up = netByUpper.find(toUpperAscii(c.name));
            if (up != netByUpper.end()) it = padsByNet.find(up->second);
        }
        if (it == padsByNet.end() || it->second.size() < 2) {
            skipped.push_back({{"net", c.name}, {"reason", "fewer than 2 pads carry this net on the PCB"}});
            continue;
        }
        netsReq.push_back({{"name", it->first}, {"pads", it->second}});
        requestedOrder.push_back(it->first);
    }
    if (!skipped.empty()) result["skipped_nets"] = skipped;
    if (netsReq.empty()) {
        return finish(false, "", "None of the unrouted nets has two or more net-assigned pads on the PCB");
    }

    nlohmann::json req = {
        {"units", units},
        {"nets", netsReq},
        {"all_pads", allPads},
        {"existing_tracks", (state.contains("tracks") && state["tracks"].is_array())
                                ? state["tracks"] : nlohmann::json::array()},
        {"existing_vias", (state.contains("vias") && state["vias"].is_array())
                              ? state["vias"] : nlohmann::json::array()},
        {"track_width_mm", resolvedTrackMm},
        {"clearance_mm", resolvedClearMm},
        {"via_size_mm", resolvedViaSizeMm},
        {"via_drill_mm", resolvedViaDrillMm}
    };
    for (const char* k : {"grid_step_mm", "max_ripup_passes", "allow_via_in_pad", "board",
                          "obstacles"}) {
        if (arguments.contains(k)) req[k] = arguments[k];
    }
    for (const char* pourKey : {"pours", "zones", "copper_pours"}) {
        if (state.contains(pourKey) && state[pourKey].is_array()) {
            req[pourKey] = state[pourKey];
        }
    }
    if (argRules.contains("grid_step_mm")) req["grid_step_mm"] = argRules["grid_step_mm"];
    if (argRules.contains("max_ripup_passes")) req["max_ripup_passes"] = argRules["max_ripup_passes"];

    nlohmann::json warnings = nlohmann::json::array();
    nlohmann::json keepouts = (arguments.contains("keepouts") && arguments["keepouts"].is_array())
        ? arguments["keepouts"] : nlohmann::json::array();
    if (jsonFlag(arguments, "respect_board_outline", true)) {
        nlohmann::json edge = boardOutlineKeepouts(state);
        result["board_outline_keepouts"] = static_cast<int>(edge.size());
        if (edge.empty()) {
            warnings.push_back("board_outline is empty; routing is constrained only by the copper bounding box");
        }
        for (auto& k : edge) keepouts.push_back(std::move(k));
    }
    if (!keepouts.empty()) req["keepouts"] = std::move(keepouts);
    if (!warnings.empty()) result["warnings"] = warnings;

    bool arOk = false;
    std::string arSummary;
    nlohmann::json ar;
    bool routedRemotely = false;
    {
        TcpGateway* gw = nullptr;
        EdaBridgeConfig cfg;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            gw = _gateway;
            cfg = _config;
        }
        const int64_t localScore = accCopy.getCapabilities().scoreForTool("eda_calc_pcb_autoroute");
        DynamicTool gwBestTool;
        std::string gwPeerAddr;
        if (cfg.enabled && (cfg.mode == "gateway" || cfg.mode == "bidirectional") && gw != nullptr &&
            gw->getBestToolProvider("eda_calc_pcb_autoroute", gwBestTool, gwPeerAddr) &&
            gwBestTool.capabilityScore > localScore) {
            nlohmann::json gwArgs = req;
            gwArgs["execution_target"] = "local";
            if (!cfg.authSecret.empty()) gwArgs["_auth_token"] = cfg.authSecret;
            std::string gwErr;
            if (gw->callTool("eda_calc_pcb_autoroute", gwArgs, ar, gwErr, cfg.timeoutMs) &&
                ar.is_object() && !ar.contains("error")) {
                arOk = ar.value("ok", false);
                arSummary = ar.value("summary", "");
                routedRemotely = true;
            }
        }
    }
    if (!routedRemotely) {
        ar = accCopy.calcPcbAutoroute(req, arOk, arSummary);
    }
    if (!ar.is_object() || ar.contains("error")) {
        return finish(false, "", "calcPcbAutoroute failed: " + jsonStr(ar, "error", arSummary));
    }
    nlohmann::json tracks = (ar.contains("tracks") && ar["tracks"].is_array()) ? ar["tracks"] : nlohmann::json::array();
    nlohmann::json vias = (ar.contains("vias") && ar["vias"].is_array()) ? ar["vias"] : nlohmann::json::array();
    nlohmann::json arCompact = ar;
    arCompact.erase("tracks");
    arCompact.erase("vias");
    result["autoroute"] = arCompact;

    std::set<std::string> failedNets;
    if (ar.contains("unrouted_net_names") && ar["unrouted_net_names"].is_array()) {
        for (const auto& n : ar["unrouted_net_names"]) {
            if (n.is_string()) failedNets.insert(n.get<std::string>());
        }
    }
    nlohmann::json routedNets = nlohmann::json::array();
    for (const auto& n : requestedOrder) {
        if (failedNets.count(n) == 0) routedNets.push_back(n);
    }
    result["routed_nets"] = routedNets;
    result["unrouted_net_names"] = nlohmann::json(std::vector<std::string>(failedNets.begin(), failedNets.end()));
    result["added_tracks_count"] = static_cast<int>(tracks.size());
    result["added_vias_count"] = static_cast<int>(vias.size());
    const int drcViolations = jsonInt(ar, "drc_violations", 0);

    if (!applyToPcb) {
        result["committed"] = false;
        result["tracks"] = tracks;
        result["vias"] = vias;
        return finish(arOk, arSummary + " • not applied (apply_to_pcb=false)",
                      arOk ? "" : "Some nets could not be routed cleanly; see unrouted_net_names / autoroute.drc_violations");
    }
    if (drcViolations > 0 && !allowDrcViolations) {
        result["committed"] = false;
        result["tracks"] = tracks;
        result["vias"] = vias;
        return finish(false, arSummary, "Refusing to commit: router reported " + std::to_string(drcViolations) +
                                        " clearance violations among new tracks/vias "
                                        "(pass allow_drc_violations: true to override)");
    }
    if (tracks.empty() && vias.empty()) {
        result["committed"] = false;
        return finish(false, arSummary, "No net could be routed; nothing to commit");
    }

    // ---- 5. Transactional commit, DRC gate, and re-audit ----------------------------------
    nlohmann::json postAudit;
    if (hasState) {
        nlohmann::json merged = state;
        if (!merged.contains("tracks") || !merged["tracks"].is_array()) merged["tracks"] = nlohmann::json::array();
        if (!merged.contains("vias") || !merged["vias"].is_array()) merged["vias"] = nlohmann::json::array();
        nlohmann::json committedIds = nlohmann::json::array();
        size_t seq = 0;
        for (const auto& t : tracks) {
            nlohmann::json copy = t;
            std::string id = "autoroute_t" + std::to_string(++seq);
            copy["id"] = id;
            committedIds.push_back(id);
            merged["tracks"].push_back(std::move(copy));
        }
        seq = 0;
        for (const auto& v : vias) {
            nlohmann::json copy = v;
            std::string id = "autoroute_v" + std::to_string(++seq);
            copy["id"] = id;
            committedIds.push_back(id);
            merged["vias"].push_back(std::move(copy));
        }
        merged["tracks_count"] = static_cast<int>(merged["tracks"].size());
        merged["vias_count"] = static_cast<int>(merged["vias"].size());
        bool postOk = false;
        std::string postSummary;
        postAudit = accCopy.calcPcbConnectivityAudit(merged, postOk, postSummary);
        merged["connectivity_audit"] = postAudit;
        result["pcb_state"] = std::move(merged);
        result["committed"] = true;
        result["committed_ids"] = std::move(committedIds);
        result["commit_target"] = "pcb_state";
    } else {
        std::set<std::string> prePrimitiveIds;
        for (const char* arrKey : {"tracks", "vias"}) {
            if (state.contains(arrKey) && state[arrKey].is_array()) {
                for (const auto& item : state[arrKey]) {
                    if (item.is_object() && item.contains("id") && item["id"].is_string()) {
                        prePrimitiveIds.insert(item["id"].get<std::string>());
                    }
                }
            }
        }

        auto countGeometricDrcViolations = [](const nlohmann::json& drcRes) -> int {
            if (!drcRes.is_object()) return 0;
            int geomCount = 0;
            if (drcRes.contains("violations") && drcRes["violations"].is_array() && !drcRes["violations"].empty()) {
                for (const auto& v : drcRes["violations"]) {
                    if (!v.is_object()) continue;
                    std::string errType = v.value("errorType", v.value("type", ""));
                    std::string ruleName = v.value("ruleName", "");
                    if (errType == "Connection Error" || errType == "Netlist Error" ||
                        ruleName == "Connection Error" || ruleName == "Netlist Error") {
                        continue;
                    }
                    ++geomCount;
                }
                return geomCount;
            }
            if (drcRes.contains("violation_summary") && drcRes["violation_summary"].is_object()) {
                const auto& vs = drcRes["violation_summary"];
                if (vs.contains("by_error_type") && vs["by_error_type"].is_object()) {
                    for (auto it = vs["by_error_type"].begin(); it != vs["by_error_type"].end(); ++it) {
                        if (it.key() == "Connection Error" || it.key() == "Netlist Error") continue;
                        if (it.value().is_number_integer()) {
                            geomCount += it.value().get<int>();
                        }
                    }
                    return geomCount;
                }
            }
            return jsonInt(drcRes, "violation_count", 0);
        };

        int preDrcViolations = 0;
        int preGeomViolations = 0;
        bool preDrcAvailable = false;
        nlohmann::json preDrcRes = callTool("easyeda_run_drc", withPcbUuid({{"target", "pcb"}}));
        if (preDrcRes.is_object() && jsonFlag(preDrcRes, "ok", false)) {
            preDrcAvailable = true;
            preDrcViolations = jsonInt(preDrcRes, "violation_count", 0);
            preGeomViolations = countGeometricDrcViolations(preDrcRes);
            result["pre_route_drc"] = {
                {"passed", jsonFlag(preDrcRes, "passed", preDrcViolations == 0)},
                {"violation_count", preDrcViolations},
                {"geometric_violation_count", preGeomViolations}
            };
            if (preDrcRes.contains("violation_summary")) {
                result["pre_drc_summary"] = preDrcRes["violation_summary"];
            }
        }

        // Router output always carries *_mm keys plus mil bare keys; commit in mil canvas units.
        // Chunked so each plugin round-trip stays well inside the bridge RPC timeout.
        constexpr size_t kCommitChunk = 80;
        int committedTracks = 0;
        int committedVias = 0;
        std::string commitErr;
        std::vector<std::string> committedIds;
        std::set<std::string> committedIdSet;
        auto recordId = [&](const std::string& id) {
            if (!id.empty() && committedIdSet.insert(id).second) {
                committedIds.push_back(id);
            }
        };
        auto commitChunks = [&](const char* key, const nlohmann::json& items, int& counter,
                                const char* countKey) -> bool {
            for (size_t i = 0; i < items.size(); i += kCommitChunk) {
                nlohmann::json chunk = nlohmann::json::array();
                for (size_t j = i; j < std::min(items.size(), i + kCommitChunk); ++j) {
                    chunk.push_back(items[j]);
                }
                nlohmann::json er = callTool("easyeda_edit_pcb", withPcbUuid({{"units", "mil"}, {key, chunk}}));
                if (er.is_object()) {
                    for (const char* idArrKey : {"committed_ids", "created_track_ids", "created_via_ids"}) {
                        if (er.contains(idArrKey) && er[idArrKey].is_array()) {
                            for (const auto& idVal : er[idArrKey]) {
                                if (idVal.is_string()) recordId(idVal.get<std::string>());
                            }
                        }
                    }
                }
                if (!jsonFlag(er, "ok", false)) {
                    commitErr = jsonStr(er, "error", "easyeda_edit_pcb failed");
                    return false;
                }
                counter += jsonInt(er, countKey, static_cast<int>(chunk.size()));
            }
            return true;
        };
        bool commitOk = commitChunks("tracks", tracks, committedTracks, "added_tracks") &&
                        commitChunks("vias", vias, committedVias, "added_vias");

        nlohmann::json post;
        std::string postErr;
        if (fetchLiveState({{"include_raw_primitives", true}}, post, postErr)) {
            if (committedIds.empty()) {
                for (const char* arrKey : {"tracks", "vias"}) {
                    if (post.contains(arrKey) && post[arrKey].is_array()) {
                        for (const auto& item : post[arrKey]) {
                            if (item.is_object() && item.contains("id") && item["id"].is_string()) {
                                const std::string id = item["id"].get<std::string>();
                                if (prePrimitiveIds.count(id) == 0) recordId(id);
                            }
                        }
                    }
                }
            }
            if (jsonInt(post, "pads_count", 0) == 0) {
                result["post_route_audit_error"] = "post-commit read returned 0 pads (active document: " +
                                                   jsonStr(post, "active_document", "unknown") + ")";
            } else {
                postAudit = (post.contains("connectivity_audit") && post["connectivity_audit"].is_object())
                    ? post["connectivity_audit"] : nlohmann::json::object();
            }
        } else {
            result["post_route_audit_error"] = postErr;
        }

        auto rollbackCommitted = [&]() -> int {
            if (committedIds.empty()) return 0;
            nlohmann::json delIds = nlohmann::json::array();
            for (const auto& id : committedIds) delIds.push_back(id);
            nlohmann::json delRes = callTool("easyeda_edit_pcb",
                                             withPcbUuid({{"delete_primitives", delIds}}));
            result["rollback"] = delRes;
            nlohmann::json rbPost;
            std::string rbErr;
            if (fetchLiveState({{"summary_only", true}}, rbPost, rbErr) &&
                rbPost.contains("connectivity_audit") && rbPost["connectivity_audit"].is_object()) {
                postAudit = rbPost["connectivity_audit"];
            }
            return static_cast<int>(committedIds.size());
        };

        result["commit_target"] = "active_pcb";
        result["committed_tracks"] = committedTracks;
        result["committed_vias"] = committedVias;
        result["committed_ids"] = committedIds;

        if (!commitOk) {
            int rbCount = rollbackCommitted();
            result["committed"] = false;
            result["rolled_back"] = (rbCount > 0);
            result["rolled_back_count"] = rbCount;
            result["post_route_audit"] = compactConnectivityAudit(postAudit);
            return finish(false, arSummary, "Commit to EasyEDA Pro failed after " + std::to_string(committedTracks) +
                                            " tracks / " + std::to_string(committedVias) + " vias: " + commitErr +
                                            (rbCount > 0 ? " (rolled back " + std::to_string(rbCount) + " primitives)" : ""));
        }

        int postDrcViolations = 0;
        int postGeomViolations = 0;
        bool postDrcAvailable = false;
        nlohmann::json postDrcRes = callTool("easyeda_run_drc", withPcbUuid({{"target", "pcb"}}));
        if (postDrcRes.is_object() && jsonFlag(postDrcRes, "ok", false)) {
            postDrcAvailable = true;
            postDrcViolations = jsonInt(postDrcRes, "violation_count", 0);
            postGeomViolations = countGeometricDrcViolations(postDrcRes);
            result["post_route_drc"] = {
                {"passed", jsonFlag(postDrcRes, "passed", postDrcViolations == 0)},
                {"violation_count", postDrcViolations},
                {"geometric_violation_count", postGeomViolations},
                {"violations", postDrcRes.value("violations", nlohmann::json::array())}
            };
            if (postDrcRes.contains("violation_summary")) {
                result["post_drc_summary"] = postDrcRes["violation_summary"];
            }
        }
        int preUnroutedCount = jsonInt(preAudit, "unrouted_nets_count", 0);
        int postUnroutedCount = jsonInt(postAudit, "unrouted_nets_count", preUnroutedCount);
        bool shouldRollback = false;
        if (postDrcAvailable && !allowDrcViolations) {
            if (postGeomViolations > (preDrcAvailable ? preGeomViolations : 0)) {
                shouldRollback = true;
            } else if (postDrcViolations > (preDrcAvailable ? preDrcViolations : 0) &&
                       postUnroutedCount >= preUnroutedCount) {
                shouldRollback = true;
            }
        }
        if (shouldRollback) {
            int rbCount = rollbackCommitted();
            result["committed"] = false;
            result["rolled_back"] = true;
            result["rolled_back_count"] = rbCount;
            result["post_route_audit"] = compactConnectivityAudit(postAudit);
            return finish(false, arSummary,
                          "Post-commit EasyEDA Pro DRC reported " + std::to_string(postDrcViolations) +
                          " violations (" + std::to_string(postGeomViolations) + " geometric vs pre-route baseline " +
                          std::to_string(preDrcAvailable ? preGeomViolations : 0) + "); rolled back " +
                          std::to_string(rbCount) +
                          " committed primitives (pass allow_drc_violations: true to override)");
        }
        result["committed"] = true;
        result["rolled_back"] = false;
    }

    result["post_route_audit"] = compactConnectivityAudit(postAudit);
    const std::set<std::string> postUnrouted = unroutedNetNamesOf(postAudit);
    nlohmann::json stillOpen = nlohmann::json::array();
    for (const auto& n : routedNets) {
        if (postUnrouted.count(n.get<std::string>()) > 0) stillOpen.push_back(n);
    }
    result["routed_nets_still_open"] = stillOpen;
    const bool auditAvailable = postAudit.is_object() && postAudit.contains("unrouted_nets_count");
    const int postUnroutedCount = jsonInt(postAudit, "unrouted_nets_count", -1);
    std::string summary = arSummary + " • committed " + std::to_string(tracks.size()) + " tracks / " +
                          std::to_string(vias.size()) + " vias • post-route unrouted nets: " +
                          (auditAvailable ? std::to_string(postUnroutedCount) : std::string("unknown"));
    if (!auditAvailable) {
        return finish(false, summary, "Routes were committed but the post-route connectivity audit is unavailable");
    }
    if (!stillOpen.empty()) {
        return finish(false, summary, std::to_string(stillOpen.size()) +
                                      " routed nets are still reported open by the post-route audit");
    }
    return finish(arOk, summary, arOk ? "" : "Some nets could not be routed; see unrouted_net_names");
}

nlohmann::json EdaBridge::callTool(const std::string& toolName,
                                   const nlohmann::json& arguments) {
    // "refs" / "component_refs" are accepted as aliases for "references" on inspect tools
    if (arguments.is_object() && !arguments.contains("references") &&
        (toolName == "easyeda_inspect_pcb" || toolName == "easyeda_inspect_schematic")) {
        for (const char* alias : {"refs", "component_refs"}) {
            auto it = arguments.find(alias);
            if (it != arguments.end() && it->is_array()) {
                nlohmann::json aliased = arguments;
                aliased["references"] = *it;
                aliased.erase(std::string(alias));
                return callTool(toolName, aliased);
            }
        }
    }
    if ((toolName == "eda_calc_pcb_autoroute" && isLiveAutorouteRequest(arguments)) ||
        (toolName == "easyeda_edit_pcb" && jsonFlag(arguments, "autoroute", false))) {
        return runLiveAutoroute(toolName, arguments);
    }

    auto startTp = std::chrono::steady_clock::now();

    if (EdaAccelerators::isAcceleratorTool(toolName)) {
        EdaAccelerators accCopy;
        bool accEnabled = true;
        EdaBridgeConfig cfg;
        TcpGateway* gw = nullptr;
        bool hasClientCaps = false;
        AcceleratorCapabilities clientCaps;
        int64_t lastClientFailEpoch = 0;
        bool hadRemoteProvider = false;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            accEnabled = _config.accelerators.enabled;
            accCopy = _accelerators;
            cfg = _config;
            gw = _gateway;
            hasClientCaps = _hasRemoteClientAcceleratorCaps;
            clientCaps = _remoteClientAcceleratorCaps;
            lastClientFailEpoch = _lastClientCapsProbeFailEpoch;
            hadRemoteProvider = _hadRemoteAcceleratorProvider;
        }
        if (!accEnabled) {
            recordToolCall("accelerators", toolName, "local", 0, false,
                           "", "Local CPU accelerators are disabled in configuration");
            return {
                {"ok", false},
                {"error", "Local CPU accelerators are disabled in configuration"}
            };
        }

        const bool forceLocal = arguments.is_object() &&
            (arguments.value("execution_target", "") == "local" ||
             arguments.value("_routed_from_aimon", false));
        const bool forceRemote = !forceLocal && arguments.is_object() &&
            (arguments.value("execution_target", "") == "remote");
        const int64_t localScore = accCopy.getCapabilities().scoreForTool(toolName);
        bool fellBackFromRemote = false;

        if (!forceLocal && cfg.enabled) {
            DynamicTool gwBestTool;
            std::string gwPeerAddr;
            nlohmann::json gwClientCaps = nlohmann::json::object();
            int64_t gwScore = 0;
            if ((cfg.mode == "gateway" || cfg.mode == "bidirectional") && gw != nullptr) {
                if (gw->getBestToolProvider(toolName, gwBestTool, gwPeerAddr, &gwClientCaps)) {
                    gwScore = gwBestTool.capabilityScore;
                }
            }

            if (!hasClientCaps && gwScore <= localScore &&
                (cfg.mode == "client" || cfg.mode == "bidirectional") &&
                !cfg.host.empty() && cfg.port > 0) {
                int64_t nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                if (nowEpoch - lastClientFailEpoch >= 3) {
                    nlohmann::json statusReq = {
                        {"jsonrpc", "2.0"},
                        {"id", "acc-probe-1"},
                        {"method", "eda/status"},
                        {"auth_token", cfg.authSecret},
                        {"params", nlohmann::json::object()}
                    };
                    nlohmann::json statusPayload;
                    std::string statusErr;
                    int statusCode = 0;
                    if (sendTcpJsonRpcRequest(cfg.host, cfg.port, statusReq, 500,
                                              static_cast<size_t>(cfg.maxOutputBytes),
                                              statusPayload, statusErr, statusCode) &&
                        statusPayload.is_object() &&
                        statusPayload.contains("accelerator_capabilities") &&
                        statusPayload["accelerator_capabilities"].is_object()) {
                        clientCaps = AcceleratorCapabilities::fromJson(
                            statusPayload["accelerator_capabilities"]);
                        hasClientCaps = true;
                        std::lock_guard<std::mutex> lock(_mutex);
                        _remoteClientAcceleratorCaps = clientCaps;
                        _hasRemoteClientAcceleratorCaps = true;
                        _lastClientCapsProbeFailEpoch = 0;
                    } else {
                        std::lock_guard<std::mutex> lock(_mutex);
                        _lastClientCapsProbeFailEpoch = nowEpoch;
                    }
                }
            }

            int64_t clientScore = 0;
            if ((cfg.mode == "client" || cfg.mode == "bidirectional") && hasClientCaps) {
                clientScore = clientCaps.scoreForTool(toolName);
            }

            const int64_t bestRemoteScore = std::max(gwScore, clientScore);
            if (bestRemoteScore > localScore || (forceRemote && bestRemoteScore > 0)) {
                nlohmann::json forwardArgs = arguments.is_object() ? arguments : nlohmann::json::object();
                forwardArgs["execution_target"] = "local";
                std::string pathErr;
                if (!validateJsonPaths(forwardArgs, pathErr)) {
                    recordToolCall("accelerators", toolName, "", 0, false, "", pathErr);
                    return {{"ok", false}, {"error", pathErr}, {"code", -32602}};
                }

                if (gwScore >= clientScore && gwScore > 0 && gw != nullptr) {
                    nlohmann::json gwArgs = forwardArgs;
                    if (!cfg.authSecret.empty()) {
                        gwArgs["_auth_token"] = cfg.authSecret;
                    }
                    nlohmann::json remoteRes;
                    std::string remoteErr;
                    if (gw->callTool(toolName, gwArgs, remoteRes, remoteErr, cfg.timeoutMs) &&
                        remoteRes.is_object() && remoteRes.value("ok", false)) {
                        {
                            std::lock_guard<std::mutex> lock(_mutex);
                            _connected = true;
                            _activeTransport = "gateway";
                            _hadRemoteAcceleratorProvider = true;
                        }
                        DynamicTool actualTool = gwBestTool;
                        std::string actualAddr = gwPeerAddr;
                        gw->getBestToolProvider(toolName, actualTool, actualAddr);
                        int64_t actualScore = remoteRes.value("capability_score",
                            actualTool.capabilityScore > 0 ? actualTool.capabilityScore : gwScore);
                        std::string devName = remoteRes.value("compute_device", actualTool.computeDevice);
                        remoteRes["routed_via"] = "gateway";
                        remoteRes["routed_to"] = actualAddr;
                        remoteRes["provider_capability_score"] = actualScore;
                        int elapsedMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - startTp).count());
                        std::string rSummary = remoteRes.value("summary", "Executed on remote accelerator");
                        std::string targetDoc = devName.empty() ? actualAddr : (devName + " @ " + actualAddr);
                        recordToolCall("accelerators", toolName, targetDoc, elapsedMs, true, rSummary, "");
                        return remoteRes;
                    }
                    fellBackFromRemote = true;
                } else if (clientScore > 0) {
                    nlohmann::json req = {
                        {"jsonrpc", "2.0"},
                        {"id", "acc-call-1"},
                        {"method", "tools/call"},
                        {"auth_token", cfg.authSecret},
                        {"params", {
                            {"name", toolName},
                            {"arguments", forwardArgs}
                        }}
                    };
                    nlohmann::json remoteRes;
                    std::string remoteErr;
                    int remoteCode = 0;
                    if (sendTcpJsonRpcRequest(cfg.host, cfg.port, req, cfg.timeoutMs,
                                              static_cast<size_t>(cfg.maxOutputBytes),
                                              remoteRes, remoteErr, remoteCode) &&
                        remoteRes.is_object() && remoteRes.value("ok", false)) {
                        {
                            std::lock_guard<std::mutex> lock(_mutex);
                            _connected = true;
                            _activeTransport = "client";
                            _hadRemoteAcceleratorProvider = true;
                        }
                        std::string endpoint = cfg.host + ":" + std::to_string(cfg.port);
                        int64_t actualScore = remoteRes.value("capability_score", clientScore);
                        std::string devName = remoteRes.value("compute_device", clientCaps.deviceName);
                        remoteRes["routed_via"] = "client";
                        remoteRes["routed_to"] = endpoint;
                        remoteRes["provider_capability_score"] = actualScore;
                        int elapsedMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - startTp).count());
                        std::string rSummary = remoteRes.value("summary", "Executed on remote accelerator");
                        std::string targetDoc = devName.empty() ? endpoint : (devName + " @ " + endpoint);
                        recordToolCall("accelerators", toolName, targetDoc, elapsedMs, true, rSummary, "");
                        return remoteRes;
                    }
                    {
                        std::lock_guard<std::mutex> lock(_mutex);
                        _hasRemoteClientAcceleratorCaps = false;
                        _lastClientCapsProbeFailEpoch = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                    }
                    fellBackFromRemote = true;
                }
            } else if (hadRemoteProvider && bestRemoteScore == 0) {
                fellBackFromRemote = true;
            }
        }

        bool ok = false;
        std::string summary;
        nlohmann::json res = accCopy.executeTool(toolName, arguments, ok, summary);
        if (res.is_object()) {
            if (fellBackFromRemote) {
                res["fallback_from_remote"] = true;
                res["routed_via"] = "local_fallback";
            } else if (!res.contains("routed_via")) {
                res["routed_via"] = "local";
            }
            res["provider_capability_score"] = res.value("capability_score", localScore);
        }
        int elapsedMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTp).count());
        recordToolCall("accelerators", toolName, "in-process solver", elapsedMs, ok,
                       summary, ok ? "" : summary);
        return res;
    }

    std::string comp = componentForTool(toolName);
    EdaBridgeConfig cfg;
    TcpGateway* gw = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        cfg = _config;
        gw = _gateway;
    }

    if ((comp == "easyeda" && !cfg.easyeda.enabled) ||
        (comp == "kicad" && !cfg.kicad.enabled) ||
        (comp == "freecad" && !cfg.freecad.enabled)) {
        std::string msg = "Component '" + comp + "' is disabled in EdaBridgeConfig";
        recordToolCall(comp, toolName, "", 0, false, "", msg);
        return {{"ok", false}, {"error", msg}};
    }

    if (toolName == "easyeda_invoke_api" && arguments.is_object()) {
        bool hasDescribe = arguments.contains("describe_api") && arguments["describe_api"].is_string() &&
                           !arguments["describe_api"].get<std::string>().empty();
        bool hasSearch = arguments.contains("search_api") && arguments["search_api"].is_string() &&
                         !arguments["search_api"].get<std::string>().empty();
        if (hasDescribe || hasSearch) {
            nlohmann::json catRes = introspectEasyEdaApiCatalogInBridge(arguments);
            int elapsedMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTp).count());
            recordToolCall(comp, toolName, "api-catalog", elapsedMs, true,
                           "Introspected @jlceda/pro-api-types catalog", "");
            return catRes;
        }
    }

    if (!cfg.enabled) {
        recordToolCall(comp, toolName, "", 0, false, "",
                       "Remote Windows EDA bridge is disabled");
        return {
            {"ok", false},
            {"error", "Remote Windows EDA bridge is disabled"}
        };
    }

    // Client-side pre-flight security & TNP guards
    if (toolName == "freecad_generate_cad" &&
        arguments.is_object() && arguments.contains("script") && arguments["script"].is_string()) {
        std::string reason;
        if (!guardFreecadScript(arguments["script"].get<std::string>(), reason)) {
            recordToolCall(comp, toolName, "", 0, false, "", reason);
            return {{"ok", false}, {"error", reason}, {"code", -32602}};
        }
    }
    if (toolName == "freecad_invoke_api" && arguments.is_object()) {
        std::string mod = arguments.value("module", "");
        std::string meth = arguments.value("method", "");
        std::string reason;
        if (!guardFreecadApiCall(mod, meth, reason)) {
            recordToolCall(comp, toolName, "", 0, false, "", reason);
            return {{"ok", false}, {"error", reason}, {"code", -32602}};
        }
    }
    std::unique_lock<std::recursive_mutex> easyedaSerialLock(_easyedaRpcMutex, std::defer_lock);
    if (comp == "easyeda") {
        easyedaSerialLock.lock();
    }

    // Validate path safety and expand ~ home paths in arguments
    nlohmann::json remoteArgs = arguments.is_object() ? arguments : nlohmann::json::object();
    std::string pathErr;
    if (!validateJsonPaths(remoteArgs, pathErr)) {
        recordToolCall(comp, toolName, "", 0, false, "", pathErr);
        return {{"ok", false}, {"error", pathErr}, {"code", -32602}};
    }

    if (toolName == "easyeda_invoke_api" && remoteArgs.is_object()) {
        std::string apiPath = remoteArgs.value("api_path", remoteArgs.value("api", remoteArgs.value("method", "")));
        std::string reason;
        if (!guardEasyEdaApiCall(apiPath, reason)) {
            recordToolCall(comp, toolName, "", 0, false, "", reason);
            return {{"ok", false}, {"error", reason}, {"code", -32602}};
        }
        nlohmann::json argsArr = (remoteArgs.contains("args") && remoteArgs["args"].is_array())
            ? remoteArgs["args"] : nlohmann::json::array();
        if (!normalizeEasyEdaNetlistArgs(apiPath, argsArr, reason)) {
            recordToolCall(comp, toolName, "", 0, false, "", reason);
            return {{"ok", false}, {"error", reason}, {"code", -32602}};
        }
        if (remoteArgs.contains("args") || !argsArr.empty()) {
            remoteArgs["args"] = argsArr;
        }
        if (apiPath == "eda.sch_PrimitivePin.modify" &&
            remoteArgs.contains("args") && remoteArgs["args"].is_array() &&
            remoteArgs["args"].size() >= 2 && remoteArgs["args"][0].is_string() &&
            remoteArgs["args"][1].is_object()) {
            std::string pid = remoteArgs["args"][0].get<std::string>();
            const auto& pMod = remoteArgs["args"][1];
            if (pMod.contains("noConnected") || pMod.contains("no_connected")) {
                bool noConn = pMod.value("noConnected", pMod.value("no_connected", false));
                std::lock_guard<std::mutex> lock(_mutex);
                _cachedPinNoConnect[pid] = noConn;
            }
        }
    }

    // Default coordinate units to "mil" for EasyEDA schematic and PCB edits
    if (toolName == "easyeda_edit_schematic" || toolName == "easyeda_edit_pcb") {
        if (!remoteArgs.contains("units") || !remoteArgs["units"].is_string() ||
            remoteArgs["units"].get<std::string>().empty()) {
            remoteArgs["units"] = "mil";
        }
    }

    if (toolName == "easyeda_edit_pcb") {
        auto mmToMil = [](double mm) -> double {
            return std::round(mm * 39.37007874 * 1000.0) / 1000.0;
        };
        auto normalizeMmInArray = [&](const char* key) {
            if (!remoteArgs.contains(key) || !remoteArgs[key].is_array()) return;
            for (auto& item : remoteArgs[key]) {
                if (!item.is_object()) continue;
                if (!item.contains("x") && item.contains("x_mm") && item["x_mm"].is_number()) {
                    item["x"] = mmToMil(item["x_mm"].get<double>());
                }
                if (!item.contains("y") && item.contains("y_mm") && item["y_mm"].is_number()) {
                    item["y"] = mmToMil(item["y_mm"].get<double>());
                }
                if (!item.contains("x1") && item.contains("x1_mm") && item["x1_mm"].is_number()) {
                    item["x1"] = mmToMil(item["x1_mm"].get<double>());
                }
                if (!item.contains("y1") && item.contains("y1_mm") && item["y1_mm"].is_number()) {
                    item["y1"] = mmToMil(item["y1_mm"].get<double>());
                }
                if (!item.contains("x2") && item.contains("x2_mm") && item["x2_mm"].is_number()) {
                    item["x2"] = mmToMil(item["x2_mm"].get<double>());
                }
                if (!item.contains("y2") && item.contains("y2_mm") && item["y2_mm"].is_number()) {
                    item["y2"] = mmToMil(item["y2_mm"].get<double>());
                }
                if (!item.contains("width") && item.contains("width_mm") && item["width_mm"].is_number()) {
                    item["width"] = mmToMil(item["width_mm"].get<double>());
                }
                if (!item.contains("to_width") && item.contains("to_width_mm") && item["to_width_mm"].is_number()) {
                    item["to_width"] = mmToMil(item["to_width_mm"].get<double>());
                }
                if (!item.contains("from_width") && item.contains("from_width_mm") && item["from_width_mm"].is_number()) {
                    item["from_width"] = mmToMil(item["from_width_mm"].get<double>());
                }
                if (!item.contains("drill") && item.contains("drill_mm") && item["drill_mm"].is_number()) {
                    item["drill"] = mmToMil(item["drill_mm"].get<double>());
                }
                if (!item.contains("size") && item.contains("size_mm") && item["size_mm"].is_number()) {
                    item["size"] = mmToMil(item["size_mm"].get<double>());
                }
                if (!item.contains("clearance") && item.contains("clearance_mm") && item["clearance_mm"].is_number()) {
                    item["clearance"] = mmToMil(item["clearance_mm"].get<double>());
                }
            }
        };
        if (remoteArgs.contains("delete_ids") && remoteArgs["delete_ids"].is_array()) {
            if (!remoteArgs.contains("delete_primitives") || !remoteArgs["delete_primitives"].is_array()) {
                remoteArgs["delete_primitives"] = nlohmann::json::array();
            }
            for (const auto& item : remoteArgs["delete_ids"]) {
                remoteArgs["delete_primitives"].push_back(item);
            }
        }
        if (remoteArgs.contains("move_components") && !remoteArgs.contains("placements")) {
            remoteArgs["placements"] = remoteArgs["move_components"];
        }
        if (remoteArgs.contains("add_tracks") && !remoteArgs.contains("tracks")) {
            remoteArgs["tracks"] = remoteArgs["add_tracks"];
        }
        if (remoteArgs.contains("tracks") && remoteArgs["tracks"].is_array()) {
            nlohmann::json expandedTracks = nlohmann::json::array();
            for (const auto& trk : remoteArgs["tracks"]) {
                if (!trk.is_object()) continue;
                std::vector<std::pair<double, double>> pts;
                if (trk.contains("points") && trk["points"].is_array()) {
                    for (const auto& p : trk["points"]) {
                        if (p.is_object()) {
                            if (p.contains("x_mm") && p.contains("y_mm")) {
                                pts.push_back({mmToMil(p["x_mm"].get<double>()), mmToMil(p["y_mm"].get<double>())});
                            } else if (p.contains("x") && p.contains("y")) {
                                pts.push_back({p["x"].get<double>(), p["y"].get<double>()});
                            }
                        } else if (p.is_array() && p.size() >= 2 && p[0].is_number() && p[1].is_number()) {
                            pts.push_back({p[0].get<double>(), p[1].get<double>()});
                        }
                    }
                } else if (trk.contains("line") && trk["line"].is_array()) {
                    const auto& lArr = trk["line"];
                    if (!lArr.empty() && lArr[0].is_number()) {
                        for (size_t k = 0; k + 1 < lArr.size(); k += 2) {
                            if (lArr[k].is_number() && lArr[k + 1].is_number()) {
                                pts.push_back({lArr[k].get<double>(), lArr[k + 1].get<double>()});
                            }
                        }
                    } else {
                        for (const auto& pt : lArr) {
                            if (pt.is_array() && pt.size() >= 2 && pt[0].is_number() && pt[1].is_number()) {
                                pts.push_back({pt[0].get<double>(), pt[1].get<double>()});
                            } else if (pt.is_object() && pt.contains("x") && pt.contains("y")) {
                                pts.push_back({pt["x"].get<double>(), pt["y"].get<double>()});
                            }
                        }
                    }
                }

                if (pts.size() >= 2) {
                    for (size_t i = 0; i + 1 < pts.size(); ++i) {
                        double x1 = pts[i].first;
                        double y1 = pts[i].second;
                        double x2 = pts[i + 1].first;
                        double y2 = pts[i + 1].second;
                        if (std::hypot(x2 - x1, y2 - y1) < 1e-3) {
                            continue;
                        }
                        nlohmann::json seg = trk;
                        seg.erase("points");
                        seg.erase("line");
                        seg["x1"] = x1;
                        seg["y1"] = y1;
                        seg["x2"] = x2;
                        seg["y2"] = y2;
                        expandedTracks.push_back(std::move(seg));
                    }
                } else {
                    double x1 = trk.contains("x1_mm") ? mmToMil(trk.value("x1_mm", 0.0)) : trk.value("x1", 0.0);
                    double y1 = trk.contains("y1_mm") ? mmToMil(trk.value("y1_mm", 0.0)) : trk.value("y1", 0.0);
                    double x2 = trk.contains("x2_mm") ? mmToMil(trk.value("x2_mm", 0.0)) : trk.value("x2", 0.0);
                    double y2 = trk.contains("y2_mm") ? mmToMil(trk.value("y2_mm", 0.0)) : trk.value("y2", 0.0);
                    if (std::hypot(x2 - x1, y2 - y1) >= 1e-3) {
                        expandedTracks.push_back(trk);
                    }
                }
            }
            remoteArgs["tracks"] = std::move(expandedTracks);
        }
        if (remoteArgs.contains("add_vias") && !remoteArgs.contains("vias")) {
            remoteArgs["vias"] = remoteArgs["add_vias"];
        }
        normalizeMmInArray("placements");
        normalizeMmInArray("move_components");
        normalizeMmInArray("tracks");
        normalizeMmInArray("add_tracks");
        normalizeMmInArray("modify_tracks");
        normalizeMmInArray("vias");
        normalizeMmInArray("add_vias");
        normalizeMmInArray("pours");
        normalizeMmInArray("zones");
        normalizeMmInArray("copper_pours");
    } else if (toolName == "easyeda_edit_schematic") {
        if (remoteArgs.contains("delete_ids") && remoteArgs["delete_ids"].is_array()) {
            if (!remoteArgs.contains("delete_primitives") || !remoteArgs["delete_primitives"].is_array()) {
                remoteArgs["delete_primitives"] = nlohmann::json::array();
            }
            for (const auto& item : remoteArgs["delete_ids"]) {
                remoteArgs["delete_primitives"].push_back(item);
            }
        }
    }

    bool callerRequestedRawPrimitives = false;
    if (toolName == "easyeda_inspect_pcb") {
        callerRequestedRawPrimitives = remoteArgs.value("include_raw_primitives",
                                           remoteArgs.value("include_primitives", false));
        remoteArgs["include_raw_primitives"] = true;
        remoteArgs["_full_for_bridge"] = true;
        remoteArgs.erase("summary_only");
    } else if (toolName == "easyeda_inspect_schematic") {
        if (remoteArgs.value("summary_only", false) ||
            (remoteArgs.contains("nets") && remoteArgs["nets"].is_array() && !remoteArgs["nets"].empty()) ||
            remoteArgs.contains("net_filter") || remoteArgs.contains("net") ||
            remoteArgs.contains("references") || remoteArgs.contains("reference") || remoteArgs.contains("ref_filter")) {
            remoteArgs["_full_for_bridge"] = true;
            remoteArgs.erase("nets");
        }
        remoteArgs.erase("summary_only");
    } else if (toolName == "easyeda_run_drc") {
        remoteArgs["_full_for_bridge"] = true;
    }

    std::string targetHost = cfg.host;
    int targetPort = cfg.port;
    int timeoutMs = cfg.timeoutMs;
    if (comp == "easyeda" && toolName == "easyeda_native_autoroute") {
        timeoutMs = std::max(timeoutMs, jsonInt(remoteArgs, "timeout_ms", 120000) + 30000);
    } else if (comp == "freecad") {
        timeoutMs = std::max(timeoutMs, 120000);
    }

    auto dispatchRemoteRpc = [&](const std::string& rTool,
                                 const nlohmann::json& rArgs,
                                 nlohmann::json& outRes,
                                 std::string& outErr,
                                 int& outCode) -> bool {
        std::unique_lock<std::recursive_mutex> rpcEasyedaLock(_easyedaRpcMutex, std::defer_lock);
        if (comp == "easyeda" || rTool.rfind("easyeda_", 0) == 0) {
            rpcEasyedaLock.lock();
        }
        outRes = nlohmann::json();
        outErr.clear();
        outCode = 0;
        if ((cfg.mode == "client" || cfg.mode == "bidirectional") && !targetHost.empty() && targetPort > 0) {
            nlohmann::json req = {
                {"jsonrpc", "2.0"},
                {"id", "call-1"},
                {"method", "tools/call"},
                {"auth_token", cfg.authSecret},
                {"params", {
                    {"name", rTool},
                    {"arguments", rArgs}
                }}
            };
            if (sendTcpJsonRpcRequest(targetHost, targetPort, req, timeoutMs,
                                      static_cast<size_t>(cfg.maxOutputBytes),
                                      outRes, outErr, outCode)) {
                std::lock_guard<std::mutex> lock(_mutex);
                _connected = true;
                _activeTransport = "client";
                return true;
            }
        }

        if ((cfg.mode == "gateway" || cfg.mode == "bidirectional") && gw != nullptr && outCode != -32001) {
            nlohmann::json gwArgs = rArgs;
            if (!cfg.authSecret.empty()) {
                gwArgs["_auth_token"] = cfg.authSecret;
            }
            std::string gwErr;
            if (gw->callTool(rTool, gwArgs, outRes, gwErr, timeoutMs)) {
                outErr.clear();
                std::lock_guard<std::mutex> lock(_mutex);
                _connected = true;
                _activeTransport = "gateway";
                return true;
            } else {
                if (gwErr.find("-32001") != std::string::npos) {
                    outCode = -32001;
                } else if (gwErr.find("-32602") != std::string::npos) {
                    outCode = -32602;
                }
                if (outErr.empty()) {
                    outErr = gwErr;
                }
            }
        }
        return false;
    };

    const bool inMemorySchInspect = (toolName == "easyeda_inspect_schematic" &&
                                     remoteArgs.contains("schematic_state") &&
                                     remoteArgs["schematic_state"].is_object());
    const bool inMemoryPcbInspect = (toolName == "easyeda_inspect_pcb" &&
                                     remoteArgs.contains("pcb_state") &&
                                     remoteArgs["pcb_state"].is_object());
    const bool inMemorySchEdit = (toolName == "easyeda_edit_schematic" &&
                                  remoteArgs.contains("schematic_state") &&
                                  remoteArgs["schematic_state"].is_object());
    const bool inMemoryPcbEdit = (toolName == "easyeda_edit_pcb" &&
                                  remoteArgs.contains("pcb_state") &&
                                  remoteArgs["pcb_state"].is_object());
    const bool inMemorySync = (toolName == "easyeda_sync_sch_to_pcb" &&
                               ((remoteArgs.contains("schematic_state") && remoteArgs["schematic_state"].is_object()) ||
                                (remoteArgs.contains("pcb_state") && remoteArgs["pcb_state"].is_object()) ||
                                remoteArgs.value("trigger_native_import", false) ||
                                remoteArgs.value("dry_run", false)));
    const bool inMemoryDrc = (toolName == "easyeda_run_drc" &&
                              ((remoteArgs.contains("drc_state") && remoteArgs["drc_state"].is_object()) ||
                               (remoteArgs.contains("in_memory_drc") && remoteArgs["in_memory_drc"].is_object()) ||
                               (remoteArgs.contains("violations") && remoteArgs["violations"].is_array()) ||
                               (remoteArgs.contains("pcb_state") && remoteArgs["pcb_state"].is_object())));
    const bool easyedaSyncDryRun = (toolName == "easyeda_sync_sch_to_pcb" &&
                                    remoteArgs.value("dry_run", false));

    nlohmann::json rpcResult;
    std::string rpcError;
    int rpcErrorCode = 0;
    bool dispatchedOk = false;
    if (toolName == "easyeda_invoke_api" && jsonFlag(remoteArgs, "dry_run", false)) {
        dispatchedOk = true;
        rpcResult = {
            {"ok", true},
            {"dry_run", true},
            {"api_path", remoteArgs.value("api_path", remoteArgs.value("api", remoteArgs.value("method", "")))},
            {"normalized_args", remoteArgs.value("args", nlohmann::json::array())}
        };
    } else if (easyedaSyncDryRun &&
        ((remoteArgs.contains("schematic_state") && remoteArgs["schematic_state"].is_object() &&
          remoteArgs.contains("pcb_state") && remoteArgs["pcb_state"].is_object()) ||
         _connected || gw != nullptr)) {
        dispatchedOk = true;
        rpcResult = {{"ok", true}, {"dry_run", true}, {"synced", false}};
    } else if (inMemorySync && !_connected && gw == nullptr) {
        dispatchedOk = true;
        rpcResult = {{"ok", true}, {"dry_run", remoteArgs.value("dry_run", false)}, {"synced", false}};
    } else if ((inMemorySchEdit || inMemoryPcbEdit) && !_connected && gw == nullptr) {
        dispatchedOk = true;
        rpcResult = {
            {"ok", true},
            {"active_document", inMemorySchEdit
                ? remoteArgs["schematic_state"].value("active_document", "Sheet1")
                : remoteArgs["pcb_state"].value("active_document", "PCB1")}
        };
    } else if (inMemoryPcbInspect && !_connected && gw == nullptr) {
        dispatchedOk = true;
        rpcResult = remoteArgs["pcb_state"];
        rpcResult["ok"] = true;
        if (!rpcResult.contains("active_document")) {
            rpcResult["active_document"] = "PCB1";
        }
    } else if (inMemorySchInspect && !_connected && gw == nullptr) {
        dispatchedOk = true;
        rpcResult = remoteArgs["schematic_state"];
        rpcResult["ok"] = true;
        if (!rpcResult.contains("active_document")) {
            rpcResult["active_document"] = "Sheet1";
        }
    } else if (inMemoryDrc && !_connected && gw == nullptr) {
        dispatchedOk = true;
        rpcResult = (remoteArgs.contains("drc_state") && remoteArgs["drc_state"].is_object())
            ? remoteArgs["drc_state"] : ((remoteArgs.contains("in_memory_drc") && remoteArgs["in_memory_drc"].is_object())
                ? remoteArgs["in_memory_drc"] : remoteArgs);
        rpcResult["ok"] = true;
        if (!rpcResult.contains("active_document")) {
            rpcResult["active_document"] = "PCB1";
        }
    } else {
        nlohmann::json pluginArgs = remoteArgs;
        if (toolName == "easyeda_edit_pcb" && !inMemoryPcbEdit && pluginArgs.is_object()) {
            pluginArgs.erase("place_components");
            pluginArgs.erase("modify_pads");
            pluginArgs.erase("pad_nets");
        }
        dispatchedOk = dispatchRemoteRpc(toolName, pluginArgs, rpcResult, rpcError, rpcErrorCode);
        if (!dispatchedOk && (inMemorySchEdit || inMemoryPcbEdit)) {
            dispatchedOk = true;
            rpcResult = {
                {"ok", true},
                {"active_document", inMemorySchEdit
                    ? remoteArgs["schematic_state"].value("active_document", "Sheet1")
                    : remoteArgs["pcb_state"].value("active_document", "PCB1")}
            };
        } else if (!dispatchedOk && inMemoryPcbInspect) {
            dispatchedOk = true;
            rpcResult = remoteArgs["pcb_state"];
            rpcResult["ok"] = true;
            if (!rpcResult.contains("active_document")) {
                rpcResult["active_document"] = "PCB1";
            }
        } else if (!dispatchedOk && inMemorySchInspect) {
            dispatchedOk = true;
            rpcResult = remoteArgs["schematic_state"];
            rpcResult["ok"] = true;
            if (!rpcResult.contains("active_document")) {
                rpcResult["active_document"] = "Sheet1";
            }
        } else if (!dispatchedOk && inMemoryDrc) {
            dispatchedOk = true;
            rpcResult = (remoteArgs.contains("drc_state") && remoteArgs["drc_state"].is_object())
                ? remoteArgs["drc_state"] : ((remoteArgs.contains("in_memory_drc") && remoteArgs["in_memory_drc"].is_object())
                    ? remoteArgs["in_memory_drc"] : remoteArgs);
            rpcResult["ok"] = true;
            if (!rpcResult.contains("active_document")) {
                rpcResult["active_document"] = "PCB1";
            }
        } else if (!dispatchedOk && inMemorySync) {
            dispatchedOk = true;
            rpcResult = {{"ok", true}, {"dry_run", remoteArgs.value("dry_run", false)}, {"synced", false}};
        }
    }

    int elapsedMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTp).count());

    if (!dispatchedOk) {
        std::string errMsg = rpcError.empty() ? "Remote Windows EDA bridge is not connected" : rpcError;
        recordToolCall(comp, toolName, "", elapsedMs, false, "", errMsg);
        nlohmann::json errObj = {
            {"ok", false},
            {"error", errMsg}
        };
        if (rpcErrorCode != 0) {
            errObj["error_code"] = rpcErrorCode;
        }
        if (toolName == "easyeda_invoke_api" && remoteArgs.contains("args")) {
            errObj["normalized_args"] = remoteArgs["args"];
        }
        return errObj;
    }

    auto invokeEasyEdaApi = [&](const std::string& apiPath,
                                const nlohmann::json& apiArgs,
                                nlohmann::json& outApiRes) -> bool {
        if ((inMemorySchEdit || inMemoryPcbEdit || inMemoryPcbInspect || inMemorySchInspect) && !_connected && gw == nullptr) {
            outApiRes = {{"ok", true}, {"result", "in_memory_prim"}};
            return true;
        }
        std::string subErr;
        int subCode = 0;
        return dispatchRemoteRpc("easyeda_invoke_api", {
            {"api_path", apiPath},
            {"args", apiArgs}
        }, outApiRes, subErr, subCode);
    };

    auto safeJsonStr = [](const nlohmann::json& obj,
                          const char* k1,
                          const char* k2 = nullptr,
                          const char* k3 = nullptr,
                          const std::string& def = "") -> std::string {
        if (!obj.is_object()) return def;
        const char* keys[3] = {k1, k2, k3};
        for (const char* k : keys) {
            if (!k || !obj.contains(k) || obj[k].is_null()) continue;
            const auto& v = obj[k];
            if (v.is_string()) {
                std::string s = v.get<std::string>();
                if (s == "{\"uuid\":\"\"}" || s == "[object Object]") return "";
                return s;
            }
            if (v.is_object()) {
                for (const char* subKey : {"name", "title", "uuid"}) {
                    if (v.contains(subKey) && v[subKey].is_string()) {
                        std::string sub = v[subKey].get<std::string>();
                        if (!sub.empty()) return sub;
                    }
                }
                return "";
            }
            if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
            if (v.is_number()) return std::to_string(v.get<double>());
        }
        return def;
    };

    auto safeJsonNum = [](const nlohmann::json& obj, const char* k1, double def = 0.0) -> double {
        if (!obj.is_object() || !k1 || !obj.contains(k1) || obj[k1].is_null()) return def;
        const auto& v = obj[k1];
        if (v.is_number()) return v.get<double>();
        if (v.is_string()) {
            try {
                return std::stod(v.get<std::string>());
            } catch (...) {
                return def;
            }
        }
        return def;
    };

    auto sanitizeEasyEdaProject = [&](nlohmann::json& payload) {
        if (!payload.is_object()) return;
        if (payload.contains("project") && payload["project"].is_object()) {
            auto& proj = payload["project"];
            if (proj.contains("data") && proj["data"].is_array()) {
                for (auto& board : proj["data"]) {
                    if (!board.is_object() || !board.contains("schematic") || !board["schematic"].is_object()) {
                        continue;
                    }
                    auto& sch = board["schematic"];
                    if (sch.contains("page") && sch["page"].is_array()) {
                        for (auto& pg : sch["page"]) {
                            if (pg.is_object()) {
                                pg.erase("titleBlockData");
                            }
                        }
                    }
                }
            }
        }
        std::map<std::string, std::string> compRefById;
        for (const char* arrKey : {"components", "schematic_components", "footprints", "pcb_components"}) {
            if (!payload.contains(arrKey) || !payload[arrKey].is_array()) continue;
            for (auto& compObj : payload[arrKey]) {
                if (!compObj.is_object()) continue;
                if (compObj.contains("footprint")) {
                    compObj["footprint"] = safeJsonStr(compObj, "footprint");
                }
                if (compObj.contains("reference")) {
                    compObj["reference"] = safeJsonStr(compObj, "reference", "ref", "designator");
                }
                if (compObj.contains("value")) {
                    compObj["value"] = safeJsonStr(compObj, "value", "name");
                }
                if (compObj.contains("unique_id")) {
                    compObj["unique_id"] = safeJsonStr(compObj, "unique_id", "uniqueId");
                }
                if (compObj.contains("component_type")) {
                    compObj["component_type"] = safeJsonStr(compObj, "component_type", "type", nullptr, "part");
                }
                std::string cid = safeJsonStr(compObj, "id", "primitiveId");
                std::string cref = safeJsonStr(compObj, "reference", "ref", "designator");
                if (!cid.empty() && !cref.empty()) {
                    compRefById[cid] = cref;
                }
            }
        }
        if (payload.contains("pads") && payload["pads"].is_array() && !compRefById.empty()) {
            for (auto& padObj : payload["pads"]) {
                if (!padObj.is_object()) continue;
                std::string pRef = safeJsonStr(padObj, "ref", "reference");
                if (!pRef.empty()) continue;
                std::string pid = safeJsonStr(padObj, "id", "primitiveId");
                size_t lastE = pid.rfind('e');
                if (lastE != std::string::npos && lastE > 0 && lastE + 1 < pid.size()) {
                    bool allDigits = true;
                    for (size_t i = lastE + 1; i < pid.size(); ++i) {
                        if (!std::isdigit(static_cast<unsigned char>(pid[i]))) {
                            allDigits = false;
                            break;
                        }
                    }
                    if (allDigits) {
                        auto it = compRefById.find(pid.substr(0, lastE));
                        if (it != compRefById.end()) {
                            padObj["ref"] = it->second;
                        }
                    }
                }
            }
        }
    };

    // Auto-switch EasyEDA Pro active document tab when Schematic/PCB tool mismatches active tab
    if (comp == "easyeda" && rpcResult.is_object() &&
        !(inMemorySchEdit || inMemoryPcbEdit || inMemoryPcbInspect || inMemorySchInspect)) {
        int neededDocType = 0;
        if (toolName == "easyeda_inspect_schematic" || toolName == "easyeda_edit_schematic" ||
            toolName == "easyeda_run_erc" ||
            (toolName == "easyeda_run_drc" && remoteArgs.value("target", "pcb") == "schematic")) {
            neededDocType = 1;
        } else if (toolName == "easyeda_inspect_pcb" || toolName == "easyeda_edit_pcb" ||
                   toolName == "easyeda_export_jlcpcb" ||
                   (toolName == "easyeda_run_drc" && remoteArgs.value("target", "pcb") == "pcb")) {
            neededDocType = 3;
        }

        if (neededDocType != 0) {
            int curDocType = (rpcResult.contains("document") && rpcResult["document"].is_object())
                ? rpcResult["document"].value("documentType", 0)
                : 0;
            std::string curDocUuid = (rpcResult.contains("document") && rpcResult["document"].is_object())
                ? safeJsonStr(rpcResult["document"], "uuid")
                : safeJsonStr(rpcResult, "active_document");
            std::string requestedUuid = safeJsonStr(remoteArgs, "page_uuid", "sheet_uuid", "pcb_uuid");
            bool pcbNeedsOpen = (neededDocType == 3 && curDocType == 0 &&
                                 rpcResult.value("components_count", -1) == 0 &&
                                 rpcResult.contains("project") && rpcResult["project"].is_object());
            bool schRefsOnOtherPage = (toolName == "easyeda_inspect_schematic" && requestedUuid.empty() &&
                                       arguments.is_object() && arguments.contains("references") &&
                                       arguments["references"].is_array() && !arguments["references"].empty() &&
                                       rpcResult.contains("components") && rpcResult["components"].is_array() &&
                                       rpcResult["components"].empty() &&
                                       rpcResult.contains("project") && rpcResult["project"].is_object());

            if ((curDocType != 0 && curDocType != neededDocType) ||
                pcbNeedsOpen || schRefsOnOtherPage ||
                (!requestedUuid.empty() && requestedUuid != curDocUuid)) {
                std::string targetUuid = requestedUuid;
                nlohmann::json projObj = (rpcResult.contains("project") && rpcResult["project"].is_object())
                    ? rpcResult["project"]
                    : nlohmann::json::object();
                if (projObj.empty()) {
                    nlohmann::json projApiRes;
                    if (invokeEasyEdaApi("eda.dmt_Project.getCurrentProjectInfo",
                                         nlohmann::json::array(), projApiRes) &&
                        projApiRes.contains("result") && projApiRes["result"].is_object()) {
                        projObj = projApiRes["result"];
                    }
                }
                if (targetUuid.empty() && projObj.contains("data") && projObj["data"].is_array()) {
                    for (const auto& b : projObj["data"]) {
                        if (!b.is_object()) continue;
                        if (neededDocType == 1 && b.contains("schematic") && b["schematic"].is_object() &&
                            b["schematic"].contains("page") && b["schematic"]["page"].is_array() &&
                            !b["schematic"]["page"].empty()) {
                            if (schRefsOnOtherPage) {
                                for (const auto& pg : b["schematic"]["page"]) {
                                    if (!pg.is_object()) continue;
                                    std::string pgUuid = safeJsonStr(pg, "uuid");
                                    if (!pgUuid.empty() && pgUuid != curDocUuid) {
                                        targetUuid = pgUuid;
                                        break;
                                    }
                                }
                            } else if (b["schematic"]["page"][0].is_object()) {
                                targetUuid = safeJsonStr(b["schematic"]["page"][0], "uuid");
                            }
                            if (!targetUuid.empty()) break;
                        }
                        if (neededDocType == 3 && b.contains("pcb") && b["pcb"].is_object()) {
                            targetUuid = safeJsonStr(b["pcb"], "uuid");
                            break;
                        }
                    }
                }
                if (!targetUuid.empty() && targetUuid != curDocUuid) {
                    nlohmann::json openRes;
                    invokeEasyEdaApi("eda.dmt_EditorControl.openDocument",
                                     nlohmann::json::array({targetUuid}), openRes);
                    std::string retryErr;
                    int retryCode = 0;
                    nlohmann::json retryRes;
                    if (dispatchRemoteRpc(toolName, remoteArgs, retryRes, retryErr, retryCode) &&
                        retryRes.is_object()) {
                        rpcResult = std::move(retryRes);
                    }
                }
            }
        }
    }

    sanitizeEasyEdaProject(rpcResult);

    auto mergeCachedPinStatesAndCacheSheet = [&](nlohmann::json& schPayload) {
        sanitizeEasyEdaProject(schPayload);
        if (!schPayload.is_object() || !schPayload.contains("components") ||
            !schPayload["components"].is_array()) {
            return;
        }
        std::string sheetKey = safeJsonStr(schPayload, "active_document", nullptr, nullptr, "default_sheet");
        if (schPayload.contains("document") && schPayload["document"].is_object()) {
            std::string docId = safeJsonStr(schPayload["document"], "uuid", "title");
            if (!docId.empty()) sheetKey = docId;
        }

        // If multi-sheet project and active-sheet components don't yet include cross-sheet items, hydrate via getAll(null, true)
        bool hasOnActiveSheetFlag = false;
        std::set<std::string> activeSheetCompKeys;
        for (auto& compObj : schPayload["components"]) {
            if (!compObj.is_object()) continue;
            if (compObj.contains("on_active_sheet")) {
                hasOnActiveSheetFlag = true;
            } else {
                compObj["on_active_sheet"] = true;
            }
            std::string uid = safeJsonStr(compObj, "unique_id", "uniqueId");
            std::string cid = safeJsonStr(compObj, "id", "primitiveId");
            std::string ref = safeJsonStr(compObj, "reference", "ref", "designator");
            std::string key = !uid.empty() ? ("uid:" + uid) : ("id:" + cid + ":" + ref);
            activeSheetCompKeys.insert(key);
        }

        size_t totalPages = 1;
        if (schPayload.contains("project") && schPayload["project"].is_object() &&
            schPayload["project"].contains("data") && schPayload["project"]["data"].is_array() &&
            !schPayload["project"]["data"].empty() && schPayload["project"]["data"][0].is_object() &&
            schPayload["project"]["data"][0].contains("schematic") &&
            schPayload["project"]["data"][0]["schematic"].is_object() &&
            schPayload["project"]["data"][0]["schematic"].contains("page") &&
            schPayload["project"]["data"][0]["schematic"]["page"].is_array()) {
            totalPages = schPayload["project"]["data"][0]["schematic"]["page"].size();
        }

        bool callerFilteredRefsOrTypes = (arguments.is_object() &&
            ((arguments.contains("references") && arguments["references"].is_array() && !arguments["references"].empty()) ||
             (arguments.contains("component_types") && arguments["component_types"].is_array() && !arguments["component_types"].empty())));

        if (!hasOnActiveSheetFlag && totalPages > 1 && !callerFilteredRefsOrTypes &&
            !(inMemorySchEdit && !_connected && gw == nullptr)) {
            nlohmann::json allCompsRes;
            if (invokeEasyEdaApi("eda.sch_PrimitiveComponent.getAll",
                                 nlohmann::json::array({nullptr, true}), allCompsRes) &&
                allCompsRes.contains("result") && allCompsRes["result"].is_array()) {
                for (const auto& rc : allCompsRes["result"]) {
                    if (!rc.is_object()) continue;
                    std::string cid = safeJsonStr(rc, "primitiveId", "id");
                    std::string ref = safeJsonStr(rc, "designator", "reference", "ref");
                    std::string uid = safeJsonStr(rc, "uniqueId", "unique_id");
                    std::string key = !uid.empty() ? ("uid:" + uid) : ("id:" + cid + ":" + ref);
                    if (activeSheetCompKeys.count(key) > 0) continue;
                    activeSheetCompKeys.insert(key);
                    std::string val = safeJsonStr(rc, "name", "value", "net");
                    std::string fp = safeJsonStr(rc, "footprint", "footprintName");
                    std::string cType = safeJsonStr(rc, "componentType", "component_type", nullptr, "part");
                    schPayload["components"].push_back({
                        {"id", cid},
                        {"reference", ref},
                        {"value", val},
                        {"footprint", fp},
                        {"unique_id", uid},
                        {"component_type", cType},
                        {"x", safeJsonNum(rc, "x", 0.0)},
                        {"y", safeJsonNum(rc, "y", 0.0)},
                        {"rotation", safeJsonNum(rc, "rotation", 0.0)},
                        {"on_active_sheet", false},
                        {"pins", nlohmann::json::array()}
                    });
                }
                schPayload["components_count"] = static_cast<int>(schPayload["components"].size());
            }
        }

        nlohmann::json sheetParts = nlohmann::json::array();
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& compObj : schPayload["components"]) {
            if (!compObj.is_object()) continue;
            std::string ref = safeJsonStr(compObj, "reference", "ref", "designator");
            std::string cType = safeJsonStr(compObj, "component_type", "type", nullptr, "part");
            bool isNetFlag = (cType == "netflag" || cType == "netport" || cType == "power" ||
                              ref.rfind("#", 0) == 0 || ref == "GND" || ref == "VCC");
            bool isFrame = (cType == "frame" || cType == "sheet" || ref.rfind("FRAME", 0) == 0);
            if (!isNetFlag && !isFrame && (cType == "part" || cType.empty()) &&
                !ref.empty() && ref.find('?') == std::string::npos) {
                sheetParts.push_back({
                    {"reference", ref},
                    {"value", safeJsonStr(compObj, "value", "name")},
                    {"footprint", safeJsonStr(compObj, "footprint")},
                    {"component_type", "part"}
                });
            }
            if (compObj.contains("pins") && compObj["pins"].is_array()) {
                for (auto& pinObj : compObj["pins"]) {
                    if (!pinObj.is_object()) continue;
                    std::string pid = safeJsonStr(pinObj, "id");
                    auto it = _cachedPinNoConnect.find(pid);
                    if (it != _cachedPinNoConnect.end()) {
                        pinObj["no_connected"] = it->second;
                    }
                }
            }
        }
        if (!callerFilteredRefsOrTypes) {
            _cachedSchematicComponentsBySheet[sheetKey] = sheetParts;
        }
    };

    // Post-processing for EasyEDA tools
    if (toolName == "easyeda_inspect_schematic" && rpcResult.is_object()) {
        mergeCachedPinStatesAndCacheSheet(rpcResult);

        if (rpcResult.contains("components") && rpcResult["components"].is_array()) {
            // Apply optional filters (component_types, references, nets, include_pins, include_wires, summary_only)
            auto parseStringSet = [&](const std::vector<const char*>& keys) -> std::set<std::string> {
                std::set<std::string> res;
                if (!arguments.is_object()) return res;
                for (const char* k : keys) {
                    if (!arguments.contains(k)) continue;
                    const auto& v = arguments[k];
                    if (v.is_array()) {
                        for (const auto& item : v) {
                            if (item.is_string() && !item.get<std::string>().empty()) {
                                res.insert(item.get<std::string>());
                            }
                        }
                    } else if (v.is_string() && !v.get<std::string>().empty()) {
                        std::stringstream ss(v.get<std::string>());
                        std::string tok;
                        while (std::getline(ss, tok, ',')) {
                            auto s = tok.find_first_not_of(" \t\r\n");
                            auto e = tok.find_last_not_of(" \t\r\n");
                            if (s != std::string::npos && e != std::string::npos) {
                                res.insert(tok.substr(s, e - s + 1));
                            }
                        }
                    }
                }
                return res;
            };

            std::set<std::string> refSet = parseStringSet({"references", "reference", "ref_filter", "refs", "component_refs"});
            std::set<std::string> netSet = parseStringSet({"nets", "net", "net_filter"});
            std::set<std::string> typeSet = parseStringSet({"component_types", "component_type"});

            bool includePins = arguments.is_object() ? arguments.value("include_pins", true) : true;
            bool includeWires = arguments.is_object() ? arguments.value("include_wires", true) : true;
            bool summaryOnly = arguments.is_object() ? arguments.value("summary_only", false) : false;
            bool hasCompFilter = (!refSet.empty() || !netSet.empty() || !typeSet.empty());

            EdaAccelerators accCopy;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                accCopy = _accelerators;
            }
            bool pluginAlreadyFiltered = false;
            if (!refSet.empty() || !typeSet.empty() || !includePins || !includeWires) {
                bool hasAnyPins = false;
                bool hasUnmatchedRef = false;
                bool hasNetFlag = false;
                for (const auto& c : rpcResult["components"]) {
                    if (!c.is_object()) continue;
                    if (c.contains("pins") && c["pins"].is_array() && !c["pins"].empty()) {
                        hasAnyPins = true;
                    }
                    std::string r = safeJsonStr(c, "reference", "ref", "designator");
                    std::string ct = safeJsonStr(c, "component_type", "type");
                    if (!refSet.empty() && !r.empty() && refSet.count(r) == 0) {
                        hasUnmatchedRef = true;
                    }
                    if (ct == "netflag" || ct == "netport" || ct == "power" || r.rfind("#", 0) == 0) {
                        hasNetFlag = true;
                    }
                }
                if ((!includePins && !hasAnyPins) ||
                    (!refSet.empty() && !hasUnmatchedRef) ||
                    (!typeSet.empty() && typeSet.count("netflag") == 0 && !hasNetFlag)) {
                    pluginAlreadyFiltered = true;
                }
            }
            if (pluginAlreadyFiltered) {
                rpcResult["_partial_component_subset"] = true;
            }
            bool ercOk = false;
            std::string ercSummary;
            rpcResult["erc_audit"] = accCopy.calcSchematicErcAudit(rpcResult, ercOk, ercSummary);
            rpcResult.erase("_partial_component_subset");

            // Hydrate pin.net on each component's pins from connected wire nets / inferred_nets
            for (auto& c : rpcResult["components"]) {
                if (!c.is_object() || !c.contains("pins") || !c["pins"].is_array()) continue;
                std::string cRef = safeJsonStr(c, "reference", "ref", "designator");
                for (auto& p : c["pins"]) {
                    if (!p.is_object()) continue;
                    if (!p.value("net", "").empty()) continue;
                    std::string pNum = p.value("number", p.value("pin_number", ""));
                    std::string matchedNet;
                    if (rpcResult.contains("erc_audit") && rpcResult["erc_audit"].contains("inferred_nets") &&
                        rpcResult["erc_audit"]["inferred_nets"].is_array()) {
                        std::string targetPinKey = cRef + "." + pNum;
                        for (const auto& inf : rpcResult["erc_audit"]["inferred_nets"]) {
                            if (!inf.is_object() || !inf.contains("pins") || !inf["pins"].is_array()) continue;
                            for (const auto& pinStrJ : inf["pins"]) {
                                if (pinStrJ.is_string() && pinStrJ.get<std::string>() == targetPinKey) {
                                    matchedNet = inf.value("name", "");
                                    break;
                                }
                            }
                            if (!matchedNet.empty()) break;
                        }
                    }
                    if (matchedNet.empty() && p.contains("x") && p.contains("y") &&
                        rpcResult.contains("wires") && rpcResult["wires"].is_array()) {
                        double px = p["x"].get<double>();
                        double py = p["y"].get<double>();
                        for (const auto& w : rpcResult["wires"]) {
                            if (!w.is_object()) continue;
                            std::string wNet = w.value("net", "");
                            if (wNet.empty()) continue;
                            if (w.contains("line") && w["line"].is_array()) {
                                const auto& lArr = w["line"];
                                bool hit = false;
                                auto checkDist = [&](double wx, double wy) {
                                    return (std::hypot(wx - px, wy - py) <= 2.5 ||
                                            std::hypot(wx - px, std::fabs(wy) - std::fabs(py)) <= 2.5);
                                };
                                if (!lArr.empty() && lArr[0].is_number()) {
                                    for (size_t k = 0; k + 1 < lArr.size(); k += 2) {
                                        if (checkDist(lArr[k].get<double>(), lArr[k + 1].get<double>())) {
                                            hit = true;
                                            break;
                                        }
                                    }
                                } else {
                                    for (const auto& pt : lArr) {
                                        if (pt.is_array() && pt.size() >= 2 && pt[0].is_number() && pt[1].is_number()) {
                                            if (checkDist(pt[0].get<double>(), pt[1].get<double>())) {
                                                hit = true;
                                                break;
                                            }
                                        } else if (pt.is_object() && pt.contains("x") && pt.contains("y")) {
                                            if (checkDist(pt["x"].get<double>(), pt["y"].get<double>())) {
                                                hit = true;
                                                break;
                                            }
                                        }
                                    }
                                }
                                if (hit) {
                                    matchedNet = wNet;
                                    break;
                                }
                            }
                        }
                    }
                    if (!matchedNet.empty()) {
                        p["net"] = matchedNet;
                    }
                }
            }

            // Detect subcircuit components
            nlohmann::json subcircuitComps = nlohmann::json::array();
            for (const auto& comp : rpcResult["components"]) {
                if (!comp.is_object()) continue;
                std::string cType = comp.value("component_type", comp.value("type", ""));
                std::string cRef = comp.value("reference", comp.value("ref", comp.value("designator", "")));
                bool isSub = (cType == "subcircuit");
                if (!isSub && cRef.size() >= 2 && cRef[0] == 'B') {
                    bool allDigits = true;
                    for (size_t i = 1; i < cRef.size(); ++i) {
                        if (!std::isdigit(static_cast<unsigned char>(cRef[i]))) {
                            allDigits = false;
                            break;
                        }
                    }
                    if (allDigits) isSub = true;
                }
                if (!isSub && comp.contains("other_property") && comp["other_property"].is_object()) {
                    std::string bType = comp["other_property"].value("block_type",
                                        comp["other_property"].value("blockType", ""));
                    if (!bType.empty()) isSub = true;
                }
                if (isSub) {
                    subcircuitComps.push_back({
                        {"id", comp.value("id", "")},
                        {"reference", cRef},
                        {"value", comp.value("value", "")},
                        {"footprint", comp.value("footprint", "")},
                        {"component_type", cType.empty() ? "subcircuit" : cType}
                    });
                }
            }

            std::set<std::string> inferredNetCompRefs;
            std::set<std::string> inferredNetPinRefs;
            if (!netSet.empty() && rpcResult.contains("erc_audit") &&
                rpcResult["erc_audit"].contains("inferred_nets") &&
                rpcResult["erc_audit"]["inferred_nets"].is_array()) {
                for (const auto& inf : rpcResult["erc_audit"]["inferred_nets"]) {
                    if (!inf.is_object()) continue;
                    std::string nName = inf.value("name", "");
                    if (netSet.count(nName) > 0 && inf.contains("pins") && inf["pins"].is_array()) {
                        for (const auto& pr : inf["pins"]) {
                            if (!pr.is_string()) continue;
                            std::string s = pr.get<std::string>();
                            inferredNetPinRefs.insert(s);
                            size_t dot = s.find('.');
                            if (dot != std::string::npos) {
                                inferredNetCompRefs.insert(s.substr(0, dot));
                            }
                        }
                    }
                }
            }

            std::vector<std::pair<double, double>> matchedPinCoords;
            if (hasCompFilter || !includePins) {
                nlohmann::json filteredComps = nlohmann::json::array();
                for (const auto& c : rpcResult["components"]) {
                    if (!c.is_object()) continue;
                    std::string ref = c.value("reference", c.value("ref", c.value("designator", "")));
                    std::string cType = c.value("component_type", c.value("type", ""));
                    if (cType.empty()) {
                        if (ref.rfind("FRAME", 0) == 0) cType = "frame";
                        else if (ref.rfind("#", 0) == 0 || ref == "GND" || ref == "VCC") cType = "netflag";
                        else cType = "part";
                    }
                    if (!typeSet.empty() && typeSet.count(cType) == 0) continue;
                    if (!refSet.empty() && refSet.count(ref) == 0) continue;
                    if (!netSet.empty()) {
                        bool netMatch = false;
                        if (netSet.count(c.value("net", "")) > 0) netMatch = true;
                        if ((cType == "netflag" || cType == "netport" || cType == "power") &&
                            netSet.count(c.value("value", "")) > 0) {
                            netMatch = true;
                        }
                        if (inferredNetCompRefs.count(ref) > 0) netMatch = true;
                        if (c.contains("pins") && c["pins"].is_array()) {
                            for (const auto& p : c["pins"]) {
                                if (!p.is_object()) continue;
                                std::string pNum = p.value("number", "");
                                if (netSet.count(p.value("net", "")) > 0 ||
                                    inferredNetPinRefs.count(ref + "." + pNum) > 0) {
                                    netMatch = true;
                                    matchedPinCoords.push_back({p.value("x", 0.0), p.value("y", 0.0)});
                                }
                            }
                        }
                        if (!netMatch) continue;
                    } else if (!refSet.empty() && c.contains("pins") && c["pins"].is_array()) {
                        for (const auto& p : c["pins"]) {
                            if (p.is_object()) {
                                matchedPinCoords.push_back({p.value("x", 0.0), p.value("y", 0.0)});
                            }
                        }
                    }

                    nlohmann::json compCopy = c;
                    if (!includePins) {
                        compCopy.erase("pins");
                    }
                    filteredComps.push_back(std::move(compCopy));
                }
                rpcResult["components"] = std::move(filteredComps);
                rpcResult["components_count"] = static_cast<int>(rpcResult["components"].size());
            }

            if (rpcResult.contains("wires") && rpcResult["wires"].is_array() && (!netSet.empty() || !refSet.empty())) {
                nlohmann::json filteredWires = nlohmann::json::array();
                for (const auto& w : rpcResult["wires"]) {
                    if (!w.is_object()) continue;
                    bool keepWire = (!netSet.empty() && netSet.count(w.value("net", "")) > 0);
                    if (!keepWire && !matchedPinCoords.empty() && w.contains("line") && w["line"].is_array() && !w["line"].empty()) {
                        const auto& lineArr = w["line"];
                        auto checkPt = [&](double wx, double wy) -> bool {
                            for (const auto& mp : matchedPinCoords) {
                                if (std::hypot(wx - mp.first, wy - mp.second) <= 1.5 ||
                                    std::hypot(wx - mp.first, wy - std::fabs(mp.second)) <= 1.5) {
                                    return true;
                                }
                            }
                            return false;
                        };
                        if (lineArr[0].is_number()) {
                            for (size_t k = 0; k + 1 < lineArr.size(); k += 2) {
                                if (lineArr[k].is_number() && lineArr[k + 1].is_number() &&
                                    checkPt(lineArr[k].get<double>(), lineArr[k + 1].get<double>())) {
                                    keepWire = true;
                                    break;
                                }
                            }
                        } else {
                            for (const auto& pt : lineArr) {
                                if (pt.is_array() && pt.size() >= 2 && pt[0].is_number() && pt[1].is_number() &&
                                    checkPt(pt[0].get<double>(), pt[1].get<double>())) {
                                    keepWire = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (keepWire) {
                        filteredWires.push_back(w);
                    }
                }
                rpcResult["wires"] = std::move(filteredWires);
                rpcResult["wires_count"] = static_cast<int>(rpcResult["wires"].size());
            }

            // Compute unnamed_wires_count and named_nets_count
            int unnamedWiresCount = 0;
            std::set<std::string> distinctNamedNets;
            if (rpcResult.contains("wires") && rpcResult["wires"].is_array()) {
                for (const auto& w : rpcResult["wires"]) {
                    if (!w.is_object()) continue;
                    std::string wn = w.value("net", "");
                    if (wn.empty()) ++unnamedWiresCount;
                    else distinctNamedNets.insert(wn);
                }
            }
            if (rpcResult.contains("components") && rpcResult["components"].is_array()) {
                for (const auto& c : rpcResult["components"]) {
                    if (!c.is_object()) continue;
                    std::string cn = c.value("net", "");
                    if (!cn.empty()) distinctNamedNets.insert(cn);
                    if (c.contains("pins") && c["pins"].is_array()) {
                        for (const auto& p : c["pins"]) {
                            if (!p.is_object()) continue;
                            std::string pn = p.value("net", "");
                            if (!pn.empty()) distinctNamedNets.insert(pn);
                        }
                    }
                }
            }

            int totalComponents = (rpcResult.contains("components") && rpcResult["components"].is_array())
                ? static_cast<int>(rpcResult["components"].size()) : 0;
            int totalWires = (rpcResult.contains("wires") && rpcResult["wires"].is_array())
                ? static_cast<int>(rpcResult["wires"].size()) : 0;
            rpcResult["components_count"] = totalComponents;
            rpcResult["component_count"] = totalComponents;
            rpcResult["wires_count"] = totalWires;
            rpcResult["wire_count"] = totalWires;
            rpcResult["unnamed_wires_count"] = unnamedWiresCount;
            rpcResult["named_nets_count"] = static_cast<int>(distinctNamedNets.size());
            rpcResult["subcircuit_components"] = subcircuitComps;
            rpcResult["subcircuit_components_count"] = static_cast<int>(subcircuitComps.size());

            if (!includeWires) {
                rpcResult.erase("wires");
            }
            if ((summaryOnly || hasCompFilter || !includePins) &&
                !(arguments.is_object() && arguments.value("include_inferred_nets", false)) &&
                rpcResult.contains("erc_audit") && rpcResult["erc_audit"].is_object()) {
                rpcResult["erc_audit"].erase("inferred_nets");
            }
            if (summaryOnly) {
                rpcResult.erase("components");
                rpcResult.erase("wires");
                rpcResult.erase("netlist");
                rpcResult.erase("subcircuit_components");
            }
        }

        if (!rpcResult.contains("components_count")) {
            rpcResult["components_count"] = 0;
        }
        if (!rpcResult.contains("component_count")) {
            rpcResult["component_count"] = rpcResult["components_count"];
        }
        if (!rpcResult.contains("wires_count")) {
            rpcResult["wires_count"] = 0;
        }
        if (!rpcResult.contains("wire_count")) {
            rpcResult["wire_count"] = rpcResult["wires_count"];
        }
        if (!rpcResult.contains("subcircuit_components_count")) {
            rpcResult["subcircuit_components_count"] = 0;
        }
        if (!rpcResult.contains("unnamed_wires_count")) {
            rpcResult["unnamed_wires_count"] = 0;
        }
        if (!rpcResult.contains("named_nets_count")) {
            rpcResult["named_nets_count"] = 0;
        }
    }

    auto hydrateLiveEasyEdaPours = [&](nlohmann::json& pcbObj) {
        if (!pcbObj.is_object() || !(_connected || gw != nullptr) ||
            inMemoryPcbInspect || inMemoryPcbEdit) {
            return;
        }
        bool needsHydration = false;
        int reportedPoursCount = pcbObj.value("pours_count", -1);
        if (!pcbObj.contains("pours") || !pcbObj["pours"].is_array() || pcbObj["pours"].empty()) {
            if (reportedPoursCount != 0) {
                needsHydration = true;
            }
        } else {
            for (const auto& pr : pcbObj["pours"]) {
                if (!pr.is_object()) continue;
                bool hasPts = pr.contains("points") && pr["points"].is_array() && !pr["points"].empty();
                bool hasPoly = pr.contains("polygon") && (pr["polygon"].is_array() || pr["polygon"].is_object());
                bool hasCplx = pr.contains("complexPolygon") && (pr["complexPolygon"].is_array() || pr["complexPolygon"].is_object());
                double pw = pr.value("width", 0.0);
                double ph = pr.value("height", 0.0);
                if (!hasPts && !hasPoly && !hasCplx && !(pw > 0.0 && ph > 0.0)) {
                    needsHydration = true;
                    break;
                }
            }
        }
        if (!needsHydration) {
            return;
        }
        nlohmann::json rawPoursRes;
        if (!invokeEasyEdaApi("eda.pcb_PrimitivePour.getAll", nlohmann::json::array(), rawPoursRes) ||
            !rawPoursRes.is_object() || !rawPoursRes.contains("result") || !rawPoursRes["result"].is_array()) {
            return;
        }
        nlohmann::json hydrated = nlohmann::json::array();
        for (const auto& pr : rawPoursRes["result"]) {
            if (!pr.is_object()) continue;
            std::string pid = safeJsonStr(pr, "primitiveId", "id");
            std::string pnet = safeJsonStr(pr, "net");
            std::string player = "TopLayer";
            if (pr.contains("layer")) {
                if (pr["layer"].is_string()) {
                    player = pr["layer"].get<std::string>();
                } else if (pr["layer"].is_number_integer()) {
                    player = std::to_string(pr["layer"].get<int>());
                }
            }
            const nlohmann::json* srcArr = nullptr;
            for (const char* k : {"complexPolygon", "polygon", "points"}) {
                if (!pr.contains(k)) continue;
                if (pr[k].is_array()) {
                    srcArr = &pr[k];
                    break;
                } else if (pr[k].is_object()) {
                    for (const char* subK : {"polygon", "points", "source", "Source"}) {
                        if (pr[k].contains(subK) && pr[k][subK].is_array()) {
                            srcArr = &pr[k][subK];
                            break;
                        }
                    }
                    if (srcArr != nullptr) break;
                }
            }
            std::vector<std::pair<double, double>> pts;
            if (srcArr != nullptr) {
                for (size_t i = 0; i < srcArr->size(); ++i) {
                    const auto& tok = (*srcArr)[i];
                    if (tok.is_string()) {
                        if (toUpperAscii(tok.get<std::string>()) == "ARC" &&
                            i + 1 < srcArr->size() && (*srcArr)[i + 1].is_number()) {
                            ++i;
                        }
                        continue;
                    }
                    if (tok.is_object() && tok.contains("x") && tok.contains("y") &&
                        tok["x"].is_number() && tok["y"].is_number()) {
                        pts.emplace_back(tok["x"].get<double>(), tok["y"].get<double>());
                    } else if (tok.is_array() && tok.size() >= 2 &&
                               tok[0].is_number() && tok[1].is_number()) {
                        pts.emplace_back(tok[0].get<double>(), tok[1].get<double>());
                    } else if (tok.is_number() && i + 1 < srcArr->size() &&
                               (*srcArr)[i + 1].is_number()) {
                        pts.emplace_back(tok.get<double>(), (*srcArr)[i + 1].get<double>());
                        ++i;
                    }
                }
            }
            double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
            nlohmann::json ptsJson = nlohmann::json::array();
            if (!pts.empty()) {
                minX = maxX = pts[0].first;
                minY = maxY = pts[0].second;
                for (const auto& pt : pts) {
                    minX = std::min(minX, pt.first);
                    maxX = std::max(maxX, pt.first);
                    minY = std::min(minY, pt.second);
                    maxY = std::max(maxY, pt.second);
                    ptsJson.push_back(nlohmann::json::array({pt.first, pt.second}));
                }
            }
            nlohmann::json pourEntry = {
                {"id", pid},
                {"net", pnet},
                {"layer", player},
                {"x", minX},
                {"y", minY},
                {"min_x", minX},
                {"min_y", minY},
                {"max_x", maxX},
                {"max_y", maxY},
                {"width", maxX - minX},
                {"height", maxY - minY},
                {"points", std::move(ptsJson)}
            };
            hydrated.push_back(std::move(pourEntry));
        }
        if (!hydrated.empty() || reportedPoursCount <= 0) {
            pcbObj["pours_count"] = static_cast<int>(hydrated.size());
            pcbObj["pours"] = std::move(hydrated);
        }
    };

    if (toolName == "easyeda_run_drc" && rpcResult.is_object()) {
        std::string target = remoteArgs.value("target", "pcb");
        if (target == "schematic") {
            rpcResult.erase("connectivity_audit");
            nlohmann::json schSnap = rpcResult;
            if (!schSnap.contains("components") || !schSnap["components"].is_array()) {
                std::string subErr;
                int subCode = 0;
                dispatchRemoteRpc("easyeda_inspect_schematic", nlohmann::json::object(),
                                  schSnap, subErr, subCode);
            }
            mergeCachedPinStatesAndCacheSheet(schSnap);
            EdaAccelerators accCopy;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                accCopy = _accelerators;
            }
            bool ercOk = false;
            std::string ercSummary;
            nlohmann::json ercAudit = accCopy.calcSchematicErcAudit(schSnap, ercOk, ercSummary);
            rpcResult["erc_audit"] = ercAudit;

            nlohmann::json violationsArr = nlohmann::json::array();
            nlohmann::json nativeSummaryArr = nlohmann::json::array();
            if (rpcResult.contains("violations") && rpcResult["violations"].is_array()) {
                for (const auto& existingV : rpcResult["violations"]) {
                    if (existingV.is_object()) {
                        std::string vt = existingV.value("type", existingV.value("errorType", ""));
                        if (vt == "unconnected_pin" || vt == "dangling_wire" ||
                            vt == "component_issue" || vt == "net_issue" ||
                            vt == "single_pin_net" || vt == "overlapping_component" ||
                            vt == "pullup_issue" || vt == "decoupling_issue" ||
                            vt == "voltage_domain_issue") {
                            continue;
                        }
                        if (existingV.contains("count") &&
                            !existingV.contains("id") &&
                            !existingV.contains("reference") &&
                            !existingV.contains("issue") &&
                            !existingV.contains("rule") &&
                            !existingV.contains("message")) {
                            nativeSummaryArr.push_back(existingV);
                            continue;
                        }
                        if ((vt == "fatalError" || vt == "warn" || vt == "error" || vt == "warning") &&
                            existingV.value("code", "").empty() &&
                            existingV.value("ruleName", "").empty() &&
                            existingV.value("primitiveId1", "").empty() &&
                            existingV.value("msg", "").empty() &&
                            !existingV.contains("id") &&
                            !existingV.contains("reference")) {
                            nativeSummaryArr.push_back(existingV);
                            continue;
                        }
                    }
                    violationsArr.push_back(existingV);
                }
            }
            if (!nativeSummaryArr.empty()) {
                rpcResult["native_drc_summary"] = std::move(nativeSummaryArr);
            }
            auto appendAuditViolations = [&](const char* arrKey, const char* vType) {
                if (ercAudit.contains(arrKey) && ercAudit[arrKey].is_array()) {
                    for (const auto& entry : ercAudit[arrKey]) {
                        if (!entry.is_object()) continue;
                        nlohmann::json item = entry;
                        item["type"] = vType;
                        violationsArr.push_back(std::move(item));
                    }
                }
            };
            appendAuditViolations("unconnected_pins", "unconnected_pin");
            appendAuditViolations("dangling_wires", "dangling_wire");
            appendAuditViolations("component_issues", "component_issue");
            appendAuditViolations("net_issues", "net_issue");
            appendAuditViolations("single_pin_nets", "single_pin_net");
            appendAuditViolations("overlapping_components", "overlapping_component");
            appendAuditViolations("pullup_issues", "pullup_issue");
            appendAuditViolations("decoupling_issues", "decoupling_issue");
            appendAuditViolations("voltage_domain_issues", "voltage_domain_issue");

            rpcResult["violations"] = std::move(violationsArr);
            rpcResult["violation_count"] = static_cast<int>(rpcResult["violations"].size());
            rpcResult["filtered_violation_count"] = static_cast<int>(rpcResult["violations"].size());
            rpcResult["passed"] = (rpcResult["violations"].empty() && ercAudit.value("passed", true));
            if (rpcResult.contains("erc_audit") && rpcResult["erc_audit"].is_object()) {
                rpcResult["erc_audit"].erase("inferred_nets");
            }
            rpcResult.erase("components");
            rpcResult.erase("wires");
            rpcResult.erase("netlist");
        } else {
            hydrateLiveEasyEdaPours(rpcResult);
            if (rpcResult.contains("pours_count") && rpcResult["pours_count"].is_number_integer() &&
                rpcResult.contains("connectivity_audit") && rpcResult["connectivity_audit"].is_object()) {
                int pCnt = rpcResult["pours_count"].get<int>();
                if (pCnt > 0 && rpcResult["connectivity_audit"].value("pours_count", 0) == 0) {
                    rpcResult["connectivity_audit"]["pours_count"] = pCnt;
                }
            }
            rpcResult.erase("components");
            rpcResult.erase("pads");
            rpcResult.erase("tracks");
            rpcResult.erase("vias");
            rpcResult.erase("pours");
            rpcResult.erase("schematic_components");

            bool includeVerbose = remoteArgs.value("include_verbose_error", false);
            bool summaryOnly = remoteArgs.value("summary_only", false);
            int maxViolations = remoteArgs.value("max_violations", 0);

            std::set<std::string> netFilter;
            for (const char* k : {"nets", "net_filter", "net"}) {
                if (remoteArgs.contains(k)) {
                    if (remoteArgs[k].is_array()) {
                        for (const auto& n : remoteArgs[k]) {
                            if (n.is_string() && !n.get<std::string>().empty()) netFilter.insert(n.get<std::string>());
                        }
                    } else if (remoteArgs[k].is_string() && !remoteArgs[k].get<std::string>().empty()) {
                        netFilter.insert(remoteArgs[k].get<std::string>());
                    }
                }
            }
            std::set<std::string> errTypeFilter;
            for (const char* k : {"error_types", "error_type"}) {
                if (remoteArgs.contains(k)) {
                    if (remoteArgs[k].is_array()) {
                        for (const auto& et : remoteArgs[k]) {
                            if (et.is_string() && !et.get<std::string>().empty()) errTypeFilter.insert(et.get<std::string>());
                        }
                    } else if (remoteArgs[k].is_string() && !remoteArgs[k].get<std::string>().empty()) {
                        errTypeFilter.insert(remoteArgs[k].get<std::string>());
                    }
                }
            }

            bool violationsOmitted = rpcResult.value("violations_omitted", false);
            if (rpcResult.contains("violations") && rpcResult["violations"].is_array() &&
                (!rpcResult["violations"].empty() || !violationsOmitted)) {
                nlohmann::json compactedViolations = nlohmann::json::array();
                nlohmann::json advisoryWarnings = nlohmann::json::array();
                std::map<std::string, int> byErrType;
                std::map<std::string, int> byRule;
                std::map<std::string, int> byNet;

                bool auditSynced = false;
                if (rpcResult.contains("connectivity_audit") && rpcResult["connectivity_audit"].is_object()) {
                    const auto& ca = rpcResult["connectivity_audit"];
                    auditSynced = ca.value("schematic_pcb_synced", false) &&
                                  ca.value("all_connected", false) &&
                                  ca.value("unsynced_schematic_components_count", 0) == 0;
                }

                for (auto& item : rpcResult["violations"]) {
                    if (!item.is_object()) continue;
                    std::string eType = item.value("errorType", item.value("type", ""));
                    std::string rName = item.value("ruleName", "");
                    std::string nName = item.value("net", item.value("net1", ""));

                    if (!includeVerbose && item.contains("explanation")) {
                        if (item["explanation"].is_object()) {
                            std::string msg = item["explanation"].value("msg",
                                              item["explanation"].value("str",
                                              item["explanation"].value("brief", "")));
                            item["explanation"].erase("errData");
                            if (!msg.empty()) {
                                item["explanation"] = msg;
                                if (!item.contains("msg") || item["msg"].is_null() ||
                                    (item["msg"].is_string() && item["msg"].get<std::string>().empty())) {
                                    item["msg"] = msg;
                                }
                            }
                        }
                    }

                    if (auditSynced &&
                        (rName == "Import Changes" ||
                         (eType == "Netlist Error" && rName == "Import Changes"))) {
                        advisoryWarnings.push_back(item);
                        continue;
                    }

                    if (!eType.empty()) byErrType[eType]++;
                    if (!rName.empty()) byRule[rName]++;
                    if (!nName.empty()) byNet[nName]++;

                    if (!errTypeFilter.empty() && errTypeFilter.count(eType) == 0 && errTypeFilter.count(rName) == 0) {
                        continue;
                    }
                    if (!netFilter.empty() && (nName.empty() || netFilter.count(nName) == 0)) {
                        continue;
                    }

                    compactedViolations.push_back(item);
                }

                if (!advisoryWarnings.empty()) {
                    rpcResult["advisory_warnings"] = std::move(advisoryWarnings);
                }

                if (!rpcResult.contains("violation_summary") || !violationsOmitted) {
                    rpcResult["violation_summary"] = {
                        {"by_error_type", byErrType},
                        {"by_rule_name", byRule},
                        {"by_net", byNet}
                    };
                }

                int totalMatching = static_cast<int>(compactedViolations.size());
                if (maxViolations > 0 && totalMatching > maxViolations) {
                    rpcResult["total_matching_violations"] = totalMatching;
                    nlohmann::json truncated = nlohmann::json::array();
                    for (int i = 0; i < maxViolations; ++i) {
                        truncated.push_back(compactedViolations[i]);
                    }
                    rpcResult["violations"] = std::move(truncated);
                    rpcResult["truncated"] = true;
                } else {
                    rpcResult["violations"] = std::move(compactedViolations);
                }

                rpcResult["violation_count"] = totalMatching;
                rpcResult["filtered_violation_count"] = totalMatching;
                rpcResult["passed"] = (totalMatching == 0) &&
                    (!rpcResult.contains("connectivity_audit") ||
                     !rpcResult["connectivity_audit"].is_object() ||
                     rpcResult["connectivity_audit"].value("all_connected", true));
                if (summaryOnly) {
                    rpcResult.erase("violations");
                    rpcResult["violations_omitted"] = true;
                }
            } else if (summaryOnly) {
                rpcResult.erase("violations");
                rpcResult["violations_omitted"] = true;
            }

            if (!arguments.value("include_audit_details", false) &&
                rpcResult.contains("connectivity_audit") && rpcResult["connectivity_audit"].is_object()) {
                auto& ca = rpcResult["connectivity_audit"];
                if (!ca.contains("decoupling_distance_warnings_count") &&
                    ca.contains("decoupling_distance_warnings") && ca["decoupling_distance_warnings"].is_array()) {
                    ca["decoupling_distance_warnings_count"] = static_cast<int>(ca["decoupling_distance_warnings"].size());
                }
                if (!ca.contains("unassigned_pads_count") &&
                    ca.contains("unassigned_pads") && ca["unassigned_pads"].is_array()) {
                    ca["unassigned_pads_count"] = static_cast<int>(ca["unassigned_pads"].size());
                }
                ca.erase("unassigned_pads");
                ca.erase("decoupling_distance_warnings");
                ca["audit_details_omitted"] = true;
            }
        }
    } else if (toolName == "easyeda_edit_schematic" && rpcResult.is_object()) {
        std::string unitsStr = remoteArgs.value("units", "mil");
        rpcResult["units"] = unitsStr;
        auto scaleCoord = [&](double val) -> double {
            if (unitsStr == "mm" || unitsStr == "MM") {
                return std::round(val * 39.37007874 * 1000.0) / 1000.0;
            }
            return val;
        };
        auto snap50 = [](double val) -> double {
            return std::round(val / 50.0) * 50.0;
        };

        bool wantAnnotate = false;
        std::string annotatePrefixFilter;
        int annotateStartIndex = 1;
        bool annotateResetExisting = false;
        if (remoteArgs.contains("annotate_schematic")) {
            if (remoteArgs["annotate_schematic"].is_boolean()) {
                wantAnnotate = remoteArgs["annotate_schematic"].get<bool>();
            } else if (remoteArgs["annotate_schematic"].is_object()) {
                const auto& annObj = remoteArgs["annotate_schematic"];
                wantAnnotate = annObj.value("enabled", true);
                annotatePrefixFilter = annObj.value("prefix_filter", "");
                annotateStartIndex = std::max(1, annObj.value("start_index", 1));
                annotateResetExisting = annObj.value("reset_existing", false);
            }
        }

        bool wantConnectToNet = (remoteArgs.contains("connect_to_net") &&
                                 remoteArgs["connect_to_net"].is_array() &&
                                 !remoteArgs["connect_to_net"].empty());
        bool wantAutoPlace = false;
        if (remoteArgs.contains("add_components") && remoteArgs["add_components"].is_array()) {
            for (const auto& ac : remoteArgs["add_components"]) {
                if (!ac.is_object()) continue;
                if (ac.value("auto_place", false) ||
                    (ac.contains("near_reference") && ac["near_reference"].is_string() &&
                     !ac["near_reference"].get<std::string>().empty())) {
                    wantAutoPlace = true;
                    break;
                }
            }
        }

        nlohmann::json schState = nlohmann::json::object();
        if (inMemorySchEdit) {
            schState = remoteArgs["schematic_state"];
        } else if (wantAnnotate || wantConnectToNet || wantAutoPlace) {
            std::string subErr;
            int subCode = 0;
            dispatchRemoteRpc("easyeda_inspect_schematic", nlohmann::json::object(),
                              schState, subErr, subCode);
            mergeCachedPinStatesAndCacheSheet(schState);
        }
        if (!schState.contains("components") || !schState["components"].is_array()) {
            schState["components"] = nlohmann::json::array();
        }
        if (!schState.contains("wires") || !schState["wires"].is_array()) {
            schState["wires"] = nlohmann::json::array();
        }

        auto computeCompBbox = [](const nlohmann::json& c,
                                  double& outMinX, double& outMinY,
                                  double& outMaxX, double& outMaxY) {
            double cx = c.value("x", 0.0);
            double cy = c.value("y", 0.0);
            if (c.contains("bbox") && c["bbox"].is_object()) {
                const auto& bb = c["bbox"];
                outMinX = bb.value("min_x", bb.value("x1", cx - 25.0));
                outMinY = bb.value("min_y", bb.value("y1", cy - 25.0));
                outMaxX = bb.value("max_x", bb.value("x2", cx + 25.0));
                outMaxY = bb.value("max_y", bb.value("y2", cy + 25.0));
                return;
            }
            if (c.contains("width") && c["width"].is_number() &&
                c.contains("height") && c["height"].is_number()) {
                double w = std::max(10.0, c["width"].get<double>());
                double h = std::max(10.0, c["height"].get<double>());
                double rot = std::fmod(std::fabs(c.value("rotation", 0.0)), 180.0);
                if (std::fabs(rot - 90.0) < 45.0) std::swap(w, h);
                outMinX = cx - w * 0.5;
                outMaxX = cx + w * 0.5;
                outMinY = cy - h * 0.5;
                outMaxY = cy + h * 0.5;
                return;
            }
            if (c.contains("pins") && c["pins"].is_array() && !c["pins"].empty()) {
                outMinX = cx;
                outMaxX = cx;
                outMinY = cy;
                outMaxY = cy;
                for (const auto& p : c["pins"]) {
                    if (!p.is_object()) continue;
                    double px = p.value("x", cx);
                    double py = p.value("y", cy);
                    outMinX = std::min(outMinX, px);
                    outMaxX = std::max(outMaxX, px);
                    outMinY = std::min(outMinY, py);
                    outMaxY = std::max(outMaxY, py);
                }
                if (outMaxX - outMinX < 10.0) { outMinX -= 10.0; outMaxX += 10.0; }
                if (outMaxY - outMinY < 10.0) { outMinY -= 10.0; outMaxY += 10.0; }
                return;
            }
            outMinX = cx - 25.0;
            outMaxX = cx + 25.0;
            outMinY = cy - 25.0;
            outMaxY = cy + 25.0;
        };

        auto isPartComponent = [](const nlohmann::json& c) -> bool {
            if (!c.is_object()) return false;
            std::string ref = c.value("reference", c.value("ref", c.value("designator", "")));
            std::string cType = c.value("component_type", c.value("type", ""));
            if (cType == "frame" || ref.rfind("FRAME", 0) == 0) return false;
            if (cType == "netflag" || cType == "netport" || cType == "netlabel" ||
                cType == "power" || ref.rfind("#", 0) == 0 || ref == "GND" || ref == "VCC") {
                return false;
            }
            return (cType == "part" || cType.empty());
        };

        int totalAnnotatedCount = 0;
        nlohmann::json annotationDetails = nlohmann::json::array();

        auto runAnnotatePass = [&]() {
            if (!wantAnnotate) return;
            auto parseRef = [](const std::string& ref, std::string& outPrefix, int& outNum, std::string& outUniqueKey) -> bool {
                outPrefix.clear();
                outNum = -1;
                outUniqueKey.clear();
                size_t i = 0;
                while (i < ref.size() && std::isalpha(static_cast<unsigned char>(ref[i]))) {
                    outPrefix.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ref[i]))));
                    ++i;
                }
                if (outPrefix.empty()) outPrefix = "U";
                if (i >= ref.size() || ref.find('?') != std::string::npos) return false;
                size_t numStart = i;
                while (i < ref.size() && std::isdigit(static_cast<unsigned char>(ref[i]))) {
                    ++i;
                }
                if (i == numStart) return false;
                for (size_t j = i; j < ref.size(); ++j) {
                    unsigned char ch = static_cast<unsigned char>(ref[j]);
                    if (!std::isalnum(ch) && ch != '_' && ch != '-' && ch != '(' && ch != ')' && ch != '.') {
                        return false;
                    }
                }
                try {
                    outNum = std::stoi(ref.substr(numStart, i - numStart));
                    if (outNum < 0) return false;
                    outUniqueKey = (i == ref.size()) ? (outPrefix + std::to_string(outNum)) : ref;
                    return true;
                } catch (...) {
                    return false;
                }
            };

            std::map<std::string, std::set<int>> usedNumsByPrefix;
            std::set<std::string> seenValidRefs;
            std::vector<size_t> toAnnotate;

            for (size_t idx = 0; idx < schState["components"].size(); ++idx) {
                const auto& c = schState["components"][idx];
                if (!isPartComponent(c)) continue;
                std::string ref = c.value("reference", c.value("ref", c.value("designator", "")));
                std::string pfx;
                int num = -1;
                std::string uKey;
                bool valid = (!annotateResetExisting && parseRef(ref, pfx, num, uKey));
                if (!annotatePrefixFilter.empty() && pfx != annotatePrefixFilter) continue;
                if (valid && seenValidRefs.insert(uKey).second) {
                    usedNumsByPrefix[pfx].insert(num);
                } else if (c.value("on_active_sheet", true)) {
                    toAnnotate.push_back(idx);
                }
            }

            std::sort(toAnnotate.begin(), toAnnotate.end(), [&](size_t a, size_t b) {
                const auto& ca = schState["components"][a];
                const auto& cb = schState["components"][b];
                double ya = ca.value("y", 0.0);
                double yb = cb.value("y", 0.0);
                if (std::fabs(ya - yb) > 1e-3) return ya < yb;
                double xa = ca.value("x", 0.0);
                double xb = cb.value("x", 0.0);
                if (std::fabs(xa - xb) > 1e-3) return xa < xb;
                return ca.value("id", "") < cb.value("id", "");
            });

            for (size_t idx : toAnnotate) {
                auto& comp = schState["components"][idx];
                std::string oldRef = comp.value("reference", comp.value("ref", comp.value("designator", "")));
                std::string pfx;
                int dummyNum = -1;
                std::string dummyKey;
                parseRef(oldRef, pfx, dummyNum, dummyKey);
                int candidate = annotateStartIndex;
                while (usedNumsByPrefix[pfx].count(candidate) > 0) {
                    ++candidate;
                }
                usedNumsByPrefix[pfx].insert(candidate);
                std::string newRef = pfx + std::to_string(candidate);
                comp["reference"] = newRef;
                std::string cid = comp.value("id", "");
                if (comp.value("unique_id", "").empty()) {
                    comp["unique_id"] = "uid_" + newRef;
                }
                if (!cid.empty() && !(inMemorySchEdit && !_connected && gw == nullptr)) {
                    nlohmann::json modRes;
                    invokeEasyEdaApi("eda.sch_PrimitiveComponent.modify",
                                     nlohmann::json::array({cid, {{"designator", newRef}}}),
                                     modRes);
                }
                if (rpcResult.contains("placed_component_details") &&
                    rpcResult["placed_component_details"].is_array()) {
                    for (auto& pd : rpcResult["placed_component_details"]) {
                        if (pd.is_object() && !cid.empty() && pd.value("id", "") == cid) {
                            pd["reference"] = newRef;
                        }
                    }
                }
                ++totalAnnotatedCount;
                annotationDetails.push_back({
                    {"id", cid},
                    {"old_reference", oldRef},
                    {"new_reference", newRef},
                    {"x", comp.value("x", 0.0)},
                    {"y", comp.value("y", 0.0)}
                });
            }
        };

        // Pre-annotate existing components if annotate_schematic is enabled so near_reference can reference them
        runAnnotatePass();

        // 1. modify_pins
        if (remoteArgs.contains("modify_pins") && remoteArgs["modify_pins"].is_array()) {
            int modPinsCount = rpcResult.value("modified_pins", 0);
            bool needRemoteModPins = !rpcResult.contains("modified_pins");
            for (const auto& mp : remoteArgs["modify_pins"]) {
                if (!mp.is_object()) continue;
                std::string pinId = mp.value("pin_id", mp.value("id", ""));
                if (pinId.empty()) continue;
                bool noConn = mp.value("no_connected", mp.value("noConnected", false));
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    _cachedPinNoConnect[pinId] = noConn;
                }
                if (needRemoteModPins) {
                    nlohmann::json apiRes;
                    if (invokeEasyEdaApi("eda.sch_PrimitivePin.modify",
                                         nlohmann::json::array({pinId, {{"noConnected", noConn}}}),
                                         apiRes)) {
                        ++modPinsCount;
                    }
                }
            }
            rpcResult["modified_pins"] = modPinsCount;
        }

        // 2. delete_primitives
        if (remoteArgs.contains("delete_primitives") && remoteArgs["delete_primitives"].is_array() &&
            !rpcResult.contains("deleted_primitives")) {
            int delCount = 0;
            for (const auto& dp : remoteArgs["delete_primitives"]) {
                std::string primId;
                std::string primType;
                if (dp.is_string()) {
                    primId = dp.get<std::string>();
                } else if (dp.is_object()) {
                    primId = dp.value("id", dp.value("primitive_id", ""));
                    primType = dp.value("type", "");
                }
                if (primId.empty()) continue;
                nlohmann::json apiRes;
                if (primType == "wire") {
                    invokeEasyEdaApi("eda.sch_PrimitiveWire.delete", nlohmann::json::array({primId}), apiRes);
                } else if (primType == "component") {
                    invokeEasyEdaApi("eda.sch_PrimitiveComponent.delete", nlohmann::json::array({primId}), apiRes);
                } else {
                    invokeEasyEdaApi("eda.sch_PrimitiveWire.delete", nlohmann::json::array({primId}), apiRes);
                    invokeEasyEdaApi("eda.sch_PrimitiveComponent.delete", nlohmann::json::array({primId}), apiRes);
                }
                if (schState.contains("wires") && schState["wires"].is_array()) {
                    auto& arr = schState["wires"];
                    for (auto it = arr.begin(); it != arr.end(); ) {
                        if (it->is_object() && it->value("id", "") == primId) {
                            it = arr.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
                if (schState.contains("components") && schState["components"].is_array()) {
                    auto& arr = schState["components"];
                    for (auto it = arr.begin(); it != arr.end(); ) {
                        if (it->is_object() && (it->value("id", "") == primId || it->value("reference", "") == primId)) {
                            it = arr.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
                ++delCount;
            }
            rpcResult["deleted_primitives"] = delCount;
        }

        // 3. add_netflags
        if (remoteArgs.contains("add_netflags") && remoteArgs["add_netflags"].is_array() &&
            !rpcResult.contains("added_netflags")) {
            int nfCount = 0;
            for (const auto& nf : remoteArgs["add_netflags"]) {
                if (!nf.is_object()) continue;
                std::string netName = nf.value("net", nf.value("name", nf.value("value", "GND")));
                std::string nfType = nf.value(
                    "type",
                    (netName == "GND" || netName == "AGND" || netName == "PGND") ? "Ground" : "Power");
                double xVal = scaleCoord(nf.value("x", 0.0));
                double yVal = scaleCoord(nf.value("y", 0.0));
                double rotVal = nf.value("rotation", 0.0);
                bool mirrorVal = nf.value("mirror", false);
                nlohmann::json apiRes;
                if (invokeEasyEdaApi("eda.sch_PrimitiveComponent.createNetFlag",
                                     nlohmann::json::array({nfType, netName, xVal, yVal, rotVal, mirrorVal}),
                                     apiRes)) {
                    ++nfCount;
                    schState["components"].push_back({
                        {"id", "nf_added_" + std::to_string(nfCount)},
                        {"reference", "#" + nfType},
                        {"value", netName},
                        {"component_type", "netflag"},
                        {"x", xVal},
                        {"y", yVal},
                        {"pins", nlohmann::json::array()}
                    });
                }
            }
            rpcResult["added_netflags"] = nfCount;
        }

        // 4. modify_components (including x/y move and preserve_connections wire rubber-banding)
        if (remoteArgs.contains("modify_components") && remoteArgs["modify_components"].is_array()) {
            int modCompCount = rpcResult.value("modified_components", 0);
            bool needRemoteModComp = !rpcResult.contains("modified_components");
            nlohmann::json rubberBandedWires = rpcResult.value("rubber_banded_wires", nlohmann::json::array());
            bool topPreserveConn = remoteArgs.value("preserve_connections", false);

            for (const auto& mc : remoteArgs["modify_components"]) {
                if (!mc.is_object()) continue;
                std::string compId = mc.value("id", mc.value("primitive_id", ""));
                std::string targetRef = mc.value("reference", mc.value("designator", ""));

                nlohmann::json* matchedComp = nullptr;
                for (auto& existingComp : schState["components"]) {
                    if (!existingComp.is_object()) continue;
                    std::string eId = existingComp.value("id", "");
                    std::string eRef = existingComp.value("reference", existingComp.value("ref", ""));
                    if ((!compId.empty() && eId == compId) || (!targetRef.empty() && eRef == targetRef)) {
                        matchedComp = &existingComp;
                        if (compId.empty()) compId = eId;
                        break;
                    }
                }
                if (compId.empty() && targetRef.empty()) continue;

                nlohmann::json modProps = nlohmann::json::object();
                if (!targetRef.empty()) {
                    modProps["designator"] = targetRef;
                    if (matchedComp != nullptr) {
                        (*matchedComp)["reference"] = targetRef;
                    }
                }
                if (mc.contains("value") || mc.contains("name")) {
                    std::string v = mc.value("value", mc.value("name", ""));
                    modProps["name"] = v;
                    if (matchedComp != nullptr) {
                        (*matchedComp)["value"] = v;
                    }
                }
                if (mc.contains("unique_id") || mc.contains("uniqueId")) {
                    std::string uId = mc.value("unique_id", mc.value("uniqueId", ""));
                    modProps["uniqueId"] = uId;
                    if (matchedComp != nullptr) {
                        (*matchedComp)["unique_id"] = uId;
                    }
                }
                if (mc.contains("footprint")) {
                    modProps["footprint"] = mc["footprint"];
                    if (matchedComp != nullptr) {
                        (*matchedComp)["footprint"] = mc["footprint"];
                    }
                }
                if (mc.contains("component_type") || mc.contains("componentType")) {
                    std::string cTypeVal = mc.value("component_type", mc.value("componentType", ""));
                    modProps["componentType"] = cTypeVal;
                    if (matchedComp != nullptr) {
                        (*matchedComp)["component_type"] = cTypeVal;
                    }
                }
                if (mc.contains("add_into_bom")) {
                    modProps["addIntoBom"] = mc.value("add_into_bom", true);
                }
                if (mc.contains("add_into_pcb")) {
                    modProps["addIntoPcb"] = mc.value("add_into_pcb", true);
                }
                if (mc.contains("x")) {
                    modProps["x"] = scaleCoord(mc.value("x", 0.0));
                }
                if (mc.contains("y")) {
                    modProps["y"] = scaleCoord(mc.value("y", 0.0));
                }
                if (mc.contains("rotation")) {
                    modProps["rotation"] = mc.value("rotation", 0.0);
                }

                if ((needRemoteModComp || mc.contains("footprint")) && compId.empty() && !targetRef.empty() &&
                    !(inMemorySchEdit && !_connected && gw == nullptr)) {
                    nlohmann::json allRes;
                    if (invokeEasyEdaApi("eda.sch_PrimitiveComponent.getAll",
                                         nlohmann::json::array({nullptr, true}), allRes) &&
                        allRes.contains("result") && allRes["result"].is_array()) {
                        for (const auto& rc : allRes["result"]) {
                            if (rc.is_object() &&
                                rc.value("designator", rc.value("reference", "")) == targetRef) {
                                compId = rc.value("primitiveId", rc.value("id", ""));
                                break;
                            }
                        }
                    }
                }

                if (needRemoteModComp && !compId.empty()) {
                    nlohmann::json apiRes;
                    if (invokeEasyEdaApi("eda.sch_PrimitiveComponent.modify",
                                         nlohmann::json::array({compId, modProps}),
                                         apiRes)) {
                        ++modCompCount;
                    }
                } else if (!needRemoteModComp && mc.contains("footprint") && !compId.empty()) {
                    nlohmann::json apiRes;
                    invokeEasyEdaApi("eda.sch_PrimitiveComponent.modify",
                                     nlohmann::json::array({compId, {{"footprint", mc["footprint"]}}}),
                                     apiRes);
                    if (modCompCount == 0) ++modCompCount;
                } else if (needRemoteModComp && matchedComp != nullptr) {
                    ++modCompCount;
                }

                if (matchedComp != nullptr && (mc.contains("x") || mc.contains("y"))) {
                    double oldX = matchedComp->value("x", 0.0);
                    double oldY = matchedComp->value("y", 0.0);
                    double newX = mc.contains("x") ? scaleCoord(mc.value("x", oldX)) : oldX;
                    double newY = mc.contains("y") ? scaleCoord(mc.value("y", oldY)) : oldY;
                    double dx = newX - oldX;
                    double dy = newY - oldY;
                    (*matchedComp)["x"] = newX;
                    (*matchedComp)["y"] = newY;

                    std::vector<std::pair<double, double>> oldPinCoords;
                    if (matchedComp->contains("pins") && (*matchedComp)["pins"].is_array()) {
                        for (auto& pin : (*matchedComp)["pins"]) {
                            if (!pin.is_object()) continue;
                            double px = pin.value("x", oldX);
                            double py = std::fabs(pin.value("y", oldY));
                            oldPinCoords.push_back({px, py});
                            pin["x"] = px + dx;
                            pin["y"] = py + dy;
                        }
                    }

                    bool preserveConn = mc.value("preserve_connections", topPreserveConn);
                    if (preserveConn &&
                        !oldPinCoords.empty() && schState.contains("wires") && schState["wires"].is_array()) {
                        const double kPinTouchTol = 2.5;
                        for (auto& w : schState["wires"]) {
                            if (!w.is_object()) continue;
                            double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
                            bool hasLineArr = (w.contains("line") && w["line"].is_array() && w["line"].size() >= 2);
                            if (hasLineArr && w["line"][0].is_array() && w["line"][0].size() >= 2 &&
                                w["line"].back().is_array() && w["line"].back().size() >= 2) {
                                x1 = w["line"][0][0].get<double>();
                                y1 = w["line"][0][1].get<double>();
                                x2 = w["line"].back()[0].get<double>();
                                y2 = w["line"].back()[1].get<double>();
                            } else {
                                x1 = w.value("x1", 0.0);
                                y1 = w.value("y1", 0.0);
                                x2 = w.value("x2", 0.0);
                                y2 = w.value("y2", 0.0);
                            }

                            bool moved1 = false, moved2 = false;
                            for (const auto& op : oldPinCoords) {
                                if (!moved1 && std::hypot(x1 - op.first, y1 - op.second) <= kPinTouchTol) {
                                    x1 = op.first + dx;
                                    y1 = op.second + dy;
                                    moved1 = true;
                                }
                                if (!moved2 && std::hypot(x2 - op.first, y2 - op.second) <= kPinTouchTol) {
                                    x2 = op.first + dx;
                                    y2 = op.second + dy;
                                    moved2 = true;
                                }
                            }

                            if (moved1 || moved2) {
                                w["x1"] = x1;
                                w["y1"] = y1;
                                w["x2"] = x2;
                                w["y2"] = y2;
                                if (hasLineArr) {
                                    w["line"][0] = nlohmann::json::array({x1, y1});
                                    w["line"].back() = nlohmann::json::array({x2, y2});
                                }
                                std::string wId = w.value("id", "");
                                if (!wId.empty() && (std::fabs(dx) > 1e-6 || std::fabs(dy) > 1e-6) &&
                                    !(inMemorySchEdit && !_connected && gw == nullptr)) {
                                    nlohmann::json delRes, createRes;
                                    invokeEasyEdaApi("eda.sch_PrimitiveWire.delete",
                                                     nlohmann::json::array({wId}), delRes);
                                    invokeEasyEdaApi("eda.sch_PrimitiveWire.create",
                                                     nlohmann::json::array({nlohmann::json::array({x1, y1, x2, y2}), w.value("net", "")}),
                                                     createRes);
                                }
                                rubberBandedWires.push_back({
                                    {"id", wId},
                                    {"net", w.value("net", "")},
                                    {"x1", x1},
                                    {"y1", y1},
                                    {"x2", x2},
                                    {"y2", y2}
                                });
                            }
                        }
                    }
                }
            }
            rpcResult["modified_components"] = modCompCount;
            if (topPreserveConn || !rubberBandedWires.empty()) {
                rpcResult["rubber_banded_wires"] = rubberBandedWires;
                rpcResult["rubber_banded_wires_count"] = static_cast<int>(rubberBandedWires.size());
            }
        }

        // modify_wires: update wire network assignments or geometry
        if (remoteArgs.contains("modify_wires") && remoteArgs["modify_wires"].is_array()) {
            int modWireCount = rpcResult.value("modified_wires", 0);
            nlohmann::json modWireIds = rpcResult.value("modified_wire_ids", nlohmann::json::array());
            bool needRemoteModWires = !rpcResult.contains("modified_wires");
            for (const auto& mw : remoteArgs["modify_wires"]) {
                if (!mw.is_object()) continue;
                std::string wireId = mw.value("id", mw.value("wire_id", mw.value("primitive_id", "")));
                if (wireId.empty()) continue;
                std::string newNet = mw.value("net", mw.value("net_name", ""));

                nlohmann::json modProps = nlohmann::json::object();
                if (!newNet.empty()) modProps["net"] = newNet;
                if (mw.contains("line")) modProps["line"] = mw["line"];

                if (schState.contains("wires") && schState["wires"].is_array()) {
                    for (auto& w : schState["wires"]) {
                        if (w.is_object() && w.value("id", "") == wireId) {
                            if (!newNet.empty()) w["net"] = newNet;
                            if (mw.contains("line")) w["line"] = mw["line"];
                            break;
                        }
                    }
                }

                if (needRemoteModWires) {
                    if (!(inMemorySchEdit && !_connected && gw == nullptr)) {
                        nlohmann::json apiRes;
                        invokeEasyEdaApi("eda.sch_PrimitiveWire.modify",
                                         nlohmann::json::array({wireId, modProps}), apiRes);
                    }
                    modWireIds.push_back(wireId);
                    ++modWireCount;
                }
            }
            rpcResult["modified_wires"] = modWireCount;
            if (!rpcResult.contains("modified_wire_ids") || rpcResult["modified_wire_ids"].empty()) {
                rpcResult["modified_wire_ids"] = std::move(modWireIds);
            }
        }

        // 5. add_components (with near_reference / auto_place on 50-mil grid and fallback placement)
        if (remoteArgs.contains("add_components") && remoteArgs["add_components"].is_array()) {
            bool alreadyPlaced = (rpcResult.value("placed_components", 0) > 0);
            if (alreadyPlaced && rpcResult.contains("placed_component_details") &&
                rpcResult["placed_component_details"].is_array()) {
                for (const auto& d : rpcResult["placed_component_details"]) {
                    if (d.is_object() && d.value("id", "").empty()) {
                        alreadyPlaced = false;
                        break;
                    }
                }
            }
            if (!alreadyPlaced) {
                int placedCount = 0;
                nlohmann::json detailsArr = nlohmann::json::array();
                for (const auto& ac : remoteArgs["add_components"]) {
                    if (!ac.is_object()) continue;
                    std::string compUuid = ac.value("uuid", ac.value("device_uuid", ""));
                    std::string libUuid = ac.value("library_uuid", ac.value("libraryUuid", ""));
                    std::string searchKey = ac.value("lcsc", ac.value("query", ac.value("value", "")));
                    if (!(inMemorySchEdit && !_connected && gw == nullptr) &&
                        (compUuid.empty() || libUuid.empty()) && !searchKey.empty()) {
                        nlohmann::json searchRes;
                        if (invokeEasyEdaApi("eda.lib_Device.search",
                                             nlohmann::json::array({searchKey}),
                                             searchRes) &&
                            searchRes.contains("result") && searchRes["result"].is_array() &&
                            !searchRes["result"].empty() && searchRes["result"][0].is_object()) {
                            const auto& hit = searchRes["result"][0];
                            if (compUuid.empty()) compUuid = hit.value("uuid", "");
                            if (libUuid.empty()) libUuid = hit.value("libraryUuid", hit.value("library_uuid", ""));
                        }
                    }

                    double xVal = scaleCoord(ac.value("x", ac.value("x_mm", 500.0)));
                    double yVal = scaleCoord(ac.value("y", ac.value("y_mm", 500.0)));
                    double rotVal = ac.value("rotation", ac.value("rotation_deg", 0.0));
                    bool mirrorVal = ac.value("mirror", false);
                    double compW = ac.value("width", 60.0);
                    double compH = ac.value("height", 40.0);
                    double rotMod = std::fmod(std::fabs(rotVal), 180.0);
                    if (std::fabs(rotMod - 90.0) < 45.0) std::swap(compW, compH);

                    std::string nearRef = ac.value("near_reference", "");
                    bool itemAutoPlace = ac.value("auto_place", false) || !nearRef.empty();
                    if (itemAutoPlace) {
                        double baseX = xVal;
                        double baseY = yVal;
                        if (!nearRef.empty()) {
                            for (const auto& existingComp : schState["components"]) {
                                if (!existingComp.is_object()) continue;
                                std::string eRef = existingComp.value("reference", existingComp.value("ref", ""));
                                if (eRef == nearRef || existingComp.value("id", "") == nearRef) {
                                    baseX = existingComp.value("x", baseX);
                                    baseY = existingComp.value("y", baseY);
                                    break;
                                }
                            }
                        }
                        baseX = snap50(baseX);
                        baseY = snap50(baseY);

                        std::vector<std::pair<int, int>> gridOffsets;
                        for (int r = 1; r <= 24; ++r) {
                            for (int dx = -r; dx <= r; ++dx) {
                                for (int dy = -r; dy <= r; ++dy) {
                                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                                    gridOffsets.push_back({dx, dy});
                                }
                            }
                        }
                        std::sort(gridOffsets.begin(), gridOffsets.end(),
                                  [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                                      int da = a.first * a.first + a.second * a.second;
                                      int db = b.first * b.first + b.second * b.second;
                                      if (da != db) return da < db;
                                      if (std::abs(a.second) != std::abs(b.second)) {
                                          return std::abs(a.second) < std::abs(b.second);
                                      }
                                      return a.first < b.first;
                                  });

                        const double kClearance = 25.0;
                        for (const auto& off : gridOffsets) {
                            double candX = baseX + off.first * 50.0;
                            double candY = baseY + off.second * 50.0;
                            double cMinX = candX - compW * 0.5 - kClearance;
                            double cMaxX = candX + compW * 0.5 + kClearance;
                            double cMinY = candY - compH * 0.5 - kClearance;
                            double cMaxY = candY + compH * 0.5 + kClearance;
                            bool collision = false;
                            for (const auto& existingComp : schState["components"]) {
                                if (!isPartComponent(existingComp)) continue;
                                double eMinX = 0.0, eMinY = 0.0, eMaxX = 0.0, eMaxY = 0.0;
                                computeCompBbox(existingComp, eMinX, eMinY, eMaxX, eMaxY);
                                if (std::min(cMaxX, eMaxX) > std::max(cMinX, eMinX) &&
                                    std::min(cMaxY, eMaxY) > std::max(cMinY, eMinY)) {
                                    collision = true;
                                    break;
                                }
                            }
                            if (!collision) {
                                xVal = candX;
                                yVal = candY;
                                break;
                            }
                        }
                    }

                    std::string createdId = ac.value("id", "");
                    if (inMemorySchEdit && !_connected && gw == nullptr) {
                        if (createdId.empty()) {
                            createdId = "comp_auto_" + std::to_string(schState["components"].size() + 1);
                        }
                    } else {
                        nlohmann::json devObj = libUuid.empty()
                            ? nlohmann::json(compUuid)
                            : nlohmann::json({{"uuid", compUuid}, {"libraryUuid", libUuid}});
                        nlohmann::json createRes;
                        if (invokeEasyEdaApi("eda.sch_PrimitiveComponent.create",
                                             nlohmann::json::array({devObj, xVal, yVal, nullptr, rotVal, mirrorVal}),
                                             createRes)) {
                            if (createRes.contains("result")) {
                                if (createRes["result"].is_string()) {
                                    createdId = createRes["result"].get<std::string>();
                                } else if (createRes["result"].is_object()) {
                                    createdId = createRes["result"].value(
                                        "primitiveId", createRes["result"].value("id", ""));
                                }
                            }
                        }
                    }

                    std::string ref = ac.value("reference", ac.value("ref", ""));
                    std::string val = ac.value("value", "");
                    std::string uid = ac.value("unique_id", ac.value("uniqueId", ""));
                    std::string fp = ac.value("footprint", "0603");
                    if (!createdId.empty() && !(inMemorySchEdit && !_connected && gw == nullptr) &&
                        (!ref.empty() || !val.empty() || !uid.empty())) {
                        nlohmann::json modProps = nlohmann::json::object();
                        if (!ref.empty()) modProps["designator"] = ref;
                        if (!val.empty()) modProps["name"] = val;
                        if (!uid.empty()) modProps["uniqueId"] = uid;
                        nlohmann::json modRes;
                        invokeEasyEdaApi("eda.sch_PrimitiveComponent.modify",
                                         nlohmann::json::array({createdId, modProps}),
                                         modRes);
                    }

                    if (!createdId.empty() || inMemorySchEdit) {
                        nlohmann::json pinsArr = nlohmann::json::array();
                        if (ac.contains("pins") && ac["pins"].is_array() && !ac["pins"].empty()) {
                            for (const auto& p : ac["pins"]) {
                                if (!p.is_object()) continue;
                                nlohmann::json pCopy = p;
                                if (p.contains("dx") || p.contains("dy")) {
                                    pCopy["x"] = xVal + p.value("dx", 0.0);
                                    pCopy["y"] = yVal + p.value("dy", 0.0);
                                }
                                pinsArr.push_back(std::move(pCopy));
                            }
                        } else {
                            pinsArr.push_back({
                                {"id", createdId + "_p1"},
                                {"number", "1"},
                                {"name", "1"},
                                {"net", ""},
                                {"x", xVal - 50.0},
                                {"y", yVal}
                            });
                            pinsArr.push_back({
                                {"id", createdId + "_p2"},
                                {"number", "2"},
                                {"name", "2"},
                                {"net", ""},
                                {"x", xVal + 50.0},
                                {"y", yVal}
                            });
                        }

                        schState["components"].push_back({
                            {"id", createdId},
                            {"reference", ref},
                            {"value", val},
                            {"component_type", "part"},
                            {"footprint", fp},
                            {"unique_id", uid},
                            {"x", xVal},
                            {"y", yVal},
                            {"width", ac.value("width", 40.0)},
                            {"height", ac.value("height", 20.0)},
                            {"rotation", rotVal},
                            {"pins", std::move(pinsArr)}
                        });

                        detailsArr.push_back({
                            {"reference", ref},
                            {"id", createdId},
                            {"x", xVal},
                            {"y", yVal},
                            {"rotation", rotVal},
                            {"auto_placed", itemAutoPlace},
                            {"near_reference", nearRef}
                        });
                        ++placedCount;
                    }
                }
                rpcResult["placed_components"] = placedCount;
                rpcResult["placed_component_details"] = std::move(detailsArr);
            }
        }

        // 6. Post-placement annotate_schematic pass and readback verification
        if (wantAnnotate) {
            runAnnotatePass();
            bool verified = true;
            std::set<std::string> checkRefs;
            for (const auto& c : schState["components"]) {
                if (!isPartComponent(c) || !c.value("on_active_sheet", true)) continue;
                std::string r = c.value("reference", "");
                if (r.empty() || r.find('?') != std::string::npos || !checkRefs.insert(r).second) {
                    verified = false;
                    break;
                }
            }
            rpcResult["annotated_count"] = totalAnnotatedCount;
            rpcResult["annotated_components"] = totalAnnotatedCount;
            rpcResult["annotation_verified"] = verified;
            rpcResult["annotation_details"] = annotationDetails;
        }

        // 7. connect_to_net outward 50-mil snapped wire stub router
        if (wantConnectToNet) {
            nlohmann::json connectedStubs = nlohmann::json::array();
            int stubIdx = 1;
            for (const auto& cn : remoteArgs["connect_to_net"]) {
                if (!cn.is_object()) continue;
                std::string targetRef = cn.value("reference", cn.value("ref", ""));
                std::string targetPin = cn.value("pin_number", cn.value("pin", cn.value("number", cn.value("pin_name", ""))));
                std::string netName = cn.value("net", cn.value("net_name", ""));
                double stubLen = std::max(50.0, snap50(scaleCoord(cn.value("stub_length_mils", cn.value("stub_length", 100.0)))));
                bool useNetFlag = cn.value("use_netflag", false);
                if (!useNetFlag && (netName == "GND" || netName == "AGND" || netName == "PGND" ||
                                    netName == "VCC" || netName == "VDD" || netName == "+3V3" ||
                                    netName == "+5V" || netName == "+12V")) {
                    useNetFlag = true;
                }
                if (targetRef.empty() || targetPin.empty() || netName.empty()) continue;

                for (auto& comp : schState["components"]) {
                    if (!comp.is_object()) continue;
                    std::string cRef = comp.value("reference", comp.value("ref", ""));
                    if (cRef != targetRef && comp.value("id", "") != targetRef) continue;
                    if (!comp.contains("pins") || !comp["pins"].is_array()) continue;

                    double cx = comp.value("x", 0.0);
                    double cy = comp.value("y", 0.0);
                    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
                    computeCompBbox(comp, minX, minY, maxX, maxY);

                    for (auto& pinObj : comp["pins"]) {
                        if (!pinObj.is_object()) continue;
                        std::string pNum = pinObj.value("number", pinObj.value("pin_number", ""));
                        std::string pName = pinObj.value("name", pinObj.value("pin_name", ""));
                        std::string pId = pinObj.value("id", "");
                        if (pNum != targetPin && pName != targetPin && pId != targetPin) continue;

                        double x1 = snap50(pinObj.value("x", cx));
                        double y1 = snap50(pinObj.value("y", cy));
                        pinObj["x"] = x1;
                        pinObj["y"] = y1;
                        pinObj["net"] = netName;

                        const std::pair<double, double> kDirs[4] = {
                            {-1.0, 0.0}, {1.0, 0.0}, {0.0, -1.0}, {0.0, 1.0}
                        };
                        double bestUx = (x1 < cx) ? -1.0 : 1.0;
                        double bestUy = 0.0;
                        double bestScore = -1e18;

                        for (const auto& d : kDirs) {
                            double ux = d.first;
                            double uy = d.second;
                            double tx = x1 + ux * stubLen;
                            double ty = y1 + uy * stubLen;
                            double mx = x1 + ux * (stubLen * 0.5);
                            double my = y1 + uy * (stubLen * 0.5);

                            bool tipOutside = (tx < minX || tx > maxX || ty < minY || ty > maxY);
                            bool midInside = (mx > minX + 1e-3 && mx < maxX - 1e-3 &&
                                              my > minY + 1e-3 && my < maxY - 1e-3);
                            if (!tipOutside || midInside) continue;

                            bool hitsOtherPin = false;
                            for (const auto& otherP : comp["pins"]) {
                                if (!otherP.is_object() || &otherP == &pinObj) continue;
                                double opx = otherP.value("x", cx);
                                double opy = otherP.value("y", cy);
                                if (std::hypot(tx - opx, ty - opy) < 10.0 ||
                                    std::hypot(mx - opx, my - opy) < 10.0) {
                                    hitsOtherPin = true;
                                    break;
                                }
                            }
                            if (hitsOtherPin) continue;

                            double outwardDot = (x1 - cx) * ux + (y1 - cy) * uy;
                            if (std::fabs(ux) > 0.5 && std::fabs(x1 - cx) >= std::fabs(y1 - cy)) {
                                outwardDot += 5.0;
                            }
                            if (outwardDot > bestScore) {
                                bestScore = outwardDot;
                                bestUx = ux;
                                bestUy = uy;
                            }
                        }

                        double x2 = snap50(x1 + bestUx * stubLen);
                        double y2 = snap50(y1 + bestUy * stubLen);
                        std::string wireId = "w_stub_" + std::to_string(stubIdx++);

                        if (!(inMemorySchEdit && !_connected && gw == nullptr)) {
                            nlohmann::json wireRes;
                            invokeEasyEdaApi("eda.sch_PrimitiveWire.create",
                                             nlohmann::json::array({
                                                 nlohmann::json::array({x1, y1, x2, y2}),
                                                 netName
                                             }),
                                             wireRes);
                            if (useNetFlag) {
                                std::string nfType = (netName == "GND" || netName == "AGND" || netName == "PGND")
                                    ? "Ground" : "Power";
                                nlohmann::json nfRes;
                                invokeEasyEdaApi("eda.sch_PrimitiveComponent.createNetFlag",
                                                 nlohmann::json::array({nfType, netName, x2, y2, 0.0, false}),
                                                 nfRes);
                            }
                        }

                        schState["wires"].push_back({
                            {"id", wireId},
                            {"net", netName},
                            {"line", nlohmann::json::array({{x1, y1}, {x2, y2}})}
                        });
                        schState["components"].push_back({
                            {"id", "nf_" + wireId},
                            {"reference", useNetFlag ? "#PWR" : "#NET"},
                            {"value", netName},
                            {"component_type", useNetFlag ? "netflag" : "netlabel"},
                            {"x", x2},
                            {"y", y2},
                            {"pins", nlohmann::json::array()}
                        });

                        connectedStubs.push_back({
                            {"reference", cRef},
                            {"pin_number", pNum},
                            {"net", netName},
                            {"wire_id", wireId},
                            {"x1", x1},
                            {"y1", y1},
                            {"x2", x2},
                            {"y2", y2},
                            {"stub_length_mils", stubLen},
                            {"netflag_added", useNetFlag}
                        });
                        break;
                    }
                    break;
                }
            }
            rpcResult["connected_net_stubs"] = std::move(connectedStubs);
            rpcResult["placed_wires"] = rpcResult.value("placed_wires", 0) +
                                        static_cast<int>(rpcResult["connected_net_stubs"].size());
        }

        if (inMemorySchEdit || wantAnnotate || wantConnectToNet || wantAutoPlace) {
            rpcResult["schematic_state"] = schState;
        }
    } else if (toolName == "easyeda_inspect_pcb" && rpcResult.is_object() && jsonFlag(rpcResult, "ok", true)) {
        // Merge cached multi-sheet schematic components into rpcResult["schematic_components"]
        nlohmann::json mergedSchComps = nlohmann::json::array();
        std::set<std::string> seenSchRefs;
        if (rpcResult.contains("schematic_components") && rpcResult["schematic_components"].is_array()) {
            for (const auto& sc : rpcResult["schematic_components"]) {
                if (!sc.is_object()) continue;
                std::string r = sc.value("reference", sc.value("ref", ""));
                if (!r.empty() && seenSchRefs.insert(r).second) {
                    mergedSchComps.push_back(sc);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            for (const auto& kv : _cachedSchematicComponentsBySheet) {
                if (!kv.second.is_array()) continue;
                for (const auto& sc : kv.second) {
                    if (!sc.is_object()) continue;
                    std::string r = sc.value("reference", sc.value("ref", ""));
                    if (!r.empty() && seenSchRefs.insert(r).second) {
                        mergedSchComps.push_back(sc);
                    }
                }
            }
        }
        rpcResult["schematic_components"] = mergedSchComps;

        if ((!rpcResult.contains("board_outline") || !rpcResult["board_outline"].is_array() ||
             rpcResult["board_outline"].empty()) &&
            (_connected || gw != nullptr)) {
            nlohmann::json outlineSegs = nlohmann::json::array();
            auto isOutlineLayerVal = [](const nlohmann::json& lv) -> bool {
                if (lv.is_number_integer()) return lv.get<int>() == 11;
                if (lv.is_string()) {
                    std::string s = lv.get<std::string>();
                    return s == "11" || s == "BoardOutLine" || s == "Edge.Cuts";
                }
                return false;
            };
            auto appendPolyObjSegments = [&](const nlohmann::json& item, double defaultW) {
                if (!item.is_object() || !item.contains("layer") || !isOutlineLayerVal(item["layer"])) {
                    return;
                }
                std::string recId = item.value("primitiveId", item.value("id", ""));
                std::string lidStr = item["layer"].is_string()
                    ? item["layer"].get<std::string>()
                    : std::to_string(item["layer"].get<int>());
                double wVal = item.contains("lineWidth") && item["lineWidth"].is_number()
                    ? item["lineWidth"].get<double>() : defaultW;
                if (!item.contains("polygon")) return;
                const nlohmann::json& rawPoly = item["polygon"];
                const nlohmann::json* srcArr = nullptr;
                if (rawPoly.is_array()) {
                    srcArr = &rawPoly;
                } else if (rawPoly.is_object()) {
                    for (const char* k : {"polygon", "Source", "source", "points"}) {
                        if (rawPoly.contains(k) && rawPoly[k].is_array()) {
                            srcArr = &rawPoly[k];
                            break;
                        }
                    }
                }
                if (srcArr == nullptr) return;
                std::vector<std::pair<double, double>> pts;
                for (size_t i = 0; i < srcArr->size(); ++i) {
                    const auto& tok = (*srcArr)[i];
                    if (tok.is_string() && toUpperAscii(tok.get<std::string>()) == "ARC") {
                        if (i + 1 < srcArr->size() && (*srcArr)[i + 1].is_number()) {
                            ++i;
                        }
                        continue;
                    }
                    if (tok.is_object() && tok.contains("x") && tok.contains("y") &&
                        tok["x"].is_number() && tok["y"].is_number()) {
                        pts.emplace_back(tok["x"].get<double>(), tok["y"].get<double>());
                    } else if (tok.is_array() && tok.size() >= 2 &&
                               tok[0].is_number() && tok[1].is_number()) {
                        pts.emplace_back(tok[0].get<double>(), tok[1].get<double>());
                    } else if (tok.is_number() && i + 1 < srcArr->size() &&
                               (*srcArr)[i + 1].is_number()) {
                        pts.emplace_back(tok.get<double>(), (*srcArr)[i + 1].get<double>());
                        ++i;
                    }
                }
                for (size_t i = 0; i + 1 < pts.size(); ++i) {
                    outlineSegs.push_back({
                        {"id", recId},
                        {"layer", lidStr},
                        {"width", wVal},
                        {"x1", pts[i].first},
                        {"y1", pts[i].second},
                        {"x2", pts[i + 1].first},
                        {"y2", pts[i + 1].second}
                    });
                }
                if (pts.size() >= 3 &&
                    std::hypot(pts.front().first - pts.back().first,
                               pts.front().second - pts.back().second) > 1e-3) {
                    outlineSegs.push_back({
                        {"id", recId},
                        {"layer", lidStr},
                        {"width", wVal},
                        {"x1", pts.back().first},
                        {"y1", pts.back().second},
                        {"x2", pts.front().first},
                        {"y2", pts.front().second}
                    });
                }
            };
            for (const char* apiPath : {"eda.pcb_PrimitivePolyline.getAll", "eda.pcb_PrimitiveRegion.getAll"}) {
                nlohmann::json apiRes;
                if (invokeEasyEdaApi(apiPath, nlohmann::json::array(), apiRes) &&
                    apiRes.is_object() && apiRes.contains("result") && apiRes["result"].is_array()) {
                    for (const auto& item : apiRes["result"]) {
                        appendPolyObjSegments(item, 0.25);
                    }
                }
                if (!outlineSegs.empty()) break;
            }
            if (!outlineSegs.empty()) {
                rpcResult["board_outline"] = std::move(outlineSegs);
            }
        }

        hydrateLiveEasyEdaPours(rpcResult);

        EdaAccelerators accCopy;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            accCopy = _accelerators;
        }
        bool auditOk = false;
        std::string auditSummary;
        rpcResult["connectivity_audit"] = accCopy.calcPcbConnectivityAudit(rpcResult, auditOk, auditSummary);

        std::set<std::string> refSet;
        if (arguments.is_object()) {
            for (const char* refKey : {"references", "component_refs", "refs", "reference", "ref_filter"}) {
                if (arguments.contains(refKey)) {
                    if (arguments[refKey].is_array()) {
                        for (const auto& r : arguments[refKey]) {
                            if (r.is_string() && !r.get<std::string>().empty()) {
                                refSet.insert(r.get<std::string>());
                            }
                        }
                    } else if (arguments[refKey].is_string() && !arguments[refKey].get<std::string>().empty()) {
                        refSet.insert(arguments[refKey].get<std::string>());
                    }
                }
            }
        }

        std::set<std::string> netSet;
        if (arguments.is_object()) {
            for (const char* netKey : {"nets", "net_filter", "net"}) {
                if (arguments.contains(netKey)) {
                    if (arguments[netKey].is_array()) {
                        for (const auto& n : arguments[netKey]) {
                            if (n.is_string() && !n.get<std::string>().empty()) {
                                netSet.insert(n.get<std::string>());
                            }
                        }
                    } else if (arguments[netKey].is_string() && !arguments[netKey].get<std::string>().empty()) {
                        netSet.insert(arguments[netKey].get<std::string>());
                    }
                }
            }
        }

        auto normalizeLayer = [](const std::string& l) -> std::string {
            std::string s = toUpperAscii(l);
            if (s == "TOPLAYER" || s == "F.CU" || s == "1" || s == "TOP") return "1";
            if (s == "BOTTOMLAYER" || s == "B.CU" || s == "2" || s == "BOTTOM") return "2";
            if (s == "INNERLAYER1" || s == "IN1.CU" || s == "15") return "15";
            if (s == "INNERLAYER2" || s == "IN2.CU" || s == "16") return "16";
            if (s == "INNERLAYER3" || s == "IN3.CU" || s == "17") return "17";
            if (s == "INNERLAYER4" || s == "IN4.CU" || s == "18") return "18";
            if (s == "MULTILAYER" || s == "*.CU" || s == "12" || s == "PTH" || s == "MULTI-LAYER") return "12";
            return s;
        };

        std::set<std::string> layerSet;
        if (arguments.is_object()) {
            for (const char* layerKey : {"layers", "layer"}) {
                if (arguments.contains(layerKey)) {
                    if (arguments[layerKey].is_array()) {
                        for (const auto& l : arguments[layerKey]) {
                            if (l.is_string() && !l.get<std::string>().empty()) {
                                layerSet.insert(normalizeLayer(l.get<std::string>()));
                            } else if (l.is_number_integer()) {
                                layerSet.insert(normalizeLayer(std::to_string(l.get<int>())));
                            }
                        }
                    } else if (arguments[layerKey].is_string() && !arguments[layerKey].get<std::string>().empty()) {
                        layerSet.insert(normalizeLayer(arguments[layerKey].get<std::string>()));
                    } else if (arguments[layerKey].is_number_integer()) {
                        layerSet.insert(normalizeLayer(std::to_string(arguments[layerKey].get<int>())));
                    }
                }
            }
        }

        bool hasBbox = false;
        double bbMinX = 0.0, bbMinY = 0.0, bbMaxX = 0.0, bbMaxY = 0.0;
        if (arguments.is_object() && arguments.contains("bbox") && arguments["bbox"].is_object()) {
            const auto& bb = arguments["bbox"];
            std::string cUnits = rpcResult.value("units", "mil");
            bool canvasMil = (cUnits == "mil");
            if (bb.contains("min_x_mm") && bb.contains("min_y_mm") && bb.contains("max_x_mm") && bb.contains("max_y_mm")) {
                hasBbox = true;
                double s = canvasMil ? 39.37007874 : 1.0;
                bbMinX = bb["min_x_mm"].get<double>() * s;
                bbMinY = bb["min_y_mm"].get<double>() * s;
                bbMaxX = bb["max_x_mm"].get<double>() * s;
                bbMaxY = bb["max_y_mm"].get<double>() * s;
            } else if (bb.contains("min_x") && bb.contains("min_y") && bb.contains("max_x") && bb.contains("max_y")) {
                hasBbox = true;
                bbMinX = bb["min_x"].get<double>();
                bbMinY = bb["min_y"].get<double>();
                bbMaxX = bb["max_x"].get<double>();
                bbMaxY = bb["max_y"].get<double>();
            } else if (bb.contains("x_min") && bb.contains("y_min") && bb.contains("x_max") && bb.contains("y_max")) {
                hasBbox = true;
                bbMinX = bb["x_min"].get<double>();
                bbMinY = bb["y_min"].get<double>();
                bbMaxX = bb["x_max"].get<double>();
                bbMaxY = bb["y_max"].get<double>();
            }
            if (hasBbox) {
                if (bbMinX > bbMaxX) std::swap(bbMinX, bbMaxX);
                if (bbMinY > bbMaxY) std::swap(bbMinY, bbMaxY);
            }
        }

        auto layerMatches = [&](const nlohmann::json& item) -> bool {
            if (layerSet.empty()) return true;
            std::string lStr;
            if (item.contains("layer")) {
                if (item["layer"].is_string()) lStr = item["layer"].get<std::string>();
                else if (item["layer"].is_number_integer()) lStr = std::to_string(item["layer"].get<int>());
            }
            std::string norm = normalizeLayer(lStr);
            if (norm == "12") return true; // MultiLayer matches any layer
            return layerSet.count(norm) > 0;
        };

        auto ptInBbox = [&](double px, double py) -> bool {
            if (!hasBbox) return true;
            return (px >= bbMinX - 1e-3 && px <= bbMaxX + 1e-3 &&
                    py >= bbMinY - 1e-3 && py <= bbMaxY + 1e-3);
        };

        bool summaryOnly = arguments.is_object() ? arguments.value("summary_only", false) : false;
        bool hasFilter = (!refSet.empty() || !netSet.empty() || !layerSet.empty() || hasBbox);

        if (hasFilter) {
            std::set<std::string> effectiveNetSet = netSet;
            std::set<std::string> effectiveRefSet = refSet;
            struct PadCoord {
                double x;
                double y;
                bool isMm;
            };
            std::vector<PadCoord> matchedPadCoords;
            if (rpcResult.contains("pads") && rpcResult["pads"].is_array()) {
                for (const auto& p : rpcResult["pads"]) {
                    if (!p.is_object()) continue;
                    std::string pr = p.value("ref", p.value("reference", ""));
                    std::string pn = p.value("net", "");
                    if (!refSet.empty() && refSet.count(pr) > 0) {
                        bool isMm = p.contains("x_mm");
                        double px = isMm ? p.value("x_mm", 0.0) : p.value("x", 0.0);
                        double py = isMm ? p.value("y_mm", 0.0) : p.value("y", 0.0);
                        matchedPadCoords.push_back({px, py, isMm});
                    }
                    if (!netSet.empty() && netSet.count(pn) > 0 && refSet.empty() && !pr.empty()) {
                        effectiveRefSet.insert(pr);
                    }
                }
            }

            if (rpcResult.contains("components") && rpcResult["components"].is_array()) {
                nlohmann::json fComps = nlohmann::json::array();
                for (const auto& c : rpcResult["components"]) {
                    if (!c.is_object()) continue;
                    std::string cr = c.value("reference", c.value("ref", ""));
                    if (!effectiveRefSet.empty() && effectiveRefSet.count(cr) == 0) continue;
                    if (!layerMatches(c)) continue;
                    double cx = c.value("x", c.value("x_mm", 0.0));
                    double cy = c.value("y", c.value("y_mm", 0.0));
                    if (!ptInBbox(cx, cy)) continue;
                    fComps.push_back(c);
                }
                rpcResult["components"] = std::move(fComps);
                rpcResult["components_count"] = static_cast<int>(rpcResult["components"].size());
            }

            if (rpcResult.contains("pads") && rpcResult["pads"].is_array()) {
                nlohmann::json fPads = nlohmann::json::array();
                for (const auto& p : rpcResult["pads"]) {
                    if (!p.is_object()) continue;
                    std::string pr = p.value("ref", p.value("reference", ""));
                    std::string pn = p.value("net", "");
                    bool matchRef = !refSet.empty() && refSet.count(pr) > 0;
                    bool matchNet = !netSet.empty() && netSet.count(pn) > 0;
                    if (!refSet.empty() || !netSet.empty()) {
                        if (!matchRef && !matchNet) continue;
                    }
                    if (!layerMatches(p)) continue;
                    double px = p.value("x", p.value("x_mm", 0.0));
                    double py = p.value("y", p.value("y_mm", 0.0));
                    if (!ptInBbox(px, py)) continue;
                    fPads.push_back(p);
                }
                rpcResult["pads"] = std::move(fPads);
                rpcResult["pads_count"] = static_cast<int>(rpcResult["pads"].size());
            }

            if (rpcResult.contains("tracks") && rpcResult["tracks"].is_array()) {
                nlohmann::json fTracks = nlohmann::json::array();
                for (const auto& t : rpcResult["tracks"]) {
                    if (!t.is_object()) continue;
                    bool matchTrack = false;
                    if (!effectiveNetSet.empty() && effectiveNetSet.count(t.value("net", "")) > 0) {
                        matchTrack = true;
                    } else if (!matchedPadCoords.empty() && netSet.empty()) {
                        bool isMm = t.contains("x1_mm");
                        double x1 = isMm ? t.value("x1_mm", 0.0) : t.value("x1", 0.0);
                        double y1 = isMm ? t.value("y1_mm", 0.0) : t.value("y1", 0.0);
                        double x2 = isMm ? t.value("x2_mm", 0.0) : t.value("x2", 0.0);
                        double y2 = isMm ? t.value("y2_mm", 0.0) : t.value("y2", 0.0);
                        const double tol = isMm ? 0.65 : 25.0;
                        for (const auto& pc : matchedPadCoords) {
                            if (pc.isMm != isMm) continue;
                            if (std::hypot(x1 - pc.x, y1 - pc.y) <= tol ||
                                std::hypot(x2 - pc.x, y2 - pc.y) <= tol) {
                                matchTrack = true;
                                break;
                            }
                        }
                    } else if (effectiveNetSet.empty() && refSet.empty()) {
                        matchTrack = true;
                    }
                    if (!matchTrack) continue;
                    if (!layerMatches(t)) continue;
                    double x1 = t.value("x1", t.value("x1_mm", 0.0));
                    double y1 = t.value("y1", t.value("y1_mm", 0.0));
                    double x2 = t.value("x2", t.value("x2_mm", 0.0));
                    double y2 = t.value("y2", t.value("y2_mm", 0.0));
                    if (hasBbox && !ptInBbox(x1, y1) && !ptInBbox(x2, y2)) continue;
                    fTracks.push_back(t);
                }
                rpcResult["tracks"] = std::move(fTracks);
                rpcResult["tracks_count"] = static_cast<int>(rpcResult["tracks"].size());
            }

            if (rpcResult.contains("vias") && rpcResult["vias"].is_array()) {
                nlohmann::json fVias = nlohmann::json::array();
                for (const auto& v : rpcResult["vias"]) {
                    if (!v.is_object()) continue;
                    bool matchVia = false;
                    if (!effectiveNetSet.empty() && effectiveNetSet.count(v.value("net", "")) > 0) {
                        matchVia = true;
                    } else if (!matchedPadCoords.empty() && netSet.empty()) {
                        bool isMm = v.contains("x_mm");
                        double vx = isMm ? v.value("x_mm", 0.0) : v.value("x", 0.0);
                        double vy = isMm ? v.value("y_mm", 0.0) : v.value("y", 0.0);
                        const double tol = isMm ? 1.27 : 50.0;
                        for (const auto& pc : matchedPadCoords) {
                            if (pc.isMm != isMm) continue;
                            if (std::hypot(vx - pc.x, vy - pc.y) <= tol) {
                                matchVia = true;
                                break;
                            }
                        }
                    } else if (effectiveNetSet.empty() && refSet.empty()) {
                        matchVia = true;
                    }
                    if (!matchVia) continue;
                    double vx = v.value("x", v.value("x_mm", 0.0));
                    double vy = v.value("y", v.value("y_mm", 0.0));
                    if (!ptInBbox(vx, vy)) continue;
                    fVias.push_back(v);
                }
                rpcResult["vias"] = std::move(fVias);
                rpcResult["vias_count"] = static_cast<int>(rpcResult["vias"].size());
            }

            if (rpcResult.contains("pours") && rpcResult["pours"].is_array()) {
                nlohmann::json fPours = nlohmann::json::array();
                for (const auto& pr : rpcResult["pours"]) {
                    if (!pr.is_object()) continue;
                    if (!effectiveNetSet.empty() && effectiveNetSet.count(pr.value("net", "")) == 0) continue;
                    if (!layerMatches(pr)) continue;
                    fPours.push_back(pr);
                }
                rpcResult["pours"] = std::move(fPours);
                rpcResult["pours_count"] = static_cast<int>(rpcResult["pours"].size());
            }

            if (!callerRequestedRawPrimitives) {
                rpcResult.erase("schematic_components");
            }
        }

        if (summaryOnly) {
            rpcResult.erase("components");
            rpcResult.erase("pads");
            rpcResult.erase("tracks");
            rpcResult.erase("vias");
            rpcResult.erase("pours");
            rpcResult.erase("board_outline");
            rpcResult.erase("schematic_components");
        } else if (!callerRequestedRawPrimitives && !hasFilter) {
            rpcResult.erase("tracks");
            rpcResult.erase("pads");
            rpcResult.erase("vias");
            rpcResult.erase("pours");
            rpcResult.erase("schematic_components");
        }

        if (!arguments.value("include_audit_details", false) &&
            rpcResult.contains("connectivity_audit") && rpcResult["connectivity_audit"].is_object()) {
            auto& ca = rpcResult["connectivity_audit"];
            if (!ca.contains("decoupling_distance_warnings_count") &&
                ca.contains("decoupling_distance_warnings") && ca["decoupling_distance_warnings"].is_array()) {
                ca["decoupling_distance_warnings_count"] = static_cast<int>(ca["decoupling_distance_warnings"].size());
            }
            if (!ca.contains("unassigned_pads_count") &&
                ca.contains("unassigned_pads") && ca["unassigned_pads"].is_array()) {
                ca["unassigned_pads_count"] = static_cast<int>(ca["unassigned_pads"].size());
            }
            ca.erase("unassigned_pads");
            ca.erase("decoupling_distance_warnings");
            ca["audit_details_omitted"] = true;
        }

        if (arguments.is_object()) {
            if (!arguments.value("include_components", true)) rpcResult.erase("components");
            if (!arguments.value("include_pads", true)) rpcResult.erase("pads");
            if (!arguments.value("include_tracks", true)) rpcResult.erase("tracks");
            if (!arguments.value("include_vias", true)) rpcResult.erase("vias");
            if (!arguments.value("include_pours", true)) rpcResult.erase("pours");
            if (!arguments.value("include_audit", true)) rpcResult.erase("connectivity_audit");
        }
    } else if (toolName == "easyeda_edit_pcb" && rpcResult.is_object()) {
        std::string unitsStr = remoteArgs.value("units", "mil");
        rpcResult["units"] = unitsStr;
        auto scaleCoord = [&](double val) -> double {
            if (unitsStr == "mm" || unitsStr == "MM") {
                return std::round(val * 39.37007874 * 1000.0) / 1000.0;
            }
            return val;
        };
        auto mmToMil = [](double mm) -> double {
            return std::round(mm * 39.37007874 * 1000.0) / 1000.0;
        };
        nlohmann::json pcbState = inMemoryPcbEdit ? remoteArgs["pcb_state"] : nlohmann::json::object();

        // 1. delete_primitives
        if (remoteArgs.contains("delete_primitives") && remoteArgs["delete_primitives"].is_array()) {
            bool alreadyDeletedByPlugin = rpcResult.contains("deleted_primitives");
            int delCount = rpcResult.value("deleted_primitives", 0);
            for (const auto& dp : remoteArgs["delete_primitives"]) {
                std::string primId;
                std::string primType;
                if (dp.is_string()) {
                    primId = dp.get<std::string>();
                } else if (dp.is_object()) {
                    primId = dp.value("id", dp.value("primitive_id", ""));
                    primType = dp.value("type", "");
                }
                if (primId.empty()) continue;
                bool foundInPcb = false;
                if (inMemoryPcbEdit && pcbState.is_object()) {
                    for (const char* k : {"tracks", "vias", "pads", "pours"}) {
                        if (pcbState.contains(k) && pcbState[k].is_array()) {
                            auto& arr = pcbState[k];
                            for (auto it = arr.begin(); it != arr.end(); ) {
                                if (it->is_object() && (it->value("id", "") == primId || it->value("primitive_id", "") == primId)) {
                                    it = arr.erase(it);
                                    foundInPcb = true;
                                } else {
                                    ++it;
                                }
                            }
                        }
                    }
                    for (const char* k : {"components", "footprints", "pcb_components"}) {
                        if (pcbState.contains(k) && pcbState[k].is_array()) {
                            auto& arr = pcbState[k];
                            for (auto it = arr.begin(); it != arr.end(); ) {
                                if (it->is_object() && (it->value("id", "") == primId || it->value("reference", "") == primId || it->value("ref", "") == primId)) {
                                    it = arr.erase(it);
                                    foundInPcb = true;
                                } else {
                                    ++it;
                                }
                            }
                        }
                    }
                }
                nlohmann::json apiRes;
                if (!alreadyDeletedByPlugin) {
                    if (inMemoryPcbEdit) {
                        if (foundInPcb) ++delCount;
                    } else {
                        if (primType == "line") {
                            invokeEasyEdaApi("eda.pcb_PrimitiveLine.delete", nlohmann::json::array({primId}), apiRes);
                        } else if (primType == "via") {
                            invokeEasyEdaApi("eda.pcb_PrimitiveVia.delete", nlohmann::json::array({primId}), apiRes);
                        } else if (primType == "component") {
                            invokeEasyEdaApi("eda.pcb_PrimitiveComponent.delete", nlohmann::json::array({primId}), apiRes);
                        } else if (primType == "pad") {
                            invokeEasyEdaApi("eda.pcb_PrimitivePad.delete", nlohmann::json::array({primId}), apiRes);
                        } else if (primType == "pour") {
                            invokeEasyEdaApi("eda.pcb_PrimitivePour.delete", nlohmann::json::array({primId}), apiRes);
                        } else {
                            invokeEasyEdaApi("eda.pcb_PrimitiveTrack.delete", nlohmann::json::array({primId}), apiRes);
                            invokeEasyEdaApi("eda.pcb_PrimitiveLine.delete", nlohmann::json::array({primId}), apiRes);
                            invokeEasyEdaApi("eda.pcb_PrimitivePour.delete", nlohmann::json::array({primId}), apiRes);
                        }
                        ++delCount;
                    }
                } else if (primType == "line" || primType == "pour" || primType.empty()) {
                    invokeEasyEdaApi("eda.pcb_PrimitiveLine.delete", nlohmann::json::array({primId}), apiRes);
                    invokeEasyEdaApi("eda.pcb_PrimitivePour.delete", nlohmann::json::array({primId}), apiRes);
                }
            }
            rpcResult["deleted_primitives"] = delCount;
        }

        // 2. modify_pads / pad_nets
        const nlohmann::json* modPadsArr = nullptr;
        if (remoteArgs.contains("modify_pads") && remoteArgs["modify_pads"].is_array()) {
            modPadsArr = &remoteArgs["modify_pads"];
        } else if (remoteArgs.contains("pad_nets") && remoteArgs["pad_nets"].is_array()) {
            modPadsArr = &remoteArgs["pad_nets"];
        }
        if (modPadsArr != nullptr && jsonInt(rpcResult, "modified_pads", 0) == 0) {
            std::map<std::pair<std::string, std::string>, std::string> padIdByRefNum;
            bool needRefPadLookup = false;
            for (const auto& mp : *modPadsArr) {
                if (!mp.is_object()) continue;
                if (mp.value("id", mp.value("pad_id", "")).empty() &&
                    !mp.value("reference", mp.value("ref", mp.value("designator", ""))).empty()) {
                    needRefPadLookup = true;
                    break;
                }
            }
            if (needRefPadLookup && !inMemoryPcbEdit) {
                nlohmann::json pcbSnap;
                std::string snapErr;
                int snapCode = 0;
                if (dispatchRemoteRpc("easyeda_inspect_pcb", {{"include_raw_primitives", true}},
                                      pcbSnap, snapErr, snapCode) && pcbSnap.is_object()) {
                    sanitizeEasyEdaProject(pcbSnap);
                    if (pcbSnap.contains("pads") && pcbSnap["pads"].is_array()) {
                        for (const auto& p : pcbSnap["pads"]) {
                            if (!p.is_object()) continue;
                            std::string pId = p.value("id", "");
                            std::string pRef = p.value("ref", p.value("reference", ""));
                            std::string pNum = p.value("pad", p.value("number", ""));
                            if (!pId.empty() && !pRef.empty() && !pNum.empty()) {
                                padIdByRefNum[{pRef, pNum}] = pId;
                            }
                        }
                    }
                }
            }
            int modPadsCount = 0;
            for (const auto& mp : *modPadsArr) {
                if (!mp.is_object()) continue;
                std::string padId = mp.value("id", mp.value("pad_id", ""));
                if (padId.empty()) {
                    std::string pRef = mp.value("reference", mp.value("ref", mp.value("designator", "")));
                    std::string pNum = mp.value("pad", mp.value("pad_number", mp.value("number", "")));
                    auto it = padIdByRefNum.find({pRef, pNum});
                    if (it != padIdByRefNum.end()) {
                        padId = it->second;
                    }
                }
                if (padId.empty()) continue;
                nlohmann::json modProps = nlohmann::json::object();
                if (mp.contains("net")) modProps["net"] = mp.value("net", "");
                if (mp.contains("pad_number") || mp.contains("number")) {
                    modProps["padNumber"] = mp.value("pad_number", mp.value("number", ""));
                }
                if (mp.contains("layer")) modProps["layer"] = mp["layer"];
                nlohmann::json apiRes;
                if (invokeEasyEdaApi("eda.pcb_PrimitivePad.modify",
                                     nlohmann::json::array({padId, modProps}),
                                     apiRes)) {
                    ++modPadsCount;
                }
            }
            rpcResult["modified_pads"] = modPadsCount;
        }

        // 3. place_components
        if (remoteArgs.contains("place_components") && remoteArgs["place_components"].is_array() &&
            jsonInt(rpcResult, "placed_components", 0) == 0) {
            int placedCompCount = 0;
            nlohmann::json placedDetails = nlohmann::json::array();
            for (const auto& pc : remoteArgs["place_components"]) {
                if (!pc.is_object()) continue;
                std::string compUuid = pc.value("uuid", pc.value("device_uuid", pc.value("footprint_uuid", "")));
                std::string libUuid = pc.value("library_uuid", pc.value("libraryUuid", ""));
                std::string searchKey = pc.value("lcsc", pc.value("query", ""));
                if ((compUuid.empty() || libUuid.empty()) && !searchKey.empty()) {
                    nlohmann::json searchRes;
                    if (invokeEasyEdaApi("eda.lib_Device.search",
                                         nlohmann::json::array({searchKey}),
                                         searchRes) &&
                        searchRes.contains("result") && searchRes["result"].is_array() &&
                        !searchRes["result"].empty() && searchRes["result"][0].is_object()) {
                        const auto& hit = searchRes["result"][0];
                        if (compUuid.empty()) {
                            compUuid = hit.value("uuid", hit.value("footprintUuid", ""));
                        }
                        if (libUuid.empty()) {
                            libUuid = hit.value("libraryUuid", hit.value("library_uuid", ""));
                        }
                    }
                }
                double xVal = scaleCoord(pc.value("x", pc.value("x_mm", 0.0)));
                double yVal = scaleCoord(pc.value("y", pc.value("y_mm", 0.0)));
                double rotVal = pc.value("rotation", pc.value("rotation_deg", 0.0));
                std::string layerStr = pc.value("layer", "TopLayer");
                int layerId = (layerStr == "BottomLayer" || layerStr == "B.Cu" || layerStr == "2") ? 2 : 1;
                nlohmann::json devObj = libUuid.empty()
                    ? nlohmann::json(compUuid)
                    : nlohmann::json({{"uuid", compUuid}, {"libraryUuid", libUuid}});
                nlohmann::json createRes;
                if (invokeEasyEdaApi("eda.pcb_PrimitiveComponent.create",
                                     nlohmann::json::array({devObj, layerId, xVal, yVal, rotVal, false}),
                                     createRes)) {
                    std::string createdId;
                    nlohmann::json rawPads = nlohmann::json::array();
                    if (createRes.contains("result")) {
                        if (createRes["result"].is_string()) {
                            createdId = createRes["result"].get<std::string>();
                        } else if (createRes["result"].is_object()) {
                            createdId = createRes["result"].value(
                                "primitiveId", createRes["result"].value("id", ""));
                            if (createRes["result"].contains("pads") &&
                                createRes["result"]["pads"].is_array()) {
                                rawPads = createRes["result"]["pads"];
                            }
                        }
                    }
                    std::string ref = pc.value("reference", pc.value("ref", pc.value("designator", "")));
                    std::string uniqueId = pc.value("unique_id", pc.value("uniqueId", ""));
                    std::string valStr = pc.value("value", pc.value("name", ""));
                    if (!createdId.empty() && (!ref.empty() || !uniqueId.empty() || !valStr.empty())) {
                        nlohmann::json modProps = nlohmann::json::object();
                        if (!ref.empty()) modProps["designator"] = ref;
                        if (!uniqueId.empty()) modProps["uniqueId"] = uniqueId;
                        if (!valStr.empty()) modProps["name"] = valStr;
                        nlohmann::json modRes;
                        invokeEasyEdaApi("eda.pcb_PrimitiveComponent.modify",
                                         nlohmann::json::array({createdId, modProps}),
                                         modRes);
                    }
                    nlohmann::json padDetails = nlohmann::json::array();
                    if (!createdId.empty() && !rawPads.empty()) {
                        const nlohmann::json padNetsObj =
                            (pc.contains("pad_nets") && pc["pad_nets"].is_object())
                                ? pc["pad_nets"]
                                : nlohmann::json::object();
                        for (const auto& rp : rawPads) {
                            if (!rp.is_object()) continue;
                            std::string pNum = rp.value("padNumber", rp.value("number", ""));
                            std::string pSubId = rp.value("primitiveId", rp.value("id", ""));
                            if (pSubId.empty()) continue;
                            std::string fullPadId = (pSubId.rfind(createdId, 0) == 0)
                                ? pSubId
                                : (createdId + pSubId);
                            std::string netToSet = padNetsObj.contains(pNum) && padNetsObj[pNum].is_string()
                                ? padNetsObj[pNum].get<std::string>()
                                : rp.value("net", "");
                            nlohmann::json padEntry = {
                                {"id", fullPadId},
                                {"pad", pNum},
                                {"net", netToSet}
                            };
                            if (padNetsObj.contains(pNum) && padNetsObj[pNum].is_string()) {
                                nlohmann::json padModRes;
                                if (invokeEasyEdaApi("eda.pcb_PrimitivePad.modify",
                                                     nlohmann::json::array({fullPadId, {{"net", netToSet}}}),
                                                     padModRes) &&
                                    padModRes.contains("result") && padModRes["result"].is_object()) {
                                    padEntry["x"] = padModRes["result"].value("x", 0.0);
                                    padEntry["y"] = padModRes["result"].value("y", 0.0);
                                }
                            }
                            padDetails.push_back(std::move(padEntry));
                        }
                    }
                    placedDetails.push_back({
                        {"id", createdId},
                        {"reference", ref},
                        {"unique_id", uniqueId},
                        {"x", xVal},
                        {"y", yVal},
                        {"rotation", rotVal},
                        {"pads", std::move(padDetails)}
                    });
                    ++placedCompCount;
                }
            }
            rpcResult["placed_components"] = placedCompCount;
            if (!placedDetails.empty()) {
                rpcResult["placed_component_details"] = std::move(placedDetails);
            }
        }

        // 4. move_components (including preserve_connections track rubber-banding)
        if (remoteArgs.contains("move_components") && remoteArgs["move_components"].is_array()) {
            int movedCompCount = rpcResult.value("moved_components", rpcResult.value("updated_placements", 0));
            bool needRemoteMoveComp = !rpcResult.contains("moved_components") &&
                                      !(rpcResult.value("updated_placements", 0) > 0 && !inMemoryPcbEdit);
            nlohmann::json rubberBandedTracks = rpcResult.value("rubber_banded_tracks", nlohmann::json::array());
            bool topPreserveConn = remoteArgs.value("preserve_connections", false);
            bool needRubberBandInspect = !inMemoryPcbEdit && topPreserveConn &&
                                         !rpcResult.contains("rubber_banded_tracks");

            if (needRubberBandInspect) {
                std::string inspErr;
                int inspCode = 0;
                dispatchRemoteRpc("easyeda_inspect_pcb", {{"include_raw_primitives", true}}, pcbState, inspErr, inspCode);
            }
            if (!pcbState.is_object()) pcbState = nlohmann::json::object();

            for (const auto& mc : remoteArgs["move_components"]) {
                if (!mc.is_object()) continue;
                std::string compId = mc.value("id", mc.value("primitive_id", ""));
                std::string targetRef = mc.value("reference", mc.value("ref", mc.value("designator", "")));

                nlohmann::json* matchedComp = nullptr;
                for (const char* arrKey : {"components", "footprints", "pcb_components"}) {
                    if (!pcbState.contains(arrKey) || !pcbState[arrKey].is_array()) continue;
                    for (auto& existingComp : pcbState[arrKey]) {
                        if (!existingComp.is_object()) continue;
                        std::string eId = existingComp.value("id", "");
                        std::string eRef = existingComp.value("reference", existingComp.value("ref", ""));
                        if ((!compId.empty() && eId == compId) || (!targetRef.empty() && eRef == targetRef)) {
                            matchedComp = &existingComp;
                            if (compId.empty()) compId = eId;
                            break;
                        }
                    }
                    if (matchedComp != nullptr) break;
                }

                double hasMm = (mc.contains("x_mm") || mc.contains("y_mm")) ? 1.0 : 0.0;
                double rawNewX = hasMm > 0.0 ? mc.value("x_mm", 0.0) : mc.value("x", 0.0);
                double rawNewY = hasMm > 0.0 ? mc.value("y_mm", 0.0) : mc.value("y", 0.0);

                if (needRemoteMoveComp && !compId.empty()) {
                    nlohmann::json modProps = {
                        {"x", hasMm > 0.0 ? std::round(rawNewX * 39.37007874 * 1000.0) / 1000.0 : scaleCoord(rawNewX)},
                        {"y", hasMm > 0.0 ? std::round(rawNewY * 39.37007874 * 1000.0) / 1000.0 : scaleCoord(rawNewY)}
                    };
                    if (mc.contains("rotation") || mc.contains("rotation_deg")) {
                        modProps["rotation"] = mc.value("rotation", mc.value("rotation_deg", 0.0));
                    }
                    nlohmann::json apiRes;
                    if (invokeEasyEdaApi("eda.pcb_PrimitiveComponent.modify",
                                         nlohmann::json::array({compId, modProps}),
                                         apiRes)) {
                        ++movedCompCount;
                    }
                } else if (needRemoteMoveComp && matchedComp != nullptr) {
                    ++movedCompCount;
                }

                if (matchedComp != nullptr) {
                    bool compInMm = matchedComp->contains("x_mm") || matchedComp->contains("y_mm") || hasMm > 0.0;
                    double oldX = compInMm ? matchedComp->value("x_mm", matchedComp->value("x", 0.0))
                                           : matchedComp->value("x", 0.0);
                    double oldY = compInMm ? matchedComp->value("y_mm", matchedComp->value("y", 0.0))
                                           : matchedComp->value("y", 0.0);
                    double newX = rawNewX;
                    double newY = rawNewY;
                    double dx = newX - oldX;
                    double dy = newY - oldY;

                    if (matchedComp->contains("x_mm")) (*matchedComp)["x_mm"] = newX;
                    else (*matchedComp)["x"] = newX;
                    if (matchedComp->contains("y_mm")) (*matchedComp)["y_mm"] = newY;
                    else (*matchedComp)["y"] = newY;

                    std::vector<std::pair<double, double>> oldPadCoords;
                    if (matchedComp->contains("pads") && (*matchedComp)["pads"].is_array()) {
                        for (auto& pad : (*matchedComp)["pads"]) {
                            if (!pad.is_object()) continue;
                            double px = pad.contains("x_mm") ? pad.value("x_mm", oldX) : pad.value("x", oldX);
                            double py = pad.contains("y_mm") ? pad.value("y_mm", oldY) : pad.value("y", oldY);
                            oldPadCoords.push_back({px, py});
                            if (pad.contains("x_mm")) pad["x_mm"] = px + dx;
                            else pad["x"] = px + dx;
                            if (pad.contains("y_mm")) pad["y_mm"] = py + dy;
                            else pad["y"] = py + dy;
                        }
                    }
                    if (pcbState.contains("pads") && pcbState["pads"].is_array() && !targetRef.empty()) {
                        for (auto& pad : pcbState["pads"]) {
                            if (!pad.is_object()) continue;
                            if (pad.value("ref", pad.value("reference", "")) != targetRef) continue;
                            double px = pad.contains("x_mm") ? pad.value("x_mm", oldX) : pad.value("x", oldX);
                            double py = pad.contains("y_mm") ? pad.value("y_mm", oldY) : pad.value("y", oldY);
                            oldPadCoords.push_back({px, py});
                            if (pad.contains("x_mm")) pad["x_mm"] = px + dx;
                            else pad["x"] = px + dx;
                            if (pad.contains("y_mm")) pad["y_mm"] = py + dy;
                            else pad["y"] = py + dy;
                        }
                    }

                    bool preserveConn = mc.value("preserve_connections", topPreserveConn);
                    if (preserveConn &&
                        !oldPadCoords.empty() && pcbState.contains("tracks") && pcbState["tracks"].is_array()) {
                        const double kPadTouchTol = compInMm ? 0.35 : 15.0;
                        for (auto& trk : pcbState["tracks"]) {
                            if (!trk.is_object()) continue;
                            bool useMmKeys = trk.contains("x1_mm");
                            double x1 = useMmKeys ? trk.value("x1_mm", 0.0) : trk.value("x1", 0.0);
                            double y1 = useMmKeys ? trk.value("y1_mm", 0.0) : trk.value("y1", 0.0);
                            double x2 = useMmKeys ? trk.value("x2_mm", 0.0) : trk.value("x2", 0.0);
                            double y2 = useMmKeys ? trk.value("y2_mm", 0.0) : trk.value("y2", 0.0);

                            bool moved1 = false, moved2 = false;
                            for (const auto& op : oldPadCoords) {
                                if (!moved1 && std::hypot(x1 - op.first, y1 - op.second) <= kPadTouchTol) {
                                    x1 = op.first + dx;
                                    y1 = op.second + dy;
                                    moved1 = true;
                                }
                                if (!moved2 && std::hypot(x2 - op.first, y2 - op.second) <= kPadTouchTol) {
                                    x2 = op.first + dx;
                                    y2 = op.second + dy;
                                    moved2 = true;
                                }
                            }

                            if (moved1 || moved2) {
                                if (useMmKeys) {
                                    trk["x1_mm"] = x1;
                                    trk["y1_mm"] = y1;
                                    trk["x2_mm"] = x2;
                                    trk["y2_mm"] = y2;
                                } else {
                                    trk["x1"] = x1;
                                    trk["y1"] = y1;
                                    trk["x2"] = x2;
                                    trk["y2"] = y2;
                                }
                                rubberBandedTracks.push_back({
                                    {"id", trk.value("id", "")},
                                    {"net", trk.value("net", "")},
                                    {"layer", trk.value("layer", "F.Cu")},
                                    {"x1", x1},
                                    {"y1", y1},
                                    {"x2", x2},
                                    {"y2", y2}
                                });
                            }
                        }
                    }
                }
            }
            rpcResult["moved_components"] = movedCompCount;
            if (topPreserveConn || !rubberBandedTracks.empty()) {
                rpcResult["rubber_banded_tracks"] = rubberBandedTracks;
                rpcResult["rubber_banded_tracks_count"] = static_cast<int>(rubberBandedTracks.size());
            }
        }

        // 5. modify_tracks
        if (remoteArgs.contains("modify_tracks") && remoteArgs["modify_tracks"].is_array() &&
            rpcResult.value("modified_tracks", 0) == 0) {
            auto layerMatchesTrack = [](const std::string& trkLayer, const std::string& queryLayer) -> bool {
                if (queryLayer.empty()) return true;
                if (trkLayer == queryLayer) return true;
                if ((queryLayer == "TopLayer" || queryLayer == "F.Cu" || queryLayer == "1") &&
                    (trkLayer == "TopLayer" || trkLayer == "F.Cu" || trkLayer == "1")) return true;
                if ((queryLayer == "BottomLayer" || queryLayer == "B.Cu" || queryLayer == "2") &&
                    (trkLayer == "BottomLayer" || trkLayer == "B.Cu" || trkLayer == "2")) return true;
                if ((queryLayer == "InnerLayer1" || queryLayer == "In1.Cu" || queryLayer == "15") &&
                    (trkLayer == "InnerLayer1" || trkLayer == "In1.Cu" || trkLayer == "15")) return true;
                if ((queryLayer == "InnerLayer2" || queryLayer == "In2.Cu" || queryLayer == "16") &&
                    (trkLayer == "InnerLayer2" || trkLayer == "In2.Cu" || trkLayer == "16")) return true;
                return false;
            };

            int modTrkCount = 0;
            nlohmann::json modTrkIds = nlohmann::json::array();
            for (const auto& mt : remoteArgs["modify_tracks"]) {
                if (!mt.is_object()) continue;
                std::string targetId = mt.value("id", mt.value("track_id", mt.value("primitive_id", "")));
                std::string matchNet = mt.value("net", "");
                std::string matchLayer = mt.value("layer", "");

                double targetWidthMil = -1.0;
                double targetWidthMm = -1.0;
                if (mt.contains("to_width_mm") && mt["to_width_mm"].is_number()) {
                    targetWidthMm = mt["to_width_mm"].get<double>();
                    targetWidthMil = mmToMil(targetWidthMm);
                } else if (mt.contains("width_mm") && mt["width_mm"].is_number()) {
                    targetWidthMm = mt["width_mm"].get<double>();
                    targetWidthMil = mmToMil(targetWidthMm);
                } else if (mt.contains("to_width") && mt["to_width"].is_number()) {
                    targetWidthMil = mt["to_width"].get<double>();
                    targetWidthMm = std::round(targetWidthMil / 39.37007874 * 1000.0) / 1000.0;
                } else if (mt.contains("width") && mt["width"].is_number()) {
                    targetWidthMil = (unitsStr == "mm" || unitsStr == "MM")
                        ? mmToMil(mt["width"].get<double>())
                        : mt["width"].get<double>();
                    targetWidthMm = std::round(targetWidthMil / 39.37007874 * 1000.0) / 1000.0;
                }

                if (pcbState.contains("tracks") && pcbState["tracks"].is_array()) {
                    for (auto& trk : pcbState["tracks"]) {
                        if (!trk.is_object()) continue;
                        std::string tId = trk.value("id", trk.value("track_id", trk.value("primitive_id", "")));
                        std::string tNet = trk.value("net", "");
                        std::string tLayer = trk.value("layer", "");

                        bool matched = false;
                        if (!targetId.empty() && tId == targetId) {
                            matched = true;
                        } else if (targetId.empty() && !matchNet.empty() && tNet == matchNet) {
                            if (layerMatchesTrack(tLayer, matchLayer)) {
                                matched = true;
                            }
                        }

                        if (matched) {
                            if (targetWidthMm > 0.0) {
                                trk["width_mm"] = targetWidthMm;
                                trk["width"] = targetWidthMil;
                            } else if (targetWidthMil > 0.0) {
                                trk["width"] = targetWidthMil;
                                trk["width_mm"] = targetWidthMm;
                            }
                            if (mt.contains("to_net") && mt["to_net"].is_string()) {
                                trk["net"] = mt["to_net"].get<std::string>();
                            }
                            if (mt.contains("to_layer") && mt["to_layer"].is_string()) {
                                trk["layer"] = mt["to_layer"].get<std::string>();
                            }
                            if (!inMemoryPcbEdit && !tId.empty()) {
                                nlohmann::json patch = nlohmann::json::object();
                                if (targetWidthMil > 0.0) patch["lineWidth"] = targetWidthMil;
                                if (mt.contains("to_net")) patch["net"] = mt["to_net"];
                                if (mt.contains("to_layer")) patch["layer"] = mt["to_layer"];
                                nlohmann::json apiRes;
                                invokeEasyEdaApi("eda.pcb_PrimitiveTrack.modify",
                                                 nlohmann::json::array({tId, patch}), apiRes);
                            }
                            ++modTrkCount;
                            if (!tId.empty() && std::find(modTrkIds.begin(), modTrkIds.end(), tId) == modTrkIds.end()) {
                                modTrkIds.push_back(tId);
                            }
                        }
                    }
                }
            }
            rpcResult["modified_tracks"] = modTrkCount;
            rpcResult["modified_track_ids"] = std::move(modTrkIds);
        }

        // 6. modify_components and resolve_value_templates
        bool doResolveTemplates = remoteArgs.value("resolve_value_templates", false);
        bool hasModComps = remoteArgs.contains("modify_components") && remoteArgs["modify_components"].is_array();
        if ((hasModComps || doResolveTemplates) && rpcResult.value("modified_components", 0) == 0) {
            int modCompCount = 0;
            nlohmann::json modCompDetails = nlohmann::json::array();

            std::map<std::string, std::string> schCompValues;
            const nlohmann::json* schCompsArr = nullptr;
            if (remoteArgs.contains("schematic_state") && remoteArgs["schematic_state"].is_object() &&
                remoteArgs["schematic_state"].contains("components") && remoteArgs["schematic_state"]["components"].is_array()) {
                schCompsArr = &remoteArgs["schematic_state"]["components"];
            }
            if (schCompsArr != nullptr) {
                for (const auto& sc : *schCompsArr) {
                    if (!sc.is_object()) continue;
                    std::string sRef = sc.value("reference", sc.value("ref", sc.value("designator", "")));
                    std::string sVal = sc.value("value", sc.value("name", ""));
                    if (!sRef.empty() && !sVal.empty() && sVal.rfind("={", 0) != 0) {
                        schCompValues[sRef] = sVal;
                    }
                }
            }

            auto resolveTemplate = [&](const std::string& rawVal, const nlohmann::json& otherProp, const std::string& ref) -> std::string {
                if (rawVal.size() > 3 && rawVal.rfind("={", 0) == 0 && rawVal.back() == '}') {
                    std::string key = rawVal.substr(2, rawVal.size() - 3);
                    std::string keyLower = key;
                    std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), [](unsigned char c) { return std::tolower(c); });

                    if (otherProp.is_object()) {
                        if (otherProp.contains(key) && otherProp[key].is_string()) {
                            std::string v = otherProp[key].get<std::string>();
                            if (!v.empty() && v.rfind("={", 0) != 0) return v;
                        }
                        for (auto it = otherProp.begin(); it != otherProp.end(); ++it) {
                            std::string kLow = it.key();
                            std::transform(kLow.begin(), kLow.end(), kLow.begin(), [](unsigned char c) { return std::tolower(c); });
                            if (kLow == keyLower && it.value().is_string()) {
                                std::string v = it.value().get<std::string>();
                                if (!v.empty() && v.rfind("={", 0) != 0) return v;
                            }
                        }
                        static const char* const kFallbackKeys[] = {
                            "Value", "value", "Manufacturer Part", "ManufacturerPart", "Part", "Comment", "DeviceName"
                        };
                        for (const char* fk : kFallbackKeys) {
                            if (otherProp.contains(fk) && otherProp[fk].is_string()) {
                                std::string v = otherProp[fk].get<std::string>();
                                if (!v.empty() && v.rfind("={", 0) != 0) return v;
                            }
                        }
                    }
                    if (!ref.empty()) {
                        auto sIt = schCompValues.find(ref);
                        if (sIt != schCompValues.end() && !sIt->second.empty() && sIt->second.rfind("={", 0) != 0) {
                            return sIt->second;
                        }
                    }
                }
                return rawVal;
            };

            if (hasModComps) {
                for (const auto& mc : remoteArgs["modify_components"]) {
                    if (!mc.is_object()) continue;
                    std::string targetId = mc.value("id", mc.value("primitive_id", ""));
                    std::string targetRef = mc.value("reference", mc.value("ref", mc.value("designator", "")));

                    for (const char* arrKey : {"components", "footprints", "pcb_components"}) {
                        if (!pcbState.contains(arrKey) || !pcbState[arrKey].is_array()) continue;
                        for (auto& comp : pcbState[arrKey]) {
                            if (!comp.is_object()) continue;
                            std::string cId = comp.value("id", comp.value("primitive_id", ""));
                            std::string cRef = comp.value("reference", comp.value("ref", comp.value("designator", "")));
                            bool matched = false;
                            if (!targetId.empty() && cId == targetId) matched = true;
                            else if (!targetRef.empty() && cRef == targetRef) matched = true;

                            if (matched) {
                                std::string compPrimId = !cId.empty() ? cId : targetId;
                                nlohmann::json modProps = nlohmann::json::object();
                                if (mc.contains("value") || mc.contains("name")) {
                                    std::string valStr = mc.value("value", mc.value("name", ""));
                                    comp["value"] = valStr;
                                    comp["name"] = valStr;
                                    modProps["name"] = valStr;
                                }
                                if (mc.contains("designator") || mc.contains("reference")) {
                                    std::string desStr = mc.value("designator", mc.value("reference", ""));
                                    comp["designator"] = desStr;
                                    comp["reference"] = desStr;
                                    comp["ref"] = desStr;
                                    modProps["designator"] = desStr;
                                }
                                if (mc.contains("footprint")) {
                                    comp["footprint"] = mc["footprint"];
                                }
                                if (!inMemoryPcbEdit && !compPrimId.empty() && !modProps.empty()) {
                                    nlohmann::json apiRes;
                                    invokeEasyEdaApi("eda.pcb_PrimitiveComponent.modify",
                                                     nlohmann::json::array({compPrimId, modProps}), apiRes);
                                }
                                ++modCompCount;
                                modCompDetails.push_back({{"id", compPrimId}, {"reference", cRef}});
                            }
                        }
                    }
                }
            }

            if (doResolveTemplates) {
                for (const char* arrKey : {"components", "footprints", "pcb_components"}) {
                    if (!pcbState.contains(arrKey) || !pcbState[arrKey].is_array()) continue;
                    for (auto& comp : pcbState[arrKey]) {
                        if (!comp.is_object()) continue;
                        std::string curVal = comp.value("value", comp.value("name", ""));
                        if (curVal.rfind("={", 0) == 0) {
                            std::string cId = comp.value("id", comp.value("primitive_id", ""));
                            std::string cRef = comp.value("reference", comp.value("ref", comp.value("designator", "")));
                            nlohmann::json otherProp = comp.value("otherProperty", nlohmann::json::object());
                            std::string resolved = resolveTemplate(curVal, otherProp, cRef);
                            if (!resolved.empty() && resolved.rfind("={", 0) != 0) {
                                comp["value"] = resolved;
                                comp["name"] = resolved;
                                if (!inMemoryPcbEdit && !cId.empty()) {
                                    nlohmann::json apiRes;
                                    invokeEasyEdaApi("eda.pcb_PrimitiveComponent.modify",
                                                     nlohmann::json::array({cId, {{"name", resolved}}}), apiRes);
                                }
                                ++modCompCount;
                                modCompDetails.push_back({{"id", cId}, {"reference", cRef}, {"value", resolved}});
                            }
                        }
                    }
                }
            }

            rpcResult["modified_components"] = modCompCount;
            if (!modCompDetails.empty()) {
                rpcResult["modified_component_details"] = std::move(modCompDetails);
            }
        }

        // 7. tracks addition to pcbState (in-memory mode or augmenting rpcResult)
        if (remoteArgs.contains("tracks") && remoteArgs["tracks"].is_array()) {
            if (!pcbState.contains("tracks") || !pcbState["tracks"].is_array()) {
                pcbState["tracks"] = nlohmann::json::array();
            }
            int addedTrkCount = rpcResult.value("added_tracks", 0);
            nlohmann::json createdTrkIds = rpcResult.value("created_track_ids", nlohmann::json::array());
            int trkIdx = 1;
            for (const auto& trk : remoteArgs["tracks"]) {
                if (!trk.is_object()) continue;
                std::string tId = trk.value("id", "trk_added_" + std::to_string(trkIdx++));
                nlohmann::json trkObj = trk;
                trkObj["id"] = tId;

                int layerId = 1;
                std::string layerName = "TopLayer";
                if (trk.contains("layerId") && trk["layerId"].is_number_integer()) {
                    layerId = trk["layerId"].get<int>();
                } else if (trk.contains("layer")) {
                    if (trk["layer"].is_number_integer()) {
                        layerId = trk["layer"].get<int>();
                    } else if (trk["layer"].is_string()) {
                        std::string lStr = trk["layer"].get<std::string>();
                        if (lStr == "2" || lStr == "BottomLayer" || lStr == "B.Cu") layerId = 2;
                        else if (lStr == "15" || lStr == "InnerLayer1" || lStr == "In1.Cu") layerId = 15;
                        else if (lStr == "16" || lStr == "InnerLayer2" || lStr == "In2.Cu") layerId = 16;
                        else layerId = 1;
                    }
                }
                if (layerId == 2) layerName = "BottomLayer";
                else if (layerId == 15) layerName = "InnerLayer1";
                else if (layerId == 16) layerName = "InnerLayer2";
                else layerName = "TopLayer";

                trkObj["layerId"] = layerId;
                trkObj["layer"] = layerName;

                if (inMemoryPcbEdit) {
                    pcbState["tracks"].push_back(trkObj);
                }
                createdTrkIds.push_back(tId);
                if (!rpcResult.contains("added_tracks")) {
                    ++addedTrkCount;
                }
            }
            if (!rpcResult.contains("added_tracks")) {
                rpcResult["added_tracks"] = addedTrkCount;
            }
            if (!rpcResult.contains("created_track_ids") || rpcResult["created_track_ids"].empty()) {
                rpcResult["created_track_ids"] = createdTrkIds;
            }
        }

        if (inMemoryPcbEdit) {
            rpcResult["pcb_state"] = pcbState;
        }
    } else if (toolName == "easyeda_sync_sch_to_pcb" &&
               (remoteArgs.value("dry_run", false) || !rpcResult.value("synced", false))) {
        if (!rpcResult.contains("components_to_add")) {
            bool isDryRun = remoteArgs.value("dry_run", false);
            nlohmann::json prevUniqueIdMismatches = (rpcResult.is_object() && rpcResult.contains("unique_id_mismatches"))
                ? rpcResult["unique_id_mismatches"] : nlohmann::json::array();
            nlohmann::json prevSubcircuitFatalErrors = (rpcResult.is_object() && rpcResult.contains("subcircuit_fatal_errors"))
                ? rpcResult["subcircuit_fatal_errors"] : nlohmann::json::array();
            nlohmann::json prevUnnamedWireDeltas = (rpcResult.is_object() && rpcResult.contains("unnamed_wire_deltas"))
                ? rpcResult["unnamed_wire_deltas"] : nlohmann::json::array();
            int prevFixedSubcircuitBlocks = rpcResult.is_object() ? rpcResult.value("fixed_subcircuit_blocks", 0) : 0;
            int prevSyncedUniqueIds = rpcResult.is_object() ? rpcResult.value("synced_unique_ids", 0) : 0;
            int prevReconciledWires = rpcResult.is_object() ? rpcResult.value("reconciled_wires", 0) : 0;
            bool prevPushedSynthesizedNetlist = rpcResult.is_object() ? rpcResult.value("pushed_synthesized_netlist", false) : false;

            nlohmann::json schState = (remoteArgs.contains("schematic_state") && remoteArgs["schematic_state"].is_object())
                ? remoteArgs["schematic_state"]
                : nlohmann::json::object();
            nlohmann::json pcbState = (remoteArgs.contains("pcb_state") && remoteArgs["pcb_state"].is_object())
                ? remoteArgs["pcb_state"]
                : nlohmann::json::object();

            EdaAccelerators accCopy;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                accCopy = _accelerators;
            }

            std::map<std::pair<std::string, std::string>, std::string> inferredPinNets;
            std::map<std::string, std::vector<std::pair<std::string, std::string>>> inferredNetMembers;
            std::set<std::string> namedSchNets;
            std::set<std::string> validSchNets;
            auto collectSheetNamedNets = [&](const nlohmann::json& sheetState) {
                if (sheetState.contains("wires") && sheetState["wires"].is_array()) {
                    for (const auto& w : sheetState["wires"]) {
                        if (w.is_object() && !w.value("net", "").empty()) {
                            namedSchNets.insert(w.value("net", ""));
                            validSchNets.insert(w.value("net", ""));
                        }
                    }
                }
                if (sheetState.contains("nets") && sheetState["nets"].is_array()) {
                    for (const auto& n : sheetState["nets"]) {
                        if (n.is_object() && !n.value("name", "").empty()) {
                            namedSchNets.insert(n.value("name", ""));
                            validSchNets.insert(n.value("name", ""));
                        }
                    }
                }
            };
            auto extractInferredNets = [&](const nlohmann::json& sheetState, const std::string& sheetKey) {
                collectSheetNamedNets(sheetState);
                bool ercOk = false;
                std::string ercSummary;
                nlohmann::json erc = accCopy.calcSchematicErcAudit(sheetState, ercOk, ercSummary);
                if (!erc.is_object() || !erc.contains("inferred_nets")) return;
                auto recordNetPins = [&](std::string netName, const nlohmann::json& pinsArr) {
                    if (netName.empty() || !pinsArr.is_array()) return;
                    if (netName.rfind("$NET_", 0) == 0) {
                        if (!sheetKey.empty()) {
                            netName = "$NET_" + sheetKey + "_" + netName.substr(5);
                        }
                    } else {
                        namedSchNets.insert(netName);
                        validSchNets.insert(netName);
                    }
                    for (const auto& pinStrJ : pinsArr) {
                        if (!pinStrJ.is_string()) continue;
                        std::string pinStr = pinStrJ.get<std::string>();
                        auto dotPos = pinStr.find('.');
                        if (dotPos == std::string::npos) continue;
                        std::string cRef = pinStr.substr(0, dotPos);
                        std::string pNum = pinStr.substr(dotPos + 1);
                        inferredPinNets[{cRef, pNum}] = netName;
                        inferredNetMembers[netName].push_back({cRef, pNum});
                    }
                };
                if (erc["inferred_nets"].is_array()) {
                    for (const auto& entry : erc["inferred_nets"]) {
                        if (!entry.is_object()) continue;
                        recordNetPins(entry.value("name", ""),
                                      entry.contains("pins") ? entry["pins"] : nlohmann::json::array());
                    }
                } else if (erc["inferred_nets"].is_object()) {
                    for (auto it = erc["inferred_nets"].begin(); it != erc["inferred_nets"].end(); ++it) {
                        recordNetPins(it.key(), it.value());
                    }
                }
            };

            if (schState.empty() && (_connected || gw != nullptr)) {
                std::string subErr;
                int subCode = 0;
                dispatchRemoteRpc("easyeda_inspect_schematic", nlohmann::json::object(), schState, subErr, subCode);
                std::vector<std::string> pageUuids;
                if (schState.is_object() && schState.contains("project") && schState["project"].is_object() &&
                    schState["project"].contains("data") && schState["project"]["data"].is_array() &&
                    !schState["project"]["data"].empty() &&
                    schState["project"]["data"][0].contains("schematic") &&
                    schState["project"]["data"][0]["schematic"].contains("page") &&
                    schState["project"]["data"][0]["schematic"]["page"].is_array()) {
                    for (const auto& pg : schState["project"]["data"][0]["schematic"]["page"]) {
                        if (pg.is_object() && !pg.value("uuid", "").empty()) {
                            pageUuids.push_back(pg.value("uuid", ""));
                        }
                    }
                }
                if (!pageUuids.empty()) {
                    nlohmann::json mergedComponents = nlohmann::json::array();
                    std::map<std::string, size_t> refToMergedIdx;
                    for (const auto& pgUuid : pageUuids) {
                        nlohmann::json openRes;
                        invokeEasyEdaApi("eda.dmt_EditorControl.openDocument",
                                         nlohmann::json::array({pgUuid}), openRes);
                        nlohmann::json pageState;
                        if (dispatchRemoteRpc("easyeda_inspect_schematic", nlohmann::json::object(),
                                              pageState, subErr, subCode) && pageState.is_object()) {
                            mergeCachedPinStatesAndCacheSheet(pageState);
                            extractInferredNets(pageState, pgUuid);
                            if (pageState.contains("components") && pageState["components"].is_array()) {
                                for (const auto& sc : pageState["components"]) {
                                    if (!sc.is_object()) continue;
                                    if (sc.contains("on_active_sheet") && sc["on_active_sheet"].is_boolean() &&
                                        !sc["on_active_sheet"].get<bool>()) {
                                        continue;
                                    }
                                    std::string r = sc.value("reference", sc.value("ref", sc.value("designator", "")));
                                    std::string ct = sc.value("component_type", sc.value("type", ""));
                                    if (!r.empty() && ct == "part") {
                                        auto exIt = refToMergedIdx.find(r);
                                        if (exIt != refToMergedIdx.end()) {
                                            bool existingEmptyPins = !mergedComponents[exIt->second].contains("pins") ||
                                                !mergedComponents[exIt->second]["pins"].is_array() ||
                                                mergedComponents[exIt->second]["pins"].empty();
                                            bool newHasPins = sc.contains("pins") && sc["pins"].is_array() && !sc["pins"].empty();
                                            if (existingEmptyPins && newHasPins) {
                                                mergedComponents[exIt->second] = sc;
                                            }
                                            continue;
                                        }
                                        refToMergedIdx[r] = mergedComponents.size();
                                    }
                                    mergedComponents.push_back(sc);
                                }
                            }
                            if (schState.empty() || !schState.contains("project")) {
                                schState = pageState;
                            }
                        }
                    }
                    schState["components"] = std::move(mergedComponents);
                } else {
                    mergeCachedPinStatesAndCacheSheet(schState);
                    extractInferredNets(schState, "sheet0");
                }
            } else if (!schState.empty()) {
                extractInferredNets(schState, "sheet0");
            }

            if (pcbState.empty() && (_connected || gw != nullptr)) {
                std::string subErr;
                int subCode = 0;
                if (schState.is_object() && schState.contains("project") && schState["project"].is_object() &&
                    schState["project"].contains("data") && schState["project"]["data"].is_array() &&
                    !schState["project"]["data"].empty() &&
                    schState["project"]["data"][0].contains("pcb") &&
                    schState["project"]["data"][0]["pcb"].is_object()) {
                    std::string pcbUuid = schState["project"]["data"][0]["pcb"].value("uuid", "");
                    if (!pcbUuid.empty()) {
                        nlohmann::json openRes;
                        invokeEasyEdaApi("eda.dmt_EditorControl.openDocument",
                                         nlohmann::json::array({pcbUuid}), openRes);
                    }
                }
                dispatchRemoteRpc("easyeda_inspect_pcb", {{"include_raw_primitives", true}}, pcbState, subErr, subCode);
                sanitizeEasyEdaProject(pcbState);
            }

            nlohmann::json componentsToAdd = nlohmann::json::array();
            nlohmann::json componentsToRemove = nlohmann::json::array();
            nlohmann::json footprintMismatches = nlohmann::json::array();
            nlohmann::json unannotatedReferences = nlohmann::json::array();
            nlohmann::json missingFootprints = nlohmann::json::array();
            nlohmann::json padNetDeltas = nlohmann::json::array();
            nlohmann::json orphanedTracks = nlohmann::json::array();
            nlohmann::json uniqueIdMismatches = prevUniqueIdMismatches;
            nlohmann::json subcircuitFatalErrors = prevSubcircuitFatalErrors;
            nlohmann::json unnamedWireDeltas = prevUnnamedWireDeltas;

            struct PcbCompEntry {
                std::string id;
                std::string uniqueId;
                std::string value;
                std::string footprint;
                std::map<std::string, std::string> padNets;
                std::map<std::string, std::string> padIds;
                std::map<std::string, std::pair<double, double>> padCoords;
                std::map<std::string, bool> padIsMm;
            };
            std::map<std::string, PcbCompEntry> pcbComps;
            for (const char* arrKey : {"components", "footprints", "pcb_components"}) {
                if (!pcbState.contains(arrKey) || !pcbState[arrKey].is_array()) continue;
                for (const auto& pc : pcbState[arrKey]) {
                    if (!pc.is_object()) continue;
                    std::string ref = pc.value("reference", pc.value("ref", pc.value("designator", "")));
                    if (ref.empty()) continue;
                    PcbCompEntry& entry = pcbComps[ref];
                    entry.id = pc.value("id", entry.id);
                    entry.uniqueId = pc.value("unique_id", pc.value("uniqueId", entry.uniqueId));
                    entry.value = pc.value("value", entry.value);
                    entry.footprint = pc.value("footprint", entry.footprint);
                    if (pc.contains("pads") && pc["pads"].is_array()) {
                        for (const auto& pad : pc["pads"]) {
                            if (!pad.is_object()) continue;
                            std::string pNum = pad.value("number", pad.value("pad", ""));
                            if (!pNum.empty()) {
                                entry.padNets[pNum] = pad.value("net", "");
                                if (!pad.value("id", "").empty()) {
                                    entry.padIds[pNum] = pad.value("id", "");
                                }
                                if (pad.contains("x") || pad.contains("x_mm")) {
                                    bool isMm = pad.contains("x_mm");
                                    entry.padCoords[pNum] = {
                                        isMm ? pad.value("x_mm", 0.0) : pad.value("x", 0.0),
                                        isMm ? pad.value("y_mm", 0.0) : pad.value("y", 0.0)
                                    };
                                    entry.padIsMm[pNum] = isMm;
                                }
                            }
                        }
                    }
                }
            }
            if (pcbState.contains("pads") && pcbState["pads"].is_array()) {
                for (const auto& pad : pcbState["pads"]) {
                    if (!pad.is_object()) continue;
                    std::string ref = pad.value("ref", pad.value("reference", ""));
                    std::string pNum = pad.value("pad", pad.value("number", ""));
                    if (!ref.empty() && !pNum.empty()) {
                        pcbComps[ref].padNets[pNum] = pad.value("net", "");
                        if (!pad.value("id", "").empty()) {
                            pcbComps[ref].padIds[pNum] = pad.value("id", "");
                        }
                        if (pad.contains("x") || pad.contains("x_mm")) {
                            bool isMm = pad.contains("x_mm");
                            pcbComps[ref].padCoords[pNum] = {
                                isMm ? pad.value("x_mm", 0.0) : pad.value("x", 0.0),
                                isMm ? pad.value("y_mm", 0.0) : pad.value("y", 0.0)
                            };
                            pcbComps[ref].padIsMm[pNum] = isMm;
                        }
                    }
                }
            }

            auto toUpperAscii = [](std::string s) {
                for (char& ch : s) {
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                }
                return s;
            };
            std::map<std::string, std::string> pcbNetByUpper;
            std::set<std::string> routedPcbNets;
            if (pcbState.contains("tracks") && pcbState["tracks"].is_array()) {
                for (const auto& trk : pcbState["tracks"]) {
                    if (!trk.is_object()) continue;
                    std::string tNet = trk.value("net", "");
                    if (!tNet.empty()) {
                        routedPcbNets.insert(tNet);
                        pcbNetByUpper[toUpperAscii(tNet)] = tNet;
                    }
                }
            }
            if (pcbState.contains("vias") && pcbState["vias"].is_array()) {
                for (const auto& via : pcbState["vias"]) {
                    if (!via.is_object()) continue;
                    std::string vNet = via.value("net", "");
                    if (!vNet.empty()) {
                        routedPcbNets.insert(vNet);
                        pcbNetByUpper[toUpperAscii(vNet)] = vNet;
                    }
                }
            }
            for (const auto& kv : pcbComps) {
                for (const auto& pkv : kv.second.padNets) {
                    if (!pkv.second.empty() &&
                        pcbNetByUpper.find(toUpperAscii(pkv.second)) == pcbNetByUpper.end()) {
                        pcbNetByUpper[toUpperAscii(pkv.second)] = pkv.second;
                    }
                }
            }
            auto normalizeToPcbNetCase = [&](const std::string& net) -> std::string {
                if (net.empty()) return net;
                auto it = pcbNetByUpper.find(toUpperAscii(net));
                if (it != pcbNetByUpper.end()) {
                    return it->second;
                }
                return net;
            };

            collectSheetNamedNets(schState);
            {
                std::set<std::string> normNamed;
                for (const auto& n : namedSchNets) normNamed.insert(normalizeToPcbNetCase(n));
                namedSchNets = std::move(normNamed);
                std::set<std::string> normValid;
                for (const auto& n : validSchNets) normValid.insert(normalizeToPcbNetCase(n));
                validSchNets = std::move(normValid);
            }

            // Resolve synthetic "$NET_*" names from calcSchematicErcAudit against existing PCB pad/track nets
            auto resolveSchNet = [&](const std::string& rawNet) -> std::string {
                if (rawNet.empty()) return "";
                if (rawNet.rfind("$NET_", 0) != 0) {
                    return normalizeToPcbNetCase(rawNet);
                }
                auto memIt = inferredNetMembers.find(rawNet);
                if (memIt != inferredNetMembers.end()) {
                    if (memIt->second.size() <= 1) {
                        return "";
                    }
                    for (const auto& rp : memIt->second) {
                        auto cIt = pcbComps.find(rp.first);
                        if (cIt != pcbComps.end()) {
                            auto pIt = cIt->second.padNets.find(rp.second);
                            if (pIt != cIt->second.padNets.end() && !pIt->second.empty() &&
                                namedSchNets.find(pIt->second) == namedSchNets.end()) {
                                return pIt->second;
                            }
                        }
                    }
                    if (pcbState.contains("tracks") && pcbState["tracks"].is_array()) {
                        for (const auto& rp : memIt->second) {
                            auto cIt = pcbComps.find(rp.first);
                            if (cIt == pcbComps.end()) continue;
                            auto coordIt = cIt->second.padCoords.find(rp.second);
                            if (coordIt == cIt->second.padCoords.end()) continue;
                            bool padMm = cIt->second.padIsMm[rp.second];
                            double px = coordIt->second.first;
                            double py = coordIt->second.second;
                            const double tol = padMm ? 0.15 : 5.0;
                            for (const auto& trk : pcbState["tracks"]) {
                                if (!trk.is_object()) continue;
                                std::string tNet = trk.value("net", "");
                                if (tNet.empty() || namedSchNets.find(tNet) != namedSchNets.end()) continue;
                                bool trkMm = trk.contains("x1_mm");
                                if (trkMm != padMm) continue;
                                double x1 = trkMm ? trk.value("x1_mm", 0.0) : trk.value("x1", 0.0);
                                double y1 = trkMm ? trk.value("y1_mm", 0.0) : trk.value("y1", 0.0);
                                double x2 = trkMm ? trk.value("x2_mm", 0.0) : trk.value("x2", 0.0);
                                double y2 = trkMm ? trk.value("y2_mm", 0.0) : trk.value("y2", 0.0);
                                if (std::hypot(x1 - px, y1 - py) <= tol ||
                                    std::hypot(x2 - px, y2 - py) <= tol) {
                                    return tNet;
                                }
                            }
                        }
                    }
                    return memIt->second.front().first + "_" + memIt->second.front().second;
                }
                return "";
            };

            std::set<std::string> schRefsSet;
            for (const auto& kv : inferredNetMembers) {
                std::string resolved = resolveSchNet(kv.first);
                if (!resolved.empty()) {
                    validSchNets.insert(resolved);
                }
            }

            bool forceOverwriteRouted = remoteArgs.value("force_overwrite_routed_pads", false);
            std::map<std::pair<std::string, std::string>, std::string> targetPadNets;
            int updatedCompMetadata = 0;
            if (schState.contains("components") && schState["components"].is_array()) {
                for (const auto& sc : schState["components"]) {
                    if (!sc.is_object()) continue;
                    std::string ref = sc.value("reference", sc.value("ref", sc.value("designator", "")));
                    std::string compType = sc.value("component_type", sc.value("type", ""));
                    std::string compTypeLower = compType;
                    std::transform(compTypeLower.begin(), compTypeLower.end(), compTypeLower.begin(), [](unsigned char c) { return std::tolower(c); });
                    std::string fp = sc.value("footprint", "");

                    bool isSubcircuit = (compTypeLower == "subcircuit");
                    if (!isSubcircuit && ref.size() >= 2 && ref[0] == 'B') {
                        bool allDigits = true;
                        for (size_t i = 1; i < ref.size(); ++i) {
                            if (!std::isdigit(static_cast<unsigned char>(ref[i]))) {
                                allDigits = false;
                                break;
                            }
                        }
                        if (allDigits && (compTypeLower.empty() || compTypeLower == "part")) isSubcircuit = true;
                    }
                    if (isSubcircuit) {
                        subcircuitFatalErrors.push_back({
                            {"reference", ref},
                            {"id", sc.value("id", "")},
                            {"value", sc.value("value", "")},
                            {"footprint", fp},
                            {"component_type", "subcircuit"}
                        });
                    }

                    if (ref.empty() || compType == "netport" || compType == "frame" ||
                        compType == "power" || compType == "netflag" || compType == "sheet" ||
                        compType == "drawing" ||
                        ref.rfind("FRAME", 0) == 0 || ref.rfind("#", 0) == 0 ||
                        ref == "GND" || ref == "VCC" || ref == "A") {
                        continue;
                    }
                    if (isSubcircuit && !remoteArgs.value("fix_subcircuit_blocks", false)) {
                        continue;
                    }
                    if (fp.empty() && !compType.empty() && compType != "part" && !remoteArgs.value("fix_subcircuit_blocks", false)) {
                        continue;
                    }
                    if (!schRefsSet.insert(ref).second) {
                        continue;
                    }
                    std::string val = sc.value("value", "");
                    std::string lcsc = sc.value("lcsc", "");
                    std::string uniqueId = sc.value("unique_id", sc.value("uniqueId", ""));
                    if (ref.find('?') != std::string::npos) {
                        unannotatedReferences.push_back(ref);
                    }
                    if (fp.empty() && (!isSubcircuit || !remoteArgs.value("fix_subcircuit_blocks", false))) {
                        missingFootprints.push_back(ref);
                    }

                    std::map<std::string, std::string> schPinNets;
                    if (sc.contains("pins") && sc["pins"].is_array()) {
                        for (const auto& p : sc["pins"]) {
                            if (!p.is_object()) continue;
                            std::string pNum = p.value("number", "");
                            bool isNoConn = p.value("no_connected", false);
                            std::string pNet = isNoConn ? "" : normalizeToPcbNetCase(p.value("net", ""));
                            if (!isNoConn && pNet.empty() && !pNum.empty()) {
                                auto infIt = inferredPinNets.find({ref, pNum});
                                if (infIt != inferredPinNets.end()) {
                                    pNet = resolveSchNet(infIt->second);
                                }
                            }
                            if (!pNum.empty()) {
                                schPinNets[pNum] = pNet;
                                targetPadNets[{ref, pNum}] = pNet;
                            }
                            if (!pNet.empty()) {
                                validSchNets.insert(pNet);
                            }
                        }
                    }

                    auto pcbIt = pcbComps.find(ref);
                    if (pcbIt == pcbComps.end()) {
                        if (ref.find('?') == std::string::npos && !fp.empty()) {
                            nlohmann::json padNetsJson = nlohmann::json::object();
                            for (const auto& kv : schPinNets) {
                                if (!kv.second.empty()) {
                                    padNetsJson[kv.first] = kv.second;
                                }
                            }
                            nlohmann::json addEntry = {
                                {"reference", ref},
                                {"value", val},
                                {"footprint", fp}
                            };
                            if (!lcsc.empty()) addEntry["lcsc"] = lcsc;
                            if (!uniqueId.empty()) addEntry["unique_id"] = uniqueId;
                            if (!padNetsJson.empty()) addEntry["pad_nets"] = std::move(padNetsJson);
                            componentsToAdd.push_back(std::move(addEntry));
                        }
                    } else {
                        if (!fp.empty() && !pcbIt->second.footprint.empty() && fp != pcbIt->second.footprint) {
                            footprintMismatches.push_back({
                                {"reference", ref},
                                {"schematic_footprint", fp},
                                {"pcb_footprint", pcbIt->second.footprint}
                            });
                        }
                        if (!uniqueId.empty() && !pcbIt->second.uniqueId.empty() && pcbIt->second.uniqueId != uniqueId) {
                            uniqueIdMismatches.push_back({
                                {"reference", ref},
                                {"schematic_id", sc.value("id", "")},
                                {"pcb_id", pcbIt->second.id},
                                {"schematic_unique_id", uniqueId},
                                {"pcb_unique_id", pcbIt->second.uniqueId}
                            });
                        }
                        if (!isDryRun && (_connected || gw != nullptr) && !pcbIt->second.id.empty()) {
                            nlohmann::json compModProps = nlohmann::json::object();
                            if (!uniqueId.empty() && pcbIt->second.uniqueId != uniqueId) {
                                compModProps["uniqueId"] = uniqueId;
                            }
                            if (!val.empty() && pcbIt->second.value != val) {
                                compModProps["name"] = val;
                            }
                            if (!compModProps.empty()) {
                                nlohmann::json modRes;
                                if (invokeEasyEdaApi("eda.pcb_PrimitiveComponent.modify",
                                                     nlohmann::json::array({pcbIt->second.id, compModProps}),
                                                     modRes)) {
                                    ++updatedCompMetadata;
                                }
                            }
                        }
                        for (const auto& kv : schPinNets) {
                            std::string oldNet = pcbIt->second.padNets.count(kv.first)
                                ? pcbIt->second.padNets[kv.first]
                                : "";
                            if (!forceOverwriteRouted && !oldNet.empty() && oldNet != kv.second &&
                                validSchNets.find(oldNet) != validSchNets.end() &&
                                routedPcbNets.find(oldNet) != routedPcbNets.end()) {
                                continue;
                            }
                            if (oldNet != kv.second && (!oldNet.empty() || !kv.second.empty())) {
                                nlohmann::json deltaObj = {
                                    {"reference", ref},
                                    {"pad", kv.first},
                                    {"old_net", oldNet},
                                    {"new_net", kv.second}
                                };
                                auto pidIt = pcbIt->second.padIds.find(kv.first);
                                if (pidIt != pcbIt->second.padIds.end() && !pidIt->second.empty()) {
                                    deltaObj["pad_id"] = pidIt->second;
                                }
                                padNetDeltas.push_back(std::move(deltaObj));
                            }
                        }
                    }
                }
            }

            for (const auto& kv : pcbComps) {
                if (schRefsSet.find(kv.first) == schRefsSet.end()) {
                    componentsToRemove.push_back({
                        {"reference", kv.first},
                        {"value", kv.second.value},
                        {"footprint", kv.second.footprint}
                    });
                }
            }

            if (pcbState.contains("tracks") && pcbState["tracks"].is_array()) {
                for (const auto& trk : pcbState["tracks"]) {
                    if (!trk.is_object()) continue;
                    std::string tNet = trk.value("net", "");
                    if (!tNet.empty() && validSchNets.find(tNet) == validSchNets.end()) {
                        orphanedTracks.push_back({
                            {"id", trk.value("id", "")},
                            {"layer", trk.value("layer", "F.Cu")},
                            {"net", tNet},
                            {"x1", trk.value("x1", trk.value("x1_mm", 0.0))},
                            {"y1", trk.value("y1", trk.value("y1_mm", 0.0))},
                            {"x2", trk.value("x2", trk.value("x2_mm", 0.0))},
                            {"y2", trk.value("y2", trk.value("y2_mm", 0.0))}
                        });
                    }
                }
            }

            int fixedSubcircuitBlocks = prevFixedSubcircuitBlocks;
            if (remoteArgs.value("fix_subcircuit_blocks", false)) {
                for (const auto& sub : subcircuitFatalErrors) {
                    std::string sid = sub.value("id", "");
                    if (!isDryRun && (_connected || gw != nullptr) && !sid.empty()) {
                        nlohmann::json modRes;
                        invokeEasyEdaApi("eda.sch_PrimitiveComponent.modify",
                                         nlohmann::json::array({sid, {{"componentType", "part"}}}), modRes);
                    }
                    ++fixedSubcircuitBlocks;
                }
            }

            int syncedUniqueIds = prevSyncedUniqueIds;
            if (remoteArgs.value("sync_unique_ids", false)) {
                for (const auto& m : uniqueIdMismatches) {
                    std::string pcbId = m.value("pcb_id", "");
                    std::string sUid = m.value("schematic_unique_id", "");
                    std::string r = m.value("reference", "");
                    if (!isDryRun && (_connected || gw != nullptr) && !pcbId.empty()) {
                        nlohmann::json modRes;
                        invokeEasyEdaApi("eda.pcb_PrimitiveComponent.modify",
                                         nlohmann::json::array({pcbId, {{"uniqueId", sUid}}}), modRes);
                    }
                    if (pcbComps.count(r) > 0) {
                        pcbComps[r].uniqueId = sUid;
                    }
                    if (pcbState.is_object()) {
                        for (const char* k : {"components", "footprints", "pcb_components"}) {
                            if (pcbState.contains(k) && pcbState[k].is_array()) {
                                for (auto& c : pcbState[k]) {
                                    if (c.is_object() && c.value("reference", c.value("ref", "")) == r) {
                                        c["unique_id"] = sUid;
                                        c["uniqueId"] = sUid;
                                    }
                                }
                            }
                        }
                    }
                    ++syncedUniqueIds;
                }
            }

            if (unnamedWireDeltas.empty() && schState.contains("wires") && schState["wires"].is_array()) {
                for (const auto& w : schState["wires"]) {
                    if (!w.is_object()) continue;
                    std::string wNet = w.value("net", "");
                    if (!wNet.empty()) continue;
                    std::string wId = w.value("id", "");
                    std::vector<std::pair<double, double>> wPts;
                    if (w.contains("line") && w["line"].is_array()) {
                        const auto& lArr = w["line"];
                        if (!lArr.empty() && lArr[0].is_number()) {
                            for (size_t k = 0; k + 1 < lArr.size(); k += 2) {
                                wPts.push_back({lArr[k].get<double>(), lArr[k + 1].get<double>()});
                            }
                        } else {
                            for (const auto& pt : lArr) {
                                if (pt.is_array() && pt.size() >= 2) {
                                    wPts.push_back({pt[0].get<double>(), pt[1].get<double>()});
                                } else if (pt.is_object() && pt.contains("x") && pt.contains("y")) {
                                    wPts.push_back({pt["x"].get<double>(), pt["y"].get<double>()});
                                }
                            }
                        }
                    } else if (w.contains("x1") && w.contains("y1") && w.contains("x2") && w.contains("y2")) {
                        wPts.push_back({w["x1"].get<double>(), w["y1"].get<double>()});
                        wPts.push_back({w["x2"].get<double>(), w["y2"].get<double>()});
                    }
                    if (wPts.empty()) continue;
                    nlohmann::json matchedDelta = nullptr;
                    for (const auto& sc : schState["components"]) {
                        if (!sc.is_object()) continue;
                        std::string ref = sc.value("reference", sc.value("ref", ""));
                        if (ref.empty() || !sc.contains("pins") || !sc["pins"].is_array()) continue;
                        auto pcbCompIt = pcbComps.find(ref);
                        for (const auto& p : sc["pins"]) {
                            if (!p.is_object()) continue;
                            std::string pNum = p.value("number", p.value("name", ""));
                            std::string targetNet;
                            if (pcbCompIt != pcbComps.end()) {
                                auto pnIt = pcbCompIt->second.padNets.find(pNum);
                                if (pnIt != pcbCompIt->second.padNets.end()) targetNet = pnIt->second;
                            }
                            if (targetNet.empty()) continue;
                            double px = p.value("x", 0.0);
                            double py = p.value("y", 0.0);
                            for (const auto& pt : wPts) {
                                if (std::fabs(pt.first - px) <= 2.5 &&
                                    (std::fabs(pt.second - py) <= 2.5 || std::fabs(std::fabs(pt.second) - std::fabs(py)) <= 2.5)) {
                                    matchedDelta = {
                                        {"wire_id", wId},
                                        {"net", targetNet},
                                        {"reference", ref},
                                        {"pin", pNum}
                                    };
                                    break;
                                }
                            }
                            if (!matchedDelta.is_null()) break;
                        }
                        if (!matchedDelta.is_null()) break;
                    }
                    if (!matchedDelta.is_null()) {
                        unnamedWireDeltas.push_back(matchedDelta);
                    }
                }
            }

            int reconciledWires = prevReconciledWires;
            if (remoteArgs.value("reconcile_net_names", false)) {
                for (const auto& uwd : unnamedWireDeltas) {
                    std::string wid = uwd.value("wire_id", "");
                    std::string net = uwd.value("net", "");
                    if (!wid.empty() && !net.empty()) {
                        if (!isDryRun && (_connected || gw != nullptr)) {
                            nlohmann::json modRes;
                            invokeEasyEdaApi("eda.sch_PrimitiveWire.modify",
                                             nlohmann::json::array({wid, {{"net", net}}}), modRes);
                        }
                        if (schState.contains("wires") && schState["wires"].is_array()) {
                            for (auto& w : schState["wires"]) {
                                if (w.is_object() && w.value("id", "") == wid) {
                                    w["net"] = net;
                                    break;
                                }
                            }
                        }
                        ++reconciledWires;
                    }
                }
            }

            bool pushedSynthesizedNetlist = prevPushedSynthesizedNetlist;
            if (remoteArgs.value("push_synthesized_netlist", false)) {
                pushedSynthesizedNetlist = true;
            }

            bool readyToSync = unannotatedReferences.empty() && missingFootprints.empty() &&
                               (subcircuitFatalErrors.empty() || remoteArgs.value("fix_subcircuit_blocks", false));
            int appliedPadModifications = 0;
            int updatedOrphanedTracks = 0;
            bool didSync = false;

            if (!isDryRun && readyToSync && (_connected || gw != nullptr)) {
                std::map<std::string, std::string> mergedOldToNewNet;
                for (const auto& delta : padNetDeltas) {
                    std::string padId = delta.value("pad_id", "");
                    std::string oldNet = delta.value("old_net", "");
                    std::string newNet = delta.value("new_net", "");
                    if (!oldNet.empty() && !newNet.empty() &&
                        validSchNets.find(oldNet) == validSchNets.end()) {
                        mergedOldToNewNet[oldNet] = newNet;
                    }
                    if (!padId.empty()) {
                        nlohmann::json modRes;
                        if (invokeEasyEdaApi("eda.pcb_PrimitivePad.modify",
                                             nlohmann::json::array({padId, {{"net", newNet}}}),
                                             modRes)) {
                            ++appliedPadModifications;
                        }
                    }
                }
                if (pcbState.contains("tracks") && pcbState["tracks"].is_array()) {
                    for (const auto& trk : pcbState["tracks"]) {
                        if (!trk.is_object()) continue;
                        std::string tNet = trk.value("net", "");
                        if (tNet.empty() || validSchNets.find(tNet) != validSchNets.end() ||
                            mergedOldToNewNet.find(tNet) != mergedOldToNewNet.end()) {
                            continue;
                        }
                        bool trkMm = trk.contains("x1_mm");
                        double x1 = trkMm ? trk.value("x1_mm", 0.0) : trk.value("x1", 0.0);
                        double y1 = trkMm ? trk.value("y1_mm", 0.0) : trk.value("y1", 0.0);
                        double x2 = trkMm ? trk.value("x2_mm", 0.0) : trk.value("x2", 0.0);
                        double y2 = trkMm ? trk.value("y2_mm", 0.0) : trk.value("y2", 0.0);
                        for (const auto& ckv : pcbComps) {
                            for (const auto& coordKv : ckv.second.padCoords) {
                                bool padMm = ckv.second.padIsMm.at(coordKv.first);
                                if (padMm != trkMm) continue;
                                const double tol = padMm ? 0.15 : 5.0;
                                double px = coordKv.second.first;
                                double py = coordKv.second.second;
                                if (std::hypot(x1 - px, y1 - py) <= tol ||
                                    std::hypot(x2 - px, y2 - py) <= tol) {
                                    auto tIt = targetPadNets.find({ckv.first, coordKv.first});
                                    if (tIt != targetPadNets.end() && !tIt->second.empty() &&
                                        validSchNets.find(tIt->second) != validSchNets.end()) {
                                        mergedOldToNewNet[tNet] = tIt->second;
                                        break;
                                    }
                                }
                            }
                            if (mergedOldToNewNet.find(tNet) != mergedOldToNewNet.end()) {
                                break;
                            }
                        }
                    }
                    for (const auto& trk : pcbState["tracks"]) {
                        if (!trk.is_object()) continue;
                        std::string trkId = trk.value("id", "");
                        std::string tNet = trk.value("net", "");
                        // Never touch child sub-primitives inside a footprint (<compId>e<subId>)
                        if (trkId.empty() || trkId.find('e', 1) != std::string::npos) continue;
                        auto mIt = mergedOldToNewNet.find(tNet);
                        if (mIt != mergedOldToNewNet.end()) {
                            nlohmann::json modRes;
                            if (invokeEasyEdaApi("eda.pcb_PrimitiveLine.modify",
                                                 nlohmann::json::array({trkId, {{"net", mIt->second}}}),
                                                 modRes)) {
                                ++updatedOrphanedTracks;
                            }
                        }
                    }
                }
                didSync = true;
            }

            nlohmann::json prevNativeImport = (rpcResult.is_object() && rpcResult.contains("native_import_changes"))
                ? rpcResult["native_import_changes"] : nlohmann::json();
            int prevResolvedTemplates = rpcResult.is_object()
                ? rpcResult.value("resolved_value_templates", 0) : 0;
            if (!isDryRun && !didSync && arguments.value("trigger_native_import", false)) {
                if (prevNativeImport.is_null()) {
                    prevNativeImport = {{"triggered", false}, {"reason", "local_or_mock_mode"}};
                } else if (prevNativeImport.is_object() && prevNativeImport.value("triggered", false)) {
                    didSync = true;
                }
            }

            rpcResult = {
                {"ok", true},
                {"dry_run", isDryRun},
                {"synced", didSync},
                {"ready_to_sync", readyToSync},
                {"applied_pad_net_deltas", appliedPadModifications},
                {"updated_component_metadata", updatedCompMetadata},
                {"updated_orphaned_tracks", updatedOrphanedTracks},
                {"components_to_add", componentsToAdd},
                {"components_to_remove", componentsToRemove},
                {"footprint_mismatches", footprintMismatches},
                {"unique_id_mismatches", uniqueIdMismatches},
                {"subcircuit_fatal_errors", subcircuitFatalErrors},
                {"unnamed_wire_deltas", unnamedWireDeltas},
                {"unannotated_references", unannotatedReferences},
                {"missing_footprints", missingFootprints},
                {"pad_net_deltas", padNetDeltas},
                {"orphaned_tracks", orphanedTracks},
                {"fixed_subcircuit_blocks", fixedSubcircuitBlocks},
                {"synced_unique_ids", syncedUniqueIds},
                {"reconciled_wires", reconciledWires},
                {"pushed_synthesized_netlist", pushedSynthesizedNetlist}
            };
            if (!prevNativeImport.is_null()) {
                rpcResult["native_import_changes"] = prevNativeImport;
            }
            if (prevResolvedTemplates > 0) {
                rpcResult["resolved_value_templates"] = prevResolvedTemplates;
            }
        }
    }

    if (toolName == "easyeda_sync_sch_to_pcb" && rpcResult.is_object() &&
        !remoteArgs.value("dry_run", false) && remoteArgs.value("trigger_native_import", false) &&
        !rpcResult.contains("native_import_changes")) {
        rpcResult["native_import_changes"] = {{"triggered", false}, {"reason", "local_or_mock_mode"}};
    }

    bool toolOk = rpcResult.is_object() ? rpcResult.value("ok", true) : true;
    std::string errMsg = rpcResult.is_object() ? rpcResult.value("error", "") : "";
    std::string targetDoc = rpcResult.is_object()
        ? rpcResult.value("active_document",
          rpcResult.value("schematic_path",
          rpcResult.value("pcb_path",
          rpcResult.value("document_path",
          rpcResult.value("output_path", "")))))
        : "";
    std::string summary = toolOk ? ("Executed " + toolName) : errMsg;

    if (toolName == "easyeda_live_status" && rpcResult.is_object()) {
        nlohmann::json recentArr = nlohmann::json::array();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            size_t limit = std::min<size_t>(25, _recentCalls.size());
            for (size_t i = 0; i < limit; ++i) {
                const auto& entry = _recentCalls[i];
                std::string entrySummary = entry.summary;
                if (entrySummary.empty()) {
                    entrySummary = entry.success ? ("Executed " + entry.toolName) : ("Error in " + entry.toolName);
                }
                recentArr.push_back({
                    {"timestamp_ms", entry.timestampEpoch * 1000},
                    {"tool", entry.toolName},
                    {"component", entry.component},
                    {"target_document", entry.targetDocument},
                    {"status", entry.success ? "ok" : "error"},
                    {"duration_ms", entry.durationMs},
                    {"summary", entrySummary}
                });
            }
        }
        rpcResult["recent_calls"] = recentArr;
        rpcResult["recent_calls_count"] = static_cast<int>(recentArr.size());
    }

    recordToolCall(comp, toolName, targetDoc, elapsedMs, toolOk, summary, errMsg);
    return rpcResult;
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
