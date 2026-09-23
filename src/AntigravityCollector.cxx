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
#include <functional>
#include <set>
#include <unistd.h>
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "AgentTelemetryDb.hxx"

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

    std::vector<pid_t> candidatePids;
    try {
        for (const auto& entry : fs::directory_iterator("/proc")) {
            if (!entry.is_directory()) continue;
            std::string dirName = entry.path().filename().string();
            if (!std::all_of(dirName.begin(), dirName.end(), ::isdigit)) continue;

            pid_t pid = std::stoi(dirName);
            candidatePids.push_back(pid);
        }
    } catch (...) {
    }

    // Sort PIDs in descending order (highest/newest PID first)
    std::sort(candidatePids.begin(), candidatePids.end(), std::greater<pid_t>());

    for (pid_t pid : candidatePids) {
        std::string cmdlinePath = "/proc/" + std::to_string(pid) + "/cmdline";
        std::string content;
        try {
            std::ifstream cmdlineFile(cmdlinePath);
            if (!cmdlineFile.is_open()) continue;

            content.assign((std::istreambuf_iterator<char>(cmdlineFile)),
                           std::istreambuf_iterator<char>());
        } catch (...) {
            continue;
        }
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

        // If this PID matches our cached PID and port, probe it first
        if (pid == _cachedPid && _cachedPort > 0 && probePort(_cachedPort, discoveredCsrf)) {
            outPort = _cachedPort;
            outCsrf = discoveredCsrf;
            return true;
        }

        std::vector<int> ports = findCandidateListeningPorts(pid);
        for (int p : ports) {
            if (probePort(p, discoveredCsrf)) {
                _cachedPid = pid;
                _cachedPort = p;
                _cachedCsrf = discoveredCsrf;
                outPort = p;
                outCsrf = discoveredCsrf;
                return true;
            }
        }
    }

    // Fallback: if cached port and CSRF still respond
    if (_cachedPort > 0 && !_cachedCsrf.empty()) {
        if (probePort(_cachedPort, _cachedCsrf)) {
            outPort = _cachedPort;
            outCsrf = _cachedCsrf;
            return true;
        }
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

        auto parseIntOrString = [](const nlohmann::json& jVal, int defaultVal = 0) -> int {
            if (jVal.is_number_integer()) {
                return jVal.get<int>();
            } else if (jVal.is_string()) {
                try {
                    return std::stoi(jVal.get<std::string>());
                } catch (...) {}
            }
            return defaultVal;
        };

        // Parse available credits from userTier or userStatus
        const nlohmann::json* creditsArray = nullptr;
        if (userStatus.contains("userTier") && userStatus["userTier"].contains("availableCredits") &&
            userStatus["userTier"]["availableCredits"].is_array()) {
            creditsArray = &userStatus["userTier"]["availableCredits"];
        } else if (userStatus.contains("availableCredits") && userStatus["availableCredits"].is_array()) {
            creditsArray = &userStatus["availableCredits"];
        }

        if (creditsArray) {
            for (const auto& c : *creditsArray) {
                if (!c.is_object()) continue;
                UserCredit uc;
                if (c.contains("creditType") && c["creditType"].is_string()) {
                    uc.creditType = c["creditType"].get<std::string>();
                }
                if (c.contains("creditAmount")) {
                    uc.creditAmount = parseIntOrString(c["creditAmount"], 0);
                }
                if (c.contains("minimumCreditAmountForUsage")) {
                    uc.minimumCreditAmountForUsage = parseIntOrString(c["minimumCreditAmountForUsage"], 0);
                }
                status.availableCredits.push_back(uc);
            }
        }

        // Parse prompt and flow credits from planStatus
        if (userStatus.contains("planStatus") && userStatus["planStatus"].is_object()) {
            const auto& ps = userStatus["planStatus"];
            if (ps.contains("availablePromptCredits")) {
                status.availablePromptCredits = parseIntOrString(ps["availablePromptCredits"], 0);
            }
            if (ps.contains("availableFlowCredits")) {
                status.availableFlowCredits = parseIntOrString(ps["availableFlowCredits"], 0);
            }
            if (ps.contains("planInfo") && ps["planInfo"].is_object()) {
                const auto& pi = ps["planInfo"];
                if (pi.contains("monthlyPromptCredits")) {
                    status.monthlyPromptCredits = parseIntOrString(pi["monthlyPromptCredits"], 0);
                }
                if (pi.contains("monthlyFlowCredits")) {
                    status.monthlyFlowCredits = parseIntOrString(pi["monthlyFlowCredits"], 0);
                }
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
                                    qb.resetTimestamp = parseIsoTimestamp(qb.resetTimeIso);
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

void AntigravityCollector::syncTranscriptTelemetry() {
    time_t now = time(nullptr);
    if (_lastTranscriptScan != 0 && (now - _lastTranscriptScan < 5)) {
        return; // Throttle scan to every 5s
    }
    _lastTranscriptScan = now;

    const char *home = getenv("HOME");
    if (!home) return;

    fs::path brainDir = fs::path(home) / ".gemini" / "antigravity-ide" / "brain";
    if (!fs::exists(brainDir) || !fs::is_directory(brainDir)) {
        return;
    }

    try {
        for (const auto& entry : fs::directory_iterator(brainDir)) {
            if (!entry.is_directory()) continue;
            std::string convId = entry.path().filename().string();
            fs::path transcriptPath = entry.path() / ".system_generated" / "logs" / "transcript.jsonl";
            if (!fs::exists(transcriptPath)) continue;

            auto ftime = fs::last_write_time(transcriptPath);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            std::time_t mtime = std::chrono::system_clock::to_time_t(sctp);
            if (now - mtime > 60 * 86400) continue; // Skip stale files older than 60d

            int64_t lastOffset = 0;
            auto it = _transcriptOffsets.find(transcriptPath.string());
            if (it != _transcriptOffsets.end()) {
                lastOffset = it->second;
            }

            uintmax_t curSize = fs::file_size(transcriptPath);
            if (curSize <= static_cast<uintmax_t>(lastOffset)) {
                continue; // No new data
            }

            std::ifstream ifs(transcriptPath);
            if (!ifs.is_open()) continue;

            if (lastOffset > 0) {
                ifs.seekg(lastOffset);
            }

            std::string line;
            std::string sessionId = "sess-" + (convId.length() >= 8 ? convId.substr(0, 8) : convId);
            std::vector<nlohmann::json> parsedSteps;

            while (std::getline(ifs, line)) {
                if (line.empty()) continue;
                try {
                    auto j = nlohmann::json::parse(line, nullptr, false);
                    if (!j.is_discarded() && j.is_object()) {
                        parsedSteps.push_back(std::move(j));
                    }
                } catch (...) {}
            }

            _transcriptOffsets[transcriptPath.string()] = curSize;
            if (parsedSteps.empty()) continue;

            std::vector<AgentLifecycleEvent> eventsBatch;
            int totalTurns = 0;
            int totalTools = 0;
            int totalErrors = 0;
            double sumDuration = 0.0;
            int durationCount = 0;
            int64_t firstTs = 0;
            int64_t lastTs = 0;

            for (size_t i = 0; i < parsedSteps.size(); ++i) {
                const auto& j = parsedSteps[i];
                std::string stype = j.value("type", "");
                std::string status = j.value("status", "DONE");
                std::string iso = j.value("created_at", "");
                int stepIdx = j.value("step_index", static_cast<int>(i));

                int64_t stepTs = mtime;
                if (!iso.empty()) {
                    stepTs = std::chrono::system_clock::to_time_t(parseIsoTimestamp(iso));
                }
                if (firstTs == 0) firstTs = stepTs;
                lastTs = stepTs;

                double stepDurationMs = 0.0;
                if (i + 1 < parsedSteps.size()) {
                    std::string nextIso = parsedSteps[i + 1].value("created_at", "");
                    if (!iso.empty() && !nextIso.empty()) {
                        auto tp1 = parseIsoTimestamp(iso);
                        auto tp2 = parseIsoTimestamp(nextIso);
                        double dms = std::chrono::duration<double, std::milli>(tp2 - tp1).count();
                        if (dms >= 0.0 && dms <= 300000.0) {
                            stepDurationMs = dms;
                        }
                    }
                }

                if (status == "ERROR") {
                    totalErrors++;
                }

                if (stype == "USER_INPUT") {
                    totalTurns++;
                    AgentLifecycleEvent ev;
                    ev.timestamp = stepTs;
                    ev.sessionId = sessionId;
                    ev.agentType = "antigravity";
                    ev.eventType = "USER_TURN";
                    ev.stepIndex = stepIdx;
                    ev.durationMs = 0.0;
                    ev.status = (status == "ERROR" ? "ERROR" : "OK");
                    eventsBatch.push_back(ev);
                } else if (stype == "PLANNER_RESPONSE") {
                    if (stepDurationMs > 0.0) {
                        sumDuration += stepDurationMs;
                        durationCount++;
                    }
                    AgentLifecycleEvent ev;
                    ev.timestamp = stepTs;
                    ev.sessionId = sessionId;
                    ev.agentType = "antigravity";
                    ev.eventType = "THINKING";
                    ev.stepIndex = stepIdx;
                    ev.durationMs = stepDurationMs;
                    ev.status = (status == "ERROR" ? "ERROR" : "OK");
                    eventsBatch.push_back(ev);
                }

                if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
                    for (const auto& tc : j["tool_calls"]) {
                        if (tc.is_object() && tc.contains("name")) {
                            totalTools++;
                            AgentLifecycleEvent ev;
                            ev.timestamp = stepTs;
                            ev.sessionId = sessionId;
                            ev.agentType = "antigravity";
                            ev.eventType = "TOOL_CALL";
                            ev.stepIndex = stepIdx;
                            ev.toolName = tc["name"].get<std::string>();
                            ev.durationMs = stepDurationMs;
                            ev.status = (status == "ERROR" ? "ERROR" : "OK");
                            eventsBatch.push_back(ev);
                        }
                    }
                }

                if (stype == "RUN_COMMAND" || stype == "VIEW_FILE" || stype == "WRITE_TO_FILE" ||
                    stype == "REPLACE_FILE_CONTENT" || stype == "MULTI_REPLACE_FILE_CONTENT" ||
                    stype == "GREP_SEARCH" || stype == "LIST_DIRECTORY") {
                    totalTools++;
                    std::string tname = stype;
                    std::transform(tname.begin(), tname.end(), tname.begin(), ::tolower);
                    AgentLifecycleEvent ev;
                    ev.timestamp = stepTs;
                    ev.sessionId = sessionId;
                    ev.agentType = "antigravity";
                    ev.eventType = "TOOL_CALL";
                    ev.stepIndex = stepIdx;
                    ev.toolName = tname;
                    ev.durationMs = stepDurationMs;
                    ev.status = (status == "ERROR" ? "ERROR" : "OK");
                    eventsBatch.push_back(ev);
                }
            }

            if (!eventsBatch.empty()) {
                AgentTelemetryDb::getInstance().insertEventsBatch(eventsBatch);
                double avgTurnMs = durationCount > 0 ? (sumDuration / durationCount) : 0.0;
                int64_t sessionStart = firstTs > 0 ? firstTs : mtime;
                int64_t sessionEnd = lastTs > 0 ? lastTs : mtime;
                std::string sessionStatus = (now - sessionEnd < 300) ? "RUNNING" : "COMPLETED";

                AgentTelemetryDb::getInstance().recordSessionStart(
                    sessionId, convId, "antigravity", "/home/samurai/work", "Antigravity", sessionStart);
                AgentTelemetryDb::getInstance().updateSessionStats(
                    sessionId, sessionStatus, totalTurns, 0, 0, totalTools, totalErrors, avgTurnMs, sessionStart, sessionEnd);
            }
        }
    } catch (...) {}
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
