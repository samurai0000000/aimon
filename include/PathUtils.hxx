/*
 * PathUtils.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_PATH_UTILS_HXX
#define AIMON_PATH_UTILS_HXX

#include <string>

namespace aimon {

class PathUtils {
public:
    static std::string expandHome(const std::string& path);
    static bool ensureDirectoryExists(const std::string& path);
    static std::string getDefaultCursorDbPath();
    static std::string getAimonConfigDir();
    static std::string getDefaultConfigFilePath();
    static std::string getDefaultLibConfigFilePath();
    static std::string getDefaultHistoryDbPath();
};

} // namespace aimon

#endif // AIMON_PATH_UTILS_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
