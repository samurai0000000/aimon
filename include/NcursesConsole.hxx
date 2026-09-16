/*
 * NcursesConsole.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_NCURSES_CONSOLE_HXX
#define AIMON_NCURSES_CONSOLE_HXX

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <functional>
#include <iostream>
#include <streambuf>
#include <curses.h>
#include "StateStore.hxx"

namespace aimon {

class NcursesConsole;

class NcursesStreamBuf : public std::streambuf {
public:
    explicit NcursesStreamBuf(NcursesConsole& console, int colorPair = 0);
    ~NcursesStreamBuf() override = default;

protected:
    int_type overflow(int_type c) override;
    int sync() override;

private:
    NcursesConsole& _console;
    int _colorPair = 0;
    std::string _lineBuffer;
    std::mutex _bufferMutex;
};

class NcursesConsole {
public:
    using ShutdownCallback = std::function<void()>;

    explicit NcursesConsole(StateStore& stateStore);
    ~NcursesConsole();

    bool init();
    void shutdown();

    void run();

    // Writes to main middle panel (Agent Chat & Commands)
    void logOutput(const std::string& text, int colorPair = 0, bool isBold = false);

    // Writes to top log pane (Server stdout/stderr logs)
    void logServer(const std::string& text, int colorPair = 0);

    // Formatted agent reply arriving in main panel
    void logAgentReply(const std::string& sessionId,
                       const std::string& messageId,
                       const std::string& replyText);

    void updateHeader();

    void setShutdownCallback(ShutdownCallback cb);

private:
    struct OutputLine {
        std::string text;
        int colorPair = 0;
        bool isBold = false;
    };
    static constexpr size_t MAX_HISTORY_LINES = 512;

    void setupWindows();
    void destroyWindows();
    void handleResize();
    void processCommand(const std::string& line);
    void redrawInputLine();
    void renderMiddlePanel();
    void addOutputLine(const std::string& line, int colorPair = 0, bool isBold = false);

    StateStore& _stateStore;
    ShutdownCallback _shutdownCb;

    std::mutex _uiMutex;
    std::atomic<bool> _running{false};
    std::atomic<bool> _initialized{false};

    WINDOW* _headerWin = nullptr;
    WINDOW* _logWin = nullptr;
    WINDOW* _midSepWin = nullptr;
    WINDOW* _cmdWin = nullptr;
    WINDOW* _bottomSepWin = nullptr;
    WINDOW* _inputWin = nullptr;

    std::string _inputBuffer;
    int _termRows = 0;
    int _termCols = 0;
    int _cmdHeight = 0;

    std::deque<OutputLine> _cmdHistory;
    int _scrollOffset = 0;

    int _activeChatAgentId = 0;
    std::string _activeChatSessionId;
    std::string _activeChatClientName;
    std::string _activeChatRemoteIp;

    NcursesStreamBuf _coutBuf;
    NcursesStreamBuf _cerrBuf;
    std::streambuf* _oldCoutBuf = nullptr;
    std::streambuf* _oldCerrBuf = nullptr;
};

} // namespace aimon

#endif // AIMON_NCURSES_CONSOLE_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
