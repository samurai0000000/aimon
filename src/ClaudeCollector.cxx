/*
 * ClaudeCollector.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "ClaudeCollector.hxx"
#include "ClaudeAccount.hxx"
#include "PathUtils.hxx"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace aimon {

namespace {

const int64_t kMaxEpoch = 4102444800LL;          // 2100-01-01T00:00:00Z, exclusive
const size_t kBatchRows = 1000;
const int64_t kRefreshRetryAfterSec = 600;
const int64_t kPruneEveryNSec = 3600;

bool allDigits(const std::string& s, size_t from, size_t count) {
    if (from + count > s.size()) {
        return false;
    }
    for (size_t i = from; i < from + count; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

int num(const std::string& s, size_t from, size_t count) {
    return std::stoi(s.substr(from, count));
}

// "YYYY-MM-DDTHH:MM:SS" with optional ".fraction" and a mandatory "Z".
bool parseIsoUtc(const std::string& ts, int64_t& epoch) {
    if (ts.size() < 20 || ts.back() != 'Z' || !allDigits(ts, 0, 4) || ts[4] != '-' || !allDigits(ts, 5, 2) ||
        ts[7] != '-' || !allDigits(ts, 8, 2) || ts[10] != 'T' || !allDigits(ts, 11, 2) || ts[13] != ':' ||
        !allDigits(ts, 14, 2) || ts[16] != ':' || !allDigits(ts, 17, 2)) {
        return false;
    }
    if (ts.size() > 20) {
        if (ts[19] != '.' || ts.size() < 22 || !allDigits(ts, 20, ts.size() - 21)) {
            return false;
        }
    }
    const int month = num(ts, 5, 2);
    const int day = num(ts, 8, 2);
    const int hour = num(ts, 11, 2);
    const int minute = num(ts, 14, 2);
    const int second = num(ts, 17, 2);
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    struct tm tmv;
    std::memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = num(ts, 0, 4) - 1900;
    tmv.tm_mon = month - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = hour;
    tmv.tm_min = minute;
    tmv.tm_sec = second;
    epoch = static_cast<int64_t>(timegm(&tmv));
    return true;
}

int64_t intField(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) {
        return 0;
    }
    if (it->is_number_unsigned()) {
        uint64_t u = it->get<uint64_t>();
        return u > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(u);
    }
    return it->get<int64_t>();
}

bool tokensOk(int64_t v) {
    return v >= 0 && v <= ClaudeUsageStore::kMaxTokensPerField;
}

std::string stringField(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

std::string formatIsoDate(int64_t epoch) {
    time_t t = static_cast<time_t>(epoch);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tmv.tm_year + 1900, tmv.tm_mon + 1,
                  tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

int64_t startOfUtcDay(int64_t epoch) {
    return epoch - (epoch % 86400);
}

int64_t dayStringToMs(const std::string& day) {
    if (day.size() != 10) {
        return 0;
    }
    struct tm tmv;
    std::memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = std::atoi(day.substr(0, 4).c_str()) - 1900;
    tmv.tm_mon = std::atoi(day.substr(5, 2).c_str()) - 1;
    tmv.tm_mday = std::atoi(day.substr(8, 2).c_str());
    return static_cast<int64_t>(timegm(&tmv)) * 1000;
}

ClaudeWindowTotals toWindow(const std::string& label, int64_t from, int64_t to,
                            const std::vector<ClaudeUsageBucket>& buckets) {
    ClaudeUsageTotals t = ClaudeUsageStore::sum(buckets);
    ClaudeWindowTotals w;
    w.label = label;
    w.fromEpoch = from;
    w.toEpoch = to;
    w.messages = t.messages;
    w.input = t.input;
    w.output = t.output;
    w.cacheRead = t.cacheRead;
    w.cacheWrite5m = t.cacheWrite5m;
    w.cacheWrite1h = t.cacheWrite1h;
    w.costNano = t.costNano;
    w.unpricedMessages = t.unpricedMessages;
    w.costOverflow = t.costOverflow;
    return w;
}

int64_t mtimeNanos(const struct stat& st) {
    return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
}

std::string formatYmdCompact(int64_t epoch) {
    time_t t = static_cast<time_t>(epoch);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return buf;
}

std::string formatYmdDashLocal(int64_t epoch) {
    time_t t = static_cast<time_t>(epoch);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return buf;
}

std::string formatYmdDashUtc(int64_t epoch) {
    time_t t = static_cast<time_t>(epoch);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return buf;
}

std::string findCcusageBinary() {
    static const char* kCandidates[] = {
        "/usr/lib/node_modules/ccusage/node_modules/@ccusage/ccusage-linux-x64/bin/ccusage",
        "/usr/local/lib/node_modules/ccusage/node_modules/@ccusage/ccusage-linux-x64/bin/ccusage",
        "/usr/bin/ccusage",
        "/usr/local/bin/ccusage"
    };
    for (const char* path : kCandidates) {
        if (::access(path, X_OK) == 0) {
            return path;
        }
    }
    return "";
}

bool runCcusageSubprocess(const std::string& configDir,
                          const std::vector<std::string>& args,
                          std::string& outJson) {
    outJson.clear();
    std::string bin = findCcusageBinary();
    if (bin.empty()) {
        return false;
    }

    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) != 0) {
        return false;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        int devNull = ::open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            ::dup2(devNull, STDERR_FILENO);
            ::close(devNull);
        }
        ::close(pipefd[1]);

        if (!configDir.empty()) {
            ::setenv("CLAUDE_CONFIG_DIR", configDir.c_str(), 1);
        }
        ::setenv("NO_COLOR", "1", 1);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(bin.c_str()));
        for (const std::string& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);

        ::execv(bin.c_str(), argv.data());
        _exit(127);
    }

    ::close(pipefd[1]);
    char buf[8192];
    while (true) {
        ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        outJson.append(buf, static_cast<size_t>(n));
        if (outJson.size() > 16u * 1024u * 1024u) {
            break;
        }
    }
    ::close(pipefd[0]);

    int wstatus = 0;
    if (::waitpid(pid, &wstatus, 0) < 0) {
        return false;
    }
    return WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0 && !outJson.empty();
}

} // namespace

ClaudeLineResult ClaudeCollector::parseLine(const std::string& line, const std::string& account,
                                            ClaudeUsageRow& row) {
    nlohmann::json d = nlohmann::json::parse(line, nullptr, false);
    if (d.is_discarded() || !d.is_object() || stringField(d, "type") != "assistant") {
        return ClaudeLineResult::Ignored;
    }
    auto mit = d.find("message");
    if (mit == d.end() || !mit->is_object()) {
        return ClaudeLineResult::Ignored;
    }
    const nlohmann::json& m = *mit;
    auto uit = m.find("usage");
    if (uit == m.end() || !uit->is_object()) {
        return ClaudeLineResult::Ignored;
    }
    const nlohmann::json& u = *uit;
    if (stringField(m, "model") == "<synthetic>") {
        return ClaudeLineResult::Ignored;
    }

    const std::string messageId = stringField(m, "id");
    const std::string requestId = stringField(d, "requestId");
    const std::string model = stringField(m, "model");
    const std::string timestamp = stringField(d, "timestamp");
    int64_t epoch = 0;
    if (messageId.empty() || requestId.empty() || model.empty() ||
        messageId.size() > ClaudeUsageStore::kMaxFieldLength || requestId.size() > ClaudeUsageStore::kMaxFieldLength ||
        model.size() > ClaudeUsageStore::kMaxFieldLength || !parseIsoUtc(timestamp, epoch) || epoch <= 0 ||
        epoch >= kMaxEpoch) {
        return ClaudeLineResult::Skipped;
    }

    ClaudeUsageRow r;
    r.account = account;
    r.messageId = messageId;
    r.requestId = requestId;
    r.model = model;
    r.timestamp = epoch;
    r.sessionId = stringField(d, "sessionId");
    if (r.sessionId.empty() || r.sessionId.size() > ClaudeUsageStore::kMaxFieldLength) {
        r.sessionId = "unknown";
    }
    r.speed = stringField(u, "speed") == "fast" ? "fast" : "standard";
    r.input = intField(u, "input_tokens");
    r.output = intField(u, "output_tokens");
    r.cacheRead = intField(u, "cache_read_input_tokens");
    auto cc = u.find("cache_creation");
    if (cc != u.end() && cc->is_object()) {
        r.cacheWrite5m = intField(*cc, "ephemeral_5m_input_tokens");
        r.cacheWrite1h = intField(*cc, "ephemeral_1h_input_tokens");
    } else {
        r.cacheWrite5m = intField(u, "cache_creation_input_tokens");
        r.creationSplitMissing = true;
    }
    auto tool = u.find("server_tool_use");
    if (tool != u.end() && tool->is_object()) {
        r.webSearch = intField(*tool, "web_search_requests");
    }
    auto side = d.find("isSidechain");
    r.sidechain = side != d.end() && side->is_boolean() && side->get<bool>();

    if (!tokensOk(r.input) || !tokensOk(r.output) || !tokensOk(r.cacheRead) || !tokensOk(r.cacheWrite5m) ||
        !tokensOk(r.cacheWrite1h) || !tokensOk(r.webSearch)) {
        return ClaudeLineResult::Skipped;
    }
    row = r;
    return ClaudeLineResult::Row;
}

ClaudeCollector::ClaudeCollector(const ClaudeConfig& config, ClaudeUsageStore& store, PriceCatalog& catalog)
    : _config(config), _store(store), _catalog(catalog) {
}

ClaudeCollector::~ClaudeCollector() {
    stopPriceRefresh();
}

void ClaudeCollector::setCcusageRunnerFn(CcusageRunnerFn fn) {
    std::lock_guard<std::mutex> lock(_mutex);
    _ccusageRunnerFn = std::move(fn);
}

bool ClaudeCollector::shouldUseCcusage(int64_t nowEpoch) const {
    if (_ccusageRunnerFn) {
        return true;
    }
    if (!_config.pricingUrl.empty()) {
        return false;
    }
    const int64_t wallNow = static_cast<int64_t>(std::time(nullptr));
    return std::llabs(wallNow - nowEpoch) < 3600;
}

bool ClaudeCollector::tryOverlayFromCcusage(const ClaudeAccountConfig& acct, int64_t nowEpoch,
                                            bool cycleConfigured, const ClaudeCycle& cycle,
                                            ClaudeAccountStatus& as) {
    const std::string expandedCfgDir = PathUtils::expandHome(acct.configDir);
    auto runner = _ccusageRunnerFn ? _ccusageRunnerFn : runCcusageSubprocess;

    int64_t earliestEpoch = nowEpoch - static_cast<int64_t>(_config.windowDays + 2) * 86400;
    if (cycleConfigured && cycle.startEpoch > 0 && cycle.startEpoch - 86400 < earliestEpoch) {
        earliestEpoch = cycle.startEpoch - 86400;
    }
    const std::string sinceYmd = formatYmdCompact(earliestEpoch);

    std::string dailyJsonStr;
    if (!runner(expandedCfgDir, {"claude", "daily", "--json", "-O", "-s", sinceYmd}, dailyJsonStr)) {
        return false;
    }

    nlohmann::json dailyDoc = nlohmann::json::parse(dailyJsonStr, nullptr, false);
    if (dailyDoc.is_discarded() || !dailyDoc.is_object() ||
        !dailyDoc.contains("daily") || !dailyDoc["daily"].is_array()) {
        return false;
    }

    const auto& dailyArr = dailyDoc["daily"];
    if (dailyArr.empty() && as.rowsInStore == 0) {
        return false;
    }

    const std::string todayStr = formatYmdDashLocal(nowEpoch);
    const std::string last7dStartStr = formatYmdDashLocal(nowEpoch - 7 * 86400);
    const std::string windowStartStr = formatYmdDashLocal(nowEpoch - static_cast<int64_t>(_config.windowDays) * 86400);
    const std::string cycleStartStr = cycleConfigured ? formatYmdDashUtc(cycle.startEpoch) : "";
    const std::string cycleEndStr = cycleConfigured ? formatYmdDashUtc(cycle.endEpoch) : "";

    std::map<std::string, int64_t> storeMsgsByDay;
    for (const auto& d : as.daily) {
        storeMsgsByDay[d.dayStr] = d.messages;
    }
    std::map<std::string, int64_t> storeMsgsByModel;
    for (const auto& m : as.models) {
        storeMsgsByModel[m.model] = m.messages;
    }

    auto resetWindowTokensAndCost = [](ClaudeWindowTotals& w) {
        w.input = 0;
        w.output = 0;
        w.cacheRead = 0;
        w.cacheWrite5m = 0;
        w.cacheWrite1h = 0;
        w.costNano = 0;
        w.unpricedMessages = 0;
        w.costOverflow = false;
    };

    resetWindowTokensAndCost(as.today);
    resetWindowTokensAndCost(as.last7d);
    resetWindowTokensAndCost(as.window);
    if (cycleConfigured) {
        resetWindowTokensAndCost(as.cycle);
    }

    std::map<std::string, ClaudeModelUsage> byModel;
    std::vector<ClaudeDailyPoint> newDaily;

    auto addDayToWindow = [](ClaudeWindowTotals& w, int64_t inTok, int64_t outTok,
                             int64_t cacheCreateTok, int64_t cacheReadTok, int64_t costNano) {
        w.input += inTok;
        w.output += outTok;
        w.cacheWrite5m += cacheCreateTok;
        w.cacheRead += cacheReadTok;
        w.costNano += costNano;
    };

    for (const auto& item : dailyArr) {
        if (!item.is_object()) continue;
        std::string dateStr = stringField(item, "date");
        if (dateStr.size() != 10) continue;

        int64_t inTok = intField(item, "inputTokens");
        int64_t outTok = intField(item, "outputTokens");
        int64_t cacheCreateTok = intField(item, "cacheCreationTokens");
        int64_t cacheReadTok = intField(item, "cacheReadTokens");
        double totalCostUsd = (item.contains("totalCost") && item["totalCost"].is_number())
            ? item["totalCost"].get<double>() : 0.0;
        int64_t costNano = static_cast<int64_t>(std::llround(totalCostUsd * 1e9));

        if (dateStr == todayStr) {
            addDayToWindow(as.today, inTok, outTok, cacheCreateTok, cacheReadTok, costNano);
        }
        if (dateStr >= last7dStartStr && dateStr <= todayStr) {
            addDayToWindow(as.last7d, inTok, outTok, cacheCreateTok, cacheReadTok, costNano);
        }
        if (cycleConfigured && dateStr >= cycleStartStr && dateStr < cycleEndStr) {
            addDayToWindow(as.cycle, inTok, outTok, cacheCreateTok, cacheReadTok, costNano);
        }
        if (dateStr >= windowStartStr && dateStr <= todayStr) {
            addDayToWindow(as.window, inTok, outTok, cacheCreateTok, cacheReadTok, costNano);

            ClaudeDailyPoint dp;
            dp.dayStr = dateStr;
            dp.dayMs = dayStringToMs(dateStr);
            auto mit = storeMsgsByDay.find(dateStr);
            dp.messages = (mit != storeMsgsByDay.end() && mit->second > 0) ? mit->second : 1;
            dp.tokens = inTok + outTok + cacheCreateTok + cacheReadTok;
            dp.costNano = costNano;
            newDaily.push_back(dp);

            if (item.contains("modelBreakdowns") && item["modelBreakdowns"].is_array()) {
                for (const auto& mb : item["modelBreakdowns"]) {
                    if (!mb.is_object()) continue;
                    std::string modelName = stringField(mb, "modelName");
                    if (modelName.empty()) continue;

                    int64_t mIn = intField(mb, "inputTokens");
                    int64_t mOut = intField(mb, "outputTokens");
                    int64_t mCC = intField(mb, "cacheCreationTokens");
                    int64_t mCR = intField(mb, "cacheReadTokens");
                    double mCostUsd = (mb.contains("cost") && mb["cost"].is_number())
                        ? mb["cost"].get<double>() : 0.0;
                    int64_t mCostNano = static_cast<int64_t>(std::llround(mCostUsd * 1e9));

                    ClaudeModelUsage& mu = byModel[modelName];
                    mu.model = modelName;
                    mu.input += mIn;
                    mu.output += mOut;
                    mu.cacheWrite5m += mCC;
                    mu.cacheRead += mCR;
                    mu.costNano += mCostNano;
                }
            }
        }
    }

    if (as.today.totalTokens() > 0 && as.today.messages == 0) as.today.messages = 1;
    if (as.last7d.totalTokens() > 0 && as.last7d.messages == 0) as.last7d.messages = 1;
    if (as.window.totalTokens() > 0 && as.window.messages == 0) as.window.messages = 1;
    if (cycleConfigured) {
        if (as.cycle.totalTokens() > 0 && as.cycle.messages == 0) as.cycle.messages = 1;
        if (acct.spendLimitUsd > 0.0 && !as.cycle.costOverflow) {
            as.estPctOfLimit = claudeNanoToUsd(as.cycle.costNano) / acct.spendLimitUsd * 100.0;
        } else {
            as.estPctOfLimit = 0.0;
        }
    }

    as.daily = std::move(newDaily);
    as.models.clear();
    for (auto& kv : byModel) {
        auto smIt = storeMsgsByModel.find(kv.first);
        if (smIt != storeMsgsByModel.end() && smIt->second > 0) {
            kv.second.messages = smIt->second;
        } else {
            // Try matching prefix/short model id in storeMsgsByModel
            for (const auto& sm : storeMsgsByModel) {
                if (sm.first.find(kv.first) != std::string::npos || kv.first.find(sm.first) != std::string::npos) {
                    kv.second.messages += sm.second;
                }
            }
            if (kv.second.messages == 0 &&
                (kv.second.input + kv.second.output + kv.second.cacheRead + kv.second.cacheWrite5m + kv.second.cacheWrite1h) > 0) {
                kv.second.messages = 1;
            }
        }
        as.models.push_back(std::move(kv.second));
    }
    std::sort(as.models.begin(), as.models.end(), [](const ClaudeModelUsage& a, const ClaudeModelUsage& b) {
        return a.costNano != b.costNano ? a.costNano > b.costNano : a.model < b.model;
    });
    as.unpricedModels.clear();

    // Query active 5-hour session block via `ccusage claude blocks --json --active -O`
    std::string blocksJsonStr;
    if (runner(expandedCfgDir, {"claude", "blocks", "--json", "--active", "-O"}, blocksJsonStr)) {
        nlohmann::json blocksDoc = nlohmann::json::parse(blocksJsonStr, nullptr, false);
        if (!blocksDoc.is_discarded() && blocksDoc.is_object() &&
            blocksDoc.contains("blocks") && blocksDoc["blocks"].is_array() &&
            !blocksDoc["blocks"].empty()) {
            resetWindowTokensAndCost(as.last5h);
            for (const auto& blk : blocksDoc["blocks"]) {
                if (!blk.is_object()) continue;
                if (blk.contains("isGap") && blk["isGap"].is_boolean() && blk["isGap"].get<bool>()) {
                    continue;
                }
                if (blk.contains("tokenCounts") && blk["tokenCounts"].is_object()) {
                    const auto& tc = blk["tokenCounts"];
                    as.last5h.input += intField(tc, "inputTokens");
                    as.last5h.output += intField(tc, "outputTokens");
                    as.last5h.cacheWrite5m += intField(tc, "cacheCreationInputTokens");
                    as.last5h.cacheRead += intField(tc, "cacheReadInputTokens");
                }
                if (blk.contains("costUSD") && blk["costUSD"].is_number()) {
                    as.last5h.costNano += static_cast<int64_t>(std::llround(blk["costUSD"].get<double>() * 1e9));
                }
            }
            if (as.last5h.totalTokens() > 0 && as.last5h.messages == 0) {
                as.last5h.messages = 1;
            }
        }
    }

    as.hasData = as.hasData || !dailyArr.empty() || as.window.totalTokens() > 0;
    return true;
}

ClaudeCollector::ScanResult ClaudeCollector::scanFile(const std::string& account, const std::string& relPath,
                                                      const std::string& fullPath, const ClaudeFileCursor* cursor) {
    ScanResult res;
    int fd = ::open(fullPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        res.ok = false;
        res.error = std::string("cannot open a transcript: ") + std::strerror(errno);
        return res;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return res;
    }
    const int64_t size = static_cast<int64_t>(st.st_size);
    const int64_t mtime = mtimeNanos(st);
    int64_t offset = cursor ? cursor->offset : 0;
    if (cursor && size < offset) {
        offset = 0;                                     // shrunk or replaced: read it again
    } else if (cursor && cursor->size == size && cursor->mtime == mtime) {
        ::close(fd);                                    // nothing changed since the last scan
        return res;
    }
    if (offset > 0 && ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
        ::close(fd);
        res.ok = false;
        res.error = std::string("cannot seek in a transcript: ") + std::strerror(errno);
        return res;
    }

    std::vector<ClaudeUsageRow> batch;
    std::string pending;
    bool oversize = false;
    int64_t committed = offset;          // file offset just after the last complete line
    int64_t position = offset;           // file offset of the next byte to read
    bool hitLimit = false;
    std::string chunk(kReadChunkBytes, '\0');

    auto flush = [&]() {
        if (batch.empty()) {
            return true;
        }
        ClaudeIngestResult r = _store.ingest(batch);
        batch.clear();
        if (!r.ok) {
            res.ok = false;
            res.error = "cannot store usage rows: " + r.error;
            return false;
        }
        return true;
    };

    while (res.ok) {
        if (res.bytesRead >= kMaxBytesPerFilePerPoll) {
            hitLimit = true;
            break;
        }
        ssize_t n = ::read(fd, &chunk[0], chunk.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            res.ok = false;
            res.error = std::string("cannot read a transcript: ") + std::strerror(errno);
            break;
        }
        if (n == 0) {
            break;
        }
        res.bytesRead += n;
        size_t start = 0;
        for (size_t i = 0; i < static_cast<size_t>(n); ++i) {
            if (chunk[i] != '\n') {
                continue;
            }
            if (!oversize) {
                pending.append(chunk, start, i - start);
            }
            if (oversize || pending.size() > kMaxLineBytes) {
                ++res.skippedLines;
            } else {
                if (!pending.empty() && pending.back() == '\r') {
                    pending.pop_back();
                }
                ClaudeUsageRow row;
                ClaudeLineResult lr = parseLine(pending, account, row);
                if (lr == ClaudeLineResult::Row) {
                    batch.push_back(std::move(row));
                } else if (lr == ClaudeLineResult::Skipped) {
                    ++res.skippedLines;
                }
            }
            pending.clear();
            oversize = false;
            start = i + 1;
            committed = position + static_cast<int64_t>(i) + 1;
            if (batch.size() >= kBatchRows && !flush()) {
                break;
            }
        }
        if (!res.ok) {
            break;
        }
        if (!oversize && start < static_cast<size_t>(n)) {
            pending.append(chunk, start, static_cast<size_t>(n) - start);
            if (pending.size() > kMaxLineBytes) {
                oversize = true;                        // drop the rest of this line; counted at its newline
                pending.clear();
                pending.shrink_to_fit();
            }
        }
        position += n;
    }
    ::close(fd);

    if (res.ok && !flush()) {
        return res;
    }
    if (!res.ok) {
        return res;                                     // the cursor is not advanced: the next poll retries
    }

    ClaudeFileCursor next;
    next.account = account;
    next.path = relPath;
    next.offset = committed;
    next.size = hitLimit ? committed : size;            // a size mismatch forces the next poll to continue
    next.mtime = mtime;
    if (!_store.saveCursor(next)) {
        res.ok = false;
        res.error = "cannot save the read position of a transcript";
    }
    return res;
}

void ClaudeCollector::recordPriceVersion(int64_t nowEpoch) {
    PriceCatalogStatus st;
    std::map<std::string, ModelPrice> models;
    _catalog.capture(nowEpoch, _config.pricingRefreshHours, st, models);
    if (!models.empty() && st.fetchedAtEpoch > 0 && !st.pageSha256.empty()) {
        _store.saveCatalogVersion(st.fetchedAtEpoch, st.pageSha256, models);
    }
}

ClaudeStatus ClaudeCollector::fetchStatus(int64_t nowEpoch) {
    std::lock_guard<std::mutex> lock(_mutex);
    ClaudeStatus status;
    status.enabled = _config.enabled;

    recordPriceVersion(nowEpoch);
    std::vector<PriceVersion> versions;
    std::string versionError;
    if (!_store.loadCatalogVersions(versions, versionError)) {
        versions.clear();
    }
    PriceCatalogStatus ps;
    {
        std::map<std::string, ModelPrice> ignored;
        _catalog.capture(nowEpoch, _config.pricingRefreshHours, ps, ignored);
    }
    status.pricing.sourceUrl = ps.sourceUrl;
    status.pricing.fetchedAtEpoch = ps.fetchedAtEpoch;
    status.pricing.stale = ps.stale;
    status.pricing.modelsLoaded = ps.modelsLoaded;
    status.pricing.quarantinedCount = ps.quarantinedCount;
    status.pricing.versionCount = versions.size();
    status.pricing.error = !ps.error.empty() ? ps.error : versionError;
    if (status.pricing.error.empty() && versions.empty()) {
        status.pricing.error = "no price catalog loaded yet";
    }

    // Phase 1: read new transcript bytes for every account.
    std::vector<std::pair<ClaudeAccountConfig, ClaudeAccountStatus>> scanned;
    for (const ClaudeAccountConfig& acct : _config.resolvedAccounts()) {
        ClaudeAccountStatus as;
        as.name = acct.name;
        as.windowDays = _config.windowDays;
        as.spendLimitUsd = acct.spendLimitUsd;

        ClaudeTierInfo tier = detectClaudeTier(acct.configDir);
        as.tier = tier.tier;
        as.rawSubscriptionType = tier.rawSubscriptionType;
        as.rawRateLimitTier = tier.rawRateLimitTier;
        as.warning = tier.warning;

        const fs::path cfgPath = PathUtils::expandHome(acct.configDir);
        const fs::path projects = cfgPath / "projects";
        std::error_code ec;
        std::string scanError;
        std::vector<fs::path> files;
        if (!fs::is_directory(cfgPath, ec)) {
            scanError = "Claude config directory not found: " + acct.configDir;
        } else if (!fs::is_directory(projects, ec)) {
            scanError = "no projects directory in the Claude config directory: " + acct.configDir;
        } else if (::access(projects.c_str(), R_OK | X_OK) != 0) {
            scanError = "the transcripts directory is not readable: " + acct.configDir + "/projects";
        } else {
            fs::recursive_directory_iterator it(projects, fs::directory_options::skip_permission_denied, ec);
            if (ec) {
                scanError = "cannot list transcripts: " + ec.message();
            } else {
                for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    if (ec) {
                        scanError = "cannot list transcripts: " + ec.message();
                        break;
                    }
                    std::error_code ec2;
                    if (it->is_regular_file(ec2) && !ec2 && it->path().extension() == ".jsonl") {
                        files.push_back(it->path());
                    }
                }
            }
        }
        std::sort(files.begin(), files.end());

        if (scanError.empty()) {
            std::map<std::string, ClaudeFileCursor> cursors;
            for (const ClaudeFileCursor& c : _store.loadCursors(acct.name)) {
                cursors[c.path] = c;
            }
            for (const fs::path& file : files) {
                const std::string rel = fs::relative(file, projects, ec).string();
                auto cit = cursors.find(rel);
                ScanResult sr = scanFile(acct.name, rel, file.string(), cit == cursors.end() ? nullptr : &cit->second);
                as.bytesReadLastPoll += sr.bytesRead;
                as.skippedLines += sr.skippedLines;
                ++as.filesScanned;
                if (!sr.ok && scanError.empty()) {
                    scanError = sr.error;
                }
            }
        }
        as.errorMessage = scanError;
        scanned.emplace_back(acct, std::move(as));
    }

    // Retention runs after ingestion so old lines never outlive their window.
    if (nowEpoch - _lastPruneEpoch >= kPruneEveryNSec) {
        _store.pruneOlderThan(nowEpoch - static_cast<int64_t>(_config.retentionDays) * 86400);
        _lastPruneEpoch = nowEpoch;
    }

    // Phase 2: aggregate and build the status.
    for (auto& entry : scanned) {
        const ClaudeAccountConfig& acct = entry.first;
        ClaudeAccountStatus& as = entry.second;
        as.rowsInStore = _store.rowCount(acct.name);
        as.hasData = as.rowsInStore > 0;

        const int64_t to = nowEpoch + 1;
        std::string aggError;
        std::vector<ClaudeUsageBucket> buckets;
        auto agg = [&](const std::string& label, int64_t from, int64_t until, ClaudeWindowTotals& out) {
            if (_store.aggregate(acct.name, from, until, versions, buckets, aggError)) {
                out = toWindow(label, from, until, buckets);
            } else {
                out = ClaudeWindowTotals();
                out.label = label;
                if (as.errorMessage.empty()) {
                    as.errorMessage = "cannot aggregate usage: " + aggError;
                }
            }
        };
        agg("last_5h", nowEpoch - 5 * 3600, to, as.last5h);
        agg("last_7d", nowEpoch - 7 * 86400, to, as.last7d);
        agg("today", startOfUtcDay(nowEpoch), to, as.today);

        const int64_t windowFrom = nowEpoch - static_cast<int64_t>(_config.windowDays) * 86400;
        agg("window", windowFrom, to, as.window);
        {
            // per-model, per-day and unpriced detail come from the window buckets
            std::vector<ClaudeUsageBucket> windowBuckets;
            if (_store.aggregate(acct.name, windowFrom, to, versions, windowBuckets, aggError)) {
                std::map<std::string, ClaudeModelUsage> byModel;
                std::map<std::string, ClaudeDailyPoint> byDay;
                std::set<std::string> unpriced;
                for (const auto& b : windowBuckets) {
                    ClaudeModelUsage& mu = byModel[b.model];
                    mu.model = b.model;
                    mu.messages += b.messages;
                    mu.input += b.input;
                    mu.output += b.output;
                    mu.cacheRead += b.cacheRead;
                    mu.cacheWrite5m += b.cacheWrite5m;
                    mu.cacheWrite1h += b.cacheWrite1h;
                    mu.costNano += b.costNano;
                    mu.unpricedMessages += b.unpricedMessages;
                    ClaudeDailyPoint& dp = byDay[b.day];
                    dp.dayStr = b.day;
                    dp.dayMs = dayStringToMs(b.day);
                    dp.messages += b.messages;
                    dp.tokens += b.input + b.output + b.cacheRead + b.cacheWrite5m + b.cacheWrite1h;
                    dp.costNano += b.costNano;
                    if (b.unpricedMessages > 0) {
                        unpriced.insert(b.model);
                    }
                }
                for (const auto& kv : byModel) {
                    as.models.push_back(kv.second);
                }
                std::sort(as.models.begin(), as.models.end(), [](const ClaudeModelUsage& a, const ClaudeModelUsage& b) {
                    return a.costNano != b.costNano ? a.costNano > b.costNano : a.model < b.model;
                });
                for (const auto& kv : byDay) {
                    as.daily.push_back(kv.second);
                }
                as.unpricedModels.assign(unpriced.begin(), unpriced.end());
            }
        }

        ClaudeCycle cycle;
        if (acct.cycleResetDay >= 1 && claudeCycleBounds(nowEpoch, acct.cycleResetDay, cycle)) {
            as.cycleConfigured = true;
            agg("cycle", cycle.startEpoch, std::min(cycle.endEpoch, to), as.cycle);
            as.cycleResetIso = formatIsoDate(cycle.endEpoch);
            if (acct.spendLimitUsd > 0.0 && !as.cycle.costOverflow) {
                as.estPctOfLimit = claudeNanoToUsd(as.cycle.costNano) / acct.spendLimitUsd * 100.0;
            }
        }
        if (as.errorMessage.empty() && shouldUseCcusage(nowEpoch)) {
            tryOverlayFromCcusage(acct, nowEpoch, as.cycleConfigured, cycle, as);
        }
        status.accounts.push_back(as);
    }
    return status;
}

void ClaudeCollector::startPriceRefresh(std::chrono::milliseconds checkEvery) {
    std::lock_guard<std::mutex> lock(_refreshMutex);
    if (_refreshThread.joinable()) {
        return;
    }
    _refreshStop = false;
    _refreshThread = std::thread(&ClaudeCollector::priceRefreshLoop, this, checkEvery);
}

void ClaudeCollector::stopPriceRefresh() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(_refreshMutex);
        _refreshStop = true;
        worker = std::move(_refreshThread);
    }
    _refreshCv.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

void ClaudeCollector::priceRefreshLoop(std::chrono::milliseconds checkEvery) {
    int64_t lastAttempt = 0;
    const int64_t refreshSec = static_cast<int64_t>(std::max(1, _config.pricingRefreshHours)) * 3600;
    while (true) {
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        PriceCatalogStatus st = _catalog.status(now, _config.pricingRefreshHours);
        const bool due = st.fetchedAtEpoch == 0 || (now - st.fetchedAtEpoch) >= refreshSec;
        if (due && (lastAttempt == 0 || now - lastAttempt >= kRefreshRetryAfterSec)) {
            lastAttempt = now;
            _catalog.refresh(now);
        }
        std::unique_lock<std::mutex> lock(_refreshMutex);
        if (_refreshCv.wait_for(lock, checkEvery, [this] { return _refreshStop; })) {
            break;
        }
    }
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
