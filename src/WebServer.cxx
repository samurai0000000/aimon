/*
 * WebServer.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "WebServer.hxx"
#include "WebAssets.hxx"
#include "McpServer.hxx"
#include "TaskRegistry.hxx"
#include "TcpGateway.hxx"
#include "AgentTelemetryDb.hxx"
#include "MobileGateway.hxx"
#include "PathUtils.hxx"
#include "InsightEngine.hxx"
#include "EdaBridge.hxx"
#include "Version.hxx"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <random>
#include <sstream>
#include <iomanip>
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#ifndef CPPHTTPLIB_THREAD_POOL_COUNT
#define CPPHTTPLIB_THREAD_POOL_COUNT 8
#endif
#include <httplib.h>

#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sqlite3.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>

namespace fs = std::filesystem;

namespace aimon {

static std::mutex s_cachedCliTokensMutex;
static std::map<int, std::string> s_cachedCliTokens;
static std::atomic<int64_t> s_lastProcHarvestSec{0};

struct AgyProcessInfo {
    pid_t pid = 0;
    std::string pidStr;
    std::string cwd;
    std::string lockedConversationId;
    std::string terminalPts;
    int httpPort = 0;
    int httpsPort = 0;
};

static std::string getAgyEndpointsStatePath() {
    const char* envEp = std::getenv("AIMON_AGY_ENDPOINTS_FILE");
    if (envEp && *envEp) {
        return std::string(envEp);
    }
    return PathUtils::expandHome("~/.cursor/hooks/state/agy_ls_endpoints.json");
}

static std::set<int> scanListeningTcpPorts() {
    std::set<int> ports;
    auto scanNetFile = [&](const std::string& netPath) {
        std::ifstream nf(netPath);
        if (!nf.is_open()) return;
        std::string line;
        std::getline(nf, line);
        while (std::getline(nf, line)) {
            std::istringstream ss(line);
            std::string sl, localAddr, remAddr, state;
            if (ss >> sl >> localAddr >> remAddr >> state) {
                if (state == "0A") {
                    auto colon = localAddr.find(':');
                    if (colon != std::string::npos) {
                        try {
                            int p = std::stoi(localAddr.substr(colon + 1), nullptr, 16);
                            if (p > 0 && p <= 65535) {
                                ports.insert(p);
                            }
                        } catch (...) {}
                    }
                }
            }
        }
    };
    scanNetFile("/proc/net/tcp");
    scanNetFile("/proc/net/tcp6");
    return ports;
}

static bool writeJsonFileAtomically(const std::string& targetPath, const nlohmann::json& j) {
    try {
        std::error_code ec;
        fs::path tp(targetPath);
        if (tp.has_parent_path()) {
            fs::create_directories(tp.parent_path(), ec);
        }
        std::string tmpPath = targetPath + ".tmp." + std::to_string(getpid());
        {
            std::ofstream of(tmpPath, std::ios::trunc | std::ios::binary);
            if (!of.is_open()) {
                return false;
            }
            of << j.dump(2);
            of.flush();
            if (!of.good()) {
                of.close();
                fs::remove(tmpPath, ec);
                return false;
            }
        }
        chmod(tmpPath.c_str(), 0600);
        fs::rename(tmpPath, targetPath, ec);
        if (ec) {
            fs::remove(tmpPath, ec);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

static std::vector<AgyProcessInfo> discoverRunningAgyProcesses() {
    std::vector<AgyProcessInfo> result;
    const char* envProcsFile = std::getenv("AIMON_AGY_PROCESSES_FILE");
    if (envProcsFile && *envProcsFile) {
        try {
            std::ifstream pf(envProcsFile);
            if (pf.is_open()) {
                auto pj = nlohmann::json::parse(pf, nullptr, false);
                const nlohmann::json* arr = nullptr;
                if (!pj.is_discarded()) {
                    if (pj.is_array()) {
                        arr = &pj;
                    } else if (pj.is_object() && pj.contains("processes") && pj["processes"].is_array()) {
                        arr = &pj["processes"];
                    }
                }
                if (arr) {
                    for (const auto& item : *arr) {
                        if (!item.is_object()) continue;
                        AgyProcessInfo info;
                        if (item.contains("pid")) {
                            if (item["pid"].is_number_integer()) {
                                info.pid = item["pid"].get<int>();
                                info.pidStr = std::to_string(info.pid);
                            } else if (item["pid"].is_string()) {
                                info.pidStr = item["pid"].get<std::string>();
                                try { info.pid = std::stoi(info.pidStr); } catch (...) {}
                            }
                        }
                        if (item.contains("pidStr") && item["pidStr"].is_string()) {
                            info.pidStr = item["pidStr"].get<std::string>();
                        }
                        info.cwd = item.value("cwd", "");
                        info.lockedConversationId = item.value("lockedConversationId", "");
                        info.terminalPts = item.value("terminalPts", "");
                        info.httpPort = item.value("httpPort", 0);
                        info.httpsPort = item.value("httpsPort", 0);
                        result.push_back(std::move(info));
                    }
                }
            }
        } catch (...) {}
        return result;
    }

    std::error_code ec;
    if (!fs::exists("/proc", ec)) {
        return result;
    }

    for (const auto& entry : fs::directory_iterator("/proc", ec)) {
        if (ec || !entry.is_directory()) continue;
        std::string dname = entry.path().filename().string();
        if (dname.empty() || !std::all_of(dname.begin(), dname.end(), ::isdigit)) {
            continue;
        }
        pid_t pid = 0;
        try {
            pid = std::stoi(dname);
        } catch (...) {
            continue;
        }
        if (pid <= 0) continue;

        std::ifstream commFile("/proc/" + dname + "/comm");
        if (!commFile.is_open()) continue;
        std::string comm;
        std::getline(commFile, comm);
        while (!comm.empty() && (comm.back() == '\r' || comm.back() == '\n' || comm.back() == ' ')) {
            comm.pop_back();
        }
        if (comm != "agy") continue;

        AgyProcessInfo info;
        info.pid = pid;
        info.pidStr = dname;

        std::error_code symEc;
        auto cwdPath = fs::read_symlink("/proc/" + dname + "/cwd", symEc);
        if (!symEc) {
            info.cwd = cwdPath.string();
        }

        auto fd0Path = fs::read_symlink("/proc/" + dname + "/fd/0", symEc);
        if (!symEc) {
            std::string fd0Str = fd0Path.string();
            auto ptsPos = fd0Str.find("pts/");
            if (ptsPos != std::string::npos) {
                info.terminalPts = fd0Str.substr(ptsPos);
            }
        }

        std::string fdDir = "/proc/" + dname + "/fd";
        std::error_code fdEc;
        std::string cliLogPath;
        if (fs::exists(fdDir, fdEc)) {
            for (const auto& fdEntry : fs::directory_iterator(fdDir, fdEc)) {
                if (fdEc) break;
                std::error_code linkEc;
                if (!fs::is_symlink(fdEntry.path(), linkEc) || linkEc) continue;
                std::string target = fs::read_symlink(fdEntry.path(), linkEc).string();
                if (linkEc || target.empty()) continue;

                auto presPos = target.find("/presence/");
                if (presPos != std::string::npos && target.size() > presPos + 15 &&
                    target.compare(target.size() - 5, 5, ".lock") == 0) {
                    std::string lockName = target.substr(presPos + 10);
                    if (lockName.size() > 5) {
                        info.lockedConversationId = lockName.substr(0, lockName.size() - 5);
                    }
                } else if (cliLogPath.empty() && target.find("/log/cli-") != std::string::npos &&
                           target.size() > 4 && target.compare(target.size() - 4, 4, ".log") == 0) {
                    cliLogPath = target;
                }
            }
        }

        if (!cliLogPath.empty()) {
            std::ifstream lf(cliLogPath);
            std::string lline;
            int scanned = 0;
            while (std::getline(lf, lline) && scanned < 200) {
                ++scanned;
                auto portPos = lline.find("port at ");
                if (portPos != std::string::npos) {
                    size_t numStart = portPos + 8;
                    size_t numEnd = lline.find(' ', numStart);
                    if (numEnd != std::string::npos && numEnd > numStart) {
                        int parsedPort = 0;
                        try {
                            parsedPort = std::stoi(lline.substr(numStart, numEnd - numStart));
                        } catch (...) {
                            parsedPort = 0;
                        }
                        if (parsedPort > 0 && parsedPort <= 65535) {
                            if (lline.find(" for HTTPS") != std::string::npos) {
                                info.httpsPort = parsedPort;
                            } else if (lline.find(" for HTTP") != std::string::npos) {
                                info.httpPort = parsedPort;
                            }
                        }
                    }
                }
                if (info.httpPort > 0 && info.httpsPort > 0) {
                    break;
                }
            }
        }

        result.push_back(std::move(info));
    }

    return result;
}

static std::map<int, std::pair<std::string, std::string>> syncAndPruneCliEndpointsState(
    const std::vector<AgyProcessInfo>& agyProcs,
    bool scanProcEnv) {
    std::map<int, std::pair<std::string, std::string>> outLiveEnvEndpoints;
    const char* envProcsFile = std::getenv("AIMON_AGY_PROCESSES_FILE");
    const char* envEpFile = std::getenv("AIMON_AGY_ENDPOINTS_FILE");
    bool hasProcsOverride = (envProcsFile && *envProcsFile);
    bool hermeticTestMode = hasProcsOverride || (envEpFile && *envEpFile);

    std::string epStatePath = getAgyEndpointsStatePath();
    std::lock_guard<std::mutex> lk(s_cachedCliTokensMutex);

    if (hermeticTestMode) {
        s_cachedCliTokens.clear();
    }

    nlohmann::json ej = {{"endpoints", nlohmann::json::object()}};
    std::map<int, int64_t> fileUpdatedAt;
    if (fs::exists(epStatePath)) {
        try {
            std::ifstream ef(epStatePath);
            auto parsed = nlohmann::json::parse(ef, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()) {
                ej = std::move(parsed);
            }
        } catch (...) {}
    }
    if (!ej.contains("endpoints") || !ej["endpoints"].is_object()) {
        ej["endpoints"] = nlohmann::json::object();
    }

    for (auto it = ej["endpoints"].begin(); it != ej["endpoints"].end(); ++it) {
        if (!it.value().is_object()) continue;
        int p = it.value().value("port", 0);
        if (p <= 0) {
            try { p = std::stoi(it.key()); } catch (...) { p = 0; }
        }
        std::string tok = it.value().value("csrf_token", "");
        int64_t upd = it.value().value("updated_at", static_cast<int64_t>(0));
        if (p > 0 && p <= 65535 && !tok.empty()) {
            s_cachedCliTokens[p] = tok;
            fileUpdatedAt[p] = upd;
        }
    }

    bool stateChanged = false;
    if (scanProcEnv && !hasProcsOverride && fs::exists("/proc")) {
        std::vector<pid_t> procPids;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator("/proc", ec)) {
            if (ec || !entry.is_directory()) continue;
            std::string dname = entry.path().filename().string();
            if (!dname.empty() && std::all_of(dname.begin(), dname.end(), ::isdigit)) {
                try { procPids.push_back(std::stoi(dname)); } catch (...) {}
            }
        }
        std::sort(procPids.begin(), procPids.end(), std::greater<pid_t>());

        for (pid_t pid : procPids) {
            std::ifstream envFile("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
            if (!envFile.is_open()) continue;
            std::string envContent((std::istreambuf_iterator<char>(envFile)),
                                   std::istreambuf_iterator<char>());
            if (envContent.find("ANTIGRAVITY_LS_ADDRESS=") == std::string::npos) continue;
            std::string lsAddr;
            std::string lsCsrf;
            size_t pos = 0;
            while (pos < envContent.size()) {
                std::string item = envContent.c_str() + pos;
                if (item.rfind("ANTIGRAVITY_LS_ADDRESS=", 0) == 0) {
                    lsAddr = item.substr(23);
                } else if (item.rfind("ANTIGRAVITY_CSRF_TOKEN=", 0) == 0) {
                    lsCsrf = item.substr(23);
                }
                pos += item.length() + 1;
            }
            if (!lsAddr.empty() && !lsCsrf.empty()) {
                auto colon = lsAddr.rfind(':');
                if (colon != std::string::npos) {
                    std::string cwdStr;
                    std::error_code cwdEc;
                    auto cwdPath = fs::read_symlink("/proc/" + std::to_string(pid) + "/cwd", cwdEc);
                    if (!cwdEc) {
                        cwdStr = cwdPath.string();
                    }
                    try {
                        int p = std::stoi(lsAddr.substr(colon + 1));
                        if (p > 0 && p <= 65535) {
                            if (s_cachedCliTokens[p] != lsCsrf) {
                                s_cachedCliTokens[p] = lsCsrf;
                                stateChanged = true;
                            }
                            outLiveEnvEndpoints[p] = {lsCsrf, cwdStr};
                        }
                    } catch (...) {}
                }
            }
        }
    }

    std::set<int> livePorts;
    for (const auto& proc : agyProcs) {
        if (proc.httpPort > 0) livePorts.insert(proc.httpPort);
        if (proc.httpsPort > 0) livePorts.insert(proc.httpsPort);
    }
    for (const auto& kv : outLiveEnvEndpoints) {
        if (kv.first > 0) livePorts.insert(kv.first);
    }
    std::set<int> tcpListening = scanListeningTcpPorts();
    if (!hasProcsOverride) {
        for (int p : tcpListening) {
            livePorts.insert(p);
        }
    }

    int64_t nowSec = static_cast<int64_t>(std::time(nullptr));
    if (!livePorts.empty() || hasProcsOverride) {
        for (auto it = s_cachedCliTokens.begin(); it != s_cachedCliTokens.end(); ) {
            int p = it->first;
            bool isAlive = (livePorts.count(p) > 0);
            if (!isAlive && !hasProcsOverride) {
                auto updIt = fileUpdatedAt.find(p);
                if (updIt != fileUpdatedAt.end() && updIt->second > 0 && (nowSec - updIt->second) < 15) {
                    isAlive = true;
                }
            }
            if (!isAlive) {
                it = s_cachedCliTokens.erase(it);
                stateChanged = true;
            } else {
                ++it;
            }
        }
        for (auto it = ej["endpoints"].begin(); it != ej["endpoints"].end(); ) {
            int p = 0;
            if (it.value().is_object()) {
                p = it.value().value("port", 0);
            }
            if (p <= 0) {
                try { p = std::stoi(it.key()); } catch (...) { p = 0; }
            }
            if (p <= 0 || s_cachedCliTokens.find(p) == s_cachedCliTokens.end()) {
                it = ej["endpoints"].erase(it);
                stateChanged = true;
            } else {
                ++it;
            }
        }
    }

    std::map<int, const AgyProcessInfo*> agyByPort;
    for (const auto& proc : agyProcs) {
        if (proc.httpPort > 0) {
            agyByPort[proc.httpPort] = &proc;
        }
    }

    for (const auto& tkv : s_cachedCliTokens) {
        std::string key = std::to_string(tkv.first);
        if (!ej["endpoints"].contains(key) || !ej["endpoints"][key].is_object()) {
            ej["endpoints"][key] = {
                {"port", tkv.first},
                {"ls_address", "localhost:" + key},
                {"csrf_token", tkv.second},
                {"updated_at", static_cast<long long>(nowSec)}
            };
            stateChanged = true;
        } else {
            auto& entryObj = ej["endpoints"][key];
            if (entryObj.value("port", 0) != tkv.first ||
                entryObj.value("csrf_token", "") != tkv.second) {
                entryObj["port"] = tkv.first;
                entryObj["csrf_token"] = tkv.second;
                entryObj["updated_at"] = static_cast<long long>(nowSec);
                stateChanged = true;
            }
        }
        auto& entryObj = ej["endpoints"][key];
        auto pIt = agyByPort.find(tkv.first);
        if (pIt != agyByPort.end()) {
            if (!pIt->second->lockedConversationId.empty() &&
                entryObj.value("conversation_id", "") != pIt->second->lockedConversationId) {
                entryObj["conversation_id"] = pIt->second->lockedConversationId;
                stateChanged = true;
            }
            if (!pIt->second->cwd.empty() && entryObj.value("workspace", "").empty()) {
                entryObj["workspace"] = pIt->second->cwd;
                stateChanged = true;
            }
        }
        auto envIt = outLiveEnvEndpoints.find(tkv.first);
        if (envIt != outLiveEnvEndpoints.end() && !envIt->second.second.empty() &&
            entryObj.value("workspace", "").empty()) {
            entryObj["workspace"] = envIt->second.second;
            stateChanged = true;
        }
    }

    if (stateChanged) {
        writeJsonFileAtomically(epStatePath, ej);
    }
    return outLiveEnvEndpoints;
}

static std::string getPrimaryLanIp() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return "";
    }
    std::string primaryIp;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            if (!(ifa->ifa_flags & IFF_LOOPBACK) && (ifa->ifa_flags & IFF_UP)) {
                char ipStr[INET_ADDRSTRLEN];
                void* sin_addr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
                if (inet_ntop(AF_INET, sin_addr, ipStr, sizeof(ipStr))) {
                    std::string ip(ipStr);
                    if (ip.rfind("192.168.", 0) == 0 || ip.rfind("10.", 0) == 0 || ip.rfind("172.", 0) == 0) {
                        primaryIp = ip;
                        break;
                    }
                    if (primaryIp.empty()) {
                        primaryIp = ip;
                    }
                }
            }
        }
    }
    freeifaddrs(ifaddr);
    return primaryIp;
}

static std::string generateSessionId() {
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    static thread_local std::uniform_int_distribution<uint64_t> dis;
    uint64_t part1 = dis(gen);
    uint64_t part2 = dis(gen);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << part1 << std::setw(16) << part2;
    return oss.str();
}

WebServer::WebServer(StateStore& stateStore, HistoryStore& historyStore,
                     const WebConfig& config, RefreshCallback onRefresh,
                     McpServer* mcpServer,
                     TcpGateway* tcpGateway)
    : _stateStore(stateStore),
      _historyStore(historyStore),
      _config(config),
      _onRefresh(onRefresh),
      _mcpServer(mcpServer),
      _tcpGateway(tcpGateway),
      _mobileGateway(&MobileGateway::getInstance()),
      _endpointsEnabled(config.endpointsEnabled),
      _server(std::make_unique<httplib::Server>()) {
}

WebServer::~WebServer() {
    stop();
}

std::string WebServer::createUiSession() {
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    static thread_local std::uniform_int_distribution<uint64_t> dis;

    uint64_t p1 = dis(gen);
    uint64_t p2 = dis(gen);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << p1 << std::setw(16) << p2;
    std::string token = oss.str();

    std::lock_guard<std::mutex> lock(_sessionMutex);
    time_t now = time(nullptr);
    for (auto it = _uiSessions.begin(); it != _uiSessions.end(); ) {
        if (now - it->second > 86400 * 7) {
            it = _uiSessions.erase(it);
        } else {
            ++it;
        }
    }
    _uiSessions[token] = now;
    return token;
}

std::string WebServer::extractMobileToken(const httplib::Request& req) {
    if (req.has_header("X-Mobile-Token")) {
        std::string token = req.get_header_value("X-Mobile-Token");
        while (!token.empty() && (token.back() == '\r' || token.back() == '\n' || token.back() == ' ')) {
            token.pop_back();
        }
        if (!token.empty()) {
            return token;
        }
    }
    if (req.has_header("Authorization")) {
        std::string auth = req.get_header_value("Authorization");
        if (auth.size() > 7) {
            std::string prefix = auth.substr(0, 7);
            for (char &c : prefix) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (prefix == "bearer ") {
                std::string token = auth.substr(7);
                size_t first = token.find_first_not_of(" \t\r\n");
                if (first != std::string::npos) {
                    size_t last = token.find_last_not_of(" \t\r\n");
                    return token.substr(first, last - first + 1);
                }
            }
        }
    }
    return "";
}

bool WebServer::isValidMobileToken(const httplib::Request& req) const {
    std::string token = extractMobileToken(req);
    if (token.empty() || !_mobileGateway) {
        return false;
    }
    return _mobileGateway->validateToken(token);
}

bool WebServer::isLoopbackRequest(const httplib::Request& req) {
    return req.remote_addr == "127.0.0.1" ||
           req.remote_addr == "::1" ||
           req.remote_addr == "::ffff:127.0.0.1";
}

bool WebServer::isValidUiSession(const httplib::Request& req) const {
    // 1. Check explicit Web UI session token via header or cookie
    std::string token;
    if (req.has_header("X-UI-Session")) {
        token = req.get_header_value("X-UI-Session");
    } else if (req.has_header("Cookie")) {
        std::string cookie = req.get_header_value("Cookie");
        size_t pos = cookie.find("aimon_session=");
        if (pos != std::string::npos) {
            size_t start = pos + 14;
            size_t end = cookie.find(';', start);
            token = (end == std::string::npos) ? cookie.substr(start) : cookie.substr(start, end - start);
        }
    }

    if (!token.empty()) {
        std::lock_guard<std::mutex> lock(_sessionMutex);
        auto it = _uiSessions.find(token);
        if (it != _uiSessions.end()) {
            time_t now = time(nullptr);
            if (now - it->second < 86400 * 7) {
                return true;
            }
        }
    }

    // 2. Allow requests with valid paired mobile authentication tokens
    if (isValidMobileToken(req)) {
        return true;
    }

    // 3. Allow internal telemetry hook
    if (req.has_header("User-Agent")) {
        std::string ua = req.get_header_value("User-Agent");
        if (ua.find("aimon-hook") != std::string::npos && isLoopbackRequest(req)) {
            return true;
        }
    }

    return false;
}

bool WebServer::isValidMobileOrUiRequest(const httplib::Request& req) const {
    return isValidMobileToken(req) || isValidUiSession(req);
}

std::string WebServer::getMcpHintForPath(const std::string& path, const std::string& body) {
    (void)body;
    if (path == "/api/status" || path == "/api/history") {
        return "Use official MCP tool 'get_combined_ai_status', 'check_antigravity_quota', or 'check_cursor_usage'.";
    }
    return "Use official MCP tools ('aimon' gateway or native tools). Direct REST API access is disabled.";
}

void WebServer::setupRoutes() {
    // Connect MobileGateway broadcast callback to broadcast SSE events
    if (_mobileGateway) {
        _mobileGateway->setBroadcastCallback([this](const std::string& type, const nlohmann::json& payload) {
            nlohmann::json evt;
            evt["type"] = type;
            evt["payload"] = payload;
            broadcastSseNotification(evt.dump());
        });
    }

    // Enforce authentication on mobile, approval, agent, and workspace routes,
    // and gate direct REST API endpoints when disabled by configuration.
    _server->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (req.method == "OPTIONS") {
            return httplib::Server::HandlerResponse::Unhandled;
        }

        // Pairing endpoint exchanges a one-time pairing secret for a token
        if (req.path == "/api/mobile/pair") {
            return httplib::Server::HandlerResponse::Unhandled;
        }

        // QR generation requires authenticated Web UI session or loopback
        if (req.path == "/api/mobile/qr") {
            if (!isValidUiSession(req) && !isLoopbackRequest(req)) {
                res.status = 401;
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        }

        // Protected mobile, agent, workspace, and EDA mutation endpoints require a valid token or UI session
        if (req.path == "/api/mobile/devices" ||
            req.path == "/api/mobile/revoke" ||
            req.path == "/api/agent" ||
            req.path.rfind("/api/agent/", 0) == 0 ||
            req.path == "/api/workspace" ||
            req.path.rfind("/api/workspace/", 0) == 0 ||
            req.path == "/api/eda/config" ||
            req.path == "/api/eda/probe") {
            if (!isValidMobileOrUiRequest(req)) {
                res.status = 401;
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        }

        // Mobile SSE push stream requires a valid mobile token or UI session
        if (req.path == "/sse") {
            std::string profile;
            if (req.has_param("profile")) {
                profile = req.get_param_value("profile");
            } else if (req.has_header("X-Aimon-Profile")) {
                profile = req.get_header_value("X-Aimon-Profile");
            }
            if (profile == "mobile" && !isValidMobileOrUiRequest(req)) {
                res.status = 401;
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        }

        if (!_endpointsEnabled) {
            if (req.path == "/api/monitors" ||
                req.path == "/api/status" ||
                req.path == "/api/health" ||
                req.path.rfind("/api/telemetry", 0) == 0 ||
                req.path.rfind("/api/v1/insights", 0) == 0) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            if (req.path == "/api" || req.path.rfind("/api/", 0) == 0) {
                if (!isValidUiSession(req)) {
                    res.status = 403;
                    std::string hint = getMcpHintForPath(req.path, req.body);
                    nlohmann::json err = {
                        {"error", "Direct REST API endpoint access is disabled by configuration."},
                        {"hint", hint},
                        {"daemon", "aimon"}
                    };
                    res.set_content(err.dump(2), "application/json");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });


    auto serveFileOrFallback = [](const std::string& diskPath,
                                  const char* fallbackAsset,
                                  const std::string& contentType,
                                  httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
        if (fs::exists(diskPath)) {
            std::ifstream f(diskPath, std::ios::binary);
            if (f.is_open()) {
                std::string content((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
                res.set_content(content, contentType.c_str());
                return;
            }
        }
        if (fallbackAsset && fallbackAsset[0] != '\0') {
            res.set_content(fallbackAsset, contentType.c_str());
        } else {
            res.status = 404;
            res.set_content("File not found", "text/plain");
        }
    };

    _server->Get("/", [this, serveFileOrFallback](const httplib::Request&, httplib::Response& res) {
        std::string token = createUiSession();
        res.set_header("Set-Cookie", "aimon_session=" + token + "; Path=/; SameSite=Strict");
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");

        std::string html;
        if (fs::exists("web/index.html")) {
            std::ifstream f("web/index.html");
            if (f.is_open()) {
                html = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            }
        }
        if (html.empty()) {
            html = assets::INDEX_HTML;
        }

        std::string sessionScript = "<script>\n"
            "window.__UI_SESSION_TOKEN__ = \"" + token + "\";\n"
            "(function() {\n"
            "    const originalFetch = window.fetch;\n"
            "    window.fetch = function(url, options) {\n"
            "        options = options || {};\n"
            "        options.headers = options.headers || {};\n"
            "        if (window.__UI_SESSION_TOKEN__) {\n"
            "            if (options.headers instanceof Headers) {\n"
            "                options.headers.set('X-UI-Session', window.__UI_SESSION_TOKEN__);\n"
            "            } else if (Array.isArray(options.headers)) {\n"
            "                options.headers.push(['X-UI-Session', window.__UI_SESSION_TOKEN__]);\n"
            "            } else {\n"
            "                options.headers['X-UI-Session'] = window.__UI_SESSION_TOKEN__;\n"
            "            }\n"
            "        }\n"
            "        return originalFetch(url, options);\n"
            "    };\n"
            "})();\n"
            "</script>\n";

        size_t headPos = html.find("</head>");
        if (headPos != std::string::npos) {
            html.insert(headPos, sessionScript);
        } else {
            html = sessionScript + html;
        }

        res.set_content(html, "text/html");
    });

    _server->Get("/style.css", [serveFileOrFallback](const httplib::Request&, httplib::Response& res) {
        serveFileOrFallback("web/style.css", assets::STYLE_CSS, "text/css", res);
    });

    _server->Get("/app.js", [serveFileOrFallback](const httplib::Request&, httplib::Response& res) {
        serveFileOrFallback("web/app.js", assets::APP_JS, "application/javascript", res);
    });

    _server->Get("/qrcode.js", [serveFileOrFallback](const httplib::Request&, httplib::Response& res) {
        serveFileOrFallback("web/qrcode.js", assets::QRCODE_JS, "application/javascript", res);
    });

    _server->Get("/download/aimon-companion.apk", [serveFileOrFallback](const httplib::Request&, httplib::Response& res) {
        res.set_header("Content-Disposition", "attachment; filename=\"aimon-companion.apk\"");
        if (fs::exists("third_party/antimatter/android/app/build/outputs/apk/debug/app-debug.apk")) {
            serveFileOrFallback("third_party/antimatter/android/app/build/outputs/apk/debug/app-debug.apk", "", "application/vnd.android.package-archive", res);
        } else {
            serveFileOrFallback("mobile/android/app/build/outputs/apk/debug/app-debug.apk", "", "application/vnd.android.package-archive", res);
        }
    });

    _server->Get("/favicon.ico", [](const httplib::Request&, httplib::Response& res) {
        static const std::string faviconSvg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\">"
            "<circle cx=\"16\" cy=\"16\" r=\"14\" fill=\"#0a0e17\" stroke=\"#00f2fe\" stroke-width=\"2\"/>"
            "<circle cx=\"16\" cy=\"16\" r=\"6\" fill=\"#00f2fe\"/>"
            "<circle cx=\"16\" cy=\"16\" r=\"10\" fill=\"none\" stroke=\"#8b5cf6\" stroke-width=\"1\" stroke-dasharray=\"2,2\"/>"
            "</svg>";
        res.set_header("Cache-Control", "public, max-age=86400");
        res.set_content(faviconSvg, "image/svg+xml");
    });

    _server->Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json j = {
            {"status", "ok"},
            {"version", AIMON_VERSION_STRING},
            {"daemon", "aimon"}
        };
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content(j.dump(2), "application/json");
    });

    _server->Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        AggregateStatus st = _stateStore.getStatus();
        if (_edaBridge) {
            st.eda = _edaBridge->getStatus();
        }
        if (st.eda.configSnapshot.is_object()) {
            st.eda.configSnapshot["auth_secret"] = st.eda.authConfigured ? "***" : "";
        }
        std::string jsonStr = st.toJson().dump(2);
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
        res.set_content(jsonStr, "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
    });

    _server->Post("/api/refresh", [this](const httplib::Request&, httplib::Response& res) {
        if (_onRefresh) {
            try {
                _onRefresh();
            } catch (...) {
            }
        }
        AggregateStatus st = _stateStore.getStatus();
        if (_edaBridge) {
            st.eda = _edaBridge->getStatus();
        }
        if (st.eda.configSnapshot.is_object()) {
            st.eda.configSnapshot["auth_secret"] = st.eda.authConfigured ? "***" : "";
        }
        std::string jsonStr = st.toJson().dump(2);
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
        res.set_content(jsonStr, "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
    });

    _server->Get("/api/history", [this](const httplib::Request& req, httplib::Response& res) {
        int limit = 50;
        if (req.has_param("limit")) {
            try {
                limit = std::stoi(req.get_param_value("limit"));
            } catch (...) {}
        }
        auto records = _historyStore.queryRecentRecords(limit);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : records) {
            arr.push_back({
                {"id", r.id},
                {"timestamp", r.timestamp},
                {"provider", r.provider},
                {"metric_key", r.metricKey},
                {"metric_value", r.metricValue},
                {"metric_limit", r.metricLimit},
                {"delta", r.delta}
            });
        }
        res.set_content(arr.dump(2), "application/json");
        res.set_header("Access-Control-Allow-Origin", "*");
    });

    _server->Get("/api/sessions", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        auto sessions = TaskRegistry::getInstance().listSessions();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : sessions) {
            arr.push_back(s.toJson());
        }
        res.set_content(arr.dump(2), "application/json");
    });

    _server->Get("/api/monitors", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
        nlohmann::json arr = nlohmann::json::array();
        if (_tcpGateway) {
            auto monitors = _tcpGateway->getDiscoveredMonitors(_config.port);
            for (const auto& m : monitors) {
                arr.push_back(m.toJson());
            }
        } else {
            DiscoveredMonitor selfMon;
            selfMon.id = "aimon";
            selfMon.name = "AI Quotas";
            selfMon.shortName = "AI Quotas";
            selfMon.subsystem = "aimon";
            selfMon.host = "127.0.0.1";
            selfMon.port = _config.port;
            selfMon.path = "/";
            selfMon.connected = true;
            selfMon.reachable = true;
            selfMon.isSelf = true;
            selfMon.priority = 0;
            selfMon.lastSeenEpoch = std::time(nullptr);
            arr.push_back(selfMon.toJson());
        }
        res.set_content(arr.dump(2), "application/json");
    });

    // --- Telemetry Endpoints ---

    _server->Post("/api/telemetry/event", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            nlohmann::json body = nlohmann::json::parse(req.body);
            std::string eventType = body.value("event_type", "");
            std::string sessionId = body.value("session_id", "default");
            std::string agentType = body.value("agent_type", "antigravity");
            int64_t ts = body.value("timestamp", static_cast<int64_t>(time(nullptr)));

            bool recordedEndpointFromDetails = false;
            if (body.contains("details") && body["details"].is_object()) {
                const auto& det = body["details"];
                std::string lsAddr = det.value("ls_address", "");
                std::string csrfTok = det.value("csrf_token", "");
                if (!lsAddr.empty() && !csrfTok.empty()) {
                    auto colon = lsAddr.rfind(':');
                    std::string portPart = (colon != std::string::npos) ? lsAddr.substr(colon + 1) : lsAddr;
                    try {
                        int p = std::stoi(portPart);
                        if (p > 0 && p <= 65535) {
                            {
                                std::lock_guard<std::mutex> lk(s_cachedCliTokensMutex);
                                s_cachedCliTokens[p] = csrfTok;
                            }
                            recordedEndpointFromDetails = true;
                            std::string epStatePath = getAgyEndpointsStatePath();
                            nlohmann::json ej = {{"endpoints", nlohmann::json::object()}};
                            if (fs::exists(epStatePath)) {
                                std::ifstream ef(epStatePath);
                                auto parsed = nlohmann::json::parse(ef, nullptr, false);
                                if (!parsed.is_discarded() && parsed.is_object()) {
                                    ej = std::move(parsed);
                                }
                            }
                            if (!ej.contains("endpoints") || !ej["endpoints"].is_object()) {
                                ej["endpoints"] = nlohmann::json::object();
                            }
                            std::string key = std::to_string(p);
                            ej["endpoints"][key] = {
                                {"port", p},
                                {"csrf_token", csrfTok},
                                {"workspace", body.value("workspace", "")},
                                {"conversation_id", body.value("conversation_id", sessionId)},
                                {"updated_at", static_cast<long long>(std::time(nullptr))}
                            };
                            writeJsonFileAtomically(epStatePath, ej);
                        }
                    } catch (...) {}
                }
            }
            if (!recordedEndpointFromDetails && agentType == "antigravity") {
                static std::atomic<int64_t> s_lastHookProcScanEpoch{0};
                int64_t nowEpoch = static_cast<int64_t>(std::time(nullptr));
                int64_t prevEpoch = s_lastHookProcScanEpoch.load();
                if (nowEpoch - prevEpoch >= 3 &&
                    s_lastHookProcScanEpoch.compare_exchange_strong(prevEpoch, nowEpoch)) {
                    syncAndPruneCliEndpointsState(discoverRunningAgyProcesses(), true);
                }
            }

            if (eventType == "SESSION_START") {
                std::string convId = body.value("conversation_id", sessionId);
                std::string workspace = body.value("workspace", "");
                std::string model = body.value("model", "default");
                AgentTelemetryDb::getInstance().recordSessionStart(sessionId, convId, agentType, workspace, model, ts);
            } else if (eventType == "SESSION_END") {
                std::string status = body.value("status", "COMPLETED");
                int turns = body.value("total_turns", 0);
                int promptTokens = body.value("prompt_tokens", 0);
                int compTokens = body.value("comp_tokens", 0);
                int toolCalls = body.value("tool_calls", 0);
                int errors = body.value("errors", 0);
                double avgTurnMs = body.value("avg_turn_ms", 0.0);
                AgentTelemetryDb::getInstance().recordSessionEnd(sessionId, status, turns, promptTokens, compTokens, toolCalls, errors, avgTurnMs, ts);
            } else if (eventType == "sample" || eventType == "SAMPLE" || eventType == "TELEMETRY_SAMPLE") {
                AgentTelemetrySample sample;
                sample.timestamp = ts;
                sample.activeAgents = body.value("active_sessions", body.value("active_agents", 0));
                sample.promptTokensSec = body.value("prompt_tokens_per_sec", body.value("prompt_tokens_sec", 0.0));
                sample.compTokensSec = body.value("completion_tokens_per_sec", body.value("comp_tokens_sec", 0.0));
                sample.toolCallsSec = body.value("tool_calls_per_sec", body.value("tool_calls_sec", 0.0));
                sample.errorRatePct = body.value("tool_error_rate_pct", body.value("error_rate_pct", 0.0));
                sample.avgTurnLatencyMs = body.value("turn_latency_ms", body.value("avg_turn_latency_ms", 0.0));
                sample.p95TurnLatencyMs = body.value("turn_latency_p95_ms", body.value("p95_turn_latency_ms", 0.0));
                AgentTelemetryDb::getInstance().insertSample(sample);
            } else {
                AgentLifecycleEvent ev;
                ev.timestamp = ts;
                ev.sessionId = sessionId;
                ev.agentType = agentType;
                ev.eventType = eventType;
                ev.stepIndex = body.value("step_index", 0);
                ev.toolName = body.value("tool_name", "");
                ev.durationMs = body.value("duration_ms", 0.0);
                ev.status = body.value("status", "OK");
                if (body.contains("details")) {
                    ev.detailsJson = body["details"];
                }
                AgentTelemetryDb::getInstance().insertEvent(ev);
            }

            res.set_content("{\"status\":\"ok\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    _server->Get("/api/telemetry/overview", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        int hours = 24;
        if (req.has_param("hours")) {
            try { hours = std::stoi(req.get_param_value("hours")); } catch (...) {}
        }
        auto data = AgentTelemetryDb::getInstance().queryOverview(hours);
        res.set_content(data.dump(2), "application/json");
    });

    _server->Get("/api/telemetry/timeseries", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string window = "24h";
        int maxPoints = 300;
        if (req.has_param("window")) {
            window = req.get_param_value("window");
        }
        if (req.has_param("max_points")) {
            try { maxPoints = std::stoi(req.get_param_value("max_points")); } catch (...) {}
        }
        auto data = AgentTelemetryDb::getInstance().queryTimeseries(window, maxPoints);
        res.set_content(data.dump(2), "application/json");
    });

    _server->Get("/api/telemetry/activity_timeline", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string window = "24h";
        int maxSessions = 50;
        if (req.has_param("window")) {
            window = req.get_param_value("window");
        }
        if (req.has_param("max_sessions")) {
            try { maxSessions = std::stoi(req.get_param_value("max_sessions")); } catch (...) {}
        }
        auto data = AgentTelemetryDb::getInstance().queryActivityTimeline(window, maxSessions);
        res.set_content(data.dump(2), "application/json");
    });

    _server->Get("/api/telemetry/sessions", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        int limit = 50;
        std::string status = "";
        if (req.has_param("limit")) {
            try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
        }
        if (req.has_param("status")) {
            status = req.get_param_value("status");
        }
        auto data = AgentTelemetryDb::getInstance().querySessions(limit, status);
        res.set_content(data.dump(2), "application/json");
    });

    _server->Get("/api/telemetry/session_events", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string sessionId = req.has_param("sessionId") ? req.get_param_value("sessionId") :
                               (req.has_param("session_id") ? req.get_param_value("session_id") : "");
        auto data = AgentTelemetryDb::getInstance().querySessionEvents(sessionId);
        res.set_content(data.dump(2), "application/json");
    });

    _server->Get("/api/telemetry/tools", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        int hours = 24;
        if (req.has_param("hours")) {
            try { hours = std::stoi(req.get_param_value("hours")); } catch (...) {}
        }
        auto data = AgentTelemetryDb::getInstance().queryToolStats(hours);
        res.set_content(data.dump(2), "application/json");
    });

    // --- Mobile Gateway & Pairing Endpoints ---

    _server->Get("/api/mobile/qr", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidUiSession(req) && !isLoopbackRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
        std::string secret = gw.createPairingSecret(300);
        std::string lanIp = getPrimaryLanIp();

        nlohmann::json resp;
        resp["secret"] = secret;
        resp["expires_in"] = 300;
        if (!lanIp.empty()) {
            resp["lan_ip"] = lanIp;
        }
        res.set_content(resp.dump(2), "application/json");
    });

    _server->Post("/api/mobile/pair", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            nlohmann::json body = nlohmann::json::parse(req.body);
            std::string secret = body.value("secret", "");
            std::string deviceId = body.value("device_id", "");
            std::string deviceName = body.value("device_name", "");

            MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
            std::string token;
            if (gw.pairDevice(secret, deviceId, deviceName, token)) {
                std::string resolvedDevId = deviceId;
                gw.authenticate(token, resolvedDevId);
                nlohmann::json resp;
                resp["token"] = token;
                resp["device_id"] = resolvedDevId;
                resp["status"] = "paired";
                res.set_content(resp.dump(2), "application/json");
            } else {
                res.status = 401;
                res.set_content("{\"error\":\"Invalid or expired pairing secret\"}", "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    _server->Get("/api/mobile/devices", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
        auto devs = gw.listDevices();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& d : devs) {
            arr.push_back({
                {"device_id", d.deviceId},
                {"device_name", d.deviceName},
                {"paired_at", d.pairedAt},
                {"last_seen_at", d.lastSeenAt}
            });
        }
        res.set_content(arr.dump(2), "application/json");
    });

    _server->Post("/api/mobile/revoke", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        try {
            nlohmann::json body = nlohmann::json::parse(req.body);
            std::string deviceId = body.value("device_id", "");
            MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
            gw.revokeDevice(deviceId);
            res.set_content("{\"status\":\"revoked\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    // --- Zero-Python Native Antimatter Agent & Workspace Bridge Endpoints ---

    auto getWorkspaceRoot = []() -> fs::path {
        const char* envWs = std::getenv("AIMON_WORKSPACE_ROOT");
        if (envWs && *envWs) {
            return fs::path(envWs);
        }
        return fs::current_path();
    };

    auto getIdeBrainDir = []() -> std::string {
        const char* envIde = std::getenv("AIMON_IDE_BRAIN_DIR");
        if (envIde && *envIde) {
            return std::string(envIde);
        }
        return PathUtils::expandHome("~/.gemini/antigravity-ide/brain");
    };

    auto getCliBrainDir = []() -> std::string {
        const char* envCli = std::getenv("AIMON_CLI_BRAIN_DIR");
        if (envCli && *envCli) {
            return std::string(envCli);
        }
        return PathUtils::expandHome("~/.gemini/antigravity-cli/brain");
    };

    auto getCliPresenceDir = []() -> std::string {
        const char* envPres = std::getenv("AIMON_CLI_PRESENCE_DIR");
        if (envPres && *envPres) {
            return std::string(envPres);
        }
        return PathUtils::expandHome("~/.gemini/antigravity-cli/presence");
    };

    auto stripUserRequestTags = [](const std::string& raw) -> std::string {
        std::string text = raw;
        auto startTag = text.find("<USER_REQUEST>");
        auto endTag = text.find("</USER_REQUEST>");
        if (startTag != std::string::npos && endTag != std::string::npos && endTag >= startTag + 14) {
            text = text.substr(startTag + 14, endTag - (startTag + 14));
        } else {
            auto stripBlock = [&](const std::string& openT, const std::string& closeT) {
                while (true) {
                    auto s = text.find(openT);
                    if (s == std::string::npos) break;
                    auto e = text.find(closeT, s + openT.size());
                    if (e == std::string::npos) {
                        text.erase(s);
                        break;
                    }
                    text.erase(s, (e + closeT.size()) - s);
                }
            };
            stripBlock("<ADDITIONAL_METADATA>", "</ADDITIONAL_METADATA>");
            stripBlock("<USER_SETTINGS_CHANGE>", "</USER_SETTINGS_CHANGE>");
            stripBlock("<SYSTEM_MESSAGE>", "</SYSTEM_MESSAGE>");
        }
        auto first = text.find_first_not_of(" \n\r\t");
        if (first == std::string::npos) {
            return "";
        }
        auto last = text.find_last_not_of(" \n\r\t");
        return text.substr(first, last - first + 1);
    };

    auto handleAgentAvailable = [getWorkspaceRoot](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string wsRoot = getWorkspaceRoot().string();
        nlohmann::json resp;
        resp["agents"] = nlohmann::json::array({
            {
                {"id", "antigravity"},
                {"name", "Google Antigravity"},
                {"status", "online"},
                {"workspaceRoot", wsRoot}
            }
        });
        resp["allowed_workspaces"] = nlohmann::json::array({wsRoot});
        resp["current_workspace"] = wsRoot;
        res.set_content(resp.dump(2), "application/json");
    };
    _server->Get("/api/agent/available", handleAgentAvailable);
    _server->Get("/api/agent/agents", handleAgentAvailable);

    struct LsEndpoint {
        int port = 0;
        std::string csrf;
        bool isHttps = false;
        bool isCli = false;
        bool isPreferredWorkspace = false;
        std::string pid;
        std::string cwd;
        std::string lockedConversationId;
        std::string terminalPts;
    };

    auto discoverLsEndpoints = []() -> std::vector<LsEndpoint> {
        std::vector<LsEndpoint> endpoints;

        auto addEndpoint = [&](int port, const std::string& csrf, bool preferred = false,
                               bool isCli = false, const std::string& pidStr = "",
                               const std::string& cwd = "", const std::string& lockedCid = "",
                               const std::string& pts = "") {
            if (port <= 0 || port > 65535 || csrf.empty()) return;
            for (auto& ep : endpoints) {
                if (ep.port == port && ep.csrf == csrf) {
                    if (preferred) ep.isPreferredWorkspace = true;
                    if (isCli) ep.isCli = true;
                    if (ep.pid.empty() && !pidStr.empty()) ep.pid = pidStr;
                    if (ep.cwd.empty() && !cwd.empty()) ep.cwd = cwd;
                    if (ep.lockedConversationId.empty() && !lockedCid.empty()) ep.lockedConversationId = lockedCid;
                    if (ep.terminalPts.empty() && !pts.empty()) ep.terminalPts = pts;
                    return;
                }
            }
            LsEndpoint ep;
            ep.port = port;
            ep.csrf = csrf;
            ep.isHttps = false;
            ep.isCli = isCli;
            ep.isPreferredWorkspace = preferred;
            ep.pid = pidStr;
            ep.cwd = cwd;
            ep.lockedConversationId = lockedCid;
            ep.terminalPts = pts;
            endpoints.push_back(std::move(ep));
        };

        const char* envLsPort = std::getenv("AGY_LS_PORT");
        const char* envLsCsrf = std::getenv("AGY_LS_CSRF_TOKEN");
        const char* envAgAddr = std::getenv("ANTIGRAVITY_LS_ADDRESS");
        const char* envAgCsrf = std::getenv("ANTIGRAVITY_CSRF_TOKEN");
        const char* envEpFileOverride = std::getenv("AIMON_AGY_ENDPOINTS_FILE");
        const char* envProcsFileOverride = std::getenv("AIMON_AGY_PROCESSES_FILE");
        bool hasHermeticTestOverrides = (envEpFileOverride && *envEpFileOverride) ||
                                        (envProcsFileOverride && *envProcsFileOverride);
        std::string fallbackCsrf = (envLsCsrf && *envLsCsrf) ? envLsCsrf :
                                   ((envAgCsrf && *envAgCsrf) ? envAgCsrf : "");
        if (envLsPort && *envLsPort) {
            try {
                addEndpoint(std::stoi(envLsPort), fallbackCsrf, true, true);
            } catch (...) {}
        }
        if (!hasHermeticTestOverrides && envAgAddr && *envAgAddr) {
            std::string addr(envAgAddr);
            auto colon = addr.rfind(':');
            if (colon != std::string::npos) {
                try {
                    int p = std::stoi(addr.substr(colon + 1));
                    if (p > 0 && p <= 65535 && !fallbackCsrf.empty()) {
                        {
                            std::lock_guard<std::mutex> lk(s_cachedCliTokensMutex);
                            s_cachedCliTokens[p] = fallbackCsrf;
                        }
                        addEndpoint(p, fallbackCsrf, true, true);
                    }
                } catch (...) {}
            }
        }

        if (!(envLsPort && *envLsPort) && (fs::exists("/proc") || hasHermeticTestOverrides)) {
            auto agyProcs = discoverRunningAgyProcesses();
            auto liveEnvEndpoints = syncAndPruneCliEndpointsState(agyProcs, !hasHermeticTestOverrides);

            for (const auto& agyProc : agyProcs) {
                if (agyProc.httpPort <= 0) continue;
                std::string tok;
                {
                    std::lock_guard<std::mutex> lk(s_cachedCliTokensMutex);
                    auto it = s_cachedCliTokens.find(agyProc.httpPort);
                    if (it != s_cachedCliTokens.end()) {
                        tok = it->second;
                    }
                }
                if (!tok.empty()) {
                    bool preferred = (agyProc.cwd.find("aimon") != std::string::npos);
                    addEndpoint(agyProc.httpPort, tok, preferred, true, agyProc.pidStr,
                                agyProc.cwd, agyProc.lockedConversationId, agyProc.terminalPts);
                }
            }

            for (const auto& kv : liveEnvEndpoints) {
                bool preferred = (kv.second.second.find("aimon") != std::string::npos);
                addEndpoint(kv.first, kv.second.first, preferred, true, "", kv.second.second, "", "");
            }

            {
                std::lock_guard<std::mutex> lk(s_cachedCliTokensMutex);
                for (const auto& tkv : s_cachedCliTokens) {
                    addEndpoint(tkv.first, tkv.second, false, true, "", "", "", "");
                }
            }

            if (!hasHermeticTestOverrides && fs::exists("/proc")) {
                std::vector<pid_t> pids;
                std::error_code ec;
                for (const auto& entry : fs::directory_iterator("/proc", ec)) {
                    if (ec || !entry.is_directory()) continue;
                    std::string dname = entry.path().filename().string();
                    if (!dname.empty() && std::all_of(dname.begin(), dname.end(), ::isdigit)) {
                        try { pids.push_back(std::stoi(dname)); } catch (...) {}
                    }
                }
                std::sort(pids.begin(), pids.end(), std::greater<pid_t>());

                auto findPortsForPid = [](pid_t pid) -> std::vector<int> {
                    std::vector<int> ports;
                    std::set<std::string> inodes;
                    std::string fdDir = "/proc/" + std::to_string(pid) + "/fd";
                    std::error_code fdEc;
                    if (!fs::exists(fdDir, fdEc)) return ports;
                    for (const auto& fdEntry : fs::directory_iterator(fdDir, fdEc)) {
                        if (fdEc) break;
                        std::error_code symEc;
                        if (fs::is_symlink(fdEntry.path(), symEc)) {
                            std::string target = fs::read_symlink(fdEntry.path(), symEc).string();
                            if (!symEc && target.rfind("socket:[", 0) == 0 && target.size() > 9) {
                                inodes.insert(target.substr(8, target.size() - 9));
                            }
                        }
                    }
                    if (inodes.empty()) return ports;

                    auto scanNetFile = [&](const std::string& netPath) {
                        std::ifstream nf(netPath);
                        std::string line;
                        std::getline(nf, line);
                        while (std::getline(nf, line)) {
                            std::istringstream ss(line);
                            std::string sl, localAddr, remAddr, state, txRx, trTm, retrnsmt, uid, timeout, inode;
                            if (ss >> sl >> localAddr >> remAddr >> state >> txRx >> trTm >> retrnsmt >> uid >> timeout >> inode) {
                                if (state == "0A" && inodes.count(inode)) {
                                    auto colon = localAddr.find(':');
                                    if (colon != std::string::npos) {
                                        try {
                                            int p = std::stoi(localAddr.substr(colon + 1), nullptr, 16);
                                            if (p > 0) ports.push_back(p);
                                        } catch (...) {}
                                    }
                                }
                            }
                        }
                    };
                    scanNetFile("/proc/net/tcp");
                    scanNetFile("/proc/net/tcp6");
                    return ports;
                };

                for (pid_t pid : pids) {
                    std::ifstream cmdFile("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
                    if (!cmdFile.is_open()) continue;
                    std::string cmdContent((std::istreambuf_iterator<char>(cmdFile)),
                                           std::istreambuf_iterator<char>());
                    if (cmdContent.find("language_server") == std::string::npos) continue;

                    std::string csrf;
                    std::string workspaceId;
                    size_t pos = 0;
                    while (pos < cmdContent.size()) {
                        std::string arg = cmdContent.c_str() + pos;
                        if (arg == "--csrf_token" && (pos + arg.length() + 1) < cmdContent.size()) {
                            csrf = cmdContent.c_str() + pos + arg.length() + 1;
                        } else if (arg == "--workspace_id" && (pos + arg.length() + 1) < cmdContent.size()) {
                            workspaceId = cmdContent.c_str() + pos + arg.length() + 1;
                        }
                        pos += arg.length() + 1;
                    }
                    if (csrf.empty()) csrf = fallbackCsrf;
                    if (csrf.empty()) continue;

                    bool preferred = (workspaceId.find("aimon") != std::string::npos);
                    for (int p : findPortsForPid(pid)) {
                        addEndpoint(p, csrf, preferred, false, std::to_string(pid), workspaceId, "", "");
                    }
                }
            }
        }

        std::stable_sort(endpoints.begin(), endpoints.end(), [](const LsEndpoint& a, const LsEndpoint& b) {
            if (a.isCli != b.isCli) {
                return a.isCli > b.isCli;
            }
            return a.isPreferredWorkspace > b.isPreferredWorkspace;
        });
        return endpoints;
    };

    auto lastRpcEndpoint = std::make_shared<std::pair<int, std::string>>(0, "");
    auto lastRpcMutex = std::make_shared<std::mutex>();

    auto toLowerAscii = [](std::string s) -> std::string {
        for (char& ch : s) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return s;
    };

    auto mapModelEnumToModelName = [](const std::string& raw) -> std::string {
        if (raw == "MODEL_PLACEHOLDER_M417" || raw == "gemini-4-argon-high") {
            return "gemini-4-argon-high";
        }
        if (raw == "MODEL_PLACEHOLDER_M418" || raw == "gemini-4-argon-medium" || raw == "pro") {
            return "gemini-4-argon-medium";
        }
        if (raw == "MODEL_PLACEHOLDER_M419" || raw == "gemini-4-argon-low") {
            return "gemini-4-argon-low";
        }
        if (raw == "MODEL_PLACEHOLDER_M318" || raw == "gemini-3.8-flash-high") {
            return "gemini-3.8-flash-high";
        }
        if (raw == "MODEL_PLACEHOLDER_M319" || raw == "gemini-3.8-flash-medium") {
            return "gemini-3.8-flash-medium";
        }
        if (raw == "MODEL_PLACEHOLDER_M320" || raw == "gemini-3.8-flash-low") {
            return "gemini-3.8-flash-low";
        }
        if (raw == "MODEL_PLACEHOLDER_M322" || raw == "gemini-3.8-flash-tiered" || raw == "flash") {
            return "gemini-3.8-flash-tiered";
        }
        if (raw == "MODEL_PLACEHOLDER_M402" || raw == "claude-opus-5-5-high") {
            return "claude-opus-5-5-high";
        }
        if (raw == "MODEL_PLACEHOLDER_M401" || raw == "claude-opus-5-5-medium") {
            return "claude-opus-5-5-medium";
        }
        if (raw == "MODEL_PLACEHOLDER_M400" || raw == "claude-opus-5-5-low") {
            return "claude-opus-5-5-low";
        }
        if (raw == "MODEL_PLACEHOLDER_M405" || raw == "claude-sonnet-5-5-high") {
            return "claude-sonnet-5-5-high";
        }
        if (raw == "MODEL_PLACEHOLDER_M404" || raw == "claude-sonnet-5-5-medium") {
            return "claude-sonnet-5-5-medium";
        }
        if (raw == "MODEL_PLACEHOLDER_M403" || raw == "claude-sonnet-5-5-low") {
            return "claude-sonnet-5-5-low";
        }
        if (raw == "MODEL_OPENAI_GPT_OSS_120B_MEDIUM" || raw == "gpt-oss-120b-medium") {
            return "gpt-oss-120b-medium";
        }
        return raw;
    };

    auto mapModelToHumanLabel = [](const std::string& raw) -> std::string {
        if (raw.find("gemini-4-argon-high") != std::string::npos ||
            raw.find("MODEL_PLACEHOLDER_M417") != std::string::npos ||
            raw.find("Gemini 4 Argon (High)") != std::string::npos) {
            return "Gemini 4 Argon (High)";
        }
        if (raw.find("gemini-4-argon-medium") != std::string::npos ||
            raw.find("MODEL_PLACEHOLDER_M418") != std::string::npos ||
            raw.find("Gemini 4 Argon (Medium)") != std::string::npos) {
            return "Gemini 4 Argon (Medium)";
        }
        if (raw.find("gemini-4-argon-low") != std::string::npos ||
            raw.find("MODEL_PLACEHOLDER_M419") != std::string::npos ||
            raw.find("Gemini 4 Argon (Low)") != std::string::npos) {
            return "Gemini 4 Argon (Low)";
        }
        if (raw.find("gemini-3.8-flash-high") != std::string::npos ||
            raw.find("MODEL_PLACEHOLDER_M318") != std::string::npos ||
            raw.find("Gemini 3.8 Flash (High)") != std::string::npos) {
            return "Gemini 3.8 Flash (High)";
        }
        if (raw.find("gemini-3.8-flash-medium") != std::string::npos ||
            raw.find("MODEL_PLACEHOLDER_M319") != std::string::npos ||
            raw.find("Gemini 3.8 Flash (Medium)") != std::string::npos) {
            return "Gemini 3.8 Flash (Medium)";
        }
        if (raw.find("gemini-3.8-flash-low") != std::string::npos ||
            raw.find("MODEL_PLACEHOLDER_M320") != std::string::npos ||
            raw.find("Gemini 3.8 Flash (Low)") != std::string::npos) {
            return "Gemini 3.8 Flash (Low)";
        }
        if (raw.find("claude-opus-5-5-high") != std::string::npos ||
            raw.find("MODEL_PLACEHOLDER_M402") != std::string::npos ||
            raw.find("Claude Opus 5.5 (High)") != std::string::npos) {
            return "Claude Opus 5.5 (High)";
        }
        if (raw.find("claude-opus-5-5-medium") != std::string::npos ||
            raw.find("MODEL_PLACEHOLDER_M401") != std::string::npos ||
            raw.find("Claude Opus 5.5 (Medium)") != std::string::npos) {
            return "Claude Opus 5.5 (Medium)";
        }
        if (raw.find("claude-opus-5-5-low") != std::string::npos ||
            raw.find("MODEL_PLACEHOLDER_M400") != std::string::npos ||
            raw.find("Claude Opus 5.5 (Low)") != std::string::npos) {
            return "Claude Opus 5.5 (Low)";
        }
        if (raw.find("claude-sonnet-5-5-high") != std::string::npos ||
            raw.find("MODEL_PLACEHOLDER_M405") != std::string::npos ||
            raw.find("Claude Sonnet 5.5 (High)") != std::string::npos) {
            return "Claude Sonnet 5.5 (High)";
        }
        if (raw.find("claude-sonnet-5-5-medium") != std::string::npos ||
            raw.find("MODEL_PLACEHOLDER_M404") != std::string::npos ||
            raw.find("Claude Sonnet 5.5 (Medium)") != std::string::npos) {
            return "Claude Sonnet 5.5 (Medium)";
        }
        if (raw.find("claude-sonnet-5-5-low") != std::string::npos ||
            raw.find("MODEL_PLACEHOLDER_M403") != std::string::npos ||
            raw.find("Claude Sonnet 5.5 (Low)") != std::string::npos) {
            return "Claude Sonnet 5.5 (Low)";
        }
        if (raw.find("gpt-oss-120b-medium") != std::string::npos ||
            raw.find("MODEL_OPENAI_GPT_OSS_120B_MEDIUM") != std::string::npos ||
            raw.find("GPT-OSS 120B (Medium)") != std::string::npos) {
            return "GPT-OSS 120B (Medium)";
        }
        return "";
    };

    auto mapLabelToCliModelId = [toLowerAscii](const std::string& modelLabel) -> std::string {
        std::string lower = toLowerAscii(modelLabel);
        if (lower.empty()) return "";
        if (lower == "pro") return "pro";
        if (lower == "flash") return "flash";
        if (lower == "flash_lite") return "flash_lite";
        if (lower.find("m417") != std::string::npos || lower == "argon" ||
            (lower.find("argon") != std::string::npos && lower.find("high") != std::string::npos)) {
            return "gemini-4-argon-high";
        }
        if (lower.find("m419") != std::string::npos ||
            (lower.find("argon") != std::string::npos && lower.find("low") != std::string::npos)) {
            return "gemini-4-argon-low";
        }
        if (lower.find("m418") != std::string::npos || lower.find("argon") != std::string::npos) {
            return "gemini-4-argon-medium";
        }
        if (lower.find("m318") != std::string::npos ||
            (lower.find("flash") != std::string::npos && lower.find("high") != std::string::npos)) {
            return "gemini-3.8-flash-high";
        }
        if (lower.find("m319") != std::string::npos ||
            (lower.find("flash") != std::string::npos && lower.find("medium") != std::string::npos)) {
            return "gemini-3.8-flash-medium";
        }
        if (lower.find("m320") != std::string::npos ||
            (lower.find("flash") != std::string::npos && lower.find("low") != std::string::npos)) {
            return "gemini-3.8-flash-low";
        }
        if (lower.find("m402") != std::string::npos ||
            (lower.find("opus") != std::string::npos && lower.find("high") != std::string::npos)) {
            return "claude-opus-5-5-high";
        }
        if (lower.find("m401") != std::string::npos ||
            (lower.find("opus") != std::string::npos && lower.find("medium") != std::string::npos)) {
            return "claude-opus-5-5-medium";
        }
        if (lower.find("m400") != std::string::npos ||
            (lower.find("opus") != std::string::npos && lower.find("low") != std::string::npos)) {
            return "claude-opus-5-5-low";
        }
        if (lower.find("m405") != std::string::npos ||
            (lower.find("sonnet") != std::string::npos && lower.find("high") != std::string::npos)) {
            return "claude-sonnet-5-5-high";
        }
        if (lower.find("m404") != std::string::npos ||
            (lower.find("sonnet") != std::string::npos && lower.find("medium") != std::string::npos)) {
            return "claude-sonnet-5-5-medium";
        }
        if (lower.find("m403") != std::string::npos ||
            (lower.find("sonnet") != std::string::npos && lower.find("low") != std::string::npos)) {
            return "claude-sonnet-5-5-low";
        }
        if (lower.find("gpt-oss") != std::string::npos || lower.find("120b") != std::string::npos) {
            return "gpt-oss-120b-medium";
        }
        return "";
    };

    auto extractConversationModelById = [getCliBrainDir, getIdeBrainDir, mapModelToHumanLabel](
                                            const std::string& conversationId) -> std::string {
        if (conversationId.empty() || conversationId == "new") {
            return "";
        }

        // 1. Check transcript.jsonl for explicit <USER_SETTINGS_CHANGE> Model Selection lines
        std::string lastTranscriptModel;
        for (const std::string& rootDir : {getCliBrainDir(), getIdeBrainDir()}) {
            if (rootDir.empty()) continue;
            fs::path tPath = fs::path(rootDir) / conversationId / ".system_generated" / "logs" / "transcript.jsonl";
            if (!fs::exists(tPath)) continue;
            std::ifstream tf(tPath);
            std::string line;
            while (std::getline(tf, line)) {
                if (line.find("USER_SETTINGS_CHANGE") == std::string::npos &&
                    line.find("Model Selection") == std::string::npos) {
                    continue;
                }
                std::string matched = mapModelToHumanLabel(line);
                if (!matched.empty()) {
                    lastTranscriptModel = matched;
                }
            }
            if (!lastTranscriptModel.empty()) {
                return lastTranscriptModel;
            }
        }

        // 2. Check <cliRoot>/conversations/<conversationId>.db (executor_metadata and gen_metadata)
        fs::path cliRootDir = fs::path(getCliBrainDir()).parent_path();
        fs::path convDbPath = cliRootDir / "conversations" / (conversationId + ".db");
        if (fs::exists(convDbPath)) {
            sqlite3* db = nullptr;
            if (sqlite3_open_v2(convDbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK && db) {
                sqlite3_busy_timeout(db, 200);
                std::string foundLabel;
                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(db, "SELECT data FROM executor_metadata ORDER BY idx DESC LIMIT 5", -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                    while (sqlite3_step(stmt) == SQLITE_ROW) {
                        const void* blob = sqlite3_column_blob(stmt, 0);
                        int bytes = sqlite3_column_bytes(stmt, 0);
                        if (blob && bytes > 0) {
                            std::string raw(static_cast<const char*>(blob), static_cast<size_t>(std::min(bytes, 4096)));
                            std::string m = mapModelToHumanLabel(raw);
                            if (!m.empty()) {
                                foundLabel = m;
                                break;
                            }
                        }
                    }
                    sqlite3_finalize(stmt);
                }
                if (foundLabel.empty()) {
                    stmt = nullptr;
                    if (sqlite3_prepare_v2(db, "SELECT data FROM gen_metadata ORDER BY idx DESC LIMIT 1", -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                        if (sqlite3_step(stmt) == SQLITE_ROW) {
                            const void* blob = sqlite3_column_blob(stmt, 0);
                            int bytes = sqlite3_column_bytes(stmt, 0);
                            if (blob && bytes > 0) {
                                int tailLen = std::min(bytes, 4096);
                                const char* startPtr = static_cast<const char*>(blob) + (bytes - tailLen);
                                std::string raw(startPtr, static_cast<size_t>(tailLen));
                                foundLabel = mapModelToHumanLabel(raw);
                            }
                        }
                        sqlite3_finalize(stmt);
                    }
                }
                sqlite3_close(db);
                if (!foundLabel.empty()) {
                    return foundLabel;
                }
            } else if (db) {
                sqlite3_close(db);
            }
        }

        return "";
    };

    auto mapLabelToModelEnum = [toLowerAscii](const LsEndpoint& ep, const std::string& modelLabel) -> std::string {
        std::string lower = toLowerAscii(modelLabel);
        std::string candidateEnum;
        if (modelLabel.rfind("MODEL_", 0) == 0) {
            candidateEnum = modelLabel;
        } else if (lower.find("m417") != std::string::npos || lower == "argon" ||
                   (lower.find("argon") != std::string::npos && lower.find("high") != std::string::npos)) {
            candidateEnum = "MODEL_PLACEHOLDER_M417";
        } else if (lower.find("m419") != std::string::npos ||
                   (lower.find("argon") != std::string::npos && lower.find("low") != std::string::npos)) {
            candidateEnum = "MODEL_PLACEHOLDER_M419";
        } else if (lower.find("m418") != std::string::npos || lower.find("argon") != std::string::npos || lower == "pro") {
            candidateEnum = "MODEL_PLACEHOLDER_M418";
        } else if (lower == "flash" || lower == "flash_lite" || lower.find("m322") != std::string::npos ||
                   lower.find("flash-tiered") != std::string::npos) {
            candidateEnum = "MODEL_PLACEHOLDER_M322";
        } else if (lower.find("m318") != std::string::npos ||
                   (lower.find("flash") != std::string::npos && lower.find("high") != std::string::npos)) {
            candidateEnum = "MODEL_PLACEHOLDER_M318";
        } else if (lower.find("m319") != std::string::npos ||
                   (lower.find("flash") != std::string::npos && lower.find("medium") != std::string::npos)) {
            candidateEnum = "MODEL_PLACEHOLDER_M319";
        } else if (lower.find("m320") != std::string::npos ||
                   (lower.find("flash") != std::string::npos && lower.find("low") != std::string::npos)) {
            candidateEnum = "MODEL_PLACEHOLDER_M320";
        } else if (lower.find("m402") != std::string::npos ||
                   (lower.find("opus") != std::string::npos && lower.find("high") != std::string::npos)) {
            candidateEnum = "MODEL_PLACEHOLDER_M402";
        } else if (lower.find("m401") != std::string::npos ||
                   (lower.find("opus") != std::string::npos && lower.find("medium") != std::string::npos)) {
            candidateEnum = "MODEL_PLACEHOLDER_M401";
        } else if (lower.find("m400") != std::string::npos ||
                   (lower.find("opus") != std::string::npos && lower.find("low") != std::string::npos)) {
            candidateEnum = "MODEL_PLACEHOLDER_M400";
        } else if (lower.find("m405") != std::string::npos ||
                   (lower.find("sonnet") != std::string::npos && lower.find("high") != std::string::npos)) {
            candidateEnum = "MODEL_PLACEHOLDER_M405";
        } else if (lower.find("m404") != std::string::npos ||
                   (lower.find("sonnet") != std::string::npos && lower.find("medium") != std::string::npos)) {
            candidateEnum = "MODEL_PLACEHOLDER_M404";
        } else if (lower.find("m403") != std::string::npos ||
                   (lower.find("sonnet") != std::string::npos && lower.find("low") != std::string::npos)) {
            candidateEnum = "MODEL_PLACEHOLDER_M403";
        } else if (lower.find("gpt-oss") != std::string::npos || lower.find("120b") != std::string::npos) {
            candidateEnum = "MODEL_OPENAI_GPT_OSS_120B_MEDIUM";
        }

        std::vector<std::pair<std::string, std::string>> supportedModels;
        if (ep.port > 0 && ep.port <= 65535 && !ep.csrf.empty()) {
            try {
                httplib::Client cli("127.0.0.1", ep.port);
                cli.set_connection_timeout(0, 400000);
                cli.set_read_timeout(1, 0);
                httplib::Headers hdrs = {
                    {"Content-Type", "application/json"},
                    {"x-codeium-csrf-token", ep.csrf}
                };
                auto stRes = cli.Post(
                    "/exa.language_server_pb.LanguageServerService/GetUserStatus",
                    hdrs,
                    "{}",
                    "application/json"
                );
                if (stRes && stRes->status == 200) {
                    auto sj = nlohmann::json::parse(stRes->body, nullptr, false);
                    if (!sj.is_discarded() && sj.is_object() &&
                        sj.contains("userStatus") && sj["userStatus"].is_object()) {
                        const auto& us = sj["userStatus"];
                        if (us.contains("cascadeModelConfigData") && us["cascadeModelConfigData"].is_object()) {
                            const auto& cmc = us["cascadeModelConfigData"];
                            if (cmc.contains("clientModelConfigs") && cmc["clientModelConfigs"].is_array()) {
                                for (const auto& item : cmc["clientModelConfigs"]) {
                                    if (!item.is_object()) continue;
                                    std::string lbl = item.value("label", "");
                                    std::string enm;
                                    if (item.contains("modelOrAlias") && item["modelOrAlias"].is_object()) {
                                        enm = item["modelOrAlias"].value("model", "");
                                    }
                                    if (!enm.empty()) {
                                        supportedModels.push_back({lbl, enm});
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (...) {}
        }

        if (!supportedModels.empty()) {
            for (const auto& pair : supportedModels) {
                if (pair.first == modelLabel || pair.second == modelLabel ||
                    (!candidateEnum.empty() && pair.second == candidateEnum)) {
                    return pair.second;
                }
            }
            for (const auto& pair : supportedModels) {
                std::string lblLow = toLowerAscii(pair.first);
                if (!lower.empty() && (lblLow == lower || lblLow.find(lower) != std::string::npos ||
                                       lower.find(lblLow) != std::string::npos)) {
                    return pair.second;
                }
            }
            // Endpoint explicitly reported its supported models and does not support the requested model
            return "";
        }

        if (ep.isCli) {
            return candidateEnum;
        }
        if (candidateEnum == "MODEL_PLACEHOLDER_M417" ||
            candidateEnum == "MODEL_PLACEHOLDER_M418" ||
            candidateEnum == "MODEL_PLACEHOLDER_M419") {
            return "";
        }
        return candidateEnum;
    };

    auto extractTrajectoryModelName = [mapModelEnumToModelName](const nlohmann::json& tj) -> std::string {
        if (!tj.is_object() || !tj.contains("trajectory") || !tj["trajectory"].is_object()) {
            return "gemini-4-argon-medium";
        }
        const auto& traj = tj["trajectory"];
        std::string fallbackM322Model;
        if (traj.contains("executorMetadatas") && traj["executorMetadatas"].is_array()) {
            const auto& ems = traj["executorMetadatas"];
            for (auto it = ems.rbegin(); it != ems.rend(); ++it) {
                if (!it->is_object() || !it->contains("cascadeConfig") || !(*it)["cascadeConfig"].is_object()) {
                    continue;
                }
                const auto& cc = (*it)["cascadeConfig"];
                if (!cc.contains("plannerConfig") || !cc["plannerConfig"].is_object()) {
                    continue;
                }
                const auto& pc = cc["plannerConfig"];
                std::string mName = pc.value("modelName", "");
                std::string pModel = pc.value("planModel", "");
                bool hasCustomCfg = pc.contains("customizationConfig") && !pc["customizationConfig"].is_null() &&
                                    pc["customizationConfig"].is_object() && !pc["customizationConfig"].empty();
                std::string candidate = !mName.empty() ? mapModelEnumToModelName(mName)
                                                       : (!pModel.empty() ? mapModelEnumToModelName(pModel) : "");
                if (candidate.empty()) {
                    continue;
                }
                if (candidate == "gemini-3.8-flash-tiered" && !hasCustomCfg) {
                    if (fallbackM322Model.empty()) {
                        fallbackM322Model = candidate;
                    }
                    continue;
                }
                return candidate;
            }
        }
        if (traj.contains("generatorMetadata") && traj["generatorMetadata"].is_array()) {
            const auto& gms = traj["generatorMetadata"];
            for (auto it = gms.rbegin(); it != gms.rend(); ++it) {
                if (it->is_object() && it->contains("chatModel") && (*it)["chatModel"].is_object()) {
                    std::string gmModel = (*it)["chatModel"].value("model", "");
                    if (!gmModel.empty()) {
                        std::string mapped = mapModelEnumToModelName(gmModel);
                        if (!mapped.empty() && (mapped != "gemini-3.8-flash-tiered" || fallbackM322Model.empty())) {
                            return mapped;
                        }
                    }
                }
            }
        }
        if (!fallbackM322Model.empty()) {
            return fallbackM322Model;
        }
        return "gemini-4-argon-medium";
    };

    auto resolveCascadeConfigForRpc = [mapLabelToModelEnum, mapModelEnumToModelName, extractConversationModelById](
                                          const LsEndpoint& ep,
                                          const std::string& targetCid,
                                          const std::string& requestedModel,
                                          const std::string& workspacePath) -> nlohmann::json {
        auto extractBestCascadeConfigFromTrajectory = [&](const nlohmann::json& tj, nlohmann::json& outConfig) -> bool {
            if (!tj.is_object() || !tj.contains("trajectory") || !tj["trajectory"].is_object()) {
                return false;
            }
            const auto& traj = tj["trajectory"];
            nlohmann::json latestCascadeConfig = nlohmann::json::object();
            nlohmann::json richCascadeConfig = nlohmann::json::object();
            nlohmann::json recoveredDynamicUris = nlohmann::json::array();

            if (traj.contains("executorMetadatas") && traj["executorMetadatas"].is_array()) {
                const auto& ems = traj["executorMetadatas"];
                for (auto it = ems.rbegin(); it != ems.rend(); ++it) {
                    if (!it->is_object() || !it->contains("cascadeConfig") || !(*it)["cascadeConfig"].is_object()) {
                        continue;
                    }
                    const auto& cc = (*it)["cascadeConfig"];
                    if (cc.empty()) {
                        continue;
                    }
                    if (latestCascadeConfig.empty()) {
                        latestCascadeConfig = cc;
                    }
                    if (recoveredDynamicUris.empty() && cc.contains("dynamicWorkspaceUris") &&
                        cc["dynamicWorkspaceUris"].is_array() && !cc["dynamicWorkspaceUris"].empty()) {
                        recoveredDynamicUris = cc["dynamicWorkspaceUris"];
                    }
                    if (richCascadeConfig.empty() && cc.contains("plannerConfig") && cc["plannerConfig"].is_object()) {
                        const auto& pc = cc["plannerConfig"];
                        if (pc.contains("customizationConfig") && pc["customizationConfig"].is_object() &&
                            !pc["customizationConfig"].empty()) {
                            richCascadeConfig = cc;
                        }
                    }
                    if (!latestCascadeConfig.empty() && !richCascadeConfig.empty() && !recoveredDynamicUris.empty()) {
                        break;
                    }
                }
            }

            std::string gmModelEnum;
            if (traj.contains("generatorMetadata") && traj["generatorMetadata"].is_array()) {
                const auto& gms = traj["generatorMetadata"];
                for (auto it = gms.rbegin(); it != gms.rend(); ++it) {
                    if (it->is_object() && it->contains("chatModel") && (*it)["chatModel"].is_object()) {
                        std::string m = (*it)["chatModel"].value("model", "");
                        if (!m.empty() && m != "MODEL_PLACEHOLDER_M322") {
                            gmModelEnum = m;
                            break;
                        }
                    }
                }
            }

            if (latestCascadeConfig.empty() && richCascadeConfig.empty() && gmModelEnum.empty()) {
                return false;
            }

            outConfig = !richCascadeConfig.empty() ? richCascadeConfig : latestCascadeConfig;
            if (!recoveredDynamicUris.empty() &&
                (!outConfig.contains("dynamicWorkspaceUris") || !outConfig["dynamicWorkspaceUris"].is_array() ||
                 outConfig["dynamicWorkspaceUris"].empty())) {
                outConfig["dynamicWorkspaceUris"] = recoveredDynamicUris;
            }

            if (!outConfig.contains("plannerConfig") || !outConfig["plannerConfig"].is_object()) {
                outConfig["plannerConfig"] = nlohmann::json::object();
            }
            auto& pc = outConfig["plannerConfig"];
            bool latestHadCustom = latestCascadeConfig.contains("plannerConfig") &&
                                   latestCascadeConfig["plannerConfig"].is_object() &&
                                   latestCascadeConfig["plannerConfig"].contains("customizationConfig") &&
                                   latestCascadeConfig["plannerConfig"]["customizationConfig"].is_object() &&
                                   !latestCascadeConfig["plannerConfig"]["customizationConfig"].empty();
            std::string curPlanModel = pc.value("planModel", "");
            if (curPlanModel.empty() || (curPlanModel == "MODEL_PLACEHOLDER_M322" && !latestHadCustom)) {
                std::string chosenEnum = !gmModelEnum.empty() ? gmModelEnum : (ep.isCli ? "MODEL_PLACEHOLDER_M418" : "");
                if (!chosenEnum.empty()) {
                    pc["planModel"] = chosenEnum;
                    pc["modelName"] = mapModelEnumToModelName(chosenEnum);
                }
            }
            return true;
        };

        nlohmann::json cascadeConfig = nlohmann::json::object();
        bool resolved = false;

        if (ep.port > 0 && ep.port <= 65535 && !ep.csrf.empty()) {
            try {
                httplib::Client cli("127.0.0.1", ep.port);
                cli.set_connection_timeout(0, 500000);
                cli.set_read_timeout(2, 0);
                httplib::Headers hdrs = {
                    {"Content-Type", "application/json"},
                    {"x-codeium-csrf-token", ep.csrf}
                };
                if (!targetCid.empty() && targetCid != "new") {
                    auto trajRes = cli.Post(
                        "/exa.language_server_pb.LanguageServerService/GetCascadeTrajectory",
                        hdrs,
                        nlohmann::json({{"cascadeId", targetCid}}).dump(),
                        "application/json"
                    );
                    if (trajRes && trajRes->status == 200) {
                        auto tj = nlohmann::json::parse(trajRes->body, nullptr, false);
                        if (!tj.is_discarded()) {
                            resolved = extractBestCascadeConfigFromTrajectory(tj, cascadeConfig);
                        }
                    }
                }
                if (!resolved) {
                    auto allRes = cli.Post(
                        "/exa.language_server_pb.LanguageServerService/GetAllCascadeTrajectories",
                        hdrs,
                        "{}",
                        "application/json"
                    );
                    if (allRes && allRes->status == 200) {
                        auto aj = nlohmann::json::parse(allRes->body, nullptr, false);
                        if (!aj.is_discarded() && aj.is_object() &&
                            aj.contains("trajectorySummaries") && aj["trajectorySummaries"].is_object()) {
                            int probed = 0;
                            for (auto it = aj["trajectorySummaries"].begin();
                                 it != aj["trajectorySummaries"].end() && probed < 5; ++it) {
                                std::string otherCid = it.key();
                                if (otherCid.empty() || otherCid == targetCid) continue;
                                ++probed;
                                auto otherRes = cli.Post(
                                    "/exa.language_server_pb.LanguageServerService/GetCascadeTrajectory",
                                    hdrs,
                                    nlohmann::json({{"cascadeId", otherCid}}).dump(),
                                    "application/json"
                                );
                                if (otherRes && otherRes->status == 200) {
                                    auto otj = nlohmann::json::parse(otherRes->body, nullptr, false);
                                    if (!otj.is_discarded() &&
                                        extractBestCascadeConfigFromTrajectory(otj, cascadeConfig)) {
                                        resolved = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (...) {}
        }

        if (!cascadeConfig.is_object()) {
            cascadeConfig = nlohmann::json::object();
        }
        if (!cascadeConfig.contains("plannerConfig") || !cascadeConfig["plannerConfig"].is_object()) {
            cascadeConfig["plannerConfig"] = nlohmann::json::object();
        }
        auto& pc = cascadeConfig["plannerConfig"];
        if (!pc.contains("planModel") || !pc["planModel"].is_string() || pc["planModel"].get<std::string>().empty()) {
            std::string dbModelLabel = extractConversationModelById(targetCid);
            std::string resolvedEnum = !dbModelLabel.empty() ? mapLabelToModelEnum(ep, dbModelLabel) : "";
            if (resolvedEnum.empty()) {
                if (ep.isCli) {
                    resolvedEnum = mapLabelToModelEnum(ep, "Gemini 4 Argon (Medium)");
                    if (resolvedEnum.empty()) resolvedEnum = "MODEL_PLACEHOLDER_M418";
                } else {
                    resolvedEnum = mapLabelToModelEnum(ep, "Claude Sonnet 5.5 (High)");
                    if (resolvedEnum.empty()) {
                        resolvedEnum = mapLabelToModelEnum(ep, "Gemini 3.8 Flash (High)");
                    }
                }
            }
            pc["planModel"] = resolvedEnum;
            if (!resolvedEnum.empty()) {
                pc["modelName"] = mapModelEnumToModelName(resolvedEnum);
            }
        }
        if ((!cascadeConfig.contains("dynamicWorkspaceUris") || !cascadeConfig["dynamicWorkspaceUris"].is_array() ||
             cascadeConfig["dynamicWorkspaceUris"].empty()) && !workspacePath.empty()) {
            cascadeConfig["dynamicWorkspaceUris"] = nlohmann::json::array({"file://" + workspacePath});
        }

        if (!requestedModel.empty()) {
            if (requestedModel == "MODEL_PLACEHOLDER_M322" || requestedModel == "gemini-3.8-flash-tiered" ||
                requestedModel == "flash" || requestedModel == "flash_lite") {
                pc["planModel"] = "MODEL_PLACEHOLDER_M322";
                pc["modelName"] = "gemini-3.8-flash-tiered";
            } else if (requestedModel.rfind("MODEL_CUSTOM_", 0) == 0) {
                pc["planModel"] = requestedModel;
            } else {
                std::string mappedEnum = mapLabelToModelEnum(ep, requestedModel);
                if (!mappedEnum.empty()) {
                    pc["planModel"] = mappedEnum;
                    pc["modelName"] = mapModelEnumToModelName(mappedEnum);
                } else if (!ep.isCli) {
                    pc["planModel"] = "";
                }
            }
        }

        std::string finalEnum = pc.value("planModel", "");
        if (!finalEnum.empty() && finalEnum != "MODEL_PLACEHOLDER_M322" && finalEnum.rfind("MODEL_CUSTOM_", 0) != 0) {
            std::string verified = mapLabelToModelEnum(ep, finalEnum);
            if (verified.empty()) {
                pc["planModel"] = "";
            } else {
                pc["planModel"] = verified;
                pc["modelName"] = mapModelEnumToModelName(verified);
            }
        }

        return cascadeConfig;
    };

    auto handleAgentConversations = [getIdeBrainDir, getCliBrainDir, getCliPresenceDir, stripUserRequestTags,
                                     toLowerAscii, extractConversationModelById, discoverLsEndpoints](
                                        const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        size_t limit = 50;
        if (req.has_param("limit")) {
            try {
                int parsed = std::stoi(req.get_param_value("limit"));
                if (parsed > 0) {
                    limit = static_cast<size_t>(parsed);
                }
            } catch (...) {}
        }
        std::string query = req.has_param("q") ? req.get_param_value("q") : "";
        std::string queryLower = toLowerAscii(query);
        bool includeSubagents = (req.has_param("include_subagents") &&
                                 req.get_param_value("include_subagents") == "true");

        struct DbConvSummary {
            std::string id;
            std::string title;
            std::string preview;
            int stepCount = 0;
            uint64_t lastModifiedSec = 0;
            std::string workspacePath;
            std::string workspaceShort;
            std::string status;
            std::string parentConversationId;
            int nestingDepth = 0;
        };

        std::map<std::string, DbConvSummary> summariesMap;
        std::set<std::string> subagentIds;
        std::set<std::string> phantomIds = {
            "74e1eba3-a305-4cf7-8ada-b6979fe09fd4",
            "7b329a4c-496a-4459-ac49-a55c176bfea1",
            "b8b5d7d4-eed2-442f-ba15-f070f3dc5c62"
        };

        fs::path cliRootDir = fs::path(getCliBrainDir()).parent_path();
        fs::path summariesDbPath = cliRootDir / "conversation_summaries.db";
        if (fs::exists(summariesDbPath)) {
            sqlite3* db = nullptr;
            if (sqlite3_open_v2(summariesDbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK && db) {
                sqlite3_busy_timeout(db, 300);
                sqlite3_stmt* stmt = nullptr;
                const char* sql =
                    "SELECT conversation_id, title, preview, step_count, "
                    "CAST(strftime('%s', substr(last_modified_time, 1, 19)) AS INTEGER), "
                    "workspace_uris, status, parent_conversation_id, nesting_depth "
                    "FROM conversation_summaries";
                if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                    while (sqlite3_step(stmt) == SQLITE_ROW) {
                        const char* cId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                        if (!cId || !*cId) continue;
                        DbConvSummary ds;
                        ds.id = cId;
                        const char* cTitle = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                        if (cTitle) ds.title = cTitle;
                        const char* cPrev = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                        if (cPrev) ds.preview = cPrev;
                        ds.stepCount = sqlite3_column_int(stmt, 3);
                        int64_t mSec = sqlite3_column_int64(stmt, 4);
                        if (mSec > 0) ds.lastModifiedSec = static_cast<uint64_t>(mSec);
                        const char* cWs = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                        if (cWs && *cWs) {
                            auto wj = nlohmann::json::parse(cWs, nullptr, false);
                            if (!wj.is_discarded() && wj.is_array() && !wj.empty() && wj[0].is_string()) {
                                std::string uri = wj[0].get<std::string>();
                                if (uri.rfind("file://", 0) == 0) {
                                    uri = uri.substr(7);
                                }
                                ds.workspacePath = uri;
                                ds.workspaceShort = fs::path(uri).filename().string();
                            }
                        }
                        const char* cSt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
                        if (cSt) ds.status = cSt;
                        const char* cPar = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
                        if (cPar) ds.parentConversationId = cPar;
                        ds.nestingDepth = sqlite3_column_int(stmt, 8);

                        if (ds.nestingDepth > 0 || !ds.parentConversationId.empty()) {
                            subagentIds.insert(ds.id);
                        }
                        summariesMap[ds.id] = std::move(ds);
                    }
                    sqlite3_finalize(stmt);
                }
                sqlite3_close(db);
            } else if (db) {
                sqlite3_close(db);
            }
        }

        fs::path metaCachePath = cliRootDir / "cache" / "conversation_metadata.json";
        if (fs::exists(metaCachePath)) {
            try {
                std::ifstream mf(metaCachePath);
                auto mj = nlohmann::json::parse(mf, nullptr, false);
                if (!mj.is_discarded() && mj.is_object() &&
                    mj.contains("conversations") && mj["conversations"].is_object()) {
                    for (auto it = mj["conversations"].begin(); it != mj["conversations"].end(); ++it) {
                        if (it.value().is_object() && it.value().value("is_internal", false)) {
                            auto sumIt = summariesMap.find(it.key());
                            if (sumIt == summariesMap.end() || sumIt->second.nestingDepth > 0 ||
                                !sumIt->second.parentConversationId.empty()) {
                                subagentIds.insert(it.key());
                            }
                        }
                    }
                }
            } catch (...) {}
        }

        std::map<std::string, AgyProcessInfo> runningAgyByConvId;
        const char* envCliBrainOverride = std::getenv("AIMON_CLI_BRAIN_DIR");
        const char* envAgyProcsOverride = std::getenv("AIMON_AGY_PROCESSES_FILE");
        bool isDefaultBrain = (!envCliBrainOverride || !*envCliBrainOverride) ||
                              (envAgyProcsOverride && *envAgyProcsOverride);
        if (isDefaultBrain) {
            for (const auto& proc : discoverRunningAgyProcesses()) {
                if (!proc.lockedConversationId.empty()) {
                    runningAgyByConvId[proc.lockedConversationId] = proc;
                }
            }
        }

        struct ConvEntry {
            std::string id;
            std::string title;
            std::string source;
            uint64_t timestamp = 0;
            bool active = false;
            bool openInTerminal = false;
            std::string terminalPts;
            std::string workspace;
            std::string workspacePath;
            std::string model;
            int stepCount = 0;
            std::string firstPrompt;
            std::string lastPrompt;
            int lastPromptStepIndex = -1;
            nlohmann::json userPrompts = nlohmann::json::array();
            std::string matchedPrompt;
            int matchedStepIndex = -1;
        };

        struct CandidateConv {
            std::string id;
            fs::path cdir;
            std::string sourceLabel;
            uint64_t ts = 0;
            bool hasTranscript = false;
            bool hasTaskMd = false;
            bool hasDbSummary = false;
        };

        std::vector<CandidateConv> candidates;
        std::set<std::string> seenIds;
        std::string presenceDir = getCliPresenceDir();
        uint64_t nowSec = static_cast<uint64_t>(std::time(nullptr));

        auto addCandidate = [&](const std::string& id, const fs::path& cdir, const std::string& sourceLabel) {
            if (id.empty() || id[0] == '.' || id == "tempmediaStorage") return;
            if (seenIds.count(id)) return;
            if (phantomIds.count(id)) return;
            if (!includeSubagents && subagentIds.count(id)) return;

            fs::path transcriptPath = cdir.empty() ? fs::path() : (cdir / ".system_generated" / "logs" / "transcript.jsonl");
            fs::path taskMdPath = cdir.empty() ? fs::path() : (cdir / "task.md");
            bool hasTranscript = !transcriptPath.empty() && fs::exists(transcriptPath);
            bool hasTaskMd = !taskMdPath.empty() && fs::exists(taskMdPath);
            auto sumIt = summariesMap.find(id);
            bool hasDbSummary = (sumIt != summariesMap.end());
            if (!hasTranscript && !hasTaskMd && !hasDbSummary) return;

            uint64_t ts = hasDbSummary ? sumIt->second.lastModifiedSec : 0;
            if (!cdir.empty() && fs::exists(cdir)) {
                try {
                    auto ftime = hasTranscript ? fs::last_write_time(transcriptPath)
                                               : fs::last_write_time(cdir);
                    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
                    );
                    auto secs = std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
                    if (secs > 0 && static_cast<uint64_t>(secs) > ts) {
                        ts = static_cast<uint64_t>(secs);
                    }
                } catch (...) {}
            }

            seenIds.insert(id);
            candidates.push_back({id, cdir, sourceLabel, ts, hasTranscript, hasTaskMd, hasDbSummary});
        };

        auto scanRoot = [&](const std::string& rootPath, const std::string& sourceLabel) {
            if (rootPath.empty() || !fs::exists(rootPath) || !fs::is_directory(rootPath)) {
                return;
            }
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(rootPath, ec)) {
                if (ec || !entry.is_directory()) continue;
                std::string id = entry.path().filename().string();
                addCandidate(id, entry.path(), sourceLabel);
            }
        };

        scanRoot(getIdeBrainDir(), "ide");
        scanRoot(getCliBrainDir(), "cli");

        for (const auto& kv : summariesMap) {
            if (seenIds.count(kv.first)) continue;
            if (!includeSubagents && subagentIds.count(kv.first)) continue;
            fs::path cdir = fs::path(getCliBrainDir()) / kv.first;
            addCandidate(kv.first, fs::exists(cdir) ? cdir : fs::path(), "cli");
        }

        std::sort(candidates.begin(), candidates.end(), [](const CandidateConv& a, const CandidateConv& b) {
            if (a.ts != b.ts) {
                return a.ts > b.ts;
            }
            return a.id > b.id;
        });

        std::vector<ConvEntry> collected;

        auto buildConvEntry = [&](const CandidateConv& cand) -> bool {
            const std::string& id = cand.id;
            const fs::path& cdir = cand.cdir;
            const std::string& sourceLabel = cand.sourceLabel;
            uint64_t ts = cand.ts;
            bool hasTranscript = cand.hasTranscript;
            bool hasTaskMd = cand.hasTaskMd;
            bool hasDbSummary = cand.hasDbSummary;

            fs::path transcriptPath = cdir.empty() ? fs::path() : (cdir / ".system_generated" / "logs" / "transcript.jsonl");
            fs::path taskMdPath = cdir.empty() ? fs::path() : (cdir / "task.md");
            fs::path planMdPath = cdir.empty() ? fs::path() : (cdir / "implementation_plan.md");
            auto sumIt = summariesMap.find(id);

            std::string mdTitle;
            for (const fs::path& mdPath : {taskMdPath, planMdPath}) {
                if (mdPath.empty() || !fs::exists(mdPath)) continue;
                std::ifstream tf(mdPath);
                std::string tline;
                while (std::getline(tf, tline)) {
                    auto first = tline.find_first_not_of(" \t\r\n");
                    if (first == std::string::npos) continue;
                    if (tline.compare(first, 2, "# ") == 0) {
                        mdTitle = tline.substr(first + 2);
                        auto last = mdTitle.find_last_not_of(" \t\r\n");
                        if (last != std::string::npos) {
                            mdTitle.erase(last + 1);
                        }
                        if (mdTitle.rfind("Task:", 0) == 0 || mdTitle.rfind("Plan:", 0) == 0) {
                            auto afterColon = mdTitle.find_first_not_of(" \t", 5);
                            if (afterColon != std::string::npos) {
                                mdTitle = mdTitle.substr(afterColon);
                            }
                        }
                        break;
                    }
                }
                if (!mdTitle.empty()) break;
            }

            struct PromptRecord {
                int stepIdx = 0;
                int filteredStepIdx = 0;
                std::string text;
                std::string timestamp;
            };
            std::vector<PromptRecord> allUserPrompts;
            std::string firstPrompt;
            std::string lastPrompt;
            int lastPromptStepIndex = -1;
            std::string matchedPrompt;
            int matchedStepIndex = -1;
            std::string assistantMatchedSnippet;
            int assistantMatchedStepIndex = -1;
            int totalStepsInTranscript = 0;
            bool isInternalSubagent = false;

            if (hasTranscript) {
                std::ifstream f(transcriptPath);
                std::string line;
                int lineNum = 0;
                int filteredIdx = 0;
                while (std::getline(f, line)) {
                    if (line.empty()) continue;
                    try {
                        auto j = nlohmann::json::parse(line, nullptr, false);
                        if (j.is_discarded() || !j.is_object()) continue;
                        int stepIdx = j.value("step_index", lineNum);
                        ++lineNum;
                        totalStepsInTranscript = std::max(totalStepsInTranscript, stepIdx + 1);
                        std::string stype = j.value("type", "");
                        if (lineNum == 1 && stype == "SYSTEM_MESSAGE" && !hasTaskMd && !hasDbSummary && !includeSubagents) {
                            isInternalSubagent = true;
                            break;
                        }
                        std::string content = j.value("content", "");
                        std::string thinking = j.value("thinking", "");
                        std::string createdAt = j.value("created_at", "");
                        bool hasToolCalls = j.contains("tool_calls") && j["tool_calls"].is_array() && !j["tool_calls"].empty();

                        if (stype == "USER_INPUT" || (lineNum == 1 && !hasDbSummary && j.contains("content") && stype != "PLANNER_RESPONSE")) {
                            std::string clean = stripUserRequestTags(content);
                            if (!clean.empty()) {
                                std::string truncated = (clean.size() > 240) ? (clean.substr(0, 237) + "...") : clean;
                                if (firstPrompt.empty()) {
                                    firstPrompt = truncated;
                                }
                                if (stype == "USER_INPUT") {
                                    lastPrompt = truncated;
                                    lastPromptStepIndex = stepIdx;
                                    allUserPrompts.push_back({stepIdx, filteredIdx, truncated, createdAt});
                                    if (!queryLower.empty() && toLowerAscii(clean).find(queryLower) != std::string::npos) {
                                        matchedPrompt = truncated;
                                        matchedStepIndex = stepIdx;
                                    }
                                    ++filteredIdx;
                                }
                            }
                        } else if (stype == "PLANNER_RESPONSE") {
                            if (!queryLower.empty() && matchedPrompt.empty() && assistantMatchedSnippet.empty()) {
                                std::string hay = !content.empty() ? content : thinking;
                                std::string hayLow = toLowerAscii(hay);
                                auto mPos = hayLow.find(queryLower);
                                if (mPos != std::string::npos) {
                                    size_t start = (mPos > 40) ? (mPos - 40) : 0;
                                    std::string snip = hay.substr(start, std::min<size_t>(200, hay.size() - start));
                                    for (char& ch : snip) {
                                        if (ch == '\r' || ch == '\n') ch = ' ';
                                    }
                                    assistantMatchedSnippet = snip;
                                    assistantMatchedStepIndex = stepIdx;
                                }
                            }
                            ++filteredIdx;
                        } else if (stype == "ERROR_MESSAGE" || hasToolCalls) {
                            ++filteredIdx;
                        }
                    } catch (...) {}
                }
            }
            if (isInternalSubagent) {
                return false;
            }

            if (firstPrompt.empty() && hasDbSummary && !sumIt->second.preview.empty()) {
                std::string cleanPrev = stripUserRequestTags(sumIt->second.preview);
                if (!cleanPrev.empty()) {
                    firstPrompt = (cleanPrev.size() > 240) ? (cleanPrev.substr(0, 237) + "...") : cleanPrev;
                    if (lastPrompt.empty()) {
                        lastPrompt = firstPrompt;
                    }
                }
            }

            std::string title;
            if (hasDbSummary && !sumIt->second.title.empty()) {
                title = sumIt->second.title;
            } else if (!mdTitle.empty()) {
                title = mdTitle;
            } else if (!firstPrompt.empty()) {
                title = firstPrompt;
                auto nlPos = title.find_first_of("\r\n");
                if (nlPos != std::string::npos) {
                    title = title.substr(0, nlPos);
                }
            }
            if (title.empty()) {
                title = "Conversation " + id.substr(0, std::min<size_t>(8, id.length()));
            } else if (title.length() > 80) {
                title = title.substr(0, 77) + "...";
            }

            bool openInTerminal = false;
            std::string terminalPts;
            std::string workspacePath = hasDbSummary ? sumIt->second.workspacePath : "";
            std::string workspaceShort = hasDbSummary ? sumIt->second.workspaceShort : "";

            auto agyIt = runningAgyByConvId.find(id);
            if (agyIt != runningAgyByConvId.end()) {
                openInTerminal = true;
                terminalPts = agyIt->second.terminalPts;
                if (workspacePath.empty() && !agyIt->second.cwd.empty()) {
                    workspacePath = agyIt->second.cwd;
                    workspaceShort = fs::path(workspacePath).filename().string();
                }
            }

            bool isActive = openInTerminal;
            if (!presenceDir.empty() && fs::exists(fs::path(presenceDir) / (id + ".lock"))) {
                isActive = true;
            } else if (hasDbSummary && sumIt->second.status == "CASCADE_RUN_STATUS_RUNNING") {
                isActive = true;
            } else if (ts > 0 && nowSec >= ts && (nowSec - ts) < 120) {
                isActive = true;
            }

            std::string modelLabel = extractConversationModelById(id);
            if (modelLabel.empty()) {
                modelLabel = (sourceLabel == "cli") ? "Gemini 4 Argon (High)" : "Claude Sonnet 5.5 (High)";
            }

            if (matchedPrompt.empty() && !assistantMatchedSnippet.empty()) {
                matchedPrompt = assistantMatchedSnippet;
                matchedStepIndex = assistantMatchedStepIndex;
            }

            if (!queryLower.empty()) {
                bool matchesMeta = (toLowerAscii(title).find(queryLower) != std::string::npos) ||
                                   (toLowerAscii(workspaceShort).find(queryLower) != std::string::npos) ||
                                   (toLowerAscii(workspacePath).find(queryLower) != std::string::npos) ||
                                   (toLowerAscii(modelLabel).find(queryLower) != std::string::npos) ||
                                   (toLowerAscii(id).find(queryLower) != std::string::npos) ||
                                   (toLowerAscii(firstPrompt).find(queryLower) != std::string::npos) ||
                                   (toLowerAscii(lastPrompt).find(queryLower) != std::string::npos) ||
                                   !matchedPrompt.empty();
                if (!matchesMeta) {
                    return false;
                }
            }

            nlohmann::json userPromptsJson = nlohmann::json::array();
            size_t startIdx = (allUserPrompts.size() > 30) ? (allUserPrompts.size() - 30) : 0;
            for (size_t i = startIdx; i < allUserPrompts.size(); ++i) {
                userPromptsJson.push_back({
                    {"stepIndex", allUserPrompts[i].stepIdx},
                    {"filteredStepIndex", allUserPrompts[i].filteredStepIdx},
                    {"text", allUserPrompts[i].text},
                    {"timestamp", allUserPrompts[i].timestamp}
                });
            }

            ConvEntry ce;
            ce.id = id;
            ce.title = title;
            ce.source = sourceLabel;
            ce.timestamp = ts;
            ce.active = isActive;
            ce.openInTerminal = openInTerminal;
            ce.terminalPts = terminalPts;
            ce.workspace = workspaceShort;
            ce.workspacePath = workspacePath;
            ce.model = modelLabel;
            ce.stepCount = std::max(totalStepsInTranscript, hasDbSummary ? sumIt->second.stepCount : 0);
            ce.firstPrompt = firstPrompt;
            ce.lastPrompt = lastPrompt;
            ce.lastPromptStepIndex = lastPromptStepIndex;
            ce.userPrompts = std::move(userPromptsJson);
            ce.matchedPrompt = matchedPrompt;
            ce.matchedStepIndex = matchedStepIndex;

            collected.push_back(std::move(ce));
            return true;
        };

        for (const auto& cand : candidates) {
            if (collected.size() >= limit) {
                break;
            }
            buildConvEntry(cand);
        }

        nlohmann::json conversations = nlohmann::json::array();
        size_t count = std::min(collected.size(), limit);
        for (size_t i = 0; i < count; ++i) {
            conversations.push_back({
                {"id", collected[i].id},
                {"title", collected[i].title},
                {"source", collected[i].source},
                {"timestamp", collected[i].timestamp},
                {"active", collected[i].active},
                {"openInTerminal", collected[i].openInTerminal},
                {"terminalPts", collected[i].terminalPts},
                {"workspace", collected[i].workspace},
                {"workspacePath", collected[i].workspacePath},
                {"model", collected[i].model},
                {"stepCount", collected[i].stepCount},
                {"firstPrompt", collected[i].firstPrompt},
                {"lastPrompt", collected[i].lastPrompt},
                {"lastPromptStepIndex", collected[i].lastPromptStepIndex},
                {"userPrompts", collected[i].userPrompts},
                {"matchedPrompt", collected[i].matchedPrompt},
                {"matchedStepIndex", collected[i].matchedStepIndex}
            });
        }

        nlohmann::json resp;
        resp["conversations"] = conversations;
        res.set_content(resp.dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    };
    _server->Get("/api/agent/conversations", handleAgentConversations);
    _server->Get("/api/agent/history", handleAgentConversations);

    _server->Get("/api/agent/transcript", [getIdeBrainDir, getCliBrainDir, stripUserRequestTags, discoverLsEndpoints, lastRpcEndpoint, lastRpcMutex, extractTrajectoryModelName, extractConversationModelById, mapLabelToCliModelId](
                                              const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string id = req.get_param_value("id");
        if (id.empty() || id.find("..") != std::string::npos || id.find('/') != std::string::npos) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing or invalid conversation id\"}", "application/json");
            return;
        }

        int sinceIndex = -1;
        if (req.has_param("since")) {
            try {
                sinceIndex = std::stoi(req.get_param_value("since"));
            } catch (...) {
                sinceIndex = -1;
            }
        }

        fs::path idePath = fs::path(getIdeBrainDir()) / id / ".system_generated" / "logs" / "transcript.jsonl";
        fs::path cliPath = fs::path(getCliBrainDir()) / id / ".system_generated" / "logs" / "transcript.jsonl";
        fs::path cliRootDir = fs::path(getCliBrainDir()).parent_path();
        bool isCliConv = fs::exists(cliPath) ||
                         fs::exists(fs::path(getCliBrainDir()) / id) ||
                         fs::exists(cliRootDir / "conversations" / (id + ".db"));
        bool isLockedByRunningAgy = false;
        int lockedHttpPort = 0;
        for (const auto& proc : discoverRunningAgyProcesses()) {
            if (proc.lockedConversationId == id) {
                isLockedByRunningAgy = true;
                lockedHttpPort = proc.httpPort;
                break;
            }
        }
        if (isLockedByRunningAgy) {
            isCliConv = true;
        }
        fs::path transcriptPath;
        std::string source = "ide";

        if (fs::exists(idePath) && fs::exists(cliPath)) {
            try {
                if (fs::last_write_time(cliPath) >= fs::last_write_time(idePath)) {
                    transcriptPath = cliPath;
                    source = "cli";
                } else {
                    transcriptPath = idePath;
                    source = "ide";
                }
            } catch (...) {
                transcriptPath = cliPath;
                source = "cli";
            }
        } else if (fs::exists(cliPath)) {
            transcriptPath = cliPath;
            source = "cli";
        } else if (fs::exists(idePath)) {
            transcriptPath = idePath;
            source = "ide";
        } else {
            // Fallback: check live in-memory trajectory via ConnectRPC GetCascadeTrajectory
            nlohmann::json liveSteps = nlohmann::json::array();
            int liveTotalSteps = 0;
            std::string liveModel = "gemini-4-argon-medium";
            bool foundInMemory = false;

            auto tryProbeRpcTrajectory = [&](int port, const std::string& csrf) -> bool {
                if (port <= 0 || port > 65535 || csrf.empty()) return false;
                try {
                    httplib::Client cli("127.0.0.1", port);
                    cli.set_connection_timeout(0, 400000);
                    cli.set_read_timeout(1, 0);
                    httplib::Headers hdrs = {
                        {"Content-Type", "application/json"},
                        {"x-codeium-csrf-token", csrf}
                    };
                    auto rpcRes = cli.Post(
                        "/exa.language_server_pb.LanguageServerService/GetCascadeTrajectory",
                        hdrs,
                        nlohmann::json({{"cascadeId", id}}).dump(),
                        "application/json"
                    );
                    if (rpcRes && rpcRes->status == 200) {
                        auto tj = nlohmann::json::parse(rpcRes->body, nullptr, false);
                        if (!tj.is_discarded() && tj.contains("trajectory") && tj["trajectory"].is_object()) {
                            liveModel = extractTrajectoryModelName(tj);
                            const auto& rawSteps = tj["trajectory"].value("steps", nlohmann::json::array());
                            nlohmann::json parsedLiveSteps = nlohmann::json::array();
                            int parsedTotalSteps = 0;
                            bool hasUnknownModelError = false;
                            int idx = 0;
                            for (const auto& rs : rawSteps) {
                                int stepIdx = idx++;
                                parsedTotalSteps = std::max(parsedTotalSteps, stepIdx + 1);
                                std::string stType = rs.value("type", "");
                                if (stType == "CORTEX_STEP_TYPE_ERROR_MESSAGE" && rs.contains("errorMessage")) {
                                    std::string errCheck;
                                    if (rs["errorMessage"].contains("error") && rs["errorMessage"]["error"].is_object()) {
                                        const auto& errObj = rs["errorMessage"]["error"];
                                        errCheck = errObj.value("modelErrorMessage",
                                                   errObj.value("shortError",
                                                   errObj.value("userErrorMessage", "")));
                                    }
                                    if (errCheck.find("unknown model key") != std::string::npos ||
                                        errCheck.find("failed to construct executor") != std::string::npos) {
                                        hasUnknownModelError = true;
                                    }
                                }
                                if (sinceIndex >= 0 && stepIdx <= sinceIndex) continue;
                                if (stType == "CORTEX_STEP_TYPE_USER_INPUT" && rs.contains("userInput")) {
                                    std::string uText = rs["userInput"].value("userResponse", "");
                                    if (uText.empty() && rs["userInput"].contains("items") && rs["userInput"]["items"].is_array() && !rs["userInput"]["items"].empty()) {
                                        uText = rs["userInput"]["items"][0].value("text", "");
                                    }
                                    parsedLiveSteps.push_back({
                                        {"index", stepIdx},
                                        {"stepIndex", stepIdx},
                                        {"type", "USER_INPUT"},
                                        {"text", uText},
                                        {"userInput", uText},
                                        {"step", {{"case", "userInput"}, {"value", uText}}}
                                    });
                                } else if (stType == "CORTEX_STEP_TYPE_PLANNER_RESPONSE" && rs.contains("plannerResponse")) {
                                    std::string pText = rs["plannerResponse"].value("modifiedResponse", rs["plannerResponse"].value("response", ""));
                                    std::string pThink = rs["plannerResponse"].value("thinking", "");
                                    parsedLiveSteps.push_back({
                                        {"index", stepIdx},
                                        {"stepIndex", stepIdx},
                                        {"type", "PLANNER_RESPONSE"},
                                        {"thinking", pThink},
                                        {"plannerResponse", pThink},
                                        {"text", pText},
                                        {"tool_calls", nlohmann::json::array()},
                                        {"step", {{"case", !pText.empty() ? "text" : "plannerResponse"}, {"value", !pText.empty() ? pText : pThink}}}
                                    });
                                } else if (stType == "CORTEX_STEP_TYPE_ERROR_MESSAGE" && rs.contains("errorMessage")) {
                                    std::string errText = "Agent execution error";
                                    if (rs["errorMessage"].contains("error") && rs["errorMessage"]["error"].is_object()) {
                                        const auto& errObj = rs["errorMessage"]["error"];
                                        errText = errObj.value("modelErrorMessage",
                                                  errObj.value("shortError",
                                                  errObj.value("userErrorMessage", errText)));
                                    }
                                    parsedLiveSteps.push_back({
                                        {"index", stepIdx},
                                        {"stepIndex", stepIdx},
                                        {"type", "ERROR_MESSAGE"},
                                        {"text", errText},
                                        {"step", {{"case", "errorMessage"}, {"value", errText}}}
                                    });
                                }
                            }
                            if (hasUnknownModelError && parsedTotalSteps <= 2) {
                                return false;
                            }
                            liveSteps = std::move(parsedLiveSteps);
                            liveTotalSteps = parsedTotalSteps;
                            return true;
                        }
                    }
                } catch (...) {}
                return false;
            };

            const char* envLsPort = std::getenv("AGY_LS_PORT");
            const char* envLsCsrf = std::getenv("AGY_LS_CSRF_TOKEN");
            if (envLsPort && *envLsPort && envLsCsrf && *envLsCsrf) {
                try {
                    int envP = std::stoi(envLsPort);
                    if (!isLockedByRunningAgy || (lockedHttpPort > 0 && envP == lockedHttpPort)) {
                        if (tryProbeRpcTrajectory(envP, envLsCsrf)) {
                            foundInMemory = true;
                        }
                    }
                } catch (...) {}
            }
            if (!foundInMemory && !isCliConv && !isLockedByRunningAgy) {
                int cachedPort = 0;
                std::string cachedCsrf;
                {
                    std::lock_guard<std::mutex> lk(*lastRpcMutex);
                    cachedPort = lastRpcEndpoint->first;
                    cachedCsrf = lastRpcEndpoint->second;
                }
                if (cachedPort > 0 && !cachedCsrf.empty()) {
                    if (tryProbeRpcTrajectory(cachedPort, cachedCsrf)) {
                        foundInMemory = true;
                    }
                }
            }
            if (!foundInMemory) {
                auto eps = discoverLsEndpoints();
                std::stable_sort(eps.begin(), eps.end(), [&](const LsEndpoint& a, const LsEndpoint& b) {
                    bool aOwns = (a.lockedConversationId == id) || (lockedHttpPort > 0 && a.port == lockedHttpPort);
                    bool bOwns = (b.lockedConversationId == id) || (lockedHttpPort > 0 && b.port == lockedHttpPort);
                    if (aOwns != bOwns) return aOwns > bOwns;
                    return false;
                });
                for (const auto& ep : eps) {
                    if (isCliConv && !ep.isCli) {
                        continue;
                    }
                    if (isLockedByRunningAgy) {
                        if (lockedHttpPort > 0 && ep.port != lockedHttpPort) {
                            continue;
                        }
                        if (lockedHttpPort <= 0 && ep.lockedConversationId != id) {
                            continue;
                        }
                    }
                    if (tryProbeRpcTrajectory(ep.port, ep.csrf)) {
                        std::lock_guard<std::mutex> lk(*lastRpcMutex);
                        *lastRpcEndpoint = {ep.port, ep.csrf};
                        foundInMemory = true;
                        break;
                    }
                }
            }

            if (foundInMemory) {
                nlohmann::json resp;
                resp["conversationId"] = id;
                resp["source"] = "live";
                resp["model"] = liveModel;
                resp["stepCount"] = liveTotalSteps;
                resp["steps"] = liveSteps;
                res.set_content(resp.dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
                return;
            }

            res.status = 404;
            res.set_content("{\"error\":\"Conversation transcript not found\"}", "application/json");
            return;
        }

        std::ifstream f(transcriptPath);
        std::string line;
        nlohmann::json steps = nlohmann::json::array();
        int lineNum = 0;
        int totalStepCount = 0;

        while (std::getline(f, line)) {
            if (line.empty()) continue;
            try {
                auto j = nlohmann::json::parse(line, nullptr, false);
                if (j.is_discarded() || !j.is_object()) continue;

                int stepIdx = j.value("step_index", lineNum);
                ++lineNum;
                totalStepCount = std::max(totalStepCount, stepIdx + 1);

                if (sinceIndex >= 0 && stepIdx <= sinceIndex) {
                    continue;
                }

                std::string type = j.value("type", "");
                std::string content = j.value("content", "");
                std::string thinking = j.value("thinking", "");
                std::string createdAt = j.value("created_at", "");

                nlohmann::json parsedToolCalls = nlohmann::json::array();
                if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
                    for (const auto& tc : j["tool_calls"]) {
                        if (!tc.is_object()) continue;
                        std::string toolName = tc.value("name", "tool");
                        nlohmann::json argsObj = nlohmann::json::object();
                        if (tc.contains("arguments") && tc["arguments"].is_object()) {
                            argsObj = tc["arguments"];
                        } else if (tc.contains("args") && tc["args"].is_object()) {
                            argsObj = tc["args"];
                        }

                        auto unquoteStr = [](const std::string& s) -> std::string {
                            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
                                return s.substr(1, s.size() - 2);
                            }
                            return s;
                        };

                        std::string toolAction = tc.value("toolAction", "");
                        if (toolAction.empty() && argsObj.contains("toolAction") && argsObj["toolAction"].is_string()) {
                            toolAction = unquoteStr(argsObj["toolAction"].get<std::string>());
                        }
                        std::string toolSummary = tc.value("toolSummary", "");
                        if (toolSummary.empty() && argsObj.contains("toolSummary") && argsObj["toolSummary"].is_string()) {
                            toolSummary = unquoteStr(argsObj["toolSummary"].get<std::string>());
                        }
                        std::string cmd;
                        if (argsObj.contains("CommandLine") && argsObj["CommandLine"].is_string()) {
                            cmd = unquoteStr(argsObj["CommandLine"].get<std::string>());
                        }

                        parsedToolCalls.push_back({
                            {"name", toolName},
                            {"toolAction", toolAction},
                            {"toolSummary", toolSummary},
                            {"command", cmd},
                            {"arguments", argsObj}
                        });
                    }
                }

                if (type == "USER_INPUT") {
                    std::string clean = stripUserRequestTags(content);
                    steps.push_back({
                        {"index", stepIdx},
                        {"stepIndex", stepIdx},
                        {"type", "USER_INPUT"},
                        {"text", clean},
                        {"userInput", clean},
                        {"createdAt", createdAt},
                        {"step", {
                            {"case", "userInput"},
                            {"value", clean}
                        }}
                    });
                } else if (type == "PLANNER_RESPONSE") {
                    std::string displayValue = !content.empty() ? content : thinking;
                    steps.push_back({
                        {"index", stepIdx},
                        {"stepIndex", stepIdx},
                        {"type", "PLANNER_RESPONSE"},
                        {"thinking", thinking},
                        {"plannerResponse", thinking},
                        {"text", content},
                        {"tool_calls", parsedToolCalls},
                        {"createdAt", createdAt},
                        {"step", {
                            {"case", !content.empty() ? "text" : "plannerResponse"},
                            {"value", displayValue}
                        }}
                    });
                } else if (type == "ERROR_MESSAGE") {
                    std::string errText = !content.empty() ? content : j.value("error", "Agent execution error");
                    steps.push_back({
                        {"index", stepIdx},
                        {"stepIndex", stepIdx},
                        {"type", "ERROR_MESSAGE"},
                        {"text", errText},
                        {"createdAt", createdAt},
                        {"step", {
                            {"case", "errorMessage"},
                            {"value", errText}
                        }}
                    });
                } else if (!parsedToolCalls.empty()) {
                    std::string firstTool = parsedToolCalls[0].value("name", "tool");
                    std::string firstCmd = parsedToolCalls[0].value("command", "");
                    steps.push_back({
                        {"index", stepIdx},
                        {"stepIndex", stepIdx},
                        {"type", type.empty() ? "TOOL_CALL" : type},
                        {"thinking", thinking},
                        {"text", content},
                        {"tool_calls", parsedToolCalls},
                        {"createdAt", createdAt},
                        {"step", {
                            {"case", "toolCall"},
                            {"tool", firstTool},
                            {"command", firstCmd}
                        }}
                    });
                }
            } catch (...) {}
        }

        std::string resolvedModel = "gemini-4-argon-medium";
        std::string extractedLabel = extractConversationModelById(id);
        if (!extractedLabel.empty()) {
            resolvedModel = extractedLabel;
        }

        nlohmann::json resp;
        resp["conversationId"] = id;
        resp["source"] = source;
        resp["model"] = resolvedModel;
        resp["modelId"] = !extractedLabel.empty() ? mapLabelToCliModelId(extractedLabel) : resolvedModel;
        resp["stepCount"] = totalStepCount;
        resp["steps"] = steps;
        res.set_content(resp.dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    });

    _server->Post("/api/agent/prompt", [getWorkspaceRoot, getCliBrainDir, discoverLsEndpoints, lastRpcEndpoint, lastRpcMutex, resolveCascadeConfigForRpc, extractConversationModelById, mapLabelToCliModelId, toLowerAscii](
                                           const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(req.body);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"status", "error"}, {"error", e.what()}}).dump(), "application/json");
            return;
        }

        std::string text = j.value("text", j.value("prompt", ""));
        std::string agentId = j.value("agentId", "antigravity");
        std::string cid = j.value("conversationId", "");
        std::string requestedModel = j.value("model", "");
        std::string requestedSource = toLowerAscii(j.value("source", ""));
        if (text.empty()) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"error\":\"Prompt text cannot be empty\"}", "application/json");
            return;
        }

        std::string agyBin = PathUtils::expandHome("~/.local/bin/agy");
        const char* envAgyBin = std::getenv("AIMON_AGY_BIN");
        if (envAgyBin && *envAgyBin) {
            agyBin = envAgyBin;
        }
        if (!fs::exists(agyBin) || access(agyBin.c_str(), X_OK) != 0) {
            res.status = 503;
            res.set_content(nlohmann::json({
                {"status", "error"},
                {"error", "Antigravity CLI binary (agy) is not available or not executable at " + agyBin}
            }).dump(2), "application/json");
            return;
        }

        fs::path cliBrainDir = fs::path(getCliBrainDir());
        fs::path cliRootDir = cliBrainDir.parent_path();
        bool hasCliDiskConversation = false;
        if (!cid.empty() && cid != "new") {
            hasCliDiskConversation = fs::exists(cliBrainDir / cid) ||
                                     fs::exists(cliRootDir / "conversations" / (cid + ".db"));
        }

        std::string desiredModel = !requestedModel.empty()
                                       ? requestedModel
                                       : ((!cid.empty() && cid != "new") ? extractConversationModelById(cid) : "");
        std::string desiredLower = toLowerAscii(desiredModel);
        bool requiresCliModel = desiredLower.find("argon") != std::string::npos ||
                                desiredLower.find("m417") != std::string::npos ||
                                desiredLower.find("m418") != std::string::npos ||
                                desiredLower.find("m419") != std::string::npos;
        bool isCliConv = hasCliDiskConversation || requestedSource == "cli" || requiresCliModel;

        std::string wsPath = getWorkspaceRoot().string();
        std::vector<AgyProcessInfo> runningAgy = discoverRunningAgyProcesses();
        bool isLockedByRunningAgy = false;
        int lockedHttpPort = 0;
        std::string lockedTerminalPts;
        for (const auto& proc : runningAgy) {
            if (!cid.empty() && cid != "new" && proc.lockedConversationId == cid) {
                isLockedByRunningAgy = true;
                isCliConv = true;
                lockedHttpPort = proc.httpPort;
                lockedTerminalPts = proc.terminalPts;
                if (!proc.cwd.empty()) {
                    wsPath = proc.cwd;
                }
                break;
            }
        }
        if (!cid.empty() && cid != "new" && wsPath == getWorkspaceRoot().string()) {
            fs::path sumDbPath = cliRootDir / "conversation_summaries.db";
            if (fs::exists(sumDbPath)) {
                sqlite3* db = nullptr;
                if (sqlite3_open_v2(sumDbPath.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
                    sqlite3_stmt* stmt = nullptr;
                    const char* sql = "SELECT workspace_uris FROM conversation_summaries WHERE conversation_id = ? LIMIT 1";
                    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_text(stmt, 1, cid.c_str(), -1, SQLITE_STATIC);
                        if (sqlite3_step(stmt) == SQLITE_ROW) {
                            const unsigned char* wuTxt = sqlite3_column_text(stmt, 0);
                            std::string wsUris = wuTxt ? reinterpret_cast<const char*>(wuTxt) : "";
                            try {
                                auto wj = nlohmann::json::parse(wsUris, nullptr, false);
                                if (!wj.is_discarded() && wj.is_array() && !wj.empty() && wj[0].is_string()) {
                                    std::string u = wj[0].get<std::string>();
                                    if (u.rfind("file://", 0) == 0) u = u.substr(7);
                                    if (!u.empty() && fs::exists(u)) {
                                        wsPath = u;
                                    }
                                }
                            } catch (...) {}
                        }
                        sqlite3_finalize(stmt);
                    }
                    sqlite3_close(db);
                }
            }
        }

        std::vector<LsEndpoint> endpoints = discoverLsEndpoints();
        std::stable_sort(endpoints.begin(), endpoints.end(), [&](const LsEndpoint& a, const LsEndpoint& b) {
            bool aOwns = (!cid.empty() && cid != "new" && a.lockedConversationId == cid);
            bool bOwns = (!cid.empty() && cid != "new" && b.lockedConversationId == cid);
            if (aOwns != bOwns) return aOwns > bOwns;
            bool aWs = (!wsPath.empty() && a.cwd == wsPath);
            bool bWs = (!wsPath.empty() && b.cwd == wsPath);
            if (aWs != bWs) return aWs > bWs;
            if (a.isCli != b.isCli) return a.isCli > b.isCli;
            return a.isPreferredWorkspace > b.isPreferredWorkspace;
        });

        if (endpoints.empty() && (!hasCliDiskConversation || isLockedByRunningAgy)) {
            res.status = 503;
            res.set_content(nlohmann::json({
                {"status", "error"},
                {"error", "No active Antigravity language server or agy agent session found"}
            }).dump(2), "application/json");
            return;
        }

        // 3. Attempt direct ConnectRPC dispatch with conversation ownership & model verification
        auto sendRpcCascadeMessage = [&](const LsEndpoint& ep, const std::string& targetCid, std::string& outBody) -> bool {
            try {
                nlohmann::json cascadeCfg = resolveCascadeConfigForRpc(ep, targetCid, requestedModel, wsPath);
                if (!cascadeCfg.contains("plannerConfig") || !cascadeCfg["plannerConfig"].is_object() ||
                    cascadeCfg["plannerConfig"].value("planModel", "").empty()) {
                    return false;
                }
                httplib::Client cli("127.0.0.1", ep.port);
                cli.set_connection_timeout(1, 0);
                cli.set_read_timeout(5, 0);
                httplib::Headers hdrs = {
                    {"Content-Type", "application/json"},
                    {"x-codeium-csrf-token", ep.csrf}
                };
                nlohmann::json payload = {
                    {"cascadeId", targetCid},
                    {"items", nlohmann::json::array({{{"text", text}}})},
                    {"cascadeConfig", cascadeCfg}
                };
                auto rpcRes = cli.Post(
                    "/exa.language_server_pb.LanguageServerService/SendUserCascadeMessage",
                    hdrs,
                    payload.dump(),
                    "application/json"
                );
                if (rpcRes && rpcRes->status == 200) {
                    // Verify that the language server did not immediately reject the model with CORTEX_STEP_TYPE_ERROR_MESSAGE
                    auto checkRes = cli.Post(
                        "/exa.language_server_pb.LanguageServerService/GetCascadeTrajectory",
                        hdrs,
                        nlohmann::json({{"cascadeId", targetCid}}).dump(),
                        "application/json"
                    );
                    if (checkRes && checkRes->status == 200) {
                        auto tj = nlohmann::json::parse(checkRes->body, nullptr, false);
                        if (!tj.is_discarded() && tj.is_object() && tj.contains("trajectory") && tj["trajectory"].is_object()) {
                            const auto& steps = tj["trajectory"].value("steps", nlohmann::json::array());
                            if (!steps.empty() && steps.back().is_object() &&
                                steps.back().value("type", "") == "CORTEX_STEP_TYPE_ERROR_MESSAGE") {
                                std::string errCheck;
                                if (steps.back().contains("errorMessage") && steps.back()["errorMessage"].is_object() &&
                                    steps.back()["errorMessage"].contains("error") && steps.back()["errorMessage"]["error"].is_object()) {
                                    const auto& errObj = steps.back()["errorMessage"]["error"];
                                    errCheck = errObj.value("modelErrorMessage",
                                               errObj.value("shortError",
                                               errObj.value("userErrorMessage", "")));
                                }
                                if (errCheck.find("unknown model key") != std::string::npos ||
                                    errCheck.find("failed to construct executor") != std::string::npos) {
                                    return false;
                                }
                            }
                        }
                    }
                    outBody = rpcRes->body;
                    std::lock_guard<std::mutex> lk(*lastRpcMutex);
                    *lastRpcEndpoint = {ep.port, ep.csrf};
                    return true;
                }
            } catch (...) {}
            return false;
        };

        auto startRpcNewCascade = [&](const LsEndpoint& ep, std::string& outNewCid) -> bool {
            try {
                httplib::Client cli("127.0.0.1", ep.port);
                cli.set_connection_timeout(1, 0);
                cli.set_read_timeout(5, 0);
                httplib::Headers hdrs = {
                    {"Content-Type", "application/json"},
                    {"x-codeium-csrf-token", ep.csrf}
                };
                nlohmann::json payload = {
                    {"source", "CORTEX_TRAJECTORY_SOURCE_CLI"}
                };
                auto rpcRes = cli.Post(
                    "/exa.language_server_pb.LanguageServerService/StartCascade",
                    hdrs,
                    payload.dump(),
                    "application/json"
                );
                if (rpcRes && rpcRes->status == 200) {
                    auto sj = nlohmann::json::parse(rpcRes->body, nullptr, false);
                    if (!sj.is_discarded() && sj.is_object() && sj.contains("cascadeId") && sj["cascadeId"].is_string()) {
                        outNewCid = sj["cascadeId"].get<std::string>();
                        return !outNewCid.empty();
                    }
                }
            } catch (...) {}
            return false;
        };

        auto endpointOwnsConversation = [&](const LsEndpoint& ep, const std::string& targetCid, bool& outEndpointReachable) -> bool {
            outEndpointReachable = false;
            try {
                httplib::Client cli("127.0.0.1", ep.port);
                cli.set_connection_timeout(0, 500000);
                cli.set_read_timeout(2, 0);
                httplib::Headers hdrs = {
                    {"Content-Type", "application/json"},
                    {"x-codeium-csrf-token", ep.csrf}
                };
                auto metaRes = cli.Post(
                    "/exa.language_server_pb.LanguageServerService/GetConversationMetadata",
                    hdrs,
                    nlohmann::json({{"conversationId", targetCid}}).dump(),
                    "application/json"
                );
                if (metaRes && metaRes->status == 200) {
                    outEndpointReachable = true;
                    auto mj = nlohmann::json::parse(metaRes->body, nullptr, false);
                    if (!mj.is_discarded() && mj.is_object() && mj.contains("metadata")) {
                        return true;
                    }
                }
                auto allRes = cli.Post(
                    "/exa.language_server_pb.LanguageServerService/GetAllCascadeTrajectories",
                    hdrs,
                    "{}",
                    "application/json"
                );
                if (allRes && allRes->status == 200) {
                    outEndpointReachable = true;
                    auto aj = nlohmann::json::parse(allRes->body, nullptr, false);
                    if (!aj.is_discarded() && aj.is_object() &&
                        aj.contains("trajectorySummaries") && aj["trajectorySummaries"].is_object() &&
                        aj["trajectorySummaries"].contains(targetCid)) {
                        return true;
                    }
                }
            } catch (...) {}
            return false;
        };

        if (!cid.empty() && cid != "new") {
            const LsEndpoint* firstReachableEp = nullptr;
            for (const auto& ep : endpoints) {
                if (isCliConv && !ep.isCli) {
                    continue;
                }
                if (isLockedByRunningAgy) {
                    if (lockedHttpPort > 0 && ep.port != lockedHttpPort) {
                        continue;
                    }
                    if (lockedHttpPort <= 0 && ep.lockedConversationId != cid) {
                        continue;
                    }
                }
                bool reachable = false;
                if (isLockedByRunningAgy || endpointOwnsConversation(ep, cid, reachable)) {
                    std::string rpcOut;
                    if (sendRpcCascadeMessage(ep, cid, rpcOut)) {
                        nlohmann::json resp;
                        resp["status"] = "dispatched";
                        resp["agentId"] = agentId;
                        resp["conversationId"] = cid;
                        resp["output"] = rpcOut;
                        res.set_content(resp.dump(2), "application/json");
                        return;
                    }
                }
                if (reachable && !firstReachableEp) {
                    firstReachableEp = &ep;
                }
            }
            // If cid was an archived conversation not loaded in any live server, start a new cascade on the primary reachable server
            // (never start a new cascade if cid is an existing CLI disk conversation or locked by a running agy TUI)
            if (firstReachableEp != nullptr && !hasCliDiskConversation && !isLockedByRunningAgy) {
                std::string newCid;
                std::string rpcOut;
                if (startRpcNewCascade(*firstReachableEp, newCid) &&
                    sendRpcCascadeMessage(*firstReachableEp, newCid, rpcOut)) {
                    nlohmann::json resp;
                    resp["status"] = "dispatched";
                    resp["agentId"] = agentId;
                    resp["conversationId"] = newCid;
                    resp["output"] = rpcOut;
                    res.set_content(resp.dump(2), "application/json");
                    return;
                }
            }
        } else {
            for (const auto& ep : endpoints) {
                if (isCliConv && !ep.isCli) {
                    continue;
                }
                std::string newCid;
                std::string rpcOut;
                if (startRpcNewCascade(ep, newCid) && sendRpcCascadeMessage(ep, newCid, rpcOut)) {
                    nlohmann::json resp;
                    resp["status"] = "dispatched";
                    resp["agentId"] = agentId;
                    resp["conversationId"] = newCid;
                    resp["output"] = rpcOut;
                    res.set_content(resp.dump(2), "application/json");
                    return;
                }
            }
        }

        // 4. Fallback: invoke agy agentapi CLI binary
        auto runAgyCommand = [&](const LsEndpoint& ep, int& outExitCode, std::string& outOutput) -> bool {
            int pipeFd[2];
            if (pipe(pipeFd) != 0) {
                return false;
            }
            pid_t child = fork();
            if (child < 0) {
                close(pipeFd[0]);
                close(pipeFd[1]);
                return false;
            }
            if (child == 0) {
                close(pipeFd[0]);
                dup2(pipeFd[1], STDOUT_FILENO);
                dup2(pipeFd[1], STDERR_FILENO);
                close(pipeFd[1]);

                std::string portStr = std::to_string(ep.port);
                std::string lsAddr = "localhost:" + portStr;
                unsetenv("ANTIGRAVITY_CONVERSATION_ID");
                setenv("AGY_LS_PORT", portStr.c_str(), 1);
                setenv("AGY_LS_CSRF_TOKEN", ep.csrf.c_str(), 1);
                setenv("ANTIGRAVITY_LS_ADDRESS", lsAddr.c_str(), 1);
                setenv("ANTIGRAVITY_CSRF_TOKEN", ep.csrf.c_str(), 1);
                setenv("ANTIGRAVITY_PROJECT_ID", "default-cli-project", 1);

                std::string cliModelFlag = "--model=pro";
                if (requestedModel == "flash" || requestedModel == "MODEL_PLACEHOLDER_M322" ||
                    requestedModel == "gemini-3.8-flash-tiered") {
                    cliModelFlag = "--model=flash";
                } else if (requestedModel == "flash_lite") {
                    cliModelFlag = "--model=flash_lite";
                } else if (requestedModel == "pro") {
                    cliModelFlag = "--model=pro";
                } else if (!requestedModel.empty()) {
                    std::string mappedCliId = mapLabelToCliModelId(requestedModel);
                    if (!mappedCliId.empty()) {
                        cliModelFlag = "--model=" + mappedCliId;
                    }
                }

                std::vector<char*> argv;
                argv.push_back(const_cast<char*>(agyBin.c_str()));
                argv.push_back(const_cast<char*>("agentapi"));
                if (cid.empty() || cid == "new") {
                    argv.push_back(const_cast<char*>("new-conversation"));
                    argv.push_back(const_cast<char*>(cliModelFlag.c_str()));
                    argv.push_back(const_cast<char*>(text.c_str()));
                } else {
                    argv.push_back(const_cast<char*>("send-message"));
                    argv.push_back(const_cast<char*>(cid.c_str()));
                    argv.push_back(const_cast<char*>(text.c_str()));
                }
                argv.push_back(nullptr);

                execv(agyBin.c_str(), argv.data());
                _exit(127);
            }

            close(pipeFd[1]);
            outOutput.clear();
            char buf[1024];
            ssize_t n;
            while ((n = read(pipeFd[0], buf, sizeof(buf))) > 0) {
                outOutput.append(buf, static_cast<size_t>(n));
            }
            close(pipeFd[0]);

            int status = 0;
            int waitedMs = 0;
            while (waitedMs < 10000) {
                pid_t w = waitpid(child, &status, WNOHANG);
                if (w == child) {
                    outExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                waitedMs += 25;
            }
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            outExitCode = -1;
            return false;
        };

        std::string lastError = "Failed to dispatch prompt via agy agentapi";
        if (isLockedByRunningAgy) {
            std::string locDesc = !lockedTerminalPts.empty()
                ? lockedTerminalPts
                : (lockedHttpPort > 0 ? ("port " + std::to_string(lockedHttpPort)) : "active terminal");
            lastError = "Conversation is open in terminal (" + locDesc +
                        "), but its language server CSRF token is not yet registered";
        }
        for (const auto& ep : endpoints) {
            if (isCliConv && !ep.isCli) {
                continue;
            }
            if (isLockedByRunningAgy) {
                if (lockedHttpPort > 0 && ep.port != lockedHttpPort) {
                    continue;
                }
                if (lockedHttpPort <= 0 && ep.lockedConversationId != cid) {
                    continue;
                }
            }
            int exitCode = -1;
            std::string output;
            if (!runAgyCommand(ep, exitCode, output)) {
                lastError = "agy agentapi execution timed out or failed to spawn";
                continue;
            }

            nlohmann::json parsedOut = nlohmann::json::parse(output, nullptr, false);
            bool hasJsonError = (!parsedOut.is_discarded() && parsedOut.is_object() &&
                                 parsedOut.contains("error") && !parsedOut["error"].is_null() &&
                                 (!parsedOut["error"].is_string() || !parsedOut["error"].get<std::string>().empty()));

            if (exitCode == 0 && !hasJsonError) {
                std::string resolvedCid = (cid == "new") ? "" : cid;
                if (!parsedOut.is_discarded() && parsedOut.is_object()) {
                    if (parsedOut.contains("conversationId") && parsedOut["conversationId"].is_string()) {
                        resolvedCid = parsedOut["conversationId"].get<std::string>();
                    } else if (parsedOut.contains("response") && parsedOut["response"].is_object()) {
                        const auto& rObj = parsedOut["response"];
                        if (rObj.contains("conversationId") && rObj["conversationId"].is_string()) {
                            resolvedCid = rObj["conversationId"].get<std::string>();
                        } else if (rObj.contains("cascadeId") && rObj["cascadeId"].is_string()) {
                            resolvedCid = rObj["cascadeId"].get<std::string>();
                        } else if (rObj.contains("newConversation") && rObj["newConversation"].is_object()) {
                            const auto& nc = rObj["newConversation"];
                            if (nc.contains("conversationId") && nc["conversationId"].is_string()) {
                                resolvedCid = nc["conversationId"].get<std::string>();
                            }
                        }
                    }
                }

                nlohmann::json resp;
                resp["status"] = "dispatched";
                resp["agentId"] = agentId;
                resp["conversationId"] = resolvedCid;
                resp["output"] = output;
                res.set_content(resp.dump(2), "application/json");
                return;
            }

            if (hasJsonError && parsedOut["error"].is_string()) {
                lastError = parsedOut["error"].get<std::string>();
            } else if (!output.empty()) {
                lastError = output;
            } else {
                lastError = "agy exited with code " + std::to_string(exitCode);
            }
        }

        // 5. Fallback for unlocked CLI conversations on disk: resume directly via detached `agy --conversation <cid> -p <prompt>`
        if (hasCliDiskConversation && !isLockedByRunningAgy && !cid.empty() && cid != "new") {
            std::string cliModelId = mapLabelToCliModelId(desiredModel);
            std::thread([agyBin, cid, text, wsPath, cliModelId]() {
                pid_t child = fork();
                if (child == 0) {
                    setsid();
                    if (!wsPath.empty()) {
                        if (chdir(wsPath.c_str()) != 0) {}
                    }
                    int devNull = open("/dev/null", O_RDWR);
                    if (devNull >= 0) {
                        dup2(devNull, STDIN_FILENO);
                        dup2(devNull, STDOUT_FILENO);
                        dup2(devNull, STDERR_FILENO);
                        if (devNull > 2) close(devNull);
                    }
                    unsetenv("ANTIGRAVITY_CONVERSATION_ID");
                    unsetenv("AGY_LS_PORT");
                    unsetenv("AGY_LS_CSRF_TOKEN");
                    unsetenv("ANTIGRAVITY_LS_ADDRESS");
                    unsetenv("ANTIGRAVITY_CSRF_TOKEN");
                    std::string modelArg = "--model=" + cliModelId;
                    std::vector<char*> argv;
                    argv.push_back(const_cast<char*>(agyBin.c_str()));
                    argv.push_back(const_cast<char*>("--conversation"));
                    argv.push_back(const_cast<char*>(cid.c_str()));
                    argv.push_back(const_cast<char*>("--dangerously-skip-permissions"));
                    if (!cliModelId.empty()) {
                        argv.push_back(const_cast<char*>(modelArg.c_str()));
                    }
                    argv.push_back(const_cast<char*>("-p"));
                    argv.push_back(const_cast<char*>(text.c_str()));
                    argv.push_back(nullptr);
                    execv(agyBin.c_str(), argv.data());
                    _exit(127);
                } else if (child > 0) {
                    int status = 0;
                    waitpid(child, &status, 0);
                }
            }).detach();

            nlohmann::json resp;
            resp["status"] = "dispatched";
            resp["agentId"] = agentId;
            resp["conversationId"] = cid;
            resp["output"] = "{\"status\":\"resumed_cli_conversation\"}";
            res.set_content(resp.dump(2), "application/json");
            return;
        }

        res.status = 503;
        res.set_content(nlohmann::json({
            {"status", "error"},
            {"error", lastError}
        }).dump(2), "application/json");
    });

    _server->Get("/api/agent/artifacts", [getIdeBrainDir, getCliBrainDir](
                                             const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string id = req.get_param_value("id");
        if (id.empty() || id.find("..") != std::string::npos || id.find('/') != std::string::npos) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing or invalid conversation id\"}", "application/json");
            return;
        }

        nlohmann::json artifacts = nlohmann::json::array();
        auto scanArtifactsDir = [&](const fs::path& dir) {
            if (!fs::exists(dir) || !fs::is_directory(dir)) return;
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (ec) break;
                if (entry.is_regular_file() && entry.path().extension() == ".md") {
                    artifacts.push_back({
                        {"name", entry.path().filename().string()},
                        {"path", entry.path().string()},
                        {"isDir", false}
                    });
                }
            }
        };
        scanArtifactsDir(fs::path(getIdeBrainDir()) / id);
        scanArtifactsDir(fs::path(getCliBrainDir()) / id);

        nlohmann::json resp;
        resp["artifacts"] = artifacts;
        res.set_content(resp.dump(2), "application/json");
    });

    _server->Get("/api/workspace/tree", [getWorkspaceRoot](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        fs::path root = getWorkspaceRoot();
        std::string relSubPath = req.get_param_value("path");
        if (relSubPath.find("..") != std::string::npos) {
            res.status = 400;
            res.set_content("{\"error\":\"Invalid path traversal\"}", "application/json");
            return;
        }

        fs::path startDir = relSubPath.empty() ? root : (root / relSubPath);
        if (!fs::exists(startDir) || !fs::is_directory(startDir)) {
            res.status = 404;
            res.set_content("{\"error\":\"Directory not found\"}", "application/json");
            return;
        }

        auto isSkippedDir = [](const std::string& name) -> bool {
            if (name.empty() || name[0] == '.') return true;
            return name == "build" || name == "node_modules" ||
                   name == "third_party" || name == "__pycache__";
        };

        std::function<nlohmann::json(const fs::path&, int)> buildTree;
        buildTree = [&](const fs::path& dirPath, int depth) -> nlohmann::json {
            nlohmann::json nodes = nlohmann::json::array();
            if (depth > 6) {
                return nodes;
            }
            std::error_code ec;
            std::vector<fs::directory_entry> entries;
            for (const auto& entry : fs::directory_iterator(dirPath, ec)) {
                if (ec) break;
                entries.push_back(entry);
            }
            std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
                bool aDir = a.is_directory();
                bool bDir = b.is_directory();
                if (aDir != bDir) return aDir > bDir;
                return a.path().filename().string() < b.path().filename().string();
            });

            for (const auto& entry : entries) {
                std::string fname = entry.path().filename().string();
                if (isSkippedDir(fname)) continue;

                std::error_code typeEc;
                bool isDir = entry.is_directory(typeEc);
                uint64_t fsize = 0;
                if (!isDir && entry.is_regular_file(typeEc)) {
                    std::error_code sizeEc;
                    fsize = static_cast<uint64_t>(entry.file_size(sizeEc));
                }

                nlohmann::json node;
                node["name"] = fname;
                node["path"] = fs::relative(entry.path(), root, typeEc).string();
                node["isDir"] = isDir;
                node["size"] = fsize;
                if (isDir && depth < 6) {
                    node["children"] = buildTree(entry.path(), depth + 1);
                }
                nodes.push_back(std::move(node));
            }
            return nodes;
        };

        try {
            nlohmann::json tree = buildTree(startDir, 1);
            nlohmann::json resp;
            resp["workspace"] = root.string();
            resp["path"] = relSubPath;
            resp["tree"] = tree;
            res.set_content(resp.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    _server->Get("/api/workspace/file", [getWorkspaceRoot](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string relPath = req.get_param_value("path");
        if (relPath.empty() || relPath.find("..") != std::string::npos) {
            res.status = 400;
            res.set_content("{\"error\":\"Invalid path\"}", "application/json");
            return;
        }
        fs::path target = getWorkspaceRoot() / relPath;
        if (!fs::exists(target) || fs::is_directory(target)) {
            res.status = 404;
            res.set_content("{\"error\":\"File not found\"}", "application/json");
            return;
        }
        std::ifstream f(target);
        std::stringstream buf;
        buf << f.rdbuf();
        nlohmann::json resp;
        resp["path"] = relPath;
        resp["content"] = buf.str();
        resp["language"] = target.extension().string();
        res.set_content(resp.dump(2), "application/json");
    });

    _server->Post("/api/workspace/file", [getWorkspaceRoot](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        try {
            auto j = nlohmann::json::parse(req.body);
            std::string relPath = j.value("path", "");
            std::string content = j.value("content", "");
            if (relPath.empty() || relPath.find("..") != std::string::npos) {
                res.status = 400;
                res.set_content("{\"error\":\"Invalid path\"}", "application/json");
                return;
            }
            fs::path target = getWorkspaceRoot() / relPath;
            std::ofstream f(target, std::ios::trunc);
            if (!f.is_open()) {
                res.status = 500;
                res.set_content("{\"error\":\"Failed to open file for writing\"}", "application/json");
                return;
            }
            f << content;
            res.set_content("{\"status\":\"ok\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });


    _server->Get("/sse", [this](const httplib::Request& req, httplib::Response& res) {
        if (!_mcpServer) {
            res.status = 503;
            res.set_content("MCP service not configured", "text/plain");
            return;
        }

        std::string profile;
        if (req.has_param("profile")) {
            profile = req.get_param_value("profile");
        } else if (req.has_header("X-Aimon-Profile")) {
            profile = req.get_header_value("X-Aimon-Profile");
        }

        std::string sessionId = generateSessionId();
        auto session = std::make_shared<SseSession>();
        session->id = sessionId;
        session->profile = profile;

        {
            std::lock_guard<std::mutex> lock(_sessionsMutex);
            _sseSessions[sessionId] = session;
        }

        std::string remoteIp = req.remote_addr;
        TaskRegistry::getInstance().registerSession(sessionId, remoteIp);

        std::cout << "[WebServer] New SSE client connected from " << remoteIp
                  << ", session: " << sessionId;
        if (!profile.empty()) {
            std::cout << ", profile: " << profile;
        }
        std::cout << std::endl;

        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("Access-Control-Allow-Origin", "*");

        auto provider = [session, initialSent = false](size_t offset, httplib::DataSink& sink) mutable -> bool {
            (void)offset;
            if (!sink.is_writable() || session->closed.load()) {
                return false;
            }

            if (!initialSent) {
                std::string endpointMsg = "event: endpoint\ndata: /message?sessionId=" + session->id + (!session->profile.empty() ? ("&profile=" + session->profile) : "") + "\n\n";
                initialSent = true;
                return sink.write(endpointMsg.data(), endpointMsg.size());
            }

            std::unique_lock<std::mutex> lock(session->mutex);
            session->cv.wait_for(lock, std::chrono::seconds(15), [&]() {
                return !session->messageQueue.empty() || session->closed.load();
            });

            if (session->closed.load()) {
                return false;
            }

            if (session->messageQueue.empty()) {
                std::string ping = ": keepalive\n\n";
                return sink.write(ping.data(), ping.size());
            }

            std::string msg = session->messageQueue.front();
            session->messageQueue.pop();
            lock.unlock();

            std::string sseData = "event: message\ndata: " + msg + "\n\n";
            return sink.write(sseData.data(), sseData.size());
        };

        auto releaser = [this, sessionId, session](bool success) {
            (void)success;
            session->closed.store(true);
            session->cv.notify_all();
            std::lock_guard<std::mutex> lock(_sessionsMutex);
            _sseSessions.erase(sessionId);
            TaskRegistry::getInstance().removeSession(sessionId);
            std::cout << "[WebServer] SSE client disconnected, session: " << sessionId << std::endl;
        };

        res.set_chunked_content_provider("text/event-stream", provider, releaser);
    });

    auto handleMcpMessage = [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, Accept, Mcp-Session-Id, mcp-session-id, X-Aimon-Profile");
        if (!_mcpServer) {
            res.status = 503;
            res.set_content("MCP service not configured", "text/plain");
            return;
        }

        // Extract session ID from query parameters or headers
        std::string sessionId;
        if (req.has_param("sessionId")) {
            sessionId = req.get_param_value("sessionId");
        } else if (req.has_param("session_id")) {
            sessionId = req.get_param_value("session_id");
        } else if (req.has_header("Mcp-Session-Id")) {
            sessionId = req.get_header_value("Mcp-Session-Id");
        } else if (req.has_header("mcp-session-id")) {
            sessionId = req.get_header_value("mcp-session-id");
        }

        // Strip trailing carriage return, newline, or whitespace
        while (!sessionId.empty() && (sessionId.back() == '\r' || sessionId.back() == '\n' || sessionId.back() == ' ')) {
            sessionId.pop_back();
        }

        // Check if an SSE session exists for push notification
        std::shared_ptr<SseSession> session;
        if (!sessionId.empty()) {
            std::lock_guard<std::mutex> lock(_sessionsMutex);
            auto it = _sseSessions.find(sessionId);
            if (it != _sseSessions.end()) {
                session = it->second;
            }
        }

        // Extract profile from query params, headers, or existing SSE session
        std::string profile;
        if (req.has_param("profile")) {
            profile = req.get_param_value("profile");
        } else if (req.has_header("X-Aimon-Profile")) {
            profile = req.get_header_value("X-Aimon-Profile");
        } else if (session && !session->profile.empty()) {
            profile = session->profile;
        }

        nlohmann::json reqJson;
        try {
            reqJson = nlohmann::json::parse(req.body);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(std::string("Invalid JSON: ") + e.what(), "text/plain");
            return;
        }

        std::string method = reqJson.value("method", "");
        std::cout << "[WebServer] MCP POST: method=" << method << " session='" << sessionId << "'";
        if (!profile.empty()) {
            std::cout << " profile='" << profile << "'";
        }
        std::cout << std::endl;

        if (!sessionId.empty()) {
            TaskRegistry::getInstance().touchSession(sessionId);
        }

        if (method == "initialize" && !sessionId.empty()) {
            if (reqJson.contains("params") && reqJson["params"].is_object()) {
                const auto& p = reqJson["params"];
                if (p.contains("clientInfo") && p["clientInfo"].is_object()) {
                    const auto& ci = p["clientInfo"];
                    std::string cName = ci.value("name", "MCP Client");
                    std::string cVer = ci.value("version", "");
                    TaskRegistry::getInstance().updateSessionClientInfo(sessionId, cName, cVer);
                    std::cout << "[WebServer] MCP client identified for session " << sessionId
                              << ": name='" << cName << "', version='" << cVer << "'" << std::endl;
                }

                // Auto-scope session profile from workspace if session has no explicit profile
                if (session && session->profile.empty()) {
                    std::string detected;
                    if (p.contains("rootUri") && p["rootUri"].is_string()) {
                        detected = McpServer::detectProfileFromWorkspace(p["rootUri"].get<std::string>());
                    }
                    if (detected.empty() && p.contains("rootPath") && p["rootPath"].is_string()) {
                        detected = McpServer::detectProfileFromWorkspace(p["rootPath"].get<std::string>());
                    }
                    if (detected.empty() && p.contains("workspaceFolders") && p["workspaceFolders"].is_array()) {
                        for (const auto& wf : p["workspaceFolders"]) {
                            if (wf.contains("uri") && wf["uri"].is_string()) {
                                detected = McpServer::detectProfileFromWorkspace(wf["uri"].get<std::string>());
                                if (!detected.empty()) break;
                            }
                        }
                    }
                    if (!detected.empty()) {
                        session->profile = detected;
                        profile = detected;  // update for this request too
                        std::cout << "[WebServer] Auto-scoped session " << sessionId
                                  << " profile to: " << detected << std::endl;
                    }
                }
            }
        }

        if (method == "tools/call" && !sessionId.empty()) {
            if (reqJson.contains("params") && reqJson["params"].is_object()) {
                auto& p = reqJson["params"];
                std::string tName = p.value("name", "");
                if (tName == "register_agent_task" ||
                    tName == "agent_check_inbox" ||
                    tName == "agent_send_reply") {
                    if (!p.contains("arguments") || !p["arguments"].is_object()) {
                        p["arguments"] = nlohmann::json::object();
                    }
                    p["arguments"]["sse_session_id"] = sessionId;
                }
            }
        }

        // Process message through MCP server engine with profile filtering
        nlohmann::json respJson = _mcpServer->handleMessage(reqJson, profile);
        std::string respStr = respJson.is_null() ? "{}" : respJson.dump();

        if (session && !respJson.is_null()) {
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->messageQueue.push(respStr);
            }
            session->cv.notify_one();
        }

        // Return JSON response directly in HTTP response body (supports Streamable HTTP)
        if (!sessionId.empty()) {
            res.set_header("Mcp-Session-Id", sessionId);
        }
        res.status = 200;
        res.set_content(respStr, "application/json");
    };

    _server->Post("/message", handleMcpMessage);
    _server->Post("/messages", handleMcpMessage);
    _server->Post("/sse", handleMcpMessage);

    auto handleOptions = [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, Accept, Mcp-Session-Id, mcp-session-id");
        res.status = 204;
    };
    _server->Options("/sse", handleOptions);
    _server->Options("/message", handleOptions);
    _server->Options("/messages", handleOptions);

    // Usage Insights & Workflow-Coaching REST API endpoints
    _server->Get("/api/v1/insights", [this](const httplib::Request& req, httplib::Response& res) {
        if (!_insightEngine) {
            res.set_content(nlohmann::json({{"findings", nlohmann::json::array()}}).dump(), "application/json");
            return;
        }
        std::string plat = req.has_param("platform") ? req.get_param_value("platform") : "";
        int days = 30;
        if (req.has_param("days")) {
            try { days = std::stoi(req.get_param_value("days")); } catch (...) {}
        }
        std::string minSev = req.has_param("min_severity") ? req.get_param_value("min_severity") : "";
        res.set_content(_insightEngine->getInsights(plat, days, minSev).dump(), "application/json");
    });

    _server->Get("/api/v1/insights/report", [this](const httplib::Request& req, httplib::Response& res) {
        if (!_insightEngine) {
            res.set_content(nlohmann::json({{"rows", nlohmann::json::array()}}).dump(), "application/json");
            return;
        }
        std::string plat = req.has_param("platform") ? req.get_param_value("platform") : "";
        int days = 30;
        if (req.has_param("days")) {
            try { days = std::stoi(req.get_param_value("days")); } catch (...) {}
        }
        std::string groupBy = req.has_param("group_by") ? req.get_param_value("group_by") : "project";
        res.set_content(_insightEngine->getUsageReport(plat, days, groupBy).dump(), "application/json");
    });

    _server->Get("/api/v1/insights/digest", [this](const httplib::Request& req, httplib::Response& res) {
        int days = 7;
        if (req.has_param("days")) {
            try { days = std::stoi(req.get_param_value("days")); } catch (...) {}
        }
        std::string md = _insightEngine ? _insightEngine->generateWeeklyDigest(days) : "";
        res.set_content(nlohmann::json({{"days", days}, {"markdown", md}}).dump(), "application/json");
    });

    _server->Get("/api/v1/insights/compare", [this](const httplib::Request& req, httplib::Response& res) {
        std::string det = req.has_param("detector") ? req.get_param_value("detector") : "cache_hit_collapse";
        std::string plat = req.has_param("platform") ? req.get_param_value("platform") : "";
        int64_t splitEpoch = 0;
        if (req.has_param("split_epoch")) {
            try { splitEpoch = std::stoll(req.get_param_value("split_epoch")); } catch (...) {}
        }
        if (!_insightEngine) {
            res.set_content(nlohmann::json({{"detector", det}, {"verdict", "insufficient_data"}}).dump(), "application/json");
            return;
        }
        res.set_content(_insightEngine->compareExperiment(det, plat, splitEpoch).dump(), "application/json");
    });

    _server->Post("/api/v1/insights/export", [this](const httplib::Request& req, httplib::Response& res) {
        if (!_insightEngine) {
            res.set_content(nlohmann::json({{"files", nlohmann::json::array()}}).dump(), "application/json");
            return;
        }
        nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
        std::string outDir;
        std::string fmt = "jsonl";
        int days = 90;
        bool anon = false;
        if (body.is_object()) {
            outDir = body.value("output_dir", "");
            fmt = body.value("format", "jsonl");
            days = body.value("days", 90);
            anon = body.value("anonymize_projects", false);
        }
        res.set_content(_insightEngine->exportDataset(outDir, fmt, days, anon).dump(), "application/json");
    });

    _server->Post("/api/v1/insights/rebuild", [this](const httplib::Request& req, httplib::Response& res) {
        if (!_insightEngine) {
            res.set_content(nlohmann::json({{"ok", false}}).dump(), "application/json");
            return;
        }
        nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
        bool full = body.is_object() ? body.value("full", false) : false;
        std::string histPath = PathUtils::getAimonConfigDir() + "/history.db";
        std::string telPath = PathUtils::getAimonConfigDir() + "/telemetry.db";
        bool ok = _insightEngine->rebuildFromConfig(histPath, telPath, full);
        res.set_content(nlohmann::json({{"ok", ok}}).dump(), "application/json");
    });

    // --- Windows EDA/CAD & Local CPU Accelerator Bridge REST API Endpoints ---

    _server->Get("/api/eda/status", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");

        EdaBridgeStatus st = _edaBridge ? _edaBridge->getStatus() : _stateStore.getStatus().eda;
        if (!st.configSnapshot.is_object()) {
            EdaBridgeConfig fallbackCfg = _configManager ? _configManager->getConfig().eda : EdaBridgeConfig();
            st.configSnapshot = fallbackCfg.toJson(true);
            st.authConfigured = !fallbackCfg.authSecret.empty();
        }
        st.configSnapshot["auth_secret"] = st.authConfigured ? "***" : "";
        res.set_content(st.toJson().dump(2), "application/json");
    });

    _server->Post("/api/eda/config", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }

        nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            res.status = 400;
            res.set_content(nlohmann::json({{"ok", false}, {"error", "Malformed JSON configuration body"}}).dump(2),
                            "application/json");
            return;
        }

        EdaBridgeConfig updatedCfg = _edaBridge
            ? _edaBridge->getConfig()
            : (_configManager ? _configManager->getConfig().eda : EdaBridgeConfig());
        updatedCfg.fromJson(body);

        std::string valErr;
        if (!updatedCfg.validate(valErr)) {
            res.status = 400;
            res.set_content(nlohmann::json({{"ok", false}, {"error", valErr}}).dump(2), "application/json");
            return;
        }

        if (_configManager) {
            _configManager->getConfig().eda = updatedCfg;
            _configManager->save();
            _configManager->save(PathUtils::getDefaultConfigFilePath());
            _configManager->saveLibConfig(PathUtils::getDefaultLibConfigFilePath());
        }

        EdaBridgeStatus st;
        if (_edaBridge) {
            _edaBridge->setConfig(updatedCfg);
            st = _edaBridge->getStatus();
        } else {
            EdaBridge tempBridge(updatedCfg);
            st = tempBridge.getStatus();
        }
        st.configSnapshot = updatedCfg.toJson(true);
        st.configSnapshot["auth_secret"] = st.authConfigured ? "***" : "";

        AggregateStatus agg = _stateStore.getStatus();
        agg.eda = st;
        _stateStore.update(agg);

        if (_mcpServer) {
            _mcpServer->notifyToolsListChanged();
        }

        nlohmann::json out = st.toJson();
        out["ok"] = true;
        res.set_content(out.dump(2), "application/json");
    });

    _server->Post("/api/eda/probe", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }

        EdaBridgeStatus st = _edaBridge ? _edaBridge->probeNow() : _stateStore.getStatus().eda;
        if (st.configSnapshot.is_object()) {
            st.configSnapshot["auth_secret"] = st.authConfigured ? "***" : "";
        }

        AggregateStatus agg = _stateStore.getStatus();
        agg.eda = st;
        _stateStore.update(agg);

        nlohmann::json out = st.toJson();
        out["ok"] = st.connected;
        res.set_content(out.dump(2), "application/json");
    });
}

bool WebServer::start(bool async) {
    if (_running) return true;

    setupRoutes();
    _running = true;

    _server->set_read_timeout(1, 0);
    _server->set_write_timeout(5, 0);

    std::cout << "[WebServer] Dashboard available at http://"
              << _config.host << ":" << _config.port << std::endl;
    if (_mcpServer) {
        std::cout << "[WebServer] MCP SSE endpoint available at http://"
                  << _config.host << ":" << _config.port << "/sse" << std::endl;
    }

    if (_config.port == 0) {
        int boundPort = _server->bind_to_any_port(_config.host.c_str());
        if (boundPort <= 0) {
            std::cerr << "[WebServer] Failed to bind to any port on " << _config.host << std::endl;
            _running = false;
            return false;
        }
        _config.port = boundPort;
        if (async) {
            _thread = std::make_unique<std::thread>([this]() {
                if (!_server->listen_after_bind()) {
                    _running = false;
                }
            });
            _server->wait_until_ready();
            return _running;
        } else {
            return _server->listen_after_bind();
        }
    }

    if (async) {
        _thread = std::make_unique<std::thread>([this]() {
            if (!_server->listen(_config.host.c_str(), _config.port)) {
                std::cerr << "[WebServer] Failed to bind to "
                          << _config.host << ":" << _config.port << std::endl;
                _running = false;
            }
        });
        _server->wait_until_ready();
        return _running;
    } else {
        return _server->listen(_config.host.c_str(), _config.port);
    }
}

void WebServer::broadcastSseNotification(const std::string& jsonRpcNotification) {
    std::lock_guard<std::mutex> lock(_sessionsMutex);
    for (auto& pair : _sseSessions) {
        auto& session = pair.second;
        if (session && !session->closed.load()) {
            {
                std::lock_guard<std::mutex> sLock(session->mutex);
                session->messageQueue.push(jsonRpcNotification);
            }
            session->cv.notify_one();
        }
    }
}

void WebServer::stop() {
    if (!_running) return;

    _running = false;
    {
        std::lock_guard<std::mutex> lock(_sessionsMutex);
        for (auto& pair : _sseSessions) {
            pair.second->closed.store(true);
            pair.second->cv.notify_all();
        }
        _sseSessions.clear();
    }

    if (_server) {
        _server->stop();
    }
    if (_thread && _thread->joinable()) {
        _thread->join();
        _thread.reset();
    }
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
