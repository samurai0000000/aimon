/*
 * NcursesConsole.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "NcursesConsole.hxx"
#include "TaskRegistry.hxx"
#include "AgentMessageBus.hxx"
#include "Version.hxx"
#include <sstream>
#include <iomanip>
#include <iostream>
#include <algorithm>

namespace aimon {

enum ColorPairs {
    PAIR_HEADER = 1,
    PAIR_PROMPT = 2,
    PAIR_AGENT = 3,
    PAIR_INFO = 4,
    PAIR_ERROR = 5,
    PAIR_TEXT = 6,
    PAIR_MUTED = 7
};

// --- NcursesStreamBuf Implementation ---

NcursesStreamBuf::NcursesStreamBuf(NcursesConsole& console, int colorPair)
    : _console(console), _colorPair(colorPair) {
}

NcursesStreamBuf::int_type NcursesStreamBuf::overflow(int_type c) {
    if (c != EOF) {
        std::lock_guard<std::mutex> lock(_bufferMutex);
        if (c == '\n') {
            _console.logServer(_lineBuffer, _colorPair);
            _lineBuffer.clear();
        } else if (c != '\r') {
            _lineBuffer += static_cast<char>(c);
        }
    }
    return c;
}

int NcursesStreamBuf::sync() {
    std::lock_guard<std::mutex> lock(_bufferMutex);
    if (!_lineBuffer.empty()) {
        _console.logServer(_lineBuffer, _colorPair);
        _lineBuffer.clear();
    }
    return 0;
}

// --- NcursesConsole Implementation ---

NcursesConsole::NcursesConsole(StateStore& stateStore)
    : _stateStore(stateStore),
      _coutBuf(*this, PAIR_MUTED),
      _cerrBuf(*this, PAIR_ERROR) {
}

NcursesConsole::~NcursesConsole() {
    shutdown();
}

bool NcursesConsole::init() {
    std::lock_guard<std::mutex> lock(_uiMutex);
    if (_initialized.load()) {
        return true;
    }

    initscr();
    cbreak();
    noecho();

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(PAIR_HEADER, COLOR_WHITE, COLOR_BLUE);
        init_pair(PAIR_PROMPT, COLOR_GREEN, -1);
        init_pair(PAIR_AGENT, COLOR_YELLOW, -1);
        init_pair(PAIR_INFO, COLOR_CYAN, -1);
        init_pair(PAIR_ERROR, COLOR_RED, -1);
        init_pair(PAIR_TEXT, COLOR_WHITE, -1);
        init_pair(PAIR_MUTED, COLOR_WHITE, -1);
    }

    setupWindows();
    _initialized = true;
    _running = true;

    // Redirect stdout and stderr so daemon server logs go strictly to the top log pane
    _oldCoutBuf = std::cout.rdbuf(&_coutBuf);
    _oldCerrBuf = std::cerr.rdbuf(&_cerrBuf);

    return true;
}

void NcursesConsole::setupWindows() {
    getmaxyx(stdscr, _termRows, _termCols);

    if (_termRows < 12) _termRows = 12;
    if (_termCols < 40) _termCols = 40;

    int headerRows = 1;
    int logRows = 5;
    int midSepRows = 1;
    int bottomSepRows = 1;
    int inputRows = 1;

    _cmdHeight = _termRows - headerRows - logRows - midSepRows - bottomSepRows - inputRows;
    if (_cmdHeight < 3) _cmdHeight = 3;

    // 1. Header window (Row 0)
    _headerWin = newwin(1, _termCols, 0, 0);

    // 2. Server logs window (Rows 1 to 5)
    _logWin = newwin(logRows, _termCols, 1, 0);
    scrollok(_logWin, TRUE);

    // 3. Middle divider with title & scroll badge (Row 6)
    _midSepWin = newwin(1, _termCols, 6, 0);

    // 4. Command outputs & chat panel (Rows 7 to 7 + _cmdHeight - 1)
    _cmdWin = newwin(_cmdHeight, _termCols, 7, 0);
    scrollok(_cmdWin, FALSE);

    // 5. Bottom divider (Row _termRows - 2)
    _bottomSepWin = newwin(1, _termCols, _termRows - 2, 0);
    whline(_bottomSepWin, ACS_HLINE, _termCols);
    wrefresh(_bottomSepWin);

    // 6. Input line (Row _termRows - 1)
    _inputWin = newwin(1, _termCols, _termRows - 1, 0);
    wtimeout(_inputWin, 100);
    keypad(_inputWin, TRUE);

    // Initial output
    if (_cmdHistory.empty()) {
        addOutputLine("--- Welcome to aimon console ---", PAIR_INFO, true);
        addOutputLine("Type 'help' for available commands. Type 'chat <id>' to chat with an agent.", PAIR_INFO, false);
        addOutputLine("", 0, false);
    }

    updateHeader();
    renderMiddlePanel();
    redrawInputLine();
}

void NcursesConsole::destroyWindows() {
    if (_headerWin) { delwin(_headerWin); _headerWin = nullptr; }
    if (_logWin) { delwin(_logWin); _logWin = nullptr; }
    if (_midSepWin) { delwin(_midSepWin); _midSepWin = nullptr; }
    if (_cmdWin) { delwin(_cmdWin); _cmdWin = nullptr; }
    if (_bottomSepWin) { delwin(_bottomSepWin); _bottomSepWin = nullptr; }
    if (_inputWin) { delwin(_inputWin); _inputWin = nullptr; }
}

void NcursesConsole::shutdown() {
    std::lock_guard<std::mutex> lock(_uiMutex);
    if (!_initialized.load()) {
        return;
    }

    if (_oldCoutBuf) {
        std::cout.rdbuf(_oldCoutBuf);
        _oldCoutBuf = nullptr;
    }
    if (_oldCerrBuf) {
        std::cerr.rdbuf(_oldCerrBuf);
        _oldCerrBuf = nullptr;
    }

    _running = false;
    destroyWindows();
    endwin();
    _initialized = false;
}

void NcursesConsole::handleResize() {
    destroyWindows();
    endwin();
    refresh();
    setupWindows();
}

void NcursesConsole::setShutdownCallback(ShutdownCallback cb) {
    _shutdownCb = cb;
}

void NcursesConsole::updateHeader() {
    if (!_headerWin) return;

    AggregateStatus status = _stateStore.getStatus();
    auto sessions = TaskRegistry::getInstance().listSessions();

    std::ostringstream ss;
    ss << " aimon v" << AIMON_VERSION_STRING << " | ";

    if (status.antigravity.isRunning) {
        ss << "Antigravity: [ACTIVE] | ";
    } else {
        ss << "Antigravity: [OFF] | ";
    }

    if (status.cursor.isAuthenticated) {
        ss << "Cursor: [" << status.cursor.planTier;
        if (status.cursor.fastRequestsLimit > 0) {
            int remaining = status.cursor.fastRequestsLimit - status.cursor.fastRequestsUsed;
            if (remaining < 0) remaining = 0;
            ss << " " << remaining << "/" << status.cursor.fastRequestsLimit << " fast";
        }
        ss << "] | ";
    } else {
        ss << "Cursor: [NO AUTH] | ";
    }

    ss << "Agents: " << sessions.size() << " connected ";

    std::string text = ss.str();
    if ((int)text.size() < _termCols) {
        text.append(_termCols - text.size(), ' ');
    } else {
        text = text.substr(0, _termCols);
    }

    wattron(_headerWin, COLOR_PAIR(PAIR_HEADER) | A_BOLD);
    mvwprintw(_headerWin, 0, 0, "%s", text.c_str());
    wattroff(_headerWin, COLOR_PAIR(PAIR_HEADER) | A_BOLD);
    wrefresh(_headerWin);
}

void NcursesConsole::addOutputLine(const std::string& text, int colorPair, bool isBold) {
    OutputLine item;
    item.text = text;
    item.colorPair = colorPair;
    item.isBold = isBold;

    _cmdHistory.push_back(item);
    while (_cmdHistory.size() > MAX_HISTORY_LINES) {
        _cmdHistory.pop_front();
    }

    int maxScroll = std::max(0, static_cast<int>(_cmdHistory.size()) - _cmdHeight);
    if (_scrollOffset > maxScroll) {
        _scrollOffset = maxScroll;
    }
}

void NcursesConsole::renderMiddlePanel() {
    if (!_cmdWin || !_midSepWin) return;

    // 1. Render middle separator line with title & scroll badge
    werase(_midSepWin);
    whline(_midSepWin, ACS_HLINE, _termCols);

    std::string title = "--- Command Outputs & Agent Chat [Up/Down/PgUp/PgDn to scroll]";
    if (_scrollOffset > 0) {
        title += " [^ " + std::to_string(_scrollOffset) + " lines scrolled]";
    }
    title += " ---";

    if ((int)title.size() > _termCols - 4) {
        title = title.substr(0, _termCols - 4);
    }
    mvwprintw(_midSepWin, 0, 2, "%s", title.c_str());
    wrefresh(_midSepWin);

    // 2. Render visible history slice in _cmdWin
    werase(_cmdWin);

    int totalLines = static_cast<int>(_cmdHistory.size());
    if (totalLines > 0 && _cmdHeight > 0) {
        int endIdx = totalLines - _scrollOffset;
        int startIdx = std::max(0, endIdx - _cmdHeight);

        int row = 0;
        for (int i = startIdx; i < endIdx && row < _cmdHeight; ++i, ++row) {
            const auto& line = _cmdHistory[i];
            int attrs = 0;
            if (line.colorPair > 0) {
                attrs |= COLOR_PAIR(line.colorPair);
            }
            if (line.isBold) {
                attrs |= A_BOLD;
            }

            if (attrs != 0) wattron(_cmdWin, attrs);
            mvwprintw(_cmdWin, row, 0, "%s", line.text.c_str());
            if (attrs != 0) wattroff(_cmdWin, attrs);
        }
    }

    wrefresh(_cmdWin);
}

void NcursesConsole::redrawInputLine() {
    if (!_inputWin) return;

    werase(_inputWin);
    wattron(_inputWin, COLOR_PAIR(PAIR_PROMPT) | A_BOLD);
    if (_activeChatAgentId > 0) {
        mvwprintw(_inputWin, 0, 0, "[#%d %s]> ", _activeChatAgentId, _activeChatClientName.c_str());
    } else {
        mvwprintw(_inputWin, 0, 0, "aimon> ");
    }
    wattroff(_inputWin, COLOR_PAIR(PAIR_PROMPT) | A_BOLD);

    wattron(_inputWin, COLOR_PAIR(PAIR_TEXT));
    wprintw(_inputWin, "%s", _inputBuffer.c_str());
    wattroff(_inputWin, COLOR_PAIR(PAIR_TEXT));

    wrefresh(_inputWin);
}

// Writes to main middle panel (Command Outputs & Agent Chat)
void NcursesConsole::logOutput(const std::string& text, int colorPair, bool isBold) {
    std::lock_guard<std::mutex> lock(_uiMutex);

    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        addOutputLine(line, colorPair, isBold);
    }

    renderMiddlePanel();
    redrawInputLine();
}

// Writes to upper log pane (Server / Daemon logs)
void NcursesConsole::logServer(const std::string& text, int colorPair) {
    std::lock_guard<std::mutex> lock(_uiMutex);
    if (!_logWin) return;

    if (colorPair > 0) {
        wattron(_logWin, COLOR_PAIR(colorPair));
    }
    wprintw(_logWin, "%s\n", text.c_str());
    if (colorPair > 0) {
        wattroff(_logWin, COLOR_PAIR(colorPair));
    }

    wrefresh(_logWin);
    redrawInputLine();
}

void NcursesConsole::logAgentReply(const std::string& sessionId,
                                   const std::string& messageId,
                                   const std::string& replyText) {
    std::lock_guard<std::mutex> lock(_uiMutex);

    auto sessions = TaskRegistry::getInstance().listSessions();
    int agentIndex = 0;
    std::string agentName = "Unknown Agent";
    std::string remoteIp = "network";

    for (size_t i = 0; i < sessions.size(); ++i) {
        if (sessions[i].sessionId == sessionId) {
            agentIndex = static_cast<int>(i + 1);
            agentName = sessions[i].clientName;
            remoteIp = sessions[i].remoteIp;
            break;
        }
    }

    std::string header;
    if (agentIndex > 0) {
        header = "[#" + std::to_string(agentIndex) + " " + agentName + " (" + remoteIp + ")] (ref: " + messageId + "):";
    } else {
        header = "[Agent (" + sessionId.substr(0, 8) + ")] (ref: " + messageId + "):";
    }

    addOutputLine("", 0, false);
    addOutputLine(header, PAIR_AGENT, true);

    std::istringstream iss(replyText);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        addOutputLine(line, PAIR_TEXT, false);
    }
    addOutputLine("", 0, false);

    _scrollOffset = 0; // Snap to latest on incoming reply
    renderMiddlePanel();
    redrawInputLine();
}

void NcursesConsole::processCommand(const std::string& line) {
    if (line.empty()) {
        return;
    }

    // --- Case 1: In dedicated chat <id> mode ---
    if (_activeChatAgentId > 0) {
        // Direct chat text! Echo user message in prompt color
        addOutputLine("[You -> #" + std::to_string(_activeChatAgentId) + " " + _activeChatClientName + "]: " + line, PAIR_PROMPT, true);

        auto sessions = TaskRegistry::getInstance().listSessions();
        bool sessionValid = false;
        for (const auto& s : sessions) {
            if (s.sessionId == _activeChatSessionId) {
                sessionValid = true;
                break;
            }
        }

        if (!sessionValid) {
            addOutputLine("[Chat] Agent #" + std::to_string(_activeChatAgentId) + " disconnected. Exited chat mode.", PAIR_ERROR, true);
            _activeChatAgentId = 0;
        } else {
            std::string msgId = AgentMessageBus::getInstance().postMessageToAgent(_activeChatSessionId, line);
            addOutputLine("[Sent " + msgId + "]", PAIR_INFO, false);
        }

        addOutputLine("", 0, false);
        _scrollOffset = 0;
        renderMiddlePanel();
        redrawInputLine();
        return;
    }

    // --- Case 2: In top-level aimon> mode ---
    addOutputLine("aimon> " + line, PAIR_PROMPT, false);

    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

    if (cmd == "help" || cmd == "?") {
        addOutputLine("Available commands:", PAIR_INFO, true);
        addOutputLine("  agents, list          List connected network agent sessions with numeric IDs", PAIR_INFO, false);
        addOutputLine("  chat <id>             Enter interactive chat mode with agent <id> (Press Ctrl+D to exit)", PAIR_INFO, false);
        addOutputLine("  msg, send <id> <text> Send a one-off asynchronous message to agent <id>", PAIR_INFO, false);
        addOutputLine("  status                Display quota metrics and daemon status", PAIR_INFO, false);
        addOutputLine("  clear                 Clear the command outputs history buffer", PAIR_INFO, false);
        addOutputLine("  help, ?               Show this command help", PAIR_INFO, false);
        addOutputLine("  quit, exit            Shutdown the aimon daemon", PAIR_INFO, false);
        addOutputLine("", 0, false);
    } else if (cmd == "agents" || cmd == "list") {
        auto sessions = TaskRegistry::getInstance().listSessions();
        if (sessions.empty()) {
            addOutputLine("No agents currently connected via SSE.", PAIR_INFO, false);
            addOutputLine("", 0, false);
        } else {
            addOutputLine("Connected Agents (" + std::to_string(sessions.size()) + "):", PAIR_INFO, true);
            for (size_t i = 0; i < sessions.size(); ++i) {
                const auto& s = sessions[i];
                std::string row = "  [" + std::to_string(i + 1) + "] " + s.clientName +
                                  " (Host: " + s.remoteIp +
                                  ", Session: " + s.sessionId.substr(0, 8) +
                                  ", Active: " + (s.active ? "yes" : "no") + ")";
                addOutputLine(row, PAIR_TEXT, false);
            }
            addOutputLine("", 0, false);
        }
    } else if (cmd == "chat") {
        int agentId = 0;
        if (!(iss >> agentId)) {
            addOutputLine("Usage: chat <agent_id>", PAIR_ERROR, false);
            addOutputLine("", 0, false);
        } else {
            auto sessions = TaskRegistry::getInstance().listSessions();
            if (agentId < 1 || agentId > static_cast<int>(sessions.size())) {
                addOutputLine("Error: Invalid agent ID " + std::to_string(agentId) +
                              " (must be 1 to " + std::to_string(sessions.size()) + ").", PAIR_ERROR, false);
                addOutputLine("", 0, false);
            } else {
                const auto& targetSession = sessions[agentId - 1];
                _activeChatAgentId = agentId;
                _activeChatSessionId = targetSession.sessionId;
                _activeChatClientName = targetSession.clientName;
                _activeChatRemoteIp = targetSession.remoteIp;

                addOutputLine("--- Entering Chat Mode with [#" + std::to_string(agentId) + " " +
                              targetSession.clientName + " (" + targetSession.remoteIp + ")] ---", PAIR_INFO, true);
                addOutputLine("Type messages to send directly. Press Ctrl+D to exit chat and return to aimon> prompt.", PAIR_INFO, false);
                addOutputLine("", 0, false);
            }
        }
    } else if (cmd == "msg" || cmd == "send") {
        int agentId = 0;
        if (!(iss >> agentId)) {
            addOutputLine("Usage: msg <id> <message text>", PAIR_ERROR, false);
            addOutputLine("", 0, false);
        } else {
            std::string text;
            std::getline(iss, text);
            size_t start = text.find_first_not_of(" \t");
            if (start != std::string::npos) {
                text = text.substr(start);
            }

            if (text.empty()) {
                addOutputLine("Error: empty message text.", PAIR_ERROR, false);
                addOutputLine("", 0, false);
            } else {
                auto sessions = TaskRegistry::getInstance().listSessions();
                if (agentId < 1 || agentId > static_cast<int>(sessions.size())) {
                    addOutputLine("Error: Invalid agent ID " + std::to_string(agentId) +
                                  " (must be 1 to " + std::to_string(sessions.size()) + ").", PAIR_ERROR, false);
                    addOutputLine("", 0, false);
                } else {
                    const auto& targetSession = sessions[agentId - 1];
                    std::string msgId = AgentMessageBus::getInstance().postMessageToAgent(
                        targetSession.sessionId, text);

                    addOutputLine("[Dispatch] Message " + msgId + " dispatched to [" +
                                  std::to_string(agentId) + "] " + targetSession.clientName +
                                  " (" + targetSession.remoteIp + ").", PAIR_INFO, false);
                    addOutputLine("", 0, false);
                }
            }
        }
    } else if (cmd == "status") {
        AggregateStatus st = _stateStore.getStatus();
        addOutputLine("--- System Status Snapshot ---", PAIR_INFO, true);
        addOutputLine("Antigravity Daemon : " + std::string(st.antigravity.isRunning ? "Running" : "Stopped") +
                      " (Tier: " + st.antigravity.planTier + ")", PAIR_TEXT, false);
        if (!st.antigravity.models.empty()) {
            addOutputLine("  Tracked Models   : " + std::to_string(st.antigravity.models.size()) + " models active", PAIR_TEXT, false);
        }
        addOutputLine("Cursor Subscription: " + std::string(st.cursor.isAuthenticated ? "Authenticated" : "Not authenticated") +
                      " (Tier: " + st.cursor.planTier + ")", PAIR_TEXT, false);
        if (st.cursor.isAuthenticated && st.cursor.fastRequestsLimit > 0) {
            int remaining = st.cursor.fastRequestsLimit - st.cursor.fastRequestsUsed;
            if (remaining < 0) remaining = 0;
            addOutputLine("  Fast Requests    : " + std::to_string(remaining) + " / " +
                          std::to_string(st.cursor.fastRequestsLimit) + " remaining (used " +
                          std::to_string(st.cursor.fastRequestsUsed) + ")", PAIR_TEXT, false);
            addOutputLine("  Billing Cycle End: " + st.cursor.cycleResetIso, PAIR_TEXT, false);
        }
        addOutputLine("", 0, false);
    } else if (cmd == "clear") {
        _cmdHistory.clear();
        _scrollOffset = 0;
        addOutputLine("History cleared.", PAIR_INFO, false);
        addOutputLine("", 0, false);
    } else if (cmd == "quit" || cmd == "exit") {
        std::string extra;
        if (iss >> extra) {
            addOutputLine("Unknown command: '" + line + "'. To chat with an agent, use 'chat <id>'.", PAIR_ERROR, false);
            addOutputLine("", 0, false);
        } else {
            addOutputLine("Shutting down aimon daemon...", PAIR_INFO, true);
            renderMiddlePanel();
            _running = false;
            if (_shutdownCb) {
                _shutdownCb();
            }
            return;
        }
    } else {
        addOutputLine("Unknown command: '" + cmd + "'. Type 'help' for available commands or 'chat <id>' to chat.", PAIR_ERROR, false);
        addOutputLine("", 0, false);
    }

    _scrollOffset = 0;
    renderMiddlePanel();
    updateHeader();
    redrawInputLine();
}

void NcursesConsole::run() {
    int headerTick = 0;

    while (_running.load()) {
        int ch = wgetch(_inputWin);

        if (ch == ERR) {
            if (++headerTick >= 20) { // update header every ~2s
                headerTick = 0;
                std::lock_guard<std::mutex> lock(_uiMutex);
                updateHeader();
            }
            continue;
        }

        if (ch == KEY_RESIZE) {
            std::lock_guard<std::mutex> lock(_uiMutex);
            handleResize();
            continue;
        }

        std::lock_guard<std::mutex> lock(_uiMutex);

        // Ctrl+D (ASCII 4) handler
        if (ch == 4) {
            if (_activeChatAgentId > 0) {
                addOutputLine("[Chat] Exited chat mode. Returned to aimon> prompt.", PAIR_INFO, true);
                addOutputLine("", 0, false);
                _activeChatAgentId = 0;
                _scrollOffset = 0;
                _inputBuffer.clear();
                renderMiddlePanel();
                redrawInputLine();
                continue;
            } else if (_inputBuffer.empty()) {
                addOutputLine("Shutting down aimon daemon...", PAIR_INFO, true);
                renderMiddlePanel();
                _running = false;
                if (_shutdownCb) {
                    _shutdownCb();
                }
                break;
            }
        }

        // ESC / Escape sequences (Up, Down, PgUp, PgDn)
        if (ch == 27) {
            wtimeout(_inputWin, 25);
            int c2 = wgetch(_inputWin);
            if (c2 == '[' || c2 == 'O') {
                int c3 = wgetch(_inputWin);
                if (c3 == 'A') { // Up
                    int maxScroll = std::max(0, static_cast<int>(_cmdHistory.size()) - _cmdHeight);
                    if (_scrollOffset < maxScroll) {
                        _scrollOffset++;
                        renderMiddlePanel();
                    }
                } else if (c3 == 'B') { // Down
                    if (_scrollOffset > 0) {
                        _scrollOffset--;
                        renderMiddlePanel();
                    }
                } else if (c3 == '5') { // PgUp (\033[5~)
                    int c4 = wgetch(_inputWin);
                    (void)c4;
                    int maxScroll = std::max(0, static_cast<int>(_cmdHistory.size()) - _cmdHeight);
                    _scrollOffset = std::min(maxScroll, _scrollOffset + std::max(1, _cmdHeight - 2));
                    renderMiddlePanel();
                } else if (c3 == '6') { // PgDn (\033[6~)
                    int c4 = wgetch(_inputWin);
                    (void)c4;
                    _scrollOffset = std::max(0, _scrollOffset - std::max(1, _cmdHeight - 2));
                    renderMiddlePanel();
                }
            }
            wtimeout(_inputWin, 100);
            continue;
        }

        // Up arrow: scroll history up
        if (ch == KEY_UP) {
            int maxScroll = std::max(0, static_cast<int>(_cmdHistory.size()) - _cmdHeight);
            if (_scrollOffset < maxScroll) {
                _scrollOffset++;
                renderMiddlePanel();
            }
            continue;
        }

        // Down arrow: scroll history down
        if (ch == KEY_DOWN) {
            if (_scrollOffset > 0) {
                _scrollOffset--;
                renderMiddlePanel();
            }
            continue;
        }

        // Page Up: scroll history up by page
        if (ch == KEY_PPAGE) {
            int maxScroll = std::max(0, static_cast<int>(_cmdHistory.size()) - _cmdHeight);
            _scrollOffset = std::min(maxScroll, _scrollOffset + std::max(1, _cmdHeight - 2));
            renderMiddlePanel();
            continue;
        }

        // Page Down: scroll history down by page
        if (ch == KEY_NPAGE) {
            _scrollOffset = std::max(0, _scrollOffset - std::max(1, _cmdHeight - 2));
            renderMiddlePanel();
            continue;
        }

        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            std::string line = _inputBuffer;
            _inputBuffer.clear();
            processCommand(line);
            redrawInputLine();
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (!_inputBuffer.empty()) {
                _inputBuffer.pop_back();
                redrawInputLine();
            }
        } else if (ch >= 32 && ch <= 126) {
            _inputBuffer.push_back(static_cast<char>(ch));
            redrawInputLine();
        }
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
