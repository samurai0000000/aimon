/*
 * HistoryStore.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_HISTORY_STORE_HXX
#define AIMON_HISTORY_STORE_HXX

#include <string>
#include <map>
#include <mutex>
#include <sqlite3.h>
#include "Models.hxx"

namespace aimon {

struct HistoryRecord {
    int64_t id = 0;
    time_t timestamp = 0;
    std::string provider;
    std::string metricKey;
    double metricValue = 0.0;
    double metricLimit = 0.0;
    double delta = 0.0;
};

class HistoryStore {
public:
    HistoryStore();
    ~HistoryStore();

    bool open(const std::string& dbPath);
    void close();

    bool recordSnapshot(const AggregateStatus& status);
    std::vector<HistoryRecord> queryRecentRecords(int limit = 50);
    double queryTotalUsage(const std::string& provider, const std::string& metricKey, time_t sinceEpoch);

private:
    bool insertSample(time_t timestamp, const std::string& provider,
                      const std::string& metricKey, double value,
                      double limit, double delta);

    sqlite3* _db = nullptr;
    std::string _dbPath;
    std::mutex _mutex;
    std::map<std::string, double> _lastValues;
    time_t _lastCheckpoint = 0;
};

} // namespace aimon

#endif // AIMON_HISTORY_STORE_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
