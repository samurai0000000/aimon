/*
 * AntigravityCollector.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "AntigravityCollector.hxx"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <set>
#include <unistd.h>
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace aimon {

AntigravityCollector::AntigravityCollector(const AntigravityConfig& config)
    : _config(config),
      _cachedPort(config.port),
      _cachedCsrf(config.csrfToken) {
}

static std::chrono::system_clock::time_point parseIsoTimestamp(const std::string& isoStr) {
    std::tm tm = {};
    std::istringstream ss(isoStr);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (ss.fail()) {
        std::tm tm2 = {};
        std::istringstream ss2(isoStr);
        ss2 >> std::get_time(&tm2, "%Y-%m-%d %H:%M:%S");
        if (!ss2.fail()) {
            return std::chrono::system_clock::from_time_t(timegm(&tm2));
        }
        return std::chrono::system_clock::now();
    }
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

bool AntigravityCollector::probePort(int port, const std::string& csrfToken) {
    if (port <= 0 || port > 65535) {
        return false;
    }

    try {
        httplib::SSLClient cli("127.0.0.1", port);
        cli.enable_server_certificate_verification(false);
        cli.set_connection_timeout(1, 0);
        cli.set_read_timeout(2, 0);

        httplib::Headers headers = {
            {"Content-Type", "application/json"},
            {"x-codeium-csrf-token", csrfToken}
        };

        auto res = cli.Post("/exa.language_server_pb.LanguageServerService/GetUserStatus", headers, "{}", "application/json");
        if (res && res->status == 200) {
            nlohmann::json j = nlohmann::json::parse(res->body, nullptr, false);
            if (!j.is_discarded() && j.contains("userStatus")) {
                return true;
            }
        }
    } catch (...) {
        return false;
    }

    return false;
}

std::vector<int> AntigravityCollector::findCandidateListeningPorts(pid_t pid) {
    std::vector<int> candidatePorts;
    std::set<std::string> socketInodes;

    std::string fdDir = "/proc/" + std::to_string(pid) + "/fd";
    if (!fs::exists(fdDir)) {
        return candidatePorts;
    }

    try {
        for (const auto& entry : fs::directory_iterator(fdDir)) {
            try {
                if (fs::is_symlink(entry.path())) {
                    std::string target = fs::read_symlink(entry.path()).string();
                    if (target.rfind("socket:[", 0) == 0) {
                        std::string inodeStr = target.substr(8, target.length() - 9);
                        socketInodes.insert(inodeStr);
                    }
                }
            } catch (...) {
            }
        }
    } catch (...) {
        return candidatePorts;
    }

    if (socketInodes.empty()) {
        return candidatePorts;
    }

    auto parseProcNet = [&](const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) return;

        std::string line;
        // Skip header
        std::getline(file, line);

        while (std::getline(file, line)) {
            std::istringstream ss(line);
            std::string sl, localAddr, remAddr, state, txRx, trTm, retrnsmt, uid, timeout, inode;
            if (ss >> sl >> localAddr >> remAddr >> state >> txRx >> trTm >> retrnsmt >> uid >> timeout >> inode) {
                if (state == "0A" && socketInodes.find(inode) != socketInodes.end()) {
                    auto colonPos = localAddr.find(':');
                    if (colonPos != std::string::npos) {
                        std::string portHex = localAddr.substr(colonPos + 1);
                        int port = std::stoi(portHex, nullptr, 16);
                        if (port > 0) {
                            candidatePorts.push_back(port);
                        }
                    }
                }
            }
        }
    };

    parseProcNet("/proc/net/tcp");
    parseProcNet("/proc/net/tcp6");

    return candidatePorts;
}

bool AntigravityCollector::discoverProcess(int& outPort, std::string& outCsrf) {
    if (_cachedPort > 0 && !_cachedCsrf.empty()) {
        if (probePort(_cachedPort, _cachedCsrf)) {
            outPort = _cachedPort;
            outCsrf = _cachedCsrf;
            return true;
        }
    }

    if (!_config.csrfToken.empty() && _config.port > 0) {
        if (probePort(_config.port, _config.csrfToken)) {
            _cachedPort = _config.port;
            _cachedCsrf = _config.csrfToken;
            outPort = _cachedPort;
            outCsrf = _cachedCsrf;
            return true;
        }
    }

    std::string envCsrf;
    const char* envTok = std::getenv("ANTIGRAVITY_CSRF_TOKEN");
    if (envTok && *envTok) {
        envCsrf = envTok;
    }

    if (!fs::exists("/proc")) {
        return false;
    }

    try {
        for (const auto& entry : fs::directory_iterator("/proc")) {
            if (!entry.is_directory()) continue;
            std::string dirName = entry.path().filename().string();
            if (!std::all_of(dirName.begin(), dirName.end(), ::isdigit)) continue;

            pid_t pid = std::stoi(dirName);
            std::string cmdlinePath = entry.path().string() + "/cmdline";
            std::ifstream cmdlineFile(cmdlinePath);
            if (!cmdlineFile.is_open()) continue;

            std::string content((std::istreambuf_iterator<char>(cmdlineFile)),
                                 std::istreambuf_iterator<char>());
            if (content.find("language_server") == std::string::npos) {
                continue;
            }

            // Extract CSRF token
            std::string discoveredCsrf;
            size_t pos = 0;
            while (pos < content.size()) {
                std::string arg = content.c_str() + pos;
                if (arg == "--csrf_token" && (pos + arg.length() + 1) < content.size()) {
                    discoveredCsrf = content.c_str() + pos + arg.length() + 1;
                    break;
                }
                pos += arg.length() + 1;
            }

            if (discoveredCsrf.empty() && !envCsrf.empty()) {
                discoveredCsrf = envCsrf;
            }

            if (discoveredCsrf.empty()) {
                continue;
            }

            std::vector<int> ports = findCandidateListeningPorts(pid);
            for (int p : ports) {
                if (probePort(p, discoveredCsrf)) {
                    _cachedPort = p;
                    _cachedCsrf = discoveredCsrf;
                    outPort = p;
                    outCsrf = discoveredCsrf;
                    return true;
                }
            }
        }
    } catch (...) {
    }

    return false;
}

AntigravityStatus AntigravityCollector::fetchStatus() {
    AntigravityStatus status;
    int port = 0;
    std::string csrf;

    if (!discoverProcess(port, csrf)) {
        status.isRunning = false;
        status.errorMessage = "Antigravity language server process not running or unreachable";
        return status;
    }

    try {
        httplib::SSLClient cli("127.0.0.1", port);
        cli.enable_server_certificate_verification(false);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(3, 0);

        httplib::Headers headers = {
            {"Content-Type", "application/json"},
            {"x-codeium-csrf-token", csrf}
        };

        auto res = cli.Post("/exa.language_server_pb.LanguageServerService/GetUserStatus", headers, "{}", "application/json");
        if (!res || res->status != 200) {
            status.isRunning = false;
            status.errorMessage = "RPC call GetUserStatus failed (status: " +
                (res ? std::to_string(res->status) : "connection error") + ")";
            return status;
        }

        nlohmann::json j = nlohmann::json::parse(res->body, nullptr, false);
        if (j.is_discarded() || !j.contains("userStatus")) {
            status.isRunning = false;
            status.errorMessage = "Invalid JSON response received from language server";
            return status;
        }

        status.isRunning = true;
        const auto& userStatus = j["userStatus"];

        if (userStatus.contains("userTier") && userStatus["userTier"].contains("name") &&
            userStatus["userTier"]["name"].is_string()) {
            status.planTier = userStatus["userTier"]["name"].get<std::string>();
        } else if (userStatus.contains("planStatus")) {
            const auto& ps = userStatus["planStatus"];
            if (ps.contains("planInfo") && ps["planInfo"].contains("planName")) {
                status.planTier = ps["planInfo"]["planName"];
            }
        }


        if (userStatus.contains("cascadeModelConfigData") &&
            userStatus["cascadeModelConfigData"].contains("clientModelConfigs")) {
            for (const auto& mc : userStatus["cascadeModelConfigData"]["clientModelConfigs"]) {
                ModelQuota mq;
                if (mc.contains("label")) mq.modelName = mc["label"];
                if (mc.contains("modelId")) mq.modelId = mc["modelId"];
                if (mc.contains("quotaInfo")) {
                    const auto& qi = mc["quotaInfo"];
                    if (qi.contains("remainingFraction")) {
                        mq.remainingFraction = qi["remainingFraction"];
                    }
                    if (qi.contains("resetTime")) {
                        mq.resetTimeIso = qi["resetTime"];
                        mq.resetTimestamp = parseIsoTimestamp(mq.resetTimeIso);
                    }
                }
                status.models.push_back(mq);
            }
        }

        // Query authoritative quota groups (Weekly & 5-Hour limits matching the IDE)
        try {
            auto quotaRes = cli.Post("/exa.language_server_pb.LanguageServerService/RetrieveUserQuotaSummary",
                                     headers, "{}", "application/json");
            if (quotaRes && quotaRes->status == 200) {
                nlohmann::json qj = nlohmann::json::parse(quotaRes->body, nullptr, false);
                if (!qj.is_discarded() && qj.contains("response") && qj["response"].contains("groups")) {
                    status.quotaGroups.clear();
                    for (const auto& g : qj["response"]["groups"]) {
                        if (!g.is_object()) continue;
                        QuotaGroup qg;
                        if (g.contains("displayName") && g["displayName"].is_string()) {
                            qg.displayName = g["displayName"].get<std::string>();
                        }
                        if (g.contains("description") && g["description"].is_string()) {
                            qg.description = g["description"].get<std::string>();
                        }

                        if (g.contains("buckets") && g["buckets"].is_array()) {
                            for (const auto& b : g["buckets"]) {
                                if (!b.is_object()) continue;
                                QuotaBucket qb;
                                if (b.contains("bucketId") && b["bucketId"].is_string()) {
                                    qb.bucketId = b["bucketId"].get<std::string>();
                                }
                                if (b.contains("displayName") && b["displayName"].is_string()) {
                                    qb.displayName = b["displayName"].get<std::string>();
                                }
                                if (b.contains("window") && b["window"].is_string()) {
                                    qb.window = b["window"].get<std::string>();
                                }
                                if (b.contains("description") && b["description"].is_string()) {
                                    qb.description = b["description"].get<std::string>();
                                }
                                if (b.contains("remainingFraction") && b["remainingFraction"].is_number()) {
                                    qb.remainingFraction = b["remainingFraction"].get<double>();
                                }
                                if (b.contains("resetTime") && b["resetTime"].is_string()) {
                                    qb.resetTimeIso = b["resetTime"].get<std::string>();
                                }
                                qg.buckets.push_back(qb);
                            }
                        }
                        status.quotaGroups.push_back(qg);
                    }
                }
            }
        } catch (...) {}

    } catch (const std::exception& e) {
        status.isRunning = false;
        status.errorMessage = std::string("Exception fetching Antigravity status: ") + e.what();
    }

    return status;
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
