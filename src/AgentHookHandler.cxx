/*
 * AgentHookHandler.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "AgentHookHandler.hxx"
#include "AgentTelemetryDb.hxx"
#include "PathUtils.hxx"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <algorithm>
#include <set>
#include <vector>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace aimon {

namespace {

std::string formatIsoTimestampNow() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm gm{};
#if defined(_WIN32)
    gmtime_s(&gm, &tt);
#else
    gmtime_r(&tt, &gm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&gm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

std::string trimString(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string firstString(const json& payload, const std::vector<std::string>& keys) {
    if (!payload.is_object()) {
        return "";
    }
    for (const auto& k : keys) {
        auto it = payload.find(k);
        if (it != payload.end() && it->is_string()) {
            std::string v = trimString(it->get<std::string>());
            if (!v.empty()) {
                return v;
            }
        }
    }
    return "";
}

double extractDurationMs(const json& payload) {
    if (!payload.is_object()) {
        return 0.0;
    }
    for (const char* k : {"duration", "duration_ms"}) {
        auto it = payload.find(k);
        if (it != payload.end() && it->is_number()) {
            double v = it->get<double>();
            if (v >= 0.0) {
                return v;
            }
        }
    }
    return 0.0;
}

std::string resolveStateDir(const std::string& provider, const std::string& customStateDir) {
    if (!customStateDir.empty()) {
        return PathUtils::expandHome(customStateDir);
    }
    if (provider == "claude") {
        const char* envDir = std::getenv("AIMON_CLAUDE_HOOK_STATE_DIR");
        if (envDir && envDir[0] != '\0') {
            return PathUtils::expandHome(envDir);
        }
        return PathUtils::expandHome("~/.claude/hooks/state");
    }
    const char* envDir = std::getenv("AIMON_CURSOR_HOOK_STATE_DIR");
    if (envDir && envDir[0] != '\0') {
        return PathUtils::expandHome(envDir);
    }
    return PathUtils::expandHome("~/.cursor/hooks/state");
}

bool ensureStateDirPerms(const std::string& stateDir) {
    try {
        if (!fs::exists(stateDir)) {
            fs::create_directories(stateDir);
        }
        ::chmod(stateDir.c_str(), 0700);
        return true;
    } catch (...) {
        return false;
    }
}

bool writeAtomicFile0600(const std::string& stateDir,
                         const std::string& filePath,
                         const std::string& content) {
    try {
        ensureStateDirPerms(stateDir);
        std::string tmpPath = filePath + ".tmp." + std::to_string(::getpid());
        {
            std::ofstream ofs(tmpPath, std::ios::out | std::ios::trunc | std::ios::binary);
            if (!ofs.is_open()) {
                return false;
            }
            ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
            ofs.flush();
            if (!ofs.good()) {
                ofs.close();
                fs::remove(tmpPath);
                return false;
            }
        }
        ::chmod(tmpPath.c_str(), 0600);
        fs::rename(tmpPath, filePath);
        ::chmod(filePath.c_str(), 0600);
        return true;
    } catch (...) {
        return false;
    }
}

bool appendAtomicJsonl0600(const std::string& stateDir,
                           const std::string& jsonlPath,
                           const json& lineObj) {
    try {
        ensureStateDirPerms(stateDir);
        std::string existing;
        if (fs::exists(jsonlPath)) {
            std::ifstream ifs(jsonlPath, std::ios::in | std::ios::binary);
            if (ifs.is_open()) {
                std::ostringstream ss;
                ss << ifs.rdbuf();
                existing = ss.str();
            }
        }
        if (!existing.empty() && existing.back() != '\n') {
            existing.push_back('\n');
        }
        existing += lineObj.dump() + "\n";
        return writeAtomicFile0600(stateDir, jsonlPath, existing);
    } catch (...) {
        return false;
    }
}

std::vector<std::string> readProcNulTokens(const std::string& procFile) {
    std::vector<std::string> tokens;
    std::string buf;
    try {
        std::ifstream ifs(procFile, std::ios::in | std::ios::binary);
        if (!ifs.is_open()) {
            return tokens;
        }
        buf.assign((std::istreambuf_iterator<char>(ifs)),
                   std::istreambuf_iterator<char>());
    } catch (...) {
        return tokens;
    }
    size_t start = 0;
    for (size_t i = 0; i <= buf.size(); ++i) {
        if (i == buf.size() || buf[i] == '\0') {
            if (i > start) {
                tokens.push_back(buf.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    return tokens;
}

std::string getExecutableBasename(const std::string& arg0) {
    if (arg0.empty()) {
        return "";
    }
    size_t slash = arg0.find_last_of('/');
    std::string base = (slash == std::string::npos) ? arg0 : arg0.substr(slash + 1);
    std::transform(base.begin(), base.end(), base.begin(), ::tolower);
    return base;
}

bool isOneShotTokens(const std::vector<std::string>& tokens) {
    for (const auto& t : tokens) {
        if (t == "-p" || t == "--print") {
            return true;
        }
    }
    return false;
}

int readParentPidFromProcStat(int pid) {
    if (pid <= 1) {
        return 0;
    }
    try {
        std::ifstream statIfs("/proc/" + std::to_string(pid) + "/stat");
        std::string statLine;
        if (!statIfs.is_open() || !std::getline(statIfs, statLine)) {
            return 0;
        }
        size_t rparen = statLine.rfind(')');
        if (rparen == std::string::npos || (rparen + 2) >= statLine.size()) {
            return 0;
        }
        std::istringstream iss(statLine.substr(rparen + 2));
        std::string stateCh;
        int ppid = 0;
        iss >> stateCh >> ppid;
        return ppid;
    } catch (...) {
        return 0;
    }
}

std::string readScreenSessionFromProc(int pid) {
    const char* envSty = std::getenv("STY");
    if (envSty && envSty[0] != '\0') {
        return std::string(envSty);
    }
    int curPid = pid;
    for (int hop = 0; hop <= 10 && curPid > 1; ++hop) {
        auto envTokens = readProcNulTokens("/proc/" + std::to_string(curPid) + "/environ");
        for (const auto& item : envTokens) {
            if (item.rfind("STY=", 0) == 0 && item.size() > 4) {
                return item.substr(4);
            }
        }
        curPid = readParentPidFromProcStat(curPid);
    }
    return "";
}

void detectMultiplexerForPid(int startPid,
                             std::string& outMuxType,
                             std::string& outMuxSession,
                             std::string& outMuxTarget,
                             std::string& outScreenSession) {
    outMuxType.clear();
    outMuxSession.clear();
    outMuxTarget.clear();
    outScreenSession.clear();

    std::string sty;
    std::string windowIdx;
    std::string tmuxEnv;
    std::string tmuxPane;
    std::string zellijSession;
    std::string weztermPane;
    std::string kittyListen;
    std::string kittyWindow;
    int screenAncestorPid = 0;

    auto inspectEnvTokens = [&](const std::vector<std::string>& envTokens) {
        for (const auto& item : envTokens) {
            if (sty.empty() && item.rfind("STY=", 0) == 0 && item.size() > 4) {
                sty = item.substr(4);
            } else if (windowIdx.empty() && item.rfind("WINDOW=", 0) == 0 && item.size() > 7) {
                windowIdx = item.substr(7);
            } else if (tmuxEnv.empty() && item.rfind("TMUX=", 0) == 0 && item.size() > 5) {
                tmuxEnv = item.substr(5);
            } else if (tmuxPane.empty() && item.rfind("TMUX_PANE=", 0) == 0 && item.size() > 10) {
                tmuxPane = item.substr(10);
            } else if (zellijSession.empty() && item.rfind("ZELLIJ_SESSION_NAME=", 0) == 0 && item.size() > 20) {
                zellijSession = item.substr(20);
            } else if (weztermPane.empty() && item.rfind("WEZTERM_PANE=", 0) == 0 && item.size() > 13) {
                weztermPane = item.substr(13);
            } else if (kittyListen.empty() && item.rfind("KITTY_LISTEN_ON=", 0) == 0 && item.size() > 16) {
                kittyListen = item.substr(16);
            } else if (kittyWindow.empty() && item.rfind("KITTY_WINDOW_ID=", 0) == 0 && item.size() > 16) {
                kittyWindow = item.substr(16);
            }
        }
    };

    int curPid = startPid;
    for (int hop = 0; hop <= 10 && curPid > 1; ++hop) {
        inspectEnvTokens(readProcNulTokens("/proc/" + std::to_string(curPid) + "/environ"));
        try {
            std::ifstream commIfs("/proc/" + std::to_string(curPid) + "/comm");
            std::string comm;
            if (commIfs.is_open() && std::getline(commIfs, comm)) {
                comm = trimString(comm);
                if (comm == "SCREEN" && screenAncestorPid == 0) {
                    screenAncestorPid = curPid;
                }
            }
        } catch (...) {}
        curPid = readParentPidFromProcStat(curPid);
    }

    if (sty.empty() && screenAncestorPid > 0) {
        sty = std::to_string(screenAncestorPid);
    }

    if (!sty.empty()) {
        outMuxType = "screen";
        outMuxSession = sty;
        outMuxTarget = windowIdx.empty() ? "0" : windowIdx;
        outScreenSession = sty;
        return;
    }

    if (!tmuxEnv.empty() || !tmuxPane.empty()) {
        std::string socketPath;
        std::string sessionIdx;
        if (!tmuxEnv.empty()) {
            size_t firstComma = tmuxEnv.find(',');
            if (firstComma != std::string::npos) {
                socketPath = tmuxEnv.substr(0, firstComma);
                size_t secondComma = tmuxEnv.find(',', firstComma + 1);
                if (secondComma != std::string::npos && (secondComma + 1) < tmuxEnv.size()) {
                    sessionIdx = tmuxEnv.substr(secondComma + 1);
                }
            } else {
                socketPath = tmuxEnv;
            }
        }
        outMuxType = "tmux";
        outMuxSession = socketPath;
        outMuxTarget = !tmuxPane.empty() ? tmuxPane : sessionIdx;
        return;
    }

    if (!zellijSession.empty()) {
        outMuxType = "zellij";
        outMuxSession = zellijSession;
        outMuxTarget = zellijSession;
        return;
    }

    if (!weztermPane.empty()) {
        outMuxType = "wezterm";
        outMuxSession = weztermPane;
        outMuxTarget = weztermPane;
        return;
    }

    if (!kittyListen.empty() && !kittyWindow.empty()) {
        outMuxType = "kitty";
        outMuxSession = kittyListen;
        outMuxTarget = kittyWindow;
        return;
    }
}

bool isCursorSubagentSession(const std::string& sessionId) {
    if (!AgentTelemetryDb::isValidSessionId(sessionId)) {
        return false;
    }
    try {
        fs::path projectsDir(PathUtils::expandHome("~/.cursor/projects"));
        if (fs::exists(projectsDir) && fs::is_directory(projectsDir)) {
            for (const auto& projEntry : fs::directory_iterator(projectsDir)) {
                if (!projEntry.is_directory()) continue;
                fs::path transcriptsDir = projEntry.path() / "agent-transcripts";
                if (!fs::exists(transcriptsDir) || !fs::is_directory(transcriptsDir)) continue;
                for (const auto& convEntry : fs::directory_iterator(transcriptsDir)) {
                    if (!convEntry.is_directory()) continue;
                    fs::path subPath = convEntry.path() / "subagents" / (sessionId + ".jsonl");
                    if (fs::exists(subPath)) {
                        return true;
                    }
                }
            }
        }
    } catch (...) {}
    return false;
}

bool hasLiveInteractiveOwnerProcess(const std::string& sessionId, const std::string& provider) {
    if (!AgentTelemetryDb::isValidSessionId(sessionId)) {
        return false;
    }
    try {
        int selfPid = ::getpid();
        int parentPid = ::getppid();
        for (const auto& entry : fs::directory_iterator("/proc")) {
            if (!entry.is_directory()) continue;
            std::string name = entry.path().filename().string();
            if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) continue;
            int pid = std::stoi(name);
            if (pid == selfPid || pid == parentPid) continue;
            if (!AgentHookHandler::isInteractiveCliProcess(pid, provider, nullptr)) continue;

            auto tokens = readProcNulTokens("/proc/" + name + "/cmdline");
            for (const auto& tok : tokens) {
                if (tok.find(sessionId) != std::string::npos) {
                    return true;
                }
            }
        }
    } catch (...) {}
    return false;
}

void rememberCurrentSession(const std::string& stateDir, const std::string& sessionId) {
    if (!AgentTelemetryDb::isValidSessionId(sessionId)) {
        return;
    }
    json data = {{"session_id", sessionId}};
    writeAtomicFile0600(stateDir, stateDir + "/current.json", data.dump());
}

std::string loadCurrentSession(const std::string& stateDir) {
    std::string path = stateDir + "/current.json";
    try {
        if (!fs::exists(path)) {
            return "";
        }
        std::ifstream ifs(path);
        json data = json::parse(ifs);
        std::string sid = firstString(data, {"session_id"});
        return AgentTelemetryDb::isValidSessionId(sid) ? sid : "";
    } catch (...) {
        return "";
    }
}

std::string extractWorkspace(const json& payload, const std::string& provider) {
    if (payload.is_object()) {
        auto rootsIt = payload.find("workspace_roots");
        if (rootsIt != payload.end() && rootsIt->is_array()) {
            for (const auto& r : *rootsIt) {
                if (r.is_string()) {
                    std::string s = trimString(r.get<std::string>());
                    if (!s.empty()) return s;
                }
            }
        }
        auto pathsIt = payload.find("workspacePaths");
        if (pathsIt != payload.end() && pathsIt->is_array()) {
            for (const auto& r : *pathsIt) {
                if (r.is_string()) {
                    std::string s = trimString(r.get<std::string>());
                    if (!s.empty()) return s;
                }
            }
        }
    }
    std::string ws = firstString(payload, {"cwd", "workspace_path", "workspace"});
    if (provider == "claude" && !ws.empty()) {
        const char* wsMode = std::getenv("AIMON_WORKSPACE");
        if (wsMode && std::string(wsMode) == "basename") {
            fs::path p(ws);
            std::string base = p.filename().string();
            if (!base.empty()) {
                return base;
            }
        }
    }
    return ws;
}

std::string extractModel(const json& payload, const std::string& provider) {
    std::string m = firstString(payload, {"modelName", "model", "model_id"});
    if (!m.empty()) {
        return m;
    }
    if (provider == "claude") return "Claude";
    if (provider == "antigravity") return "Antigravity";
    return "Cursor";
}

json createDefaultState(const std::string& sessionId,
                        const std::string& workspace,
                        const std::string& model,
                        int64_t nowSec) {
    return json{
        {"session_id", sessionId},
        {"conversation_id", sessionId},
        {"workspace", workspace},
        {"model", model},
        {"first_prompt", ""},
        {"last_prompt", ""},
        {"start_ts", nowSec},
        {"updated_ts", nowSec},
        {"owner_pid", 0},
        {"mux_type", ""},
        {"mux_session", ""},
        {"mux_target", ""},
        {"screen_session", ""},
        {"source", ""},
        {"status", "RUNNING"},
        {"step", 0},
        {"tools", 0},
        {"errors", 0},
        {"turns", 0},
        {"prompt_tokens", 0},
        {"comp_tokens", 0},
        {"turn_ms_total", 0.0},
        {"prompt_ts", 0},
        {"active_subagents", 0},
        {"started", false},
        {"tool_ids", json::array()}
    };
}

json loadSessionState(const std::string& stateDir,
                      const std::string& sessionId,
                      const std::string& workspace,
                      const std::string& model,
                      const std::string& defaultModel,
                      int64_t nowSec) {
    std::string path = stateDir + "/" + sessionId + ".json";
    json state;
    try {
        if (fs::exists(path)) {
            std::ifstream ifs(path);
            state = json::parse(ifs);
        }
    } catch (...) {
        state = json();
    }
    if (!state.is_object()) {
        state = createDefaultState(sessionId, workspace, model, nowSec);
    }
    state["session_id"] = sessionId;
    if (!state.contains("conversation_id") || !state["conversation_id"].is_string() ||
        state["conversation_id"].get<std::string>().empty()) {
        state["conversation_id"] = sessionId;
    }
    if (!workspace.empty() &&
        (!state.contains("workspace") || !state["workspace"].is_string() ||
         state["workspace"].get<std::string>().empty())) {
        state["workspace"] = workspace;
    }
    if (!model.empty() &&
        (!state.contains("model") || !state["model"].is_string() ||
         state["model"].get<std::string>().empty() ||
         state["model"].get<std::string>() == defaultModel)) {
        state["model"] = model;
    }
    if (!state.contains("first_prompt") || !state["first_prompt"].is_string()) {
        state["first_prompt"] = "";
    }
    if (!state.contains("last_prompt") || !state["last_prompt"].is_string()) {
        state["last_prompt"] = "";
    }
    if (!state.contains("start_ts") || !state["start_ts"].is_number_integer()) {
        state["start_ts"] = nowSec;
    }
    state["updated_ts"] = nowSec;
    if (!state.contains("owner_pid") || !state["owner_pid"].is_number_integer()) {
        state["owner_pid"] = 0;
    }
    if (!state.contains("mux_type") || !state["mux_type"].is_string()) {
        state["mux_type"] = "";
    }
    if (!state.contains("mux_session") || !state["mux_session"].is_string()) {
        state["mux_session"] = "";
    }
    if (!state.contains("mux_target") || !state["mux_target"].is_string()) {
        state["mux_target"] = "";
    }
    if (!state.contains("screen_session") || !state["screen_session"].is_string()) {
        state["screen_session"] = "";
    }
    if (!state.contains("source") || !state["source"].is_string()) {
        state["source"] = "";
    }
    if (!state.contains("status") || !state["status"].is_string()) {
        state["status"] = "RUNNING";
    }
    if (!state.contains("step") || !state["step"].is_number_integer()) {
        state["step"] = 0;
    }
    if (!state.contains("tools") || !state["tools"].is_number_integer()) {
        state["tools"] = 0;
    }
    if (!state.contains("errors") || !state["errors"].is_number_integer()) {
        state["errors"] = 0;
    }
    if (!state.contains("turns") || !state["turns"].is_number_integer()) {
        state["turns"] = 0;
    }
    if (!state.contains("prompt_tokens") || !state["prompt_tokens"].is_number_integer()) {
        state["prompt_tokens"] = 0;
    }
    if (!state.contains("comp_tokens") || !state["comp_tokens"].is_number_integer()) {
        state["comp_tokens"] = 0;
    }
    if (!state.contains("turn_ms_total") || !state["turn_ms_total"].is_number()) {
        state["turn_ms_total"] = 0.0;
    }
    if (!state.contains("prompt_ts") || !state["prompt_ts"].is_number_integer()) {
        state["prompt_ts"] = 0;
    }
    if (!state.contains("active_subagents") || !state["active_subagents"].is_number_integer()) {
        state["active_subagents"] = 0;
    }
    if (!state.contains("tool_ids") || !state["tool_ids"].is_array()) {
        state["tool_ids"] = json::array();
    }
    return state;
}

void saveSessionState(const std::string& stateDir,
                      const std::string& provider,
                      json& state) {
    std::string sid = state.value("session_id", "");
    if (!AgentTelemetryDb::isValidSessionId(sid)) {
        return;
    }
    int ownerPid = 0;
    std::string screenSess;
    int curPid = ::getppid();
    for (int hop = 0; hop < 12 && curPid > 1; ++hop) {
        if (AgentHookHandler::isInteractiveCliProcess(curPid, provider, &screenSess)) {
            ownerPid = curPid;
            break;
        }
        curPid = readParentPidFromProcStat(curPid);
    }

    if (provider == "cursor" && (isCursorSubagentSession(sid) || sid.rfind("agent-", 0) == 0)) {
        state["is_subagent"] = true;
    } else if (ownerPid > 0) {
        state["owner_pid"] = ownerPid;
    }

    if (ownerPid > 0) {
        state["source"] = "cli";
    } else if (state.value("source", "") != "cli") {
        state["source"] = "ide";
    }

    int muxStartPid = (ownerPid > 0) ? ownerPid : ::getppid();
    std::string muxType;
    std::string muxSession;
    std::string muxTarget;
    std::string detectedScreen;
    detectMultiplexerForPid(muxStartPid, muxType, muxSession, muxTarget, detectedScreen);

    if (!muxType.empty()) {
        state["mux_type"] = muxType;
        state["mux_session"] = muxSession;
        state["mux_target"] = muxTarget;
    }
    if (screenSess.empty()) {
        screenSess = detectedScreen;
    }
    if (!screenSess.empty()) {
        state["screen_session"] = screenSess;
    }
    std::string path = stateDir + "/" + sid + ".json";
    writeAtomicFile0600(stateDir, path, state.dump());
}

json extractSafeToolInput(const json& payload) {
    json safeObj = json::object();
    if (!payload.is_object()) {
        return safeObj;
    }
    json inputSrc;
    auto it = payload.find("tool_input");
    if (it == payload.end()) {
        it = payload.find("input");
    }
    if (it != payload.end()) {
        if (it->is_object()) {
            inputSrc = *it;
        } else if (it->is_string()) {
            try {
                json parsed = json::parse(it->get<std::string>());
                if (parsed.is_object()) {
                    inputSrc = parsed;
                }
            } catch (...) {}
        }
    }
    if (!inputSrc.is_object()) {
        return safeObj;
    }

    static const std::set<std::string> kSafeKeys = {
        "path", "file_path", "target_file", "command", "query",
        "pattern", "toolSummary", "CommandLine", "Path", "url", "description"
    };
    for (auto fieldIt = inputSrc.begin(); fieldIt != inputSrc.end(); ++fieldIt) {
        const std::string& key = fieldIt.key();
        if (kSafeKeys.find(key) == kSafeKeys.end()) {
            continue;
        }
        if (fieldIt.value().is_string()) {
            std::string val = AgentTelemetryDb::scrubSecrets(fieldIt.value().get<std::string>());
            if (val.size() > 160) {
                val = val.substr(0, 160) + "...";
            }
            safeObj[key] = val;
        } else if (fieldIt.value().is_number() || fieldIt.value().is_boolean()) {
            safeObj[key] = fieldIt.value();
        }
    }
    return safeObj;
}

bool saveAgyEndpointEntry(const std::string& stateDir,
                          const std::string& lsAddr,
                          const std::string& csrf,
                          const std::string& convId,
                          const std::string& workspace,
                          const std::string& model) {
    if (lsAddr.empty() || csrf.empty()) {
        return false;
    }
    std::string portStr = lsAddr;
    size_t colonPos = lsAddr.rfind(':');
    if (colonPos != std::string::npos) {
        portStr = trimString(lsAddr.substr(colonPos + 1));
    }
    int port = 0;
    try {
        port = std::stoi(portStr);
    } catch (...) {
        return false;
    }
    if (port <= 0 || port > 65535) {
        return false;
    }
    if (!convId.empty() && !AgentTelemetryDb::isValidSessionId(convId)) {
        return false;
    }

    ensureStateDirPerms(stateDir);
    std::string epPath = stateDir + "/agy_ls_endpoints.json";
    std::string lockPath = epPath + ".lock";
    int lockFd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0600);
    if (lockFd >= 0) {
        (void)::flock(lockFd, LOCK_EX);
    }

    json data = json::object();
    try {
        if (fs::exists(epPath)) {
            std::ifstream ifs(epPath);
            data = json::parse(ifs);
        }
    } catch (...) {
        data = json::object();
    }
    if (!data.is_object() || !data.contains("endpoints") || !data["endpoints"].is_object()) {
        data = json{{"endpoints", json::object()}};
    }
    std::string portKey = std::to_string(port);
    json existing = data["endpoints"].value(portKey, json::object());
    if (!existing.is_object()) {
        existing = json::object();
    }

    std::string effConvId = !convId.empty() ? convId : existing.value("conversation_id", "");
    std::string effWorkspace = !workspace.empty() ? workspace : existing.value("workspace", "");
    std::string effModel = (!model.empty() && model != "Antigravity")
        ? model
        : existing.value("model", model.empty() ? "Antigravity" : model);

    data["endpoints"][portKey] = json{
        {"port", port},
        {"ls_address", lsAddr},
        {"csrf_token", csrf},
        {"conversation_id", effConvId},
        {"workspace", effWorkspace},
        {"model", effModel},
        {"updated_at", static_cast<int64_t>(std::time(nullptr))}
    };
    bool ok = writeAtomicFile0600(stateDir, epPath, data.dump());
    if (lockFd >= 0) {
        (void)::flock(lockFd, LOCK_UN);
        ::close(lockFd);
    }
    return ok;
}

std::vector<int> findAncestorAgyPids() {
    std::vector<int> pids;
    int cur = ::getppid();
    for (int depth = 0; depth < 5 && cur > 1; ++depth) {
        try {
            std::ifstream commIfs("/proc/" + std::to_string(cur) + "/comm");
            std::string comm;
            if (commIfs.is_open()) {
                std::getline(commIfs, comm);
                comm = trimString(comm);
                if (comm == "agy") {
                    pids.push_back(cur);
                }
            }
            std::ifstream statIfs("/proc/" + std::to_string(cur) + "/stat");
            std::string statLine;
            if (!statIfs.is_open() || !std::getline(statIfs, statLine)) {
                break;
            }
            size_t rparen = statLine.rfind(')');
            if (rparen == std::string::npos || (rparen + 2) >= statLine.size()) {
                break;
            }
            std::istringstream iss(statLine.substr(rparen + 2));
            std::string stateCh;
            int nextPpid = 0;
            iss >> stateCh >> nextPpid;
            cur = nextPpid;
        } catch (...) {
            break;
        }
    }
    return pids;
}

void resolveAncestorAgyContext(std::string& ioConvId, std::string& ioWorkspace) {
    auto agyPids = findAncestorAgyPids();
    for (int apid : agyPids) {
        std::string pidStr = std::to_string(apid);
        if (ioWorkspace.empty()) {
            std::error_code ec;
            auto cwdPath = fs::read_symlink("/proc/" + pidStr + "/cwd", ec);
            if (!ec && !cwdPath.empty()) {
                ioWorkspace = cwdPath.string();
            }
        }
        if (ioConvId.empty()) {
            std::error_code ec;
            std::string fdDir = "/proc/" + pidStr + "/fd";
            if (fs::exists(fdDir, ec)) {
                for (const auto& fdEntry : fs::directory_iterator(fdDir, ec)) {
                    if (ec) break;
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
                            ioConvId = lockName.substr(0, lockName.size() - 5);
                            break;
                        }
                    }
                }
            }
        }
        if (!ioConvId.empty() && !ioWorkspace.empty()) {
            break;
        }
    }
}

void recordAgyLsEndpointFromEnv(const std::string& stateDir, const json& payload) {
    const char* lsEnv = std::getenv("ANTIGRAVITY_LS_ADDRESS");
    const char* csrfEnv = std::getenv("ANTIGRAVITY_CSRF_TOKEN");
    const char* cidEnv = std::getenv("ANTIGRAVITY_CONVERSATION_ID");
    std::string lsAddr = lsEnv ? trimString(lsEnv) : "";
    std::string csrf = csrfEnv ? trimString(csrfEnv) : "";
    std::string convId = firstString(payload, {"conversationId", "conversation_id"});
    if (convId.empty() && cidEnv) {
        convId = trimString(cidEnv);
    }
    std::string workspace = extractWorkspace(payload, "antigravity");
    resolveAncestorAgyContext(convId, workspace);
    if (workspace.empty()) {
        try {
            workspace = fs::current_path().string();
        } catch (...) {}
    }
    std::string model = extractModel(payload, "antigravity");
    saveAgyEndpointEntry(stateDir, lsAddr, csrf, convId, workspace, model);
}

void spawnAgyChildEnvWatcher(const std::string& stateDir, const json& payload) {
    auto agyPids = findAncestorAgyPids();
    if (agyPids.empty()) {
        return;
    }
    std::string convId = firstString(payload, {"conversationId", "conversation_id"});
    std::string workspace = extractWorkspace(payload, "antigravity");
    resolveAncestorAgyContext(convId, workspace);
    if (workspace.empty()) {
        try {
            workspace = fs::current_path().string();
        } catch (...) {}
    }
    std::string model = extractModel(payload, "antigravity");

    pid_t pid = ::fork();
    if (pid != 0) {
        return;
    }
    ::setsid();
    int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        if (devnull > 2) {
            ::close(devnull);
        }
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(450);
    while (std::chrono::steady_clock::now() < deadline) {
        for (int apid : agyPids) {
            std::string pidStr = std::to_string(apid);
            std::string taskDir = "/proc/" + pidStr + "/task";
            try {
                if (!fs::exists(taskDir)) continue;
                std::set<std::string> childPids;
                std::ifstream mainChildrenIfs(taskDir + "/" + pidStr + "/children");
                if (mainChildrenIfs.is_open()) {
                    std::string tok;
                    while (mainChildrenIfs >> tok) {
                        if (!tok.empty() && std::all_of(tok.begin(), tok.end(), ::isdigit)) {
                            childPids.insert(tok);
                        }
                    }
                }
                if (childPids.empty()) {
                    for (const auto& tidEntry : fs::directory_iterator(taskDir)) {
                        std::ifstream cIfs(tidEntry.path() / "children");
                        std::string tok;
                        while (cIfs >> tok) {
                            if (!tok.empty() && std::all_of(tok.begin(), tok.end(), ::isdigit)) {
                                childPids.insert(tok);
                            }
                        }
                    }
                }
                for (const auto& cpid : childPids) {
                    auto envTokens = readProcNulTokens("/proc/" + cpid + "/environ");
                    std::string lsAddr;
                    std::string csrf;
                    std::string cid = convId;
                    for (const auto& item : envTokens) {
                        if (item.rfind("ANTIGRAVITY_LS_ADDRESS=", 0) == 0) {
                            lsAddr = trimString(item.substr(23));
                        } else if (item.rfind("ANTIGRAVITY_CSRF_TOKEN=", 0) == 0) {
                            csrf = trimString(item.substr(23));
                        } else if (cid.empty() && item.rfind("ANTIGRAVITY_CONVERSATION_ID=", 0) == 0) {
                            cid = trimString(item.substr(28));
                        }
                    }
                    if (!csrf.empty() && !lsAddr.empty() &&
                        saveAgyEndpointEntry(stateDir, lsAddr, csrf, cid, workspace, model)) {
                        ::_exit(0);
                    }
                }
            } catch (...) {}
        }
        ::usleep(200);
    }
    ::_exit(0);
}

} // anonymous namespace

bool AgentHookHandler::isInteractiveCliProcess(int pid,
                                               const std::string& provider,
                                               std::string* outScreenSession) {
    if (pid <= 0) {
        return false;
    }
    auto tokens = readProcNulTokens("/proc/" + std::to_string(pid) + "/cmdline");
    if (tokens.empty() || isOneShotTokens(tokens)) {
        return false;
    }

    // Explicitly reject IDE extensionHost, node IDE workers, daemons, shells, and hook wrappers
    for (const auto& tok : tokens) {
        std::string lower = tok;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find(".cursor-server") != std::string::npos ||
            lower.find("extensionhost") != std::string::npos ||
            lower.find("gpg-agent") != std::string::npos ||
            lower.find("ssh-agent") != std::string::npos ||
            lower.find("-javaagent:") != std::string::npos ||
            lower.find("aimon_telemetry") != std::string::npos) {
            return false;
        }
    }

    std::string base0 = getExecutableBasename(tokens[0]);
    if (base0 == "aimon" || base0 == "python" || base0 == "python3" ||
        base0 == "sh" || base0 == "bash" || base0 == "zsh" || base0 == "dash") {
        return false;
    }

    bool matched = false;
    if (provider == "cursor") {
        if (base0 == "cursor-agent" || base0 == "agent") {
            matched = true;
        } else if (base0 == "node" && tokens.size() > 1) {
            std::string base1 = getExecutableBasename(tokens[1]);
            if (base1 == "cursor-agent" || tokens[1].find("cursor-agent") != std::string::npos) {
                matched = true;
            }
        }
    } else if (provider == "claude") {
        if (base0 == "claude") {
            matched = true;
        } else if (base0 == "node" && tokens.size() > 1) {
            std::string base1 = getExecutableBasename(tokens[1]);
            if (base1 == "claude" || tokens[1].find("claude") != std::string::npos) {
                matched = true;
            }
        }
    }

    if (matched && outScreenSession) {
        *outScreenSession = readScreenSessionFromProc(pid);
    }
    return matched;
}

bool AgentHookHandler::processPayload(const nlohmann::json& payload,
                                      const std::string& forcedProvider,
                                      const std::string& eventKindOverride,
                                      const std::string& customStateDir,
                                      const std::string& customDbPath) {
    try {
        if (!payload.is_object()) {
            return false;
        }

        auto& db = AgentTelemetryDb::getInstance();
        if (!db.open(customDbPath)) {
            return false;
        }

        std::string eventName = firstString(payload, {"hook_event_name", "event"});
        std::string provider = forcedProvider;
        if (!eventKindOverride.empty() || provider == "antigravity") {
            provider = "antigravity";
        } else if (provider.empty()) {
            if (eventName == "SessionStart" || eventName == "UserPromptSubmit" ||
                eventName == "PreToolUse" ||
                eventName == "PostToolUse" || eventName == "PostToolUseFailure" ||
                eventName == "Stop" || eventName == "SessionEnd") {
                provider = "claude";
            } else {
                provider = "cursor";
            }
        }

        std::string stateDir = resolveStateDir(provider, customStateDir);
        int64_t nowSec = static_cast<int64_t>(std::time(nullptr));

        // 1. Antigravity Hook Handling
        if (provider == "antigravity") {
            std::string rawConvId = firstString(payload, {"conversationId", "conversation_id", "session_id"});
            if (!rawConvId.empty() && !AgentTelemetryDb::isValidSessionId(rawConvId)) {
                return false;
            }
            recordAgyLsEndpointFromEnv(stateDir, payload);
            if (rawConvId.empty()) {
                rawConvId = loadCurrentSession(stateDir);
            } else {
                rememberCurrentSession(stateDir, rawConvId);
            }
            if (rawConvId.empty() || !AgentTelemetryDb::isValidSessionId(rawConvId)) {
                return true;
            }

            std::string agySid = (rawConvId.rfind("sess-", 0) == 0)
                ? rawConvId
                : ("sess-" + rawConvId.substr(0, 8));
            std::string workspace = extractWorkspace(payload, "antigravity");
            std::string model = extractModel(payload, "antigravity");

            if (eventKindOverride == "post-tool") {
                json state = loadSessionState(stateDir, agySid, workspace, model, "Antigravity", nowSec);
                bool failed = payload.contains("error") && payload["error"].is_string() &&
                              !trimString(payload["error"].get<std::string>()).empty();
                state["tools"] = state.value("tools", 0) + 1;
                if (failed) {
                    state["errors"] = state.value("errors", 0) + 1;
                }
                int step = state.value("step", 0) + 1;
                if (payload.contains("stepIdx") && payload["stepIdx"].is_number_integer()) {
                    step = payload["stepIdx"].get<int>();
                } else if (payload.contains("step_index") && payload["step_index"].is_number_integer()) {
                    step = payload["step_index"].get<int>();
                }
                state["step"] = step;
                saveSessionState(stateDir, "antigravity", state);

                std::string toolName = firstString(payload, {"toolName", "tool_name"});
                if (toolName.empty() && payload.contains("toolCall") && payload["toolCall"].is_object()) {
                    toolName = firstString(payload["toolCall"], {"name"});
                }
                if (toolName.empty()) toolName = "tool";

                AgentLifecycleEvent ev;
                ev.timestamp = nowSec;
                ev.sessionId = agySid;
                ev.agentType = "antigravity";
                ev.eventType = "TOOL_CALL";
                ev.stepIndex = step;
                ev.toolName = toolName;
                ev.durationMs = extractDurationMs(payload);
                ev.status = failed ? "ERROR" : "OK";
                ev.detailsJson = {{"workspace", workspace}, {"model", model}, {"conversation_id", rawConvId}};
                db.insertEvent(ev);
            } else if (eventKindOverride == "session-start") {
                db.recordSessionStart(agySid, rawConvId, "antigravity", workspace, model, nowSec);
            }
            return true;
        }

        // 2. Extract and strictly validate session_id / conversation_id
        std::string rawSessionId = firstString(payload, {"conversation_id", "session_id"});
        if (!rawSessionId.empty()) {
            if (!AgentTelemetryDb::isValidSessionId(rawSessionId)) {
                return false;
            }
            if (provider == "cursor" && !isCursorSubagentSession(rawSessionId)) {
                rememberCurrentSession(stateDir, rawSessionId);
            }
        } else if (provider == "cursor") {
            rawSessionId = loadCurrentSession(stateDir);
        }

        if (rawSessionId.empty() || !AgentTelemetryDb::isValidSessionId(rawSessionId)) {
            return false;
        }

        const std::string cid = rawSessionId;
        std::string workspace = extractWorkspace(payload, provider);
        std::string model = extractModel(payload, provider);
        std::string isoTs = formatIsoTimestampNow();
        std::string jsonlPath = stateDir + "/" + cid + ".jsonl";

        // 3. Claude Hook Events
        if (provider == "claude") {
            json state = loadSessionState(stateDir, cid, workspace, model, "Claude", nowSec);
            auto ensureStarted = [&]() {
                if (!state.value("started", false)) {
                    db.recordSessionStart(cid, cid, "claude",
                                          state.value("workspace", ""),
                                          state.value("model", "Claude"),
                                          state.value("start_ts", nowSec));
                    state["started"] = true;
                }
            };

            if (eventName == "SessionStart") {
                bool isFresh = (state.value("turns", 0) == 0 &&
                                state.value("first_prompt", "").empty());
                bool wasStarted = state.value("started", false);
                state["started"] = true;
                if (isFresh) {
                    state["status"] = "IDLE";
                } else if (state.value("status", "") != "RUNNING") {
                    state["status"] = "COMPLETED";
                }
                saveSessionState(stateDir, "claude", state);
                if (isFresh && !wasStarted) {
                    db.recordSessionStart(cid, cid, "claude",
                                          state.value("workspace", ""),
                                          state.value("model", "Claude"),
                                          state.value("start_ts", nowSec));
                    db.touchSession(cid, "IDLE", nowSec);
                } else if (isFresh) {
                    db.touchSession(cid, "IDLE", nowSec);
                } else {
                    db.touchSession(cid, state.value("status", "COMPLETED"), nowSec);
                }
            } else if (eventName == "UserPromptSubmit") {
                ensureStarted();
                state["prompt_ts"] = nowSec;
                state["status"] = "RUNNING";
                std::string prompt = AgentTelemetryDb::scrubSecrets(
                    firstString(payload, {"prompt", "text", "user_prompt"}));
                if (!prompt.empty()) {
                    std::string summaryPrompt = prompt.size() > 500 ? prompt.substr(0, 500) : prompt;
                    if (state.value("first_prompt", "").empty()) {
                        state["first_prompt"] = summaryPrompt;
                    }
                    state["last_prompt"] = summaryPrompt;
                }
                saveSessionState(stateDir, "claude", state);
                db.touchSession(cid, "RUNNING", nowSec);
            } else if (eventName == "PreToolUse") {
                ensureStarted();
                state["status"] = "RUNNING";
                saveSessionState(stateDir, "claude", state);
                db.touchSession(cid, "RUNNING", nowSec);
            } else if (eventName == "PostToolUse" || eventName == "PostToolUseFailure") {
                ensureStarted();
                bool failed = (eventName == "PostToolUseFailure");
                std::string toolUseId = firstString(payload, {"tool_use_id"});
                json& toolIds = state["tool_ids"];
                bool alreadySeen = false;
                if (!toolUseId.empty()) {
                    for (const auto& existingId : toolIds) {
                        if (existingId.is_string() && existingId.get<std::string>() == toolUseId) {
                            alreadySeen = true;
                            break;
                        }
                    }
                }
                if (alreadySeen) {
                    saveSessionState(stateDir, "claude", state);
                    return true;
                }
                if (!toolUseId.empty()) {
                    toolIds.push_back(toolUseId);
                    if (toolIds.size() > 400) {
                        toolIds.erase(toolIds.begin(), toolIds.begin() + (toolIds.size() - 400));
                    }
                }
                int step = state.value("step", 0) + 1;
                int tools = state.value("tools", 0) + 1;
                int errors = state.value("errors", 0) + (failed ? 1 : 0);
                state["step"] = step;
                state["tools"] = tools;
                state["errors"] = errors;
                state["status"] = "RUNNING";
                saveSessionState(stateDir, "claude", state);

                std::string toolName = firstString(payload, {"tool_name"});
                if (toolName.empty()) toolName = "tool";
                json safeInput = extractSafeToolInput(payload);

                AgentLifecycleEvent ev;
                ev.timestamp = nowSec;
                ev.sessionId = cid;
                ev.agentType = "claude";
                ev.eventType = "TOOL_CALL";
                ev.stepIndex = step;
                ev.toolName = toolName;
                ev.durationMs = extractDurationMs(payload);
                ev.status = failed ? "ERROR" : "OK";
                ev.detailsJson = {
                    {"workspace", state.value("workspace", "")},
                    {"model", state.value("model", "Claude")},
                    {"input", safeInput}
                };
                db.insertEvent(ev);
                db.updateSessionStats(cid, "RUNNING",
                                      state.value("turns", 0), 0, 0,
                                      tools, errors, 0.0,
                                      state.value("start_ts", nowSec), nowSec);
            } else if (eventName == "Stop") {
                ensureStarted();
                int64_t promptTs = state.value("prompt_ts", int64_t(0));
                double turnMsTotal = state.value("turn_ms_total", 0.0);
                if (promptTs > 0 && nowSec >= promptTs) {
                    turnMsTotal += static_cast<double>(nowSec - promptTs) * 1000.0;
                }
                state["turn_ms_total"] = turnMsTotal;
                state["prompt_ts"] = 0;
                int turns = state.value("turns", 0) + 1;
                int step = state.value("step", 0) + 1;
                state["turns"] = turns;
                state["step"] = step;
                state["status"] = "COMPLETED";
                saveSessionState(stateDir, "claude", state);

                AgentLifecycleEvent ev;
                ev.timestamp = nowSec;
                ev.sessionId = cid;
                ev.agentType = "claude";
                ev.eventType = "USER_TURN";
                ev.stepIndex = step;
                ev.status = "OK";
                ev.detailsJson = {
                    {"workspace", state.value("workspace", "")},
                    {"model", state.value("model", "Claude")}
                };
                db.insertEvent(ev);

                double avgTurnMs = (turns > 0) ? (turnMsTotal / turns) : 0.0;
                db.updateSessionStats(cid, "COMPLETED",
                                      turns, 0, 0,
                                      state.value("tools", 0),
                                      state.value("errors", 0),
                                      avgTurnMs,
                                      state.value("start_ts", nowSec), nowSec);
            } else if (eventName == "SessionEnd") {
                auto ppidTokens = readProcNulTokens("/proc/" + std::to_string(::getppid()) + "/cmdline");
                if (isOneShotTokens(ppidTokens)) {
                    int ownerPid = state.value("owner_pid", 0);
                    if (isInteractiveCliProcess(ownerPid, "claude", nullptr) ||
                        hasLiveInteractiveOwnerProcess(cid, "claude")) {
                        return true;
                    }
                }
                ensureStarted();
                state["status"] = "COMPLETED";
                saveSessionState(stateDir, "claude", state);
                int turns = state.value("turns", 0);
                double turnMsTotal = state.value("turn_ms_total", 0.0);
                double avgTurnMs = (turns > 0) ? (turnMsTotal / turns) : 0.0;
                db.recordSessionEnd(cid, "COMPLETED", turns, 0, 0,
                                    state.value("tools", 0),
                                    state.value("errors", 0),
                                    avgTurnMs, nowSec);
            }
            return true;
        }

        // 4. Cursor Hook Events
        json state = loadSessionState(stateDir, cid, workspace, model, "Cursor", nowSec);

        if (eventName == "sessionStart") {
            bool isFresh = (state.value("turns", 0) == 0 &&
                            state.value("first_prompt", "").empty());
            bool wasStarted = state.value("started", false);
            state["started"] = true;
            if (isFresh) {
                state["status"] = "IDLE";
            } else if (state.value("status", "") != "RUNNING") {
                state["status"] = "COMPLETED";
            }
            saveSessionState(stateDir, "cursor", state);
            if (isFresh && !wasStarted) {
                db.recordSessionStart(cid, cid, "cursor",
                                      state.value("workspace", ""),
                                      state.value("model", "Cursor"),
                                      state.value("start_ts", nowSec));
                db.touchSession(cid, "IDLE", nowSec);
            } else if (isFresh) {
                db.touchSession(cid, "IDLE", nowSec);
            } else {
                db.touchSession(cid, state.value("status", "COMPLETED"), nowSec);
            }
        } else if (eventName == "beforeSubmitPrompt") {
            std::string rawPrompt = firstString(payload, {"prompt", "text", "user_prompt"});
            std::string scrubbedPrompt = AgentTelemetryDb::scrubSecrets(rawPrompt);
            if (scrubbedPrompt.size() > 4096) {
                scrubbedPrompt = scrubbedPrompt.substr(0, 4096);
            }
            if (!scrubbedPrompt.empty()) {
                std::string shortPrompt = scrubbedPrompt.size() > 500
                    ? scrubbedPrompt.substr(0, 500)
                    : scrubbedPrompt;
                if (state.value("first_prompt", "").empty()) {
                    state["first_prompt"] = shortPrompt;
                }
                state["last_prompt"] = shortPrompt;
            }
            bool wasStarted = state.value("started", false);
            state["status"] = "RUNNING";
            state["started"] = true;
            int step = state.value("step", 0) + 1;
            state["step"] = step;
            saveSessionState(stateDir, "cursor", state);

            if (!scrubbedPrompt.empty()) {
                json userLine = {
                    {"role", "user"},
                    {"message", {
                        {"content", json::array({
                            {{"type", "text"}, {"text", scrubbedPrompt}}
                        })}
                    }},
                    {"timestamp", isoTs}
                };
                appendAtomicJsonl0600(stateDir, jsonlPath, userLine);
            }

            if (!wasStarted && state.value("turns", 0) == 0 && state.value("tools", 0) == 0) {
                db.recordSessionStart(cid, cid, "cursor",
                                      state.value("workspace", ""),
                                      state.value("model", "Cursor"),
                                      state.value("start_ts", nowSec));
            }

            AgentLifecycleEvent ev;
            ev.timestamp = nowSec;
            ev.sessionId = cid;
            ev.agentType = "cursor";
            ev.eventType = "USER_TURN";
            ev.stepIndex = step;
            ev.status = "OK";
            ev.detailsJson = {
                {"workspace", state.value("workspace", "")},
                {"model", state.value("model", "Cursor")},
                {"prompt", state.value("last_prompt", "")}
            };
            db.insertEvent(ev);
            db.touchSession(cid, "RUNNING", nowSec);
        } else if (eventName == "preToolUse" || eventName == "PreToolUse") {
            state["status"] = "RUNNING";
            saveSessionState(stateDir, "cursor", state);
            db.touchSession(cid, "RUNNING", nowSec);
        } else if (eventName == "afterAgentThought") {
            std::string rawText = firstString(payload, {"text", "thought", "thinking"});
            std::string scrubbedText = AgentTelemetryDb::scrubSecrets(rawText);
            state["status"] = "RUNNING";
            saveSessionState(stateDir, "cursor", state);
            if (!scrubbedText.empty()) {
                json thoughtLine = {
                    {"role", "assistant"},
                    {"message", {
                        {"content", json::array({
                            {{"type", "thinking"}, {"thinking", scrubbedText}}
                        })}
                    }},
                    {"timestamp", isoTs}
                };
                appendAtomicJsonl0600(stateDir, jsonlPath, thoughtLine);
            }
        } else if (eventName == "afterAgentResponse") {
            std::string rawText = firstString(payload, {"text", "response", "message"});
            std::string scrubbedText = AgentTelemetryDb::scrubSecrets(rawText);
            state["status"] = "RUNNING";
            saveSessionState(stateDir, "cursor", state);
            if (!scrubbedText.empty()) {
                json respLine = {
                    {"role", "assistant"},
                    {"message", {
                        {"content", json::array({
                            {{"type", "text"}, {"text", scrubbedText}}
                        })}
                    }},
                    {"timestamp", isoTs}
                };
                appendAtomicJsonl0600(stateDir, jsonlPath, respLine);
            }
        } else if (eventName == "postToolUse" || eventName == "postToolUseFailure" ||
                   eventName == "afterShellExecution" || eventName == "afterMCPExecution") {
            bool failed = (eventName == "postToolUseFailure");
            std::string toolUseId = firstString(payload, {"tool_use_id", "id"});
            json& toolIds = state["tool_ids"];
            bool alreadySeen = false;
            if (!toolUseId.empty()) {
                for (const auto& existingId : toolIds) {
                    if (existingId.is_string() && existingId.get<std::string>() == toolUseId) {
                        alreadySeen = true;
                        break;
                    }
                }
            }
            if (alreadySeen) {
                saveSessionState(stateDir, "cursor", state);
                return true;
            }
            if (!toolUseId.empty()) {
                toolIds.push_back(toolUseId);
                if (toolIds.size() > 400) {
                    toolIds.erase(toolIds.begin(), toolIds.begin() + (toolIds.size() - 400));
                }
            }

            int step = state.value("step", 0) + 1;
            int tools = state.value("tools", 0) + 1;
            int errors = state.value("errors", 0) + (failed ? 1 : 0);
            state["step"] = step;
            state["tools"] = tools;
            state["errors"] = errors;
            state["status"] = "RUNNING";
            saveSessionState(stateDir, "cursor", state);

            std::string toolName = firstString(payload, {"tool_name"});
            if (toolName.empty()) {
                if (eventName == "afterShellExecution") toolName = "Shell";
                else if (eventName == "afterMCPExecution") toolName = "MCP";
                else toolName = "tool";
            }

            json safeInput = extractSafeToolInput(payload);
            std::string effectiveToolId = toolUseId.empty()
                ? ("tool-" + std::to_string(step))
                : toolUseId;

            json toolLine = {
                {"role", "assistant"},
                {"message", {
                    {"content", json::array({
                        {
                            {"type", "tool_use"},
                            {"id", effectiveToolId},
                            {"name", toolName},
                            {"input", safeInput}
                        }
                    })}
                }},
                {"timestamp", isoTs}
            };
            appendAtomicJsonl0600(stateDir, jsonlPath, toolLine);

            AgentLifecycleEvent ev;
            ev.timestamp = nowSec;
            ev.sessionId = cid;
            ev.agentType = "cursor";
            ev.eventType = "TOOL_CALL";
            ev.stepIndex = step;
            ev.toolName = toolName;
            ev.durationMs = extractDurationMs(payload);
            ev.status = failed ? "ERROR" : "OK";
            ev.detailsJson = {
                {"workspace", state.value("workspace", "")},
                {"model", state.value("model", "Cursor")},
                {"input", safeInput}
            };
            db.insertEvent(ev);
            db.updateSessionStats(cid, "RUNNING",
                                   std::max(1, state.value("turns", 0)),
                                  state.value("prompt_tokens", 0),
                                  state.value("comp_tokens", 0),
                                  tools, errors, 0.0,
                                  state.value("start_ts", nowSec), nowSec);
        } else if (eventName == "subagentStart" || eventName == "subagentStop") {
            int activeSubs = state.value("active_subagents", 0);
            if (eventName == "subagentStart") {
                activeSubs += 1;
                state["status"] = "RUNNING";
            } else {
                activeSubs = std::max(0, activeSubs - 1);
            }
            state["active_subagents"] = activeSubs;
            saveSessionState(stateDir, "cursor", state);

            std::string subId = firstString(payload, {"subagent_id", "subagentId", "agent_id"});
            if (AgentTelemetryDb::isValidSessionId(subId) && subId != cid) {
                json subState = loadSessionState(stateDir, subId, workspace, model, "Cursor", nowSec);
                subState["is_subagent"] = true;
                subState["parent_session_id"] = cid;
                if (eventName == "subagentStop") {
                    subState["status"] = "COMPLETED";
                }
                saveSessionState(stateDir, "cursor", subState);
            }
        } else if (eventName == "stop") {
            int turns = state.value("turns", 0) + 1;
            int step = state.value("step", 0) + 1;
            state["turns"] = turns;
            state["step"] = step;
            if (payload.contains("input_tokens") && payload["input_tokens"].is_number()) {
                int64_t v = payload["input_tokens"].get<int64_t>();
                if (v > 0) state["prompt_tokens"] = state.value("prompt_tokens", 0) + static_cast<int>(v);
            }
            if (payload.contains("output_tokens") && payload["output_tokens"].is_number()) {
                int64_t v = payload["output_tokens"].get<int64_t>();
                if (v > 0) state["comp_tokens"] = state.value("comp_tokens", 0) + static_cast<int>(v);
            }

            std::string finalStatus = (state.value("active_subagents", 0) > 0)
                ? "RUNNING"
                : "COMPLETED";
            state["status"] = finalStatus;
            saveSessionState(stateDir, "cursor", state);

            json turnEndLine = {
                {"type", "turn_ended"},
                {"timestamp", isoTs}
            };
            appendAtomicJsonl0600(stateDir, jsonlPath, turnEndLine);

            AgentLifecycleEvent ev;
            ev.timestamp = nowSec;
            ev.sessionId = cid;
            ev.agentType = "cursor";
            ev.eventType = "USER_TURN";
            ev.stepIndex = step;
            ev.status = "OK";
            ev.detailsJson = {
                {"workspace", state.value("workspace", "")},
                {"model", state.value("model", "Cursor")}
            };
            db.insertEvent(ev);

            int64_t startTs = state.value("start_ts", nowSec);
            double elapsedMs = extractDurationMs(payload);
            if (elapsedMs <= 0.0 && nowSec >= startTs) {
                elapsedMs = static_cast<double>(nowSec - startTs) * 1000.0;
            }
            double avgTurnMs = (turns > 0) ? (elapsedMs / turns) : elapsedMs;

            db.updateSessionStats(cid, finalStatus, turns,
                                  state.value("prompt_tokens", 0),
                                  state.value("comp_tokens", 0),
                                  state.value("tools", 0),
                                  state.value("errors", 0),
                                  avgTurnMs, startTs, nowSec);
        } else if (eventName == "sessionEnd") {
            auto ppidTokens = readProcNulTokens("/proc/" + std::to_string(::getppid()) + "/cmdline");
            if (isOneShotTokens(ppidTokens)) {
                int ownerPid = state.value("owner_pid", 0);
                if (isInteractiveCliProcess(ownerPid, "cursor", nullptr) ||
                    hasLiveInteractiveOwnerProcess(cid, "cursor")) {
                    return true;
                }
            }

            std::string reason = firstString(payload, {"reason", "final_status", "status"});
            std::transform(reason.begin(), reason.end(), reason.begin(), ::tolower);
            std::string finalStatus = "COMPLETED";
            if (reason == "error") {
                finalStatus = "ERROR";
            } else if (reason == "aborted" || reason == "user_close" || reason == "window_close") {
                finalStatus = "ABORTED";
            }

            state["status"] = finalStatus;
            saveSessionState(stateDir, "cursor", state);

            json turnEndLine = {
                {"type", "turn_ended"},
                {"timestamp", isoTs}
            };
            appendAtomicJsonl0600(stateDir, jsonlPath, turnEndLine);

            int turns = state.value("turns", 0);
            int64_t startTs = state.value("start_ts", nowSec);
            double elapsedMs = extractDurationMs(payload);
            if (elapsedMs <= 0.0 && nowSec >= startTs) {
                elapsedMs = static_cast<double>(nowSec - startTs) * 1000.0;
            }
            double avgTurnMs = (turns > 0) ? (elapsedMs / turns) : elapsedMs;

            db.recordSessionEnd(cid, finalStatus, turns,
                                state.value("prompt_tokens", 0),
                                state.value("comp_tokens", 0),
                                state.value("tools", 0),
                                state.value("errors", 0),
                                avgTurnMs, nowSec);
        }

        return true;
    } catch (...) {
        return false;
    }
}

int AgentHookHandler::run(int argc, char* argv[], std::istream& inStream, std::ostream& outStream,
                          const std::string& customStateDir,
                          const std::string& customDbPath) {
    std::string forcedProvider;
    std::string eventKindOverride;
    std::string stateDir = customStateDir;
    std::string dbPath = customDbPath;
    bool registerLsOnly = false;

    try {
        for (int i = 0; i < argc; ++i) {
            if (!argv[i]) continue;
            std::string arg = argv[i];
            if (arg == "hook" || arg == "aimon" ||
                arg.rfind("/aimon", 0) != std::string::npos ||
                arg.find("aimon_telemetry") != std::string::npos) {
                continue;
            } else if (arg == "cursor" || arg == "claude" || arg == "antigravity") {
                forcedProvider = arg;
            } else if (arg == "--event" && (i + 1) < argc && argv[i + 1]) {
                eventKindOverride = argv[++i];
            } else if (arg == "--register-ls-only") {
                registerLsOnly = true;
            } else if (arg == "--state-dir" && (i + 1) < argc && argv[i + 1]) {
                stateDir = argv[++i];
            } else if (arg == "--db" && (i + 1) < argc && argv[i + 1]) {
                dbPath = argv[++i];
            }
        }

        if (registerLsOnly) {
            std::string effStateDir = resolveStateDir("antigravity", stateDir);
            recordAgyLsEndpointFromEnv(effStateDir, json::object());
            return 0;
        }

        std::ostringstream ss;
        ss << inStream.rdbuf();
        std::string raw = trimString(ss.str());
        json parsedPayload = json::object();

        if (raw.empty()) {
            if (!eventKindOverride.empty()) {
                std::string effStateDir = resolveStateDir("antigravity", stateDir);
                recordAgyLsEndpointFromEnv(effStateDir, json::object());
            }
        } else {
            json payload = json::parse(raw, nullptr, false);
            if (payload.is_object()) {
                parsedPayload = payload;
                processPayload(payload, forcedProvider, eventKindOverride, stateDir, dbPath);
            }
        }

        if (eventKindOverride == "stop" || eventKindOverride == "pre-tool") {
            outStream << "{\"decision\":\"allow\"}\n";
        } else {
            outStream << "{}\n";
        }
        outStream.flush();

        if (eventKindOverride == "pre-tool") {
            std::string effStateDir = resolveStateDir("antigravity", stateDir);
            spawnAgyChildEnvWatcher(effStateDir, parsedPayload);
        }
        return 0;
    } catch (...) {
        // Never allow an exception to escape hook execution
    }

    if (eventKindOverride == "stop" || eventKindOverride == "pre-tool") {
        outStream << "{\"decision\":\"allow\"}\n";
    } else {
        outStream << "{}\n";
    }
    outStream.flush();
    return 0;
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
