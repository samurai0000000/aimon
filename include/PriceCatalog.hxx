/*
 * PriceCatalog.hxx
 *
 * Retrieves Claude model prices from the official Anthropic pricing page
 * (markdown form), validates them strictly, and caches them on disk. Prices
 * are never embedded in the program. All amounts are integer nano-dollars per
 * token: a price of $P / MTok is P * 1000 nano-dollars per token.
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_PRICE_CATALOG_HXX
#define AIMON_PRICE_CATALOG_HXX

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aimon {

struct ModelPrice {
    std::string modelId;
    int64_t inputNano = 0;
    int64_t write5mNano = 0;
    int64_t write1hNano = 0;
    int64_t readNano = 0;
    int64_t outputNano = 0;
    bool hasFast = false;
    int64_t fastInputNano = 0;
    int64_t fastWrite5mNano = 0;
    int64_t fastWrite1hNano = 0;
    int64_t fastReadNano = 0;
    int64_t fastOutputNano = 0;

    // Models priced by prompt length (for example Claude Haiku 5.5): the fields
    // above apply to prompts up to tierThresholdTokens and `upperTier` to longer
    // ones. tierThresholdTokens is 0 and upperTier is null for flat-priced models.
    int64_t tierThresholdTokens = 0;
    std::shared_ptr<ModelPrice> upperTier;

    bool operator==(const ModelPrice& other) const;
};

struct PriceQuarantine {
    std::string modelId;
    std::string reason;
};

struct PriceParseResult {
    bool ok = false;
    std::string error;
    int skippedFastEntries = 0;
    int skippedRows = 0;                       // rows whose model name could not be read
    std::vector<PriceQuarantine> quarantined;  // models left unpriced because of a row problem
    std::map<std::string, ModelPrice> models;
};

struct PriceFetchResult {
    int status = 0;
    std::string body;
    std::string error;
};

struct PriceCatalogStatus {
    std::string sourceUrl;
    int64_t fetchedAtEpoch = 0;
    std::string pageSha256;
    size_t modelsLoaded = 0;
    size_t quarantinedCount = 0;
    bool stale = true;
    std::string error;
};

class PriceCatalog {
public:
    using FetchFn = std::function<PriceFetchResult(const std::string& url)>;

    static const char* const kDefaultUrl;
    static const size_t kMaxBodyBytes = 4u * 1024u * 1024u;

    PriceCatalog(const std::string& cachePath, const std::string& url, FetchFn fetch);

    // Pure functions (no I/O).
    static PriceParseResult parsePage(const std::string& markdown);
    static bool parsePriceCell(const std::string& cell, int64_t& nano);
    static std::string modelNameToId(const std::string& name);
    static std::string sha256Hex(const std::string& data);

    // JSON form shared by the disk cache and the store's price versions.
    static std::string modelsToJson(const std::map<std::string, ModelPrice>& models);
    static bool modelsFromJson(const std::string& json, std::map<std::string, ModelPrice>& models,
                               std::string& error);

    // Resolves a transcript model id: the id itself, or the id followed by a
    // dash and exactly eight digits (a date). Anything else is not priced.
    static std::string resolveModelId(const std::map<std::string, ModelPrice>& models,
                                      const std::string& modelId);

    // Default network fetcher: HTTPS GET with certificate verification, 20 s
    // timeout, body capped at kMaxBodyBytes, at most 3 same-host redirects.
    static FetchFn httpFetcher();

    bool loadCache();
    // `changed` (optional) is set when the page hash differs from the previous one.
    bool refresh(int64_t nowEpoch, bool* changed = nullptr);

    std::optional<ModelPrice> lookup(const std::string& modelId) const;
    std::map<std::string, ModelPrice> snapshot() const;
    PriceCatalogStatus status(int64_t nowEpoch, int refreshHours) const;

    // Status and models read atomically (a refresh cannot slip in between).
    void capture(int64_t nowEpoch, int refreshHours, PriceCatalogStatus& status,
                 std::map<std::string, ModelPrice>& models) const;

private:
    bool writeCache(const std::string& pageSha, int64_t fetchedAt, std::string& error) const;

    std::string _cachePath;
    std::string _url;
    FetchFn _fetch;

    mutable std::mutex _mutex;
    std::map<std::string, ModelPrice> _models;
    std::vector<PriceQuarantine> _quarantined;
    int64_t _fetchedAtEpoch = 0;
    std::string _pageSha256;
    std::string _lastError;
};

} // namespace aimon

#endif // AIMON_PRICE_CATALOG_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
