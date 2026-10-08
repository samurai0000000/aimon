/*
 * PriceCatalog.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "PriceCatalog.hxx"
#include "PathUtils.hxx"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace aimon {

const char* const PriceCatalog::kDefaultUrl =
    "https://platform.claude.com/docs/en/about-claude/pricing.md";

namespace {

const char* const kMainHeading = "## Model pricing";
const char* const kFastHeading = "### Fast mode pricing";
const std::vector<std::string> kMainHeader = {
    "Model", "Base input tokens", "5m cache writes", "1h cache writes",
    "Cache hits and refreshes", "Output tokens"};
const std::vector<std::string> kFastHeader = {"Model", "Input", "Output"};

const int64_t kMaxPriceNano = 999999999;
const size_t kMaxCacheBytes = 4u * 1024u * 1024u;

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
        --e;
    }
    return s.substr(b, e - b);
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        std::string line = text.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
        start = nl + 1;
    }
    return lines;
}

bool isTableLine(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    return i < line.size() && line[i] == '|';
}

bool isSeparatorLine(const std::string& line) {
    if (!isTableLine(line)) {
        return false;
    }
    bool dash = false;
    for (char c : line) {
        if (c == '-') {
            dash = true;
        } else if (c != '|' && c != ':' && c != ' ' && c != '\t') {
            return false;
        }
    }
    return dash;
}

std::vector<std::string> splitCells(const std::string& line) {
    std::string s = trim(line);
    if (!s.empty() && s.front() == '|') {
        s.erase(0, 1);
    }
    if (!s.empty() && s.back() == '|') {
        s.pop_back();
    }
    std::vector<std::string> cells;
    size_t start = 0;
    while (true) {
        size_t bar = s.find('|', start);
        if (bar == std::string::npos) {
            cells.push_back(trim(s.substr(start)));
            break;
        }
        cells.push_back(trim(s.substr(start, bar - start)));
        start = bar + 1;
    }
    return cells;
}

int64_t roundHalfUp(int64_t a, int64_t b, int64_t c) {
    return (a * b + c / 2) / c;
}

int64_t absDiff(int64_t a, int64_t b) {
    return a > b ? a - b : b - a;
}

// Finds `heading`, then the table below it, checks the header and returns the
// body rows as cell vectors. A heading, header or separator problem is an error.
bool readTable(const std::vector<std::string>& lines, const std::string& heading,
               const std::vector<std::string>& header,
               std::vector<std::vector<std::string>>& rows, std::string& error) {
    size_t i = 0;
    for (; i < lines.size(); ++i) {
        if (trim(lines[i]) == heading) {
            break;
        }
    }
    if (i == lines.size()) {
        error = "heading not found: " + heading;
        return false;
    }
    ++i;
    while (i < lines.size() && !isTableLine(lines[i])) {
        if (!lines[i].empty() && lines[i][0] == '#') {
            error = "no table under " + heading;
            return false;
        }
        ++i;
    }
    if (i >= lines.size() || splitCells(lines[i]) != header) {
        error = "unexpected table header under " + heading;
        return false;
    }
    ++i;
    if (i >= lines.size() || !isSeparatorLine(lines[i])) {
        error = "missing table separator under " + heading;
        return false;
    }
    ++i;
    while (i < lines.size() && isTableLine(lines[i])) {
        std::vector<std::string> cells = splitCells(lines[i]);
        if (cells.size() != header.size()) {
            error = "row has " + std::to_string(cells.size()) + " cells, expected " +
                    std::to_string(header.size()) + " under " + heading;
            return false;
        }
        rows.push_back(cells);
        ++i;
    }
    return true;
}

bool validateStandard(const ModelPrice& m, std::string& error) {
    if (m.inputNano <= 0 || m.write5mNano <= 0 || m.write1hNano <= 0 ||
        m.readNano <= 0 || m.outputNano <= 0) {
        error = "non-positive price for " + m.modelId;
        return false;
    }
    if (absDiff(4 * m.write5mNano, 5 * m.inputNano) * 100 > 5 * m.inputNano) {
        error = "5m cache write is not 1.25x input for " + m.modelId;
        return false;
    }
    if (absDiff(m.write1hNano, 2 * m.inputNano) * 100 > 2 * m.inputNano) {
        error = "1h cache write is not 2x input for " + m.modelId;
        return false;
    }
    if (m.readNano > m.inputNano) {
        error = "cache read above input for " + m.modelId;
        return false;
    }
    return true;
}

void deriveFast(ModelPrice& m, int64_t fastInput, int64_t fastOutput) {
    m.hasFast = true;
    m.fastInputNano = fastInput;
    m.fastOutputNano = fastOutput;
    m.fastWrite5mNano = roundHalfUp(fastInput, m.write5mNano, m.inputNano);
    m.fastWrite1hNano = roundHalfUp(fastInput, m.write1hNano, m.inputNano);
    m.fastReadNano = roundHalfUp(fastInput, m.readNano, m.inputNano);
}

bool jsonInt(const nlohmann::json& obj, const char* key, int64_t& out) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) {
        return false;
    }
    out = it->get<int64_t>();
    return true;
}

bool readFileBounded(const std::string& path, size_t maxBytes, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return out.size() <= maxBytes;
}


nlohmann::json rateSetJson(const ModelPrice& m) {
    return {{"input_nano", m.inputNano}, {"write5m_nano", m.write5mNano},
            {"write1h_nano", m.write1hNano}, {"read_nano", m.readNano},
            {"output_nano", m.outputNano}};
}

nlohmann::json modelJson(const ModelPrice& m) {
    nlohmann::json j = rateSetJson(m);
    if (m.hasFast) {
        j["fast_input_nano"] = m.fastInputNano;
        j["fast_write5m_nano"] = m.fastWrite5mNano;
        j["fast_write1h_nano"] = m.fastWrite1hNano;
        j["fast_read_nano"] = m.fastReadNano;
        j["fast_output_nano"] = m.fastOutputNano;
    }
    if (m.upperTier) {
        j["tier_threshold_tokens"] = m.tierThresholdTokens;
        j["upper"] = rateSetJson(*m.upperTier);
    }
    return j;
}

bool rateSetFromJson(const std::string& id, const nlohmann::json& j, ModelPrice& m, std::string& error) {
    m.modelId = id;
    if (!j.is_object() || !jsonInt(j, "input_nano", m.inputNano) || !jsonInt(j, "write5m_nano", m.write5mNano) ||
        !jsonInt(j, "write1h_nano", m.write1hNano) || !jsonInt(j, "read_nano", m.readNano) ||
        !jsonInt(j, "output_nano", m.outputNano)) {
        error = "rate set is invalid for " + id.substr(0, 60);
        return false;
    }
    if (m.inputNano > kMaxPriceNano || m.write5mNano > kMaxPriceNano || m.write1hNano > kMaxPriceNano ||
        m.readNano > kMaxPriceNano || m.outputNano > kMaxPriceNano) {
        error = "price out of range for " + id.substr(0, 60);
        return false;
    }
    return validateStandard(m, error);
}

bool modelFromJson(const std::string& id, const nlohmann::json& j, ModelPrice& m, std::string& error) {
    if (id.rfind("claude-", 0) != 0 || !rateSetFromJson(id, j, m, error)) {
        if (error.empty()) {
            error = "invalid model id " + id.substr(0, 60);
        }
        return false;
    }
    if (j.contains("fast_input_nano")) {
        int64_t fastIn = 0;
        int64_t fastOut = 0;
        if (!jsonInt(j, "fast_input_nano", fastIn) || !jsonInt(j, "fast_output_nano", fastOut) ||
            fastIn <= 0 || fastOut <= 0 || fastIn > kMaxPriceNano || fastOut > kMaxPriceNano) {
            error = "fast entry is invalid for " + id.substr(0, 60);
            return false;
        }
        deriveFast(m, fastIn, fastOut);
    }
    const bool hasThreshold = j.contains("tier_threshold_tokens");
    const bool hasUpper = j.contains("upper");
    if (hasThreshold != hasUpper) {
        error = "incomplete tier data for " + id.substr(0, 60);
        return false;
    }
    if (hasUpper) {
        int64_t threshold = 0;
        auto upper = std::make_shared<ModelPrice>();
        if (!jsonInt(j, "tier_threshold_tokens", threshold) || threshold <= 0 || threshold > 1000000000000LL ||
            !rateSetFromJson(id, j["upper"], *upper, error)) {
            if (error.empty()) {
                error = "tier threshold is invalid for " + id.substr(0, 60);
            }
            return false;
        }
        m.tierThresholdTokens = threshold;
        m.upperTier = upper;
    }
    return true;
}

bool parsePositiveInt(const std::string& digitsWithCommas, int64_t& out) {
    std::string digits;
    for (char c : digitsWithCommas) {
        if (c == ',') {
            continue;
        }
        if (c < '0' || c > '9') {
            return false;
        }
        digits.push_back(c);
    }
    if (digits.empty() || digits.size() > 12) {
        return false;
    }
    out = std::stoll(digits);
    return out > 0;
}

// Recognizes "(for prompts up to N tokens)" / "(for prompts over N tokens)".
// Returns 0 for no annotation, 1 or 2 for a recognized "up to" / "over" tier
// (with `threshold` and the name before the annotation), -1 for an annotation
// that starts with "(for prompts" but is not in the recognized form.
int readTierAnnotation(const std::string& name, std::string& baseName, int64_t& threshold) {
    size_t pos = name.find("(for prompts");
    if (pos == std::string::npos) {
        baseName = name;
        return 0;
    }
    baseName = name.substr(0, pos);
    std::string rest = name.substr(pos);
    int kind = 0;
    size_t i = 0;
    const char kUpTo[] = "(for prompts up to ";
    const char kOver[] = "(for prompts over ";
    const size_t upLen = sizeof(kUpTo) - 1;
    const size_t overLen = sizeof(kOver) - 1;
    if (rest.compare(0, upLen, kUpTo) == 0) {
        kind = 1;
        i = upLen;
    } else if (rest.compare(0, overLen, kOver) == 0) {
        kind = 2;
        i = overLen;
    } else {
        return -1;
    }
    size_t end = rest.find(" tokens)", i);
    if (end == std::string::npos || trim(rest.substr(end + 8)) != "" || !parsePositiveInt(rest.substr(i, end - i), threshold)) {
        return -1;
    }
    return kind;
}

struct RowEntry {
    int kind = 0;               // 0 plain, 1 "up to", 2 "over"
    int64_t threshold = 0;
    std::vector<std::string> cells;
};

bool parseRateCells(const std::vector<std::string>& cells, ModelPrice& m, std::string& reason) {
    if (!PriceCatalog::parsePriceCell(cells[1], m.inputNano) || !PriceCatalog::parsePriceCell(cells[2], m.write5mNano) ||
        !PriceCatalog::parsePriceCell(cells[3], m.write1hNano) || !PriceCatalog::parsePriceCell(cells[4], m.readNano) ||
        !PriceCatalog::parsePriceCell(cells[5], m.outputNano)) {
        reason = "unparseable price cell";
        return false;
    }
    return validateStandard(m, reason);
}

} // namespace

bool ModelPrice::operator==(const ModelPrice& o) const {
    return modelId == o.modelId && inputNano == o.inputNano && write5mNano == o.write5mNano &&
           write1hNano == o.write1hNano && readNano == o.readNano && outputNano == o.outputNano &&
           hasFast == o.hasFast && fastInputNano == o.fastInputNano &&
           fastWrite5mNano == o.fastWrite5mNano && fastWrite1hNano == o.fastWrite1hNano &&
           fastReadNano == o.fastReadNano && fastOutputNano == o.fastOutputNano &&
           tierThresholdTokens == o.tierThresholdTokens &&
           (upperTier == nullptr) == (o.upperTier == nullptr) &&
           (upperTier == nullptr || *upperTier == *o.upperTier);
}

PriceCatalog::PriceCatalog(const std::string& cachePath, const std::string& url, FetchFn fetch)
    : _cachePath(cachePath), _url(url), _fetch(std::move(fetch)) {
}

bool PriceCatalog::parsePriceCell(const std::string& cellIn, int64_t& nano) {
    std::string cell = cellIn;
    for (size_t open = cell.find("<sup>"); open != std::string::npos; open = cell.find("<sup>")) {
        size_t close = cell.find("</sup>", open);
        if (close == std::string::npos) {
            return false;
        }
        cell.erase(open, close + 6 - open);
    }
    cell = trim(cell);
    size_t i = 0;
    auto skipSpaces = [&]() {
        while (i < cell.size() && (cell[i] == ' ' || cell[i] == '\t')) {
            ++i;
        }
    };
    if (cell.empty() || cell[i] != '$') {
        return false;
    }
    ++i;
    skipSpaces();
    size_t wholeStart = i;
    while (i < cell.size() && cell[i] >= '0' && cell[i] <= '9') {
        ++i;
    }
    size_t wholeLen = i - wholeStart;
    if (wholeLen == 0 || wholeLen > 6) {
        return false;
    }
    int64_t whole = std::stoll(cell.substr(wholeStart, wholeLen));
    int64_t frac = 0;
    if (i < cell.size() && cell[i] == '.') {
        ++i;
        size_t fracStart = i;
        while (i < cell.size() && cell[i] >= '0' && cell[i] <= '9') {
            ++i;
        }
        size_t fracLen = i - fracStart;
        if (fracLen == 0 || fracLen > 3) {
            return false;
        }
        frac = std::stoll(cell.substr(fracStart, fracLen));
        for (size_t k = fracLen; k < 3; ++k) {
            frac *= 10;
        }
    }
    skipSpaces();
    if (i >= cell.size() || cell[i] != '/') {
        return false;
    }
    ++i;
    skipSpaces();
    if (cell.compare(i, std::string::npos, "MTok") != 0) {
        return false;
    }
    nano = whole * 1000 + frac;
    return nano <= kMaxPriceNano;
}

std::string PriceCatalog::modelNameToId(const std::string& nameIn) {
    std::string name = nameIn;
    size_t paren = name.find('(');
    if (paren != std::string::npos) {
        name.erase(paren);
    }
    name = trim(name);
    std::string id;
    for (char c : name) {
        if (c >= 'A' && c <= 'Z') {
            id.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            id.push_back(c);
        } else if (c == ' ' || c == '.' || c == '-') {
            id.push_back('-');
        } else {
            return std::string();
        }
    }
    if (id.size() <= 7 || id.compare(0, 7, "claude-") != 0 || id.back() == '-') {
        return std::string();
    }
    return id;
}

std::string PriceCatalog::modelsToJson(const std::map<std::string, ModelPrice>& models) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& kv : models) {
        j[kv.first] = modelJson(kv.second);
    }
    return j.dump(1);
}

bool PriceCatalog::modelsFromJson(const std::string& json, std::map<std::string, ModelPrice>& models,
                                  std::string& error) {
    models.clear();
    nlohmann::json doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        error = "model catalog is not a JSON object";
        return false;
    }
    std::map<std::string, ModelPrice> out;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        ModelPrice m;
        if (!modelFromJson(it.key(), it.value(), m, error)) {
            return false;
        }
        out[m.modelId] = m;
    }
    models = std::move(out);
    return true;
}

std::string PriceCatalog::sha256Hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return std::string();
    }
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
              EVP_DigestFinal_ex(ctx, digest, &len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) {
        return std::string();
    }
    static const char* const hex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(hex[digest[i] >> 4]);
        out.push_back(hex[digest[i] & 0x0f]);
    }
    return out;
}

PriceParseResult PriceCatalog::parsePage(const std::string& markdown) {
    PriceParseResult result;
    std::vector<std::string> lines = splitLines(markdown);
    std::vector<std::vector<std::string>> rows;
    std::string error;

    if (!readTable(lines, kMainHeading, kMainHeader, rows, error)) {
        result.error = error;
        return result;
    }

    std::map<std::string, std::vector<RowEntry>> entries;
    std::map<std::string, std::string> quarantine;
    int skippedRows = 0;
    for (const auto& cells : rows) {
        std::string baseName;
        int64_t threshold = 0;
        int kind = readTierAnnotation(cells[0], baseName, threshold);
        std::string id = modelNameToId(baseName);
        if (id.empty()) {
            ++skippedRows;
            continue;
        }
        if (kind < 0) {
            quarantine[id] = "unrecognized tier annotation";
            continue;
        }
        RowEntry e;
        e.kind = kind;
        e.threshold = threshold;
        e.cells = cells;
        entries[id].push_back(e);
    }

    std::map<std::string, ModelPrice> models;
    for (const auto& kv : entries) {
        const std::string& id = kv.first;
        const std::vector<RowEntry>& list = kv.second;
        if (quarantine.count(id)) {
            continue;
        }
        bool anyTier = false;
        bool anyPlain = false;
        for (const RowEntry& e : list) {
            (e.kind == 0 ? anyPlain : anyTier) = true;
        }
        std::string reason;
        ModelPrice m;
        m.modelId = id;
        if (anyTier && anyPlain) {
            reason = "mixed tiered and plain rows";
        } else if (!anyTier) {
            if (list.size() != 1) {
                reason = "duplicate model";
            } else {
                parseRateCells(list[0].cells, m, reason);
            }
        } else {
            const RowEntry* up = nullptr;
            const RowEntry* over = nullptr;
            int ups = 0;
            int overs = 0;
            for (const RowEntry& e : list) {
                if (e.kind == 1) { up = &e; ++ups; }
                if (e.kind == 2) { over = &e; ++overs; }
            }
            if (list.size() != 2 || ups != 1 || overs != 1 || up->threshold != over->threshold) {
                reason = "inconsistent tier rows";
            } else if (parseRateCells(up->cells, m, reason)) {
                auto upper = std::make_shared<ModelPrice>();
                upper->modelId = id;
                if (parseRateCells(over->cells, *upper, reason)) {
                    m.tierThresholdTokens = up->threshold;
                    m.upperTier = upper;
                }
            }
        }
        if (reason.empty()) {
            models[id] = m;
        } else {
            quarantine[id] = reason;
        }
    }
    for (const auto& q : quarantine) {
        result.quarantined.push_back({q.first, q.second});
    }
    result.skippedRows = skippedRows;

    if (models.size() < 3) {
        result.quarantined.clear();
        result.error = "fewer than 3 models found";
        return result;
    }
    if ((quarantine.size() + static_cast<size_t>(skippedRows)) * 4 > rows.size()) {
        result.quarantined.clear();
        result.error = "too many anomalous rows (" +
                       std::to_string(quarantine.size() + static_cast<size_t>(skippedRows)) + " of " +
                       std::to_string(rows.size()) + ")";
        return result;
    }

    rows.clear();
    if (!readTable(lines, kFastHeading, kFastHeader, rows, error)) {
        result.quarantined.clear();
        result.error = error;
        return result;
    }
    int skippedFast = 0;
    for (const auto& cells : rows) {
        int64_t fastIn = 0;
        int64_t fastOut = 0;
        std::vector<std::string> ids;
        bool rowOk = parsePriceCell(cells[1], fastIn) && parsePriceCell(cells[2], fastOut) && fastIn > 0 && fastOut > 0;
        size_t start = 0;
        while (rowOk) {
            size_t sep = cells[0].find(" / ", start);
            std::string part = cells[0].substr(start, sep == std::string::npos ? std::string::npos : sep - start);
            std::string id = modelNameToId(part);
            if (id.empty()) {
                rowOk = false;
                break;
            }
            ids.push_back(id);
            if (sep == std::string::npos) {
                break;
            }
            start = sep + 3;
        }
        if (!rowOk) {
            ++skippedFast;
            continue;
        }
        for (const std::string& id : ids) {
            auto it = models.find(id);
            if (it == models.end()) {
                ++skippedFast;
            } else {
                deriveFast(it->second, fastIn, fastOut);
            }
        }
    }

    result.ok = true;
    result.skippedFastEntries = skippedFast;
    result.models = std::move(models);
    return result;
}

std::string PriceCatalog::resolveModelId(const std::map<std::string, ModelPrice>& models,
                                         const std::string& modelId) {
    if (models.count(modelId)) {
        return modelId;
    }
    if (modelId.size() <= 9) {
        return std::string();
    }
    const size_t baseLen = modelId.size() - 9;
    if (modelId[baseLen] != '-') {
        return std::string();
    }
    for (size_t i = baseLen + 1; i < modelId.size(); ++i) {
        if (modelId[i] < '0' || modelId[i] > '9') {
            return std::string();
        }
    }
    std::string base = modelId.substr(0, baseLen);
    return models.count(base) ? base : std::string();
}

PriceCatalog::FetchFn PriceCatalog::httpFetcher() {
    return [](const std::string& urlIn) {
        PriceFetchResult out;
        std::string url = urlIn;
        for (int hop = 0; hop <= 3; ++hop) {
            const std::string scheme = "https://";
            if (url.compare(0, scheme.size(), scheme) != 0) {
                out.error = "only https URLs are allowed";
                return out;
            }
            size_t pathPos = url.find('/', scheme.size());
            std::string authority = url.substr(scheme.size(), pathPos == std::string::npos
                                                                  ? std::string::npos
                                                                  : pathPos - scheme.size());
            std::string path = pathPos == std::string::npos ? "/" : url.substr(pathPos);
            int port = 443;
            std::string host = authority;
            size_t colon = authority.rfind(':');
            if (colon != std::string::npos) {
                host = authority.substr(0, colon);
                port = std::atoi(authority.c_str() + colon + 1);
            }
            if (host.empty() || port <= 0 || port > 65535) {
                out.error = "invalid URL";
                return out;
            }

            httplib::SSLClient cli(host, port);
            cli.set_connection_timeout(10, 0);
            cli.set_read_timeout(20, 0);
            cli.enable_server_certificate_verification(true);
            cli.set_follow_location(false);

            std::string body;
            bool tooLarge = false;
            httplib::Headers headers = {{"User-Agent", "aimon-price-catalog/1.0"},
                                        {"Accept", "text/markdown, text/plain"}};
            auto res = cli.Get(
                path, headers,
                [](const httplib::Response&) { return true; },
                [&](const char* data, size_t len) {
                    if (body.size() + len > PriceCatalog::kMaxBodyBytes) {
                        tooLarge = true;
                        return false;
                    }
                    body.append(data, len);
                    return true;
                });
            if (tooLarge) {
                out.error = "response too large";
                return out;
            }
            if (!res) {
                out.error = "request failed (error code " + std::to_string(static_cast<int>(res.error())) + ")";
                return out;
            }
            out.status = res->status;
            if (res->status == 301 || res->status == 302 || res->status == 303 ||
                res->status == 307 || res->status == 308) {
                std::string location = res->get_header_value("Location");
                if (location.empty()) {
                    out.error = "redirect without Location";
                    return out;
                }
                if (location[0] == '/') {
                    url = scheme + authority + location;
                } else if (location.compare(0, scheme.size(), scheme + host) == 0) {
                    url = location;
                } else {
                    out.error = "redirect to a different host refused";
                    return out;
                }
                continue;
            }
            out.body = std::move(body);
            return out;
        }
        out.error = "too many redirects";
        return out;
    };
}

bool PriceCatalog::writeCache(const std::string& pageSha, int64_t fetchedAt, std::string& error) const {
    nlohmann::json models = nlohmann::json::object();
    for (const auto& kv : _models) {
        models[kv.first] = modelJson(kv.second);
    }
    nlohmann::json quarantined = nlohmann::json::array();
    for (const auto& q : _quarantined) {
        quarantined.push_back({{"model", q.modelId}, {"reason", q.reason}});
    }
    nlohmann::json doc = {{"source_url", _url}, {"fetched_at", fetchedAt},
                          {"page_sha256", pageSha}, {"models", models}, {"quarantined", quarantined}};
    std::string text = doc.dump(1) + "\n";

    std::string dir = fs::path(_cachePath).parent_path().string();
    if (!dir.empty() && !PathUtils::ensureDirectoryExists(dir)) {
        error = "cannot create directory " + dir;
        return false;
    }
    std::string tmp = _cachePath + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        error = std::string("cannot open temp file: ") + std::strerror(errno);
        return false;
    }
    bool ok = ::fchmod(fd, 0600) == 0;
    size_t written = 0;
    while (ok && written < text.size()) {
        ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
        } else {
            written += static_cast<size_t>(n);
        }
    }
    ok = ok && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), _cachePath.c_str()) != 0) {
        error = std::string("cannot write cache: ") + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    if (!dir.empty()) {
        int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            ::fsync(dfd);
            ::close(dfd);
        }
    }
    return true;
}

bool PriceCatalog::loadCache() {
    std::lock_guard<std::mutex> lock(_mutex);
    struct stat st;
    if (::stat(_cachePath.c_str(), &st) != 0) {
        return false;  // no cache yet is normal on a first run
    }
    std::string text;
    if (!readFileBounded(_cachePath, kMaxCacheBytes, text)) {
        _lastError = "cache unreadable or too large";
        return false;
    }
    nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("models") || !doc["models"].is_object()) {
        _lastError = "cache is not valid JSON of the expected shape";
        return false;
    }
    int64_t fetchedAt = 0;
    if (!jsonInt(doc, "fetched_at", fetchedAt) || fetchedAt <= 0 ||
        !doc.contains("page_sha256") || !doc["page_sha256"].is_string()) {
        _lastError = "cache header is invalid";
        return false;
    }
    std::map<std::string, ModelPrice> models;
    for (auto it = doc["models"].begin(); it != doc["models"].end(); ++it) {
        ModelPrice m;
        std::string error;
        if (!modelFromJson(it.key(), it.value(), m, error)) {
            _lastError = "cache entry rejected: " + error;
            return false;
        }
        models[m.modelId] = m;
    }
    std::vector<PriceQuarantine> quarantined;
    if (doc.contains("quarantined") && doc["quarantined"].is_array()) {
        for (const auto& q : doc["quarantined"]) {
            if (q.is_object() && q.contains("model") && q["model"].is_string() &&
                q.contains("reason") && q["reason"].is_string()) {
                quarantined.push_back({q["model"].get<std::string>(), q["reason"].get<std::string>()});
            }
        }
    }
    if (models.size() < 3) {
        _lastError = "cache has fewer than 3 models";
        return false;
    }
    _models = std::move(models);
    _quarantined = std::move(quarantined);
    _fetchedAtEpoch = fetchedAt;
    _pageSha256 = doc["page_sha256"].get<std::string>();
    _lastError.clear();
    return true;
}

bool PriceCatalog::refresh(int64_t nowEpoch, bool* changed) {
    if (changed) {
        *changed = false;
    }
    PriceFetchResult res = _fetch ? _fetch(_url) : PriceFetchResult();
    std::string failure;
    if (!res.error.empty()) {
        failure = "fetch failed: " + res.error;
    } else if (res.status != 200) {
        failure = "fetch failed: HTTP " + std::to_string(res.status);
    } else if (res.body.size() > kMaxBodyBytes) {
        failure = "fetch failed: response too large";
    }
    PriceParseResult parsed;
    if (failure.empty()) {
        parsed = parsePage(res.body);
        if (!parsed.ok) {
            failure = "page rejected: " + parsed.error;
        }
    }
    if (!failure.empty()) {
        std::lock_guard<std::mutex> lock(_mutex);
        _lastError = failure;
        return false;
    }

    std::string sha = sha256Hex(res.body);
    std::lock_guard<std::mutex> lock(_mutex);
    if (changed) {
        *changed = (sha != _pageSha256);
    }
    _models = std::move(parsed.models);
    _quarantined = std::move(parsed.quarantined);
    _fetchedAtEpoch = nowEpoch;
    _pageSha256 = sha;
    std::string cacheError;
    if (!writeCache(sha, nowEpoch, cacheError)) {
        _lastError = "cache write failed: " + cacheError;
    } else {
        _lastError.clear();
    }
    return true;
}

std::optional<ModelPrice> PriceCatalog::lookup(const std::string& modelId) const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string id = resolveModelId(_models, modelId);
    if (id.empty()) {
        return std::nullopt;
    }
    return _models.at(id);
}

std::map<std::string, ModelPrice> PriceCatalog::snapshot() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _models;
}

PriceCatalogStatus PriceCatalog::status(int64_t nowEpoch, int refreshHours) const {
    std::lock_guard<std::mutex> lock(_mutex);
    PriceCatalogStatus s;
    s.sourceUrl = _url;
    s.fetchedAtEpoch = _fetchedAtEpoch;
    s.pageSha256 = _pageSha256;
    s.modelsLoaded = _models.size();
    s.quarantinedCount = _quarantined.size();
    int64_t hours = refreshHours > 0 ? refreshHours : 1;
    s.stale = _fetchedAtEpoch == 0 || (nowEpoch - _fetchedAtEpoch) > 3 * hours * 3600;
    s.error = _lastError;
    return s;
}

void PriceCatalog::capture(int64_t nowEpoch, int refreshHours, PriceCatalogStatus& out,
                           std::map<std::string, ModelPrice>& models) const {
    std::lock_guard<std::mutex> lock(_mutex);
    out.sourceUrl = _url;
    out.fetchedAtEpoch = _fetchedAtEpoch;
    out.pageSha256 = _pageSha256;
    out.modelsLoaded = _models.size();
    out.quarantinedCount = _quarantined.size();
    int64_t hours = refreshHours > 0 ? refreshHours : 1;
    out.stale = _fetchedAtEpoch == 0 || (nowEpoch - _fetchedAtEpoch) > 3 * hours * 3600;
    out.error = _lastError;
    models = _models;
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
