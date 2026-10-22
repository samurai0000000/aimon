/*
 * CursorCollector.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_CURSOR_COLLECTOR_HXX
#define AIMON_CURSOR_COLLECTOR_HXX

#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "Models.hxx"
#include "ConfigManager.hxx"

namespace aimon {

class HistoryStore;

struct CursorHttpResponse {
    bool connected = false;
    int status = 0;
    std::string body;
};

class CursorCollector {
public:
    using HttpRequestFn = std::function<CursorHttpResponse(
        const std::string& method,
        const std::string& path,
        const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& body)>;

    explicit CursorCollector(const CursorConfig& config);

    CursorStatus fetchStatus();

    std::string resolveAccessToken();
    void setHistoryStore(HistoryStore* historyStore);
    void setHttpRequestFn(HttpRequestFn fn);

private:
    std::string extractTokenFromDb(const std::string& dbPath, std::string& outTier);
    CursorHttpResponse performRequest(
        const std::string& method,
        const std::string& path,
        const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& body = "");
    void fetchSpendData(const std::string& token,
                        const std::string& cycleStartIso,
                        const std::string& cycleEndIso,
                        CursorStatus& status);

    std::mutex _mutex;
    CursorConfig _config;
    HistoryStore* _historyStore = nullptr;
    HttpRequestFn _httpRequestFn;
    CursorStatus _lastValidStatus;
    bool _hasLastValidStatus = false;
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
