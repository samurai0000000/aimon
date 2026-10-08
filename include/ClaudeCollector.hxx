/*
 * ClaudeCollector.hxx
 *
 * Collects Claude Code usage by itself: on every poll it reads only the new
 * bytes of each transcript under <config dir>/projects, turns usage lines into
 * normalized rows (no prompt text, working directory or identity), stores them
 * in a ClaudeUsageStore, and builds the per-account status from the store and
 * the price catalog. File positions are persisted, so restarts do not re-read.
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_CLAUDE_COLLECTOR_HXX
#define AIMON_CLAUDE_COLLECTOR_HXX

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "ClaudeUsageStore.hxx"
#include "ConfigManager.hxx"
#include "Models.hxx"
#include "PriceCatalog.hxx"

namespace aimon {

enum class ClaudeLineResult {
    Ignored,    // not a usage line (other record type, synthetic model, not JSON)
    Skipped,    // looks like usage but a field is missing or out of range
    Row         // a row was produced
};

class ClaudeCollector {
public:
    static const size_t kMaxLineBytes = 4u * 1024u * 1024u;
    static const size_t kReadChunkBytes = 1024u * 1024u;
    static const int64_t kMaxBytesPerFilePerPoll = 64LL * 1024 * 1024;

    ClaudeCollector(const ClaudeConfig& config, ClaudeUsageStore& store, PriceCatalog& catalog);
    ~ClaudeCollector();

    ClaudeCollector(const ClaudeCollector&) = delete;
    ClaudeCollector& operator=(const ClaudeCollector&) = delete;

    // Scans new transcript bytes for every account, stores the rows, records a
    // price version when the catalog changed, and returns the full status.
    ClaudeStatus fetchStatus(int64_t nowEpoch);

    // Background price refresh: fetches the catalog when it is missing or older
    // than pricingRefreshHours, checking every `checkEvery`. Never blocks a poll.
    void startPriceRefresh(std::chrono::milliseconds checkEvery = std::chrono::hours(1));
    void stopPriceRefresh();

    static ClaudeLineResult parseLine(const std::string& line, const std::string& account,
                                      ClaudeUsageRow& row);

private:
    struct ScanResult {
        int64_t bytesRead = 0;
        int64_t skippedLines = 0;
        bool ok = true;
        std::string error;
    };

    ScanResult scanFile(const std::string& account, const std::string& relPath,
                        const std::string& fullPath, const ClaudeFileCursor* cursor);
    void recordPriceVersion(int64_t nowEpoch);
    void priceRefreshLoop(std::chrono::milliseconds checkEvery);

    ClaudeConfig _config;
    ClaudeUsageStore& _store;
    PriceCatalog& _catalog;

    std::mutex _mutex;                       // serializes fetchStatus
    int64_t _lastPruneEpoch = 0;

    std::thread _refreshThread;
    std::mutex _refreshMutex;
    std::condition_variable _refreshCv;
    bool _refreshStop = false;
};

} // namespace aimon

#endif // AIMON_CLAUDE_COLLECTOR_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
