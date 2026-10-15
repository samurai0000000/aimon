/*
 * ProcessLauncher.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "ProcessLauncher.hxx"
#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace aimon {

#ifdef _WIN32

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len);
    return out;
}

std::wstring quoteWinArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring quoted = L"\"";
    for (auto it = arg.begin(); ; ++it) {
        size_t numBackslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++numBackslashes;
        }
        if (it == arg.end()) {
            quoted.append(numBackslashes * 2, L'\\');
            break;
        } else if (*it == L'"') {
            quoted.append(numBackslashes * 2 + 1, L'\\');
            quoted.push_back(*it);
        } else {
            quoted.append(numBackslashes, L'\\');
            quoted.push_back(*it);
        }
    }
    quoted.push_back(L'"');
    return quoted;
}

} // anonymous namespace

ProcessResult ProcessLauncher::run(const std::string& executable,
                                   const std::vector<std::string>& args,
                                   int timeoutMs,
                                   size_t maxOutputBytes,
                                   const std::string& workingDir) {
    ProcessResult res;
    if (executable.empty()) {
        res.errorMessage = "Empty executable path";
        return res;
    }

    auto startTp = std::chrono::steady_clock::now();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr, outWrite = nullptr;
    HANDLE errRead = nullptr, errWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0) ||
        !SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0)) {
        res.errorMessage = "Failed to create stdout pipe";
        return res;
    }
    if (!CreatePipe(&errRead, &errWrite, &sa, 0) ||
        !SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(outRead);
        CloseHandle(outWrite);
        res.errorMessage = "Failed to create stderr pipe";
        return res;
    }

    std::wstring wExe = utf8ToWide(executable);
    std::wstring cmdLine = quoteWinArg(wExe);
    for (const auto& a : args) {
        cmdLine.push_back(L' ');
        cmdLine += quoteWinArg(utf8ToWide(a));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring wDir = utf8ToWide(workingDir);
    LPCWSTR lpDir = wDir.empty() ? nullptr : wDir.c_str();

    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    BOOL ok = CreateProcessW(
        wExe.c_str(),
        mutableCmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        lpDir,
        &si,
        &pi);

    CloseHandle(outWrite);
    CloseHandle(errWrite);

    if (!ok) {
        CloseHandle(outRead);
        CloseHandle(errRead);
        if (hJob) CloseHandle(hJob);
        res.errorMessage = "CreateProcessW failed for: " + executable;
        return res;
    }

    res.launched = true;
    if (hJob) {
        AssignProcessToJobObject(hJob, pi.hProcess);
    }

    auto drainPipe = [&](HANDLE hPipe, std::string& dst) {
        DWORD avail = 0;
        while (PeekNamedPipe(hPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[4096];
            DWORD toRead = (avail < sizeof(buf)) ? avail : static_cast<DWORD>(sizeof(buf));
            DWORD bytesRead = 0;
            if (!ReadFile(hPipe, buf, toRead, &bytesRead, nullptr) || bytesRead == 0) break;
            if (dst.size() < maxOutputBytes) {
                size_t room = maxOutputBytes - dst.size();
                if (bytesRead > room) {
                    dst.append(buf, room);
                    res.truncated = true;
                } else {
                    dst.append(buf, bytesRead);
                }
            } else {
                res.truncated = true;
            }
        }
    };

    while (true) {
        drainPipe(outRead, res.stdoutText);
        drainPipe(errRead, res.stderrText);

        DWORD waitRes = WaitForSingleObject(pi.hProcess, 20);
        if (waitRes == WAIT_OBJECT_0) {
            drainPipe(outRead, res.stdoutText);
            drainPipe(errRead, res.stderrText);
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTp).count();
        if (timeoutMs > 0 && elapsed >= timeoutMs) {
            res.timedOut = true;
            if (hJob) {
                TerminateJobObject(hJob, 137);
            } else {
                TerminateProcess(pi.hProcess, 137);
            }
            WaitForSingleObject(pi.hProcess, 1000);
            break;
        }
    }

    DWORD exitCode = 0;
    if (GetExitCodeProcess(pi.hProcess, &exitCode)) {
        res.exitCode = static_cast<int>(exitCode);
    }

    CloseHandle(outRead);
    CloseHandle(errRead);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (hJob) CloseHandle(hJob);

    res.durationMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTp).count());
    if (res.timedOut) {
        res.errorMessage = "Process timed out after " + std::to_string(timeoutMs) + " ms";
    }
    return res;
}

#else // POSIX

ProcessResult ProcessLauncher::run(const std::string& executable,
                                   const std::vector<std::string>& args,
                                   int timeoutMs,
                                   size_t maxOutputBytes,
                                   const std::string& workingDir) {
    ProcessResult res;
    if (executable.empty() || executable.find('\0') != std::string::npos) {
        res.errorMessage = "Invalid or empty executable path";
        return res;
    }

    auto startTp = std::chrono::steady_clock::now();

    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (::pipe(outPipe) != 0) {
        res.errorMessage = std::string("pipe(stdout) failed: ") + ::strerror(errno);
        return res;
    }
    if (::pipe(errPipe) != 0) {
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        res.errorMessage = std::string("pipe(stderr) failed: ") + ::strerror(errno);
        return res;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        res.errorMessage = std::string("fork() failed: ") + ::strerror(errno);
        return res;
    }

    if (pid == 0) {
        // Child process: create dedicated process group so timeout kills the entire subtree
        ::setpgid(0, 0);
        ::close(outPipe[0]);
        ::close(errPipe[0]);
        ::dup2(outPipe[1], STDOUT_FILENO);
        ::dup2(errPipe[1], STDERR_FILENO);
        ::close(outPipe[1]);
        ::close(errPipe[1]);

        if (!workingDir.empty()) {
            if (::chdir(workingDir.c_str()) != 0) {
                ::_exit(126);
            }
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);

        ::execvp(executable.c_str(), argv.data());
        ::_exit(127);
    }

    // Parent process
    ::close(outPipe[1]);
    ::close(errPipe[1]);

    int flOut = ::fcntl(outPipe[0], F_GETFL, 0);
    if (flOut >= 0) ::fcntl(outPipe[0], F_SETFL, flOut | O_NONBLOCK);
    int flErr = ::fcntl(errPipe[0], F_GETFL, 0);
    if (flErr >= 0) ::fcntl(errPipe[0], F_SETFL, flErr | O_NONBLOCK);

    bool outOpen = true;
    bool errOpen = true;

    auto readAvailable = [&](int fd, std::string& dst, bool& isOpen) {
        char buf[4096];
        while (isOpen) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                if (dst.size() < maxOutputBytes) {
                    size_t room = maxOutputBytes - dst.size();
                    if (static_cast<size_t>(n) > room) {
                        dst.append(buf, room);
                        res.truncated = true;
                    } else {
                        dst.append(buf, static_cast<size_t>(n));
                    }
                } else {
                    res.truncated = true;
                }
            } else if (n == 0) {
                isOpen = false;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    break;
                }
                isOpen = false;
            }
        }
    };

    int status = 0;
    bool childExited = false;

    while (outOpen || errOpen || !childExited) {
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTp).count();
        if (timeoutMs > 0 && elapsedMs >= timeoutMs) {
            res.timedOut = true;
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            childExited = true;
            break;
        }

        struct pollfd pfds[2];
        nfds_t nfds = 0;
        int outIdx = -1, errIdx = -1;
        if (outOpen) {
            outIdx = static_cast<int>(nfds);
            pfds[nfds].fd = outPipe[0];
            pfds[nfds].events = POLLIN | POLLHUP;
            pfds[nfds].revents = 0;
            ++nfds;
        }
        if (errOpen) {
            errIdx = static_cast<int>(nfds);
            pfds[nfds].fd = errPipe[0];
            pfds[nfds].events = POLLIN | POLLHUP;
            pfds[nfds].revents = 0;
            ++nfds;
        }

        if (nfds > 0) {
            ::poll(pfds, nfds, 15);
            if (outIdx >= 0 && (pfds[outIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
                readAvailable(outPipe[0], res.stdoutText, outOpen);
            }
            if (errIdx >= 0 && (pfds[errIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
                readAvailable(errPipe[0], res.stderrText, errOpen);
            }
        } else {
            ::usleep(5000);
        }

        if (!childExited) {
            pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                childExited = true;
                readAvailable(outPipe[0], res.stdoutText, outOpen);
                readAvailable(errPipe[0], res.stderrText, errOpen);
                break;
            }
        }
    }

    if (!childExited) {
        ::waitpid(pid, &status, 0);
    }

    ::close(outPipe[0]);
    ::close(errPipe[0]);

    res.durationMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTp).count());

    if (res.timedOut) {
        res.launched = true;
        res.exitCode = -1;
        res.errorMessage = "Process timed out after " + std::to_string(timeoutMs) + " ms";
        return res;
    }

    if (WIFEXITED(status)) {
        res.exitCode = WEXITSTATUS(status);
        if (res.exitCode == 127 && res.stdoutText.empty()) {
            res.launched = false;
            res.errorMessage = "Executable not found: " + executable;
        } else {
            res.launched = true;
        }
    } else if (WIFSIGNALED(status)) {
        res.launched = true;
        res.exitCode = 128 + WTERMSIG(status);
    }

    return res;
}

#endif

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
