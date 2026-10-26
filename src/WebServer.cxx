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
#include <openssl/sha.h>

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
#include <sys/socket.h>
#include <sys/un.h>
#include <pwd.h>
#include <signal.h>

namespace fs = std::filesystem;

namespace aimon {

static std::string extractOrGenerateRequestId(const httplib::Request& req) {
    std::string reqId = req.get_header_value("X-Request-Id");
    if (!reqId.empty()) {
        std::string sanitized;
        sanitized.reserve(reqId.size());
        for (char c : reqId) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
                sanitized.push_back(c);
            }
            if (sanitized.size() >= 64) break;
        }
        if (!sanitized.empty()) return sanitized;
    }
    static std::atomic<uint64_t> s_reqCounter{1};
    uint64_t seq = s_reqCounter.fetch_add(1);
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream oss;
    oss << "srv-" << std::hex << nowMs << "-" << seq;
    return oss.str();
}

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

        std::string comm;
        try {
            std::ifstream commFile("/proc/" + dname + "/comm");
            if (!commFile.is_open()) continue;
            std::getline(commFile, comm);
        } catch (...) {
            continue;
        }
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
                const std::string deletedSuffix = " (deleted)";
                if (target.size() > deletedSuffix.size() &&
                    target.compare(target.size() - deletedSuffix.size(), deletedSuffix.size(), deletedSuffix) == 0) {
                    target.resize(target.size() - deletedSuffix.size());
                }

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
        std::vector<pid_t> fastChildPids;
        std::set<pid_t> seenFastPids;

        auto readChildPids = [](const std::string& pStr) -> std::vector<pid_t> {
            std::vector<pid_t> cPids;
            std::error_code ec;
            std::string taskPath = "/proc/" + pStr + "/task";
            if (!fs::exists(taskPath, ec)) return cPids;
            for (const auto& tentry : fs::directory_iterator(taskPath, ec)) {
                if (ec || !tentry.is_directory()) continue;
                try {
                    std::ifstream cf(tentry.path() / "children");
                    if (!cf.is_open()) continue;
                    std::string word;
                    while (cf >> word) {
                        if (!word.empty() && std::all_of(word.begin(), word.end(), ::isdigit)) {
                            try { cPids.push_back(std::stoi(word)); } catch (...) {}
                        }
                    }
                } catch (...) {}
            }
            return cPids;
        };

        for (const auto& agyProc : agyProcs) {
            if (agyProc.pidStr.empty()) continue;
            auto ch1 = readChildPids(agyProc.pidStr);
            for (pid_t c1 : ch1) {
                if (seenFastPids.insert(c1).second) {
                    fastChildPids.push_back(c1);
                }
                auto ch2 = readChildPids(std::to_string(c1));
                for (pid_t c2 : ch2) {
                    if (seenFastPids.insert(c2).second) {
                        fastChildPids.push_back(c2);
                    }
                }
            }
        }

        std::vector<pid_t> procPids;
        procPids.insert(procPids.end(), fastChildPids.begin(), fastChildPids.end());

        std::vector<pid_t> generalPids;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator("/proc", ec)) {
            if (ec || !entry.is_directory()) continue;
            std::string dname = entry.path().filename().string();
            if (!dname.empty() && std::all_of(dname.begin(), dname.end(), ::isdigit)) {
                try {
                    pid_t p = std::stoi(dname);
                    if (seenFastPids.find(p) == seenFastPids.end()) {
                        generalPids.push_back(p);
                    }
                } catch (...) {}
            }
        }
        std::sort(generalPids.begin(), generalPids.end(), std::greater<pid_t>());
        procPids.insert(procPids.end(), generalPids.begin(), generalPids.end());

        for (pid_t pid : procPids) {
            std::string envContent;
            try {
                std::ifstream envFile("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
                if (!envFile.is_open()) continue;
                envContent.assign((std::istreambuf_iterator<char>(envFile)),
                                  std::istreambuf_iterator<char>());
            } catch (...) {
                continue;
            }
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

static std::string getClaudeProjectsDir() {
    const char* envClaude = std::getenv("AIMON_CLAUDE_PROJECTS_DIR");
    if (envClaude && *envClaude) {
        return std::string(envClaude);
    }
    const char* envCli = std::getenv("AIMON_CLI_BRAIN_DIR");
    const char* envIde = std::getenv("AIMON_IDE_BRAIN_DIR");
    if ((envCli && *envCli) || (envIde && *envIde)) {
        return "";
    }
    return PathUtils::expandHome("~/.claude/projects");
}

static std::string getCursorProjectsDir() {
    const char* envCursor = std::getenv("AIMON_CURSOR_PROJECTS_DIR");
    if (envCursor && *envCursor) {
        return std::string(envCursor);
    }
    const char* envCli = std::getenv("AIMON_CLI_BRAIN_DIR");
    const char* envIde = std::getenv("AIMON_IDE_BRAIN_DIR");
    if ((envCli && *envCli) || (envIde && *envIde)) {
        return "";
    }
    return PathUtils::expandHome("~/.cursor/projects");
}

static std::string getClaudeBin() {
    const char* envBin = std::getenv("AIMON_CLAUDE_BIN");
    if (envBin && *envBin) {
        return std::string(envBin);
    }
    return PathUtils::expandHome("~/.local/bin/claude");
}

static std::string getCursorBin() {
    const char* envBin = std::getenv("AIMON_CURSOR_BIN");
    if (envBin && *envBin) {
        return std::string(envBin);
    }
    return PathUtils::expandHome("~/.local/bin/cursor-agent");
}

static uint64_t parseIso8601ToEpochSec(const std::string& iso) {
    if (iso.size() < 19) return 0;
    int yr = 0, mo = 0, dy = 0, hr = 0, mn = 0, sc = 0;
    if (std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &yr, &mo, &dy, &hr, &mn, &sc) != 6) {
        return 0;
    }
    std::tm tmVal{};
    tmVal.tm_year = yr - 1900;
    tmVal.tm_mon = mo - 1;
    tmVal.tm_mday = dy;
    tmVal.tm_hour = hr;
    tmVal.tm_min = mn;
    tmVal.tm_sec = sc;
    time_t epoch = timegm(&tmVal);
    return (epoch > 0) ? static_cast<uint64_t>(epoch) : 0;
}

static std::string cleanProviderUserPrompt(const std::string& raw) {
    std::string text = raw;
    auto uqStart = text.find("<user_query>");
    auto uqEnd = text.find("</user_query>");
    if (uqStart != std::string::npos && uqEnd != std::string::npos && uqEnd >= uqStart + 12) {
        text = text.substr(uqStart + 12, uqEnd - (uqStart + 12));
    }
    auto urStart = text.find("<USER_REQUEST>");
    auto urEnd = text.find("</USER_REQUEST>");
    if (urStart != std::string::npos && urEnd != std::string::npos && urEnd >= urStart + 14) {
        text = text.substr(urStart + 14, urEnd - (urStart + 14));
    }
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
    stripBlock("<CONTEXT_SUMMARY>", "</CONTEXT_SUMMARY>");
    stripBlock("<ide_opened_file>", "</ide_opened_file>");
    stripBlock("<ide_selection>", "</ide_selection>");
    stripBlock("<system-reminder>", "</system-reminder>");
    stripBlock("<ADDITIONAL_METADATA>", "</ADDITIONAL_METADATA>");
    stripBlock("<USER_SETTINGS_CHANGE>", "</USER_SETTINGS_CHANGE>");
    stripBlock("<SYSTEM_MESSAGE>", "</SYSTEM_MESSAGE>");
    stripBlock("<task-notification>", "</task-notification>");
    stripBlock("<agent-message>", "</agent-message>");
    stripBlock("<local-command-caveat>", "</local-command-caveat>");
    stripBlock("<command-name>", "</command-name>");
    stripBlock("<command-message>", "</command-message>");
    stripBlock("<command-args>", "</command-args>");
    stripBlock("<local-command-stdout>", "</local-command-stdout>");
    stripBlock("<local-command-stderr>", "</local-command-stderr>");
    stripBlock("<tools-used-before-summary>", "</tools-used-before-summary>");
    stripBlock("<timestamp>", "</timestamp>");
    stripBlock("<available_subagent_types>", "</available_subagent_types>");
    stripBlock("<available_subagent_models>", "</available_subagent_models>");
    stripBlock("<dynamic_tools>", "</dynamic_tools>");
    stripBlock("<dynamic_tool_catalog>", "</dynamic_tool_catalog>");
    stripBlock("<dynamic_tool_namespaces>", "</dynamic_tool_namespaces>");
    stripBlock("<mcp_meta_tools>", "</mcp_meta_tools>");
    stripBlock("<mcp_meta_tool_servers>", "</mcp_meta_tool_servers>");

    while (true) {
        auto rPos = text.find("Read the output file to retrieve the result:");
        if (rPos == std::string::npos) break;
        auto nl = text.find('\n', rPos);
        if (nl == std::string::npos) {
            text.erase(rPos);
            break;
        }
        text.erase(rPos, nl - rPos + 1);
    }

    auto first = text.find_first_not_of(" \n\r\t");
    if (first == std::string::npos) return "";
    auto last = text.find_last_not_of(" \n\r\t");
    std::string trimmed = text.substr(first, last - first + 1);
    if (trimmed == "Briefly inform the user about the task result and perform any follow-up actions (if needed). If there's no follow-ups needed, don't explicitly say that.") {
        return "";
    }
    return trimmed;
}

static nlohmann::json normalizeQuestionsJson(const nlohmann::json& rawQ) {
    nlohmann::json parsedQ = rawQ;
    if (rawQ.is_string()) {
        auto p = nlohmann::json::parse(rawQ.get<std::string>(), nullptr, false);
        if (!p.is_discarded()) {
            parsedQ = std::move(p);
        }
    }
    nlohmann::json items = nlohmann::json::array();
    if (parsedQ.is_array()) {
        items = parsedQ;
    } else if (parsedQ.is_object() && parsedQ.contains("question")) {
        items.push_back(parsedQ);
    }

    nlohmann::json out = nlohmann::json::array();
    for (const auto& qObj : items) {
        if (!qObj.is_object()) continue;
        std::string qText = qObj.value("question", "");
        bool isMulti = qObj.value("is_multi_select", qObj.value("isMultiSelect", false));
        nlohmann::json optsStrArr = nlohmann::json::array();
        nlohmann::json optObjsArr = nlohmann::json::array();

        if (qObj.contains("options")) {
            nlohmann::json rawOpts = qObj["options"];
            if (rawOpts.is_string()) {
                auto po = nlohmann::json::parse(rawOpts.get<std::string>(), nullptr, false);
                if (!po.is_discarded()) {
                    rawOpts = std::move(po);
                }
            }
            if (rawOpts.is_array()) {
                for (size_t idx = 0; idx < rawOpts.size(); ++idx) {
                    const auto& opt = rawOpts[idx];
                    if (opt.is_string()) {
                        std::string txt = opt.get<std::string>();
                        optsStrArr.push_back(txt);
                        optObjsArr.push_back({{"id", std::to_string(idx + 1)}, {"text", txt}});
                    } else if (opt.is_object()) {
                        std::string txt = opt.value("text", opt.value("label", ""));
                        std::string idStr = opt.value("id", std::to_string(idx + 1));
                        optsStrArr.push_back(txt);
                        optObjsArr.push_back({{"id", idStr}, {"text", txt}});
                    }
                }
            }
        }

        if (qText.empty() && optsStrArr.empty()) continue;
        out.push_back({
            {"question", qText},
            {"options", optsStrArr},
            {"optionObjects", optObjsArr},
            {"is_multi_select", isMulti},
            {"isMultiSelect", isMulti}
        });
    }
    return out;
}

static void inferWorkspaceFromProjectSlug(const std::string& slug,
                                          std::string& outWorkspacePath,
                                          std::string& outWorkspaceShort) {
    if (slug.empty()) return;
    std::string s = slug;
    if (!s.empty() && s[0] == '-') s.erase(0, 1);
    auto workPos = s.find("-work-");
    if (workPos != std::string::npos && workPos + 6 < s.size()) {
        std::string sub = s.substr(workPos + 6);
        if (outWorkspaceShort.empty()) {
            outWorkspaceShort = sub;
        }
        if (outWorkspacePath.empty()) {
            std::string prefix = "/" + s.substr(0, workPos + 5);
            std::replace(prefix.begin(), prefix.end(), '-', '/');
            outWorkspacePath = prefix + "/" + sub;
        }
        return;
    }
    if (outWorkspaceShort.empty()) {
        auto dash = s.rfind('-');
        outWorkspaceShort = (dash != std::string::npos && dash + 1 < s.size()) ? s.substr(dash + 1) : s;
    }
}

static std::string getCursorHookStateDir() {
    const char* envHook = std::getenv("AIMON_CURSOR_HOOK_STATE_DIR");
    if (envHook && *envHook) {
        return std::string(envHook);
    }
    const char* envState = std::getenv("AIMON_CURSOR_STATE_DIR");
    if (envState && *envState) {
        return std::string(envState);
    }
    std::string cursorProjects = getCursorProjectsDir();
    if (!cursorProjects.empty()) {
        return (fs::path(cursorProjects).parent_path() / "hooks" / "state").string();
    }
    const char* envCli = std::getenv("AIMON_CLI_BRAIN_DIR");
    const char* envIde = std::getenv("AIMON_IDE_BRAIN_DIR");
    if ((envCli && *envCli) || (envIde && *envIde)) {
        return "";
    }
    return PathUtils::expandHome("~/.cursor/hooks/state");
}

static bool readCursorHookStateJson(const std::string& cid, nlohmann::json& outJson) {
    if (cid.empty() || cid.find("..") != std::string::npos || cid.find('/') != std::string::npos) {
        return false;
    }
    std::vector<fs::path> candidates;
    std::string hookDir = getCursorHookStateDir();
    if (!hookDir.empty()) {
        candidates.push_back(fs::path(hookDir) / (cid + ".json"));
    }
    std::string cursorProjects = getCursorProjectsDir();
    if (!cursorProjects.empty()) {
        fs::path alt = fs::path(cursorProjects).parent_path() / "hooks" / "state" / (cid + ".json");
        if (candidates.empty() || candidates.front() != alt) {
            candidates.push_back(alt);
        }
    }
    const char* envCli = std::getenv("AIMON_CLI_BRAIN_DIR");
    const char* envIde = std::getenv("AIMON_IDE_BRAIN_DIR");
    if ((!envCli || !*envCli) && (!envIde || !*envIde)) {
        candidates.push_back(fs::path(PathUtils::expandHome("~/.cursor/hooks/state")) / (cid + ".json"));
    }
    std::error_code ec;
    for (const auto& p : candidates) {
        if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) continue;
        std::ifstream f(p);
        if (!f.is_open()) continue;
        auto j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        outJson = std::move(j);
        return true;
    }
    return false;
}

static std::string getClaudeHookStateDir() {
    const char* envHook = std::getenv("AIMON_CLAUDE_HOOK_STATE_DIR");
    if (envHook && *envHook) {
        return std::string(envHook);
    }
    const char* envState = std::getenv("AIMON_CLAUDE_STATE_DIR");
    if (envState && *envState) {
        return std::string(envState);
    }
    std::string claudeProjects = getClaudeProjectsDir();
    if (!claudeProjects.empty()) {
        return (fs::path(claudeProjects).parent_path() / "hooks" / "state").string();
    }
    const char* envCli = std::getenv("AIMON_CLI_BRAIN_DIR");
    const char* envIde = std::getenv("AIMON_IDE_BRAIN_DIR");
    if ((envCli && *envCli) || (envIde && *envIde)) {
        return "";
    }
    return PathUtils::expandHome("~/.claude/hooks/state");
}

static bool readClaudeHookStateJson(const std::string& cid, nlohmann::json& outJson) {
    if (cid.empty() || cid.find("..") != std::string::npos || cid.find('/') != std::string::npos) {
        return false;
    }
    std::vector<fs::path> candidates;
    std::string hookDir = getClaudeHookStateDir();
    if (!hookDir.empty()) {
        candidates.push_back(fs::path(hookDir) / (cid + ".json"));
    }
    std::string claudeProjects = getClaudeProjectsDir();
    if (!claudeProjects.empty()) {
        fs::path alt = fs::path(claudeProjects).parent_path() / "hooks" / "state" / (cid + ".json");
        if (candidates.empty() || candidates.front() != alt) {
            candidates.push_back(alt);
        }
    }
    const char* envCli = std::getenv("AIMON_CLI_BRAIN_DIR");
    const char* envIde = std::getenv("AIMON_IDE_BRAIN_DIR");
    if ((!envCli || !*envCli) && (!envIde || !*envIde)) {
        candidates.push_back(fs::path(PathUtils::expandHome("~/.claude/hooks/state")) / (cid + ".json"));
    }
    std::error_code ec;
    for (const auto& p : candidates) {
        if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) continue;
        std::ifstream f(p);
        if (!f.is_open()) continue;
        auto j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        outJson = std::move(j);
        return true;
    }
    return false;
}

static void collectCursorSubagentIdsAndParentMtimes(
    const std::string& cursorRoot,
    std::set<std::string>& outSubagentIds,
    std::map<std::string, uint64_t>& outParentMaxSubagentTs) {
    if (cursorRoot.empty()) return;
    std::error_code ec;
    if (!fs::exists(cursorRoot, ec) || !fs::is_directory(cursorRoot, ec)) return;

    auto fileMtimeSec = [](const fs::path& p) -> uint64_t {
        try {
            auto ftime = fs::last_write_time(p);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
            return (secs > 0) ? static_cast<uint64_t>(secs) : 0;
        } catch (...) {
            return 0;
        }
    };

    std::map<std::string, std::string> subToParent;
    for (const auto& projEntry : fs::directory_iterator(cursorRoot, ec)) {
        if (ec || !projEntry.is_directory()) continue;
        fs::path transDir = projEntry.path() / "agent-transcripts";
        std::error_code tEc;
        if (!fs::exists(transDir, tEc) || !fs::is_directory(transDir, tEc)) continue;
        for (const auto& convEntry : fs::directory_iterator(transDir, tEc)) {
            if (tEc || !convEntry.is_directory()) continue;
            std::string parentId = convEntry.path().filename().string();
            if (parentId.empty() || parentId[0] == '.' || parentId == "subagents") continue;
            fs::path subDir = convEntry.path() / "subagents";
            std::error_code sEc;
            if (fs::exists(subDir, sEc) && fs::is_directory(subDir, sEc)) {
                for (const auto& subFile : fs::directory_iterator(subDir, sEc)) {
                    if (sEc || !subFile.is_regular_file() || subFile.path().extension() != ".jsonl") continue;
                    std::string subId = subFile.path().stem().string();
                    if (subId.empty()) continue;
                    outSubagentIds.insert(subId);
                    subToParent[subId] = parentId;
                    uint64_t mSec = fileMtimeSec(subFile.path());
                    if (mSec > outParentMaxSubagentTs[parentId]) {
                        outParentMaxSubagentTs[parentId] = mSec;
                    }
                }
            }
        }
        // Second loop over transDir to fold top-level duplicate subagent mtimes into parent and check hook state
        for (const auto& convEntry : fs::directory_iterator(transDir, tEc)) {
            if (tEc || !convEntry.is_directory()) continue;
            std::string cid = convEntry.path().filename().string();
            if (cid.empty() || cid[0] == '.' || cid == "subagents") continue;
            fs::path topJsonl = convEntry.path() / (cid + ".jsonl");
            auto spIt = subToParent.find(cid);
            if (spIt != subToParent.end()) {
                uint64_t topSec = fileMtimeSec(topJsonl);
                if (topSec > outParentMaxSubagentTs[spIt->second]) {
                    outParentMaxSubagentTs[spIt->second] = topSec;
                }
            }
            nlohmann::json hookJson;
            std::error_code topEc;
            bool topExists = fs::exists(topJsonl, topEc) && fs::is_regular_file(topJsonl, topEc);
            bool hasSubagentsDir = fs::exists(convEntry.path() / "subagents", topEc) &&
                                   fs::is_directory(convEntry.path() / "subagents", topEc);
            if (readCursorHookStateJson(cid, hookJson)) {
                if (!hasSubagentsDir &&
                    (hookJson.value("is_subagent", false) || !hookJson.value("parent_session_id", "").empty())) {
                    outSubagentIds.insert(cid);
                } else if (topExists && !hasSubagentsDir && hookJson.value("status", "") != "RUNNING") {
                    int hTurns = hookJson.value("turns", -1);
                    int hStep = hookJson.value("step", 0);
                    if (hTurns == 0 && hStep > 0) {
                        bool hasAssistant = false;
                        std::ifstream tjf(topJsonl);
                        if (tjf.is_open()) {
                            std::string line;
                            while (std::getline(tjf, line)) {
                                if (line.find("\"assistant\"") != std::string::npos) {
                                    hasAssistant = true;
                                    break;
                                }
                            }
                        }
                        if (!hasAssistant) {
                            outSubagentIds.insert(cid);
                        }
                    }
                }
            }
        }
    }

    // Also check <cursorRoot>/../chats/*/<cid>/store.db for subagentInfo / parentAgentId
    fs::path chatsDir = fs::path(cursorRoot).parent_path() / "chats";
    std::error_code cEc;
    if (fs::exists(chatsDir, cEc) && fs::is_directory(chatsDir, cEc)) {
        for (const auto& wsEntry : fs::directory_iterator(chatsDir, cEc)) {
            if (cEc || !wsEntry.is_directory()) continue;
            std::error_code uEc;
            for (const auto& chatEntry : fs::directory_iterator(wsEntry.path(), uEc)) {
                if (uEc || !chatEntry.is_directory()) continue;
                std::string cid = chatEntry.path().filename().string();
                fs::path storeDb = chatEntry.path() / "store.db";
                if (!fs::exists(storeDb, uEc) || !fs::is_regular_file(storeDb, uEc)) continue;
                sqlite3* db = nullptr;
                if (sqlite3_open_v2(storeDb.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK && db) {
                    sqlite3_busy_timeout(db, 100);
                    sqlite3_stmt* stmt = nullptr;
                    if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key='0'", -1, &stmt, nullptr) == SQLITE_OK && stmt) {
                        if (sqlite3_step(stmt) == SQLITE_ROW) {
                            const char* hexVal = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                            if (hexVal && *hexVal) {
                                std::string hexStr(hexVal);
                                std::string decoded;
                                decoded.reserve(hexStr.size() / 2);
                                for (size_t i = 0; i + 1 < hexStr.size(); i += 2) {
                                    char byte = static_cast<char>(std::strtol(hexStr.substr(i, 2).c_str(), nullptr, 16));
                                    decoded.push_back(byte);
                                }
                                if (decoded.find("\"subagentInfo\"") != std::string::npos ||
                                    decoded.find("\"parentAgentId\"") != std::string::npos) {
                                    outSubagentIds.insert(cid);
                                }
                            }
                        }
                        sqlite3_finalize(stmt);
                    }
                    sqlite3_close(db);
                } else if (db) {
                    sqlite3_close(db);
                }
            }
        }
    }
}

struct NormalizedProviderTranscript {
    nlohmann::json steps = nlohmann::json::array();
    int totalSteps = 0;
    uint64_t lastActivitySec = 0;
    std::string firstPrompt;
    std::string lastPrompt;
    int lastPromptStepIndex = -1;
    nlohmann::json userPrompts = nlohmann::json::array();
    std::string workspacePath;
    std::string workspaceShort;
    std::string model;
    std::string status = "DONE";
    bool hasValidTurns = false;
    bool interactive = true;
    std::string nonInteractiveReason;
    nlohmann::json lastStepObj;
};

struct LiveProviderSessionInfo {
    pid_t ownerPid = 0;
    std::string muxType;        // "screen" | "tmux" | "zellij" | "wezterm" | "kitty" | ""
    std::string muxSession;     // e.g. "21504.cursor" or "/tmp/tmux-1000/default" or "default"
    std::string muxTarget;      // e.g. "0" (screen win) or "%3" (tmux pane) or pane_id
    std::string screenSession;  // Backwards-compatible alias populated when muxType == "screen"
    bool isIdeRendererSession = false;
    bool hasCliStore = true;
};

struct SessionInteractivityInfo {
    bool interactive = true;
    std::string nonInteractiveReason;
};

static bool isClaudeIdeExtensionTranscript(const fs::path& jsonlPath);
static bool hasCursorCliStoreForConversation(const fs::path& homeDir,
                                             const std::string& conversationId);
static LiveProviderSessionInfo discoverLiveProviderSession(
    const std::string& targetProvider,
    const std::string& cid,
    const std::string& wsDir,
    const std::string& existingTranscriptPath);
static SessionInteractivityInfo evaluateSessionInteractivity(
    const std::string& provider,
    const std::string& conversationId,
    const fs::path& homeDir,
    const LiveProviderSessionInfo& liveInfo,
    const fs::path& transcriptPath = fs::path());

static std::string getCliBrainDir() {
    const char* envCli = std::getenv("AIMON_CLI_BRAIN_DIR");
    if (envCli && *envCli) {
        return std::string(envCli);
    }
    return PathUtils::expandHome("~/.gemini/antigravity-cli/brain");
}

static std::string getIdeBrainDir() {
    const char* envIde = std::getenv("AIMON_IDE_BRAIN_DIR");
    if (envIde && *envIde) {
        return std::string(envIde);
    }
    return PathUtils::expandHome("~/.gemini/antigravity-ide/brain");
}

static std::string formatCompactionTokens(uint64_t tokens) {
    char buf[64];
    if (tokens >= 1000000) {
        std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(tokens) / 1000000.0);
        return std::string(buf);
    } else if (tokens >= 1000) {
        std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(tokens) / 1000.0);
        return std::string(buf);
    }
    return std::to_string(tokens);
}

static std::string formatCompactionDuration(int64_t seconds) {
    if (seconds >= 60) {
        return std::to_string(seconds / 60) + "m " + std::to_string(seconds % 60) + "s";
    }
    return std::to_string(std::max<int64_t>(0, seconds)) + "s";
}

static std::string joinCompactionStatsParts(int64_t stepsSummarized, uint64_t beforeTokens, uint64_t afterTokens, uint64_t summaryTokens, int64_t durationSec) {
    std::vector<std::string> parts;
    if (stepsSummarized > 0) {
        parts.push_back(std::to_string(stepsSummarized) + (stepsSummarized == 1 ? " step summarized" : " steps summarized"));
    }
    if (summaryTokens > 0) {
        if (beforeTokens > 0 && afterTokens > 0) {
            parts.push_back("context " + formatCompactionTokens(beforeTokens) + " → ~" + formatCompactionTokens(afterTokens) + " (" + formatCompactionTokens(summaryTokens) + " summary)");
        } else {
            parts.push_back("~" + formatCompactionTokens(summaryTokens) + " summary");
        }
    } else if (beforeTokens > 0 && afterTokens > 0) {
        parts.push_back("context " + formatCompactionTokens(beforeTokens) + " → ~" + formatCompactionTokens(afterTokens));
    }
    if (durationSec >= 0) {
        parts.push_back("took " + formatCompactionDuration(durationSec));
    }
    if (parts.empty()) {
        return "Context compacted";
    }
    std::string res;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) res += " · ";
        res += parts[i];
    }
    return res;
}

struct AntigravityCheckpointStats {
    int64_t stepsSummarized = 0;
    uint64_t beforeTokens = 0;
    uint64_t afterTokens = 0;
    uint64_t summaryTokens = 0;
    int64_t durationSec = -1;
};

static bool pbReadVarint(const uint8_t*& ptr, const uint8_t* end, uint64_t& val) {
    val = 0;
    int shift = 0;
    while (ptr < end && shift < 64) {
        uint8_t byte = *ptr++;
        val |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool pbSkipField(const uint8_t*& ptr, const uint8_t* end, uint32_t wireType) {
    if (wireType == 0) { // Varint
        uint64_t dummy = 0;
        return pbReadVarint(ptr, end, dummy);
    } else if (wireType == 1) { // 64-bit
        if (end - ptr < 8) return false;
        ptr += 8;
        return true;
    } else if (wireType == 2) { // Length-delimited
        uint64_t len = 0;
        if (!pbReadVarint(ptr, end, len)) return false;
        if (len > static_cast<uint64_t>(end - ptr)) return false;
        ptr += len;
        return true;
    } else if (wireType == 5) { // 32-bit
        if (end - ptr < 4) return false;
        ptr += 4;
        return true;
    }
    return false;
}

static bool pbDecodeTimestamp(const uint8_t* ptr, const uint8_t* end, double& tsSec) {
    int64_t seconds = 0;
    int32_t nanos = 0;
    while (ptr < end) {
        uint64_t tag = 0;
        if (!pbReadVarint(ptr, end, tag)) return false;
        uint32_t fieldNum = static_cast<uint32_t>(tag >> 3);
        uint32_t wireType = static_cast<uint32_t>(tag & 0x07);
        if (fieldNum == 1 && wireType == 0) {
            uint64_t v = 0;
            if (!pbReadVarint(ptr, end, v)) return false;
            seconds = static_cast<int64_t>(v);
        } else if (fieldNum == 2 && wireType == 0) {
            uint64_t v = 0;
            if (!pbReadVarint(ptr, end, v)) return false;
            nanos = static_cast<int32_t>(v);
        } else {
            if (!pbSkipField(ptr, end, wireType)) return false;
        }
    }
    tsSec = static_cast<double>(seconds) + static_cast<double>(nanos) / 1e9;
    return true;
}

static bool loadAntigravityCheckpointStatsFromDb(const std::string& cid, std::map<int, AntigravityCheckpointStats>& outByStepIndex) {
    if (cid.empty()) return false;
    fs::path p1 = fs::path(getCliBrainDir()).parent_path() / "conversations" / (cid + ".db");
    fs::path p2 = fs::path(getIdeBrainDir()).parent_path() / "conversations" / (cid + ".db");
    std::string dbPath;
    std::error_code ec;
    if (fs::exists(p1, ec)) {
        dbPath = p1.string();
    } else if (fs::exists(p2, ec)) {
        dbPath = p2.string();
    } else {
        return false;
    }

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 100);

    struct DbCloser {
        sqlite3* d;
        ~DbCloser() { if (d) sqlite3_close(d); }
    } closer{db};

    uint64_t overheadTokens = 0;
    uint64_t chatMessagesTokens = 0;
    uint64_t usageInput = 0;
    uint64_t usageCacheRead = 0;

    sqlite3_stmt* stmt = nullptr;
    const char* sqlGen = "SELECT data FROM gen_metadata ORDER BY size DESC LIMIT 1";
    if (sqlite3_prepare_v2(db, sqlGen, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlGen = "SELECT data FROM gen_metadata ORDER BY length(data) DESC LIMIT 1";
        sqlite3_prepare_v2(db, sqlGen, -1, &stmt, nullptr);
    }

    if (stmt) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const void* rawBlob = sqlite3_column_blob(stmt, 0);
            int blobBytes = sqlite3_column_bytes(stmt, 0);
            if (rawBlob && blobBytes > 0) {
                const uint8_t* ptr = static_cast<const uint8_t*>(rawBlob);
                const uint8_t* end = ptr + blobBytes;
                while (ptr < end) {
                    uint64_t tag = 0;
                    if (!pbReadVarint(ptr, end, tag)) break;
                    uint32_t fn = static_cast<uint32_t>(tag >> 3);
                    uint32_t wt = static_cast<uint32_t>(tag & 7);
                    if (fn == 1 && wt == 2) {
                        uint64_t csLen = 0;
                        if (!pbReadVarint(ptr, end, csLen)) break;
                        if (csLen > static_cast<uint64_t>(end - ptr)) break;
                        const uint8_t* csPtr = ptr;
                        const uint8_t* csEnd = ptr + csLen;
                        ptr += csLen;

                        while (csPtr < csEnd) {
                            uint64_t csTag = 0;
                            if (!pbReadVarint(csPtr, csEnd, csTag)) break;
                            uint32_t csFn = static_cast<uint32_t>(csTag >> 3);
                            uint32_t csWt = static_cast<uint32_t>(csTag & 7);
                            if (csFn == 4 && csWt == 2) {
                                uint64_t uLen = 0;
                                if (!pbReadVarint(csPtr, csEnd, uLen)) break;
                                if (uLen > static_cast<uint64_t>(csEnd - csPtr)) break;
                                const uint8_t* uPtr = csPtr;
                                const uint8_t* uEnd = csPtr + uLen;
                                csPtr += uLen;
                                while (uPtr < uEnd) {
                                    uint64_t uTag = 0;
                                    if (!pbReadVarint(uPtr, uEnd, uTag)) break;
                                    uint32_t uFn = static_cast<uint32_t>(uTag >> 3);
                                    uint32_t uWt = static_cast<uint32_t>(uTag & 7);
                                    if (uFn == 2 && uWt == 0) {
                                        pbReadVarint(uPtr, uEnd, usageInput);
                                    } else if (uFn == 5 && uWt == 0) {
                                        pbReadVarint(uPtr, uEnd, usageCacheRead);
                                    } else {
                                        if (!pbSkipField(uPtr, uEnd, uWt)) break;
                                    }
                                }
                            } else if (csFn == 9 && csWt == 2) {
                                uint64_t cwLen = 0;
                                if (!pbReadVarint(csPtr, csEnd, cwLen)) break;
                                if (cwLen > static_cast<uint64_t>(csEnd - csPtr)) break;
                                const uint8_t* cwPtr = csPtr;
                                const uint8_t* cwEnd = csPtr + cwLen;
                                csPtr += cwLen;

                                while (cwPtr < cwEnd) {
                                    uint64_t cwTag = 0;
                                    if (!pbReadVarint(cwPtr, cwEnd, cwTag)) break;
                                    uint32_t cwFn = static_cast<uint32_t>(cwTag >> 3);
                                    uint32_t cwWt = static_cast<uint32_t>(cwTag & 7);
                                    if (cwFn == 10 && cwWt == 2) {
                                        uint64_t f10Len = 0;
                                        if (!pbReadVarint(cwPtr, cwEnd, f10Len)) break;
                                        if (f10Len > static_cast<uint64_t>(cwEnd - cwPtr)) break;
                                        const uint8_t* f10Ptr = cwPtr;
                                        const uint8_t* f10End = cwPtr + f10Len;
                                        cwPtr += f10Len;

                                        while (f10Ptr < f10End) {
                                            uint64_t f10Tag = 0;
                                            if (!pbReadVarint(f10Ptr, f10End, f10Tag)) break;
                                            uint32_t f10Fn = static_cast<uint32_t>(f10Tag >> 3);
                                            uint32_t f10Wt = static_cast<uint32_t>(f10Tag & 7);
                                            if (f10Fn == 3 && f10Wt == 2) {
                                                uint64_t f3Len = 0;
                                                if (!pbReadVarint(f10Ptr, f10End, f3Len)) break;
                                                if (f3Len > static_cast<uint64_t>(f10End - f10Ptr)) break;
                                                const uint8_t* f3Ptr = f10Ptr;
                                                const uint8_t* f3End = f10Ptr + f3Len;
                                                f10Ptr += f3Len;

                                                while (f3Ptr < f3End) {
                                                    uint64_t f3Tag = 0;
                                                    if (!pbReadVarint(f3Ptr, f3End, f3Tag)) break;
                                                    uint32_t f3Fn = static_cast<uint32_t>(f3Tag >> 3);
                                                    uint32_t f3Wt = static_cast<uint32_t>(f3Tag & 7);
                                                    if (f3Fn == 1 && f3Wt == 2) {
                                                        uint64_t sLen = 0;
                                                        if (!pbReadVarint(f3Ptr, f3End, sLen)) break;
                                                        if (sLen > static_cast<uint64_t>(f3End - f3Ptr)) break;
                                                        const uint8_t* sPtr = f3Ptr;
                                                        const uint8_t* sEnd = f3Ptr + sLen;
                                                        f3Ptr += sLen;

                                                        uint64_t sType = 0;
                                                        uint64_t sTokens = 0;
                                                        while (sPtr < sEnd) {
                                                            uint64_t sTag = 0;
                                                            if (!pbReadVarint(sPtr, sEnd, sTag)) break;
                                                            uint32_t sF = static_cast<uint32_t>(sTag >> 3);
                                                            uint32_t sW = static_cast<uint32_t>(sTag & 7);
                                                            if (sF == 2 && sW == 0) {
                                                                pbReadVarint(sPtr, sEnd, sType);
                                                            } else if (sF == 4 && sW == 0) {
                                                                pbReadVarint(sPtr, sEnd, sTokens);
                                                            } else {
                                                                if (!pbSkipField(sPtr, sEnd, sW)) break;
                                                            }
                                                        }
                                                        if (sType == 7) {
                                                            chatMessagesTokens += sTokens;
                                                        } else {
                                                            overheadTokens += sTokens;
                                                        }
                                                    } else {
                                                        if (!pbSkipField(f3Ptr, f3End, f3Wt)) break;
                                                    }
                                                }
                                            } else {
                                                if (!pbSkipField(f10Ptr, f10End, f10Wt)) break;
                                            }
                                        }
                                    } else {
                                        if (!pbSkipField(cwPtr, cwEnd, cwWt)) break;
                                    }
                                }
                            } else {
                                if (!pbSkipField(csPtr, csEnd, csWt)) break;
                            }
                        }
                    } else {
                        if (!pbSkipField(ptr, end, wt)) break;
                    }
                }
            }
        }
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }

    if (overheadTokens == 0 && chatMessagesTokens > 0 && (usageInput + usageCacheRead) > chatMessagesTokens) {
        overheadTokens = (usageInput + usageCacheRead) - chatMessagesTokens;
    }

    const char* sqlSteps = "SELECT idx, step_type, metadata, step_payload FROM steps WHERE step_type IN (14, 23) ORDER BY idx";
    if (sqlite3_prepare_v2(db, sqlSteps, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
        int lastStep14Idx = -999;
        double lastStep14CreatedSec = 0.0;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int idx = sqlite3_column_int(stmt, 0);
            int stepType = sqlite3_column_int(stmt, 1);
            const void* metaBlob = sqlite3_column_blob(stmt, 2);
            int metaBytes = sqlite3_column_bytes(stmt, 2);
            const void* payloadBlob = sqlite3_column_blob(stmt, 3);
            int payloadBytes = sqlite3_column_bytes(stmt, 3);

            if (stepType == 14) {
                if (metaBlob && metaBytes > 0) {
                    const uint8_t* mPtr = static_cast<const uint8_t*>(metaBlob);
                    const uint8_t* mEnd = mPtr + metaBytes;
                    while (mPtr < mEnd) {
                        uint64_t mTag = 0;
                        if (!pbReadVarint(mPtr, mEnd, mTag)) break;
                        uint32_t mFn = static_cast<uint32_t>(mTag >> 3);
                        uint32_t mWt = static_cast<uint32_t>(mTag & 7);
                        if (mFn == 1 && mWt == 2) {
                            uint64_t tsLen = 0;
                            if (!pbReadVarint(mPtr, mEnd, tsLen)) break;
                            if (tsLen > static_cast<uint64_t>(mEnd - mPtr)) break;
                            double tsSec = 0.0;
                            if (pbDecodeTimestamp(mPtr, mPtr + tsLen, tsSec)) {
                                lastStep14Idx = idx;
                                lastStep14CreatedSec = tsSec;
                            }
                            mPtr += tsLen;
                        } else {
                            if (!pbSkipField(mPtr, mEnd, mWt)) break;
                        }
                    }
                }
            } else if (stepType == 23) {
                AntigravityCheckpointStats st;

                if (payloadBlob && payloadBytes > 0) {
                    const uint8_t* pPtr = static_cast<const uint8_t*>(payloadBlob);
                    const uint8_t* pEnd = pPtr + payloadBytes;
                    while (pPtr < pEnd) {
                        uint64_t pTag = 0;
                        if (!pbReadVarint(pPtr, pEnd, pTag)) break;
                        uint32_t pFn = static_cast<uint32_t>(pTag >> 3);
                        uint32_t pWt = static_cast<uint32_t>(pTag & 7);
                        if (pFn == 30 && pWt == 2) {
                            uint64_t cpLen = 0;
                            if (!pbReadVarint(pPtr, pEnd, cpLen)) break;
                            if (cpLen > static_cast<uint64_t>(pEnd - pPtr)) break;
                            const uint8_t* cpPtr = pPtr;
                            const uint8_t* cpEnd = pPtr + cpLen;
                            pPtr += cpLen;

                            uint64_t startIdx = 0;
                            uint64_t endIdx = 0;
                            while (cpPtr < cpEnd) {
                                uint64_t cpTag = 0;
                                if (!pbReadVarint(cpPtr, cpEnd, cpTag)) break;
                                uint32_t cpFn = static_cast<uint32_t>(cpTag >> 3);
                                uint32_t cpWt = static_cast<uint32_t>(cpTag & 7);
                                if (cpFn == 11 && cpWt == 0) {
                                    pbReadVarint(cpPtr, cpEnd, startIdx);
                                } else if (cpFn == 12 && cpWt == 0) {
                                    pbReadVarint(cpPtr, cpEnd, endIdx);
                                } else {
                                    if (!pbSkipField(cpPtr, cpEnd, cpWt)) break;
                                }
                            }
                            if (endIdx > startIdx) {
                                st.stepsSummarized = static_cast<int64_t>(endIdx - startIdx);
                            }
                        } else {
                            if (!pbSkipField(pPtr, pEnd, pWt)) break;
                        }
                    }
                }

                if (metaBlob && metaBytes > 0) {
                    const uint8_t* mPtr = static_cast<const uint8_t*>(metaBlob);
                    const uint8_t* mEnd = mPtr + metaBytes;
                    double createdSec = 0.0;
                    double completedSec = 0.0;

                    while (mPtr < mEnd) {
                        uint64_t mTag = 0;
                        if (!pbReadVarint(mPtr, mEnd, mTag)) break;
                        uint32_t mFn = static_cast<uint32_t>(mTag >> 3);
                        uint32_t mWt = static_cast<uint32_t>(mTag & 7);
                        if (mFn == 1 && mWt == 2) {
                            uint64_t tsLen = 0;
                            if (!pbReadVarint(mPtr, mEnd, tsLen)) break;
                            if (tsLen > static_cast<uint64_t>(mEnd - mPtr)) break;
                            pbDecodeTimestamp(mPtr, mPtr + tsLen, createdSec);
                            mPtr += tsLen;
                        } else if (mFn == 8 && mWt == 2) {
                            uint64_t tsLen = 0;
                            if (!pbReadVarint(mPtr, mEnd, tsLen)) break;
                            if (tsLen > static_cast<uint64_t>(mEnd - mPtr)) break;
                            pbDecodeTimestamp(mPtr, mPtr + tsLen, completedSec);
                            mPtr += tsLen;
                        } else if (mFn == 28 && mWt == 2) {
                            uint64_t gLen = 0;
                            if (!pbReadVarint(mPtr, mEnd, gLen)) break;
                            if (gLen > static_cast<uint64_t>(mEnd - mPtr)) break;
                            const uint8_t* gPtr = mPtr;
                            const uint8_t* gEnd = mPtr + gLen;
                            mPtr += gLen;

                            while (gPtr < gEnd) {
                                uint64_t gTag = 0;
                                if (!pbReadVarint(gPtr, gEnd, gTag)) break;
                                uint32_t gFn = static_cast<uint32_t>(gTag >> 3);
                                uint32_t gWt = static_cast<uint32_t>(gTag & 7);
                                if (gFn == 2 && gWt == 2) {
                                    uint64_t f2Len = 0;
                                    if (!pbReadVarint(gPtr, gEnd, f2Len)) break;
                                    if (f2Len > static_cast<uint64_t>(gEnd - gPtr)) break;
                                    const uint8_t* f2Ptr = gPtr;
                                    const uint8_t* f2End = gPtr + f2Len;
                                    gPtr += f2Len;

                                    uint64_t inTok = 0;
                                    uint64_t outTok = 0;
                                    uint64_t cacheTok = 0;
                                    uint64_t thinkTok = 0;
                                    uint64_t respTok = 0;

                                    auto parseUsageFields = [&](const uint8_t* uP, const uint8_t* uE) {
                                        while (uP < uE) {
                                            uint64_t uTag = 0;
                                            if (!pbReadVarint(uP, uE, uTag)) break;
                                            uint32_t uFn = static_cast<uint32_t>(uTag >> 3);
                                            uint32_t uWt = static_cast<uint32_t>(uTag & 7);
                                            if (uFn == 2 && uWt == 0) {
                                                pbReadVarint(uP, uE, inTok);
                                            } else if (uFn == 3 && uWt == 0) {
                                                pbReadVarint(uP, uE, outTok);
                                            } else if (uFn == 5 && uWt == 0) {
                                                pbReadVarint(uP, uE, cacheTok);
                                            } else if (uFn == 9 && uWt == 0) {
                                                pbReadVarint(uP, uE, thinkTok);
                                            } else if (uFn == 10 && uWt == 0) {
                                                pbReadVarint(uP, uE, respTok);
                                            } else {
                                                if (!pbSkipField(uP, uE, uWt)) break;
                                            }
                                        }
                                    };

                                    const uint8_t* checkPtr = f2Ptr;
                                    bool hasNestedField2Usage = false;
                                    while (checkPtr < f2End) {
                                        uint64_t cTag = 0;
                                        if (!pbReadVarint(checkPtr, f2End, cTag)) break;
                                        uint32_t cFn = static_cast<uint32_t>(cTag >> 3);
                                        uint32_t cWt = static_cast<uint32_t>(cTag & 7);
                                        if (cFn == 2 && cWt == 2) {
                                            hasNestedField2Usage = true;
                                            uint64_t subLen = 0;
                                            if (pbReadVarint(checkPtr, f2End, subLen) && subLen <= static_cast<uint64_t>(f2End - checkPtr)) {
                                                parseUsageFields(checkPtr, checkPtr + subLen);
                                            }
                                            break;
                                        } else {
                                            if (!pbSkipField(checkPtr, f2End, cWt)) break;
                                        }
                                    }
                                    if (!hasNestedField2Usage) {
                                        parseUsageFields(f2Ptr, f2End);
                                    }

                                    uint64_t totalIn = inTok + cacheTok;
                                    uint64_t totalSum = respTok;
                                    if (totalSum == 0 && outTok > thinkTok) {
                                        totalSum = outTok - thinkTok;
                                    }
                                    if (totalIn > 0 || totalSum > 0) {
                                        st.beforeTokens = totalIn;
                                        st.summaryTokens = totalSum;
                                    }
                                } else {
                                    if (!pbSkipField(gPtr, gEnd, gWt)) break;
                                }
                            }
                        } else {
                            if (!pbSkipField(mPtr, mEnd, mWt)) break;
                        }
                    }

                    double startSec = (lastStep14Idx == idx - 1 && lastStep14CreatedSec > 0.0)
                        ? lastStep14CreatedSec : createdSec;
                    if (completedSec >= startSec && startSec > 0.0) {
                        st.durationSec = static_cast<int64_t>(std::llround(completedSec - startSec));
                    }
                }

                if (st.summaryTokens > 0 && st.beforeTokens > 0 && overheadTokens > 0) {
                    st.afterTokens = overheadTokens + st.summaryTokens;
                }
                outByStepIndex[idx] = st;
            }
        }
        sqlite3_finalize(stmt);
    }
    return true;
}

static std::string extractCompactionSummary(const std::string& rawSegment, size_t maxBytes = 12288) {
    std::string text = rawSegment;
    if (text.rfind("---", 0) == 0) {
        auto endFm = text.find("\n---", 3);
        if (endFm != std::string::npos) {
            size_t resume = endFm + 4;
            if (resume < text.size() && text[resume] == '\r') resume++;
            if (resume < text.size() && text[resume] == '\n') resume++;
            text = text.substr(resume);
        }
    }

    if (text.rfind("## Verbatim turns", 0) == 0 || text.rfind("# Verbatim turns", 0) == 0) {
        text.clear();
    } else {
        auto vPos = text.find("\n## Verbatim turns");
        if (vPos == std::string::npos) {
            vPos = text.find("\n# Verbatim turns");
        }
        if (vPos != std::string::npos) {
            text.erase(vPos);
        }
    }

    auto first = text.find_first_not_of(" \r\n\t");
    if (first == std::string::npos) return "";
    auto last = text.find_last_not_of(" \r\n\t");
    text = text.substr(first, last - first + 1);

    if (text.size() > maxBytes) {
        text = text.substr(0, maxBytes);
        text += "\n\n*(Compaction summary truncated)*";
    }

    return text;
}

static std::string extractAntigravityCheckpointSummary(const std::string& raw, size_t maxBytes = 12288) {
    std::string body = raw;
    auto sumStart = body.find("<summary>");
    if (sumStart != std::string::npos) {
        sumStart += 9;
        auto sumEnd = body.find("</summary>", sumStart);
        if (sumEnd != std::string::npos) {
            body = body.substr(sumStart, sumEnd - sumStart);
        } else {
            body = body.substr(sumStart);
        }
    } else {
        const std::string kHeader = "# Resuming from a compaction";
        if (body.rfind(kHeader, 0) == 0) {
            auto nl = body.find('\n');
            if (nl != std::string::npos) {
                body = body.substr(nl + 1);
            }
        }
    }
    std::string cleaned = extractCompactionSummary(body, maxBytes);
    if (cleaned.empty()) {
        cleaned = "Session context compacted.";
    }
    return "📦 **Context Compacted**\n\n" + cleaned;
}

static NormalizedProviderTranscript parseProviderTranscriptJsonl(
    const fs::path& filePath,
    const std::string& providerSource,
    int sinceIndex = -1,
    const std::string& projectSlug = "",
    bool summaryOnly = false) {
    NormalizedProviderTranscript out;
    inferWorkspaceFromProjectSlug(projectSlug, out.workspacePath, out.workspaceShort);
    std::string hookFirstPrompt;
    std::string hookStatus;
    bool hookIsSubagent = false;
    bool hasHookState = false;
    if ((providerSource == "cursor" || providerSource == "claude") &&
        filePath.parent_path().filename() != "subagents") {
        std::string cid = filePath.stem().string();
        nlohmann::json hookJson;
        bool readOk = (providerSource == "cursor")
            ? readCursorHookStateJson(cid, hookJson)
            : readClaudeHookStateJson(cid, hookJson);
        if (readOk) {
            hasHookState = true;
            hookIsSubagent = hookJson.value("is_subagent", false) ||
                             !hookJson.value("parent_session_id", "").empty();
            hookStatus = hookJson.value("status", "");
            std::string hookModel = hookJson.value("model", "");
            if (!hookModel.empty() && hookModel != "Cursor" && hookModel != "Claude Code") {
                out.model = hookModel;
            }
            std::string hookWorkspace = hookJson.value("workspace", hookJson.value("workspace_path", ""));
            if (!hookWorkspace.empty()) {
                out.workspacePath = hookWorkspace;
                out.workspaceShort = fs::path(hookWorkspace).filename().string();
            }
            hookFirstPrompt = cleanProviderUserPrompt(
                hookJson.value("first_prompt", hookJson.value("prompt_preview", "")));
            if (hookStatus == "RUNNING") {
                out.status = "RUNNING";
            } else if (hookStatus == "IDLE") {
                out.status = "IDLE";
            }
            uint64_t upd = static_cast<uint64_t>(
                hookJson.value("updated_ts", hookJson.value("updated_at", hookJson.value("updated_epoch", hookJson.value("start_ts", static_cast<int64_t>(0))))));
            if (upd > out.lastActivitySec) {
                out.lastActivitySec = upd;
            }
        }
    }

    std::set<std::string> activeClaudeUuids;
    if (providerSource == "claude" && !summaryOnly) {
        std::ifstream preIn(filePath);
        if (preIn.is_open()) {
            std::map<std::string, std::string> parentMap;
            std::string lastUuid;
            bool hasNonEmptyParentUuid = false;
            std::string preLine;
            while (std::getline(preIn, preLine)) {
                if (preLine.empty()) continue;
                if (preLine.find("\"uuid\"") == std::string::npos) continue;
                auto pj = nlohmann::json::parse(preLine, nullptr, false);
                if (pj.is_discarded() || !pj.is_object()) continue;
                std::string u = pj.value("uuid", "");
                if (u.empty()) continue;
                std::string pu;
                if (pj.contains("parentUuid") && pj["parentUuid"].is_string()) {
                    pu = pj["parentUuid"].get<std::string>();
                }
                parentMap[u] = pu;
                if (!pu.empty()) {
                    hasNonEmptyParentUuid = true;
                }
                std::string ptype = pj.value("type", "");
                if (ptype != "user" && ptype != "assistant") {
                    ptype = pj.value("role", "");
                }
                if ((ptype == "user" || ptype == "assistant") &&
                    !pj.value("isSidechain", false) &&
                    !pj.value("isMeta", false) &&
                    !pj.value("queueTranscriptOnly", false)) {
                    bool isTaskNotif = (pj.value("turnOrigin", "") == "task_notification");
                    if (!isTaskNotif && pj.contains("origin") && pj["origin"].is_object()) {
                        if (pj["origin"].value("kind", "") == "task-notification") {
                            isTaskNotif = true;
                        }
                    }
                    if (!isTaskNotif && preLine.find("<task-notification>") != std::string::npos) {
                        isTaskNotif = true;
                    }
                    if (!isTaskNotif) {
                        lastUuid = u;
                    }
                }
            }
            if (!lastUuid.empty() && hasNonEmptyParentUuid) {
                std::string cur = lastUuid;
                std::set<std::string> visited;
                while (!cur.empty() && visited.insert(cur).second) {
                    activeClaudeUuids.insert(cur);
                    auto it = parentMap.find(cur);
                    if (it != parentMap.end()) {
                        cur = it->second;
                    } else {
                        break;
                    }
                }
            }
        }
    }

    std::error_code fileExistEc;
    std::string cursorHookDir = (providerSource == "cursor") ? getCursorHookStateDir() : "";
    bool isHookShadowFile = !cursorHookDir.empty() && (filePath.parent_path() == fs::path(cursorHookDir));
    bool parentFileExists = !isHookShadowFile &&
                            fs::exists(filePath, fileExistEc) &&
                            fs::is_regular_file(filePath, fileExistEc) &&
                            fs::file_size(filePath, fileExistEc) > 0;
    std::ifstream in(filePath);
    if (!in.is_open() || !parentFileExists) {
        if (providerSource == "cursor" && filePath.parent_path().filename() != "subagents") {
            std::string cid = filePath.stem().string();
            if (!cursorHookDir.empty()) {
                fs::path hookJsonl = fs::path(cursorHookDir) / (cid + ".jsonl");
                std::error_code hEc;
                if (fs::exists(hookJsonl, hEc) && fs::is_regular_file(hookJsonl, hEc) &&
                    fs::file_size(hookJsonl, hEc) > 0) {
                    in.close();
                    in.clear();
                    in.open(hookJsonl);
                }
            }
        } else if (!in.is_open()) {
            return out;
        }
    }

    auto extractFastTimestamp = [](const std::string& rawLine) -> uint64_t {
        auto pos = rawLine.find("\"timestamp\"");
        if (pos == std::string::npos) {
            pos = rawLine.find("\"created_at\"");
        }
        if (pos == std::string::npos) return 0;
        auto q1 = rawLine.find('"', pos + 11);
        if (q1 == std::string::npos) return 0;
        auto q2 = rawLine.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 <= q1 + 1) return 0;
        return parseIso8601ToEpochSec(rawLine.substr(q1 + 1, q2 - q1 - 1));
    };

    int stepIdx = 0;

    if (providerSource == "cursor") {
        fs::path compDir = filePath.parent_path() / "compaction";
        std::error_code compEc;
        if (fs::exists(compDir, compEc) && fs::is_directory(compDir, compEc)) {
            std::string latestSegmentFile;
            for (const auto& entry : fs::directory_iterator(compDir, compEc)) {
                if (compEc || !entry.is_regular_file()) continue;
                std::string fname = entry.path().filename().string();
                if (fname.rfind("segment_", 0) == 0 && entry.path().extension() == ".md") {
                    if (fname > latestSegmentFile) {
                        latestSegmentFile = fname;
                    }
                }
            }
            if (!latestSegmentFile.empty()) {
                std::ifstream segFile(compDir / latestSegmentFile);
                if (segFile.is_open()) {
                    std::stringstream buffer;
                    buffer << segFile.rdbuf();
                    std::string rawSeg = buffer.str();
                    std::string summaryBody = extractCompactionSummary(rawSeg);
                    if (!summaryBody.empty() || rawSeg.find("- Turns:") != std::string::npos) {
                        int curIdx = stepIdx++;
                        out.totalSteps = curIdx + 1;
                        out.hasValidTurns = true;
                        if (!summaryOnly) {
                            int64_t cursorTurns = 0;
                            auto findTurns = [&](const std::string& prefix) -> int64_t {
                                auto p = rawSeg.find(prefix);
                                if (p == std::string::npos) return 0;
                                p += prefix.size();
                                while (p < rawSeg.size() && (rawSeg[p] == ' ' || rawSeg[p] == '\t')) p++;
                                int64_t val = 0;
                                while (p < rawSeg.size() && std::isdigit(static_cast<unsigned char>(rawSeg[p]))) {
                                    val = val * 10 + (rawSeg[p] - '0');
                                    p++;
                                }
                                return val;
                            };
                            cursorTurns = findTurns("- Turns:");
                            if (cursorTurns == 0) {
                                cursorTurns = findTurns("- Turn count:");
                            }

                            uint64_t verboseBytes = 0;
                            const std::string vPrefix = "- Verbose-render size estimate:";
                            auto vp = rawSeg.find(vPrefix);
                            if (vp != std::string::npos) {
                                vp += vPrefix.size();
                                while (vp < rawSeg.size() && (rawSeg[vp] == ' ' || rawSeg[vp] == '\t')) vp++;
                                uint64_t bVal = 0;
                                while (vp < rawSeg.size() && (std::isdigit(static_cast<unsigned char>(rawSeg[vp])) || rawSeg[vp] == ',')) {
                                    if (rawSeg[vp] != ',') {
                                        bVal = bVal * 10 + (rawSeg[vp] - '0');
                                    }
                                    vp++;
                                }
                                verboseBytes = bVal;
                            }

                            uint64_t summaryTokens = summaryBody.empty() ? 0 : std::max<uint64_t>(1, summaryBody.size() / 4);
                            uint64_t beforeTokens = (verboseBytes > 0) ? (verboseBytes / 4) : 0;
                            uint64_t afterTokens = (beforeTokens > 0 && summaryTokens > 0) ? summaryTokens : 0;
                            std::string compactDisplay = joinCompactionStatsParts(cursorTurns, beforeTokens, afterTokens, summaryTokens, -1);

                            nlohmann::json stepJson = {
                                {"index", curIdx},
                                {"stepIndex", curIdx},
                                {"step_index", curIdx},
                                {"type", "PLANNER_RESPONSE"},
                                {"status", "DONE"},
                                {"content", compactDisplay},
                                {"thinking", ""},
                                {"plannerResponse", ""},
                                {"text", compactDisplay},
                                {"tool_calls", nlohmann::json::array()},
                                {"createdAt", ""},
                                {"step", {{"case", "text"}, {"value", compactDisplay}}}
                            };
                            out.lastStepObj = stepJson;
                            if (sinceIndex < 0 || curIdx > sinceIndex) {
                                out.steps.push_back(std::move(stepJson));
                            }
                        }
                    }
                }
            }
        }
    }

    std::string line;
    std::string lastConsecutiveUserPrompt;
    bool parentTurnEnded = false;
    std::set<std::string> pendingSubagentToolUseIds;
    int lastClaudeCompactStepIdx = 0;
    uint64_t pendingClaudePreTokens = 0;
    uint64_t pendingClaudePostTokens = 0;
    int64_t pendingClaudeDurationSec = -1;
    bool hasPendingClaudeBoundary = false;
    (void)hasPendingClaudeBoundary;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        // Fast skip non-turn metadata records before invoking full JSON parser
        if (line.find("\"file-history-snapshot\"") != std::string::npos ||
            line.find("\"queue-operation\"") != std::string::npos ||
            line.find("\"type\":\"progress\"") != std::string::npos ||
            line.find("\"type\": \"progress\"") != std::string::npos) {
            continue;
        }
        if (summaryOnly && activeClaudeUuids.empty() && (line.size() > 32768 || line.find("\"tool_result\"") != std::string::npos)) {
            uint64_t fastSec = extractFastTimestamp(line);
            if (fastSec > out.lastActivitySec) {
                out.lastActivitySec = fastSec;
            }
            if (line.find("\"role\":\"assistant\"") != std::string::npos ||
                line.find("\"type\":\"assistant\"") != std::string::npos) {
                int curIdx = stepIdx++;
                out.totalSteps = curIdx + 1;
                out.hasValidTurns = true;
                lastConsecutiveUserPrompt.clear();
            }
            continue;
        }

        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        if (j.value("isSidechain", false)) continue;

        if (j.value("type", "") == "turn_ended" ||
            line.find("\"type\":\"turn_ended\"") != std::string::npos ||
            line.find("\"type\": \"turn_ended\"") != std::string::npos) {
            parentTurnEnded = true;
        }

        auto tStart = line.find("<tool-use-id>");
        if (tStart != std::string::npos) {
            auto tEnd = line.find("</tool-use-id>", tStart + 13);
            if (tEnd != std::string::npos && tEnd > tStart + 13) {
                std::string completedId = line.substr(tStart + 13, tEnd - (tStart + 13));
                while (!completedId.empty() && (completedId.front() == ' ' || completedId.front() == '\t' || completedId.front() == '\r' || completedId.front() == '\n')) completedId.erase(0, 1);
                while (!completedId.empty() && (completedId.back() == ' ' || completedId.back() == '\t' || completedId.back() == '\r' || completedId.back() == '\n')) completedId.pop_back();
                if (!completedId.empty()) {
                    pendingSubagentToolUseIds.erase(completedId);
                }
            }
        } else if (line.find("<task-notification>") != std::string::npos && pendingSubagentToolUseIds.size() == 1) {
            pendingSubagentToolUseIds.clear();
        }

        std::string cwd = j.value("cwd", "");
        if (!cwd.empty()) {
            out.workspacePath = cwd;
            out.workspaceShort = fs::path(cwd).filename().string();
        }

        std::string role = j.value("type", "");
        if (role == "system" && j.value("subtype", "") == "compact_boundary") {
            hasPendingClaudeBoundary = true;
            if (j.contains("compactMetadata") && j["compactMetadata"].is_object()) {
                const auto& cm = j["compactMetadata"];
                pendingClaudePreTokens = cm.value("preTokens", static_cast<uint64_t>(0));
                pendingClaudePostTokens = cm.value("postTokens", static_cast<uint64_t>(0));
                if (cm.contains("durationMs") && cm["durationMs"].is_number()) {
                    pendingClaudeDurationSec = static_cast<int64_t>(std::llround(cm["durationMs"].get<double>() / 1000.0));
                }
            }
            continue;
        }
        if (role != "user" && role != "assistant") {
            role = j.value("role", "");
        }
        if (role != "user" && role != "assistant") {
            continue;
        }

        std::string lineUuid = j.value("uuid", "");
        if (providerSource == "claude" && !activeClaudeUuids.empty() && !lineUuid.empty() &&
            !j.value("isCompactSummary", false)) {
            if (activeClaudeUuids.count(lineUuid) == 0) {
                continue;
            }
        }

        parentTurnEnded = false;

        std::string tsStr;
        uint64_t parsedSec = 0;
        if (j.contains("timestamp")) {
            if (j["timestamp"].is_string()) {
                tsStr = j["timestamp"].get<std::string>();
            } else if (j["timestamp"].is_number_integer()) {
                int64_t rawTs = j["timestamp"].get<int64_t>();
                if (rawTs > 9999999999LL) rawTs /= 1000;
                if (rawTs > 0) parsedSec = static_cast<uint64_t>(rawTs);
            }
        } else if (j.contains("created_at") && j["created_at"].is_string()) {
            tsStr = j["created_at"].get<std::string>();
        }
        if (tsStr.empty()) {
            auto tsTagStart = line.find("<timestamp>");
            auto tsTagEnd = line.find("</timestamp>");
            if (tsTagStart != std::string::npos && tsTagEnd != std::string::npos && tsTagEnd > tsTagStart + 11) {
                tsStr = line.substr(tsTagStart + 11, tsTagEnd - (tsTagStart + 11));
            }
        }
        if (parsedSec == 0 && !tsStr.empty()) {
            parsedSec = parseIso8601ToEpochSec(tsStr);
        }
        if (parsedSec > out.lastActivitySec) {
            out.lastActivitySec = parsedSec;
        }

        const nlohmann::json& msgObj = (j.contains("message") && j["message"].is_object()) ? j["message"] : j;
        if (msgObj.contains("model") && msgObj["model"].is_string()) {
            std::string m = msgObj["model"].get<std::string>();
            if (!m.empty() && (providerSource != "cursor" || out.model.empty())) out.model = m;
        }

        if (msgObj.contains("content") && msgObj["content"].is_array()) {
            for (const auto& blk : msgObj["content"]) {
                if (blk.is_object() && blk.value("type", "") == "tool_result") {
                    std::string trId = blk.value("tool_use_id", "");
                    if (!trId.empty()) {
                        pendingSubagentToolUseIds.erase(trId);
                    }
                }
            }
        }

        if (role == "user") {
            if (stepIdx == 0 && line.find("<CONTEXT_SUMMARY>") != std::string::npos &&
                (line.find("/compact") != std::string::npos ||
                 line.find("Please compact and summarize our conversation context") != std::string::npos)) {
                out.hasValidTurns = false;
                return out;
            }

            if (!j.value("isCompactSummary", false)) {
                bool isSyntheticUser = j.value("isMeta", false) ||
                                       j.value("queueTranscriptOnly", false) ||
                                       (j.value("promptSource", "") == "system") ||
                                       (j.value("turnOrigin", "") == "task_notification") ||
                                       (j.value("turnOrigin", "") == "peer");
                if (!isSyntheticUser && j.contains("origin") && j["origin"].is_object()) {
                    std::string origKind = j["origin"].value("kind", "");
                    if (origKind == "task-notification" || origKind == "peer") {
                        isSyntheticUser = true;
                    }
                }
                if (isSyntheticUser) {
                    continue;
                }
            }

            if (!summaryOnly) {
                for (auto& prevSt : out.steps) {
                    if (prevSt.is_object() && prevSt.contains("askQuestion") &&
                        prevSt["askQuestion"].is_object() &&
                        prevSt["askQuestion"].value("answered", true) == false) {
                        prevSt["askQuestion"]["answered"] = true;
                    }
                }
            }

            std::string rawUserText;
            if (msgObj.contains("content")) {
                if (msgObj["content"].is_string()) {
                    rawUserText = msgObj["content"].get<std::string>();
                } else if (msgObj["content"].is_array()) {
                    for (const auto& blk : msgObj["content"]) {
                        if (blk.is_object() && blk.value("type", "") == "text") {
                            std::string t = cleanProviderUserPrompt(blk.value("text", ""));
                            if (!t.empty()) {
                                if (!rawUserText.empty()) rawUserText += "\n";
                                rawUserText += t;
                            }
                        }
                    }
                }
            }

            if (j.value("isCompactSummary", false)) {
                int curIdx = stepIdx++;
                out.totalSteps = curIdx + 1;
                out.hasValidTurns = true;
                if (out.firstPrompt.empty()) {
                    out.firstPrompt = "Compacted Claude Code Session";
                }
                if (!summaryOnly) {
                    int64_t stepsSum = (curIdx > lastClaudeCompactStepIdx) ? (curIdx - lastClaudeCompactStepIdx) : 0;
                    uint64_t sumTok = (pendingClaudePostTokens > 0) ? pendingClaudePostTokens : (rawUserText.empty() ? 0 : std::max<uint64_t>(1, rawUserText.size() / 4));
                    uint64_t befTok = pendingClaudePreTokens;
                    uint64_t aftTok = (befTok > 0 && sumTok > 0) ? sumTok : 0;
                    std::string compactDisplay = joinCompactionStatsParts(stepsSum, befTok, aftTok, sumTok, pendingClaudeDurationSec);
                    nlohmann::json stepJson = {
                        {"index", curIdx},
                        {"stepIndex", curIdx},
                        {"step_index", curIdx},
                        {"type", "PLANNER_RESPONSE"},
                        {"status", "DONE"},
                        {"content", compactDisplay},
                        {"thinking", ""},
                        {"plannerResponse", ""},
                        {"text", compactDisplay},
                        {"tool_calls", nlohmann::json::array()},
                        {"createdAt", tsStr},
                        {"step", {{"case", "text"}, {"value", compactDisplay}}}
                    };
                    out.lastStepObj = stepJson;
                    if (sinceIndex < 0 || curIdx > sinceIndex) {
                        out.steps.push_back(std::move(stepJson));
                    }
                }
                hasPendingClaudeBoundary = false;
                pendingClaudePreTokens = 0;
                pendingClaudePostTokens = 0;
                pendingClaudeDurationSec = -1;
                lastClaudeCompactStepIdx = curIdx + 1;
                continue;
            }

            std::string cleanText = cleanProviderUserPrompt(rawUserText);
            if (cleanText.empty()) {
                continue;
            }
            if (cleanText == lastConsecutiveUserPrompt) {
                continue;
            }
            lastConsecutiveUserPrompt = cleanText;

            int curIdx = stepIdx++;
            out.totalSteps = curIdx + 1;
            out.hasValidTurns = true;

            std::string truncated = (cleanText.size() > 240) ? (cleanText.substr(0, 237) + "...") : cleanText;
            if (out.firstPrompt.empty()) out.firstPrompt = truncated;
            out.lastPrompt = truncated;
            out.lastPromptStepIndex = curIdx;
            out.userPrompts.push_back({
                {"stepIndex", curIdx},
                {"filteredStepIndex", curIdx},
                {"text", truncated},
                {"timestamp", tsStr}
            });

            if (!summaryOnly) {
                nlohmann::json stepJson = {
                    {"index", curIdx},
                    {"stepIndex", curIdx},
                    {"step_index", curIdx},
                    {"type", "USER_INPUT"},
                    {"status", "DONE"},
                    {"content", cleanText},
                    {"text", cleanText},
                    {"userInput", cleanText},
                    {"createdAt", tsStr},
                    {"step", {{"case", "userInput"}, {"value", cleanText}}}
                };
                out.lastStepObj = stepJson;
                if (sinceIndex < 0 || curIdx > sinceIndex) {
                    out.steps.push_back(std::move(stepJson));
                }
            }
        } else if (role == "assistant") {
            if (!summaryOnly) {
                for (auto& prevSt : out.steps) {
                    if (prevSt.is_object() && prevSt.contains("askQuestion") &&
                        prevSt["askQuestion"].is_object() &&
                        prevSt["askQuestion"].value("answered", true) == false) {
                        prevSt["askQuestion"]["answered"] = true;
                    }
                }
            }

            std::string thinkingText;
            std::string replyText;
            std::string leadingText;
            nlohmann::json parsedToolCalls = nlohmann::json::array();
            nlohmann::json stepAskQuestions = nlohmann::json::array();

            if (msgObj.contains("content")) {
                if (msgObj["content"].is_string()) {
                    replyText = msgObj["content"].get<std::string>();
                } else if (msgObj["content"].is_array()) {
                    for (const auto& blk : msgObj["content"]) {
                        if (!blk.is_object()) continue;
                        std::string btype = blk.value("type", "");
                        if (btype == "thinking") {
                            std::string th = blk.value("thinking", blk.value("text", ""));
                            if (!th.empty()) {
                                if (!thinkingText.empty()) thinkingText += "\n";
                                thinkingText += th;
                            }
                        } else if (btype == "text") {
                            std::string tx = blk.value("text", "");
                            if (!tx.empty()) {
                                if (parsedToolCalls.empty()) {
                                    if (!leadingText.empty()) leadingText += "\n";
                                    leadingText += tx;
                                } else {
                                    if (!replyText.empty()) replyText += "\n";
                                    replyText += tx;
                                }
                            }
                        } else if (btype == "tool_use") {
                            std::string toolName = blk.value("name", "tool");
                            std::string toolId = blk.value("id", "");
                            if ((toolName == "Agent" || toolName == "Task") && !toolId.empty()) {
                                pendingSubagentToolUseIds.insert(toolId);
                            }
                            nlohmann::json inputObj = (blk.contains("input") && blk["input"].is_object())
                                                          ? blk["input"]
                                                          : nlohmann::json::object();
                            if (toolName == "GetDynamicTools") {
                                continue;
                            }
                            if (toolName == "UpdateCurrentStep") {
                                std::string curStep = inputObj.value("current_step", "");
                                std::string compSub = inputObj.value("completed_subtitle", "");
                                std::string finSum = inputObj.value("final_summary", "");
                                std::string stepNote;
                                if (!curStep.empty()) stepNote = curStep;
                                if (!compSub.empty()) {
                                    if (!stepNote.empty()) stepNote += " — ";
                                    stepNote += compSub;
                                }
                                if (!stepNote.empty()) {
                                    if (!thinkingText.empty()) thinkingText += "\n";
                                    thinkingText += stepNote;
                                }
                                if (!finSum.empty() && leadingText.empty() && replyText.empty()) {
                                    replyText = finSum;
                                }
                                continue;
                            }
                            if (toolName == "CallDynamicTool") {
                                std::string innerName = inputObj.value("toolName", "");
                                if (!innerName.empty()) {
                                    toolName = innerName;
                                }
                                if (inputObj.contains("arguments") && inputObj["arguments"].is_object()) {
                                    inputObj = inputObj["arguments"];
                                }
                                if ((toolName == "Agent" || toolName == "Task") && !toolId.empty()) {
                                    pendingSubagentToolUseIds.insert(toolId);
                                }
                            }
                            if (toolName == "AskUserQuestion" || toolName == "ask_question") {
                                nlohmann::json normQ = inputObj.contains("questions")
                                    ? normalizeQuestionsJson(inputObj["questions"])
                                    : nlohmann::json::array();
                                if (normQ.empty() && inputObj.contains("question") && inputObj["question"].is_string()) {
                                    normQ = normalizeQuestionsJson(nlohmann::json::array({inputObj}));
                                }
                                if (!normQ.empty()) {
                                    inputObj["questions"] = normQ;
                                    stepAskQuestions = normQ;
                                }
                            }
                            std::string cmd = inputObj.value("command", inputObj.value("CommandLine", ""));
                            std::string summary = inputObj.value("description",
                                                  inputObj.value("toolSummary",
                                                  inputObj.value("path",
                                                  inputObj.value("file_path",
                                                  inputObj.value("pattern",
                                                  inputObj.value("glob_pattern",
                                                  inputObj.value("target_directory",
                                                  inputObj.value("query", ""))))))));
                            if (summary.empty() && toolName == "TodoWrite" &&
                                inputObj.contains("todos") && inputObj["todos"].is_array()) {
                                summary = std::to_string(inputObj["todos"].size()) + " todo items";
                            }
                            if (summary.empty() && toolName == "AwaitShell" && inputObj.contains("shell_id")) {
                                summary = "shell " + inputObj.value("shell_id", "");
                            }
                            parsedToolCalls.push_back({
                                {"name", toolName},
                                {"toolAction", ""},
                                {"toolSummary", summary},
                                {"command", cmd},
                                {"arguments", inputObj},
                                {"args", inputObj}
                            });
                        }
                    }
                }
            }

            bool isSubagentTranscript = (filePath.parent_path().filename() == "subagents");
            bool splitLeadingTextAsBubble = !parsedToolCalls.empty() &&
                                            !leadingText.empty() &&
                                            stepAskQuestions.empty() &&
                                            (isSubagentTranscript || leadingText.size() >= 60 || parsedToolCalls.size() > 2);

            if (splitLeadingTextAsBubble) {
                lastConsecutiveUserPrompt.clear();
                int leadIdx = stepIdx++;
                out.totalSteps = leadIdx + 1;
                out.hasValidTurns = true;
                if (!summaryOnly) {
                    nlohmann::json leadStepJson = {
                        {"index", leadIdx},
                        {"stepIndex", leadIdx},
                        {"step_index", leadIdx},
                        {"type", "PLANNER_RESPONSE"},
                        {"status", "DONE"},
                        {"content", leadingText},
                        {"thinking", thinkingText},
                        {"plannerResponse", thinkingText},
                        {"text", leadingText},
                        {"tool_calls", nlohmann::json::array()},
                        {"createdAt", tsStr},
                        {"step", {
                            {"case", "text"},
                            {"value", leadingText}
                        }}
                    };
                    out.lastStepObj = leadStepJson;
                    if (sinceIndex < 0 || leadIdx > sinceIndex) {
                        out.steps.push_back(std::move(leadStepJson));
                    }
                }
                leadingText.clear();
                thinkingText.clear();
            } else if (!parsedToolCalls.empty()) {
                if (!leadingText.empty() && stepAskQuestions.empty()) {
                    if (!thinkingText.empty()) thinkingText += "\n";
                    thinkingText += leadingText;
                } else if (!leadingText.empty()) {
                    if (!replyText.empty()) {
                        replyText = leadingText + "\n" + replyText;
                    } else {
                        replyText = std::move(leadingText);
                    }
                }
            } else {
                if (!leadingText.empty()) {
                    if (!replyText.empty()) {
                        replyText = leadingText + "\n" + replyText;
                    } else {
                        replyText = std::move(leadingText);
                    }
                }
            }

            if (thinkingText.empty() && replyText.empty() && parsedToolCalls.empty()) {
                continue;
            }
            lastConsecutiveUserPrompt.clear();
            int curIdx = stepIdx++;
            out.totalSteps = curIdx + 1;
            out.hasValidTurns = true;

            if (!summaryOnly) {
                std::string displayValue = !replyText.empty() ? replyText : thinkingText;
                nlohmann::json stepJson = {
                    {"index", curIdx},
                    {"stepIndex", curIdx},
                    {"step_index", curIdx},
                    {"type", "PLANNER_RESPONSE"},
                    {"status", "DONE"},
                    {"content", replyText},
                    {"thinking", thinkingText},
                    {"plannerResponse", thinkingText},
                    {"text", replyText},
                    {"tool_calls", parsedToolCalls},
                    {"createdAt", tsStr},
                    {"step", {
                        {"case", !replyText.empty() ? "text" : "plannerResponse"},
                        {"value", displayValue}
                    }}
                };
                if (!stepAskQuestions.empty()) {
                    stepJson["askQuestion"] = {
                        {"questions", stepAskQuestions},
                        {"answered", false},
                        {"answers", nlohmann::json::array()}
                    };
                }
                out.lastStepObj = stepJson;
                if (sinceIndex < 0 || curIdx > sinceIndex) {
                    out.steps.push_back(std::move(stepJson));
                }
            }
        }
    }

    if (providerSource == "cursor" && !parentFileExists && filePath.parent_path().filename() != "subagents") {
        std::string cid = filePath.stem().string();
        nlohmann::json sessionRec = AgentTelemetryDb::getInstance().querySessionRecord(cid);
        bool hasDbRec = !sessionRec.empty() && !sessionRec.value("session_id", "").empty();
        if (hasDbRec) {
            if (out.workspacePath.empty()) {
                std::string dbWs = sessionRec.value("workspace_path", sessionRec.value("workspace", ""));
                if (!dbWs.empty()) {
                    out.workspacePath = dbWs;
                    out.workspaceShort = fs::path(dbWs).filename().string();
                }
            }
            if (out.model.empty()) {
                std::string dbModel = sessionRec.value("model_name", sessionRec.value("model", ""));
                if (!dbModel.empty() && dbModel != "Cursor") {
                    out.model = dbModel;
                }
            }
            uint64_t dbUpd = static_cast<uint64_t>(
                sessionRec.value("end_timestamp", sessionRec.value("start_timestamp", static_cast<int64_t>(0))));
            if (dbUpd > out.lastActivitySec) {
                out.lastActivitySec = dbUpd;
            }
        }

        fs::path candidateSubDir = filePath.parent_path() / "subagents";
        std::error_code sdEc;
        bool hasSubagentJsonl = false;
        if (fs::exists(candidateSubDir, sdEc) && fs::is_directory(candidateSubDir, sdEc)) {
            for (const auto& se : fs::directory_iterator(candidateSubDir, sdEc)) {
                if (!sdEc && se.is_regular_file() && se.path().extension() == ".jsonl") {
                    hasSubagentJsonl = true;
                    break;
                }
            }
        }

        if (hookIsSubagent && !hasSubagentJsonl) {
            return out;
        }

        if (hasHookState || hasDbRec || hasSubagentJsonl) {
            if (hookStatus == "RUNNING" || hasSubagentJsonl) {
                out.status = "RUNNING";
            }
            if (out.userPrompts.empty() && !hookFirstPrompt.empty()) {
                int curIdx = stepIdx++;
                out.totalSteps = curIdx + 1;
                out.hasValidTurns = true;
                out.firstPrompt = hookFirstPrompt;
                out.lastPrompt = hookFirstPrompt;
                out.lastPromptStepIndex = curIdx;
                out.userPrompts.push_back({
                    {"stepIndex", curIdx},
                    {"filteredStepIndex", curIdx},
                    {"text", hookFirstPrompt},
                    {"timestamp", ""}
                });
                if (!summaryOnly) {
                    nlohmann::json uStep = {
                        {"index", curIdx},
                        {"stepIndex", curIdx},
                        {"step_index", curIdx},
                        {"type", "USER_INPUT"},
                        {"status", "DONE"},
                        {"content", hookFirstPrompt},
                        {"text", hookFirstPrompt},
                        {"userInput", hookFirstPrompt},
                        {"createdAt", ""},
                        {"step", {{"case", "userInput"}, {"value", hookFirstPrompt}}}
                    };
                    out.lastStepObj = uStep;
                    if (sinceIndex < 0 || curIdx > sinceIndex) {
                        out.steps.push_back(std::move(uStep));
                    }
                }
            }

            bool hasAssistantToolSteps = false;
            for (const auto& st : out.steps) {
                if (st.value("type", "") == "PLANNER_RESPONSE" &&
                    st.contains("tool_calls") && st["tool_calls"].is_array() && !st["tool_calls"].empty()) {
                    hasAssistantToolSteps = true;
                    break;
                }
            }
            if (!hasAssistantToolSteps && (out.hasValidTurns || hasSubagentJsonl)) {
                nlohmann::json evArr = AgentTelemetryDb::getInstance().querySessionEvents(cid);
                nlohmann::json synthTools = nlohmann::json::array();
                if (evArr.is_array()) {
                    for (const auto& ev : evArr) {
                        if (!ev.is_object() || ev.value("event_type", "") != "TOOL_CALL") continue;
                        std::string tName = ev.value("tool_name", "Tool");
                        nlohmann::json metaObj = nlohmann::json::parse(ev.value("metadata_json", "{}"), nullptr, false);
                        if (metaObj.is_discarded() || !metaObj.is_object()) {
                            metaObj = nlohmann::json::object();
                        }
                        std::string cmd = metaObj.value("command", "");
                        std::string summary = metaObj.value("description", metaObj.value("file_path", cmd));
                        synthTools.push_back({
                            {"name", tName},
                            {"command", cmd},
                            {"toolSummary", summary},
                            {"args", metaObj},
                            {"arguments", metaObj}
                        });
                    }
                }
                if (!synthTools.empty()) {
                    int curIdx = stepIdx++;
                    out.totalSteps = curIdx + 1;
                    out.hasValidTurns = true;
                    if (!summaryOnly) {
                        nlohmann::json tStep = {
                            {"index", curIdx},
                            {"stepIndex", curIdx},
                            {"step_index", curIdx},
                            {"type", "PLANNER_RESPONSE"},
                            {"status", "RUNNING"},
                            {"content", ""},
                            {"thinking", "Executing tool calls..."},
                            {"plannerResponse", "Executing tool calls..."},
                            {"text", ""},
                            {"tool_calls", std::move(synthTools)},
                            {"createdAt", ""},
                            {"step", {{"case", "plannerResponse"}, {"value", "Executing tool calls..."}}}
                        };
                        out.lastStepObj = tStep;
                        if (sinceIndex < 0 || curIdx > sinceIndex) {
                            out.steps.push_back(std::move(tStep));
                        }
                    }
                }
            }
        }
    }

    if (providerSource == "cursor" && !parentTurnEnded) {
        if (parentFileExists ? (!out.steps.empty() &&
                                out.steps.back().contains("tool_calls") &&
                                out.steps.back()["tool_calls"].is_array() &&
                                !out.steps.back()["tool_calls"].empty())
                             : (hookStatus == "RUNNING" || out.hasValidTurns)) {
            out.status = "RUNNING";
        }
    }

    bool cursorCanMerge = (providerSource == "cursor" &&
                           filePath.parent_path().filename() != "subagents" &&
                           !parentTurnEnded &&
                           (!parentFileExists ||
                            (!out.steps.empty() &&
                             out.steps.back().contains("tool_calls") &&
                             out.steps.back()["tool_calls"].is_array() &&
                             !out.steps.back()["tool_calls"].empty())));

    bool claudeCanMerge = (providerSource == "claude" &&
                           filePath.parent_path().filename() != "subagents" &&
                           !pendingSubagentToolUseIds.empty());

    fs::path subDir;
    if (cursorCanMerge) {
        subDir = filePath.parent_path() / "subagents";
        std::error_code sdEc;
        if (!fs::exists(subDir, sdEc) || !fs::is_directory(subDir, sdEc)) {
            std::string cid = filePath.stem().string();
            std::string cursorRoot = getCursorProjectsDir();
            if (!cursorRoot.empty() && fs::exists(cursorRoot, sdEc) && fs::is_directory(cursorRoot, sdEc)) {
                for (const auto& projEntry : fs::directory_iterator(cursorRoot, sdEc)) {
                    if (sdEc || !projEntry.is_directory()) continue;
                    fs::path altSub = projEntry.path() / "agent-transcripts" / cid / "subagents";
                    std::error_code aEc;
                    if (fs::exists(altSub, aEc) && fs::is_directory(altSub, aEc)) {
                        subDir = altSub;
                        break;
                    }
                }
            }
        }
    } else if (claudeCanMerge) {
        subDir = filePath.parent_path() / filePath.stem() / "subagents";
    }

    if (cursorCanMerge || claudeCanMerge) {
        std::error_code subEc;
        if (fs::exists(subDir, subEc) && fs::is_directory(subDir, subEc)) {
            auto fileMtimeSec = [](const fs::path& p) -> uint64_t {
                try {
                    auto ftime = fs::last_write_time(p);
                    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
                    );
                    auto secs = std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
                    return (secs > 0) ? static_cast<uint64_t>(secs) : 0;
                } catch (...) {
                    return 0;
                }
            };
            std::error_code pEc;
            auto parentFtime = fs::last_write_time(filePath, pEc);
            uint64_t parentMtimeSec = parentFileExists ? fileMtimeSec(filePath) : 0;
            struct ActiveSubagentFile {
                fs::path path;
                std::string subId;
                uint64_t startSec = 0;
                uint64_t mtimeSec = 0;
            };
            std::vector<ActiveSubagentFile> activeSubs;
            for (const auto& subEntry : fs::directory_iterator(subDir, subEc)) {
                if (subEc || !subEntry.is_regular_file() || subEntry.path().extension() != ".jsonl") continue;
                std::error_code sEc;
                auto subFtime = fs::last_write_time(subEntry.path(), sEc);
                if (sEc || (parentFileExists && !pEc && subFtime < parentFtime)) continue;
                uint64_t subMtimeSec = fileMtimeSec(subEntry.path());
                if (subMtimeSec == 0 || (parentFileExists && subMtimeSec < parentMtimeSec)) continue;
                std::string subId = subEntry.path().stem().string();
                uint64_t startSec = 0;
                std::ifstream sf(subEntry.path());
                std::string firstLine;
                if (sf.is_open() && std::getline(sf, firstLine)) {
                    auto tsTagStart = firstLine.find("<timestamp>");
                    auto tsTagEnd = firstLine.find("</timestamp>");
                    if (tsTagStart != std::string::npos && tsTagEnd != std::string::npos && tsTagEnd > tsTagStart + 11) {
                        startSec = parseIso8601ToEpochSec(
                            firstLine.substr(tsTagStart + 11, tsTagEnd - (tsTagStart + 11)));
                    }
                    if (startSec == 0) {
                        startSec = extractFastTimestamp(firstLine);
                    }
                }
                if (startSec == 0) startSec = subMtimeSec;
                activeSubs.push_back({subEntry.path(), subId, startSec, subMtimeSec});
            }
            std::sort(activeSubs.begin(), activeSubs.end(), [](const ActiveSubagentFile& a, const ActiveSubagentFile& b) {
                if (a.startSec != b.startSec) return a.startSec < b.startSec;
                if (a.mtimeSec != b.mtimeSec) return a.mtimeSec < b.mtimeSec;
                return a.subId < b.subId;
            });
            for (const auto& subInfo : activeSubs) {
                NormalizedProviderTranscript subParsed = parseProviderTranscriptJsonl(
                    subInfo.path, providerSource, -1, projectSlug, false);
                if (parentFileExists ? (subParsed.steps.size() < 2) : subParsed.steps.empty()) {
                    continue;
                }
                if (subInfo.mtimeSec > out.lastActivitySec) {
                    out.lastActivitySec = subInfo.mtimeSec;
                }
                if (subParsed.lastActivitySec > out.lastActivitySec) {
                    out.lastActivitySec = subParsed.lastActivitySec;
                }
                std::string shortSubId = subInfo.subId;
                if (shortSubId.rfind("agent-", 0) == 0) {
                    shortSubId = shortSubId.substr(6);
                }
                shortSubId = shortSubId.substr(0, std::min<size_t>(8, shortSubId.size()));
                for (auto& subStep : subParsed.steps) {
                    if (!subStep.is_object()) continue;
                    int curIdx = stepIdx++;
                    out.totalSteps = curIdx + 1;
                    out.hasValidTurns = true;
                    std::string stType = subStep.value("type", "");
                    if (stType == "USER_INPUT" && !parentFileExists && out.firstPrompt.empty()) {
                        std::string rawTask = cleanProviderUserPrompt(subStep.value("text", ""));
                        if (!rawTask.empty()) {
                            out.firstPrompt = rawTask;
                            out.lastPrompt = rawTask;
                            out.lastPromptStepIndex = curIdx;
                            if (out.userPrompts.empty()) {
                                out.userPrompts.push_back({
                                    {"stepIndex", curIdx},
                                    {"filteredStepIndex", curIdx},
                                    {"text", rawTask},
                                    {"timestamp", subStep.value("createdAt", "")}
                                });
                            }
                        }
                    }
                    if (summaryOnly) continue;

                    if (stType == "USER_INPUT") {
                        std::string rawTask = subStep.value("text", "");
                        std::string firstLine = rawTask;
                        auto nl = firstLine.find_first_of("\r\n");
                        if (nl != std::string::npos) {
                            firstLine = firstLine.substr(0, nl);
                        }
                        if (firstLine.size() > 160) {
                            firstLine = firstLine.substr(0, 157) + "...";
                        }
                        std::string banner = "🤖 **Subagent (" + shortSubId + ")**" +
                                             (firstLine.empty() ? "" : (" — " + firstLine));
                        nlohmann::json bannerStep = {
                            {"index", curIdx},
                            {"stepIndex", curIdx},
                            {"step_index", curIdx},
                            {"type", "PLANNER_RESPONSE"},
                            {"status", "DONE"},
                            {"content", banner},
                            {"thinking", ""},
                            {"plannerResponse", ""},
                            {"text", banner},
                            {"tool_calls", nlohmann::json::array()},
                            {"createdAt", subStep.value("createdAt", "")},
                            {"isSubagentStep", true},
                            {"isSubagent", true},
                            {"subagentId", subInfo.subId},
                            {"step", {{"case", "text"}, {"value", banner}}}
                        };
                        out.lastStepObj = bannerStep;
                        if (sinceIndex < 0 || curIdx > sinceIndex) {
                            out.steps.push_back(std::move(bannerStep));
                        }
                    } else {
                        subStep["index"] = curIdx;
                        subStep["stepIndex"] = curIdx;
                        subStep["step_index"] = curIdx;
                        subStep["isSubagentStep"] = true;
                        subStep["isSubagent"] = true;
                        subStep["subagentId"] = subInfo.subId;
                        out.lastStepObj = subStep;
                        if (sinceIndex < 0 || curIdx > sinceIndex) {
                            out.steps.push_back(std::move(subStep));
                        }
                    }
                }
            }
        }
    }

    if (out.firstPrompt.empty() && out.hasValidTurns) {
        out.firstPrompt = (providerSource == "cursor") ? "Compacted Cursor Session" : "Compacted Session";
    }

    if (out.model.empty()) {
        out.model = (providerSource == "claude") ? "claude-sonnet-4-6" : "cursor-auto";
    }
    if (out.hasValidTurns && out.status == "IDLE" && (parentTurnEnded || providerSource == "claude")) {
        out.status = "DONE";
    }
    if (filePath.parent_path().filename() != "subagents") {
        std::string cid = filePath.stem().string();
        if (providerSource == "cursor" || providerSource == "claude") {
            LiveProviderSessionInfo liveInfo = discoverLiveProviderSession(
                providerSource, cid, out.workspacePath, filePath.string());
            SessionInteractivityInfo inter = evaluateSessionInteractivity(
                providerSource, cid, fs::path(), liveInfo, filePath);
            out.interactive = inter.interactive;
            out.nonInteractiveReason = inter.nonInteractiveReason;
        }
    }
    return out;
}

static fs::path findProviderTranscriptById(const std::string& id,
                                           std::string& outSource,
                                           std::string& outProjectSlug) {
    if (id.empty() || id.find("..") != std::string::npos || id.find('/') != std::string::npos) {
        return {};
    }
    std::error_code ec;
    std::string claudeRoot = getClaudeProjectsDir();
    if (!claudeRoot.empty() && fs::exists(claudeRoot, ec) && fs::is_directory(claudeRoot, ec)) {
        for (const auto& projEntry : fs::directory_iterator(claudeRoot, ec)) {
            if (ec || !projEntry.is_directory()) continue;
            fs::path candidate = projEntry.path() / (id + ".jsonl");
            if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
                outSource = "claude";
                outProjectSlug = projEntry.path().filename().string();
                return candidate;
            }
        }
    }
    std::string cursorRoot = getCursorProjectsDir();
    fs::path fallbackCursorDirCandidate;
    std::string fallbackCursorSlug;
    if (!cursorRoot.empty() && fs::exists(cursorRoot, ec) && fs::is_directory(cursorRoot, ec)) {
        for (const auto& projEntry : fs::directory_iterator(cursorRoot, ec)) {
            if (ec || !projEntry.is_directory()) continue;
            fs::path convDir = projEntry.path() / "agent-transcripts" / id;
            fs::path candidate = convDir / (id + ".jsonl");
            if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
                outSource = "cursor";
                outProjectSlug = projEntry.path().filename().string();
                return candidate;
            }
            if (fallbackCursorDirCandidate.empty() && fs::exists(convDir, ec) && fs::is_directory(convDir, ec)) {
                fallbackCursorDirCandidate = candidate;
                fallbackCursorSlug = projEntry.path().filename().string();
            }
        }
    }
    if (!fallbackCursorDirCandidate.empty()) {
        outSource = "cursor";
        outProjectSlug = fallbackCursorSlug;
        return fallbackCursorDirCandidate;
    }

    nlohmann::json hookJson;
    if (readCursorHookStateJson(id, hookJson)) {
        outSource = "cursor";
        std::string ws = hookJson.value("workspace", hookJson.value("workspace_path", ""));
        if (!ws.empty()) {
            outProjectSlug = fs::path(ws).filename().string();
        }
        std::string hookDir = getCursorHookStateDir();
        return fs::path(hookDir.empty() ? "/tmp" : hookDir) / (id + ".jsonl");
    }

    nlohmann::json sessionRec = AgentTelemetryDb::getInstance().querySessionRecord(id);
    if (!sessionRec.empty() && sessionRec.value("agent_type", "") == "cursor") {
        outSource = "cursor";
        std::string ws = sessionRec.value("workspace_path", sessionRec.value("workspace", ""));
        if (!ws.empty()) {
            outProjectSlug = fs::path(ws).filename().string();
        }
        std::string hookDir = getCursorHookStateDir();
        return fs::path(hookDir.empty() ? "/tmp" : hookDir) / (id + ".jsonl");
    }

    return {};
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

static std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

static bool isClaudeIdeExtensionTranscript(const fs::path& jsonlPath) {
    if (jsonlPath.empty()) return false;
    std::error_code ec;
    if (!fs::exists(jsonlPath, ec) || !fs::is_regular_file(jsonlPath, ec)) {
        return false;
    }
    std::ifstream f(jsonlPath);
    if (!f.is_open()) return false;
    std::string line;
    int count = 0;
    bool sawIdeMarker = false;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        ++count;
        if (line.find("\"entrypoint\":\"cli\"") != std::string::npos ||
            line.find("\"entrypoint\": \"cli\"") != std::string::npos) {
            return false;
        }
        if (count <= 15) {
            if (line.find("<ide_opened_file>") != std::string::npos ||
                line.find("<ide_selection>") != std::string::npos ||
                line.find("\"entrypoint\":\"ide\"") != std::string::npos ||
                line.find("\"entrypoint\": \"ide\"") != std::string::npos ||
                line.find("\"entrypoint\":\"claude-vscode\"") != std::string::npos ||
                line.find("\"entrypoint\": \"claude-vscode\"") != std::string::npos ||
                line.find("\"isIde\":true") != std::string::npos ||
                line.find("\"isIde\": true") != std::string::npos) {
                sawIdeMarker = true;
            }
        }
    }
    return sawIdeMarker;
}

static bool hasCursorCliStoreForConversation(const fs::path& homeDir,
                                             const std::string& conversationId) {
    if (conversationId.empty() || conversationId == "new") return true;
    std::vector<fs::path> candidateDirs;
    const char* envChatsDir = std::getenv("AIMON_CURSOR_CHATS_DIR");
    if (envChatsDir && *envChatsDir) {
        candidateDirs.emplace_back(envChatsDir);
    }
    std::string cProjDir = getCursorProjectsDir();
    if (!cProjDir.empty()) {
        candidateDirs.push_back(fs::path(cProjDir).parent_path() / "chats");
    }
    if (!homeDir.empty()) {
        candidateDirs.push_back(homeDir / ".cursor" / "chats");
    }
    if (candidateDirs.empty()) {
        candidateDirs.emplace_back(PathUtils::expandHome("~/.cursor/chats"));
    }

    for (const auto& chatsDir : candidateDirs) {
        std::error_code ec;
        if (!fs::exists(chatsDir, ec) || !fs::is_directory(chatsDir, ec)) continue;
        for (const auto& wsEnt : fs::directory_iterator(chatsDir, ec)) {
            if (ec || !wsEnt.is_directory()) continue;
            fs::path histPath = wsEnt.path() / conversationId / "prompt_history.json";
            fs::path storePath = wsEnt.path() / conversationId / "store.db";
            if (fs::exists(histPath, ec) || fs::exists(storePath, ec)) {
                return true;
            }
        }
    }
    return false;
}

static SessionInteractivityInfo evaluateSessionInteractivity(
    const std::string& provider,
    const std::string& conversationId,
    const fs::path& homeDir,
    const LiveProviderSessionInfo& liveInfo,
    const fs::path& transcriptPath) {
    SessionInteractivityInfo result;
    if (conversationId.empty() || conversationId == "new") {
        return result;
    }
    if (provider == "cursor" || provider == "claude") {
        if (liveInfo.ownerPid > 0 && !liveInfo.muxType.empty()) {
            return result;
        }
        result.interactive = false;
        if (liveInfo.ownerPid > 0 && liveInfo.muxType.empty()) {
            result.nonInteractiveReason =
                "Read-only terminal session (PID " + std::to_string(liveInfo.ownerPid) +
                "): not running inside a terminal multiplexer (GNU screen or tmux). "
                "Re-launch inside screen or tmux to enable mobile replies.";
            return result;
        }
        if (provider == "cursor") {
            if (!hasCursorCliStoreForConversation(homeDir, conversationId)) {
                result.nonInteractiveReason =
                    "Bi-directional communication is not possible with this Cursor IDE GUI agent "
                    "(Cursor IDE GUI session is read-only; no CLI chat store or screen/tmux multiplexer is attached). "
                    "Launch cursor-agent in a screen/tmux session to interact from mobile.";
                return result;
            }
        } else if (provider == "claude") {
            fs::path resolvedPath = transcriptPath;
            if (resolvedPath.empty()) {
                std::string dummySrc;
                std::string dummySlug;
                resolvedPath = findProviderTranscriptById(conversationId, dummySrc, dummySlug);
            }
            if (!resolvedPath.empty() && isClaudeIdeExtensionTranscript(resolvedPath)) {
                result.nonInteractiveReason =
                    "Bi-directional communication is not possible with this Claude IDE extension agent "
                    "(Claude Code IDE extension session is read-only; no interactive screen/tmux session is attached). "
                    "Launch claude in a screen/tmux session to interact from mobile.";
                return result;
            }
        }
        result.nonInteractiveReason =
            "Inactive CLI session: no live process in screen or tmux. "
            "Resume this session inside screen or tmux on the host to reply from mobile.";
        return result;
    }
    return result;
}

static std::string readProcCmdline(pid_t pid) {
    if (pid <= 0) return "";
    try {
        std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return "";
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    } catch (...) {
        return "";
    }
}

static std::string readProcComm(pid_t pid) {
    if (pid <= 0) return "";
    try {
        std::string commPath = "/proc/" + std::to_string(pid) + "/comm";
        std::ifstream commF(commPath);
        if (!commF.is_open()) return "";
        std::string comm;
        std::getline(commF, comm);
        while (!comm.empty() && (comm.back() == '\r' || comm.back() == '\n' || comm.back() == ' ')) {
            comm.pop_back();
        }
        return comm;
    } catch (...) {
        return "";
    }
}

static bool isOneShotCmdline(const std::string& rawCmdline) {
    if (rawCmdline.empty()) return false;
    size_t start = 0;
    while (start < rawCmdline.size()) {
        size_t end = rawCmdline.find('\0', start);
        if (end == std::string::npos) end = rawCmdline.size();
        std::string token = rawCmdline.substr(start, end - start);
        if (token == "-p" || token == "--print") return true;
        start = end + 1;
    }
    return false;
}

static bool isProcessInteractiveProvider(pid_t pid, const std::string& targetProvider) {
    if (pid <= 0) return false;
    std::string cmdline = readProcCmdline(pid);
    if (cmdline.empty() || isOneShotCmdline(cmdline)) return false;

    std::vector<std::string> args;
    size_t start = 0;
    while (start < cmdline.size()) {
        size_t end = cmdline.find('\0', start);
        if (end == std::string::npos) end = cmdline.size();
        if (end > start) {
            args.push_back(cmdline.substr(start, end - start));
        }
        start = end + 1;
    }
    if (args.empty()) return false;

    for (const auto& tok : args) {
        if (tok == "-p" || tok == "--print" || tok.rfind("--type=", 0) == 0) {
            return false;
        }
    }

    auto baseNameOf = [](const std::string& p) -> std::string {
        auto sl = p.rfind('/');
        return (sl != std::string::npos) ? p.substr(sl + 1) : p;
    };

    std::string arg0Lower = toLowerAscii(args[0]);
    std::string exeBase = baseNameOf(arg0Lower);
    std::string scriptBase;
    if (exeBase == "node" || exeBase == "nodejs" || exeBase == "bash" ||
        exeBase == "sh" || exeBase == "env") {
        for (size_t i = 1; i < args.size(); ++i) {
            if (!args[i].empty() && args[i][0] != '-' && args[i].find('=') == std::string::npos) {
                scriptBase = baseNameOf(toLowerAscii(args[i]));
                break;
            }
        }
    }

    std::string commLower = toLowerAscii(readProcComm(pid));

    auto endsWith = [](const std::string& s, const std::string& suffix) -> bool {
        return s.size() >= suffix.size() &&
               s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    if (targetProvider == "cursor") {
        if (commLower == "cursor-agent" ||
            exeBase == "cursor-agent" ||
            scriptBase == "cursor-agent" ||
            endsWith(arg0Lower, "/.local/bin/cursor-agent") ||
            endsWith(arg0Lower, "/cursor-agent") ||
            (arg0Lower.find("/.cursor-server/") != std::string::npos && exeBase == "cursor-agent")) {
            return true;
        }
    } else if (targetProvider == "claude") {
        if (arg0Lower.find("/extensions/anthropic.claude-code-") != std::string::npos) {
            return false;
        }
        for (const auto& tok : args) {
            std::string tokLower = toLowerAscii(tok);
            if (tokLower.find("/extensions/anthropic.claude-code-") != std::string::npos ||
                tokLower == "--input-format" ||
                tokLower == "--output-format" ||
                tokLower == "stream-json" ||
                tokLower.rfind("--input-format=", 0) == 0 ||
                tokLower.rfind("--output-format=", 0) == 0) {
                return false;
            }
        }
        if (commLower == "claude" ||
            exeBase == "claude" ||
            scriptBase == "claude" ||
            endsWith(arg0Lower, "/claude")) {
            return true;
        }
    }
    return false;
}

static pid_t getParentPid(pid_t pid) {
    if (pid <= 1) return 0;
    std::string line;
    try {
        std::string statPath = "/proc/" + std::to_string(pid) + "/stat";
        std::ifstream f(statPath);
        if (!f.is_open()) return 0;
        std::getline(f, line);
    } catch (...) {
        return 0;
    }
    size_t lastParen = line.rfind(')');
    if (lastParen == std::string::npos || lastParen + 2 >= line.size()) return 0;
    std::istringstream ss(line.substr(lastParen + 2));
    char state;
    pid_t ppid = 0;
    if (ss >> state >> ppid) {
        return ppid;
    }
    return 0;
}

static std::map<std::string, std::string> readProcEnvironMap(pid_t pid) {
    std::map<std::string, std::string> envMap;
    if (pid <= 0) return envMap;
    try {
        std::ifstream f("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
        if (!f.is_open()) return envMap;
        std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t end = raw.find('\0', pos);
            if (end == std::string::npos) end = raw.size();
            if (end > pos) {
                std::string entry = raw.substr(pos, end - pos);
                auto eq = entry.find('=');
                if (eq != std::string::npos && eq > 0) {
                    envMap[entry.substr(0, eq)] = entry.substr(eq + 1);
                }
            }
            pos = end + 1;
        }
    } catch (...) {}
    return envMap;
}

static std::string findTerminalMultiplexerForPid(
    pid_t pid,
    std::string& outMuxType,
    std::string& outMuxSession,
    std::string& outMuxTarget,
    const std::string& cid = "") {
    outMuxType.clear();
    outMuxSession.clear();
    outMuxTarget.clear();

    const char* envMuxOverride = std::getenv("AIMON_MUX_SESSION_OVERRIDE");
    if (envMuxOverride && *envMuxOverride) {
        std::string ov(envMuxOverride);
        auto c1 = ov.find(':');
        if (c1 != std::string::npos) {
            outMuxType = ov.substr(0, c1);
            auto c2 = ov.find(':', c1 + 1);
            if (c2 != std::string::npos) {
                outMuxSession = ov.substr(c1 + 1, c2 - (c1 + 1));
                outMuxTarget = ov.substr(c2 + 1);
            } else {
                outMuxSession = ov.substr(c1 + 1);
                outMuxTarget = (outMuxType == "screen") ? "0" : outMuxSession;
            }
            return outMuxType;
        }
    }

    const char* envScreenOverride = std::getenv("AIMON_SCREEN_SESSION_OVERRIDE");
    if (envScreenOverride && *envScreenOverride) {
        outMuxType = "screen";
        outMuxSession = envScreenOverride;
        outMuxTarget = "0";
        return outMuxType;
    }

    pid_t cur = pid;
    pid_t screenPid = 0;
    std::string styEnv;
    std::string windowEnv;
    std::string tmuxEnv;
    std::string tmuxPaneEnv;
    std::string zellijSessionEnv;
    std::string weztermPaneEnv;
    std::string weztermSockEnv;
    std::string kittyListenEnv;
    std::string kittyWinEnv;

    std::set<pid_t> ancestorPids;
    for (int depth = 0; depth < 30 && cur > 1; ++depth) {
        ancestorPids.insert(cur);
        auto envMap = readProcEnvironMap(cur);
        if (styEnv.empty() && envMap.count("STY")) styEnv = envMap["STY"];
        if (windowEnv.empty() && envMap.count("WINDOW")) windowEnv = envMap["WINDOW"];
        if (tmuxEnv.empty() && envMap.count("TMUX")) tmuxEnv = envMap["TMUX"];
        if (tmuxPaneEnv.empty() && envMap.count("TMUX_PANE")) tmuxPaneEnv = envMap["TMUX_PANE"];
        if (zellijSessionEnv.empty() && envMap.count("ZELLIJ_SESSION_NAME")) {
            zellijSessionEnv = envMap["ZELLIJ_SESSION_NAME"];
        }
        if (weztermPaneEnv.empty() && envMap.count("WEZTERM_PANE")) weztermPaneEnv = envMap["WEZTERM_PANE"];
        if (weztermSockEnv.empty() && envMap.count("WEZTERM_UNIX_SOCKET")) {
            weztermSockEnv = envMap["WEZTERM_UNIX_SOCKET"];
        }
        if (kittyListenEnv.empty() && envMap.count("KITTY_LISTEN_ON")) kittyListenEnv = envMap["KITTY_LISTEN_ON"];
        if (kittyWinEnv.empty() && envMap.count("KITTY_WINDOW_ID")) kittyWinEnv = envMap["KITTY_WINDOW_ID"];

        std::string comm = readProcComm(cur);
        std::string cmd = readProcCmdline(cur);
        std::string commUpper = comm;
        for (auto& c : commUpper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (screenPid <= 0 &&
            (commUpper == "SCREEN" || cmd.rfind("SCREEN", 0) == 0 || cmd.rfind("screen", 0) == 0)) {
            screenPid = cur;
        }
        cur = getParentPid(cur);
    }

    // 1. GNU screen via parent SCREEN daemon or STY environment variable
    if (screenPid > 0 || !styEnv.empty()) {
        std::vector<fs::path> screenDirs;
        const char* envSd = std::getenv("SCREENDIR");
        if (envSd && *envSd) {
            screenDirs.emplace_back(envSd);
        }
        const char* user = std::getenv("USER");
        std::string uName = (user && *user) ? user : "";
        if (uName.empty()) {
            struct passwd* pw = getpwuid(getuid());
            if (pw && pw->pw_name) uName = pw->pw_name;
        }
        if (!uName.empty()) {
            screenDirs.emplace_back("/run/screen/S-" + uName);
            screenDirs.emplace_back("/tmp/screens/S-" + uName);
        }

        std::string pidPrefix = (screenPid > 0) ? (std::to_string(screenPid) + ".") : "";
        for (const auto& sd : screenDirs) {
            std::error_code ec;
            if (!fs::exists(sd, ec) || !fs::is_directory(sd, ec)) continue;
            for (const auto& ent : fs::directory_iterator(sd, ec)) {
                if (ec) break;
                std::string fname = ent.path().filename().string();
                if ((!pidPrefix.empty() && fname.rfind(pidPrefix, 0) == 0) ||
                    (!styEnv.empty() && fname == styEnv)) {
                    outMuxType = "screen";
                    outMuxSession = fname;
                    outMuxTarget = !windowEnv.empty() ? windowEnv : "0";
                    return outMuxType;
                }
            }
        }
    }

    // 2. tmux via TMUX / TMUX_PANE or cursor-agent-persist binding
    const char* envTmuxBin = std::getenv("AIMON_TMUX_BIN");
    std::string tmuxBin = (envTmuxBin && *envTmuxBin) ? envTmuxBin : "/usr/bin/tmux";
    if (!tmuxEnv.empty()) {
        auto c1 = tmuxEnv.find(',');
        std::string sockPath = (c1 != std::string::npos) ? tmuxEnv.substr(0, c1) : tmuxEnv;
        std::error_code ec;
        if (!sockPath.empty() && fs::exists(sockPath, ec)) {
            outMuxType = "tmux";
            outMuxSession = sockPath;
            outMuxTarget = !tmuxPaneEnv.empty() ? tmuxPaneEnv : "0";
            return outMuxType;
        }
    }

    if (!cid.empty() && cid != "new") {
        fs::path persistBind = "/tmp/cursor-agent-persist-" + std::to_string(getuid()) +
                               "/bindings/" + cid + ".json";
        std::error_code bEc;
        if (fs::exists(persistBind, bEc) && fs::is_regular_file(persistBind, bEc)) {
            try {
                std::ifstream bf(persistBind);
                auto bj = nlohmann::json::parse(bf, nullptr, false);
                if (!bj.is_discarded() && bj.is_object()) {
                    std::string tmuxSess = bj.value("tmux_session", "");
                    if (!tmuxSess.empty()) {
                        outMuxType = "tmux";
                        outMuxSession = tmuxSess;
                        outMuxTarget = tmuxSess;
                        return outMuxType;
                    }
                }
            } catch (...) {}
        }
    }

    // 3. Zellij
    if (!zellijSessionEnv.empty()) {
        outMuxType = "zellij";
        outMuxSession = zellijSessionEnv;
        outMuxTarget = "0";
        return outMuxType;
    }

    // 4. WezTerm
    if (!weztermPaneEnv.empty() && !weztermSockEnv.empty()) {
        std::error_code ec;
        if (fs::exists(weztermSockEnv, ec)) {
            outMuxType = "wezterm";
            outMuxSession = weztermSockEnv;
            outMuxTarget = weztermPaneEnv;
            return outMuxType;
        }
    }

    // 5. Kitty
    if (!kittyListenEnv.empty()) {
        outMuxType = "kitty";
        outMuxSession = kittyListenEnv;
        outMuxTarget = !kittyWinEnv.empty() ? kittyWinEnv : "0";
        return outMuxType;
    }

    return "";
}

static bool processHoldsCidFd(pid_t pid, const std::string& cid) {
    if (cid.empty() || pid <= 0) return false;
    std::string fdDir = "/proc/" + std::to_string(pid) + "/fd";
    std::error_code ec;
    if (!fs::exists(fdDir, ec)) return false;
    for (const auto& ent : fs::directory_iterator(fdDir, ec)) {
        if (ec) break;
        std::error_code lEc;
        fs::path target = fs::read_symlink(ent.path(), lEc);
        if (!lEc && target.string().find(cid) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static bool processHoldsOtherCursorStoreFd(pid_t pid, const std::string& cid) {
    if (pid <= 0) return false;
    std::string fdDir = "/proc/" + std::to_string(pid) + "/fd";
    std::error_code ec;
    if (!fs::exists(fdDir, ec)) return false;
    for (const auto& ent : fs::directory_iterator(fdDir, ec)) {
        if (ec) break;
        std::error_code lEc;
        fs::path target = fs::read_symlink(ent.path(), lEc);
        if (lEc) continue;
        std::string tStr = target.string();
        if (tStr.find("/.cursor/chats/") != std::string::npos &&
            tStr.size() > 9 && tStr.compare(tStr.size() - 9, 9, "/store.db") == 0) {
            if (cid.empty() || tStr.find("/" + cid + "/store.db") == std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

static bool processCwdEquals(pid_t pid, const std::string& wsDir) {
    if (wsDir.empty() || pid <= 0) return false;
    std::string cwdPath = "/proc/" + std::to_string(pid) + "/cwd";
    std::error_code ec;
    fs::path target = fs::read_symlink(cwdPath, ec);
    if (!ec) {
        return target.string() == wsDir;
    }
    return false;
}

static LiveProviderSessionInfo discoverLiveProviderSession(
    const std::string& targetProvider,
    const std::string& cid,
    const std::string& wsDir,
    const std::string& existingTranscriptPath) {
    LiveProviderSessionInfo info;
    if (cid.empty() || cid == "new") {
        return info;
    }

    const char* envOwnerOverride = std::getenv("AIMON_LIVE_OWNER_PID_OVERRIDE");
    if (envOwnerOverride && *envOwnerOverride) {
        info.ownerPid = static_cast<pid_t>(std::atoi(envOwnerOverride));
    }
    const char* envMuxOverride = std::getenv("AIMON_MUX_SESSION_OVERRIDE");
    const char* envScreenOverride = std::getenv("AIMON_SCREEN_SESSION_OVERRIDE");
    if ((envMuxOverride && *envMuxOverride) || (envScreenOverride && *envScreenOverride)) {
        findTerminalMultiplexerForPid(info.ownerPid, info.muxType, info.muxSession, info.muxTarget, cid);
        if (info.muxType == "screen") {
            info.screenSession = info.muxSession;
        }
        if (!info.muxType.empty() && info.ownerPid <= 0) {
            info.ownerPid = getpid();
        }
    }

    std::string stateDirStr = (targetProvider == "claude")
        ? getClaudeHookStateDir()
        : getCursorHookStateDir();
    if (stateDirStr.empty()) {
        stateDirStr = (targetProvider == "claude")
            ? PathUtils::expandHome("~/.claude/hooks/state")
            : PathUtils::expandHome("~/.cursor/hooks/state");
    }
    fs::path stateDirPath(stateDirStr);

    bool recordedDeadOwnerPid = false;
    std::string hookMuxType;
    std::string hookMuxSession;
    std::string hookMuxTarget;
    std::set<pid_t> claimedByOtherSessions;

    std::error_code sEc;
    if (fs::exists(stateDirPath, sEc) && fs::is_directory(stateDirPath, sEc)) {
        for (const auto& sEnt : fs::directory_iterator(stateDirPath, sEc)) {
            if (sEc || !sEnt.is_regular_file() || sEnt.path().extension() != ".json") continue;
            std::string otherCid = sEnt.path().stem().string();
            if (otherCid.empty() || otherCid == cid) continue;
            try {
                std::ifstream of(sEnt.path());
                auto oj = nlohmann::json::parse(of, nullptr, false);
                if (!oj.is_discarded() && oj.is_object() &&
                    oj.contains("owner_pid") && oj["owner_pid"].is_number_integer()) {
                    pid_t oPid = oj["owner_pid"].get<pid_t>();
                    if (oPid > 0 && kill(oPid, 0) == 0) {
                        claimedByOtherSessions.insert(oPid);
                    }
                }
            } catch (...) {}
        }
    }

    if (info.ownerPid == 0) {
        fs::path statePath = stateDirPath / (cid + ".json");
        std::error_code ec;
        if (fs::exists(statePath, ec)) {
            std::ifstream sf(statePath);
            if (sf.is_open()) {
                try {
                    nlohmann::json sj;
                    sf >> sj;
                    if (sj.contains("owner_pid") && sj["owner_pid"].is_number_integer()) {
                        pid_t candPid = sj["owner_pid"].get<pid_t>();
                        if (candPid > 0) {
                            if (kill(candPid, 0) == 0 &&
                                (!fs::exists("/proc/" + std::to_string(candPid), ec) ||
                                 isProcessInteractiveProvider(candPid, targetProvider))) {
                                info.ownerPid = candPid;
                                if (sj.contains("mux_type") && sj["mux_type"].is_string()) {
                                    hookMuxType = sj.value("mux_type", "");
                                    hookMuxSession = sj.value("mux_session", sj.value("screen_session", ""));
                                    hookMuxTarget = sj.value("mux_target", "0");
                                }
                            } else {
                                recordedDeadOwnerPid = true;
                            }
                        }
                    }
                } catch (...) {}
            }
        }
    }

    bool hasCliStore = true;
    if (targetProvider == "cursor" && !cid.empty() && cid != "new") {
        hasCliStore = hasCursorCliStoreForConversation(fs::path(), cid);
        info.hasCliStore = hasCliStore;
        if (!hasCliStore) {
            info.isIdeRendererSession = true;
        }
    }
    bool isClaudeIdeTrans = (targetProvider == "claude" && !existingTranscriptPath.empty() &&
                             isClaudeIdeExtensionTranscript(existingTranscriptPath));

    const char* envCliBrain = std::getenv("AIMON_CLI_BRAIN_DIR");
    const char* envCursorProj = std::getenv("AIMON_CURSOR_PROJECTS_DIR");
    const char* envClaudeProj = std::getenv("AIMON_CLAUDE_PROJECTS_DIR");
    const char* envWsRoot = std::getenv("AIMON_WORKSPACE_ROOT");
    bool isHermeticTestEnv = (envCliBrain && *envCliBrain) ||
                             (envCursorProj && *envCursorProj) ||
                             (envClaudeProj && *envClaudeProj);
    bool allowCwdFallback = true;
    if (isHermeticTestEnv) {
        if (!envWsRoot || !*envWsRoot || wsDir.rfind(envWsRoot, 0) != 0) {
            allowCwdFallback = false;
        }
    }

    std::vector<pid_t> interactiveCandidates;
    if (info.ownerPid == 0) {
        std::error_code pEc;
        fs::path procDir = "/proc";
        uid_t myUid = getuid();
        for (const auto& ent : fs::directory_iterator(procDir, pEc)) {
            if (pEc) break;
            std::string name = ent.path().filename().string();
            if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) continue;
            pid_t pid = static_cast<pid_t>(std::atoi(name.c_str()));
            if (pid <= 0) continue;
            struct stat st;
            if (stat(ent.path().c_str(), &st) != 0 || st.st_uid != myUid) continue;

            if (isProcessInteractiveProvider(pid, targetProvider)) {
                interactiveCandidates.push_back(pid);
                if (info.ownerPid == 0) {
                    if (readProcCmdline(pid).find(cid) != std::string::npos ||
                        processHoldsCidFd(pid, cid)) {
                        info.ownerPid = pid;
                    }
                }
            }
        }

        if (info.ownerPid == 0 && !recordedDeadOwnerPid && allowCwdFallback &&
            !wsDir.empty() && hasCliStore && !isClaudeIdeTrans) {
            pid_t fallbackUnwrappedPid = 0;
            for (const auto& pid : interactiveCandidates) {
                if (claimedByOtherSessions.count(pid)) continue;
                if (targetProvider == "cursor" && processHoldsOtherCursorStoreFd(pid, cid)) continue;
                if (!processCwdEquals(pid, wsDir)) continue;
                std::string mType, mSess, mTgt;
                if (!findTerminalMultiplexerForPid(pid, mType, mSess, mTgt, cid).empty()) {
                    info.ownerPid = pid;
                    info.muxType = mType;
                    info.muxSession = mSess;
                    info.muxTarget = mTgt;
                    if (mType == "screen") {
                        info.screenSession = mSess;
                    }
                    break;
                }
                if (fallbackUnwrappedPid == 0) {
                    fallbackUnwrappedPid = pid;
                }
            }
            if (info.ownerPid == 0 && fallbackUnwrappedPid > 0) {
                info.ownerPid = fallbackUnwrappedPid;
            }
        }
    }

    if (info.ownerPid > 0 && info.muxType.empty()) {
        findTerminalMultiplexerForPid(info.ownerPid, info.muxType, info.muxSession, info.muxTarget, cid);
        if (info.muxType.empty() && !hookMuxType.empty()) {
            info.muxType = hookMuxType;
            info.muxSession = hookMuxSession;
            info.muxTarget = hookMuxTarget;
        }
        if (info.muxType == "screen") {
            info.screenSession = info.muxSession;
        }
    }

    return info;
}

static bool runCommandSync(const std::vector<std::string>& cmdArgs) {
    if (cmdArgs.empty()) return false;
    pid_t ch = fork();
    if (ch == 0) {
        int devNull = open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            dup2(devNull, STDIN_FILENO);
            dup2(devNull, STDOUT_FILENO);
            dup2(devNull, STDERR_FILENO);
            if (devNull > 2) close(devNull);
        }
        std::vector<char*> argv;
        for (const auto& a : cmdArgs) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execvp(cmdArgs[0].c_str(), argv.data());
        _exit(127);
    } else if (ch > 0) {
        int st = 0;
        waitpid(ch, &st, 0);
        return WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }
    return false;
}

static bool isUserTranscriptLine(const std::string& targetProvider, const std::string& line) {
    if (targetProvider == "cursor") {
        return line.find("\"role\":\"user\"") != std::string::npos ||
               line.find("\"role\": \"user\"") != std::string::npos;
    }
    return line.find("\"type\":\"user\"") != std::string::npos ||
           line.find("\"type\": \"user\"") != std::string::npos;
}

static bool pollLiveProviderTurn(
    const std::string& targetProvider,
    const std::string& cid,
    const std::string& existingTranscriptPath,
    const std::string& existingSlug,
    size_t priorLineCount,
    size_t priorUserLineCount,
    std::string& replyText) {
    (void)priorLineCount;
    int timeoutMs = 90000;
    const char* envTimeout = std::getenv("AIMON_LIVE_ROUTE_TIMEOUT_MS");
    if (envTimeout && *envTimeout) {
        timeoutMs = std::atoi(envTimeout);
        if (timeoutMs <= 0) timeoutMs = 90000;
    }
    int pollIntervalMs = 200;
    const char* envPoll = std::getenv("AIMON_LIVE_ROUTE_POLL_MS");
    if (envPoll && *envPoll) {
        pollIntervalMs = std::atoi(envPoll);
        if (pollIntervalMs <= 0) pollIntervalMs = 200;
    }

    int elapsedMs = 0;
    bool turnFinished = false;
    while (elapsedMs < timeoutMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
        elapsedMs += pollIntervalMs;

        std::error_code ec;
        if (!fs::exists(existingTranscriptPath, ec)) {
            continue;
        }

        std::vector<std::string> lines;
        {
            std::ifstream fIn(existingTranscriptPath);
            std::string l;
            while (std::getline(fIn, l)) {
                if (!l.empty()) {
                    lines.push_back(l);
                }
            }
        }

        size_t userLineCount = 0;
        size_t lastUserLineIdx = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (isUserTranscriptLine(targetProvider, lines[i])) {
                ++userLineCount;
                lastUserLineIdx = i;
            }
        }

        if (userLineCount <= priorUserLineCount) {
            continue;
        }

        if (targetProvider == "cursor") {
            bool sawTurnEnded = false;
            for (size_t i = lastUserLineIdx + 1; i < lines.size(); ++i) {
                const auto& cl = lines[i];
                if (cl.find("\"type\":\"turn_ended\"") != std::string::npos ||
                    cl.find("\"type\": \"turn_ended\"") != std::string::npos) {
                    sawTurnEnded = true;
                    break;
                }
            }
            if (sawTurnEnded) {
                nlohmann::json hookJ;
                int activeSubs = 0;
                if (!cid.empty() && readCursorHookStateJson(cid, hookJ)) {
                    activeSubs = hookJ.value("active_subagents", 0);
                }
                if (activeSubs <= 0) {
                    turnFinished = true;
                    break;
                }
            }
        } else {
            for (size_t i = lastUserLineIdx + 1; i < lines.size(); ++i) {
                const auto& cl = lines[i];
                if ((cl.find("\"type\":\"assistant\"") != std::string::npos ||
                     cl.find("\"type\": \"assistant\"") != std::string::npos) &&
                    (cl.find("\"stop_reason\":\"end_turn\"") != std::string::npos ||
                     cl.find("\"stop_reason\": \"end_turn\"") != std::string::npos ||
                     cl.find("\"stop_reason\":\"stop_sequence\"") != std::string::npos ||
                     cl.find("\"stop_reason\": \"stop_sequence\"") != std::string::npos)) {
                    turnFinished = true;
                    break;
                }
            }
            if (turnFinished) {
                break;
            }
        }
    }

    if (!turnFinished) {
        return false;
    }

    auto norm = parseProviderTranscriptJsonl(existingTranscriptPath, targetProvider, -1, existingSlug, false);
    std::string collectedReply;
    size_t lastUserIdx = norm.steps.size();
    for (size_t i = norm.steps.size(); i > 0; --i) {
        const auto& s = norm.steps[i - 1];
        std::string sType = s.value("type", "");
        if (sType == "USER_INPUT") {
            lastUserIdx = i - 1;
            break;
        }
    }

    for (size_t i = (lastUserIdx < norm.steps.size() ? lastUserIdx + 1 : 0); i < norm.steps.size(); ++i) {
        const auto& s = norm.steps[i];
        std::string textContent = s.value("content", s.value("text", ""));
        if (!textContent.empty()) {
            if (!collectedReply.empty()) collectedReply += "\n";
            collectedReply += textContent;
        } else if (s.contains("tool_calls") && s["tool_calls"].is_array() && !s["tool_calls"].empty()) {
            for (const auto& tc : s["tool_calls"]) {
                std::string tName = tc.value("name", "tool");
                std::string tSumm = tc.value("toolSummary", "");
                if (!collectedReply.empty()) collectedReply += "\n";
                collectedReply += "[Executed: " + tName + (tSumm.empty() ? "" : (" " + tSumm)) + "]";
            }
        }
    }
    if (collectedReply.empty()) {
        collectedReply = "OK";
    }
    replyText = collectedReply;
    return true;
}

static bool routePromptViaMultiplexer(
    const LiveProviderSessionInfo& sessionInfo,
    const std::string& targetProvider,
    const std::string& cid,
    const std::string& promptArg,
    const std::string& existingTranscriptPath,
    const std::string& existingSlug,
    std::string& replyText,
    bool& outDispatchFailed) {
    outDispatchFailed = false;
    if (sessionInfo.muxType.empty() || sessionInfo.muxSession.empty()) {
        outDispatchFailed = true;
        return false;
    }

    size_t priorLineCount = 0;
    size_t priorUserLineCount = 0;
    std::error_code ec;
    if (fs::exists(existingTranscriptPath, ec)) {
        std::ifstream countIn(existingTranscriptPath);
        std::string line;
        while (std::getline(countIn, line)) {
            if (!line.empty()) {
                ++priorLineCount;
                if (isUserTranscriptLine(targetProvider, line)) {
                    ++priorUserLineCount;
                }
            }
        }
    }

    std::string sanitizedPrompt = promptArg;
    for (auto& c : sanitizedPrompt) {
        if (c == '\r' || c == '\n') c = ' ';
    }

    if (sessionInfo.muxType == "screen") {
        const char* envScreenBin = std::getenv("AIMON_SCREEN_BIN");
        std::string screenBin = (envScreenBin && *envScreenBin) ? envScreenBin : "/usr/bin/screen";
        std::string winTarget = !sessionInfo.muxTarget.empty() ? sessionInfo.muxTarget : "0";

        std::string escapedPrompt;
        escapedPrompt.reserve(sanitizedPrompt.size() * 2);
        for (char c : sanitizedPrompt) {
            if (c == '^' || c == '\\') {
                escapedPrompt.push_back('\\');
            }
            escapedPrompt.push_back(c);
        }

        const size_t kMaxChunk = 256;
        for (size_t off = 0; off < escapedPrompt.size(); off += kMaxChunk) {
            std::string chunk = escapedPrompt.substr(off, kMaxChunk);
            if (!chunk.empty() && chunk.back() == '\\' && (off + kMaxChunk) < escapedPrompt.size()) {
                chunk.push_back(escapedPrompt[off + kMaxChunk]);
                ++off;
            }
            std::vector<std::string> stuffCmd = {
                screenBin, "-S", sessionInfo.muxSession, "-p", winTarget, "-X", "stuff", chunk
            };
            if (!runCommandSync(stuffCmd)) {
                outDispatchFailed = true;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        std::vector<std::string> enterCmd = {
            screenBin, "-S", sessionInfo.muxSession, "-p", winTarget, "-X", "stuff", "\r"
        };
        if (!runCommandSync(enterCmd)) {
            outDispatchFailed = true;
            return false;
        }
    } else if (sessionInfo.muxType == "tmux") {
        const char* envTmuxBin = std::getenv("AIMON_TMUX_BIN");
        std::string tmuxBin = (envTmuxBin && *envTmuxBin) ? envTmuxBin : "/usr/bin/tmux";
        std::string paneTarget = !sessionInfo.muxTarget.empty() ? sessionInfo.muxTarget : sessionInfo.muxSession;
        bool isSocketPath = (sessionInfo.muxSession.find('/') != std::string::npos);

        const size_t kMaxChunk = 512;
        for (size_t off = 0; off < sanitizedPrompt.size(); off += kMaxChunk) {
            std::string chunk = sanitizedPrompt.substr(off, kMaxChunk);
            std::vector<std::string> sendCmd = {tmuxBin};
            if (isSocketPath) {
                sendCmd.push_back("-S");
                sendCmd.push_back(sessionInfo.muxSession);
            }
            sendCmd.insert(sendCmd.end(), {"send-keys", "-t", paneTarget, "-l", "--", chunk});
            if (!runCommandSync(sendCmd)) {
                outDispatchFailed = true;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::vector<std::string> enterCmd = {tmuxBin};
        if (isSocketPath) {
            enterCmd.push_back("-S");
            enterCmd.push_back(sessionInfo.muxSession);
        }
        enterCmd.insert(enterCmd.end(), {"send-keys", "-t", paneTarget, "Enter"});
        if (!runCommandSync(enterCmd)) {
            outDispatchFailed = true;
            return false;
        }
    } else if (sessionInfo.muxType == "zellij") {
        std::vector<std::string> writeCmd = {
            "zellij", "--session", sessionInfo.muxSession, "action", "write-chars", sanitizedPrompt
        };
        if (!runCommandSync(writeCmd)) {
            outDispatchFailed = true;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::vector<std::string> enterCmd = {
            "zellij", "--session", sessionInfo.muxSession, "action", "write", "13"
        };
        if (!runCommandSync(enterCmd)) {
            outDispatchFailed = true;
            return false;
        }
    } else if (sessionInfo.muxType == "wezterm") {
        std::vector<std::string> sendCmd = {
            "wezterm", "cli", "--pane-id", sessionInfo.muxTarget, "send-text", "--no-paste", sanitizedPrompt + "\r"
        };
        if (!runCommandSync(sendCmd)) {
            outDispatchFailed = true;
            return false;
        }
    } else if (sessionInfo.muxType == "kitty") {
        std::string matchArg = "id:" + (!sessionInfo.muxTarget.empty() ? sessionInfo.muxTarget : "0");
        std::vector<std::string> sendCmd = {
            "kitty", "@", "--to", sessionInfo.muxSession, "send-text", "--match", matchArg, sanitizedPrompt + "\r"
        };
        if (!runCommandSync(sendCmd)) {
            outDispatchFailed = true;
            return false;
        }
    } else {
        outDispatchFailed = true;
        return false;
    }

    return pollLiveProviderTurn(
        targetProvider, cid, existingTranscriptPath, existingSlug,
        priorLineCount, priorUserLineCount, replyText);
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
            req.path == "/api/mobile/notifications" ||
            req.path == "/api/mobile/test-notification" ||
            req.path == "/api/agent" ||
            req.path.rfind("/api/agent/", 0) == 0 ||
            req.path == "/api/workspace" ||
            req.path.rfind("/api/workspace/", 0) == 0 ||
            req.path == "/api/config" ||
            req.path == "/api/telemetry/prune" ||
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

    auto handleTelemetryOrHookEvent = [](const httplib::Request& req, httplib::Response& res) {
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
                auto agyProcs = discoverRunningAgyProcesses();
                bool anyMissingToken = false;
                {
                    std::lock_guard<std::mutex> lk(s_cachedCliTokensMutex);
                    for (const auto& proc : agyProcs) {
                        if (proc.httpPort > 0 && s_cachedCliTokens.find(proc.httpPort) == s_cachedCliTokens.end()) {
                            anyMissingToken = true;
                            break;
                        }
                    }
                }

                static std::atomic<int64_t> s_lastHookProcScanEpoch{0};
                int64_t nowEpoch = static_cast<int64_t>(std::time(nullptr));
                int64_t prevEpoch = s_lastHookProcScanEpoch.load();
                bool isPreToolUse = (eventType == "PRE_TOOL_USE" || eventType == "PreToolUse");

                if (anyMissingToken || isPreToolUse || (nowEpoch - prevEpoch >= 3 &&
                    s_lastHookProcScanEpoch.compare_exchange_strong(prevEpoch, nowEpoch))) {
                    syncAndPruneCliEndpointsState(agyProcs, true);
                }

                bool stillLacksToken = false;
                std::vector<AgyProcessInfo> procsLackingToken;
                {
                    std::lock_guard<std::mutex> lk(s_cachedCliTokensMutex);
                    for (const auto& proc : agyProcs) {
                        if (proc.httpPort > 0 && s_cachedCliTokens.find(proc.httpPort) == s_cachedCliTokens.end()) {
                            stillLacksToken = true;
                            procsLackingToken.push_back(proc);
                        }
                    }
                }

                if ((isPreToolUse || stillLacksToken) && !procsLackingToken.empty()) {
                    static std::atomic<bool> s_fastChildWatchActive{false};
                    bool expected = false;
                    if (s_fastChildWatchActive.compare_exchange_strong(expected, true)) {
                        std::thread([procsLackingToken]() {
                            for (int iter = 0; iter < 60; ++iter) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                bool foundAny = false;
                                for (const auto& proc : procsLackingToken) {
                                    if (proc.pidStr.empty()) continue;
                                    std::error_code ec;
                                    std::string taskDir = "/proc/" + proc.pidStr + "/task";
                                    if (!fs::exists(taskDir, ec)) continue;
                                    std::vector<pid_t> chPids;
                                    for (const auto& tentry : fs::directory_iterator(taskDir, ec)) {
                                        if (ec || !tentry.is_directory()) continue;
                                        std::ifstream cf(tentry.path() / "children");
                                        if (!cf.is_open()) continue;
                                        std::string word;
                                        while (cf >> word) {
                                            if (!word.empty() && std::all_of(word.begin(), word.end(), ::isdigit)) {
                                                try { chPids.push_back(std::stoi(word)); } catch (...) {}
                                            }
                                        }
                                    }
                                    for (pid_t cpid : chPids) {
                                        std::ifstream envFile("/proc/" + std::to_string(cpid) + "/environ", std::ios::binary);
                                        if (!envFile.is_open()) continue;
                                        std::string envContent((std::istreambuf_iterator<char>(envFile)),
                                                               std::istreambuf_iterator<char>());
                                        if (envContent.find("ANTIGRAVITY_CSRF_TOKEN=") != std::string::npos &&
                                            envContent.find("ANTIGRAVITY_LS_ADDRESS=") != std::string::npos) {
                                            foundAny = true;
                                            break;
                                        }
                                    }
                                    if (foundAny) break;
                                }
                                if (foundAny) {
                                    syncAndPruneCliEndpointsState(discoverRunningAgyProcesses(), true);
                                    break;
                                }
                            }
                            s_fastChildWatchActive.store(false);
                        }).detach();
                    }
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
    };
    _server->Post("/api/telemetry/event", handleTelemetryOrHookEvent);
    _server->Post("/api/hooks/event", handleTelemetryOrHookEvent);

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

    _server->Get("/api/mobile/notifications", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        uint64_t sinceSeq = 0;
        if (req.has_param("since")) {
            try {
                sinceSeq = std::stoull(req.get_param_value("since"));
            } catch (...) {}
        }
        MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
        auto events = gw.getRecentAgentEventsSince(sinceSeq);
        nlohmann::json arr = nlohmann::json::array();
        uint64_t maxSeq = sinceSeq;
        for (const auto& ev : events) {
            arr.push_back(ev.toJson());
            if (ev.seq > maxSeq) maxSeq = ev.seq;
        }
        res.set_content(nlohmann::json({
            {"events", arr},
            {"latestSeq", maxSeq}
        }).dump(2), "application/json");
    });

    _server->Post("/api/mobile/test-notification", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        nlohmann::json body = nlohmann::json::object();
        if (!req.body.empty()) {
            auto parsed = nlohmann::json::parse(req.body, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()) {
                body = parsed;
            }
        }
        AgentSessionEvent ev;
        ev.event = body.value("event", "prompt_answered");
        ev.conversationId = body.value("conversationId", "new");
        ev.source = body.value("source", "cli");
        ev.title = body.value("title", "Antigravity Session Test");
        ev.workspace = body.value("workspace", "aimon");
        ev.snippet = body.value("snippet", "Test notification chime, vibration, and session deep-link.");
        ev.stepIndex = -1; // Never suppress test notifications via stepIndex deduplication
        ev.timestamp = static_cast<int64_t>(time(nullptr));

        MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
        uint64_t seq = gw.broadcastAgentSessionEvent(ev);
        ev.seq = seq;
        res.set_content(nlohmann::json({
            {"status", "ok"},
            {"seq", seq},
            {"event", ev.toJson()}
        }).dump(2), "application/json");
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
        return aimon::getIdeBrainDir();
    };

    auto getCliBrainDir = []() -> std::string {
        return aimon::getCliBrainDir();
    };

    auto resolveSessionWorkspacePath = [getCliBrainDir, getIdeBrainDir](const std::string& cid) -> std::string {
        if (cid.empty() || cid == "new" || cid.find("..") != std::string::npos || cid.find('/') != std::string::npos) {
            return "";
        }

        // Step 1 (Running agy process): Check discoverRunningAgyProcesses()
        const char* envProcsOverride = std::getenv("AIMON_AGY_PROCESSES_FILE");
        for (const auto& proc : discoverRunningAgyProcesses()) {
            if (proc.lockedConversationId == cid && !proc.cwd.empty()) {
                if ((fs::exists(proc.cwd) && fs::is_directory(proc.cwd)) ||
                    (envProcsOverride && *envProcsOverride)) {
                    return proc.cwd;
                }
            }
        }

        // Step 2 (Antigravity conversation_summaries.db)
        fs::path sumDbPath = fs::path(getCliBrainDir()).parent_path() / "conversation_summaries.db";
        if (!fs::exists(sumDbPath)) {
            sumDbPath = fs::path(getCliBrainDir()) / "conversation_summaries.db";
        }
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
                                if (!u.empty() && fs::exists(u) && fs::is_directory(u)) {
                                    sqlite3_finalize(stmt);
                                    sqlite3_close(db);
                                    return u;
                                }
                            }
                        } catch (...) {}
                    }
                    sqlite3_finalize(stmt);
                }
                sqlite3_close(db);
            }
        }

        // Step 3 (Antigravity transcript.jsonl <user_information>)
        auto checkTranscriptCwd = [](const fs::path& transPath) -> std::string {
            if (!fs::exists(transPath)) return "";
            std::ifstream tf(transPath);
            if (!tf.is_open()) return "";
            std::string line;
            int lineCount = 0;
            while (lineCount < 30 && std::getline(tf, line)) {
                lineCount++;
                if (line.empty()) continue;
                std::string contentToScan = line;
                auto j = nlohmann::json::parse(line, nullptr, false);
                if (!j.is_discarded() && j.contains("content") && j["content"].is_string()) {
                    contentToScan = j["content"].get<std::string>();
                }
                auto cwdPos = contentToScan.find("Command Working Directory: ");
                if (cwdPos != std::string::npos) {
                    size_t start = cwdPos + 27;
                    size_t end = contentToScan.find_first_of("\r\n\"", start);
                    std::string p = (end == std::string::npos) ? contentToScan.substr(start) : contentToScan.substr(start, end - start);
                    auto first = p.find_first_not_of(" \t\r\n");
                    if (first != std::string::npos) {
                        auto last = p.find_last_not_of(" \t\r\n");
                        p = p.substr(first, last - first + 1);
                    } else {
                        p.clear();
                    }
                    if (!p.empty() && fs::exists(p) && fs::is_directory(p)) {
                        return p;
                    }
                }
                auto wsPos = contentToScan.find("Active Workspaces:");
                if (wsPos != std::string::npos) {
                    size_t dashPos = contentToScan.find("- ", wsPos + 18);
                    if (dashPos != std::string::npos) {
                        size_t start = dashPos + 2;
                        size_t end = contentToScan.find_first_of("\r\n\"", start);
                        std::string p = (end == std::string::npos) ? contentToScan.substr(start) : contentToScan.substr(start, end - start);
                        auto first = p.find_first_not_of(" \t\r\n");
                        if (first != std::string::npos) {
                            auto last = p.find_last_not_of(" \t\r\n");
                            p = p.substr(first, last - first + 1);
                        } else {
                            p.clear();
                        }
                        if (!p.empty() && fs::exists(p) && fs::is_directory(p)) {
                            return p;
                        }
                    }
                }
            }
            return "";
        };

        fs::path cliTrans = fs::path(getCliBrainDir()) / cid / ".system_generated" / "logs" / "transcript.jsonl";
        std::string cliFound = checkTranscriptCwd(cliTrans);
        if (!cliFound.empty()) return cliFound;

        fs::path ideTrans = fs::path(getIdeBrainDir()) / cid / ".system_generated" / "logs" / "transcript.jsonl";
        std::string ideFound = checkTranscriptCwd(ideTrans);
        if (!ideFound.empty()) return ideFound;

        // Step 4 (Claude Code / Cursor transcript)
        std::string provSource;
        std::string provSlug;
        fs::path provPath = findProviderTranscriptById(cid, provSource, provSlug);
        if (!provPath.empty()) {
            auto parsed = parseProviderTranscriptJsonl(provPath, provSource, -1, provSlug, true);
            if (!parsed.workspacePath.empty() && fs::exists(parsed.workspacePath) && fs::is_directory(parsed.workspacePath)) {
                return parsed.workspacePath;
            }
        }

        // Step 5 (Telemetry DB / Session Hook state)
        try {
            auto sRec = AgentTelemetryDb::getInstance().querySessionRecord(cid);
            if (!sRec.empty()) {
                std::string wsp = sRec.value("workspace_path", sRec.value("workspace", ""));
                if (!wsp.empty() && fs::exists(wsp) && fs::is_directory(wsp)) {
                    return wsp;
                }
            }
        } catch (...) {}

        nlohmann::json hookStateJson;
        if (readCursorHookStateJson(cid, hookStateJson) || readClaudeHookStateJson(cid, hookStateJson)) {
            std::string wsp = hookStateJson.value("workspace", hookStateJson.value("workspace_path", ""));
            if (!wsp.empty() && fs::exists(wsp) && fs::is_directory(wsp)) {
                return wsp;
            }
        }

        fs::path hookSessionPath = PathUtils::expandHome("~/.config/aimon/sessions/" + cid + ".json");
        if (fs::exists(hookSessionPath)) {
            try {
                std::ifstream sf(hookSessionPath);
                if (sf.is_open()) {
                    auto sj = nlohmann::json::parse(sf, nullptr, false);
                    if (!sj.is_discarded()) {
                        std::string wsp = sj.value("workspace_path", sj.value("workspace", ""));
                        if (!wsp.empty() && fs::exists(wsp) && fs::is_directory(wsp)) {
                            return wsp;
                        }
                    }
                }
            } catch (...) {}
        }

        return "";
    };

    auto resolveRequestedWorkspaceRoot = [resolveSessionWorkspacePath](const std::string& cid, const std::string& explicitWorkspace) -> fs::path {
        if (!cid.empty() && cid != "new") {
            std::string ws = resolveSessionWorkspacePath(cid);
            if (!ws.empty()) {
                return fs::path(ws);
            }
            if (!explicitWorkspace.empty() && explicitWorkspace.find("..") == std::string::npos) {
                fs::path p = PathUtils::expandHome(explicitWorkspace);
                if (fs::exists(p) && fs::is_directory(p)) {
                    return p;
                }
            }
            return fs::path();
        }
        if (cid.empty()) {
            if (!explicitWorkspace.empty() && explicitWorkspace.find("..") == std::string::npos) {
                fs::path p = PathUtils::expandHome(explicitWorkspace);
                if (fs::exists(p) && fs::is_directory(p)) {
                    return p;
                }
            } else if (const char* envWs = std::getenv("AIMON_WORKSPACE_ROOT"); envWs && *envWs) {
                if (fs::exists(envWs) && fs::is_directory(envWs)) {
                    return fs::path(envWs);
                }
            }
        }
        return fs::path();
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

    auto handleAgentAvailable = [resolveRequestedWorkspaceRoot](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string cid = req.get_param_value("conversationId");
        if (cid.empty()) cid = req.get_param_value("id");
        fs::path wsPath = resolveRequestedWorkspaceRoot(cid, "");
        std::string wsRoot = wsPath.string();
        nlohmann::json resp;
        resp["agents"] = nlohmann::json::array({
            {
                {"id", "antigravity"},
                {"name", "Google Antigravity"},
                {"status", "online"},
                {"workspaceRoot", wsRoot}
            },
            {
                {"id", "claude"},
                {"name", "Claude Code"},
                {"status", "online"},
                {"workspaceRoot", wsRoot}
            },
            {
                {"id", "cursor"},
                {"name", "Cursor Agent"},
                {"status", "online"},
                {"workspaceRoot", wsRoot}
            }
        });
        resp["allowed_workspaces"] = wsRoot.empty() ? nlohmann::json::array() : nlohmann::json::array({wsRoot});
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
                                            const std::string& conversationId,
                                            bool skipTranscriptScan = false) -> std::string {
        if (conversationId.empty() || conversationId == "new") {
            return "";
        }

        // 1. Check transcript.jsonl for explicit <USER_SETTINGS_CHANGE> Model Selection lines
        if (!skipTranscriptScan) {
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

    auto handleAgentConversations = [this, getIdeBrainDir, getCliBrainDir, getCliPresenceDir, stripUserRequestTags,
                                     toLowerAscii, mapModelToHumanLabel, extractConversationModelById, discoverLsEndpoints](
                                        const httplib::Request& req, httplib::Response& res) {
        std::string reqId = extractOrGenerateRequestId(req);
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("X-Request-Id", reqId);
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
            std::string agentName;
        };

        std::map<std::string, DbConvSummary> summariesMap;
        std::set<std::string> subagentIds;
        std::map<std::string, uint64_t> cursorParentMaxSubagentTs;
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
                const char* sqlWithAgent =
                    "SELECT conversation_id, title, preview, step_count, "
                    "CAST(strftime('%s', substr(last_modified_time, 1, 19)) AS INTEGER), "
                    "workspace_uris, status, parent_conversation_id, nesting_depth, agent_name "
                    "FROM conversation_summaries";
                const char* sqlFallback =
                    "SELECT conversation_id, title, preview, step_count, "
                    "CAST(strftime('%s', substr(last_modified_time, 1, 19)) AS INTEGER), "
                    "workspace_uris, status, parent_conversation_id, nesting_depth "
                    "FROM conversation_summaries";
                if (sqlite3_prepare_v2(db, sqlWithAgent, -1, &stmt, nullptr) != SQLITE_OK) {
                    if (stmt) {
                        sqlite3_finalize(stmt);
                        stmt = nullptr;
                    }
                    sqlite3_prepare_v2(db, sqlFallback, -1, &stmt, nullptr);
                }
                if (stmt) {
                    int colCount = sqlite3_column_count(stmt);
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
                        if (colCount >= 10) {
                            const char* cAg = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
                            if (cAg) ds.agentName = cAg;
                        }

                        if (ds.nestingDepth > 0 || !ds.parentConversationId.empty() || !ds.agentName.empty()) {
                            subagentIds.insert(ds.id);
                        }
                        if (ds.stepCount <= 0 && ds.title.empty() && ds.preview.empty()) {
                            continue;
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
                        if (it.value().is_object() &&
                            (it.value().value("is_internal", false) ||
                             !it.value().value("parent_conversation_id", "").empty())) {
                            subagentIds.insert(it.key());
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
            std::string status = "DONE";
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
            bool interactive = true;
            std::string nonInteractiveReason;
        };

        struct CandidateConv {
            std::string id;
            fs::path cdir;
            std::string sourceLabel;
            uint64_t ts = 0;
            bool hasTranscript = false;
            bool hasTaskMd = false;
            bool hasDbSummary = false;
            fs::path explicitTranscriptFile;
            std::string projectSlug;
        };

        std::vector<CandidateConv> candidates;
        std::set<std::string> seenIds;
        std::string presenceDir = getCliPresenceDir();
        uint64_t nowSec = static_cast<uint64_t>(std::time(nullptr));
        GatewayConfig gatewayCfg = this->getGatewayConfig();

        if ((gatewayCfg.autoPruneStalePresenceLocks || gatewayCfg.pruneStalePresenceLocks) && !presenceDir.empty()) {
            std::error_code pEc;
            if (fs::exists(presenceDir, pEc) && fs::is_directory(presenceDir, pEc)) {
                int ttlSec = (gatewayCfg.presenceStaleTtlSec > 0)
                    ? gatewayCfg.presenceStaleTtlSec
                    : gatewayCfg.stalePresenceLockMaxAgeSec;
                for (const auto& lockEntry : fs::directory_iterator(presenceDir, pEc)) {
                    if (pEc || !lockEntry.is_regular_file()) continue;
                    if (lockEntry.path().extension() != ".lock") continue;
                    std::string lockId = lockEntry.path().stem().string();
                    if (runningAgyByConvId.find(lockId) == runningAgyByConvId.end()) {
                        std::error_code mEc;
                        auto ftime = fs::last_write_time(lockEntry.path(), mEc);
                        if (!mEc) {
                            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
                            );
                            auto lockSec = std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
                            int64_t ageSec = static_cast<int64_t>(nowSec) - lockSec;
                            if (ageSec >= ttlSec) {
                                std::error_code rmEc;
                                fs::remove(lockEntry.path(), rmEc);
                            }
                        }
                    }
                }
            }
        }

        auto addCandidate = [&](const std::string& id, const fs::path& cdir, const std::string& sourceLabel) {
            if (id.empty() || id[0] == '.' || id == "tempmediaStorage") return;
            if (seenIds.count(id)) return;
            if (phantomIds.count(id)) return;
            if (!includeSubagents && subagentIds.count(id)) return;
            if (!includeSubagents && !cdir.empty()) {
                fs::path perConvMeta = cdir / "cache" / "conversation_metadata.json";
                std::error_code mEc;
                if (fs::exists(perConvMeta, mEc) && !mEc) {
                    try {
                        std::ifstream mf(perConvMeta);
                        auto mj = nlohmann::json::parse(mf, nullptr, false);
                        if (!mj.is_discarded() && mj.is_object() &&
                            (mj.value("is_internal", false) ||
                             !mj.value("parent_conversation_id", "").empty())) {
                            subagentIds.insert(id);
                            return;
                        }
                    } catch (...) {}
                }
            }

            fs::path transcriptPath = cdir.empty() ? fs::path() : (cdir / ".system_generated" / "logs" / "transcript.jsonl");
            fs::path taskMdPath = cdir.empty() ? fs::path() : (cdir / "task.md");
            bool hasTranscript = !transcriptPath.empty() && fs::exists(transcriptPath);
            bool hasTaskMd = !taskMdPath.empty() && fs::exists(taskMdPath);
            auto sumIt = summariesMap.find(id);
            bool hasDbSummary = (sumIt != summariesMap.end());
            bool isRunningInAgy = (runningAgyByConvId.find(id) != runningAgyByConvId.end());
            if (!hasTranscript && !hasTaskMd && !isRunningInAgy) return;

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
            candidates.push_back({id, cdir, sourceLabel, ts, hasTranscript, hasTaskMd, hasDbSummary, {}, ""});
        };

        auto addProviderFileCandidate = [&](const std::string& id,
                                            const fs::path& jsonlPath,
                                            const std::string& sourceLabel,
                                            const std::string& projectSlug) {
            if (id.empty() || id[0] == '.') return;
            if (seenIds.count(id)) return;
            if (!includeSubagents && subagentIds.count(id)) return;
            std::error_code ec;
            bool fileReady = fs::exists(jsonlPath, ec) &&
                             fs::is_regular_file(jsonlPath, ec) &&
                             fs::file_size(jsonlPath, ec) > 0 && !ec;
            uint64_t ts = 0;
            if (fileReady) {
                try {
                    auto ftime = fs::last_write_time(jsonlPath);
                    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
                    );
                    auto secs = std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
                    if (secs > 0) ts = static_cast<uint64_t>(secs);
                } catch (...) {}
            } else if (sourceLabel == "cursor") {
                bool hasLiveSubagents = (cursorParentMaxSubagentTs.find(id) != cursorParentMaxSubagentTs.end());
                nlohmann::json hookJ;
                bool hasHook = readCursorHookStateJson(id, hookJ);
                bool hookRunningWithPrompt = false;
                if (hasHook && !hookJ.value("is_subagent", false) &&
                    hookJ.value("parent_session_id", "").empty() &&
                    hookJ.value("status", "") == "RUNNING") {
                    std::string fp = cleanProviderUserPrompt(
                        hookJ.value("first_prompt", hookJ.value("prompt_preview", "")));
                    hookRunningWithPrompt = !fp.empty();
                }
                if (!hasLiveSubagents && !hookRunningWithPrompt) {
                    return;
                }
                if (hasHook) {
                    uint64_t upd = static_cast<uint64_t>(
                        hookJ.value("updated_ts", hookJ.value("updated_at", hookJ.value("updated_epoch", hookJ.value("start_ts", static_cast<int64_t>(0))))));
                    if (upd > ts) ts = upd;
                }
            } else {
                return;
            }

            if (sourceLabel == "cursor") {
                auto pIt = cursorParentMaxSubagentTs.find(id);
                if (pIt != cursorParentMaxSubagentTs.end() && pIt->second > ts) {
                    ts = pIt->second;
                }
                if (ts == 0) {
                    return;
                }
            }

            seenIds.insert(id);
            candidates.push_back({id, jsonlPath.parent_path(), sourceLabel, ts, true, false, false, jsonlPath, projectSlug});
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

        for (const auto& kv : runningAgyByConvId) {
            if (seenIds.count(kv.first)) continue;
            if (!includeSubagents && subagentIds.count(kv.first)) continue;
            fs::path cdir = fs::path(getCliBrainDir()) / kv.first;
            addCandidate(kv.first, fs::exists(cdir) ? cdir : fs::path(), "cli");
        }

        // Scan Claude Code sessions (~/.claude/projects/<project-slug>/<uuid>.jsonl)
        {
            std::string claudeRoot = getClaudeProjectsDir();
            std::error_code ec;
            if (!claudeRoot.empty() && fs::exists(claudeRoot, ec) && fs::is_directory(claudeRoot, ec)) {
                for (const auto& projEntry : fs::directory_iterator(claudeRoot, ec)) {
                    if (ec || !projEntry.is_directory()) continue;
                    std::string slug = projEntry.path().filename().string();
                    std::error_code fEc;
                    for (const auto& fEntry : fs::directory_iterator(projEntry.path(), fEc)) {
                        if (fEc || !fEntry.is_regular_file()) continue;
                        if (fEntry.path().extension() != ".jsonl") continue;
                        std::string id = fEntry.path().stem().string();
                        addProviderFileCandidate(id, fEntry.path(), "claude", slug);
                    }
                }
            }
        }

        // Scan Cursor sessions (~/.cursor/projects/<project-slug>/agent-transcripts/<uuid>/<uuid>.jsonl)
        {
            std::string cursorRoot = getCursorProjectsDir();
            collectCursorSubagentIdsAndParentMtimes(cursorRoot, subagentIds, cursorParentMaxSubagentTs);
            std::error_code ec;
            if (!cursorRoot.empty() && fs::exists(cursorRoot, ec) && fs::is_directory(cursorRoot, ec)) {
                for (const auto& projEntry : fs::directory_iterator(cursorRoot, ec)) {
                    if (ec || !projEntry.is_directory()) continue;
                    std::string slug = projEntry.path().filename().string();
                    fs::path transDir = projEntry.path() / "agent-transcripts";
                    std::error_code tEc;
                    if (!fs::exists(transDir, tEc) || !fs::is_directory(transDir, tEc)) continue;
                    for (const auto& convEntry : fs::directory_iterator(transDir, tEc)) {
                        if (tEc || !convEntry.is_directory()) continue;
                        std::string id = convEntry.path().filename().string();
                        if (id == "subagents") continue;
                        fs::path jsonlFile = convEntry.path() / (id + ".jsonl");
                        addProviderFileCandidate(id, jsonlFile, "cursor", slug);
                    }
                }
            }
            // Also scan Cursor hook state dir for Turn-1 active sessions without a transcript dir yet
            std::string hookDir = getCursorHookStateDir();
            std::error_code hEc;
            if (!hookDir.empty() && fs::exists(hookDir, hEc) && fs::is_directory(hookDir, hEc)) {
                for (const auto& hEntry : fs::directory_iterator(hookDir, hEc)) {
                    if (hEc || !hEntry.is_regular_file() || hEntry.path().extension() != ".json") continue;
                    std::string id = hEntry.path().stem().string();
                    if (id.empty() || id[0] == '.' || id == "agy_ls_endpoints" ||
                        id == "current" || id == "cursor-telemetry-probe" ||
                        id.rfind("sess-", 0) == 0 || id.rfind("agent-", 0) == 0) continue;
                    if (seenIds.count(id)) continue;
                    if (!includeSubagents && subagentIds.count(id)) continue;
                    nlohmann::json hj;
                    if (!readCursorHookStateJson(id, hj)) continue;
                    if (hj.value("is_subagent", false) || !hj.value("parent_session_id", "").empty()) {
                        continue;
                    }
                    std::string hStatus = hj.value("status", "");
                    if (hStatus != "RUNNING" && hStatus != "DONE" && hStatus != "IDLE") continue;
                    std::string fp = cleanProviderUserPrompt(
                        hj.value("first_prompt", hj.value("prompt_preview", "")));
                    bool hasLiveSubagents = (cursorParentMaxSubagentTs.find(id) != cursorParentMaxSubagentTs.end());
                    if (fp.empty() && !hasLiveSubagents) continue;
                    uint64_t upd = static_cast<uint64_t>(
                        hj.value("updated_ts", hj.value("updated_at", hj.value("updated_epoch", hj.value("start_ts", static_cast<int64_t>(0))))));
                    if (upd == 0 || (nowSec > upd && (nowSec - upd) > 86400)) continue;
                    std::string ws = hj.value("workspace", hj.value("workspace_path", ""));
                    std::string slug = ws.empty() ? "" : fs::path(ws).filename().string();
                    fs::path synthJsonl = fs::path(hookDir) / (id + ".jsonl");
                    addProviderFileCandidate(id, synthJsonl, "cursor", slug);
                }
            }
        }

        std::vector<LsEndpoint> liveLsEndpoints = discoverLsEndpoints();
        bool hasAnyLiveCliLs = false;
        for (const auto& ep : liveLsEndpoints) {
            if (ep.isCli) {
                hasAnyLiveCliLs = true;
                break;
            }
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
            bool hasDbSummary = cand.hasDbSummary;

            if (sourceLabel == "claude" || sourceLabel == "cursor") {
                NormalizedProviderTranscript norm = parseProviderTranscriptJsonl(
                    cand.explicitTranscriptFile, sourceLabel, -1, cand.projectSlug, queryLower.empty());
                if (!norm.hasValidTurns) {
                    return false;
                }
                if (norm.lastActivitySec > ts) {
                    ts = norm.lastActivitySec;
                }
                if (sourceLabel == "cursor") {
                    auto pIt = cursorParentMaxSubagentTs.find(id);
                    if (pIt != cursorParentMaxSubagentTs.end() && pIt->second > ts) {
                        ts = pIt->second;
                    }
                }
                std::string title = norm.firstPrompt;
                auto nlPos = title.find_first_of("\r\n");
                if (nlPos != std::string::npos) {
                    title = title.substr(0, nlPos);
                }
                if (title.empty()) {
                    if (!includeSubagents) {
                        return false;
                    }
                    title = "Conversation " + id.substr(0, std::min<size_t>(8, id.length()));
                } else if (title.length() > 80) {
                    title = title.substr(0, 77) + "...";
                }

                std::string matchedPrompt;
                int matchedStepIndex = -1;
                if (!queryLower.empty()) {
                    for (const auto& st : norm.steps) {
                        std::string stText = st.value("text", st.value("thinking", ""));
                        if (toLowerAscii(stText).find(queryLower) != std::string::npos) {
                            matchedPrompt = (stText.size() > 200) ? (stText.substr(0, 197) + "...") : stText;
                            matchedStepIndex = st.value("stepIndex", -1);
                            break;
                        }
                    }
                    bool matchesMeta = (toLowerAscii(title).find(queryLower) != std::string::npos) ||
                                       (toLowerAscii(norm.workspaceShort).find(queryLower) != std::string::npos) ||
                                       (toLowerAscii(norm.workspacePath).find(queryLower) != std::string::npos) ||
                                       (toLowerAscii(norm.model).find(queryLower) != std::string::npos) ||
                                       (toLowerAscii(id).find(queryLower) != std::string::npos) ||
                                       !matchedPrompt.empty();
                    if (!matchesMeta) {
                        return false;
                    }
                }

                ConvEntry ce;
                ce.id = id;
                ce.title = title;
                ce.source = sourceLabel;
                ce.status = norm.status;
                ce.timestamp = ts;
                int recWin = (gatewayCfg.activeRecencyWindowSec > 0) ? gatewayCfg.activeRecencyWindowSec : 120;
                ce.active = (norm.status != "IDLE") &&
                            ((norm.status == "RUNNING") ||
                             (ts > 0 && nowSec >= ts && (nowSec - ts) < static_cast<uint64_t>(recWin)));
                ce.openInTerminal = false;
                ce.terminalPts = "";
                ce.workspace = norm.workspaceShort;
                ce.workspacePath = norm.workspacePath;
                ce.model = norm.model;
                ce.stepCount = norm.totalSteps;
                ce.firstPrompt = norm.firstPrompt;
                ce.lastPrompt = norm.lastPrompt;
                ce.lastPromptStepIndex = norm.lastPromptStepIndex;
                ce.userPrompts = std::move(norm.userPrompts);
                ce.matchedPrompt = matchedPrompt;
                ce.matchedStepIndex = matchedStepIndex;
                ce.interactive = norm.interactive;
                ce.nonInteractiveReason = norm.nonInteractiveReason;
                collected.push_back(std::move(ce));
                return true;
            }

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
            std::string inlineTranscriptModel;

            if (hasTranscript) {
                std::ifstream f(transcriptPath);
                std::string line;
                int lineNum = 0;
                int filteredIdx = 0;
                while (std::getline(f, line)) {
                    if (line.empty()) continue;
                    if (line.find("USER_SETTINGS_CHANGE") != std::string::npos ||
                        line.find("Model Selection") != std::string::npos) {
                        std::string matched = mapModelToHumanLabel(line);
                        if (!matched.empty()) {
                            inlineTranscriptModel = matched;
                        }
                    }
                    if (queryLower.empty() && lineNum > 0 &&
                        line.find("\"USER_INPUT\"") == std::string::npos) {
                        int stepIdx = lineNum;
                        auto siPos = line.find("\"step_index\"");
                        if (siPos != std::string::npos) {
                            auto colonPos = line.find(':', siPos + 12);
                            if (colonPos != std::string::npos) {
                                size_t numPos = line.find_first_of("0123456789", colonPos + 1);
                                if (numPos != std::string::npos && numPos - colonPos <= 4) {
                                    try {
                                        stepIdx = std::stoi(line.substr(numPos, 10));
                                    } catch (...) {}
                                }
                            }
                        }
                        ++lineNum;
                        totalStepsInTranscript = std::max(totalStepsInTranscript, stepIdx + 1);
                        auto caPos = line.find("\"created_at\"");
                        if (caPos != std::string::npos) {
                            auto q1 = line.find('"', caPos + 12);
                            if (q1 != std::string::npos) {
                                auto q2 = line.find('"', q1 + 1);
                                if (q2 != std::string::npos && q2 > q1 + 1 && (q2 - q1) < 40) {
                                    int64_t stepEpoch = parseIso8601ToEpochSec(line.substr(q1 + 1, q2 - q1 - 1));
                                    if (stepEpoch > 0 && static_cast<uint64_t>(stepEpoch) > ts) {
                                        ts = static_cast<uint64_t>(stepEpoch);
                                    }
                                }
                            }
                        }
                        bool hasToolsFast = line.find("\"tool_calls\"") != std::string::npos &&
                                            line.find("\"tool_calls\":[]") == std::string::npos &&
                                            line.find("\"tool_calls\": []") == std::string::npos;
                        if (line.find("\"PLANNER_RESPONSE\"") != std::string::npos ||
                            line.find("\"ERROR_MESSAGE\"") != std::string::npos ||
                            hasToolsFast) {
                            ++filteredIdx;
                        }
                        continue;
                    }
                    try {
                        auto j = nlohmann::json::parse(line, nullptr, false);
                        if (j.is_discarded() || !j.is_object()) continue;
                        int stepIdx = j.value("step_index", lineNum);
                        ++lineNum;
                        totalStepsInTranscript = std::max(totalStepsInTranscript, stepIdx + 1);
                        std::string stype = j.value("type", "");
                        if (lineNum == 1 && (stype == "SYSTEM_MESSAGE" || j.value("source", "") == "SYSTEM") && !includeSubagents) {
                            isInternalSubagent = true;
                            break;
                        }
                        std::string content = j.value("content", "");
                        std::string thinking = j.value("thinking", "");
                        std::string createdAt = j.value("created_at", "");
                        if (!createdAt.empty()) {
                            int64_t stepEpoch = parseIso8601ToEpochSec(createdAt);
                            if (stepEpoch > 0 && static_cast<uint64_t>(stepEpoch) > ts) {
                                ts = static_cast<uint64_t>(stepEpoch);
                            }
                        }
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
            if (!includeSubagents && hasTranscript && firstPrompt.empty() &&
                (!hasDbSummary || sumIt->second.title.empty())) {
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

            if (ts == 0 && openInTerminal) {
                ts = nowSec;
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
                if (openInTerminal) {
                    title = !workspaceShort.empty()
                        ? ("New Session (" + workspaceShort + ")")
                        : ("New Session " + id.substr(0, std::min<size_t>(8, id.length())));
                } else if (!includeSubagents) {
                    return false;
                } else {
                    title = "Conversation " + id.substr(0, std::min<size_t>(8, id.length()));
                }
            } else if (title.length() > 80) {
                title = title.substr(0, 77) + "...";
            }

            int recWin = (gatewayCfg.activeRecencyWindowSec > 0) ? gatewayCfg.activeRecencyWindowSec : 120;
            bool isActive = openInTerminal;
            if (!presenceDir.empty() && fs::exists(fs::path(presenceDir) / (id + ".lock"))) {
                if (!gatewayCfg.strictPresenceFdCheck || openInTerminal) {
                    isActive = true;
                }
            }
            if (!isActive) {
                if (hasDbSummary && sumIt->second.status == "CASCADE_RUN_STATUS_RUNNING") {
                    isActive = true;
                } else if (ts > 0 && nowSec >= ts && (nowSec - ts) < static_cast<uint64_t>(recWin)) {
                    isActive = true;
                }
            }

            std::string modelLabel = !inlineTranscriptModel.empty()
                ? inlineTranscriptModel
                : extractConversationModelById(id, hasTranscript);
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
            ce.status = isActive ? "RUNNING" : "DONE";
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

            if (openInTerminal) {
                bool hasCsrf = false;
                int lockedPort = (agyIt != runningAgyByConvId.end()) ? agyIt->second.httpPort : 0;
                for (const auto& ep : liveLsEndpoints) {
                    if (!ep.isCli) continue;
                    if ((lockedPort > 0 && ep.port == lockedPort) ||
                        (lockedPort <= 0 && ep.lockedConversationId == id)) {
                        hasCsrf = true;
                        break;
                    }
                }
                if (!hasCsrf && !gatewayCfg.allowAgyAgentApiFallback) {
                    ce.interactive = false;
                    ce.nonInteractiveReason =
                        "Antigravity CLI session is open in terminal without a registered ConnectRPC CSRF token.";
                }
            } else if (sourceLabel == "cli" && !hasAnyLiveCliLs) {
                ce.interactive = false;
                ce.nonInteractiveReason =
                    "Antigravity CLI session is not running in an active terminal or language server. Resume it in a terminal to interact.";
            } else if (sourceLabel == "ide" && liveLsEndpoints.empty()) {
                ce.interactive = false;
                ce.nonInteractiveReason =
                    "Antigravity IDE language server is not currently running.";
            }

            collected.push_back(std::move(ce));
            return true;
        };

        for (const auto& cand : candidates) {
            if (collected.size() >= limit) {
                break;
            }
            buildConvEntry(cand);
        }

        // Strict descending sort by last activity timestamp (latest prompt/response/question or file mtime)
        std::stable_sort(collected.begin(), collected.end(), [](const ConvEntry& a, const ConvEntry& b) {
            return a.timestamp > b.timestamp;
        });

        nlohmann::json conversations = nlohmann::json::array();
        size_t count = std::min(collected.size(), limit);
        for (size_t i = 0; i < count; ++i) {
            conversations.push_back({
                {"id", collected[i].id},
                {"title", collected[i].title},
                {"source", collected[i].source},
                {"status", collected[i].status},
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
                {"matchedStepIndex", collected[i].matchedStepIndex},
                {"interactive", collected[i].interactive},
                {"nonInteractiveReason", collected[i].nonInteractiveReason}
            });
        }

        nlohmann::json resp;
        resp["conversations"] = conversations;
        res.set_content(resp.dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    };
    _server->Get("/api/agent/conversations", handleAgentConversations);
    _server->Get("/api/agent/history", handleAgentConversations);

    _server->Get("/api/agent/transcript", [this, resolveSessionWorkspacePath, getIdeBrainDir, getCliBrainDir, stripUserRequestTags, discoverLsEndpoints, lastRpcEndpoint, lastRpcMutex, extractTrajectoryModelName, extractConversationModelById, mapLabelToCliModelId](
                                              const httplib::Request& req, httplib::Response& res) {
        std::string reqId = extractOrGenerateRequestId(req);
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("X-Request-Id", reqId);
        std::string id = req.get_param_value("id");
        if (id.empty()) {
            id = req.get_param_value("conversationId");
        }
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
            // Check Claude Code and Cursor transcript directories before falling back to ConnectRPC
            std::string providerSource;
            std::string providerSlug;
            fs::path providerPath = findProviderTranscriptById(id, providerSource, providerSlug);
            if (!providerPath.empty()) {
                auto parsed = parseProviderTranscriptJsonl(providerPath, providerSource, sinceIndex, providerSlug);
                nlohmann::json resp;
                resp["conversationId"] = id;
                resp["source"] = providerSource;
                resp["status"] = parsed.status;
                resp["model"] = parsed.model;
                resp["modelId"] = parsed.model;
                resp["workspace"] = parsed.workspaceShort;
                resp["workspacePath"] = parsed.workspacePath;
                resp["stepCount"] = parsed.totalSteps;
                resp["steps"] = std::move(parsed.steps);
                resp["interactive"] = parsed.interactive;
                resp["nonInteractiveReason"] = parsed.nonInteractiveReason;
                res.set_content(resp.dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
                return;
            }

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
                                } else if (stType == "CORTEX_STEP_TYPE_ASK_QUESTION" ||
                                           rs.contains("askQuestion") ||
                                           (rs.contains("requestedInteraction") && rs["requestedInteraction"].is_object() &&
                                            rs["requestedInteraction"].contains("askQuestion"))) {
                                    nlohmann::json rawQ = nlohmann::json::array();
                                    if (rs.contains("askQuestion") && rs["askQuestion"].is_object() &&
                                        rs["askQuestion"].contains("questions")) {
                                        rawQ = rs["askQuestion"]["questions"];
                                    } else if (rs.contains("requestedInteraction") && rs["requestedInteraction"].is_object() &&
                                               rs["requestedInteraction"].contains("askQuestion") &&
                                               rs["requestedInteraction"]["askQuestion"].is_object() &&
                                               rs["requestedInteraction"]["askQuestion"].contains("questions")) {
                                        rawQ = rs["requestedInteraction"]["askQuestion"]["questions"];
                                    }
                                    nlohmann::json normQ = normalizeQuestionsJson(rawQ);
                                    bool isAnswered = (rs.value("status", "") != "CORTEX_STEP_STATUS_WAITING");
                                    std::string qDisplayText;
                                    for (size_t qi = 0; qi < normQ.size(); ++qi) {
                                        if (!qDisplayText.empty()) qDisplayText += "\n\n";
                                        qDisplayText += normQ[qi].value("question", "Question");
                                        if (normQ[qi].contains("options") && normQ[qi]["options"].is_array()) {
                                            size_t oi = 0;
                                            for (const auto& optStr : normQ[qi]["options"]) {
                                                if (optStr.is_string()) {
                                                    qDisplayText += "\n" + std::to_string(++oi) + ". " + optStr.get<std::string>();
                                                }
                                            }
                                        }
                                    }
                                    parsedLiveSteps.push_back({
                                        {"index", stepIdx},
                                        {"stepIndex", stepIdx},
                                        {"type", "ASK_QUESTION"},
                                        {"status", isAnswered ? "DONE" : "WAITING"},
                                        {"text", qDisplayText},
                                        {"askQuestion", {
                                            {"questions", normQ},
                                            {"answered", isAnswered},
                                            {"answers", nlohmann::json::array()}
                                        }},
                                        {"step", {
                                            {"case", "askQuestion"},
                                            {"value", qDisplayText},
                                            {"questions", normQ},
                                            {"answered", isAnswered}
                                        }}
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
                                } else if (stType == "CORTEX_STEP_TYPE_CHECKPOINT") {
                                    std::string rawSummary;
                                    int64_t rpcStepsSum = 0;
                                    if (rs.contains("checkpoint") && rs["checkpoint"].is_object()) {
                                        const auto& cp = rs["checkpoint"];
                                        rawSummary = cp.value("sessionSummary",
                                                     cp.value("userIntent", ""));
                                        int64_t startIdx = cp.value("includedStepIndexStart", static_cast<int64_t>(0));
                                        int64_t endIdx = cp.value("includedStepIndexEnd", static_cast<int64_t>(0));
                                        if (endIdx > startIdx) rpcStepsSum = endIdx - startIdx;
                                    }
                                    std::map<int, AntigravityCheckpointStats> rpcDbStats;
                                    loadAntigravityCheckpointStatsFromDb(id, rpcDbStats);
                                    std::string compactDisplay;
                                    if (rpcDbStats.count(stepIdx)) {
                                        const auto& cs = rpcDbStats[stepIdx];
                                        int64_t sSum = (cs.stepsSummarized > 0) ? cs.stepsSummarized : rpcStepsSum;
                                        compactDisplay = joinCompactionStatsParts(sSum, cs.beforeTokens, cs.afterTokens, cs.summaryTokens, cs.durationSec);
                                    } else {
                                        uint64_t sumTok = rawSummary.empty() ? 0 : std::max<uint64_t>(1, rawSummary.size() / 4);
                                        compactDisplay = joinCompactionStatsParts(rpcStepsSum, 0, 0, sumTok, -1);
                                    }
                                    parsedLiveSteps.push_back({
                                        {"index", stepIdx},
                                        {"stepIndex", stepIdx},
                                        {"type", "PLANNER_RESPONSE"},
                                        {"thinking", ""},
                                        {"plannerResponse", ""},
                                        {"text", compactDisplay},
                                        {"tool_calls", nlohmann::json::array()},
                                        {"step", {{"case", "text"}, {"value", compactDisplay}}}
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
                resp["interactive"] = true;
                resp["nonInteractiveReason"] = "";
                std::string wsPath = resolveSessionWorkspacePath(id);
                resp["workspacePath"] = wsPath;
                resp["workspace"] = !wsPath.empty() ? fs::path(wsPath).filename().string() : "";
                res.set_content(resp.dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
                return;
            }

            fs::path cliIdDir = fs::path(getCliBrainDir()) / id;
            fs::path ideIdDir = fs::path(getIdeBrainDir()) / id;
            bool cliHasTaskMd = fs::exists(cliIdDir / "task.md") || fs::exists(cliIdDir / "implementation_plan.md");
            bool ideHasTaskMd = fs::exists(ideIdDir / "task.md") || fs::exists(ideIdDir / "implementation_plan.md");

            if (isLockedByRunningAgy || cliHasTaskMd || ideHasTaskMd) {
                fs::path fallbackBrainDir = (fs::exists(cliIdDir) || isLockedByRunningAgy) ? cliIdDir : ideIdDir;
                nlohmann::json fallbackSteps = nlohmann::json::array();
                fs::path mdPath = fs::exists(fallbackBrainDir / "task.md") ? (fallbackBrainDir / "task.md")
                                : (fs::exists(fallbackBrainDir / "implementation_plan.md") ? (fallbackBrainDir / "implementation_plan.md") : fs::path());
                if (!mdPath.empty()) {
                    std::ifstream mdf(mdPath);
                    if (mdf.is_open()) {
                        std::string mdContent((std::istreambuf_iterator<char>(mdf)),
                                              std::istreambuf_iterator<char>());
                        fallbackSteps.push_back({
                            {"index", 0},
                            {"stepIndex", 0},
                            {"type", "PLANNER_RESPONSE"},
                            {"thinking", ""},
                            {"plannerResponse", ""},
                            {"text", mdContent},
                            {"tool_calls", nlohmann::json::array()},
                            {"step", {{"case", "text"}, {"value", mdContent}}}
                        });
                    }
                }

                bool agyInteractive = false;
                std::string agyNonInteractiveReason;
                {
                    auto eps = discoverLsEndpoints();
                    bool hasLiveCliLs = false;
                    bool hasLiveIdeLs = false;
                    bool hasRegisteredCsrf = false;
                    for (const auto& ep : eps) {
                        if (ep.isCli) {
                            hasLiveCliLs = true;
                            if (isLockedByRunningAgy) {
                                if ((lockedHttpPort > 0 && ep.port == lockedHttpPort) ||
                                    (lockedHttpPort <= 0 && ep.lockedConversationId == id)) {
                                    hasRegisteredCsrf = true;
                                }
                            }
                        } else {
                            hasLiveIdeLs = true;
                        }
                    }
                    const char* envP = std::getenv("AGY_LS_PORT");
                    const char* envC = std::getenv("AGY_LS_CSRF_TOKEN");
                    if (envP && *envP && envC && *envC) {
                        hasLiveCliLs = true;
                        hasLiveIdeLs = true;
                        if (isLockedByRunningAgy) {
                            try {
                                int epPort = std::stoi(envP);
                                if (lockedHttpPort <= 0 || epPort == lockedHttpPort) {
                                    hasRegisteredCsrf = true;
                                }
                            } catch (...) {}
                        }
                    }
                    if (isLockedByRunningAgy) {
                        agyInteractive = hasRegisteredCsrf;
                        if (!agyInteractive) {
                            agyNonInteractiveReason =
                                "Antigravity CLI session is locked in terminal without a registered ConnectRPC endpoint.";
                        }
                    } else if (isCliConv) {
                        agyInteractive = hasLiveCliLs;
                        if (!agyInteractive) {
                            agyNonInteractiveReason =
                                "No live Antigravity CLI language server endpoint is running for this session.";
                        }
                    } else {
                        agyInteractive = hasLiveIdeLs || hasLiveCliLs;
                        if (!agyInteractive) {
                            agyNonInteractiveReason =
                                "No live Antigravity language server endpoint is running for this session.";
                        }
                    }
                }

                nlohmann::json resp;
                resp["conversationId"] = id;
                resp["source"] = isLockedByRunningAgy ? "cli" : "ide";
                resp["model"] = extractConversationModelById(id);
                resp["stepCount"] = static_cast<int>(fallbackSteps.size());
                resp["steps"] = fallbackSteps;
                resp["openInTerminal"] = isLockedByRunningAgy;
                resp["interactive"] = agyInteractive;
                resp["nonInteractiveReason"] = agyNonInteractiveReason;

                if (isLockedByRunningAgy) {
                    for (const auto& proc : discoverRunningAgyProcesses()) {
                        if (proc.lockedConversationId == id) {
                            resp["terminalPts"] = proc.terminalPts;
                            break;
                        }
                    }
                }
                std::string wsPath = resolveSessionWorkspacePath(id);
                resp["workspacePath"] = wsPath;
                resp["workspace"] = !wsPath.empty() ? fs::path(wsPath).filename().string() : "";

                res.set_content(resp.dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
                return;
            }

            res.status = 404;
            res.set_content("{\"error\":\"Conversation transcript not found\"}", "application/json");
            return;
        }

        std::map<int, AntigravityCheckpointStats> dbCheckpointStats;
        loadAntigravityCheckpointStatsFromDb(id, dbCheckpointStats);

        std::ifstream f(transcriptPath);
        std::string line;
        nlohmann::json steps = nlohmann::json::array();
        int lineNum = 0;
        int totalStepCount = 0;
        int prevCheckpointStepIdx = 0;
        uint64_t lastPlannerContextTokens = 0;
        uint64_t prevStepEpochSec = 0;

        while (std::getline(f, line)) {
            if (line.empty()) continue;
            try {
                auto j = nlohmann::json::parse(line, nullptr, false);
                if (j.is_discarded() || !j.is_object()) continue;

                int stepIdx = j.value("step_index", lineNum);
                ++lineNum;
                totalStepCount = std::max(totalStepCount, stepIdx + 1);

                std::string lineType = j.value("type", "");
                std::string lineCreatedAt = j.value("created_at", "");
                uint64_t curEpochSec = lineCreatedAt.empty() ? 0 : parseIso8601ToEpochSec(lineCreatedAt);

                if (lineType == "PLANNER_RESPONSE") {
                    uint64_t inT = j.value("input_tokens", static_cast<uint64_t>(0)) +
                                   j.value("cache_read_tokens", static_cast<uint64_t>(0)) +
                                   j.value("output_tokens", static_cast<uint64_t>(0));
                    if (inT > 0) lastPlannerContextTokens = inT;
                }

                std::string checkpointCompactDisplay;
                if (lineType == "CHECKPOINT") {
                    if (dbCheckpointStats.count(stepIdx)) {
                        const auto& cs = dbCheckpointStats[stepIdx];
                        int64_t stepsSum = (cs.stepsSummarized > 0) ? cs.stepsSummarized : std::max<int64_t>(0, stepIdx - prevCheckpointStepIdx);
                        checkpointCompactDisplay = joinCompactionStatsParts(stepsSum, cs.beforeTokens, cs.afterTokens, cs.summaryTokens, cs.durationSec);
                    } else {
                        int64_t stepsSum = (stepIdx > prevCheckpointStepIdx) ? (stepIdx - prevCheckpointStepIdx) : stepIdx;
                        std::string rawContent = j.value("content", "");
                        uint64_t sumTok = rawContent.empty() ? 0 : std::max<uint64_t>(1, rawContent.size() / 4);
                        uint64_t befTok = lastPlannerContextTokens;
                        uint64_t aftTok = (befTok > 0 && sumTok > 0) ? (16280 + sumTok) : 0;
                        int64_t durSec = (curEpochSec > 0 && prevStepEpochSec > 0 && curEpochSec >= prevStepEpochSec)
                            ? static_cast<int64_t>(curEpochSec - prevStepEpochSec) : -1;
                        checkpointCompactDisplay = joinCompactionStatsParts(stepsSum, befTok, aftTok, sumTok, durSec);
                    }
                    prevCheckpointStepIdx = stepIdx;
                }
                if (curEpochSec > 0) {
                    prevStepEpochSec = curEpochSec;
                }

                if (sinceIndex >= 0 && stepIdx <= sinceIndex) {
                    continue;
                }

                std::string type = j.value("type", "");
                std::string content = j.value("content", "");
                std::string thinking = j.value("thinking", "");
                std::string createdAt = j.value("created_at", "");

                if (type == "GENERIC" || type == "USER_INPUT") {
                    for (auto& prevSt : steps) {
                        if (prevSt.is_object() && prevSt.contains("askQuestion") &&
                            prevSt["askQuestion"].is_object() &&
                            prevSt["askQuestion"].value("answered", true) == false) {
                            prevSt["askQuestion"]["answered"] = true;
                            if (prevSt.contains("step") && prevSt["step"].is_object() && prevSt["step"].contains("answered")) {
                                prevSt["step"]["answered"] = true;
                            }
                            nlohmann::json ansArr = nlohmann::json::array();
                            std::istringstream iss(content);
                            std::string aLine;
                            while (std::getline(iss, aLine)) {
                                auto fPos = aLine.find_first_not_of(" \t\r\n");
                                if (fPos == std::string::npos) continue;
                                auto lPos = aLine.find_last_not_of(" \t\r\n");
                                std::string trimmedLine = aLine.substr(fPos, lPos - fPos + 1);
                                if (trimmedLine.size() > 3 && trimmedLine[0] == 'A' &&
                                    std::isdigit(static_cast<unsigned char>(trimmedLine[1]))) {
                                    auto colonPos = trimmedLine.find(':');
                                    if (colonPos != std::string::npos && colonPos + 1 < trimmedLine.size()) {
                                        std::string val = trimmedLine.substr(colonPos + 1);
                                        auto vf = val.find_first_not_of(" \t");
                                        if (vf != std::string::npos) val = val.substr(vf);
                                        if (!val.empty()) ansArr.push_back(val);
                                        continue;
                                    }
                                }
                                if (type == "USER_INPUT") {
                                    std::string cleanAns = stripUserRequestTags(content);
                                    if (!cleanAns.empty()) {
                                        ansArr.push_back(cleanAns);
                                    }
                                    break;
                                }
                            }
                            if (!ansArr.empty()) {
                                prevSt["askQuestion"]["answers"] = ansArr;
                            }
                        }
                    }
                }

                nlohmann::json parsedToolCalls = nlohmann::json::array();
                nlohmann::json stepAskQuestions = nlohmann::json::array();
                if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
                    for (const auto& tc : j["tool_calls"]) {
                        if (!tc.is_object()) continue;
                        std::string toolName = tc.value("name", "tool");
                        nlohmann::json argsObj = nlohmann::json::object();
                        if (tc.contains("arguments")) {
                            if (tc["arguments"].is_object()) {
                                argsObj = tc["arguments"];
                            } else if (tc["arguments"].is_string()) {
                                auto pa = nlohmann::json::parse(tc["arguments"].get<std::string>(), nullptr, false);
                                if (!pa.is_discarded() && pa.is_object()) argsObj = std::move(pa);
                            }
                        } else if (tc.contains("args")) {
                            if (tc["args"].is_object()) {
                                argsObj = tc["args"];
                            } else if (tc["args"].is_string()) {
                                auto pa = nlohmann::json::parse(tc["args"].get<std::string>(), nullptr, false);
                                if (!pa.is_discarded() && pa.is_object()) argsObj = std::move(pa);
                            }
                        }

                        if (toolName == "ask_question" || toolName == "AskUserQuestion") {
                            nlohmann::json normQ = argsObj.contains("questions")
                                ? normalizeQuestionsJson(argsObj["questions"])
                                : nlohmann::json::array();
                            if (normQ.empty() && argsObj.contains("question") && argsObj["question"].is_string()) {
                                normQ = normalizeQuestionsJson(nlohmann::json::array({argsObj}));
                            }
                            if (!normQ.empty()) {
                                argsObj["questions"] = normQ;
                                stepAskQuestions = normQ;
                            }
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
                    nlohmann::json stepJson = {
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
                    };
                    if (!stepAskQuestions.empty()) {
                        stepJson["askQuestion"] = {
                            {"questions", stepAskQuestions},
                            {"answered", false},
                            {"answers", nlohmann::json::array()}
                        };
                    }
                    steps.push_back(std::move(stepJson));
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
                } else if (type == "CHECKPOINT") {
                    std::string compactDisplay = !checkpointCompactDisplay.empty()
                        ? checkpointCompactDisplay
                        : extractAntigravityCheckpointSummary(content);
                    steps.push_back({
                        {"index", stepIdx},
                        {"stepIndex", stepIdx},
                        {"type", "PLANNER_RESPONSE"},
                        {"thinking", ""},
                        {"plannerResponse", ""},
                        {"text", compactDisplay},
                        {"tool_calls", nlohmann::json::array()},
                        {"createdAt", createdAt},
                        {"step", {
                            {"case", "text"},
                            {"value", compactDisplay}
                        }}
                    });
                } else if (!parsedToolCalls.empty()) {
                    std::string firstTool = parsedToolCalls[0].value("name", "tool");
                    std::string firstCmd = parsedToolCalls[0].value("command", "");
                    nlohmann::json stepJson = {
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
                    };
                    if (!stepAskQuestions.empty()) {
                        stepJson["askQuestion"] = {
                            {"questions", stepAskQuestions},
                            {"answered", false},
                            {"answers", nlohmann::json::array()}
                        };
                    }
                    steps.push_back(std::move(stepJson));
                }
            } catch (...) {}
        }

        std::string resolvedModel = "gemini-4-argon-medium";
        std::string extractedLabel = extractConversationModelById(id);
        if (!extractedLabel.empty()) {
            resolvedModel = extractedLabel;
        }

        bool agyInteractive = false;
        std::string agyNonInteractiveReason;
        {
            auto eps = discoverLsEndpoints();
            bool hasLiveCliLs = false;
            bool hasLiveIdeLs = false;
            bool hasRegisteredCsrf = false;
            for (const auto& ep : eps) {
                if (ep.isCli) {
                    hasLiveCliLs = true;
                    if (isLockedByRunningAgy) {
                        if ((lockedHttpPort > 0 && ep.port == lockedHttpPort) ||
                            (lockedHttpPort <= 0 && ep.lockedConversationId == id)) {
                            hasRegisteredCsrf = true;
                        }
                    }
                } else {
                    hasLiveIdeLs = true;
                }
            }
            const char* envP = std::getenv("AGY_LS_PORT");
            const char* envC = std::getenv("AGY_LS_CSRF_TOKEN");
            if (envP && *envP && envC && *envC) {
                hasLiveCliLs = true;
                hasLiveIdeLs = true;
                if (isLockedByRunningAgy) {
                    try {
                        int epPort = std::stoi(envP);
                        if (lockedHttpPort <= 0 || epPort == lockedHttpPort) {
                            hasRegisteredCsrf = true;
                        }
                    } catch (...) {}
                }
            }
            if (isLockedByRunningAgy) {
                agyInteractive = hasRegisteredCsrf;
                if (!agyInteractive) {
                    agyNonInteractiveReason =
                        "Antigravity CLI session is locked in terminal without a registered ConnectRPC endpoint.";
                }
            } else if (isCliConv || source == "cli") {
                agyInteractive = hasLiveCliLs;
                if (!agyInteractive) {
                    agyNonInteractiveReason =
                        "No live Antigravity CLI language server endpoint is running for this session.";
                }
            } else {
                agyInteractive = hasLiveIdeLs || hasLiveCliLs;
                if (!agyInteractive) {
                    agyNonInteractiveReason =
                        "No live Antigravity language server endpoint is running for this session.";
                }
            }
        }

        nlohmann::json resp;
        resp["conversationId"] = id;
        resp["source"] = source;
        resp["model"] = resolvedModel;
        resp["modelId"] = !extractedLabel.empty() ? mapLabelToCliModelId(extractedLabel) : resolvedModel;
        resp["stepCount"] = totalStepCount;
        resp["steps"] = steps;
        resp["interactive"] = agyInteractive;
        resp["nonInteractiveReason"] = agyNonInteractiveReason;
        std::string wsPath = resolveSessionWorkspacePath(id);
        resp["workspacePath"] = wsPath;
        resp["workspace"] = !wsPath.empty() ? fs::path(wsPath).filename().string() : "";
        res.set_content(resp.dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    });

    _server->Post("/api/agent/prompt", [this, getWorkspaceRoot, getCliBrainDir, discoverLsEndpoints, lastRpcEndpoint, lastRpcMutex, resolveCascadeConfigForRpc, extractConversationModelById, mapLabelToCliModelId, toLowerAscii](
                                           const httplib::Request& req, httplib::Response& res) {
        std::string reqId = extractOrGenerateRequestId(req);
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("X-Request-Id", reqId);
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
        std::string requestedSource = toLowerAscii(j.value("source", j.value("provider", "")));
        bool autoApprove = j.value("autoApprove", true);
        bool hasQuestionResponses = j.contains("questionResponses") &&
                                    j["questionResponses"].is_array() &&
                                    !j["questionResponses"].empty();
        if (text.empty() && hasQuestionResponses) {
            for (const auto& qr : j["questionResponses"]) {
                if (!qr.is_object()) continue;
                if (qr.contains("selectedOptions") && qr["selectedOptions"].is_array()) {
                    for (const auto& so : qr["selectedOptions"]) {
                        if (so.is_string() && !so.get<std::string>().empty()) {
                            if (!text.empty()) text += "\n";
                            text += so.get<std::string>();
                        }
                    }
                } else if (qr.contains("selectedOptionIds") && qr["selectedOptionIds"].is_array()) {
                    for (const auto& sid : qr["selectedOptionIds"]) {
                        if (sid.is_string() && !sid.get<std::string>().empty()) {
                            if (!text.empty()) text += "\n";
                            text += sid.get<std::string>();
                        }
                    }
                }
                if (qr.contains("writeInResponse") && qr["writeInResponse"].is_string()) {
                    std::string wir = qr["writeInResponse"].get<std::string>();
                    if (!wir.empty()) {
                        if (!text.empty()) text += "\n";
                        text += wir;
                    }
                }
            }
            if (text.empty()) {
                text = "1";
            }
        }
        if (text.empty()) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"error\":\"Prompt text cannot be empty\"}", "application/json");
            return;
        }

        // Trim leading/trailing whitespace to inspect potential slash commands
        std::string trimmedText = text;
        {
            auto fPos = trimmedText.find_first_not_of(" \t\r\n");
            if (fPos != std::string::npos) {
                auto lPos = trimmedText.find_last_not_of(" \t\r\n");
                trimmedText = trimmedText.substr(fPos, lPos - fPos + 1);
            } else {
                trimmedText.clear();
            }
        }

        std::string targetProvider = requestedSource;
        std::string resolvedProviderSlug;
        if (targetProvider != "claude" && targetProvider != "cursor") {
            std::string agentLower = toLowerAscii(agentId);
            if (agentLower == "claude" || agentLower == "cursor") {
                targetProvider = agentLower;
            } else if (!cid.empty() && cid != "new") {
                std::string foundSrc;
                fs::path foundPath = findProviderTranscriptById(cid, foundSrc, resolvedProviderSlug);
                if (!foundPath.empty() && (foundSrc == "claude" || foundSrc == "cursor")) {
                    targetProvider = foundSrc;
                }
            }
        }

        if (targetProvider == "claude" || targetProvider == "cursor") {
            if (j.contains("images") && j["images"].is_array() && !j["images"].empty()) {
                res.status = 400;
                res.set_content(nlohmann::json({
                    {"ok", false},
                    {"status", "error"},
                    {"error", "Image attachments are not supported for terminal multiplexer relay sessions"}
                }).dump(2), "application/json");
                return;
            }

            std::string cmdVerb;
            std::string cmdArgs;
            if (!trimmedText.empty() && trimmedText[0] == '/') {
                auto spPos = trimmedText.find_first_of(" \t\r\n");
                cmdVerb = toLowerAscii(spPos == std::string::npos ? trimmedText : trimmedText.substr(0, spPos));
                if (spPos != std::string::npos) {
                    auto argStart = trimmedText.find_first_not_of(" \t\r\n", spPos);
                    if (argStart != std::string::npos) {
                        cmdArgs = trimmedText.substr(argStart);
                    }
                }
            }

            if (cmdVerb == "/clear" || cmdVerb == "/new" || cmdVerb == "/reset") {
                res.set_content(nlohmann::json({
                    {"status", "cleared"},
                    {"agentId", targetProvider},
                    {"source", targetProvider},
                    {"conversationId", "new"},
                    {"command", cmdVerb},
                    {"response", "Conversation cleared."}
                }).dump(2), "application/json");
                return;
            }
            if (cmdVerb == "/help") {
                std::string helpText = (targetProvider == "claude")
                    ? "Claude Code Mobile Commands:\n"
                      "/compact [focus]\tCompact & summarize conversation context\n"
                      "/clear\tStart a fresh Claude Code conversation\n"
                      "/plan <prompt>\tRun prompt in read-only planning mode (--permission-mode plan)\n"
                      "/effort <level> [prompt]\tRun with effort level (low, medium, high, max)\n"
                      "/status\tShow active session, workspace & model\n"
                      "/doctor\tCheck Claude Code CLI binary & version\n"
                      "/model\tSwitch Claude model\n"
                      "/models\tList available Claude Code models\n"
                      "/provider\tSwitch AI provider (agy, claude, cursor)\n"
                      "/settings\tOpen AIMon mobile Settings tab\n"
                      "/help\tShow available commands\n"
                    : "Cursor Agent Mobile Commands:\n"
                      "/compact [focus]\tCompact & roll over Cursor Agent session context\n"
                      "/summarize [focus]\tAlias for /compact\n"
                      "/clear\tStart a fresh Cursor Agent conversation\n"
                      "/plan <prompt>\tRun prompt in read-only plan mode (--mode plan)\n"
                      "/ask <question>\tRun prompt in read-only Q&A ask mode (--mode ask)\n"
                      "/status\tShow active session, workspace & model\n"
                      "/doctor\tCheck Cursor Agent CLI binary & version\n"
                      "/model\tSwitch Cursor Agent model\n"
                      "/models\tList available Cursor Agent models\n"
                      "/provider\tSwitch AI provider (agy, claude, cursor)\n"
                      "/settings\tOpen AIMon mobile Settings tab\n"
                      "/help\tShow available commands\n";
                res.set_content(nlohmann::json({
                    {"status", "command_response"},
                    {"agentId", targetProvider},
                    {"source", targetProvider},
                    {"conversationId", cid.empty() ? "new" : cid},
                    {"command", cmdVerb},
                    {"response", helpText}
                }).dump(2), "application/json");
                return;
            }

            std::string providerBin = (targetProvider == "claude") ? getClaudeBin() : getCursorBin();
            std::string wsDir = getWorkspaceRoot().string();
            std::string activeModel = !requestedModel.empty()
                ? requestedModel
                : ((targetProvider == "claude") ? "claude-sonnet-4-6" : "cursor-auto");
            std::string existingSlug;
            fs::path existingTranscriptPath;
            if (!cid.empty() && cid != "new") {
                std::string dummySrc;
                existingTranscriptPath = findProviderTranscriptById(cid, dummySrc, existingSlug);
                if (!existingTranscriptPath.empty()) {
                    auto parsed = parseProviderTranscriptJsonl(existingTranscriptPath, targetProvider, 999999, existingSlug, true);
                    if (!parsed.workspacePath.empty() && fs::exists(parsed.workspacePath)) {
                        wsDir = parsed.workspacePath;
                    }
                    if (requestedModel.empty() && !parsed.model.empty()) {
                        activeModel = parsed.model;
                    }
                }
            }

            if (cmdVerb == "/status") {
                std::string providerLabel = (targetProvider == "claude") ? "Claude Code" : "Cursor Agent";
                std::string statusMd = "**" + providerLabel + " Session Status**\n"
                    "- **Provider**: `" + targetProvider + "`\n"
                    "- **Session ID**: `" + (cid.empty() ? "new" : cid) + "`\n"
                    "- **Model**: `" + activeModel + "`\n"
                    "- **Workspace**: `" + wsDir + "`\n"
                    "- **CLI Binary**: `" + (providerBin.empty() ? std::string("not found") : providerBin) + "`";
                res.set_content(nlohmann::json({
                    {"status", "command_response"},
                    {"agentId", targetProvider},
                    {"source", targetProvider},
                    {"conversationId", cid.empty() ? "new" : cid},
                    {"command", cmdVerb},
                    {"response", statusMd}
                }).dump(2), "application/json");
                return;
            }

            if (cmdVerb == "/models") {
                std::string modelsMd = (targetProvider == "claude")
                    ? "**Available Claude Code Models**\n"
                      "- `claude-sonnet-4-6` — Claude Sonnet 4.6 (Default)\n"
                      "- `claude-opus-4-6` — Claude Opus 4.6 (Most capable)\n"
                      "- `claude-haiku-4-5` — Claude Haiku 4.5 (Fast & lightweight)\n"
                      "Active model: `" + activeModel + "`"
                    : "**Available Cursor Agent Models**\n"
                      "- `auto` — Cursor Auto Router (Default)\n"
                      "- `claude-4.6-sonnet` — Claude 4.6 Sonnet\n"
                      "- `claude-4.6-opus` — Claude 4.6 Opus\n"
                      "- `gpt-5.3-codex` — GPT-5.3 Codex\n"
                      "- `gpt-5` — GPT-5\n"
                      "- `gemini-2.5-pro` — Gemini 2.5 Pro\n"
                      "Active model: `" + activeModel + "`";
                res.set_content(nlohmann::json({
                    {"status", "command_response"},
                    {"agentId", targetProvider},
                    {"source", targetProvider},
                    {"conversationId", cid.empty() ? "new" : cid},
                    {"command", cmdVerb},
                    {"response", modelsMd}
                }).dump(2), "application/json");
                return;
            }

            if (cmdVerb == "/doctor") {
                bool binOk = !providerBin.empty() && fs::exists(providerBin) && access(providerBin.c_str(), X_OK) == 0;
                std::string verOut = "Binary not found or not executable";
                if (binOk) {
                    int pfd[2];
                    if (pipe(pfd) == 0) {
                        pid_t ch = fork();
                        if (ch == 0) {
                            close(pfd[0]);
                            dup2(pfd[1], STDOUT_FILENO);
                            dup2(pfd[1], STDERR_FILENO);
                            close(pfd[1]);
                            char* const argv[] = {
                                const_cast<char*>(providerBin.c_str()),
                                const_cast<char*>("--version"),
                                nullptr
                            };
                            execv(providerBin.c_str(), argv);
                            _exit(127);
                        } else if (ch > 0) {
                            close(pfd[1]);
                            std::string vBuf;
                            char buf[512];
                            ssize_t n;
                            while ((n = read(pfd[0], buf, sizeof(buf))) > 0) {
                                vBuf.append(buf, static_cast<size_t>(n));
                            }
                            close(pfd[0]);
                            int st = 0;
                            waitpid(ch, &st, 0);
                            auto f = vBuf.find_first_not_of(" \t\r\n");
                            if (f != std::string::npos) {
                                auto l = vBuf.find_last_not_of(" \t\r\n");
                                verOut = vBuf.substr(f, l - f + 1);
                            } else {
                                verOut = "OK (no version output)";
                            }
                        } else {
                            close(pfd[0]);
                            close(pfd[1]);
                        }
                    }
                }
                std::string docMd = "**" + std::string(targetProvider == "claude" ? "Claude Code" : "Cursor Agent") + " Doctor**\n"
                    "- **Binary**: `" + (providerBin.empty() ? std::string("missing") : providerBin) + "`\n"
                    "- **Executable**: " + std::string(binOk ? "Yes" : "No") + "\n"
                    "- **Version**: `" + verOut + "`\n"
                    "- **Workspace**: `" + wsDir + "`";
                res.set_content(nlohmann::json({
                    {"status", "command_response"},
                    {"agentId", targetProvider},
                    {"source", targetProvider},
                    {"conversationId", cid.empty() ? "new" : cid},
                    {"command", cmdVerb},
                    {"response", docMd}
                }).dump(2), "application/json");
                return;
            }

            std::vector<std::string> extraCliArgs;
            if (cmdVerb == "/plan") {
                if (cmdArgs.empty()) {
                    res.set_content(nlohmann::json({
                        {"status", "command_response"},
                        {"agentId", targetProvider},
                        {"source", targetProvider},
                        {"conversationId", cid.empty() ? "new" : cid},
                        {"command", cmdVerb},
                        {"response", "Usage: `/plan <prompt>` — runs your prompt in read-only planning mode."}
                    }).dump(2), "application/json");
                    return;
                }
                text = cmdArgs;
                if (targetProvider == "claude") {
                    extraCliArgs = {"--permission-mode", "plan"};
                } else {
                    extraCliArgs = {"--mode", "plan"};
                }
            } else if (cmdVerb == "/ask") {
                if (cmdArgs.empty()) {
                    res.set_content(nlohmann::json({
                        {"status", "command_response"},
                        {"agentId", targetProvider},
                        {"source", targetProvider},
                        {"conversationId", cid.empty() ? "new" : cid},
                        {"command", cmdVerb},
                        {"response", "Usage: `/ask <question>` — runs your question in read-only Q&A ask mode."}
                    }).dump(2), "application/json");
                    return;
                }
                text = cmdArgs;
                if (targetProvider == "cursor") {
                    extraCliArgs = {"--mode", "ask"};
                } else {
                    extraCliArgs = {"--permission-mode", "plan"};
                }
            } else if (cmdVerb == "/effort") {
                auto sp2 = cmdArgs.find_first_of(" \t\r\n");
                std::string level = toLowerAscii(sp2 == std::string::npos ? cmdArgs : cmdArgs.substr(0, sp2));
                bool validLevel = (level == "low" || level == "medium" || level == "high" || level == "max");
                if (cmdArgs.empty() || !validLevel) {
                    res.set_content(nlohmann::json({
                        {"status", "command_response"},
                        {"agentId", targetProvider},
                        {"source", targetProvider},
                        {"conversationId", cid.empty() ? "new" : cid},
                        {"command", cmdVerb},
                        {"response", "Usage: `/effort <low|medium|high|max> [prompt]`"}
                    }).dump(2), "application/json");
                    return;
                }
                std::string trailingPrompt;
                if (sp2 != std::string::npos) {
                    auto pStart = cmdArgs.find_first_not_of(" \t\r\n", sp2);
                    if (pStart != std::string::npos) {
                        trailingPrompt = cmdArgs.substr(pStart);
                    }
                }
                if (trailingPrompt.empty()) {
                    res.set_content(nlohmann::json({
                        {"status", "command_response"},
                        {"agentId", targetProvider},
                        {"source", targetProvider},
                        {"conversationId", cid.empty() ? "new" : cid},
                        {"command", cmdVerb},
                        {"response", "Claude Code effort level: `" + level + "`. Pass `/effort " + level + " <prompt>` to run with that effort."}
                    }).dump(2), "application/json");
                    return;
                }
                text = trailingPrompt;
                extraCliArgs = {"--effort", level};
            }

            auto runProviderCli = [&](const std::string& promptArg,
                                      const std::string& resumeCid,
                                      const std::vector<std::string>& extraArgs,
                                      std::string& outRaw,
                                      std::string& outReply,
                                      std::string& outSessionId) -> bool {
                int pipeFd[2];
                if (pipe(pipeFd) != 0) {
                    return false;
                }
                pid_t child = fork();
                if (child == 0) {
                    close(pipeFd[0]);
                    dup2(pipeFd[1], STDOUT_FILENO);
                    dup2(pipeFd[1], STDERR_FILENO);
                    close(pipeFd[1]);
                    if (!wsDir.empty()) {
                        int chRes = chdir(wsDir.c_str());
                        (void)chRes;
                    }
                    std::vector<std::string> argStorage;
                    argStorage.push_back(providerBin);
                    argStorage.push_back("-p");
                    argStorage.push_back(promptArg);
                    argStorage.push_back("--output-format");
                    argStorage.push_back("json");
                    if (!resumeCid.empty() && resumeCid != "new") {
                        argStorage.push_back("--resume");
                        argStorage.push_back(resumeCid);
                    }
                    if (!requestedModel.empty()) {
                        argStorage.push_back("--model");
                        argStorage.push_back(requestedModel);
                    }
                    for (const auto& ea : extraArgs) {
                        argStorage.push_back(ea);
                    }
                    if (autoApprove) {
                        if (targetProvider == "claude") {
                            argStorage.push_back("--dangerously-skip-permissions");
                        } else {
                            argStorage.push_back("-f");
                        }
                    }
                    std::vector<char*> argv;
                    for (auto& s : argStorage) {
                        argv.push_back(const_cast<char*>(s.c_str()));
                    }
                    argv.push_back(nullptr);
                    execv(providerBin.c_str(), argv.data());
                    _exit(127);
                } else if (child > 0) {
                    close(pipeFd[1]);
                    outRaw.clear();
                    char buf[1024];
                    ssize_t n;
                    while ((n = read(pipeFd[0], buf, sizeof(buf))) > 0) {
                        outRaw.append(buf, static_cast<size_t>(n));
                    }
                    close(pipeFd[0]);
                    int st = 0;
                    waitpid(child, &st, 0);

                    outReply = outRaw;
                    outSessionId = (!resumeCid.empty() && resumeCid != "new") ? resumeCid : "";
                    auto pj = nlohmann::json::parse(outRaw, nullptr, false);
                    if (!pj.is_discarded() && pj.is_object()) {
                        if (pj.contains("session_id") && pj["session_id"].is_string()) {
                            outSessionId = pj["session_id"].get<std::string>();
                        } else if (pj.contains("conversation_id") && pj["conversation_id"].is_string()) {
                            outSessionId = pj["conversation_id"].get<std::string>();
                        }
                        if (pj.contains("result") && pj["result"].is_string()) {
                            outReply = pj["result"].get<std::string>();
                        } else if (pj.contains("response") && pj["response"].is_string()) {
                            outReply = pj["response"].get<std::string>();
                        }
                    }
                    return true;
                } else {
                    close(pipeFd[0]);
                    close(pipeFd[1]);
                    return false;
                }
            };

            if (cmdVerb == "/compact" || cmdVerb == "/summarize" || cmdVerb == "/compress") {
                if (cid.empty() || cid == "new" || existingTranscriptPath.empty() || !fs::exists(existingTranscriptPath)) {
                    res.set_content(nlohmann::json({
                        {"status", "command_response"},
                        {"agentId", targetProvider},
                        {"source", targetProvider},
                        {"conversationId", cid.empty() ? "new" : cid},
                        {"command", cmdVerb},
                        {"response", "No active conversation to compact."}
                    }).dump(2), "application/json");
                    return;
                }

                NormalizedProviderTranscript parsedFull = parseProviderTranscriptJsonl(
                    existingTranscriptPath, targetProvider, -1, existingSlug, false);

                if (targetProvider == "claude") {
                    if (parsedFull.steps.size() >= 4) {
                        size_t priorLineCount = 0;
                        {
                            std::ifstream countIn(existingTranscriptPath);
                            std::string dummyLine;
                            while (std::getline(countIn, dummyLine)) {
                                if (!dummyLine.empty()) ++priorLineCount;
                            }
                        }
                        std::string rawOut;
                        std::string replyText;
                        std::string resolvedCid;
                        std::string nativeCmd = cmdArgs.empty() ? "/compact" : ("/compact " + cmdArgs);
                        runProviderCli(nativeCmd, cid, {}, rawOut, replyText, resolvedCid);

                        if (fs::exists(existingTranscriptPath)) {
                            std::vector<std::string> curLines;
                            {
                                std::ifstream fIn(existingTranscriptPath);
                                std::string cl;
                                while (std::getline(fIn, cl)) {
                                    if (!cl.empty()) curLines.push_back(cl);
                                }
                            }
                            std::vector<std::string> filteredLines;
                            for (size_t li = 0; li < curLines.size(); ++li) {
                                const auto& cl = curLines[li];
                                if (li >= priorLineCount &&
                                    (cl.find("\"entrypoint\":\"sdk-cli\"") != std::string::npos ||
                                     cl.find("\"entrypoint\": \"sdk-cli\"") != std::string::npos) &&
                                    cl.find("\"task-notification\"") != std::string::npos &&
                                    cl.find("<status>stopped</status>") != std::string::npos) {
                                    continue;
                                }
                                filteredLines.push_back(cl);
                            }
                            if (filteredLines.size() != curLines.size()) {
                                std::ofstream fOut(existingTranscriptPath, std::ios::trunc);
                                for (const auto& fl : filteredLines) {
                                    fOut << fl << "\n";
                                }
                            }
                        }

                        std::ifstream checkIn(existingTranscriptPath);
                        std::string fileContent((std::istreambuf_iterator<char>(checkIn)),
                                                std::istreambuf_iterator<char>());
                        if ((fileContent.find("\"compact_boundary\"") != std::string::npos ||
                             fileContent.find("\"isCompactSummary\"") != std::string::npos) &&
                            replyText.find("Not compacted") == std::string::npos) {
                            auto compactedParsed = parseProviderTranscriptJsonl(
                                existingTranscriptPath, "claude", -1, existingSlug, false);
                            res.set_content(nlohmann::json({
                                {"status", "compacted"},
                                {"compactedFrom", cid},
                                {"agentId", "claude"},
                                {"source", "claude"},
                                {"conversationId", cid},
                                {"command", cmdVerb},
                                {"steps", compactedParsed.steps},
                                {"response", replyText}
                            }).dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
                            return;
                        }
                    }

                    res.set_content(nlohmann::json({
                        {"status", "command_response"},
                        {"agentId", "claude"},
                        {"source", "claude"},
                        {"conversationId", cid},
                        {"command", cmdVerb},
                        {"response", "Conversation is too short to compact (fewer than 4 steps). Existing history preserved."}
                    }).dump(2), "application/json");
                    return;
                }

                if (targetProvider == "cursor") {
                    std::string compNote = "Cursor Agent sessions are compacted natively in the Cursor prompt via `/summarize` (or `/compress`); `cursor-agent --print` does not expose a headless compaction action. Full session history (" + std::to_string(parsedFull.steps.size()) + " steps) preserved.";
                    fs::path compIndex = existingTranscriptPath.parent_path() / "compaction" / "INDEX.md";
                    if (fs::exists(compIndex)) {
                        compNote = "Native Cursor compaction checkpoint present (`compaction/INDEX.md`). " + compNote;
                    }
                    res.set_content(nlohmann::json({
                        {"status", "command_response"},
                        {"agentId", "cursor"},
                        {"source", "cursor"},
                        {"conversationId", cid},
                        {"command", cmdVerb},
                        {"response", compNote}
                    }).dump(2), "application/json");
                    return;
                }
            }

            LiveProviderSessionInfo sessionInfo;
            if (!cid.empty() && cid != "new") {
                sessionInfo = discoverLiveProviderSession(targetProvider, cid, wsDir, existingTranscriptPath.string());
                const SessionInteractivityInfo interactivity = evaluateSessionInteractivity(
                    targetProvider, cid, fs::path(), sessionInfo, existingTranscriptPath);
                if (!interactivity.interactive) {
                    res.status = 409;
                    res.set_content(nlohmann::json({
                        {"ok", false},
                        {"status", "error"},
                        {"interactive", false},
                        {"nonInteractiveReason", interactivity.nonInteractiveReason},
                        {"error", interactivity.nonInteractiveReason}
                    }).dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
                    return;
                }

                std::string muxReply;
                bool dispatchFailed = false;
                if (routePromptViaMultiplexer(
                        sessionInfo, targetProvider, cid, text,
                        existingTranscriptPath.string(), existingSlug,
                        muxReply, dispatchFailed)) {
                    res.set_content(nlohmann::json({
                        {"status", "ok"},
                        {"agentId", targetProvider},
                        {"source", targetProvider},
                        {"conversationId", cid},
                        {"routedVia", sessionInfo.muxType},
                        {"muxType", sessionInfo.muxType},
                        {"muxSession", sessionInfo.muxSession},
                        {"muxTarget", sessionInfo.muxTarget},
                        {"screenSession", sessionInfo.screenSession},
                        {"autoApprove", autoApprove},
                        {"response", muxReply}
                    }).dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
                    return;
                }

                res.status = dispatchFailed ? 502 : 504;
                std::string muxErr = dispatchFailed
                    ? ("Failed to dispatch prompt to live " + sessionInfo.muxType + " session " + sessionInfo.muxSession)
                    : ("Timed out waiting for response from live " + sessionInfo.muxType + " session " + sessionInfo.muxSession);
                res.set_content(nlohmann::json({
                    {"ok", false},
                    {"status", "error"},
                    {"error", muxErr},
                    {"conversationId", cid}
                }).dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
                return;
            }

            GatewayConfig gatewayCfg = this->getGatewayConfig();
            if (!gatewayCfg.allowCliNewSessionSpawn) {
                res.status = 409;
                res.set_content(nlohmann::json({
                    {"ok", false},
                    {"status", "error"},
                    {"error", "Spawning new headless CLI sessions is disabled"}
                }).dump(2), "application/json");
                return;
            }

            if (providerBin.empty() || !fs::exists(providerBin) || access(providerBin.c_str(), X_OK) != 0) {
                res.status = 503;
                res.set_content(nlohmann::json({
                    {"status", "error"},
                    {"error", (targetProvider == "claude" ? "Claude Code CLI binary (claude)" : "Cursor Agent CLI binary (cursor-agent)") +
                              std::string(" is not available or not executable")}
                }).dump(2), "application/json");
                return;
            }

            std::string selectedRoute = "cli_print";
            std::string rawOut;
            std::string replyText;
            std::string resolvedCid;
            if (!runProviderCli(text, cid, extraCliArgs, rawOut, replyText, resolvedCid)) {
                res.status = 500;
                res.set_content("{\"status\":\"error\",\"error\":\"Failed to fork provider CLI\"}", "application/json");
                return;
            }
            if (resolvedCid.empty()) {
                resolvedCid = (!cid.empty() && cid != "new") ? cid : "new";
            }
            res.set_content(nlohmann::json({
                {"status", "ok"},
                {"agentId", targetProvider},
                {"source", targetProvider},
                {"conversationId", resolvedCid},
                {"routedVia", selectedRoute},
                {"autoApprove", autoApprove},
                {"response", replyText}
            }).dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
            return;
        }

        std::string agyBin = PathUtils::expandHome("~/.local/bin/agy");
        const char* envAgyBin = std::getenv("AIMON_AGY_BIN");
        if (envAgyBin && *envAgyBin) {
            agyBin = envAgyBin;
        }

        if (!trimmedText.empty() && trimmedText[0] == '/') {
            auto spPos = trimmedText.find_first_of(" \t\r\n");
            std::string cmdVerb = toLowerAscii(spPos == std::string::npos ? trimmedText : trimmedText.substr(0, spPos));
            if (cmdVerb == "/clear" || cmdVerb == "/new" || cmdVerb == "/reset") {
                res.set_content(nlohmann::json({
                    {"status", "cleared"},
                    {"agentId", agentId},
                    {"conversationId", "new"},
                    {"command", cmdVerb},
                    {"response", "Conversation cleared."}
                }).dump(2), "application/json");
                return;
            }

            static const std::set<std::string> kStatelessPrintCommands = {
                "/help", "/usage", "/quota", "/credits", "/agents",
                "/skills", "/permissions", "/hooks", "/changelog"
            };
            if (kStatelessPrintCommands.count(cmdVerb) > 0) {
                if (!fs::exists(agyBin) || access(agyBin.c_str(), X_OK) != 0) {
                    res.status = 503;
                    res.set_content(nlohmann::json({
                        {"status", "error"},
                        {"error", "Antigravity CLI binary (agy) is not available or not executable at " + agyBin}
                    }).dump(2), "application/json");
                    return;
                }
                int pipeFd[2];
                if (pipe(pipeFd) == 0) {
                    pid_t child = fork();
                    if (child == 0) {
                        close(pipeFd[0]);
                        dup2(pipeFd[1], STDOUT_FILENO);
                        dup2(pipeFd[1], STDERR_FILENO);
                        close(pipeFd[1]);
                        std::string wsDir = getWorkspaceRoot().string();
                        if (!wsDir.empty()) {
                            int chRes = chdir(wsDir.c_str());
                            (void)chRes;
                        }
                        std::vector<char*> argv;
                        argv.push_back(const_cast<char*>(agyBin.c_str()));
                        argv.push_back(const_cast<char*>("-p"));
                        argv.push_back(const_cast<char*>(trimmedText.c_str()));
                        argv.push_back(const_cast<char*>("--output-format"));
                        argv.push_back(const_cast<char*>("json"));
                        argv.push_back(nullptr);
                        execv(agyBin.c_str(), argv.data());
                        _exit(127);
                    } else if (child > 0) {
                        close(pipeFd[1]);
                        std::string cmdOut;
                        char buf[1024];
                        ssize_t n;
                        while ((n = read(pipeFd[0], buf, sizeof(buf))) > 0) {
                            cmdOut.append(buf, static_cast<size_t>(n));
                        }
                        close(pipeFd[0]);
                        int st = 0;
                        int waitedMs = 0;
                        while (waitedMs < 8000) {
                            if (waitpid(child, &st, WNOHANG) == child) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                            waitedMs += 20;
                        }
                        if (waitedMs >= 8000) {
                            kill(child, SIGKILL);
                            waitpid(child, &st, 0);
                        }
                        std::string responseMarkdown = cmdOut;
                        auto cj = nlohmann::json::parse(cmdOut, nullptr, false);
                        if (!cj.is_discarded() && cj.is_object()) {
                            if (cj.contains("response") && cj["response"].is_string()) {
                                responseMarkdown = cj["response"].get<std::string>();
                            } else if (cj.contains("error") && cj["error"].is_string()) {
                                responseMarkdown = cj["error"].get<std::string>();
                            }
                        }
                        if (cmdVerb == "/help" && responseMarkdown.find("/compact") == std::string::npos) {
                            responseMarkdown += "\n/clear\tStart a fresh conversation\n/compact\tCompact and summarize conversation context\n/settings\tOpen AIMon mobile Settings tab\n";
                        }
                        res.set_content(nlohmann::json({
                            {"status", "command_response"},
                            {"agentId", agentId},
                            {"conversationId", cid.empty() ? "new" : cid},
                            {"command", cmdVerb},
                            {"response", responseMarkdown}
                        }).dump(2), "application/json");
                        return;
                    } else {
                        close(pipeFd[0]);
                        close(pipeFd[1]);
                    }
                }
            }
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

        GatewayConfig gatewayCfg = this->getGatewayConfig();
        bool lockedHasRegisteredCsrf = false;
        if (isLockedByRunningAgy) {
            for (const auto& ep : endpoints) {
                if (!ep.isCli) continue;
                if (lockedHttpPort > 0 && ep.port == lockedHttpPort) {
                    lockedHasRegisteredCsrf = true;
                    break;
                }
                if (lockedHttpPort <= 0 && !cid.empty() && ep.lockedConversationId == cid) {
                    lockedHasRegisteredCsrf = true;
                    break;
                }
            }
        }

        if (isLockedByRunningAgy && !lockedHasRegisteredCsrf &&
            !(gatewayCfg.allowCliNewSessionSpawn && !hasCliDiskConversation && !wsPath.empty())) {
            std::string locDesc = !lockedTerminalPts.empty()
                ? lockedTerminalPts
                : (lockedHttpPort > 0 ? ("port " + std::to_string(lockedHttpPort)) : "active terminal");
            std::string errMsg = "Conversation is open in terminal (" + locDesc +
                                 ") without a registered ConnectRPC CSRF token";
            res.status = 409;
            res.set_content(nlohmann::json({
                {"ok", false},
                {"status", "error"},
                {"interactive", false},
                {"nonInteractiveReason", errMsg},
                {"error", errMsg}
            }).dump(2), "application/json");
            return;
        }

        if (endpoints.empty() &&
            !(isLockedByRunningAgy && gatewayCfg.allowCliNewSessionSpawn && !hasCliDiskConversation && !wsPath.empty())) {
            if (!cid.empty() && cid != "new") {
                std::string errMsg = "No live Antigravity ConnectRPC or agentapi endpoint is running for this conversation";
                res.status = 409;
                res.set_content(nlohmann::json({
                    {"ok", false},
                    {"status", "error"},
                    {"interactive", false},
                    {"nonInteractiveReason", errMsg},
                    {"error", errMsg}
                }).dump(2), "application/json");
                return;
            }
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
                nlohmann::json itemsJson = nlohmann::json::array({{{"text", text}}});
                if (!trimmedText.empty() && trimmedText[0] == '/' && trimmedText.size() > 1) {
                    auto spPos = trimmedText.find_first_of(" \t\r\n");
                    std::string rawCmd = (spPos == std::string::npos)
                        ? trimmedText.substr(1)
                        : trimmedText.substr(1, spPos - 1);
                    std::string trailingText = (spPos == std::string::npos)
                        ? ""
                        : trimmedText.substr(spPos);
                    std::string cmdLower = toLowerAscii(rawCmd);
                    if (cmdLower == "summarize" || cmdLower == "compress") {
                        cmdLower = "compact";
                    }
                    if (!cmdLower.empty() && cmdLower.find('/') == std::string::npos) {
                        nlohmann::json slashInfo = nlohmann::json::object();
                        // 1. Query GetSlashCommands on ep
                        try {
                            nlohmann::json slashReq = {
                                {"cascadeConfig", cascadeCfg},
                                {"cascadeId", targetCid}
                            };
                            if (cascadeCfg.contains("dynamicWorkspaceUris") && cascadeCfg["dynamicWorkspaceUris"].is_array()) {
                                slashReq["workspaceUris"] = cascadeCfg["dynamicWorkspaceUris"];
                            } else if (!wsPath.empty()) {
                                slashReq["workspaceUris"] = nlohmann::json::array({"file://" + wsPath});
                            }
                            auto slashRes = cli.Post(
                                "/exa.language_server_pb.LanguageServerService/GetSlashCommands",
                                hdrs,
                                slashReq.dump(),
                                "application/json"
                            );
                            if (slashRes && slashRes->status == 200) {
                                auto sj = nlohmann::json::parse(slashRes->body, nullptr, false);
                                if (!sj.is_discarded() && sj.is_object() &&
                                    sj.contains("commands") && sj["commands"].is_array()) {
                                    for (const auto& cmdDef : sj["commands"]) {
                                        if (cmdDef.is_object() && cmdDef.contains("info") && cmdDef["info"].is_object()) {
                                            std::string defName = toLowerAscii(cmdDef["info"].value("name", ""));
                                            if (defName == cmdLower) {
                                                slashInfo = cmdDef["info"];
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        } catch (...) {}

                        // 2. Fallback for built-in system slash commands if GetSlashCommands was unavailable or did not list it
                        static const std::set<std::string> kBuiltinSystemSlashCommands = {
                            "compact", "grill-me", "goal", "plan", "boost",
                            "learn", "teamwork", "browser", "schedule"
                        };
                        if (slashInfo.empty() && kBuiltinSystemSlashCommands.count(cmdLower) > 0) {
                            slashInfo = {
                                {"name", cmdLower},
                                {"type", "SLASH_COMMAND_TYPE_SYSTEM"}
                            };
                            if (cmdLower == "grill-me") {
                                slashInfo["modelFacingText"] =
                                    "<GRILL_ME>\nThe user has requested that you interview them about every aspect of their task until you've reach a shared understanding. Walk down each branch of the design tree, resolving dependencies between decisions one-by-one. For each question, provide your recommended answer.\n\nGuidelines:\n- Ask the questions one at a time.\n- If a question can be answered by exploring the codebase, explore the codebase instead.\n- Use the ask_question tool for asking questions to the user.\n</GRILL_ME>";
                            }
                        }

                        if (!slashInfo.empty()) {
                            itemsJson = nlohmann::json::array({
                                {
                                    {"item", {
                                        {"slashCommand", {
                                            {"info", slashInfo}
                                        }}
                                    }}
                                },
                                {
                                    {"text", trailingText}
                                }
                            });
                        }
                    }
                }
                nlohmann::json payload = {
                    {"cascadeId", targetCid},
                    {"items", itemsJson},
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

        auto tryHandleWaitingAskQuestion = [&](const LsEndpoint& ep, const std::string& targetCid, std::string& outBody) -> bool {
            if (targetCid.empty() || targetCid == "new") return false;
            if (!hasQuestionResponses && !trimmedText.empty() && trimmedText[0] == '/') return false;
            try {
                httplib::Client cli("127.0.0.1", ep.port);
                cli.set_connection_timeout(0, 500000);
                cli.set_read_timeout(3, 0);
                httplib::Headers hdrs = {
                    {"Content-Type", "application/json"},
                    {"x-codeium-csrf-token", ep.csrf}
                };
                auto trajRes = cli.Post(
                    "/exa.language_server_pb.LanguageServerService/GetCascadeTrajectory",
                    hdrs,
                    nlohmann::json({{"cascadeId", targetCid}}).dump(),
                    "application/json"
                );
                if (!trajRes || trajRes->status != 200) {
                    return false;
                }
                auto tj = nlohmann::json::parse(trajRes->body, nullptr, false);
                if (tj.is_discarded() || !tj.is_object() || !tj.contains("trajectory") || !tj["trajectory"].is_object()) {
                    return false;
                }
                const auto& steps = tj["trajectory"].value("steps", nlohmann::json::array());
                for (int i = static_cast<int>(steps.size()) - 1; i >= 0; --i) {
                    const auto& rs = steps[static_cast<size_t>(i)];
                    if (!rs.is_object()) continue;
                    std::string stStatus = rs.value("status", "");
                    std::string stType = rs.value("type", "");
                    bool hasAskQ = (stType == "CORTEX_STEP_TYPE_ASK_QUESTION") ||
                                   rs.contains("askQuestion") ||
                                   (rs.contains("requestedInteraction") && rs["requestedInteraction"].is_object() &&
                                    rs["requestedInteraction"].contains("askQuestion"));
                    if (stStatus != "CORTEX_STEP_STATUS_WAITING" || !hasAskQ) {
                        continue;
                    }

                    std::string trajectoryId = tj["trajectory"].value("trajectoryId", targetCid);
                    int stepIndex = i;
                    if (rs.contains("metadata") && rs["metadata"].is_object() &&
                        rs["metadata"].contains("sourceTrajectoryStepInfo") &&
                        rs["metadata"]["sourceTrajectoryStepInfo"].is_object()) {
                        const auto& stInfo = rs["metadata"]["sourceTrajectoryStepInfo"];
                        trajectoryId = stInfo.value("trajectoryId", trajectoryId);
                        stepIndex = stInfo.value("stepIndex", stepIndex);
                    }

                    nlohmann::json rawQuestions = nlohmann::json::array();
                    if (rs.contains("requestedInteraction") && rs["requestedInteraction"].is_object() &&
                        rs["requestedInteraction"].contains("askQuestion") &&
                        rs["requestedInteraction"]["askQuestion"].is_object() &&
                        rs["requestedInteraction"]["askQuestion"].contains("questions")) {
                        rawQuestions = rs["requestedInteraction"]["askQuestion"]["questions"];
                    } else if (rs.contains("askQuestion") && rs["askQuestion"].is_object() &&
                               rs["askQuestion"].contains("questions")) {
                        rawQuestions = rs["askQuestion"]["questions"];
                    }
                    nlohmann::json normQuestions = normalizeQuestionsJson(rawQuestions);
                    if (normQuestions.empty()) {
                        return false;
                    }

                    nlohmann::json responses = nlohmann::json::array();
                    for (size_t qIdx = 0; qIdx < normQuestions.size(); ++qIdx) {
                        const auto& nq = normQuestions[qIdx];
                        std::string qText = nq.value("question", "");
                        bool isMulti = nq.value("isMultiSelect", false);
                        nlohmann::json qOptObjs = nq.value("optionObjects", nlohmann::json::array());
                        nlohmann::json selectedOptionIds = nlohmann::json::array();
                        std::string writeInResponse;

                        auto matchOptionStringToId = [&](const std::string& candidate) -> std::string {
                            auto cf = candidate.find_first_not_of(" \t\r\n");
                            if (cf == std::string::npos) return "";
                            auto cl = candidate.find_last_not_of(" \t\r\n");
                            std::string cTrim = candidate.substr(cf, cl - cf + 1);
                            if (cTrim.empty()) return "";

                            bool allDigits = std::all_of(cTrim.begin(), cTrim.end(), [](unsigned char ch) {
                                return std::isdigit(ch) != 0;
                            });
                            if (allDigits) {
                                try {
                                    int idx1 = std::stoi(cTrim);
                                    if (idx1 >= 1 && static_cast<size_t>(idx1) <= qOptObjs.size()) {
                                        return qOptObjs[static_cast<size_t>(idx1 - 1)].value("id", std::to_string(idx1));
                                    }
                                } catch (...) {}
                            }

                            std::string cLow = toLowerAscii(cTrim);
                            for (const auto& optObj : qOptObjs) {
                                if (!optObj.is_object()) continue;
                                std::string oId = optObj.value("id", "");
                                std::string oTxt = optObj.value("text", "");
                                std::string oTxtLow = toLowerAscii(oTxt);
                                if (toLowerAscii(oId) == cLow || oTxtLow == cLow) {
                                    return oId;
                                }
                            }
                            for (const auto& optObj : qOptObjs) {
                                if (!optObj.is_object()) continue;
                                std::string oId = optObj.value("id", "");
                                std::string oTxtLow = toLowerAscii(optObj.value("text", ""));
                                if (!oTxtLow.empty() && oTxtLow.find(cLow) != std::string::npos) {
                                    return oId;
                                }
                            }
                            return "";
                        };

                        if (hasQuestionResponses && qIdx < j["questionResponses"].size() &&
                            j["questionResponses"][qIdx].is_object()) {
                            const auto& qr = j["questionResponses"][qIdx];
                            if (qr.contains("selectedOptionIds") && qr["selectedOptionIds"].is_array()) {
                                for (const auto& sid : qr["selectedOptionIds"]) {
                                    if (sid.is_string() && !sid.get<std::string>().empty()) {
                                        selectedOptionIds.push_back(sid.get<std::string>());
                                    }
                                }
                            } else if (qr.contains("selectedOptions") && qr["selectedOptions"].is_array()) {
                                for (const auto& so : qr["selectedOptions"]) {
                                    if (!so.is_string()) continue;
                                    std::string sVal = so.get<std::string>();
                                    std::string matchedId = matchOptionStringToId(sVal);
                                    if (!matchedId.empty()) {
                                        selectedOptionIds.push_back(matchedId);
                                    } else if (!sVal.empty()) {
                                        if (!writeInResponse.empty()) writeInResponse += ", ";
                                        writeInResponse += sVal;
                                    }
                                }
                            }
                            if (qr.contains("writeInResponse") && qr["writeInResponse"].is_string()) {
                                std::string wir = qr["writeInResponse"].get<std::string>();
                                if (!wir.empty()) {
                                    writeInResponse = wir;
                                }
                            }
                        } else {
                            std::string matchedId = matchOptionStringToId(trimmedText);
                            if (!matchedId.empty()) {
                                selectedOptionIds.push_back(matchedId);
                            } else {
                                writeInResponse = trimmedText;
                            }
                        }

                        responses.push_back({
                            {"question", qText},
                            {"options", qOptObjs},
                            {"isMultiSelect", isMulti},
                            {"selectedOptionIds", selectedOptionIds},
                            {"writeInResponse", writeInResponse}
                        });
                    }

                    nlohmann::json interactionPayload = {
                        {"cascadeId", targetCid},
                        {"interaction", {
                            {"trajectoryId", trajectoryId},
                            {"stepIndex", stepIndex},
                            {"askQuestion", {
                                {"responses", responses},
                                {"cancelled", false}
                            }}
                        }}
                    };
                    auto intRes = cli.Post(
                        "/exa.language_server_pb.LanguageServerService/HandleCascadeUserInteraction",
                        hdrs,
                        interactionPayload.dump(),
                        "application/json"
                    );
                    if (intRes && intRes->status == 200) {
                        outBody = intRes->body;
                        std::lock_guard<std::mutex> lk(*lastRpcMutex);
                        *lastRpcEndpoint = {ep.port, ep.csrf};
                        return true;
                    }
                    break;
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
                std::string interactionOut;
                if (tryHandleWaitingAskQuestion(ep, cid, interactionOut)) {
                    nlohmann::json resp;
                    resp["status"] = "dispatched";
                    resp["interactionHandled"] = true;
                    resp["agentId"] = agentId;
                    resp["conversationId"] = cid;
                    resp["output"] = interactionOut;
                    resp["response"] = interactionOut;
                    res.set_content(resp.dump(2), "application/json");
                    return;
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
        if (gatewayCfg.allowAgyAgentApiFallback) {
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
                    resp["routedVia"] = "agy_agentapi";
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
        }

        // 5. Fallback for 0-step locked CLI conversations without CSRF token in workspace: start new detached prompt in wsPath
        if (gatewayCfg.allowCliNewSessionSpawn && isLockedByRunningAgy && !hasCliDiskConversation && !wsPath.empty()) {
            std::string cliModelId = mapLabelToCliModelId(desiredModel);
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
                bool spawnOk = true;
                int status = 0;
                for (int waitedMs = 0; waitedMs < 150; waitedMs += 15) {
                    pid_t w = waitpid(child, &status, WNOHANG);
                    if (w == child) {
                        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                        if (code != 0) {
                            spawnOk = false;
                        }
                        child = -1;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
                }
                if (child > 0) {
                    std::thread([child]() {
                        int st = 0;
                        waitpid(child, &st, 0);
                    }).detach();
                }
                if (spawnOk) {
                    nlohmann::json resp;
                    resp["status"] = "dispatched";
                    resp["agentId"] = agentId;
                    resp["conversationId"] = cid;
                    resp["routedVia"] = "agy_cli_new";
                    resp["output"] = "{\"status\":\"started_cli_conversation_in_workspace\"}";
                    res.set_content(resp.dump(2), "application/json");
                    return;
                }
            }
        }

        if (!cid.empty() && cid != "new") {
            res.status = 409;
            res.set_content(nlohmann::json({
                {"ok", false},
                {"status", "error"},
                {"interactive", false},
                {"nonInteractiveReason", lastError},
                {"error", lastError}
            }).dump(2), "application/json");
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
        if (id.empty()) {
            id = req.get_param_value("conversationId");
        }
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

    _server->Get("/api/workspace/tree", [resolveRequestedWorkspaceRoot](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string relSubPath = req.get_param_value("path");
        if (relSubPath.find("..") != std::string::npos) {
            res.status = 400;
            res.set_content("{\"error\":\"Invalid path traversal\"}", "application/json");
            return;
        }

        std::string cid = req.get_param_value("conversationId");
        if (cid.empty()) cid = req.get_param_value("id");
        std::string explicitWs = req.get_param_value("workspace");
        fs::path root = resolveRequestedWorkspaceRoot(cid, explicitWs);

        if (root.empty() || !fs::exists(root) || !fs::is_directory(root)) {
            nlohmann::json resp;
            resp["workspace"] = "";
            resp["workspaceAvailable"] = false;
            resp["reason"] = "Workspace information is not available for the selected session.";
            resp["path"] = "";
            resp["tree"] = nlohmann::json::array();
            res.set_content(resp.dump(2), "application/json");
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

        size_t totalNodes = 0;
        const size_t maxNodes = 1000;

        std::function<nlohmann::json(const fs::path&, int)> buildTree;
        buildTree = [&](const fs::path& dirPath, int depth) -> nlohmann::json {
            if (depth > 6) {
                return nullptr;
            }
            if (depth > 1 && totalNodes >= maxNodes) {
                return nullptr;
            }
            std::error_code ec;
            std::vector<fs::directory_entry> entries;
            for (const auto& entry : fs::directory_iterator(dirPath, ec)) {
                if (ec) break;
                std::string fname = entry.path().filename().string();
                if (isSkippedDir(fname)) continue;
                entries.push_back(entry);
            }
            if (depth > 1 && totalNodes + entries.size() > maxNodes) {
                totalNodes = maxNodes;
                return nullptr;
            }
            std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
                bool aDir = a.is_directory();
                bool bDir = b.is_directory();
                if (aDir != bDir) return aDir > bDir;
                return a.path().filename().string() < b.path().filename().string();
            });

            nlohmann::json nodes = nlohmann::json::array();
            std::vector<fs::path> childPaths;
            for (const auto& entry : entries) {
                if (depth == 1 && nodes.size() >= maxNodes) break;
                std::string fname = entry.path().filename().string();

                std::error_code typeEc;
                bool isDir = entry.is_directory(typeEc);
                uint64_t fsize = 0;
                if (!isDir && entry.is_regular_file(typeEc)) {
                    std::error_code sizeEc;
                    fsize = static_cast<uint64_t>(entry.file_size(sizeEc));
                }

                totalNodes++;
                nlohmann::json node;
                node["name"] = fname;
                node["path"] = fs::relative(entry.path(), root, typeEc).string();
                node["isDir"] = isDir;
                node["size"] = fsize;
                nodes.push_back(std::move(node));
                childPaths.push_back(entry.path());
            }

            if (depth < 6 && totalNodes < maxNodes) {
                for (size_t i = 0; i < nodes.size(); ++i) {
                    if (totalNodes >= maxNodes) break;
                    if (nodes[i].value("isDir", false)) {
                        nlohmann::json sub = buildTree(childPaths[i], depth + 1);
                        if (sub.is_array()) {
                            nodes[i]["children"] = std::move(sub);
                        }
                    }
                }
            }
            return nodes;
        };

        try {
            nlohmann::json tree = buildTree(startDir, 1);
            nlohmann::json resp;
            resp["workspace"] = root.string();
            resp["workspaceAvailable"] = true;
            resp["reason"] = "";
            resp["path"] = relSubPath;
            resp["tree"] = tree;
            res.set_content(resp.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(nlohmann::json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    _server->Get("/api/workspace/file", [resolveRequestedWorkspaceRoot](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string relPath = req.get_param_value("path");
        if (relPath.empty() || relPath.find("..") != std::string::npos) {
            res.status = 400;
            res.set_content("{\"error\":\"Invalid path\"}", "application/json");
            return;
        }

        std::string cid = req.get_param_value("conversationId");
        if (cid.empty()) cid = req.get_param_value("id");
        std::string explicitWs = req.get_param_value("workspace");
        fs::path root = resolveRequestedWorkspaceRoot(cid, explicitWs);
        if (root.empty() || !fs::exists(root) || !fs::is_directory(root)) {
            res.status = 404;
            res.set_content("{\"error\":\"Workspace information is not available for the selected session\"}", "application/json");
            return;
        }

        fs::path target = root / relPath;
        if (!fs::exists(target) || fs::is_directory(target)) {
            res.status = 404;
            res.set_content("{\"error\":\"File not found\"}", "application/json");
            return;
        }

        std::error_code szEc;
        auto fsize = fs::file_size(target, szEc);
        if (!szEc && fsize > 1024 * 1024) {
            res.status = 413;
            res.set_content("{\"error\":\"File exceeds 1 MB preview limit\"}", "application/json");
            return;
        }

        std::ifstream f(target, std::ios::binary);
        if (!f.is_open()) {
            res.status = 404;
            res.set_content("{\"error\":\"File not found\"}", "application/json");
            return;
        }
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (content.find('\0') != std::string::npos) {
            res.status = 415;
            res.set_content("{\"error\":\"Binary file cannot be displayed as text\"}", "application/json");
            return;
        }

        nlohmann::json resp;
        resp["path"] = relPath;
        resp["content"] = content;
        resp["language"] = target.extension().string();
        res.set_content(resp.dump(2, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    });

    _server->Post("/api/workspace/file", [resolveRequestedWorkspaceRoot](const httplib::Request& req, httplib::Response& res) {
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

            std::string cid = j.value("conversationId", j.value("id", req.get_param_value("conversationId")));
            if (cid.empty()) cid = req.get_param_value("id");
            std::string explicitWs = j.value("workspace", req.get_param_value("workspace"));
            fs::path root = resolveRequestedWorkspaceRoot(cid, explicitWs);
            if (root.empty() || !fs::exists(root) || !fs::is_directory(root)) {
                res.status = 404;
                res.set_content("{\"error\":\"Workspace information is not available for the selected session\"}", "application/json");
                return;
            }

            fs::path target = root / relPath;
            if (target.has_parent_path()) {
                std::error_code dirEc;
                fs::create_directories(target.parent_path(), dirEc);
            }
            std::ofstream f(target, std::ios::trunc | std::ios::binary);
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
        std::string profile;
        if (req.has_param("profile")) {
            profile = req.get_param_value("profile");
        } else if (req.has_header("X-Aimon-Profile")) {
            profile = req.get_header_value("X-Aimon-Profile");
        }

        if (!_mcpServer && profile != "mobile") {
            res.status = 503;
            res.set_content("MCP service not configured", "text/plain");
            return;
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

    // --- Unified Runtime Configuration & Diagnostics Management Endpoints ---

    auto countStalePresenceLocks = [getCliPresenceDir]() -> int {
        std::string presDir = getCliPresenceDir();
        if (presDir.empty()) return 0;
        std::error_code ec;
        if (!fs::exists(presDir, ec) || !fs::is_directory(presDir, ec)) return 0;
        std::set<std::string> liveLockedIds;
        for (const auto& proc : discoverRunningAgyProcesses()) {
            if (!proc.lockedConversationId.empty()) {
                liveLockedIds.insert(proc.lockedConversationId);
            }
        }
        int staleCount = 0;
        for (const auto& entry : fs::directory_iterator(presDir, ec)) {
            if (ec || !entry.is_regular_file()) continue;
            if (entry.path().extension() != ".lock") continue;
            std::string cid = entry.path().stem().string();
            if (liveLockedIds.find(cid) == liveLockedIds.end()) {
                ++staleCount;
            }
        }
        return staleCount;
    };

    _server->Get("/api/config", [this, countStalePresenceLocks](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        AimonConfig snap;
        if (this->_configManager) {
            snap = this->_configManager->getConfigSnapshot();
        } else {
            snap.gateway = this->getGatewayConfig();
        }
        nlohmann::json cfgJson = snap.toJson();
        if (cfgJson.contains("eda") && cfgJson["eda"].is_object()) {
            cfgJson["eda"] = snap.eda.toJson(true);
        }
        if (cfgJson.contains("mqtt") && cfgJson["mqtt"].is_object() &&
            !snap.mqtt.password.empty()) {
            cfgJson["mqtt"]["password"] = "***";
        }
        nlohmann::json out = cfgJson;
        out["ok"] = true;
        out["config"] = cfgJson;
        out["storage_stats"] = AgentTelemetryDb::getInstance().getStorageStats();
        out["stale_presence_locks_count"] = countStalePresenceLocks();
        res.set_content(out.dump(2), "application/json");
    });

    _server->Post("/api/config", [this, countStalePresenceLocks](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            res.status = 400;
            res.set_content(nlohmann::json({{"ok", false}, {"error", "Malformed JSON body"}}).dump(2),
                            "application/json");
            return;
        }
        const nlohmann::json& patch = (body.contains("config") && body["config"].is_object())
            ? body["config"]
            : body;

        AimonConfig snap;
        std::string err;
        if (this->_configManager) {
            if (!this->_configManager->updateFromJson(patch, err)) {
                res.status = 400;
                res.set_content(nlohmann::json({{"ok", false}, {"error", err}}).dump(2), "application/json");
                return;
            }
            snap = this->_configManager->getConfigSnapshot();
        } else {
            snap.gateway = this->getGatewayConfig();
            snap.fromJson(patch);
            if (!snap.validate(err)) {
                res.status = 400;
                res.set_content(nlohmann::json({{"ok", false}, {"error", err}}).dump(2), "application/json");
                return;
            }
        }

        this->setGatewayConfig(snap.gateway);

        nlohmann::json cfgJson = snap.toJson();
        if (cfgJson.contains("eda") && cfgJson["eda"].is_object()) {
            cfgJson["eda"] = snap.eda.toJson(true);
        }
        nlohmann::json out = cfgJson;
        out["ok"] = true;
        out["config"] = cfgJson;
        out["storage_stats"] = AgentTelemetryDb::getInstance().getStorageStats();
        out["stale_presence_locks_count"] = countStalePresenceLocks();
        res.set_content(out.dump(2), "application/json");
    });

    _server->Post("/api/telemetry/prune", [this, getCliPresenceDir](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!isValidMobileOrUiRequest(req)) {
            res.status = 401;
            res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
            return;
        }
        nlohmann::json body = nlohmann::json::object();
        if (!req.body.empty()) {
            auto parsed = nlohmann::json::parse(req.body, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()) {
                body = std::move(parsed);
            }
        }
        auto& db = AgentTelemetryDb::getInstance();
        int64_t sizeBefore = db.getDatabaseSizeBytes();

        bool pruneTelemetry = body.value("prune_telemetry", true);
        bool pruneStaleLocks = body.value("prune_stale_locks", false);
        bool vacuum = body.value("vacuum", true);
        int retentionDays = body.value("retention_days", 14);

        size_t prunedTelemetryRows = 0;
        int prunedStaleLocks = 0;

        if (pruneTelemetry) {
            prunedTelemetryRows = db.rollupAndPrune(retentionDays > 0 ? retentionDays : 14);
        } else if (vacuum) {
            db.rollupAndPrune(36500);
        }

        if (pruneStaleLocks) {
            std::string presDir = getCliPresenceDir();
            std::error_code ec;
            if (!presDir.empty() && fs::exists(presDir, ec) && fs::is_directory(presDir, ec)) {
                std::set<std::string> liveLockedIds;
                for (const auto& proc : discoverRunningAgyProcesses()) {
                    if (!proc.lockedConversationId.empty()) {
                        liveLockedIds.insert(proc.lockedConversationId);
                    }
                }
                for (const auto& entry : fs::directory_iterator(presDir, ec)) {
                    if (ec || !entry.is_regular_file()) continue;
                    if (entry.path().extension() != ".lock") continue;
                    std::string lockId = entry.path().stem().string();
                    if (liveLockedIds.find(lockId) == liveLockedIds.end()) {
                        std::error_code rmEc;
                        if (fs::remove(entry.path(), rmEc) && !rmEc) {
                            ++prunedStaleLocks;
                        }
                    }
                }
            }
        }

        int64_t sizeAfter = db.getDatabaseSizeBytes();
        int64_t reclaimed = (sizeBefore > sizeAfter) ? (sizeBefore - sizeAfter) : 0;
        nlohmann::json out = {
            {"ok", true},
            {"pruned_telemetry_rows", prunedTelemetryRows},
            {"pruned_stale_locks", prunedStaleLocks},
            {"before_bytes", sizeBefore},
            {"after_bytes", sizeAfter},
            {"reclaimed_bytes", reclaimed},
            {"size_before_bytes", sizeBefore},
            {"size_after_bytes", sizeAfter},
            {"storage_stats", db.getStorageStats()}
        };
        res.set_content(out.dump(2), "application/json");
    });
}

bool WebServer::start(bool async) {
    if (_running) return true;

    setupRoutes();
    _running = true;

    MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
    gw.setBroadcastCallback([this](const std::string&, const nlohmann::json& payload) {
        this->broadcastSseNotification(payload.dump());
    });

    // Seed pre-existing transcript steps so historical turns do not fire alerts on startup
    scanTranscriptsForNotifications(true);

    _notificationThread = std::make_unique<std::thread>([this]() {
        while (_running) {
            std::unique_lock<std::mutex> lk(_notificationStopMutex);
            _notificationStopCv.wait_for(lk, std::chrono::milliseconds(500), [this]() {
                return !_running;
            });
            if (!_running) break;
            lk.unlock();
            try {
                scanTranscriptsForNotifications(false);
            } catch (...) {}
        }
    });

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

void WebServer::scanTranscriptsForNotifications(bool seedOnly) {
    std::lock_guard<std::mutex> lock(_watchedTranscriptsMutex);

    const char* envIde = std::getenv("AIMON_IDE_BRAIN_DIR");
    std::string ideBrain = (envIde && *envIde) ? std::string(envIde) : PathUtils::expandHome("~/.gemini/antigravity-ide/brain");
    const char* envCli = std::getenv("AIMON_CLI_BRAIN_DIR");
    std::string cliBrain = (envCli && *envCli) ? std::string(envCli) : PathUtils::expandHome("~/.gemini/antigravity-cli/brain");

    int settleMs = 1200;
    const char* envSettle = std::getenv("AIMON_NOTIFICATION_SETTLE_MS");
    if (envSettle && *envSettle) {
        try {
            settleMs = std::max(0, std::stoi(envSettle));
        } catch (...) {}
    }

    auto stripUserTags = [](const std::string& raw) -> std::string {
        std::string text = raw;
        auto startTag = text.find("<USER_REQUEST>");
        auto endTag = text.find("</USER_REQUEST>");
        if (startTag != std::string::npos && endTag != std::string::npos && endTag >= startTag + 14) {
            text = text.substr(startTag + 14, endTag - (startTag + 14));
        }
        auto first = text.find_first_not_of(" \n\r\t");
        if (first == std::string::npos) return "";
        auto last = text.find_last_not_of(" \n\r\t");
        return text.substr(first, last - first + 1);
    };

    auto trimText = [](const std::string& s) -> std::string {
        auto f = s.find_first_not_of(" \n\r\t");
        if (f == std::string::npos) return "";
        auto l = s.find_last_not_of(" \n\r\t");
        return s.substr(f, l - f + 1);
    };

    MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();

    std::set<std::string> knownSubagentIds;
    for (const std::string& rPath : {cliBrain, ideBrain}) {
        if (rPath.empty()) continue;
        fs::path metaCachePath = fs::path(rPath).parent_path() / "cache" / "conversation_metadata.json";
        std::error_code mEc;
        if (fs::exists(metaCachePath, mEc) && !mEc) {
            try {
                std::ifstream mf(metaCachePath);
                auto mj = nlohmann::json::parse(mf, nullptr, false);
                if (!mj.is_discarded() && mj.is_object() &&
                    mj.contains("conversations") && mj["conversations"].is_object()) {
                    for (auto it = mj["conversations"].begin(); it != mj["conversations"].end(); ++it) {
                        if (it.value().is_object() &&
                            (it.value().value("is_internal", false) ||
                             !it.value().value("parent_conversation_id", "").empty())) {
                            knownSubagentIds.insert(it.key());
                        }
                    }
                }
            } catch (...) {}
        }
    }

    std::vector<std::pair<std::string, std::string>> roots = {
        {"cli", cliBrain},
        {"ide", ideBrain}
    };

    for (const auto& rootPair : roots) {
        const std::string& source = rootPair.first;
        const std::string& rootPath = rootPair.second;
        if (rootPath.empty()) continue;

        std::error_code dirEc;
        if (!fs::exists(rootPath, dirEc) || !fs::is_directory(rootPath, dirEc)) {
            continue;
        }

        for (const auto& entry : fs::directory_iterator(rootPath, dirEc)) {
            if (dirEc || !entry.is_directory()) continue;
            std::string cid = entry.path().filename().string();
            if (cid.empty() || cid[0] == '.') continue;

            fs::path tPath = entry.path() / ".system_generated" / "logs" / "transcript.jsonl";
            std::error_code fEc;
            if (!fs::exists(tPath, fEc) || fEc) continue;

            auto mtime = fs::last_write_time(tPath, fEc);
            if (fEc) continue;
            auto fsize = fs::file_size(tPath, fEc);
            if (fEc) continue;

            auto ageSec = std::chrono::duration_cast<std::chrono::seconds>(
                fs::file_time_type::clock::now() - mtime).count();
            bool alreadyTracked = (_watchedTranscripts.find(cid) != _watchedTranscripts.end());
            if (ageSec > 900 && !alreadyTracked) {
                continue;
            }

            int64_t mtimeTicks = mtime.time_since_epoch().count();
            WatchedTranscriptState& st = _watchedTranscripts[cid];
            if (st.isSubagent) {
                continue;
            }
            if (knownSubagentIds.count(cid) > 0) {
                st.isSubagent = true;
                continue;
            }
            fs::path perConvMetaPath = entry.path() / "cache" / "conversation_metadata.json";
            if (fs::exists(perConvMetaPath, fEc) && !fEc) {
                try {
                    std::ifstream pmf(perConvMetaPath);
                    auto pmj = nlohmann::json::parse(pmf, nullptr, false);
                    if (!pmj.is_discarded() && pmj.is_object() &&
                        (pmj.value("is_internal", false) ||
                         !pmj.value("parent_conversation_id", "").empty())) {
                        st.isSubagent = true;
                        continue;
                    }
                } catch (...) {}
            }

            if (!seedOnly && st.seeded &&
                mtimeTicks == st.lastMtimeTicks &&
                fsize == st.lastFileSize &&
                st.pendingStepIndex < 0) {
                continue;
            }

            st.lastMtimeTicks = mtimeTicks;
            st.lastFileSize = fsize;
            st.seeded = true;

            if (st.cachedTitle.empty()) {
                fs::path taskMdPath = entry.path() / "task.md";
                if (fs::exists(taskMdPath, fEc)) {
                    std::ifstream tf(taskMdPath);
                    std::string tLine;
                    while (std::getline(tf, tLine)) {
                        std::string trimmed = trimText(tLine);
                        if (!trimmed.empty() && trimmed[0] == '#') {
                            size_t hPos = trimmed.find_first_not_of("# \t");
                            if (hPos != std::string::npos) {
                                st.cachedTitle = trimmed.substr(hPos, 80);
                                break;
                            }
                        }
                    }
                }
            }

            std::ifstream in(tPath);
            if (!in.is_open()) continue;

            int maxStepIdx = -1;
            nlohmann::json lastStepObj;
            std::string prevGenericContent;
            std::string line;
            bool seenFirstValidStep = false;
            bool seenUserInput = false;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                auto j = nlohmann::json::parse(line, nullptr, false);
                if (j.is_discarded() || !j.is_object()) continue;

                int sIdx = j.value("step_index", j.value("index", -1));
                if (sIdx > maxStepIdx) {
                    maxStepIdx = sIdx;
                }
                std::string sType = j.value("type", "");
                if (!seenFirstValidStep) {
                    seenFirstValidStep = true;
                    if (sType == "SYSTEM_MESSAGE" || j.value("source", "") == "SYSTEM") {
                        st.isSubagent = true;
                        break;
                    }
                }
                if (sType == "USER_INPUT") {
                    seenUserInput = true;
                }
                if (st.cachedTitle.empty() && sType == "USER_INPUT") {
                    std::string uText = stripUserTags(j.value("content", ""));
                    if (!uText.empty()) {
                        auto nlPos = uText.find('\n');
                        if (nlPos != std::string::npos) {
                            uText = uText.substr(0, nlPos);
                        }
                        st.cachedTitle = uText.substr(0, std::min<size_t>(uText.size(), 80));
                    }
                }
                if (sType == "GENERIC" || sType == "SYSTEM_MESSAGE") {
                    prevGenericContent = j.value("content", "");
                } else if (sType == "USER_INPUT") {
                    prevGenericContent.clear();
                }
                lastStepObj = std::move(j);
            }

            if (st.isSubagent || !seenUserInput) {
                st.pendingStepIndex = -1;
                continue;
            }

            if (seedOnly) {
                st.lastNotifiedStepIndex = maxStepIdx;
                st.pendingStepIndex = -1;
                continue;
            }

            if (lastStepObj.is_null() || !lastStepObj.is_object()) {
                continue;
            }

            int lastStepIdx = lastStepObj.value("step_index", lastStepObj.value("index", -1));
            if (lastStepIdx < 0 || lastStepIdx <= st.lastNotifiedStepIndex) {
                st.pendingStepIndex = -1;
                continue;
            }

            std::string lastType = lastStepObj.value("type", "");
            std::string lastStatus = lastStepObj.value("status", "DONE");
            if (lastStatus != "DONE") {
                st.pendingStepIndex = -1;
                continue;
            }

            // 1. Check for agent_question (ask_question tool call or ArtifactMetadata.RequestFeedback == true)
            bool isAgentQuestion = false;
            std::string questionSnippet;
            if (lastStepObj.contains("tool_calls") && lastStepObj["tool_calls"].is_array()) {
                for (const auto& tc : lastStepObj["tool_calls"]) {
                    if (!tc.is_object()) continue;
                    std::string tcName = tc.value("name", "");
                    if (tcName.empty() && tc.contains("function") && tc["function"].is_object()) {
                        tcName = tc["function"].value("name", "");
                    }
                    nlohmann::json argsObj = nlohmann::json::object();
                    if (tc.contains("args")) {
                        if (tc["args"].is_object()) argsObj = tc["args"];
                        else if (tc["args"].is_string()) {
                            auto p = nlohmann::json::parse(tc["args"].get<std::string>(), nullptr, false);
                            if (!p.is_discarded() && p.is_object()) argsObj = p;
                        }
                    } else if (tc.contains("arguments")) {
                        if (tc["arguments"].is_object()) argsObj = tc["arguments"];
                        else if (tc["arguments"].is_string()) {
                            auto p = nlohmann::json::parse(tc["arguments"].get<std::string>(), nullptr, false);
                            if (!p.is_discarded() && p.is_object()) argsObj = p;
                        }
                    } else if (tc.contains("function") && tc["function"].is_object() &&
                               tc["function"].contains("arguments")) {
                        const auto& fa = tc["function"]["arguments"];
                        if (fa.is_object()) argsObj = fa;
                        else if (fa.is_string()) {
                            auto p = nlohmann::json::parse(fa.get<std::string>(), nullptr, false);
                            if (!p.is_discarded() && p.is_object()) argsObj = p;
                        }
                    }

                    if (tcName == "ask_question" || tcName == "AskUserQuestion") {
                        isAgentQuestion = true;
                        if (argsObj.contains("questions")) {
                            nlohmann::json normQ = normalizeQuestionsJson(argsObj["questions"]);
                            if (normQ.is_array() && !normQ.empty() && normQ[0].is_object()) {
                                questionSnippet = trimText(normQ[0].value("question", ""));
                            }
                        }
                        if (questionSnippet.empty() && argsObj.contains("question") && argsObj["question"].is_string()) {
                            questionSnippet = trimText(argsObj["question"].get<std::string>());
                        }
                        break;
                    }
                    if ((tcName == "write_file" || tcName == "edit_file") &&
                        argsObj.contains("ArtifactMetadata") && argsObj["ArtifactMetadata"].is_object() &&
                        argsObj["ArtifactMetadata"].value("RequestFeedback", false)) {
                        isAgentQuestion = true;
                        questionSnippet = trimText(argsObj["ArtifactMetadata"].value("Summary", "Plan is ready for your review."));
                        break;
                    }
                }
            }

            if (isAgentQuestion) {
                AgentSessionEvent ev;
                ev.event = "agent_question";
                ev.conversationId = cid;
                ev.source = source;
                ev.title = st.cachedTitle.empty() ? ("Session " + cid.substr(0, std::min<size_t>(cid.size(), 8))) : st.cachedTitle;
                ev.workspace = st.cachedWorkspace;
                ev.snippet = questionSnippet.empty()
                    ? "Agent is waiting for your input."
                    : questionSnippet.substr(0, std::min<size_t>(questionSnippet.size(), 180));
                ev.stepIndex = lastStepIdx;
                ev.timestamp = static_cast<int64_t>(time(nullptr));

                st.lastNotifiedStepIndex = lastStepIdx;
                st.pendingStepIndex = -1;
                gw.broadcastAgentSessionEvent(ev);
                continue;
            }

            // 2. Check for prompt_answered (terminal PLANNER_RESPONSE with empty tool_calls and non-empty content, or CHECKPOINT)
            bool hasToolCalls = lastStepObj.contains("tool_calls") &&
                                lastStepObj["tool_calls"].is_array() &&
                                !lastStepObj["tool_calls"].empty();
            if ((lastType == "PLANNER_RESPONSE" && !hasToolCalls) || lastType == "CHECKPOINT") {
                std::string replyContent = (lastType == "CHECKPOINT")
                    ? std::string("Context compacted.")
                    : trimText(lastStepObj.value("content", ""));
                bool waitingForBgTask =
                    (prevGenericContent.find("Tool is running as a background task with task id:") != std::string::npos) ||
                    (prevGenericContent.find("Created the following subagent:") != std::string::npos);
                if (!replyContent.empty() && !waitingForBgTask) {
                    if (st.pendingStepIndex != lastStepIdx) {
                        st.pendingStepIndex = lastStepIdx;
                        st.pendingEvent = "prompt_answered";
                        st.pendingSnippet = replyContent.substr(0, std::min<size_t>(replyContent.size(), 180));
                        st.pendingSince = std::chrono::steady_clock::now();
                    }
                    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - st.pendingSince).count();
                    if (elapsedMs >= settleMs) {
                        AgentSessionEvent ev;
                        ev.event = "prompt_answered";
                        ev.conversationId = cid;
                        ev.source = source;
                        ev.title = st.cachedTitle.empty() ? ("Session " + cid.substr(0, std::min<size_t>(cid.size(), 8))) : st.cachedTitle;
                        ev.workspace = st.cachedWorkspace;
                        ev.snippet = st.pendingSnippet;
                        ev.stepIndex = lastStepIdx;
                        ev.timestamp = static_cast<int64_t>(time(nullptr));

                        st.lastNotifiedStepIndex = lastStepIdx;
                        st.pendingStepIndex = -1;
                        gw.broadcastAgentSessionEvent(ev);
                    }
                    continue;
                }
            }

            st.pendingStepIndex = -1;
        }
    }

    // Also scan Claude Code and Cursor transcripts for agent_question and prompt_answered events
    struct ProviderWatchItem {
        std::string cid;
        std::string source;
        std::string projectSlug;
        fs::path transcriptPath;
    };
    std::vector<ProviderWatchItem> providerItems;

    std::string claudeRoot = getClaudeProjectsDir();
    if (!claudeRoot.empty()) {
        std::error_code ec;
        if (fs::exists(claudeRoot, ec) && fs::is_directory(claudeRoot, ec)) {
            for (const auto& projEntry : fs::directory_iterator(claudeRoot, ec)) {
                if (ec || !projEntry.is_directory()) continue;
                std::string slug = projEntry.path().filename().string();
                if (slug.empty() || slug[0] == '.') continue;
                std::error_code subEc;
                for (const auto& fEntry : fs::directory_iterator(projEntry.path(), subEc)) {
                    if (subEc || !fEntry.is_regular_file()) continue;
                    if (fEntry.path().extension() != ".jsonl") continue;
                    std::string cid = fEntry.path().stem().string();
                    if (cid.empty() || cid[0] == '.') continue;
                    providerItems.push_back({cid, "claude", slug, fEntry.path()});
                }
            }
        }
    }

    std::string cursorRoot = getCursorProjectsDir();
    if (!cursorRoot.empty()) {
        std::set<std::string> cursorSubagentIds;
        std::map<std::string, uint64_t> dummyParentTs;
        collectCursorSubagentIdsAndParentMtimes(cursorRoot, cursorSubagentIds, dummyParentTs);

        std::error_code ec;
        if (fs::exists(cursorRoot, ec) && fs::is_directory(cursorRoot, ec)) {
            for (const auto& projEntry : fs::directory_iterator(cursorRoot, ec)) {
                if (ec || !projEntry.is_directory()) continue;
                std::string slug = projEntry.path().filename().string();
                if (slug.empty() || slug[0] == '.') continue;
                fs::path transDir = projEntry.path() / "agent-transcripts";
                std::error_code subEc;
                if (!fs::exists(transDir, subEc) || !fs::is_directory(transDir, subEc)) continue;
                for (const auto& convEntry : fs::directory_iterator(transDir, subEc)) {
                    if (subEc) continue;
                    if (convEntry.is_directory()) {
                        std::string cid = convEntry.path().filename().string();
                        if (cid.empty() || cid[0] == '.' || cid == "subagents") continue;
                        if (cursorSubagentIds.count(cid)) continue;
                        fs::path jPath = convEntry.path() / (cid + ".jsonl");
                        std::error_code fEc;
                        if (fs::exists(jPath, fEc) && !fEc) {
                            providerItems.push_back({cid, "cursor", slug, jPath});
                        }
                    } else if (convEntry.is_regular_file() && convEntry.path().extension() == ".jsonl") {
                        std::string cid = convEntry.path().stem().string();
                        if (!cid.empty() && cid[0] != '.' && !cursorSubagentIds.count(cid)) {
                            providerItems.push_back({cid, "cursor", slug, convEntry.path()});
                        }
                    }
                }
            }
        }
    }

    for (const auto& item : providerItems) {
        std::error_code fEc;
        auto mtime = fs::last_write_time(item.transcriptPath, fEc);
        if (fEc) continue;
        auto fsize = fs::file_size(item.transcriptPath, fEc);
        if (fEc) continue;

        auto ageSec = std::chrono::duration_cast<std::chrono::seconds>(
            fs::file_time_type::clock::now() - mtime).count();
        bool alreadyTracked = (_watchedTranscripts.find(item.cid) != _watchedTranscripts.end());
        if (ageSec > 900 && !alreadyTracked) {
            continue;
        }

        int64_t mtimeTicks = mtime.time_since_epoch().count();
        WatchedTranscriptState& st = _watchedTranscripts[item.cid];

        if (!seedOnly && st.seeded &&
            mtimeTicks == st.lastMtimeTicks &&
            fsize == st.lastFileSize &&
            st.pendingStepIndex < 0) {
            continue;
        }

        st.lastMtimeTicks = mtimeTicks;
        st.lastFileSize = fsize;
        st.seeded = true;

        auto parsed = parseProviderTranscriptJsonl(item.transcriptPath, item.source, -1, item.projectSlug, seedOnly);
        if (!parsed.hasValidTurns || parsed.firstPrompt.empty()) {
            st.pendingStepIndex = -1;
            continue;
        }
        if (st.cachedTitle.empty() && !parsed.firstPrompt.empty()) {
            st.cachedTitle = parsed.firstPrompt;
            auto nlPos = st.cachedTitle.find_first_of("\r\n");
            if (nlPos != std::string::npos) {
                st.cachedTitle = st.cachedTitle.substr(0, nlPos);
            }
            if (st.cachedTitle.size() > 80) {
                st.cachedTitle = st.cachedTitle.substr(0, 77) + "...";
            }
        }
        if (st.cachedWorkspace.empty() && !parsed.workspaceShort.empty()) {
            st.cachedWorkspace = parsed.workspaceShort;
        }

        int maxStepIdx = parsed.totalSteps - 1;
        if (seedOnly) {
            st.lastNotifiedStepIndex = maxStepIdx;
            st.pendingStepIndex = -1;
            continue;
        }

        if (parsed.steps.empty() || !parsed.steps.back().is_object()) {
            continue;
        }

        const auto& lastStepObj = parsed.steps.back();
        int lastStepIdx = lastStepObj.value("stepIndex", maxStepIdx);
        if (lastStepIdx < 0 || lastStepIdx <= st.lastNotifiedStepIndex) {
            st.pendingStepIndex = -1;
            continue;
        }

        std::string lastType = lastStepObj.value("type", "");
        bool isAgentQuestion = false;
        std::string questionSnippet;

        if (lastStepObj.contains("tool_calls") && lastStepObj["tool_calls"].is_array()) {
            for (const auto& tc : lastStepObj["tool_calls"]) {
                if (!tc.is_object()) continue;
                std::string tcName = tc.value("name", "");
                if (tcName == "ask_question" || tcName == "AskUserQuestion") {
                    isAgentQuestion = true;
                    if (tc.contains("arguments") && tc["arguments"].is_object()) {
                        const auto& argsObj = tc["arguments"];
                        if (argsObj.contains("questions") && argsObj["questions"].is_array() &&
                            !argsObj["questions"].empty() && argsObj["questions"][0].is_object()) {
                            questionSnippet = trimText(argsObj["questions"][0].value("question", ""));
                        } else if (argsObj.contains("question") && argsObj["question"].is_string()) {
                            questionSnippet = trimText(argsObj["question"].get<std::string>());
                        }
                    }
                    break;
                }
            }
        }

        if (isAgentQuestion) {
            AgentSessionEvent ev;
            ev.event = "agent_question";
            ev.conversationId = item.cid;
            ev.source = item.source;
            ev.title = st.cachedTitle.empty()
                ? ("Session " + item.cid.substr(0, std::min<size_t>(item.cid.size(), 8)))
                : st.cachedTitle;
            ev.workspace = st.cachedWorkspace;
            ev.snippet = questionSnippet.empty()
                ? "Agent is waiting for your input."
                : questionSnippet.substr(0, std::min<size_t>(questionSnippet.size(), 180));
            ev.stepIndex = lastStepIdx;
            ev.timestamp = static_cast<int64_t>(time(nullptr));

            st.lastNotifiedStepIndex = lastStepIdx;
            st.pendingStepIndex = -1;
            gw.broadcastAgentSessionEvent(ev);
            continue;
        }

        bool hasToolCalls = lastStepObj.contains("tool_calls") &&
                            lastStepObj["tool_calls"].is_array() &&
                            !lastStepObj["tool_calls"].empty();
        bool isSubagentStep = lastStepObj.value("isSubagentStep", false);
        if (lastType == "PLANNER_RESPONSE" && !hasToolCalls && !isSubagentStep) {
            std::string replyContent = trimText(lastStepObj.value("text", ""));
            if (!replyContent.empty()) {
                if (st.pendingStepIndex != lastStepIdx) {
                    st.pendingStepIndex = lastStepIdx;
                    st.pendingEvent = "prompt_answered";
                    st.pendingSnippet = replyContent.substr(0, std::min<size_t>(replyContent.size(), 180));
                    st.pendingSince = std::chrono::steady_clock::now();
                }
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - st.pendingSince).count();
                if (elapsedMs >= settleMs) {
                    AgentSessionEvent ev;
                    ev.event = "prompt_answered";
                    ev.conversationId = item.cid;
                    ev.source = item.source;
                    ev.title = st.cachedTitle.empty()
                        ? ("Session " + item.cid.substr(0, std::min<size_t>(item.cid.size(), 8)))
                        : st.cachedTitle;
                    ev.workspace = st.cachedWorkspace;
                    ev.snippet = st.pendingSnippet;
                    ev.stepIndex = lastStepIdx;
                    ev.timestamp = static_cast<int64_t>(time(nullptr));

                    st.lastNotifiedStepIndex = lastStepIdx;
                    st.pendingStepIndex = -1;
                    gw.broadcastAgentSessionEvent(ev);
                }
                continue;
            }
        }

        st.pendingStepIndex = -1;
    }
}

void WebServer::stop() {
    if (!_running) return;

    _running = false;
    _notificationStopCv.notify_all();
    if (_notificationThread && _notificationThread->joinable()) {
        _notificationThread->join();
        _notificationThread.reset();
    }

    MobileGateway& gw = _mobileGateway ? *_mobileGateway : MobileGateway::getInstance();
    gw.setBroadcastCallback(nullptr);

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
