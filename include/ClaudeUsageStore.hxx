/*
 * ClaudeUsageStore.hxx
 *
 * Persistent, deduplicated store of Claude Code message usage rows plus the
 * per-file read cursors used by the transcript collector. Rows are keyed by
 * (account, message id, request id); the row with the strictly greatest
 * output token count wins and the first stored row wins ties. Aggregation is
 * done in SQL; costs are applied at query time from a price catalog snapshot
 * in integer nano-dollars, so a price refresh re-costs everything without
 * re-ingesting.
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_CLAUDE_USAGE_STORE_HXX
#define AIMON_CLAUDE_USAGE_STORE_HXX

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <sqlite3.h>

#include "PriceCatalog.hxx"

namespace aimon {

struct ClaudeUsageRow {
    std::string account;
    std::string sessionId;
    std::string messageId;
    std::string requestId;
    int64_t timestamp = 0;          // epoch seconds, UTC
    std::string model;
    std::string speed = "standard"; // "fast" or anything else (standard)
    int64_t input = 0;
    int64_t output = 0;
    int64_t cacheRead = 0;
    int64_t cacheWrite5m = 0;
    int64_t cacheWrite1h = 0;
    int64_t webSearch = 0;
    bool sidechain = false;
    bool creationSplitMissing = false;  // cache write counted as 5m for lack of a split
};

struct ClaudeIngestResult {
    bool ok = false;
    std::string error;
    int inserted = 0;
    int updated = 0;
    int ignored = 0;    // duplicate key with output not greater than the stored row
    int rejected = 0;   // failed validation
};

struct ClaudeFileCursor {
    std::string account;
    std::string path;
    int64_t offset = 0;
    int64_t size = 0;
    int64_t mtime = 0;
};

struct ClaudeUsageBucket {
    std::string day;    // UTC, YYYY-MM-DD
    std::string model;
    int64_t messages = 0;
    int64_t input = 0;
    int64_t output = 0;
    int64_t cacheRead = 0;
    int64_t cacheWrite5m = 0;
    int64_t cacheWrite1h = 0;
    int64_t webSearch = 0;
    int64_t costNano = 0;
    int64_t unpricedMessages = 0;
    bool costOverflow = false;
};

struct ClaudeUsageTotals {
    int64_t messages = 0;
    int64_t input = 0;
    int64_t output = 0;
    int64_t cacheRead = 0;
    int64_t cacheWrite5m = 0;
    int64_t cacheWrite1h = 0;
    int64_t webSearch = 0;
    int64_t costNano = 0;
    int64_t unpricedMessages = 0;
    bool costOverflow = false;
};

// A price catalog together with the time from which it applies. A message is
// costed with the latest version whose effectiveFrom is not after the message
// time; messages older than every version use the first one.
struct PriceVersion {
    int64_t effectiveFrom = 0;
    std::string pageSha256;
    std::map<std::string, ModelPrice> models;
};

enum class VersionSave { Inserted, Unchanged, Failed };

struct ClaudeCycle {
    int64_t startEpoch = 0;   // inclusive, 00:00 UTC on the reset day
    int64_t endEpoch = 0;     // exclusive, 00:00 UTC on the next reset day
};

// Billing cycle containing `nowEpoch` for a reset day of 1..28 (UTC). Returns
// false for a reset day outside that range.
bool claudeCycleBounds(int64_t nowEpoch, int resetDay, ClaudeCycle& out);

class ClaudeUsageStore {
public:
    static const int64_t kMaxTokensPerField = 1000000000;   // 1e9 tokens in one message
    static const size_t kMaxFieldLength = 256;

    ClaudeUsageStore();
    ~ClaudeUsageStore();

    ClaudeUsageStore(const ClaudeUsageStore&) = delete;
    ClaudeUsageStore& operator=(const ClaudeUsageStore&) = delete;

    bool open(const std::string& dbPath);
    void close();

    ClaudeIngestResult ingest(const std::vector<ClaudeUsageRow>& rows);

    // Aggregates rows of `account` with fromEpoch <= timestamp < toEpoch into
    // (day, model) buckets. Each message is costed with the price version in
    // effect at its time and, for tiered models, the rate set chosen by its
    // prompt length (input + cache read + cache writes). Returns false on SQL failure.
    bool aggregate(const std::string& account, int64_t fromEpoch, int64_t toEpoch,
                   const std::vector<PriceVersion>& versions,
                   std::vector<ClaudeUsageBucket>& out, std::string& error) const;

    // Single catalog applied to all time.
    bool aggregate(const std::string& account, int64_t fromEpoch, int64_t toEpoch,
                   const std::map<std::string, ModelPrice>& catalog,
                   std::vector<ClaudeUsageBucket>& out, std::string& error) const;

    // Stores a catalog version unless the latest stored version has the same page hash.
    VersionSave saveCatalogVersion(int64_t fetchedAt, const std::string& pageSha256,
                                   const std::map<std::string, ModelPrice>& models);
    bool loadCatalogVersions(std::vector<PriceVersion>& out, std::string& error) const;

    static ClaudeUsageTotals sum(const std::vector<ClaudeUsageBucket>& buckets);

    // Deletes rows older than the cutoff; returns the number deleted, -1 on error.
    int64_t pruneOlderThan(int64_t cutoffEpoch);

    int64_t rowCount(const std::string& account) const;

    bool saveCursor(const ClaudeFileCursor& cursor);
    std::vector<ClaudeFileCursor> loadCursors(const std::string& account) const;

private:
    bool execLocked(const char* sql, std::string& error) const;

    sqlite3* _db = nullptr;
    mutable std::mutex _mutex;
};

} // namespace aimon

#endif // AIMON_CLAUDE_USAGE_STORE_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
