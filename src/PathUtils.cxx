/*
 * PathUtils.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "PathUtils.hxx"
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

namespace fs = std::filesystem;

namespace aimon {

std::string PathUtils::expandHome(const std::string& path) {
    if (path.empty() || path[0] != '~') {
        return path;
    }

    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw && pw->pw_dir) {
            home = pw->pw_dir;
        }
    }

    if (!home) {
        return path;
    }

    if (path.length() == 1) {
        return std::string(home);
    }

    if (path[1] == '/') {
        return std::string(home) + path.substr(1);
    }

    return path;
}

bool PathUtils::ensureDirectoryExists(const std::string& path) {
    try {
        std::string expanded = expandHome(path);
        if (!fs::exists(expanded)) {
            return fs::create_directories(expanded);
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string PathUtils::getDefaultCursorDbPath() {
#if defined(_WIN32)
    const char* appData = std::getenv("APPDATA");
    if (appData) {
        return std::string(appData) + "\\Cursor\\User\\globalStorage\\state.vscdb";
    }
    return "";
#elif defined(__APPLE__)
    return expandHome("~/Library/Application Support/Cursor/User/globalStorage/state.vscdb");
#else
    return expandHome("~/.config/Cursor/User/globalStorage/state.vscdb");
#endif
}

std::string PathUtils::getAimonConfigDir() {
#if defined(_WIN32)
    const char* appData = std::getenv("APPDATA");
    if (appData) {
        return std::string(appData) + "\\aimon";
    }
    return expandHome("~/.config/aimon");
#elif defined(__APPLE__)
    return expandHome("~/Library/Application Support/aimon");
#else
    return expandHome("~/.config/aimon");
#endif
}

std::string PathUtils::getDefaultConfigFilePath() {
    return getAimonConfigDir() + "/config.json";
}

std::string PathUtils::getDefaultLibConfigFilePath() {
    return getAimonConfigDir() + "/aimon.cfg";
}

std::string PathUtils::getDefaultHistoryDbPath() {
    return getAimonConfigDir() + "/history.db";
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
