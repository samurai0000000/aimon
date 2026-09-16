/*
 * AgentRunner.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "AgentRunner.hxx"
#include "StateStore.hxx"
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <chrono>
#include <vector>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <iostream>

namespace fs = std::filesystem;

namespace aimon {

static void replaceAll(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t startPos = 0;
    while ((startPos = str.find(from, startPos)) != std::string::npos) {
        str.replace(startPos, from.length(), to);
        startPos += to.length();
    }
}

AgentRunner::AgentRunner(const CollaborationConfig& config)
    : _config(config) {
}

void AgentRunner::updateConfig(const CollaborationConfig& config) {
    _config = config;
}

bool AgentRunner::isAvailable(const std::string& agentId) const {
    auto it = _config.runners.find(agentId);
    if (it != _config.runners.end()) {
        if (!it->second.scriptBridge.empty()) {
            std::string script = it->second.scriptBridge;
            size_t space = script.find(' ');
            if (space != std::string::npos) {
                script = script.substr(0, space);
            }
            if (fs::exists(script)) return true;
        }
    }

    if (agentId == "agent-antigravity-builder") {
        if (fs::exists("/usr/bin/gemini")) return true;
        const char* path = std::getenv("PATH");
        if (path) {
            std::string pStr(path);
            std::stringstream ss(pStr);
            std::string dir;
            while (std::getline(ss, dir, ':')) {
                if (fs::exists(fs::path(dir) / "gemini")) return true;
            }
        }
        return false;
    }

    if (agentId == "agent-cursor-windows") {
        const char* home = std::getenv("HOME");
        if (home && fs::exists(fs::path(home) / ".local/bin/cursor-agent")) return true;
        if (fs::exists("/usr/bin/cursor")) return true;
        const char* path = std::getenv("PATH");
        if (path) {
            std::string pStr(path);
            std::stringstream ss(pStr);
            std::string dir;
            while (std::getline(ss, dir, ':')) {
                if (fs::exists(fs::path(dir) / "cursor-agent")) return true;
            }
        }
        return false;
    }

    return false;
}

bool AgentRunner::checkQuotaOk(const std::string& agentId, std::string& outReason) const {
    if (agentId.find("cursor") != std::string::npos) {
        CursorStatus st = StateStore::getInstance().getStatus().cursor;
        if (st.fastRequestsLimit > 0) {
            int remaining = st.fastRequestsLimit - st.fastRequestsUsed;
            if (remaining < _config.minCursorQuota) {
                outReason = "Cursor fast requests remaining (" + std::to_string(remaining) +
                            ") is below minimum threshold (" + std::to_string(_config.minCursorQuota) + ")";
                return false;
            }
        }
    }
    return true;
}

int AgentRunner::timeoutForAgent(const std::string& agentId, int fallbackSec) const {
    auto it = _config.runners.find(agentId);
    if (it != _config.runners.end() && it->second.timeoutSeconds > 0) {
        return it->second.timeoutSeconds;
    }
    return fallbackSec;
}

bool AgentRunner::isUnavailabilityFailure(const AgentRunResult& res, std::string& outReason) {
    if (res.ok || res.timedOut) {
        return false;
    }

    if (res.exitCode == 127) {
        outReason = "runner binary not found or not executable";
        return true;
    }

    std::string blob = res.stderrText + "\n" + res.stdoutText;
    for (auto& c : blob) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    static const struct { const char* needle; const char* reason; } signatures[] = {
        {"authentication required", "runner is not authenticated"},
        {"please run 'agent login'", "runner is not authenticated"},
        {"not logged in", "runner is not authenticated"},
        {"unauthorized", "runner credentials were rejected"},
        {"invalid api key", "runner credentials were rejected"},
        {"token expired", "runner credentials expired"},
        {"rate limit", "runner is rate limited"},
        {"quota exceeded", "runner quota is exhausted"},
        {"command not found", "runner binary not found"},
    };

    for (const auto& sig : signatures) {
        if (blob.find(sig.needle) != std::string::npos) {
            outReason = sig.reason;
            return true;
        }
    }

    return false;
}

std::string AgentRunner::resolveCommand(const AgentRunSpec& spec, bool& outIsScriptBridge) const {
    outIsScriptBridge = false;
    auto it = _config.runners.find(spec.agentId);
    if (it != _config.runners.end()) {
        const auto& r = it->second;
        if (!r.scriptBridge.empty()) {
            outIsScriptBridge = true;
            return r.scriptBridge;
        }
        if (spec.writeEnabled && !r.writeCommand.empty()) {
            return r.writeCommand;
        }
        if (!spec.writeEnabled && !r.readCommand.empty()) {
            return r.readCommand;
        }
    }

    // Default command templates if not configured
    if (spec.agentId == "agent-antigravity-builder") {
        if (spec.writeEnabled) {
            return "gemini -p \"{prompt}\" --approval-mode yolo --skip-trust";
        } else {
            return "gemini -p \"{prompt}\" --approval-mode plan --skip-trust";
        }
    }

    if (spec.agentId == "agent-cursor-windows") {
        std::string cursorBin = "cursor-agent";
        const char* home = std::getenv("HOME");
        if (home && fs::exists(fs::path(home) / ".local/bin/cursor-agent")) {
            cursorBin = std::string(home) + "/.local/bin/cursor-agent";
        }
        if (spec.writeEnabled) {
            return cursorBin + " -p --force --trust --workspace {ws} \"{prompt}\"";
        } else {
            return cursorBin + " -p --mode=plan --trust --workspace {ws} \"{prompt}\"";
        }
    }

    return "";
}

AgentRunResult AgentRunner::run(const AgentRunSpec& spec) const {
    AgentRunResult result;
    auto startTime = std::chrono::steady_clock::now();

    bool isScriptBridge = false;
    std::string cmdTemplate = resolveCommand(spec, isScriptBridge);
    if (cmdTemplate.empty()) {
        result.ok = false;
        result.stderrText = "No runner command configured for agent: " + spec.agentId;
        return result;
    }

    std::string ws = spec.workspace.empty() ? "." : spec.workspace;
    std::string finalCmd = cmdTemplate;
    replaceAll(finalCmd, "{ws}", ws);
    replaceAll(finalCmd, "{plan}", spec.planFile);
    replaceAll(finalCmd, "{turn}", std::to_string(spec.turnNumber));
    replaceAll(finalCmd, "{role}", spec.role);

    bool embedPrompt = (finalCmd.find("{prompt}") != std::string::npos);
    if (embedPrompt) {
        // Escape double quotes and backslashes in prompt for safe shell embedding
        std::string escPrompt;
        escPrompt.reserve(spec.prompt.size() * 2);
        for (char c : spec.prompt) {
            if (c == '"' || c == '\\' || c == '$' || c == '`') {
                escPrompt.push_back('\\');
            }
            escPrompt.push_back(c);
        }
        replaceAll(finalCmd, "{prompt}", escPrompt);
    }

    int inPipe[2];
    int outPipe[2];
    int errPipe[2];

    if (pipe(inPipe) != 0 || pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        result.ok = false;
        result.stderrText = "Failed to create pipes for subprocess";
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        result.ok = false;
        result.stderrText = "Failed to fork subprocess";
        return result;
    }

    if (pid == 0) {
        // Child process: set process group
        setpgid(0, 0);

        close(inPipe[1]);
        dup2(inPipe[0], STDIN_FILENO);
        close(inPipe[0]);

        close(outPipe[0]);
        dup2(outPipe[1], STDOUT_FILENO);
        close(outPipe[1]);

        close(errPipe[0]);
        dup2(errPipe[1], STDERR_FILENO);
        close(errPipe[1]);

        if (!spec.workspace.empty()) {
            chdir(spec.workspace.c_str());
        }

        // Set environment variables
        setenv("AIMON_PLAN_FILE", spec.planFile.c_str(), 1);
        setenv("AIMON_TURN", std::to_string(spec.turnNumber).c_str(), 1);
        setenv("AIMON_ROLE", spec.role.c_str(), 1);
        setenv("AIMON_AGENT_ID", spec.agentId.c_str(), 1);
        setenv("AIMON_WRITE", spec.writeEnabled ? "1" : "0", 1);

        execlp("/bin/bash", "bash", "-c", finalCmd.c_str(), nullptr);
        _exit(127);
    }

    // Parent process
    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);

    // Feed prompt to child via stdin if not embedded in argv
    if (!embedPrompt && !spec.prompt.empty()) {
        ssize_t written = write(inPipe[1], spec.prompt.c_str(), spec.prompt.size());
        (void)written;
    }
    close(inPipe[1]);

    // Make read ends non-blocking
    fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

    int timeoutSec = spec.timeoutSec > 0 ? spec.timeoutSec : 300;
    auto deadline = startTime + std::chrono::seconds(timeoutSec);

    bool outClosed = false;
    bool errClosed = false;
    char buffer[4096];

    while (!outClosed || !errClosed) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            result.timedOut = true;
            // Send SIGTERM to entire process group
            kill(-pid, SIGTERM);

            // Give grace period (2 seconds)
            auto killDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            bool reaped = false;
            while (std::chrono::steady_clock::now() < killDeadline) {
                int status = 0;
                pid_t r = waitpid(pid, &status, WNOHANG);
                if (r > 0) {
                    reaped = true;
                    break;
                }
                usleep(50000);
            }
            if (!reaped) {
                kill(-pid, SIGKILL);
                waitpid(pid, nullptr, 0);
            }
            break;
        }

        struct pollfd pfd[2];
        int nfds = 0;
        int outIdx = -1;
        int errIdx = -1;

        if (!outClosed) {
            outIdx = nfds++;
            pfd[outIdx].fd = outPipe[0];
            pfd[outIdx].events = POLLIN;
        }
        if (!errClosed) {
            errIdx = nfds++;
            pfd[errIdx].fd = errPipe[0];
            pfd[errIdx].events = POLLIN;
        }

        int pollRet = poll(pfd, nfds, 100);
        if (pollRet > 0) {
            if (outIdx >= 0 && (pfd[outIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
                ssize_t n = read(outPipe[0], buffer, sizeof(buffer));
                if (n > 0) {
                    if (result.stdoutText.size() < spec.maxOutputBytes) {
                        result.stdoutText.append(buffer, n);
                    }
                } else if (n == 0) {
                    outClosed = true;
                }
            }
            if (errIdx >= 0 && (pfd[errIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
                ssize_t n = read(errPipe[0], buffer, sizeof(buffer));
                if (n > 0) {
                    if (result.stderrText.size() < spec.maxOutputBytes) {
                        result.stderrText.append(buffer, n);
                    }
                } else if (n == 0) {
                    errClosed = true;
                }
            }
        }
    }

    close(outPipe[0]);
    close(errPipe[0]);

    int status = 0;
    if (!result.timedOut) {
        waitpid(pid, &status, 0);
        result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        result.ok = (result.exitCode == 0);
    } else {
        result.exitCode = -1;
        result.ok = false;
    }

    auto endTime = std::chrono::steady_clock::now();
    result.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    return result;
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
