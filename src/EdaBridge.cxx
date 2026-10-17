/*
 * EdaBridge.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "EdaBridge.hxx"
#include "EasyEdaConverter.hxx"
#include "TcpGateway.hxx"
#include "PathUtils.hxx"
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

bool isWindowsStylePrefix(const std::string& prefix) {
    if (prefix.find('\\') != std::string::npos) return true;
    if (prefix.size() >= 2 && std::isalpha(static_cast<unsigned char>(prefix[0])) && prefix[1] == ':') {
        return true;
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

    EdaToolRuntimeInfo acc;
    acc.name = "accelerators";
    acc.enabled = _config.accelerators.enabled;
    acc.resolvedHost = "local (aimon)";
    acc.resolvedPort = 0;
    acc.resolvedPluginPort = 0;
    acc.executionMode = "in_process_cxx17";
    acc.available = _config.accelerators.enabled;
    acc.pluginConnected = _config.accelerators.enabled;
    acc.binaryPath = "aimon::EdaAccelerators (C++17)";
    acc.version = "1.0";
    acc.activeDocument = "7 local solvers ready";
    acc.lastProbeLatencyMs = 0;
    preserveStats("accelerators", acc);
    updated.push_back(acc);

    EdaToolRuntimeInfo ee;
    ee.name = "easyeda";
    ee.enabled = _config.easyeda.enabled;
    ee.resolvedHost = _config.resolvedHostFor(_config.easyeda.host);
    ee.resolvedPort = _config.resolvedPortFor(_config.easyeda.port);
    ee.resolvedPluginPort = _config.resolvedPluginPortFor(_config.easyeda.pluginPort);
    ee.executionMode = _config.easyeda.executionMode;
    ee.binaryPath = _config.easyeda.customBinaryPath;
    preserveStats("easyeda", ee);
    updated.push_back(ee);

    EdaToolRuntimeInfo kc;
    kc.name = "kicad";
    kc.enabled = _config.kicad.enabled;
    kc.resolvedHost = _config.resolvedHostFor(_config.kicad.host);
    kc.resolvedPort = _config.resolvedPortFor(_config.kicad.port);
    kc.resolvedPluginPort = _config.resolvedPluginPortFor(_config.kicad.pluginPort);
    kc.executionMode = _config.kicad.executionMode;
    kc.binaryPath = _config.kicad.customBinaryPath;
    preserveStats("kicad", kc);
    updated.push_back(kc);

    EdaToolRuntimeInfo fc;
    fc.name = "freecad";
    fc.enabled = _config.freecad.enabled;
    fc.resolvedHost = _config.resolvedHostFor(_config.freecad.host);
    fc.resolvedPort = _config.resolvedPortFor(_config.freecad.port);
    fc.resolvedPluginPort = _config.resolvedPluginPortFor(_config.freecad.pluginPort);
    fc.executionMode = _config.freecad.executionMode;
    fc.binaryPath = _config.freecad.customBinaryPath;
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

bool EdaBridge::translateLinuxToWindows(const std::string& linuxPath,
                                        std::string& outWinPath,
                                        std::string& outError) const {
    outWinPath.clear();
    outError.clear();

    if (hasNullOrDotDotSegment(linuxPath)) {
        outError = "Invalid or traversal-containing Linux path";
        return false;
    }

    std::string linuxPrefixCfg;
    std::string winPrefixCfg;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        linuxPrefixCfg = _config.linuxPathPrefix;
        winPrefixCfg = _config.windowsPathPrefix;
    }

    if (hasNullOrDotDotSegment(linuxPrefixCfg) || winPrefixCfg.empty()) {
        outError = "Invalid path mapping configuration";
        return false;
    }

    std::string expandedPrefix = fs::path(PathUtils::expandHome(linuxPrefixCfg)).lexically_normal().string();
    while (expandedPrefix.size() > 1 && expandedPrefix.back() == '/') {
        expandedPrefix.pop_back();
    }

    std::string expandedInput = fs::path(PathUtils::expandHome(linuxPath)).lexically_normal().string();
    while (expandedInput.size() > 1 && expandedInput.back() == '/') {
        expandedInput.pop_back();
    }

    if (expandedInput.find("..") != std::string::npos) {
        outError = "Normalized path escapes workspace";
        return false;
    }

    if (expandedInput.rfind(expandedPrefix, 0) != 0) {
        outError = "Path is outside configured linux_path_prefix (" + linuxPrefixCfg + ")";
        return false;
    }

    std::string relSuffix;
    if (expandedInput.size() > expandedPrefix.size()) {
        if (expandedInput[expandedPrefix.size()] != '/') {
            outError = "Sibling path does not match linux_path_prefix boundary";
            return false;
        }
        relSuffix = expandedInput.substr(expandedPrefix.size() + 1);
    }

    bool winTarget = isWindowsStylePrefix(winPrefixCfg);
    char sep = winTarget ? '\\' : '/';
    std::string base = winPrefixCfg;
    while (base.size() > 1 && (base.back() == '\\' || base.back() == '/')) {
        // Keep trailing slash only for bare drive root like "Z:\"
        if (winTarget && base.size() == 3 && base[1] == ':') {
            break;
        }
        base.pop_back();
    }

    if (relSuffix.empty()) {
        outWinPath = base;
        return true;
    }

    for (char& ch : relSuffix) {
        if (ch == '/' || ch == '\\') {
            ch = sep;
        }
    }

    if (base.back() == sep) {
        outWinPath = base + relSuffix;
    } else {
        outWinPath = base + sep + relSuffix;
    }
    return true;
}

bool EdaBridge::translateWindowsToLinux(const std::string& winPath,
                                        std::string& outLinuxPath) const {
    outLinuxPath.clear();
    if (hasNullOrDotDotSegment(winPath)) {
        return false;
    }

    std::string linuxPrefixCfg;
    std::string winPrefixCfg;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        linuxPrefixCfg = _config.linuxPathPrefix;
        winPrefixCfg = _config.windowsPathPrefix;
    }

    if (linuxPrefixCfg.empty() || winPrefixCfg.empty()) {
        return false;
    }

    std::string expandedLinuxPrefix = fs::path(PathUtils::expandHome(linuxPrefixCfg)).lexically_normal().string();
    while (expandedLinuxPrefix.size() > 1 && expandedLinuxPrefix.back() == '/') {
        expandedLinuxPrefix.pop_back();
    }

    std::string normWinPrefix = winPrefixCfg;
    std::replace(normWinPrefix.begin(), normWinPrefix.end(), '\\', '/');
    while (normWinPrefix.size() > 1 && normWinPrefix.back() == '/') {
        normWinPrefix.pop_back();
    }

    std::string normWinInput = winPath;
    std::replace(normWinInput.begin(), normWinInput.end(), '\\', '/');
    while (normWinInput.size() > 1 && normWinInput.back() == '/') {
        normWinInput.pop_back();
    }

    auto caseInsensitiveDriveMatch = [](const std::string& input, const std::string& prefix) -> bool {
        if (input.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (i == 0 && prefix.size() >= 2 && prefix[1] == ':') {
                if (std::tolower(static_cast<unsigned char>(input[i])) !=
                    std::tolower(static_cast<unsigned char>(prefix[i]))) {
                    return false;
                }
            } else if (input[i] != prefix[i]) {
                return false;
            }
        }
        return true;
    };

    if (!caseInsensitiveDriveMatch(normWinInput, normWinPrefix)) {
        return false;
    }

    std::string rel;
    if (normWinInput.size() > normWinPrefix.size()) {
        if (normWinInput[normWinPrefix.size()] != '/') {
            return false;
        }
        rel = normWinInput.substr(normWinPrefix.size() + 1);
    }

    if (rel.empty()) {
        outLinuxPath = expandedLinuxPrefix;
    } else {
        outLinuxPath = expandedLinuxPrefix + "/" + rel;
    }
    return true;
}

bool EdaBridge::rewriteJsonPaths(nlohmann::json& payload,
                                 bool linuxToWindows,
                                 std::string& outError) const {
    outError.clear();
    if (payload.is_object()) {
        for (auto it = payload.begin(); it != payload.end(); ++it) {
            const std::string& key = it.key();
            nlohmann::json& val = it.value();
            if (val.is_string() && isPathFieldKey(key)) {
                std::string s = val.get<std::string>();
                if (linuxToWindows) {
                    std::string mapped;
                    if (!translateLinuxToWindows(s, mapped, outError)) {
                        return false;
                    }
                    val = mapped;
                } else {
                    std::string mapped;
                    if (!translateWindowsToLinux(s, mapped)) {
                        outError = "Windows path outside mapped prefix: " + s;
                        return false;
                    }
                    val = mapped;
                }
            } else if (val.is_array() && isPathFieldKey(key)) {
                for (auto& elem : val) {
                    if (elem.is_string()) {
                        std::string s = elem.get<std::string>();
                        if (linuxToWindows) {
                            std::string mapped;
                            if (!translateLinuxToWindows(s, mapped, outError)) {
                                return false;
                            }
                            elem = mapped;
                        } else {
                            std::string mapped;
                            if (!translateWindowsToLinux(s, mapped)) {
                                outError = "Windows path outside mapped prefix: " + s;
                                return false;
                            }
                            elem = mapped;
                        }
                    } else if (elem.is_object() || elem.is_array()) {
                        if (!rewriteJsonPaths(elem, linuxToWindows, outError)) {
                            return false;
                        }
                    }
                }
            } else if (val.is_object() || val.is_array()) {
                if (!rewriteJsonPaths(val, linuxToWindows, outError)) {
                    return false;
                }
            }
        }
    } else if (payload.is_array()) {
        for (auto& elem : payload) {
            if (elem.is_object() || elem.is_array()) {
                if (!rewriteJsonPaths(elem, linuxToWindows, outError)) {
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
        if (apiPath.rfind(prefix, 0) == 0 && apiPath.size() > pLen &&
            apiPath.find('.', pLen) != std::string::npos) {
            return true;
        }
    }

    outReason = "EasyEDA API '" + apiPath +
                "' is outside the allowed eda.{sch_*,pcb_*,lib_*,dmt_*,drc_*,sys_Document*} whitelist";
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
    std::lock_guard<std::mutex> lock(_mutex);
    EdaBridgeStatus st;
    st.enabled = _config.enabled;
    st.connected = _connected;
    st.mode = _config.mode;
    st.activeTransport = _activeTransport;
    st.host = _config.host;
    st.port = _config.port;
    st.pluginPort = _config.pluginPort;
    st.authConfigured = !_config.authSecret.empty();
    st.healthPollIntervalSec = _config.healthPollIntervalSec;
    st.requireApprovalForExports = _config.requireApprovalForExports;
    st.linuxPathPrefix = _config.linuxPathPrefix;
    st.windowsPathPrefix = _config.windowsPathPrefix;
    st.lastProbeEpoch = _lastProbeEpoch;
    st.lastLatencyMs = _lastLatencyMs;
    st.remotePlatform = _remotePlatform;
    st.tools = _toolInfos;
    st.recentCalls.assign(_recentCalls.begin(), _recentCalls.end());
    st.configSnapshot = _config.toJson(true);
    st.errorMessage = _lastError;
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
            if (statusPayload.contains("tools") && statusPayload["tools"].is_array()) {
                for (const auto& rt : statusPayload["tools"]) {
                    if (!rt.is_object()) continue;
                    std::string tName = rt.value("name", "");
                    EdaToolRuntimeInfo* info = findToolInfoLocked(tName);
                    if (info) {
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
            for (auto& info : _toolInfos) {
                if (info.name != "accelerators") {
                    info.available = false;
                    info.pluginConnected = false;
                    info.lastProbeLatencyMs = -1;
                }
            }
        }
    }

    return getStatus();
}

nlohmann::json EdaBridge::callTool(const std::string& toolName,
                                   const nlohmann::json& arguments) {
    auto startTp = std::chrono::steady_clock::now();

    if (EdaAccelerators::isAcceleratorTool(toolName)) {
        EdaAccelerators accCopy;
        bool accEnabled = true;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            accEnabled = _config.accelerators.enabled;
            accCopy = _accelerators;
        }
        if (!accEnabled) {
            recordToolCall("accelerators", toolName, "local", 0, false,
                           "", "Local CPU accelerators are disabled in configuration");
            return {
                {"ok", false},
                {"error", "Local CPU accelerators are disabled in configuration"}
            };
        }

        bool ok = false;
        std::string summary;
        nlohmann::json res = accCopy.executeTool(toolName, arguments, ok, summary);
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

    if (!cfg.enabled) {
        recordToolCall(comp, toolName, "", 0, false, "",
                       "Remote Windows EDA bridge is disabled");
        return {
            {"ok", false},
            {"error", "Remote Windows EDA bridge is disabled"}
        };
    }

    if ((comp == "easyeda" && !cfg.easyeda.enabled) ||
        (comp == "kicad" && !cfg.kicad.enabled) ||
        (comp == "freecad" && !cfg.freecad.enabled)) {
        std::string msg = "Component '" + comp + "' is disabled in EdaBridgeConfig";
        recordToolCall(comp, toolName, "", 0, false, "", msg);
        return {{"ok", false}, {"error", msg}};
    }

    // Client-side pre-flight security & TNP guards
    if (toolName == "freecad_generate_cad" && cfg.freecad.enforceTnpGuard &&
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
    if (toolName == "easyeda_invoke_api" && arguments.is_object()) {
        std::string apiPath = arguments.value("api_path", arguments.value("api", arguments.value("method", "")));
        std::string reason;
        if (!guardEasyEdaApiCall(apiPath, reason)) {
            recordToolCall(comp, toolName, "", 0, false, "", reason);
            return {{"ok", false}, {"error", reason}, {"code", -32602}};
        }
        if (apiPath == "eda.sch_PrimitivePin.modify" &&
            arguments.contains("args") && arguments["args"].is_array() &&
            arguments["args"].size() >= 2 && arguments["args"][0].is_string() &&
            arguments["args"][1].is_object()) {
            std::string pid = arguments["args"][0].get<std::string>();
            const auto& pMod = arguments["args"][1];
            if (pMod.contains("noConnected") || pMod.contains("no_connected")) {
                bool noConn = pMod.value("noConnected", pMod.value("no_connected", false));
                std::lock_guard<std::mutex> lock(_mutex);
                _cachedPinNoConnect[pid] = noConn;
            }
        }
    }

    // Rewrite Linux paths in arguments to Windows/remote paths
    nlohmann::json remoteArgs = arguments.is_object() ? arguments : nlohmann::json::object();
    std::string pathErr;
    if (!rewriteJsonPaths(remoteArgs, true, pathErr)) {
        recordToolCall(comp, toolName, "", 0, false, "", pathErr);
        return {{"ok", false}, {"error", pathErr}, {"code", -32602}};
    }

    // Default coordinate units to "mil" for EasyEDA schematic and PCB edits
    if (toolName == "easyeda_edit_schematic" || toolName == "easyeda_edit_pcb") {
        if (!remoteArgs.contains("units") || !remoteArgs["units"].is_string() ||
            remoteArgs["units"].get<std::string>().empty()) {
            remoteArgs["units"] = "mil";
        }
    }

    if (toolName == "easyeda_edit_pcb") {
        if (remoteArgs.contains("move_components") && !remoteArgs.contains("placements")) {
            remoteArgs["placements"] = remoteArgs["move_components"];
        }
        if (remoteArgs.contains("add_tracks") && !remoteArgs.contains("tracks")) {
            remoteArgs["tracks"] = remoteArgs["add_tracks"];
        }
        if (remoteArgs.contains("add_vias") && !remoteArgs.contains("vias")) {
            remoteArgs["vias"] = remoteArgs["add_vias"];
        }
    }

    bool callerRequestedRawPrimitives = false;
    if (toolName == "easyeda_inspect_pcb") {
        callerRequestedRawPrimitives = remoteArgs.value("include_raw_primitives", false);
        remoteArgs["include_raw_primitives"] = true;
    }

    std::string targetHost = cfg.host;
    int targetPort = cfg.port;
    int timeoutMs = cfg.timeoutMs;
    if (comp == "easyeda") {
        targetHost = cfg.resolvedHostFor(cfg.easyeda.host);
        targetPort = cfg.resolvedPortFor(cfg.easyeda.port);
        timeoutMs = cfg.easyeda.timeoutMs;
    } else if (comp == "kicad") {
        targetHost = cfg.resolvedHostFor(cfg.kicad.host);
        targetPort = cfg.resolvedPortFor(cfg.kicad.port);
        timeoutMs = cfg.kicad.timeoutMs;
    } else if (comp == "freecad") {
        targetHost = cfg.resolvedHostFor(cfg.freecad.host);
        targetPort = cfg.resolvedPortFor(cfg.freecad.port);
        timeoutMs = cfg.freecad.timeoutMs;
    }

    auto dispatchRemoteRpc = [&](const std::string& rTool,
                                 const nlohmann::json& rArgs,
                                 nlohmann::json& outRes,
                                 std::string& outErr,
                                 int& outCode) -> bool {
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

    nlohmann::json rpcResult;
    std::string rpcError;
    int rpcErrorCode = 0;
    bool dispatchedOk = dispatchRemoteRpc(toolName, remoteArgs, rpcResult, rpcError, rpcErrorCode);

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
        return errObj;
    }

    // Rewrite any Windows paths in result back to Linux paths where applicable
    std::string ignoreReverseErr;
    rewriteJsonPaths(rpcResult, false, ignoreReverseErr);

    auto invokeEasyEdaApi = [&](const std::string& apiPath,
                                const nlohmann::json& apiArgs,
                                nlohmann::json& outApiRes) -> bool {
        std::string subErr;
        int subCode = 0;
        return dispatchRemoteRpc("easyeda_invoke_api", {
            {"api_path", apiPath},
            {"args", apiArgs}
        }, outApiRes, subErr, subCode);
    };

    auto mergeCachedPinStatesAndCacheSheet = [&](nlohmann::json& schPayload) {
        if (!schPayload.is_object() || !schPayload.contains("components") ||
            !schPayload["components"].is_array()) {
            return;
        }
        std::string sheetKey = schPayload.value("active_document", "default_sheet");
        if (schPayload.contains("document") && schPayload["document"].is_object()) {
            std::string docId = schPayload["document"].value(
                "uuid", schPayload["document"].value("title", ""));
            if (!docId.empty()) sheetKey = docId;
        }

        nlohmann::json sheetParts = nlohmann::json::array();
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& compObj : schPayload["components"]) {
            if (!compObj.is_object()) continue;
            std::string ref = compObj.value("reference", compObj.value("ref", compObj.value("designator", "")));
            std::string cType = compObj.value("component_type", compObj.value("type", "part"));
            bool isNetFlag = (cType == "netflag" || cType == "netport" || cType == "power" ||
                              ref.rfind("#", 0) == 0 || ref == "GND" || ref == "VCC");
            bool isFrame = (cType == "frame" || ref.rfind("FRAME", 0) == 0);
            if (!isNetFlag && !isFrame && (cType == "part" || cType.empty()) &&
                !ref.empty() && ref.find('?') == std::string::npos) {
                sheetParts.push_back({
                    {"reference", ref},
                    {"value", compObj.value("value", "")},
                    {"footprint", compObj.value("footprint", "")},
                    {"component_type", "part"}
                });
            }
            if (compObj.contains("pins") && compObj["pins"].is_array()) {
                for (auto& pinObj : compObj["pins"]) {
                    if (!pinObj.is_object()) continue;
                    std::string pid = pinObj.value("id", "");
                    auto it = _cachedPinNoConnect.find(pid);
                    if (it != _cachedPinNoConnect.end()) {
                        pinObj["no_connected"] = it->second;
                    }
                }
            }
        }
        _cachedSchematicComponentsBySheet[sheetKey] = sheetParts;
    };

    // Post-processing for EasyEDA tools
    if (toolName == "easyeda_inspect_schematic" && rpcResult.is_object()) {
        mergeCachedPinStatesAndCacheSheet(rpcResult);

        if (rpcResult.contains("components") && rpcResult["components"].is_array()) {
            EdaAccelerators accCopy;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                accCopy = _accelerators;
            }
            bool ercOk = false;
            std::string ercSummary;
            rpcResult["erc_audit"] = accCopy.calcSchematicErcAudit(rpcResult, ercOk, ercSummary);

            // Apply optional filters (component_types, references, nets, include_pins, include_wires, summary_only)
            std::set<std::string> refSet;
            if (arguments.is_object() && arguments.contains("references") && arguments["references"].is_array()) {
                for (const auto& r : arguments["references"]) {
                    if (r.is_string() && !r.get<std::string>().empty()) {
                        refSet.insert(r.get<std::string>());
                    }
                }
            }
            std::set<std::string> netSet;
            if (arguments.is_object() && arguments.contains("nets") && arguments["nets"].is_array()) {
                for (const auto& n : arguments["nets"]) {
                    if (n.is_string() && !n.get<std::string>().empty()) {
                        netSet.insert(n.get<std::string>());
                    }
                }
            }
            std::set<std::string> typeSet;
            if (arguments.is_object() && arguments.contains("component_types") && arguments["component_types"].is_array()) {
                for (const auto& ct : arguments["component_types"]) {
                    if (ct.is_string() && !ct.get<std::string>().empty()) {
                        typeSet.insert(ct.get<std::string>());
                    }
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

            bool includePins = arguments.is_object() ? arguments.value("include_pins", true) : true;
            bool includeWires = arguments.is_object() ? arguments.value("include_wires", true) : true;
            bool summaryOnly = arguments.is_object() ? arguments.value("summary_only", false) : false;
            bool hasCompFilter = (!refSet.empty() || !netSet.empty() || !typeSet.empty());

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
                    if (!keepWire && !matchedPinCoords.empty() && w.contains("line") && w["line"].is_array()) {
                        for (const auto& pt : w["line"]) {
                            if (pt.is_array() && pt.size() >= 2 && pt[0].is_number() && pt[1].is_number()) {
                                double wx = pt[0].get<double>();
                                double wy = pt[1].get<double>();
                                for (const auto& mp : matchedPinCoords) {
                                    if (std::hypot(wx - mp.first, wy - mp.second) <= 1.5 ||
                                        std::hypot(wx - mp.first, wy - std::fabs(mp.second)) <= 1.5) {
                                        keepWire = true;
                                        break;
                                    }
                                }
                            }
                            if (keepWire) break;
                        }
                    }
                    if (keepWire) {
                        filteredWires.push_back(w);
                    }
                }
                rpcResult["wires"] = std::move(filteredWires);
                rpcResult["wires_count"] = static_cast<int>(rpcResult["wires"].size());
            }

            if (!includeWires) {
                rpcResult.erase("wires");
            }
            if (summaryOnly) {
                rpcResult.erase("components");
                rpcResult.erase("wires");
                rpcResult.erase("netlist");
            }
        }
    } else if (toolName == "easyeda_run_drc" && rpcResult.is_object()) {
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
            if (rpcResult.contains("violations") && rpcResult["violations"].is_array()) {
                for (const auto& existingV : rpcResult["violations"]) {
                    if (existingV.is_object()) {
                        std::string vt = existingV.value("type", "");
                        if (vt == "unconnected_pin" || vt == "dangling_wire" ||
                            vt == "component_issue" || vt == "net_issue") {
                            continue;
                        }
                    }
                    violationsArr.push_back(existingV);
                }
            }
            if (ercAudit.contains("unconnected_pins") && ercAudit["unconnected_pins"].is_array()) {
                for (const auto& up : ercAudit["unconnected_pins"]) {
                    nlohmann::json item = up;
                    item["type"] = "unconnected_pin";
                    violationsArr.push_back(std::move(item));
                }
            }
            if (ercAudit.contains("dangling_wires") && ercAudit["dangling_wires"].is_array()) {
                for (const auto& dw : ercAudit["dangling_wires"]) {
                    nlohmann::json item = dw;
                    item["type"] = "dangling_wire";
                    violationsArr.push_back(std::move(item));
                }
            }
            if (ercAudit.contains("component_issues") && ercAudit["component_issues"].is_array()) {
                for (const auto& ci : ercAudit["component_issues"]) {
                    nlohmann::json item = ci;
                    item["type"] = "component_issue";
                    violationsArr.push_back(std::move(item));
                }
            }
            if (ercAudit.contains("net_issues") && ercAudit["net_issues"].is_array()) {
                for (const auto& ni : ercAudit["net_issues"]) {
                    nlohmann::json item = ni;
                    item["type"] = "net_issue";
                    violationsArr.push_back(std::move(item));
                }
            }
            rpcResult["violations"] = std::move(violationsArr);
            rpcResult["violation_count"] = static_cast<int>(rpcResult["violations"].size());
            if (!ercAudit.value("passed", true)) {
                rpcResult["passed"] = false;
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
                }
            }
            rpcResult["added_netflags"] = nfCount;
        }

        // 4. modify_components
        if (remoteArgs.contains("modify_components") && remoteArgs["modify_components"].is_array() &&
            !rpcResult.contains("modified_components")) {
            int modCompCount = 0;
            for (const auto& mc : remoteArgs["modify_components"]) {
                if (!mc.is_object()) continue;
                std::string compId = mc.value("id", mc.value("primitive_id", ""));
                if (compId.empty()) continue;
                nlohmann::json modProps = nlohmann::json::object();
                if (mc.contains("reference") || mc.contains("designator")) {
                    modProps["designator"] = mc.value("reference", mc.value("designator", ""));
                }
                if (mc.contains("value") || mc.contains("name")) {
                    modProps["name"] = mc.value("value", mc.value("name", ""));
                }
                if (mc.contains("unique_id") || mc.contains("uniqueId")) {
                    modProps["uniqueId"] = mc.value("unique_id", mc.value("uniqueId", ""));
                }
                if (mc.contains("footprint")) {
                    modProps["footprint"] = mc["footprint"];
                }
                if (mc.contains("add_into_bom")) {
                    modProps["addIntoBom"] = mc.value("add_into_bom", true);
                }
                if (mc.contains("add_into_pcb")) {
                    modProps["addIntoPcb"] = mc.value("add_into_pcb", true);
                }
                nlohmann::json apiRes;
                if (invokeEasyEdaApi("eda.sch_PrimitiveComponent.modify",
                                     nlohmann::json::array({compId, modProps}),
                                     apiRes)) {
                    ++modCompCount;
                }
            }
            rpcResult["modified_components"] = modCompCount;
        }

        // 5. add_components fallback if remote v1.1.3 extension did not place or lacked libraryUuid/modify
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
                    if ((compUuid.empty() || libUuid.empty()) && !searchKey.empty()) {
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
                    double xVal = scaleCoord(ac.value("x", ac.value("x_mm", 0.0)));
                    double yVal = scaleCoord(ac.value("y", ac.value("y_mm", 0.0)));
                    double rotVal = ac.value("rotation", ac.value("rotation_deg", 0.0));
                    bool mirrorVal = ac.value("mirror", false);
                    nlohmann::json devObj = libUuid.empty()
                        ? nlohmann::json(compUuid)
                        : nlohmann::json({{"uuid", compUuid}, {"libraryUuid", libUuid}});
                    nlohmann::json createRes;
                    if (invokeEasyEdaApi("eda.sch_PrimitiveComponent.create",
                                         nlohmann::json::array({devObj, xVal, yVal, nullptr, rotVal, mirrorVal}),
                                         createRes)) {
                        std::string createdId;
                        if (createRes.contains("result")) {
                            if (createRes["result"].is_string()) {
                                createdId = createRes["result"].get<std::string>();
                            } else if (createRes["result"].is_object()) {
                                createdId = createRes["result"].value(
                                    "primitiveId", createRes["result"].value("id", ""));
                            }
                        }
                        std::string ref = ac.value("reference", ac.value("ref", ""));
                        std::string val = ac.value("value", "");
                        std::string uid = ac.value("unique_id", ac.value("uniqueId", ""));
                        if (!createdId.empty() && (!ref.empty() || !val.empty() || !uid.empty())) {
                            nlohmann::json modProps = nlohmann::json::object();
                            if (!ref.empty()) modProps["designator"] = ref;
                            if (!val.empty()) modProps["name"] = val;
                            if (!uid.empty()) modProps["uniqueId"] = uid;
                            nlohmann::json modRes;
                            invokeEasyEdaApi("eda.sch_PrimitiveComponent.modify",
                                             nlohmann::json::array({createdId, modProps}),
                                             modRes);
                        }
                        detailsArr.push_back({
                            {"reference", ref},
                            {"id", createdId},
                            {"x", xVal},
                            {"y", yVal},
                            {"rotation", rotVal}
                        });
                        ++placedCount;
                    }
                }
                rpcResult["placed_components"] = placedCount;
                rpcResult["placed_component_details"] = std::move(detailsArr);
            }
        }
    } else if (toolName == "easyeda_inspect_pcb" && rpcResult.is_object()) {
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

        EdaAccelerators accCopy;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            accCopy = _accelerators;
        }
        bool auditOk = false;
        std::string auditSummary;
        rpcResult["connectivity_audit"] = accCopy.calcPcbConnectivityAudit(rpcResult, auditOk, auditSummary);

        std::set<std::string> refSet;
        if (arguments.is_object() && arguments.contains("references") && arguments["references"].is_array()) {
            for (const auto& r : arguments["references"]) {
                if (r.is_string() && !r.get<std::string>().empty()) {
                    refSet.insert(r.get<std::string>());
                }
            }
        }
        std::set<std::string> netSet;
        if (arguments.is_object() && arguments.contains("nets") && arguments["nets"].is_array()) {
            for (const auto& n : arguments["nets"]) {
                if (n.is_string() && !n.get<std::string>().empty()) {
                    netSet.insert(n.get<std::string>());
                }
            }
        }
        bool summaryOnly = arguments.is_object() ? arguments.value("summary_only", false) : false;
        bool hasFilter = (!refSet.empty() || !netSet.empty());

        if (hasFilter) {
            std::set<std::string> effectiveNetSet = netSet;
            std::set<std::string> effectiveRefSet = refSet;
            if (rpcResult.contains("pads") && rpcResult["pads"].is_array()) {
                for (const auto& p : rpcResult["pads"]) {
                    if (!p.is_object()) continue;
                    std::string pr = p.value("ref", p.value("reference", ""));
                    std::string pn = p.value("net", "");
                    if (!refSet.empty() && refSet.count(pr) > 0 && netSet.empty() && !pn.empty()) {
                        effectiveNetSet.insert(pn);
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
                    if (effectiveRefSet.count(cr) > 0) {
                        fComps.push_back(c);
                    }
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
                    if (matchRef || matchNet) {
                        fPads.push_back(p);
                    }
                }
                rpcResult["pads"] = std::move(fPads);
                rpcResult["pads_count"] = static_cast<int>(rpcResult["pads"].size());
            }

            if (rpcResult.contains("tracks") && rpcResult["tracks"].is_array()) {
                nlohmann::json fTracks = nlohmann::json::array();
                for (const auto& t : rpcResult["tracks"]) {
                    if (!t.is_object()) continue;
                    if (effectiveNetSet.count(t.value("net", "")) > 0) {
                        fTracks.push_back(t);
                    }
                }
                rpcResult["tracks"] = std::move(fTracks);
                rpcResult["tracks_count"] = static_cast<int>(rpcResult["tracks"].size());
            }

            if (rpcResult.contains("vias") && rpcResult["vias"].is_array()) {
                nlohmann::json fVias = nlohmann::json::array();
                for (const auto& v : rpcResult["vias"]) {
                    if (!v.is_object()) continue;
                    if (effectiveNetSet.count(v.value("net", "")) > 0) {
                        fVias.push_back(v);
                    }
                }
                rpcResult["vias"] = std::move(fVias);
                rpcResult["vias_count"] = static_cast<int>(rpcResult["vias"].size());
            }

            if (rpcResult.contains("pours") && rpcResult["pours"].is_array()) {
                nlohmann::json fPours = nlohmann::json::array();
                for (const auto& pr : rpcResult["pours"]) {
                    if (!pr.is_object()) continue;
                    if (effectiveNetSet.count(pr.value("net", "")) > 0) {
                        fPours.push_back(pr);
                    }
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
    } else if (toolName == "easyeda_edit_pcb" && rpcResult.is_object()) {
        std::string unitsStr = remoteArgs.value("units", "mil");
        rpcResult["units"] = unitsStr;
        auto scaleCoord = [&](double val) -> double {
            if (unitsStr == "mm" || unitsStr == "MM") {
                return std::round(val * 39.37007874 * 1000.0) / 1000.0;
            }
            return val;
        };

        // 1. delete_primitives
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
                }
                ++delCount;
            }
            rpcResult["deleted_primitives"] = delCount;
        }

        // 2. modify_pads
        if (remoteArgs.contains("modify_pads") && remoteArgs["modify_pads"].is_array() &&
            !rpcResult.contains("modified_pads")) {
            int modPadsCount = 0;
            for (const auto& mp : remoteArgs["modify_pads"]) {
                if (!mp.is_object()) continue;
                std::string padId = mp.value("id", mp.value("pad_id", ""));
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
            !rpcResult.contains("placed_components")) {
            int placedCompCount = 0;
            for (const auto& pc : remoteArgs["place_components"]) {
                if (!pc.is_object()) continue;
                std::string compUuid = pc.value("uuid", pc.value("footprint_uuid", ""));
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
                            compUuid = hit.value("footprintUuid", hit.value("uuid", ""));
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
                    if (createRes.contains("result")) {
                        if (createRes["result"].is_string()) {
                            createdId = createRes["result"].get<std::string>();
                        } else if (createRes["result"].is_object()) {
                            createdId = createRes["result"].value(
                                "primitiveId", createRes["result"].value("id", ""));
                        }
                    }
                    std::string ref = pc.value("reference", pc.value("ref", ""));
                    if (!createdId.empty() && !ref.empty()) {
                        nlohmann::json modRes;
                        invokeEasyEdaApi("eda.pcb_PrimitiveComponent.modify",
                                         nlohmann::json::array({createdId, {{"designator", ref}}}),
                                         modRes);
                    }
                    ++placedCompCount;
                }
            }
            rpcResult["placed_components"] = placedCompCount;
        }
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
