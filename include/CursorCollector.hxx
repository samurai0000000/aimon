/*
 * CursorCollector.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_CURSOR_COLLECTOR_HXX
#define AIMON_CURSOR_COLLECTOR_HXX

#include <string>
#include "Models.hxx"
#include "ConfigManager.hxx"

namespace httplib {
    class SSLClient;
}

namespace aimon {

class CursorCollector {
public:
    explicit CursorCollector(const CursorConfig& config);

    CursorStatus fetchStatus();

    std::string resolveAccessToken();

private:
    std::string extractTokenFromDb(const std::string& dbPath, std::string& outTier);
    void fetchSpendData(httplib::SSLClient& cli,
                        const std::string& token,
                        const std::string& cycleStartIso,
                        const std::string& cycleEndIso,
                        CursorStatus& status);

    CursorConfig _config;
};

} // namespace aimon

#endif // AIMON_CURSOR_COLLECTOR_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
