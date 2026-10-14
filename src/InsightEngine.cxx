/*
 * InsightEngine.cxx
 *
 * Normalized multi-platform AI usage analytics, programmatic workflow-coaching
 * detectors, before/after experiment comparator, dataset exporter, and weekly
 * digest generator.
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "InsightEngine.hxx"
#include "ClaudeUsageStore.hxx"
#include "PathUtils.hxx"
#include "PriceCatalog.hxx"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <openssl/sha.h>
#include <set>
#include <sstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace aimon {

namespace {

static int64_t parseIso8601ToEpoch(const std::string& iso) {
    if (iso.empty()) return 0;
    std::tm tm = {};
    if (strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%S", &tm)) {
        time_t t = timegm(&tm);
        return (t > 0) ? static_cast<int64_t>(t) : 0;
    }
    return 0;
}

static std::string lstripStr(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) {
        ++i;
    }
    return s.substr(i);
}

static int64_t countWordsInString(const std::string& s) {
    int64_t count = 0;
    bool inWord = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            inWord = false;
        } else if (!inWord) {
            inWord = true;
            ++count;
        }
    }
    return count;
}

static bool isBulletLine(const std::string& lstripped) {
    if (lstripped.size() >= 2) {
        if ((lstripped[0] == '-' || lstripped[0] == '*' || lstripped[0] == '+') &&
            (lstripped[1] == ' ' || lstripped[1] == '\t')) {
            return true;
        }
    }
    size_t i = 0;
    while (i < lstripped.size() && std::isdigit(static_cast<unsigned char>(lstripped[i]))) {
        ++i;
    }
    if (i > 0 && i + 1 < lstripped.size() &&
        (lstripped[i] == '.' || lstripped[i] == ')') &&
        (lstripped[i + 1] == ' ' || lstripped[i + 1] == '\t')) {
        return true;
    }
    return false;
}

static std::string toLowerAscii(const std::string& s) {
    std::string out(s.size(), '\0');
    for (size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    }
    return out;
}

static bool isCompilerOrLogPasteLine(const std::string& line) {
    if (line.empty()) return false;
    if (line.rfind("Traceback (most recent call last):", 0) == 0 ||
        line.rfind("diff --git", 0) == 0 ||
        line.rfind("@@ ", 0) == 0 ||
        line.rfind("+++ ", 0) == 0 ||
        line.rfind("--- ", 0) == 0) {
        return true;
    }

    // Check YYYY-MM-DD[T ]HH:MM:SS prefix
    if (line.size() >= 19 &&
        std::isdigit(static_cast<unsigned char>(line[0])) &&
        std::isdigit(static_cast<unsigned char>(line[1])) &&
        std::isdigit(static_cast<unsigned char>(line[2])) &&
        std::isdigit(static_cast<unsigned char>(line[3])) &&
        line[4] == '-' &&
        std::isdigit(static_cast<unsigned char>(line[5])) &&
        std::isdigit(static_cast<unsigned char>(line[6])) &&
        line[7] == '-' &&
        std::isdigit(static_cast<unsigned char>(line[8])) &&
        std::isdigit(static_cast<unsigned char>(line[9])) &&
        (line[10] == ' ' || line[10] == 'T') &&
        std::isdigit(static_cast<unsigned char>(line[11])) &&
        std::isdigit(static_cast<unsigned char>(line[12])) &&
        line[13] == ':' &&
        std::isdigit(static_cast<unsigned char>(line[14])) &&
        std::isdigit(static_cast<unsigned char>(line[15])) &&
        line[16] == ':' &&
        std::isdigit(static_cast<unsigned char>(line[17])) &&
        std::isdigit(static_cast<unsigned char>(line[18]))) {
        return true;
    }

    std::string ls = lstripStr(line);
    if (ls.rfind("File \"", 0) == 0 && ls.find("\", line ") != std::string::npos) {
        return true;
    }
    if (ls.rfind("at ", 0) == 0 && ls.find('(') != std::string::npos) {
        return true;
    }

    std::string low = toLowerAscii(line);
    if (low.find("error:") != std::string::npos ||
        low.find("warning:") != std::string::npos ||
        low.find("note:") != std::string::npos) {
        // Look for :<digit> pattern characteristic of file:line:col: error:
        for (size_t i = 0; i + 1 < low.size(); ++i) {
            if (low[i] == ':' && std::isdigit(static_cast<unsigned char>(low[i + 1]))) {
                return true;
            }
        }
    }
    return false;
}

static bool isKnownCodeExtension(const std::string& ext) {
    static constexpr const char* kExts[] = {
        "cxx", "hxx", "cpp", "hpp", "cc", "hh", "c", "h", "py", "rs", "go",
        "js", "ts", "tsx", "jsx", "json", "md", "sql", "sh", "yml", "yaml",
        "toml", "cmake", "txt"
    };
    for (const char* e : kExts) {
        if (ext == e) return true;
    }
    return false;
}

static int64_t countFilePathsInProseLine(const std::string& line) {
    int64_t cnt = 0;
    size_t i = 0;
    auto isDelim = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) ||
               c == '`' || c == '"' || c == '\'' ||
               c == '(' || c == ')' || c == '[' || c == ']';
    };
    while (i < line.size()) {
        while (i < line.size() && isDelim(line[i])) {
            ++i;
        }
        if (i >= line.size()) break;
        size_t start = i;
        while (i < line.size() && !isDelim(line[i])) {
            ++i;
        }
        size_t end = i;
        while (end > start && (line[end - 1] == ':' || line[end - 1] == ';' ||
                               line[end - 1] == ',' || line[end - 1] == '.')) {
            --end;
        }
        if (end <= start) continue;
        std::string tok = line.substr(start, end - start);
        size_t dotPos = tok.find_last_of('.');
        if (dotPos == std::string::npos || dotPos == 0 || dotPos + 1 >= tok.size()) continue;
        std::string ext = tok.substr(dotPos + 1);
        bool validChars = true;
        for (char c : tok) {
            if (!std::isalnum(static_cast<unsigned char>(c)) &&
                c != '_' && c != '-' && c != '.' && c != '/' && c != '+') {
                validChars = false;
                break;
            }
        }
        if (!validChars) continue;
        bool hasSlash = (tok.find('/') != std::string::npos && tok.front() != '/' && tok.back() != '/');
        if (hasSlash || isKnownCodeExtension(ext)) {
            ++cnt;
        }
    }
    return cnt;
}

static bool hasConstraintKeywords(const std::string& proseText) {
    std::string low = toLowerAscii(proseText);
    static constexpr const char* kWords[] = {
        "must", "do not", "don't", "never", "only", "without", "strictly", "require", "required"
    };
    auto isWordChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    for (const char* kw : kWords) {
        size_t kwLen = std::strlen(kw);
        size_t pos = 0;
        while ((pos = low.find(kw, pos)) != std::string::npos) {
            bool leftOk = (pos == 0) || !isWordChar(low[pos - 1]);
            bool rightOk = (pos + kwLen >= low.size()) || !isWordChar(low[pos + kwLen]);
            if (leftOk && rightOk) {
                return true;
            }
            pos += kwLen;
        }
    }
    return false;
}

static std::string stripXmlBlock(const std::string& input, const std::string& tag) {
    std::string out = input;
    std::string openTag = "<" + tag;
    std::string closeTag = "</" + tag + ">";
    while (true) {
        size_t start = out.find(openTag);
        if (start == std::string::npos) break;
        size_t end = out.find(closeTag, start);
        if (end == std::string::npos) {
            out.erase(start);
            break;
        }
        size_t eraseLen = (end + closeTag.size()) - start;
        while (start + eraseLen < out.size() && (out[start + eraseLen] == '\n' || out[start + eraseLen] == '\r')) {
            ++eraseLen;
        }
        out.erase(start, eraseLen);
    }
    return out;
}

static bool readVarint(const uint8_t* data, size_t len, size_t& pos, uint64_t& outVal) {
    outVal = 0;
    int shift = 0;
    while (pos < len && shift < 64) {
        uint8_t byte = data[pos++];
        outVal |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool skipField(const uint8_t* data, size_t len, size_t& pos, uint8_t wireType) {
    if (wireType == 0) {
        uint64_t dummy = 0;
        return readVarint(data, len, pos, dummy);
    } else if (wireType == 1) {
        if (pos + 8 > len) return false;
        pos += 8;
        return true;
    } else if (wireType == 2) {
        uint64_t subLen = 0;
        if (!readVarint(data, len, pos, subLen)) return false;
        if (subLen > len || pos + subLen > len) return false;
        pos += static_cast<size_t>(subLen);
        return true;
    } else if (wireType == 5) {
        if (pos + 4 > len) return false;
        pos += 4;
        return true;
    }
    return false;
}

static std::string mapModelEnumToName(int64_t modelEnum) {
    switch (modelEnum) {
        case 342: return "gemini-2.5-flash";
        case 1016: return "gemini-3.1-pro-low";
        case 1018: return "gemini-3.1-pro-high";
        case 1026: return "claude-sonnet-4-6";
        case 1035: return "claude-opus-4-5";
        case 1298: return "gemini-3.8-pro-high";
        case 1299: return "gemini-3.8-pro-low";
        case 1318: return "gemini-3.8-flash";
        case 1319: return "gemini-3.8-flash-low";
        case 1404: return "claude-opus-4-6-thinking";
        case 1418: return "antigravity-cli-default";
        default: return "agy-model-" + std::to_string(modelEnum);
    }
}

} // anonymous namespace

InsightEngine::InsightEngine() = default;

InsightEngine::~InsightEngine() {
    close();
}

bool InsightEngine::open(const std::string& dbPath) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_db) {
        sqlite3_close(_db);
        _db = nullptr;
    }

    std::string rawPath = dbPath.empty() ? _config.insights.dbPath : dbPath;
    if (rawPath.empty()) {
        rawPath = "~/.config/aimon/insights.db";
    }
    _dbPath = PathUtils::expandHome(rawPath);

    std::string parentDir = fs::path(_dbPath).parent_path().string();
    if (!parentDir.empty()) {
        PathUtils::ensureDirectoryExists(parentDir);
        ::chmod(parentDir.c_str(), 0700);
    }

    int rc = sqlite3_open(_dbPath.c_str(), &_db);
    if (rc != SQLITE_OK) {
        if (_db) {
            sqlite3_close(_db);
            _db = nullptr;
        }
        return false;
    }

    ::chmod(_dbPath.c_str(), 0600);
    sqlite3_busy_timeout(_db, 5000);
    sqlite3_exec(_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(_db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    return initSchema();
}

void InsightEngine::close() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_db) {
        sqlite3_close(_db);
        _db = nullptr;
    }
}

bool InsightEngine::isOpen() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _db != nullptr;
}

void InsightEngine::setConfig(const AimonConfig& cfg) {
    std::lock_guard<std::mutex> lock(_mutex);
    _config = cfg;
}

bool InsightEngine::initSchema() {
    if (!_db) return false;
    const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS ins_requests (
            platform         TEXT NOT NULL,
            account          TEXT NOT NULL,
            session_id       TEXT NOT NULL,
            project          TEXT NOT NULL DEFAULT '',
            request_id       TEXT NOT NULL,
            message_id       TEXT NOT NULL DEFAULT '',
            ts               INTEGER NOT NULL,
            model            TEXT NOT NULL,
            speed            TEXT NOT NULL DEFAULT 'standard',
            is_sidechain     INTEGER NOT NULL DEFAULT 0,
            input_tokens     INTEGER NOT NULL DEFAULT 0,
            output_tokens    INTEGER NOT NULL DEFAULT 0,
            cache_read       INTEGER NOT NULL DEFAULT 0,
            cache_write_5m   INTEGER NOT NULL DEFAULT 0,
            cache_write_1h   INTEGER NOT NULL DEFAULT 0,
            context_tokens   INTEGER NOT NULL DEFAULT 0,
            compacted        INTEGER NOT NULL DEFAULT 0,
            thinking_tokens  INTEGER NOT NULL DEFAULT 0,
            web_search_reqs  INTEGER NOT NULL DEFAULT 0,
            est_cost_nano    INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (platform, account, message_id, request_id)
        );
        CREATE INDEX IF NOT EXISTS idx_ins_req_ts ON ins_requests(ts);
        CREATE INDEX IF NOT EXISTS idx_ins_req_session ON ins_requests(platform, session_id, ts);
        CREATE INDEX IF NOT EXISTS idx_ins_req_project ON ins_requests(project, ts);

        CREATE TABLE IF NOT EXISTS ins_sessions (
            platform          TEXT NOT NULL,
            session_id        TEXT NOT NULL,
            conversation_id   TEXT NOT NULL DEFAULT '',
            session_link      TEXT NOT NULL DEFAULT 'exact',
            project           TEXT NOT NULL DEFAULT '',
            model_primary     TEXT NOT NULL DEFAULT '',
            start_ts          INTEGER NOT NULL,
            end_ts            INTEGER NOT NULL,
            duration_sec      INTEGER NOT NULL DEFAULT 0,
            active_sec        INTEGER NOT NULL DEFAULT 0,
            turn_count        INTEGER NOT NULL DEFAULT 0,
            request_count     INTEGER NOT NULL DEFAULT 0,
            sidechain_reqs    INTEGER NOT NULL DEFAULT 0,
            tool_calls        INTEGER NOT NULL DEFAULT 0,
            tool_errors       INTEGER NOT NULL DEFAULT 0,
            edit_calls        INTEGER NOT NULL DEFAULT 0,
            bash_calls        INTEGER NOT NULL DEFAULT 0,
            subagent_spawns   INTEGER NOT NULL DEFAULT 0,
            input_tokens      INTEGER NOT NULL DEFAULT 0,
            output_tokens     INTEGER NOT NULL DEFAULT 0,
            cache_read        INTEGER NOT NULL DEFAULT 0,
            cache_write       INTEGER NOT NULL DEFAULT 0,
            peak_context      INTEGER NOT NULL DEFAULT 0,
            compaction_count  INTEGER NOT NULL DEFAULT 0,
            est_cost_nano     INTEGER NOT NULL DEFAULT 0,
            cost_basis        TEXT NOT NULL DEFAULT 'list_price',
            status            TEXT NOT NULL DEFAULT 'UNKNOWN',
            PRIMARY KEY (platform, session_id)
        );
        CREATE INDEX IF NOT EXISTS idx_ins_sess_ts ON ins_sessions(start_ts);

        CREATE TABLE IF NOT EXISTS ins_daily (
            platform      TEXT NOT NULL,
            day           TEXT NOT NULL,
            model         TEXT NOT NULL,
            tokens        INTEGER NOT NULL DEFAULT 0,
            est_cost_nano INTEGER NOT NULL DEFAULT 0,
            sessions      INTEGER NOT NULL DEFAULT 0,
            requests      INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (platform, day, model)
        );

        CREATE TABLE IF NOT EXISTS ins_tool_calls (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            platform      TEXT NOT NULL,
            session_id    TEXT NOT NULL,
            ts            INTEGER NOT NULL,
            step_index    INTEGER NOT NULL DEFAULT 0,
            tool_name     TEXT NOT NULL,
            target_hash   TEXT NOT NULL DEFAULT '',
            duration_ms   REAL NOT NULL DEFAULT 0,
            status        TEXT NOT NULL DEFAULT 'OK',
            UNIQUE(platform, session_id, step_index, tool_name, ts)
        );
        CREATE INDEX IF NOT EXISTS idx_ins_tools_sess ON ins_tool_calls(platform, session_id, ts);
        CREATE INDEX IF NOT EXISTS idx_ins_tools_name ON ins_tool_calls(tool_name, ts);

        CREATE TABLE IF NOT EXISTS ins_quota (
            ts            INTEGER NOT NULL,
            platform      TEXT NOT NULL,
            metric_key    TEXT NOT NULL,
            value         REAL NOT NULL,
            limit_val     REAL NOT NULL,
            delta         REAL NOT NULL,
            PRIMARY KEY (ts, platform, metric_key)
        );

        CREATE TABLE IF NOT EXISTS ins_spend_daily (
            day           TEXT NOT NULL,
            platform      TEXT NOT NULL,
            category      TEXT NOT NULL,
            spend_cents   REAL NOT NULL,
            requests      INTEGER NOT NULL DEFAULT 0,
            cycle_id      TEXT NOT NULL DEFAULT '',
            PRIMARY KEY (day, platform, category)
        );

        CREATE TABLE IF NOT EXISTS ins_generations (
            conversation_id    TEXT NOT NULL,
            session_id         TEXT NOT NULL DEFAULT '',
            idx                INTEGER NOT NULL,
            ts                 INTEGER NOT NULL,
            model_enum         INTEGER NOT NULL,
            input_tokens       INTEGER NOT NULL,
            output_tokens      INTEGER NOT NULL,
            thinking_tokens    INTEGER NOT NULL,
            response_tokens    INTEGER NOT NULL,
            cache_read_tokens  INTEGER NOT NULL,
            f1_f4_f6           INTEGER NOT NULL DEFAULT 0,
            context_tokens     INTEGER NOT NULL,
            context_limit      INTEGER NOT NULL,
            max_output_tokens  INTEGER NOT NULL DEFAULT 0,
            ttft_ms            REAL NOT NULL,
            duration_ms        REAL NOT NULL,
            validated          INTEGER NOT NULL DEFAULT 1,
            PRIMARY KEY (conversation_id, idx)
        );
        CREATE INDEX IF NOT EXISTS idx_ins_gen_ts ON ins_generations(ts);

        CREATE TABLE IF NOT EXISTS ins_prompts (
            platform              TEXT NOT NULL,
            session_id            TEXT NOT NULL,
            turn_index            INTEGER NOT NULL,
            ts                    INTEGER NOT NULL,
            project               TEXT NOT NULL DEFAULT '',
            chars                 INTEGER NOT NULL,
            words                 INTEGER NOT NULL,
            lines                 INTEGER NOT NULL,
            prose_chars           INTEGER NOT NULL DEFAULT 0,
            prose_words           INTEGER NOT NULL DEFAULT 0,
            fenced_chars          INTEGER NOT NULL DEFAULT 0,
            fenced_block_count    INTEGER NOT NULL DEFAULT 0,
            paste_chars           INTEGER NOT NULL DEFAULT 0,
            mixed_paste           INTEGER NOT NULL DEFAULT 0,
            file_path_count       INTEGER NOT NULL DEFAULT 0,
            bullet_count          INTEGER NOT NULL DEFAULT 0,
            is_sidechain          INTEGER NOT NULL DEFAULT 0,
            has_code_block        INTEGER NOT NULL DEFAULT 0,
            has_file_ref          INTEGER NOT NULL DEFAULT 0,
            has_constraints       INTEGER NOT NULL DEFAULT 0,
            is_first_in_session   INTEGER NOT NULL DEFAULT 0,
            next_tool_calls       INTEGER NOT NULL DEFAULT 0,
            next_tool_errors      INTEGER NOT NULL DEFAULT 0,
            next_edits            INTEGER NOT NULL DEFAULT 0,
            next_Read_before_Edit INTEGER NOT NULL DEFAULT 0,
            reprompt_within_120s  INTEGER NOT NULL DEFAULT 0,
            interrupted_turn      INTEGER NOT NULL DEFAULT 0,
            turn_tokens           INTEGER NOT NULL DEFAULT 0,
            turn_cost_nano        INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (platform, session_id, turn_index)
        );
        CREATE INDEX IF NOT EXISTS idx_ins_prompts_ts ON ins_prompts(ts);
        CREATE INDEX IF NOT EXISTS idx_ins_prompts_proj ON ins_prompts(project, ts);

        CREATE TABLE IF NOT EXISTS ins_scan_cursors (
            source_type   TEXT NOT NULL,
            source_key    TEXT NOT NULL,
            last_offset   INTEGER NOT NULL DEFAULT 0,
            last_mtime    INTEGER NOT NULL DEFAULT 0,
            updated_ts    INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (source_type, source_key)
        );

        CREATE TABLE IF NOT EXISTS ins_findings (
            id                   INTEGER PRIMARY KEY AUTOINCREMENT,
            computed_ts          INTEGER NOT NULL,
            window_days          INTEGER NOT NULL,
            detector             TEXT NOT NULL,
            platform             TEXT NOT NULL,
            scope                TEXT NOT NULL,
            scope_id             TEXT NOT NULL,
            severity             TEXT NOT NULL,
            confidence           TEXT NOT NULL,
            n                    INTEGER NOT NULL DEFAULT 0,
            metric               REAL NOT NULL,
            summary              TEXT NOT NULL,
            evidence_json        TEXT NOT NULL,
            suggested_experiment TEXT NOT NULL DEFAULT '',
            experiment_start     INTEGER NOT NULL DEFAULT 0,
            experiment_metric    TEXT NOT NULL DEFAULT '',
            first_seen           INTEGER NOT NULL DEFAULT 0,
            status               TEXT NOT NULL DEFAULT 'active'
        );
        CREATE INDEX IF NOT EXISTS idx_ins_find_ts ON ins_findings(computed_ts);
    )SQL";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(_db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

PromptFeatures InsightEngine::extractPromptFeatures(const std::string& text) {
    PromptFeatures pf;
    if (text.empty()) {
        return pf;
    }

    pf.chars = static_cast<int64_t>(text.size());
    pf.words = countWordsInString(text);

    std::vector<std::string> lines;
    {
        size_t start = 0;
        while (true) {
            size_t pos = text.find('\n', start);
            if (pos == std::string::npos) {
                lines.push_back(text.substr(start));
                break;
            }
            lines.push_back(text.substr(start, pos - start));
            start = pos + 1;
        }
    }
    pf.lines = static_cast<int64_t>(lines.size());

    size_t nLines = lines.size();
    std::vector<bool> isFenced(nLines, false);
    std::vector<bool> isPaste(nLines, false);

    bool inFence = false;
    for (size_t i = 0; i < nLines; ++i) {
        std::string ls = lstripStr(lines[i]);
        bool isMarker = (ls.rfind("```", 0) == 0 || ls.rfind("~~~", 0) == 0);
        if (!inFence) {
            if (isMarker) {
                inFence = true;
                isFenced[i] = true;
                pf.fencedBlockCount++;
            }
        } else {
            isFenced[i] = true;
            if (isMarker) {
                inFence = false;
            }
        }
    }
    pf.hasCodeBlock = (pf.fencedBlockCount > 0) ? 1 : 0;

    for (size_t i = 0; i < nLines; ++i) {
        if (isFenced[i]) continue;
        if (isCompilerOrLogPasteLine(lines[i])) {
            isPaste[i] = true;
        }
    }

    size_t runStart = 0;
    size_t runLen = 0;
    auto isIndentedCodeLine = [](const std::string& line) -> bool {
        if (line.empty()) return false;
        bool indent = (line[0] == '\t') || (line.size() >= 4 && line.compare(0, 4, "    ") == 0);
        if (!indent) return false;
        return !isBulletLine(lstripStr(line));
    };

    for (size_t i = 0; i <= nLines; ++i) {
        if (i < nLines && !isFenced[i] && isIndentedCodeLine(lines[i])) {
            if (runLen == 0) runStart = i;
            ++runLen;
        } else {
            if (runLen >= 4) {
                for (size_t k = runStart; k < runStart + runLen; ++k) {
                    isPaste[k] = true;
                }
            }
            runLen = 0;
        }
    }

    std::string proseCombined;
    for (size_t i = 0; i < nLines; ++i) {
        int64_t lineLen = static_cast<int64_t>(lines[i].size());
        if (isFenced[i]) {
            pf.fencedChars += lineLen;
        } else if (isPaste[i]) {
            pf.pasteChars += lineLen;
        } else {
            pf.proseChars += lineLen;
            pf.proseWords += countWordsInString(lines[i]);
            pf.filePathCount += countFilePathsInProseLine(lines[i]);
            if (isBulletLine(lstripStr(lines[i]))) {
                pf.bulletCount++;
            }
            if (!proseCombined.empty()) proseCombined.push_back('\n');
            proseCombined += lines[i];
        }
    }

    pf.hasFileRef = (pf.filePathCount > 0) ? 1 : 0;
    pf.hasConstraints = hasConstraintKeywords(proseCombined) ? 1 : 0;
    pf.mixedPaste = ((pf.fencedChars + pf.pasteChars) > pf.proseChars &&
                     (pf.fencedChars + pf.pasteChars) > 0) ? 1 : 0;

    return pf;
}

bool InsightEngine::extractClaudeUserPromptFeatures(const nlohmann::json& lineObj,
                                                    PromptFeatures& outFeatures,
                                                    bool& outIsInterrupted) {
    outIsInterrupted = false;
    outFeatures = PromptFeatures{};

    if (!lineObj.is_object()) return false;
    if (!lineObj.contains("type") || !lineObj["type"].is_string() ||
        lineObj["type"].get<std::string>() != "user") {
        return false;
    }

    if (lineObj.contains("isSidechain") && lineObj["isSidechain"].is_boolean() &&
        lineObj["isSidechain"].get<bool>()) {
        return false;
    }
    if (lineObj.contains("isMeta") && lineObj["isMeta"].is_boolean() &&
        lineObj["isMeta"].get<bool>()) {
        return false;
    }

    if (!lineObj.contains("message") || !lineObj["message"].is_object()) {
        return false;
    }
    const auto& msg = lineObj["message"];
    if (!msg.contains("content")) {
        return false;
    }

    std::string combinedText;
    const auto& content = msg["content"];
    if (content.is_string()) {
        combinedText = content.get<std::string>();
    } else if (content.is_array()) {
        bool hasTextElem = false;
        for (const auto& el : content) {
            if (!el.is_object()) continue;
            std::string elType = el.value("type", "");
            if (elType == "tool_result") {
                return false;
            }
            if (elType == "text" && el.contains("text") && el["text"].is_string()) {
                std::string t = el["text"].get<std::string>();
                if (t.rfind("[Request interrupted", 0) == 0) {
                    outIsInterrupted = true;
                    return false;
                }
                t = stripXmlBlock(t, "ide_opened_file");
                t = stripXmlBlock(t, "ide_selection");
                t = stripXmlBlock(t, "system-reminder");
                if (!t.empty()) {
                    if (!combinedText.empty()) combinedText.push_back('\n');
                    combinedText += t;
                    hasTextElem = true;
                }
            }
        }
        if (!hasTextElem) {
            return false;
        }
    } else {
        return false;
    }

    std::string trimmed = lstripStr(combinedText);
    if (trimmed.empty()) return false;
    if (trimmed.rfind("<command-name>", 0) == 0 ||
        trimmed.rfind("<command-message>", 0) == 0 ||
        trimmed.rfind("<local-command-", 0) == 0) {
        return false;
    }
    if (trimmed.rfind("[Request interrupted", 0) == 0) {
        outIsInterrupted = true;
        return false;
    }

    combinedText = stripXmlBlock(combinedText, "ide_opened_file");
    combinedText = stripXmlBlock(combinedText, "ide_selection");
    combinedText = stripXmlBlock(combinedText, "system-reminder");
    if (lstripStr(combinedText).empty()) {
        return false;
    }

    outFeatures = extractPromptFeatures(combinedText);
    return true;
}

bool InsightEngine::decodeGenMetadata(const uint8_t* data, size_t len, GenerationRecord& out) {
    out = GenerationRecord{};
    if (!data || len == 0) return false;

    auto parseSubVarints = [](const uint8_t* buf, size_t bLen, std::map<uint32_t, uint64_t>& vOut,
                              std::map<uint32_t, std::pair<const uint8_t*, size_t>>& bytesOut) -> bool {
        size_t pos = 0;
        while (pos < bLen) {
            uint64_t tag = 0;
            if (!readVarint(buf, bLen, pos, tag) || tag == 0) return false;
            uint32_t fieldNum = static_cast<uint32_t>(tag >> 3);
            uint8_t wireType = static_cast<uint8_t>(tag & 0x07);
            if (fieldNum == 0) return false;
            if (wireType == 0) {
                uint64_t val = 0;
                if (!readVarint(buf, bLen, pos, val)) return false;
                vOut[fieldNum] = val;
            } else if (wireType == 2) {
                uint64_t sLen = 0;
                if (!readVarint(buf, bLen, pos, sLen)) return false;
                if (sLen > bLen || pos + sLen > bLen) return false;
                bytesOut[fieldNum] = {buf + pos, static_cast<size_t>(sLen)};
                pos += static_cast<size_t>(sLen);
            } else {
                if (!skipField(buf, bLen, pos, wireType)) return false;
            }
        }
        return true;
    };

    std::map<uint32_t, uint64_t> rootVarints;
    std::map<uint32_t, std::pair<const uint8_t*, size_t>> rootBytes;
    if (!parseSubVarints(data, len, rootVarints, rootBytes)) {
        return false;
    }

    auto itF1 = rootBytes.find(1);
    if (itF1 == rootBytes.end()) {
        return false;
    }

    std::map<uint32_t, uint64_t> f1Varints;
    std::map<uint32_t, std::pair<const uint8_t*, size_t>> f1Bytes;
    if (!parseSubVarints(itF1->second.first, itF1->second.second, f1Varints, f1Bytes)) {
        return false;
    }

    if (f1Varints.count(3)) {
        out.modelEnum = static_cast<int64_t>(f1Varints[3]);
    }

    bool hasTokenSub = false;
    if (f1Bytes.count(4)) {
        std::map<uint32_t, uint64_t> f4V;
        std::map<uint32_t, std::pair<const uint8_t*, size_t>> f4B;
        if (parseSubVarints(f1Bytes[4].first, f1Bytes[4].second, f4V, f4B)) {
            hasTokenSub = true;
            if (out.modelEnum == 0 && f4V.count(1)) out.modelEnum = static_cast<int64_t>(f4V[1]);
            if (f4V.count(2)) out.inputTokens = static_cast<int64_t>(f4V[2]);
            if (f4V.count(3)) out.outputTokens = static_cast<int64_t>(f4V[3]);
            if (f4V.count(5)) out.cacheReadTokens = static_cast<int64_t>(f4V[5]);
            if (f4V.count(6)) out.f1F4F6 = static_cast<int64_t>(f4V[6]);
            if (f4V.count(9)) out.thinkingTokens = static_cast<int64_t>(f4V[9]);
            if (f4V.count(10)) out.responseTokens = static_cast<int64_t>(f4V[10]);
        }
    }

    if (f1Bytes.count(9)) {
        std::map<uint32_t, uint64_t> f9V;
        std::map<uint32_t, std::pair<const uint8_t*, size_t>> f9B;
        if (parseSubVarints(f1Bytes[9].first, f1Bytes[9].second, f9V, f9B)) {
            if (f9B.count(4)) {
                std::map<uint32_t, uint64_t> tsV;
                std::map<uint32_t, std::pair<const uint8_t*, size_t>> tsB;
                if (parseSubVarints(f9B[4].first, f9B[4].second, tsV, tsB) && tsV.count(1)) {
                    out.ts = static_cast<int64_t>(tsV[1]);
                }
            }
            if (f9B.count(8)) {
                std::map<uint32_t, uint64_t> ttftV;
                std::map<uint32_t, std::pair<const uint8_t*, size_t>> ttftB;
                if (parseSubVarints(f9B[8].first, f9B[8].second, ttftV, ttftB)) {
                    double sec = ttftV.count(1) ? static_cast<double>(ttftV[1]) : 0.0;
                    double nanos = ttftV.count(2) ? static_cast<double>(ttftV[2]) : 0.0;
                    out.ttftMs = sec * 1000.0 + nanos / 1e6;
                }
            }
            if (f9B.count(10)) {
                std::map<uint32_t, uint64_t> ctxV;
                std::map<uint32_t, std::pair<const uint8_t*, size_t>> ctxB;
                if (parseSubVarints(f9B[10].first, f9B[10].second, ctxV, ctxB)) {
                    if (ctxV.count(1)) out.contextTokens = static_cast<int64_t>(ctxV[1]);
                    if (ctxV.count(4)) out.contextLimit = static_cast<int64_t>(ctxV[4]);
                }
            }
        }
    }

    if (f1Bytes.count(11)) {
        std::map<uint32_t, uint64_t> durV;
        std::map<uint32_t, std::pair<const uint8_t*, size_t>> durB;
        if (parseSubVarints(f1Bytes[11].first, f1Bytes[11].second, durV, durB)) {
            double sec = durV.count(1) ? static_cast<double>(durV[1]) : 0.0;
            double nanos = durV.count(2) ? static_cast<double>(durV[2]) : 0.0;
            out.durationMs = sec * 1000.0 + nanos / 1e6;
        }
    }

    if (f1Bytes.count(15)) {
        std::map<uint32_t, uint64_t> maxOutV;
        std::map<uint32_t, std::pair<const uint8_t*, size_t>> maxOutB;
        if (parseSubVarints(f1Bytes[15].first, f1Bytes[15].second, maxOutV, maxOutB) && maxOutV.count(2)) {
            out.maxOutputTokens = static_cast<int64_t>(maxOutV[2]);
        }
    }

    if (!hasTokenSub && out.modelEnum == 0 && out.ts == 0) {
        return false;
    }

    if (out.inputTokens < 0 || out.outputTokens < 0 ||
        out.thinkingTokens < 0 || out.responseTokens < 0 || out.cacheReadTokens < 0) {
        return false;
    }

    out.validated = (out.outputTokens == out.thinkingTokens + out.responseTokens) ? 1 : 0;
    return true;
}

std::string InsightEngine::mapAntigravityStepType(int stepType) {
    switch (stepType) {
        case 5: return "CODE_ACTION";
        case 7: return "GREP_SEARCH";
        case 8: return "VIEW_FILE";
        case 9: return "LIST_DIRECTORY";
        case 14: return "USER_INPUT";
        case 15: return "PLANNER_RESPONSE";
        case 17: return "ERROR_MESSAGE";
        case 21: return "RUN_COMMAND";
        case 23: return "CHECKPOINT";
        case 33: return "SEARCH_WEB";
        case 38: return "MCP_TOOL";
        case 83: return "CODE_ACKNOWLEDGEMENT";
        case 90: return "EPHEMERAL_MESSAGE";
        case 91: return "GENERATE_IMAGE";
        case 98: return "CONVERSATION_HISTORY";
        case 99: return "KNOWLEDGE_ARTIFACTS";
        case 101: return "SYSTEM_MESSAGE";
        case 132: return "GENERIC";
        case 138: return "ASK_QUESTION";
        default: return "STEP_" + std::to_string(stepType);
    }
}

std::string InsightEngine::hashFilePath(const std::string& path) {
    if (path.empty()) return "";
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(path.data()), path.size(), digest);
    char hex[17];
    for (int i = 0; i < 8; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    hex[16] = '\0';
    return std::string(hex);
}

std::string InsightEngine::basenameOnly(const std::string& path) {
    if (path.empty()) return "";
    std::string p = path;
    for (char& ch : p) {
        if (ch == '\\') ch = '/';
    }
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t pos = p.find_last_of('/');
    return (pos == std::string::npos) ? p : p.substr(pos + 1);
}

static bool isHexHashString(const std::string& s) {
    if (s.size() < 16) return false;
    for (unsigned char c : s) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

std::string InsightEngine::decodeProjectSlug(const std::string& slug) {
    if (slug.empty()) return "unknown";
    std::string homeDir = PathUtils::expandHome("~");
    if (!homeDir.empty() && slug == homeDir) {
        return "unattributed";
    }
    std::string res;
    if (slug.find('/') != std::string::npos || slug.find('\\') != std::string::npos) {
        res = basenameOnly(slug);
    } else {
        size_t workPos = slug.rfind("-work-");
        if (workPos != std::string::npos && workPos + 6 < slug.size()) {
            res = slug.substr(workPos + 6);
        } else {
            size_t lastDash = slug.find_last_of('-');
            if (lastDash != std::string::npos && lastDash + 1 < slug.size()) {
                res = slug.substr(lastDash + 1);
            } else {
                res = slug;
            }
        }
    }
    if (res.empty() || isHexHashString(res)) {
        return "unattributed";
    }
    return res;
}

bool InsightEngine::ingestLiveStatus(const AggregateStatus& status) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) return false;

    int64_t nowSec = static_cast<int64_t>(std::chrono::system_clock::to_time_t(
        status.lastUpdated.time_since_epoch().count() > 0 ?
        status.lastUpdated : std::chrono::system_clock::now()));

    sqlite3_exec(_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    if (status.cursor.isAuthenticated) {
        const char* spendSql =
            "INSERT OR REPLACE INTO ins_spend_daily (day, platform, category, spend_cents, requests, cycle_id) "
            "VALUES (?, 'cursor', ?, ?, 0, ?);";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(_db, spendSql, -1, &st, nullptr) == SQLITE_OK) {
            for (const auto& cs : status.cursor.dailyCategorySpends) {
                if (cs.dayStr.empty()) continue;
                sqlite3_reset(st);
                sqlite3_bind_text(st, 1, cs.dayStr.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, cs.category.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(st, 3, cs.spendCents);
                sqlite3_bind_text(st, 4, cs.cycleId.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(st);
            }
            sqlite3_finalize(st);
        }
    }

    const char* qSql =
        "INSERT OR IGNORE INTO ins_quota (ts, platform, metric_key, value, limit_val, delta) "
        "VALUES (?, ?, ?, ?, ?, 0.0);";
    sqlite3_stmt* qSt = nullptr;
    if (sqlite3_prepare_v2(_db, qSql, -1, &qSt, nullptr) == SQLITE_OK) {
        if (status.antigravity.isRunning) {
            for (const auto& m : status.antigravity.models) {
                sqlite3_reset(qSt);
                sqlite3_bind_int64(qSt, 1, nowSec);
                sqlite3_bind_text(qSt, 2, "antigravity", -1, SQLITE_STATIC);
                sqlite3_bind_text(qSt, 3, m.modelId.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(qSt, 4, m.remainingFraction);
                sqlite3_bind_double(qSt, 5, 1.0);
                sqlite3_step(qSt);
            }
            for (const auto& g : status.antigravity.quotaGroups) {
                for (const auto& b : g.buckets) {
                    std::string bKey = "bucket:" + (b.bucketId.empty() ? (g.displayName + ":" + b.window) : b.bucketId);
                    sqlite3_reset(qSt);
                    sqlite3_bind_int64(qSt, 1, nowSec);
                    sqlite3_bind_text(qSt, 2, "antigravity", -1, SQLITE_STATIC);
                    sqlite3_bind_text(qSt, 3, bKey.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_double(qSt, 4, b.remainingFraction);
                    sqlite3_bind_double(qSt, 5, 1.0);
                    sqlite3_step(qSt);
                }
            }
        }
        sqlite3_finalize(qSt);
    }

    sqlite3_exec(_db, "COMMIT;", nullptr, nullptr, nullptr);
    return true;
}

bool InsightEngine::rebuildFromConfig(const std::string& historyDbPath,
                                      const std::string& telemetryDbPath,
                                      bool fullResetCursors) {
    std::vector<std::string> claudeDirs;
    for (const auto& acct : _config.claude.accounts) {
        if (!acct.configDir.empty()) {
            claudeDirs.push_back(PathUtils::expandHome(acct.configDir));
        }
    }
    if (claudeDirs.empty()) {
        std::string defClaude = PathUtils::expandHome("~/.claude");
        if (fs::exists(defClaude)) claudeDirs.push_back(defClaude);
    }

    std::vector<std::string> agyRoots;
    std::string ideRoot = PathUtils::expandHome("~/.gemini/antigravity-ide");
    std::string cliRoot = PathUtils::expandHome("~/.gemini/antigravity-cli");
    if (fs::exists(ideRoot)) agyRoots.push_back(ideRoot);
    if (fs::exists(cliRoot)) agyRoots.push_back(cliRoot);

    std::string cursorHome = PathUtils::expandHome("~/.cursor");
    std::string cursorSrv = PathUtils::expandHome("~/.cursor-server");

    return rebuild(historyDbPath, telemetryDbPath, claudeDirs, agyRoots, cursorHome, cursorSrv, fullResetCursors);
}

bool InsightEngine::rebuild(const std::string& historyDbPath,
                            const std::string& telemetryDbPath,
                            const std::vector<std::string>& claudeConfigDirs,
                            const std::vector<std::string>& antigravityRoots,
                            const std::string& cursorHomeDir,
                            const std::string& cursorServerDir,
                            bool fullResetCursors) {
    std::unique_lock<std::mutex> lock(_mutex);
    if (!_db) return false;

    int64_t nowSec = static_cast<int64_t>(std::time(nullptr));
    if (fullResetCursors) {
        sqlite3_exec(_db,
            "DELETE FROM ins_scan_cursors;"
            "DELETE FROM ins_sessions;"
            "DELETE FROM ins_requests;"
            "DELETE FROM ins_daily;"
            "DELETE FROM ins_prompts;"
            "DELETE FROM ins_findings;"
            "DELETE FROM ins_tool_calls;"
            "DELETE FROM ins_generations;"
            "DELETE FROM ins_quota WHERE platform LIKE 'claude:%';",
            nullptr, nullptr, nullptr);
    }

    auto getCursor = [&](const std::string& srcType, const std::string& srcKey, int64_t& outOffset, int64_t& outMtime) {
        outOffset = 0;
        outMtime = 0;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(_db,
            "SELECT last_offset, last_mtime FROM ins_scan_cursors WHERE source_type = ? AND source_key = ?;",
            -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, srcType.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, srcKey.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) {
                outOffset = sqlite3_column_int64(st, 0);
                outMtime = sqlite3_column_int64(st, 1);
            }
            sqlite3_finalize(st);
        }
    };

    auto setCursor = [&](const std::string& srcType, const std::string& srcKey, int64_t offset, int64_t mtime) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(_db,
            "INSERT OR REPLACE INTO ins_scan_cursors (source_type, source_key, last_offset, last_mtime, updated_ts) "
            "VALUES (?, ?, ?, ?, ?);",
            -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, srcType.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, srcKey.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 3, offset);
            sqlite3_bind_int64(st, 4, mtime);
            sqlite3_bind_int64(st, 5, nowSec);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    };

    std::map<std::string, std::string> sessionProjectMap;
    std::set<std::string> compactedSessionIds;

    sqlite3_exec(_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    // 1. Scan Claude JSONL transcripts
    for (const auto& rawDir : claudeConfigDirs) {
        std::string cDir = PathUtils::expandHome(rawDir);
        std::string projRoot = cDir + "/projects";
        if (!fs::exists(projRoot)) continue;

        for (const auto& projEntry : fs::directory_iterator(projRoot)) {
            if (!projEntry.is_directory()) continue;
            std::string projSlug = decodeProjectSlug(projEntry.path().filename().string());

            for (const auto& fileEntry : fs::recursive_directory_iterator(projEntry.path())) {
                if (!fileEntry.is_regular_file() || fileEntry.path().extension() != ".jsonl") continue;
                std::string fPath = fileEntry.path().string();
                std::error_code ec;
                auto fSize = static_cast<int64_t>(fs::file_size(fileEntry.path(), ec));
                auto fMtime = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        fs::last_write_time(fileEntry.path(), ec).time_since_epoch()).count());

                std::string sessStem = fileEntry.path().stem().string();
                sessionProjectMap[sessStem] = projSlug;

                int64_t prevOffset = 0, prevMtime = 0;
                getCursor("claude_jsonl", fPath, prevOffset, prevMtime);
                if (prevOffset == fSize && prevMtime == fMtime) {
                    continue;
                }

                std::ifstream in(fPath);
                if (!in.is_open()) continue;

                struct PendingTurn {
                    std::string sessionId;
                    std::string project;
                    int turnIndex = 0;
                    int64_t ts = 0;
                    PromptFeatures pf;
                    int nextToolCalls = 0;
                    int nextToolErrors = 0;
                    int nextEdits = 0;
                    int nextReadBeforeEdit = 0;
                    bool sawReadThisTurn = false;
                    int repromptWithin120s = 0;
                    int interruptedTurn = 0;
                    int64_t turnTokens = 0;
                };
                std::vector<PendingTurn> turns;
                int stepIdx = 0;

                sqlite3_stmt* toolStmt = nullptr;
                sqlite3_prepare_v2(_db,
                    "INSERT OR IGNORE INTO ins_tool_calls "
                    "(platform, session_id, ts, step_index, tool_name, target_hash, duration_ms, status) "
                    "VALUES ('claude', ?, ?, ?, ?, ?, 0.0, ?);",
                    -1, &toolStmt, nullptr);

                std::string line;
                while (std::getline(in, line)) {
                    if (line.empty()) continue;
                    nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
                    if (j.is_discarded() || !j.is_object()) continue;

                    std::string sId = j.value("sessionId", sessStem);
                    std::string cwdStr = j.value("cwd", "");
                    std::string lineProj = !cwdStr.empty() ? basenameOnly(cwdStr) : projSlug;
                    if (!lineProj.empty() && lineProj != "unknown") {
                        sessionProjectMap[sId] = lineProj;
                    } else if (!sessionProjectMap.count(sId)) {
                        sessionProjectMap[sId] = projSlug;
                    }

                    std::string recType = j.value("type", "");
                    if (recType == "summary" || recType == "compact_boundary") {
                        compactedSessionIds.insert(sId);
                    }

                    int64_t lineTs = parseIso8601ToEpoch(j.value("timestamp", ""));
                    if (lineTs <= 0) lineTs = nowSec;

                    PromptFeatures pf;
                    bool isInterrupted = false;
                    if (extractClaudeUserPromptFeatures(j, pf, isInterrupted)) {
                        if (!turns.empty() && lineTs > turns.back().ts && (lineTs - turns.back().ts) <= _config.insights.quickRepromptSeconds &&
                            turns.back().nextEdits == 0) {
                            turns.back().repromptWithin120s = 1;
                        }
                        PendingTurn pt;
                        pt.sessionId = sId;
                        pt.project = sessionProjectMap[sId];
                        pt.turnIndex = static_cast<int>(turns.size());
                        pt.ts = lineTs;
                        pf.isFirstInSession = turns.empty() ? 1 : 0;
                        pt.pf = pf;
                        turns.push_back(pt);
                    } else if (isInterrupted && !turns.empty()) {
                        turns.back().interruptedTurn = 1;
                    } else if (recType == "assistant" && j.contains("message") && j["message"].is_object()) {
                        const auto& msg = j["message"];
                        if (msg.contains("usage") && msg["usage"].is_object() && !turns.empty()) {
                            const auto& u = msg["usage"];
                            int64_t inT = u.value("input_tokens", int64_t(0));
                            int64_t outT = u.value("output_tokens", int64_t(0));
                            turns.back().turnTokens += (inT + outT);
                        }
                        if (msg.contains("content") && msg["content"].is_array()) {
                            for (const auto& c : msg["content"]) {
                                if (!c.is_object() || c.value("type", "") != "tool_use") continue;
                                std::string tName = c.value("name", "unknown");
                                std::string fTarget;
                                if (c.contains("input") && c["input"].is_object()) {
                                    fTarget = c["input"].value("file_path", c["input"].value("path", ""));
                                }
                                std::string tHash = hashFilePath(fTarget);
                                if (toolStmt) {
                                    sqlite3_reset(toolStmt);
                                    sqlite3_bind_text(toolStmt, 1, sId.c_str(), -1, SQLITE_TRANSIENT);
                                    sqlite3_bind_int64(toolStmt, 2, lineTs);
                                    sqlite3_bind_int(toolStmt, 3, stepIdx++);
                                    sqlite3_bind_text(toolStmt, 4, tName.c_str(), -1, SQLITE_TRANSIENT);
                                    sqlite3_bind_text(toolStmt, 5, tHash.c_str(), -1, SQLITE_TRANSIENT);
                                    sqlite3_bind_text(toolStmt, 6, "OK", -1, SQLITE_STATIC);
                                    sqlite3_step(toolStmt);
                                }
                                if (!turns.empty()) {
                                    turns.back().nextToolCalls++;
                                    if (tName == "Read" || tName == "Grep" || tName == "Glob") {
                                        turns.back().sawReadThisTurn = true;
                                    } else if (tName == "Edit" || tName == "MultiEdit" || tName == "Write") {
                                        turns.back().nextEdits++;
                                        if (turns.back().sawReadThisTurn) {
                                            turns.back().nextReadBeforeEdit = 1;
                                        }
                                    }
                                }
                            }
                        }
                    } else if (recType == "user" && j.contains("message") && j["message"].is_object()) {
                        const auto& msg = j["message"];
                        if (msg.contains("content") && msg["content"].is_array() && !turns.empty()) {
                            for (const auto& c : msg["content"]) {
                                if (c.is_object() && c.value("type", "") == "tool_result" &&
                                    c.value("is_error", false)) {
                                    turns.back().nextToolErrors++;
                                }
                            }
                        }
                    }
                }
                if (toolStmt) sqlite3_finalize(toolStmt);

                sqlite3_stmt* pStmt = nullptr;
                if (sqlite3_prepare_v2(_db,
                    "INSERT OR REPLACE INTO ins_prompts ("
                    "platform, session_id, turn_index, ts, project, chars, words, lines, "
                    "prose_chars, prose_words, fenced_chars, fenced_block_count, paste_chars, "
                    "mixed_paste, file_path_count, bullet_count, is_sidechain, has_code_block, "
                    "has_file_ref, has_constraints, is_first_in_session, next_tool_calls, "
                    "next_tool_errors, next_edits, next_Read_before_Edit, reprompt_within_120s, "
                    "interrupted_turn, turn_tokens, turn_cost_nano"
                    ") VALUES ('claude', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0);",
                    -1, &pStmt, nullptr) == SQLITE_OK) {
                    for (const auto& pt : turns) {
                        sqlite3_reset(pStmt);
                        sqlite3_bind_text(pStmt, 1, pt.sessionId.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int(pStmt, 2, pt.turnIndex);
                        sqlite3_bind_int64(pStmt, 3, pt.ts);
                        sqlite3_bind_text(pStmt, 4, pt.project.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int64(pStmt, 5, pt.pf.chars);
                        sqlite3_bind_int64(pStmt, 6, pt.pf.words);
                        sqlite3_bind_int64(pStmt, 7, pt.pf.lines);
                        sqlite3_bind_int64(pStmt, 8, pt.pf.proseChars);
                        sqlite3_bind_int64(pStmt, 9, pt.pf.proseWords);
                        sqlite3_bind_int64(pStmt, 10, pt.pf.fencedChars);
                        sqlite3_bind_int64(pStmt, 11, pt.pf.fencedBlockCount);
                        sqlite3_bind_int64(pStmt, 12, pt.pf.pasteChars);
                        sqlite3_bind_int(pStmt, 13, pt.pf.mixedPaste);
                        sqlite3_bind_int64(pStmt, 14, pt.pf.filePathCount);
                        sqlite3_bind_int64(pStmt, 15, pt.pf.bulletCount);
                        sqlite3_bind_int(pStmt, 16, pt.pf.hasCodeBlock);
                        sqlite3_bind_int(pStmt, 17, pt.pf.hasFileRef);
                        sqlite3_bind_int(pStmt, 18, pt.pf.hasConstraints);
                        sqlite3_bind_int(pStmt, 19, pt.pf.isFirstInSession);
                        sqlite3_bind_int(pStmt, 20, pt.nextToolCalls);
                        sqlite3_bind_int(pStmt, 21, pt.nextToolErrors);
                        sqlite3_bind_int(pStmt, 22, pt.nextEdits);
                        sqlite3_bind_int(pStmt, 23, pt.nextReadBeforeEdit);
                        sqlite3_bind_int(pStmt, 24, pt.repromptWithin120s);
                        sqlite3_bind_int(pStmt, 25, pt.interruptedTurn);
                        sqlite3_bind_int64(pStmt, 26, pt.turnTokens);
                        sqlite3_step(pStmt);
                    }
                    sqlite3_finalize(pStmt);
                }

                setCursor("claude_jsonl", fPath, fSize, fMtime);
            }
        }
    }

    // 2. Scan telemetry.db if present
    std::string expTelPath = PathUtils::expandHome(telemetryDbPath);
    if (!expTelPath.empty() && fs::exists(expTelPath)) {
        sqlite3* tdb = nullptr;
        if (sqlite3_open_v2(expTelPath.c_str(), &tdb, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
            sqlite3_stmt* sSt = nullptr;
            if (sqlite3_prepare_v2(tdb,
                "SELECT session_id, agent_type, workspace_path, model_name, start_timestamp, end_timestamp, "
                "status, total_turns, total_prompt_tokens, total_comp_tokens, total_tool_calls, total_errors "
                "FROM agent_sessions;", -1, &sSt, nullptr) == SQLITE_OK) {
                sqlite3_stmt* insSess = nullptr;
                sqlite3_prepare_v2(_db,
                    "INSERT OR REPLACE INTO ins_sessions ("
                    "platform, session_id, conversation_id, session_link, project, model_primary, "
                    "start_ts, end_ts, duration_sec, active_sec, turn_count, request_count, "
                    "sidechain_reqs, tool_calls, tool_errors, edit_calls, bash_calls, subagent_spawns, "
                    "input_tokens, output_tokens, cache_read, cache_write, peak_context, compaction_count, "
                    "est_cost_nano, cost_basis, status"
                    ") VALUES (?, ?, '', 'exact', ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?, 0, 0, 0, ?, ?, 0, 0, 0, 0, 0, 'none', ?);",
                    -1, &insSess, nullptr);

                while (sqlite3_step(sSt) == SQLITE_ROW) {
                    const char* sIdC = reinterpret_cast<const char*>(sqlite3_column_text(sSt, 0));
                    const char* aTypeC = reinterpret_cast<const char*>(sqlite3_column_text(sSt, 1));
                    const char* wsC = reinterpret_cast<const char*>(sqlite3_column_text(sSt, 2));
                    const char* modC = reinterpret_cast<const char*>(sqlite3_column_text(sSt, 3));
                    int64_t startTs = sqlite3_column_int64(sSt, 4);
                    int64_t endTs = sqlite3_column_int64(sSt, 5);
                    const char* statC = reinterpret_cast<const char*>(sqlite3_column_text(sSt, 6));
                    int turns = sqlite3_column_int(sSt, 7);
                    int pTok = sqlite3_column_int(sSt, 8);
                    int cTok = sqlite3_column_int(sSt, 9);
                    int tCalls = sqlite3_column_int(sSt, 10);
                    int errs = sqlite3_column_int(sSt, 11);

                    std::string sId = sIdC ? sIdC : "";
                    std::string rawPlat = aTypeC ? aTypeC : "unknown";
                    std::string plat = (rawPlat.find("claude") != std::string::npos) ? "claude" :
                                       (rawPlat.find("cursor") != std::string::npos) ? "cursor" :
                                       (rawPlat.find("antigravity") != std::string::npos) ? "antigravity" : rawPlat;
                    std::string proj = decodeProjectSlug(wsC ? wsC : "");
                    if (!sId.empty() && proj != "unknown" && !sessionProjectMap.count(sId)) {
                        sessionProjectMap[sId] = proj;
                    }
                    int64_t dur = (endTs > startTs && startTs > 0) ? (endTs - startTs) : 0;

                    if (insSess && !sId.empty()) {
                        sqlite3_reset(insSess);
                        sqlite3_bind_text(insSess, 1, plat.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(insSess, 2, sId.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(insSess, 3, sessionProjectMap.count(sId) ? sessionProjectMap[sId].c_str() : proj.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(insSess, 4, modC ? modC : "", -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int64(insSess, 5, startTs);
                        sqlite3_bind_int64(insSess, 6, endTs > 0 ? endTs : startTs);
                        sqlite3_bind_int64(insSess, 7, dur);
                        sqlite3_bind_int64(insSess, 8, dur);
                        sqlite3_bind_int(insSess, 9, turns);
                        sqlite3_bind_int(insSess, 10, turns);
                        sqlite3_bind_int(insSess, 11, tCalls);
                        sqlite3_bind_int(insSess, 12, errs);
                        sqlite3_bind_int(insSess, 13, pTok);
                        sqlite3_bind_int(insSess, 14, cTok);
                        sqlite3_bind_text(insSess, 15, statC ? statC : "UNKNOWN", -1, SQLITE_TRANSIENT);
                        sqlite3_step(insSess);
                    }
                }
                if (insSess) sqlite3_finalize(insSess);
                sqlite3_finalize(sSt);
            }

            int64_t lastEvId = 0, dummyMtime = 0;
            getCursor("telemetry_events", expTelPath, lastEvId, dummyMtime);
            int64_t maxEvId = lastEvId;

            sqlite3_stmt* eSt = nullptr;
            if (sqlite3_prepare_v2(tdb,
                "SELECT id, timestamp, session_id, agent_type, event_type, step_index, tool_name, duration_ms, status, details_json "
                "FROM agent_lifecycle_events WHERE id > ? ORDER BY id ASC;", -1, &eSt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(eSt, 1, lastEvId);

                sqlite3_stmt* insTool = nullptr;
                sqlite3_prepare_v2(_db,
                    "INSERT OR IGNORE INTO ins_tool_calls "
                    "(platform, session_id, ts, step_index, tool_name, target_hash, duration_ms, status) "
                    "VALUES (?, ?, ?, ?, ?, '', ?, ?);",
                    -1, &insTool, nullptr);

                while (sqlite3_step(eSt) == SQLITE_ROW) {
                    int64_t evId = sqlite3_column_int64(eSt, 0);
                    int64_t ts = sqlite3_column_int64(eSt, 1);
                    const char* sIdC = reinterpret_cast<const char*>(sqlite3_column_text(eSt, 2));
                    const char* aTypeC = reinterpret_cast<const char*>(sqlite3_column_text(eSt, 3));
                    const char* evTypeC = reinterpret_cast<const char*>(sqlite3_column_text(eSt, 4));
                    int stepIdx = sqlite3_column_int(eSt, 5);
                    const char* tNameC = reinterpret_cast<const char*>(sqlite3_column_text(eSt, 6));
                    double durMs = sqlite3_column_double(eSt, 7);
                    const char* stC = reinterpret_cast<const char*>(sqlite3_column_text(eSt, 8));

                    std::string rawPlat = aTypeC ? aTypeC : "unknown";
                    std::string plat = (rawPlat.find("claude") != std::string::npos) ? "claude" :
                                       (rawPlat.find("cursor") != std::string::npos) ? "cursor" :
                                       (rawPlat.find("antigravity") != std::string::npos) ? "antigravity" : rawPlat;
                    std::string evType = evTypeC ? evTypeC : "";
                    std::string sId = sIdC ? sIdC : "";

                    if (evType == "TOOL_CALL" && insTool && !sId.empty()) {
                        sqlite3_reset(insTool);
                        sqlite3_bind_text(insTool, 1, plat.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(insTool, 2, sId.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int64(insTool, 3, ts);
                        sqlite3_bind_int(insTool, 4, stepIdx);
                        sqlite3_bind_text(insTool, 5, (tNameC && *tNameC) ? tNameC : "tool", -1, SQLITE_TRANSIENT);
                        sqlite3_bind_double(insTool, 6, durMs);
                        sqlite3_bind_text(insTool, 7, stC ? stC : "OK", -1, SQLITE_TRANSIENT);
                        sqlite3_step(insTool);
                    }
                    if (evId > maxEvId) maxEvId = evId;
                }
                if (insTool) sqlite3_finalize(insTool);
                sqlite3_finalize(eSt);
            }
            setCursor("telemetry_events", expTelPath, maxEvId, nowSec);
            sqlite3_close(tdb);
        }
    }

    // 3. Scan history.db (claude_usage_rows + quota_samples)
    std::string expHistPath = PathUtils::expandHome(historyDbPath);
    if (!expHistPath.empty() && fs::exists(expHistPath)) {
        sqlite3* hdb = nullptr;
        if (sqlite3_open_v2(expHistPath.c_str(), &hdb, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
            std::vector<PriceVersion> versions;
            {
                sqlite3_stmt* pvSt = nullptr;
                if (sqlite3_prepare_v2(hdb,
                    "SELECT fetched_at, page_sha256, catalog_json FROM claude_price_versions ORDER BY fetched_at, id;",
                    -1, &pvSt, nullptr) == SQLITE_OK) {
                    while (sqlite3_step(pvSt) == SQLITE_ROW) {
                        PriceVersion v;
                        v.effectiveFrom = sqlite3_column_int64(pvSt, 0);
                        const char* shaC = reinterpret_cast<const char*>(sqlite3_column_text(pvSt, 1));
                        const char* catC = reinterpret_cast<const char*>(sqlite3_column_text(pvSt, 2));
                        v.pageSha256 = shaC ? shaC : "";
                        std::string mErr;
                        if (catC && PriceCatalog::modelsFromJson(catC, v.models, mErr)) {
                            versions.push_back(std::move(v));
                        }
                    }
                    sqlite3_finalize(pvSt);
                }
            }
            if (versions.empty()) {
                PriceVersion defV;
                auto makePrice = [](int64_t inN, int64_t outN, int64_t rdN, int64_t w5N, int64_t w1N) {
                    ModelPrice mp;
                    mp.inputNano = inN; mp.outputNano = outN;
                    mp.readNano = rdN; mp.write5mNano = w5N; mp.write1hNano = w1N;
                    return mp;
                };
                defV.models["claude-opus-4"]     = makePrice(15000, 75000, 1500, 18750, 30000);
                defV.models["claude-opus-4-1"]   = makePrice(15000, 75000, 1500, 18750, 30000);
                defV.models["claude-opus-4-5"]   = makePrice(5000, 25000, 500, 6250, 10000);
                defV.models["claude-opus-4-6"]   = makePrice(5000, 25000, 500, 6250, 10000);
                defV.models["claude-opus-4-7"]   = makePrice(5000, 25000, 500, 6250, 10000);
                defV.models["claude-opus-4-8"]   = makePrice(5000, 25000, 500, 6250, 10000);
                defV.models["claude-opus-5"]     = makePrice(5000, 25000, 500, 6250, 10000);
                defV.models["claude-opus-5-5"]   = makePrice(4000, 20000, 200, 5000, 8000);
                defV.models["claude-sonnet-4"]   = makePrice(3000, 15000, 300, 3750, 6000);
                defV.models["claude-sonnet-4-5"] = makePrice(3000, 15000, 300, 3750, 6000);
                defV.models["claude-sonnet-4-6"] = makePrice(3000, 15000, 300, 3750, 6000);
                defV.models["claude-sonnet-5"]   = makePrice(2000, 10000, 200, 2500, 4000);
                defV.models["claude-sonnet-5-5"] = makePrice(2000, 10000, 100, 2500, 4000);
                defV.models["claude-haiku-3-5"]  = makePrice(800, 4000, 80, 1000, 1600);
                defV.models["claude-haiku-4-5"]  = makePrice(1000, 5000, 100, 1250, 2000);
                ModelPrice h55 = makePrice(100, 500, 10, 125, 200);
                h55.tierThresholdTokens = 100000;
                h55.upperTier = std::make_shared<ModelPrice>(makePrice(500, 2500, 50, 625, 1000));
                defV.models["claude-haiku-5-5"]  = h55;
                versions.push_back(std::move(defV));
            }

            sqlite3_stmt* cSt = nullptr;
            if (sqlite3_prepare_v2(hdb,
                "SELECT account, session_id, message_id, request_id, ts, model, speed, "
                "input, output, cache_read, cache_w5m, cache_w1h, web_search, sidechain "
                "FROM claude_usage_rows ORDER BY session_id ASC, ts ASC;", -1, &cSt, nullptr) == SQLITE_OK) {

                sqlite3_stmt* insReq = nullptr;
                sqlite3_prepare_v2(_db,
                    "INSERT OR REPLACE INTO ins_requests ("
                    "platform, account, session_id, project, request_id, message_id, ts, model, speed, "
                    "is_sidechain, input_tokens, output_tokens, cache_read, cache_write_5m, cache_write_1h, "
                    "context_tokens, compacted, thinking_tokens, web_search_reqs, est_cost_nano"
                    ") VALUES ('claude', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?);",
                    -1, &insReq, nullptr);

                std::string prevSess;
                int64_t prevCtx = 0;

                while (sqlite3_step(cSt) == SQLITE_ROW) {
                    std::string acct = reinterpret_cast<const char*>(sqlite3_column_text(cSt, 0));
                    std::string sId = reinterpret_cast<const char*>(sqlite3_column_text(cSt, 1));
                    std::string mId = reinterpret_cast<const char*>(sqlite3_column_text(cSt, 2));
                    std::string rId = reinterpret_cast<const char*>(sqlite3_column_text(cSt, 3));
                    int64_t ts = sqlite3_column_int64(cSt, 4);
                    std::string model = reinterpret_cast<const char*>(sqlite3_column_text(cSt, 5));
                    std::string speed = reinterpret_cast<const char*>(sqlite3_column_text(cSt, 6));
                    int64_t inT = sqlite3_column_int64(cSt, 7);
                    int64_t outT = sqlite3_column_int64(cSt, 8);
                    int64_t crT = sqlite3_column_int64(cSt, 9);
                    int64_t cw5T = sqlite3_column_int64(cSt, 10);
                    int64_t cw1T = sqlite3_column_int64(cSt, 11);
                    int64_t webReqs = sqlite3_column_int64(cSt, 12);
                    int side = sqlite3_column_int(cSt, 13);

                    int64_t ctxTok = inT + crT + cw5T + cw1T;
                    int compacted = 0;
                    if (!side) {
                        if (sId == prevSess && prevCtx > 0 &&
                            (prevCtx - ctxTok) >= 20000 &&
                            static_cast<double>(prevCtx - ctxTok) / static_cast<double>(prevCtx) >= 0.30) {
                            compacted = 1;
                        }
                        prevSess = sId;
                        prevCtx = ctxTok;
                    }

                    std::string proj = sessionProjectMap.count(sId) ? sessionProjectMap[sId] : "claude-project";
                    size_t verIdx = 0;
                    for (size_t vi = 0; vi < versions.size(); ++vi) {
                        if (versions[vi].effectiveFrom <= ts) verIdx = vi;
                    }
                    const auto& modelsMap = versions[verIdx].models;
                    std::string resolvedId = PriceCatalog::resolveModelId(modelsMap, model);
                    int64_t costNano = 0;
                    if (!resolvedId.empty()) {
                        const ModelPrice& base = modelsMap.at(resolvedId);
                        bool upper = (base.upperTier && base.tierThresholdTokens > 0 && ctxTok > base.tierThresholdTokens);
                        const ModelPrice& mp = upper ? *base.upperTier : base;
                        bool fast = (speed == "fast" && !upper && base.hasFast);
                        if (fast) {
                            costNano = inT * mp.fastInputNano + outT * mp.fastOutputNano +
                                       crT * mp.fastReadNano + cw5T * mp.fastWrite5mNano +
                                       cw1T * mp.fastWrite1hNano;
                        } else {
                            costNano = inT * mp.inputNano + outT * mp.outputNano +
                                       crT * mp.readNano + cw5T * mp.write5mNano +
                                       cw1T * mp.write1hNano;
                        }
                    }

                    if (insReq) {
                        sqlite3_reset(insReq);
                        sqlite3_bind_text(insReq, 1, acct.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(insReq, 2, sId.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(insReq, 3, proj.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(insReq, 4, rId.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(insReq, 5, mId.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int64(insReq, 6, ts);
                        sqlite3_bind_text(insReq, 7, model.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(insReq, 8, speed.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int(insReq, 9, side);
                        sqlite3_bind_int64(insReq, 10, inT);
                        sqlite3_bind_int64(insReq, 11, outT);
                        sqlite3_bind_int64(insReq, 12, crT);
                        sqlite3_bind_int64(insReq, 13, cw5T);
                        sqlite3_bind_int64(insReq, 14, cw1T);
                        sqlite3_bind_int64(insReq, 15, ctxTok);
                        sqlite3_bind_int(insReq, 16, compacted);
                        sqlite3_bind_int64(insReq, 17, webReqs);
                        sqlite3_bind_int64(insReq, 18, costNano);
                        sqlite3_step(insReq);
                    }
                }
                if (insReq) sqlite3_finalize(insReq);
                sqlite3_finalize(cSt);
            }

            int64_t lastQId = 0, qMtime = 0;
            getCursor("history_quota", expHistPath, lastQId, qMtime);
            int64_t maxQId = lastQId;

            sqlite3_stmt* qSt = nullptr;
            if (sqlite3_prepare_v2(hdb,
                "SELECT id, timestamp, provider, metric_key, metric_value, metric_limit, delta "
                "FROM quota_samples WHERE id > ? ORDER BY id ASC;", -1, &qSt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(qSt, 1, lastQId);

                sqlite3_stmt* insQ = nullptr;
                sqlite3_prepare_v2(_db,
                    "INSERT OR REPLACE INTO ins_quota (ts, platform, metric_key, value, limit_val, delta) "
                    "VALUES (?, ?, ?, ?, ?, ?);",
                    -1, &insQ, nullptr);

                while (sqlite3_step(qSt) == SQLITE_ROW) {
                    int64_t qId = sqlite3_column_int64(qSt, 0);
                    int64_t ts = sqlite3_column_int64(qSt, 1);
                    const char* provC = reinterpret_cast<const char*>(sqlite3_column_text(qSt, 2));
                    const char* keyC = reinterpret_cast<const char*>(sqlite3_column_text(qSt, 3));
                    double val = sqlite3_column_double(qSt, 4);
                    double lim = sqlite3_column_double(qSt, 5);
                    double del = sqlite3_column_double(qSt, 6);

                    if (insQ && provC && keyC) {
                        std::string qPlat(provC);
                        std::string qKey(keyC);
                        if (qPlat.rfind("claude:", 0) == 0) {
                            std::string acct = qPlat.substr(7);
                            qPlat = "claude";
                            if (!acct.empty() && acct != "default") {
                                qKey = acct + ":" + qKey;
                            }
                        }
                        sqlite3_reset(insQ);
                        sqlite3_bind_int64(insQ, 1, ts);
                        sqlite3_bind_text(insQ, 2, qPlat.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(insQ, 3, qKey.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_double(insQ, 4, val);
                        sqlite3_bind_double(insQ, 5, lim);
                        sqlite3_bind_double(insQ, 6, del);
                        sqlite3_step(insQ);
                    }
                    if (qId > maxQId) maxQId = qId;
                }
                if (insQ) sqlite3_finalize(insQ);
                sqlite3_finalize(qSt);
            }
            setCursor("history_quota", expHistPath, maxQId, nowSec);
            sqlite3_close(hdb);
        }
    }

    // 4. Scan Antigravity roots and track 8-char prefix collisions (§2 item 14)
    std::map<std::string, std::set<std::string>> agyPrefixToUuids;
    std::map<std::string, std::string> agyProjectMap;
    for (const auto& rawRoot : antigravityRoots) {
        std::string agyRoot = PathUtils::expandHome(rawRoot);
        if (agyRoot.empty() || !fs::exists(agyRoot)) continue;

        std::string sumDb = agyRoot + "/conversation_summaries.db";
        if (fs::exists(sumDb)) {
            sqlite3* sdb = nullptr;
            if (sqlite3_open_v2(sumDb.c_str(), &sdb, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
                sqlite3_stmt* sSt = nullptr;
                if (sqlite3_prepare_v2(sdb,
                    "SELECT conversation_id, workspace_uris FROM conversation_summaries;",
                    -1, &sSt, nullptr) == SQLITE_OK) {
                    while (sqlite3_step(sSt) == SQLITE_ROW) {
                        const char* cidC = reinterpret_cast<const char*>(sqlite3_column_text(sSt, 0));
                        const char* urisC = reinterpret_cast<const char*>(sqlite3_column_text(sSt, 1));
                        if (cidC && urisC) {
                            nlohmann::json uj = nlohmann::json::parse(urisC, nullptr, false);
                            if (uj.is_array() && !uj.empty() && uj[0].is_string()) {
                                std::string uStr = uj[0].get<std::string>();
                                std::string b = basenameOnly(uStr);
                                if (!b.empty()) agyProjectMap[cidC] = b;
                            }
                        }
                    }
                    sqlite3_finalize(sSt);
                }
                sqlite3_close(sdb);
            }
        }

        std::string histJsonl = agyRoot + "/history.jsonl";
        if (fs::exists(histJsonl)) {
            std::ifstream hin(histJsonl);
            std::string hline;
            while (std::getline(hin, hline)) {
                if (hline.empty()) continue;
                nlohmann::json hj = nlohmann::json::parse(hline, nullptr, false);
                if (hj.is_discarded() || !hj.is_object()) continue;
                std::string cid = hj.value("conversationId", "");
                std::string ws = hj.value("workspace", "");
                if (!cid.empty() && !ws.empty() && !agyProjectMap.count(cid)) {
                    std::string b = basenameOnly(ws);
                    if (!b.empty()) agyProjectMap[cid] = b;
                }
            }
        }

        std::string convDir = agyRoot + "/conversations";
        if (fs::exists(convDir)) {
            for (const auto& entry : fs::directory_iterator(convDir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".db") {
                    std::string u = entry.path().stem().string();
                    if (u.size() >= 8) agyPrefixToUuids[u.substr(0, 8)].insert(u);
                }
            }
        }
    }

    for (const auto& rawRoot : antigravityRoots) {
        std::string agyRoot = PathUtils::expandHome(rawRoot);
        if (agyRoot.empty() || !fs::exists(agyRoot)) continue;

        std::string convDir = agyRoot + "/conversations";
        if (fs::exists(convDir)) {
            for (const auto& entry : fs::directory_iterator(convDir)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".db") continue;
                std::string dbFile = entry.path().string();
                std::string convUuid = entry.path().stem().string();
                std::string shortSessId = (convUuid.size() >= 8) ? ("sess-" + convUuid.substr(0, 8)) : convUuid;
                std::string agyProj = agyProjectMap.count(convUuid) ? agyProjectMap[convUuid] :
                                      (sessionProjectMap.count(shortSessId) ? sessionProjectMap[shortSessId] : "antigravity");

                int64_t lastGenIdx = -1, lastStepIdx = -1, dummyMtime = 0;
                getCursor("agy_gen", dbFile, lastGenIdx, dummyMtime);
                getCursor("agy_step", dbFile, lastStepIdx, dummyMtime);

                sqlite3* cdb = nullptr;
                if (sqlite3_open_v2(dbFile.c_str(), &cdb, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
                    if (cdb) sqlite3_close(cdb);
                    continue;
                }

                int64_t maxGenIdx = lastGenIdx;
                sqlite3_stmt* gSt = nullptr;
                if (sqlite3_prepare_v2(cdb,
                    "SELECT idx, data, size FROM gen_metadata WHERE idx > ? ORDER BY idx ASC;",
                    -1, &gSt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int64(gSt, 1, lastGenIdx);

                    sqlite3_stmt* insGen = nullptr;
                    sqlite3_prepare_v2(_db,
                        "INSERT OR REPLACE INTO ins_generations ("
                        "conversation_id, session_id, idx, ts, model_enum, input_tokens, output_tokens, "
                        "thinking_tokens, response_tokens, cache_read_tokens, f1_f4_f6, context_tokens, "
                        "context_limit, max_output_tokens, ttft_ms, duration_ms, validated"
                        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
                        -1, &insGen, nullptr);

                    sqlite3_stmt* insReq = nullptr;
                    sqlite3_prepare_v2(_db,
                        "INSERT OR REPLACE INTO ins_requests ("
                        "platform, account, session_id, project, request_id, message_id, ts, model, speed, "
                        "is_sidechain, input_tokens, output_tokens, cache_read, cache_write_5m, cache_write_1h, "
                        "context_tokens, compacted, thinking_tokens, web_search_reqs, est_cost_nano"
                        ") VALUES ('antigravity', 'default', ?, ?, ?, ?, ?, ?, 'standard', 0, ?, ?, ?, 0, 0, ?, 0, ?, 0, 0);",
                        -1, &insReq, nullptr);

                    while (sqlite3_step(gSt) == SQLITE_ROW) {
                        int64_t idx = sqlite3_column_int64(gSt, 0);
                        const uint8_t* blob = static_cast<const uint8_t*>(sqlite3_column_blob(gSt, 1));
                        int blobLen = sqlite3_column_bytes(gSt, 1);

                        GenerationRecord rec;
                        if (blob && blobLen > 0 && decodeGenMetadata(blob, static_cast<size_t>(blobLen), rec)) {
                            rec.conversationId = convUuid;
                            rec.sessionId = shortSessId;
                            rec.idx = idx;
                            if (rec.ts <= 0) rec.ts = nowSec;

                            if (insGen) {
                                sqlite3_reset(insGen);
                                sqlite3_bind_text(insGen, 1, convUuid.c_str(), -1, SQLITE_TRANSIENT);
                                sqlite3_bind_text(insGen, 2, shortSessId.c_str(), -1, SQLITE_TRANSIENT);
                                sqlite3_bind_int64(insGen, 3, idx);
                                sqlite3_bind_int64(insGen, 4, rec.ts);
                                sqlite3_bind_int64(insGen, 5, rec.modelEnum);
                                sqlite3_bind_int64(insGen, 6, rec.inputTokens);
                                sqlite3_bind_int64(insGen, 7, rec.outputTokens);
                                sqlite3_bind_int64(insGen, 8, rec.thinkingTokens);
                                sqlite3_bind_int64(insGen, 9, rec.responseTokens);
                                sqlite3_bind_int64(insGen, 10, rec.cacheReadTokens);
                                sqlite3_bind_int64(insGen, 11, rec.f1F4F6);
                                sqlite3_bind_int64(insGen, 12, rec.contextTokens);
                                sqlite3_bind_int64(insGen, 13, rec.contextLimit);
                                sqlite3_bind_int64(insGen, 14, rec.maxOutputTokens);
                                sqlite3_bind_double(insGen, 15, rec.ttftMs);
                                sqlite3_bind_double(insGen, 16, rec.durationMs);
                                sqlite3_bind_int(insGen, 17, rec.validated);
                                sqlite3_step(insGen);
                            }

                            if (insReq && rec.validated == 1) {
                                std::string reqId = convUuid + ":" + std::to_string(idx);
                                std::string mName = mapModelEnumToName(rec.modelEnum);
                                sqlite3_reset(insReq);
                                sqlite3_bind_text(insReq, 1, convUuid.c_str(), -1, SQLITE_TRANSIENT);
                                sqlite3_bind_text(insReq, 2, agyProj.c_str(), -1, SQLITE_TRANSIENT);
                                sqlite3_bind_text(insReq, 3, reqId.c_str(), -1, SQLITE_TRANSIENT);
                                sqlite3_bind_text(insReq, 4, reqId.c_str(), -1, SQLITE_TRANSIENT);
                                sqlite3_bind_int64(insReq, 5, rec.ts);
                                sqlite3_bind_text(insReq, 6, mName.c_str(), -1, SQLITE_TRANSIENT);
                                sqlite3_bind_int64(insReq, 7, rec.inputTokens);
                                sqlite3_bind_int64(insReq, 8, rec.outputTokens);
                                sqlite3_bind_int64(insReq, 9, rec.cacheReadTokens);
                                sqlite3_bind_int64(insReq, 10, rec.contextTokens);
                                sqlite3_bind_int64(insReq, 11, rec.thinkingTokens);
                                sqlite3_step(insReq);
                            }
                        }
                        if (idx > maxGenIdx) maxGenIdx = idx;
                    }
                    if (insGen) sqlite3_finalize(insGen);
                    if (insReq) sqlite3_finalize(insReq);
                    sqlite3_finalize(gSt);
                }
                setCursor("agy_gen", dbFile, maxGenIdx, nowSec);

                int64_t maxStepIdx = lastStepIdx;
                sqlite3_stmt* stSt = nullptr;
                if (sqlite3_prepare_v2(cdb,
                    "SELECT idx, step_type FROM steps WHERE idx > ? ORDER BY idx ASC;",
                    -1, &stSt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int64(stSt, 1, lastStepIdx);

                    sqlite3_stmt* insTool = nullptr;
                    sqlite3_prepare_v2(_db,
                        "INSERT OR IGNORE INTO ins_tool_calls "
                        "(platform, session_id, ts, step_index, tool_name, target_hash, duration_ms, status) "
                        "VALUES ('antigravity', ?, ?, ?, ?, '', 0.0, ?);",
                        -1, &insTool, nullptr);

                    while (sqlite3_step(stSt) == SQLITE_ROW) {
                        int64_t sIdx = sqlite3_column_int64(stSt, 0);
                        int sType = sqlite3_column_int(stSt, 1);
                        std::string tName = mapAntigravityStepType(sType);
                        std::string statusStr = (sType == 17) ? "ERROR" : "OK";
                        if (insTool) {
                            sqlite3_reset(insTool);
                            sqlite3_bind_text(insTool, 1, convUuid.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_int64(insTool, 2, nowSec);
                            sqlite3_bind_int64(insTool, 3, sIdx);
                            sqlite3_bind_text(insTool, 4, tName.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_text(insTool, 5, statusStr.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_step(insTool);
                        }
                        if (sIdx > maxStepIdx) maxStepIdx = sIdx;
                    }
                    if (insTool) sqlite3_finalize(insTool);
                    sqlite3_finalize(stSt);
                }
                setCursor("agy_step", dbFile, maxStepIdx, nowSec);
                sqlite3_close(cdb);
            }
        }

        std::string brainDir = agyRoot + "/brain";
        if (fs::exists(brainDir)) {
            for (const auto& bEntry : fs::directory_iterator(brainDir)) {
                if (!bEntry.is_directory()) continue;
                std::string uuid = bEntry.path().filename().string();
                std::string shortSessId = (uuid.size() >= 8) ? ("sess-" + uuid.substr(0, 8)) : uuid;
                std::string agyProj = agyProjectMap.count(uuid) ? agyProjectMap[uuid] :
                                      (sessionProjectMap.count(shortSessId) ? sessionProjectMap[shortSessId] : "antigravity");
                std::string transPath = bEntry.path().string() + "/.system_generated/logs/transcript.jsonl";
                if (!fs::exists(transPath)) continue;

                std::error_code ec;
                auto tSize = static_cast<int64_t>(fs::file_size(transPath, ec));
                auto tMtime = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        fs::last_write_time(transPath, ec).time_since_epoch()).count());
                int64_t prevOff = 0, prevMt = 0;
                getCursor("ag_transcript", transPath, prevOff, prevMt);
                if (prevOff == tSize && prevMt == tMtime) continue;

                std::ifstream tin(transPath);
                if (!tin.is_open()) continue;

                int turnIdx = 0;
                std::string tline;
                sqlite3_stmt* pStmt = nullptr;
                sqlite3_prepare_v2(_db,
                    "INSERT OR REPLACE INTO ins_prompts ("
                    "platform, session_id, turn_index, ts, project, chars, words, lines, "
                    "prose_chars, prose_words, fenced_chars, fenced_block_count, paste_chars, "
                    "mixed_paste, file_path_count, bullet_count, is_sidechain, has_code_block, "
                    "has_file_ref, has_constraints, is_first_in_session, next_tool_calls, "
                    "next_tool_errors, next_edits, next_Read_before_Edit, reprompt_within_120s, "
                    "interrupted_turn, turn_tokens, turn_cost_nano"
                    ") VALUES ('antigravity', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?, ?, 0, 0, 0, 0, 0, 0, 0, 0);",
                    -1, &pStmt, nullptr);

                while (std::getline(tin, tline)) {
                    if (tline.empty()) continue;
                    nlohmann::json tj = nlohmann::json::parse(tline, nullptr, false);
                    if (tj.is_discarded() || !tj.is_object()) continue;
                    if (tj.value("type", "") == "USER_INPUT" && tj.contains("content") && tj["content"].is_string()) {
                        std::string content = tj["content"].get<std::string>();
                        int64_t ts = parseIso8601ToEpoch(tj.value("created_at", ""));
                        if (ts <= 0) ts = nowSec;
                        PromptFeatures pf = extractPromptFeatures(content);
                        pf.isFirstInSession = (turnIdx == 0) ? 1 : 0;
                        if (pStmt) {
                            sqlite3_reset(pStmt);
                            sqlite3_bind_text(pStmt, 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_int(pStmt, 2, turnIdx++);
                            sqlite3_bind_int64(pStmt, 3, ts);
                            sqlite3_bind_text(pStmt, 4, agyProj.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_int64(pStmt, 5, pf.chars);
                            sqlite3_bind_int64(pStmt, 6, pf.words);
                            sqlite3_bind_int64(pStmt, 7, pf.lines);
                            sqlite3_bind_int64(pStmt, 8, pf.proseChars);
                            sqlite3_bind_int64(pStmt, 9, pf.proseWords);
                            sqlite3_bind_int64(pStmt, 10, pf.fencedChars);
                            sqlite3_bind_int64(pStmt, 11, pf.fencedBlockCount);
                            sqlite3_bind_int64(pStmt, 12, pf.pasteChars);
                            sqlite3_bind_int(pStmt, 13, pf.mixedPaste);
                            sqlite3_bind_int64(pStmt, 14, pf.filePathCount);
                            sqlite3_bind_int64(pStmt, 15, pf.bulletCount);
                            sqlite3_bind_int(pStmt, 16, pf.hasCodeBlock);
                            sqlite3_bind_int(pStmt, 17, pf.hasFileRef);
                            sqlite3_bind_int(pStmt, 18, pf.hasConstraints);
                            sqlite3_bind_int(pStmt, 19, pf.isFirstInSession);
                            sqlite3_step(pStmt);
                        }
                    }
                }
                if (pStmt) sqlite3_finalize(pStmt);
                setCursor("ag_transcript", transPath, tSize, tMtime);
            }
        }
    }

    // 4b. Scan Cursor chats/**/meta.json and Remote-SSH checkpoints/*/metadata.json (§3.3 items 3 & 5)
    std::map<std::string, std::string> cursorWsHashMap;
    std::string expCursorHome = PathUtils::expandHome(cursorHomeDir);
    if (!expCursorHome.empty() && fs::exists(expCursorHome + "/chats")) {
        std::error_code ec;
        for (const auto& cEntry : fs::recursive_directory_iterator(
                 expCursorHome + "/chats", fs::directory_options::skip_permission_denied, ec)) {
            if (!cEntry.is_regular_file() || cEntry.path().filename() != "meta.json") continue;
            std::string metaPath = cEntry.path().string();
            std::ifstream min(metaPath);
            if (!min.is_open()) continue;
            nlohmann::json mj = nlohmann::json::parse(min, nullptr, false);
            if (mj.is_discarded() || !mj.is_object()) continue;
            fs::path chatDir = cEntry.path().parent_path();
            std::string chatId = mj.value("chatId", chatDir.filename().string());
            std::string cwd = mj.value("cwd", "");
            std::string proj = !cwd.empty() ? decodeProjectSlug(cwd) : "unattributed";
            fs::path wsHashDir = chatDir.parent_path();
            if (wsHashDir.filename() != "chats" && proj != "unattributed" && proj != "unknown") {
                cursorWsHashMap[wsHashDir.filename().string()] = proj;
            }
            int64_t createdMs = mj.value("createdAt", mj.value("createdAtMs", int64_t(0)));
            int64_t createdSec = (createdMs > 1000000000000LL) ? (createdMs / 1000) : (createdMs > 0 ? createdMs : nowSec);
            if (!chatId.empty() && !proj.empty()) {
                sessionProjectMap[chatId] = proj;
                sqlite3_stmt* csSt = nullptr;
                if (sqlite3_prepare_v2(_db,
                    "INSERT OR IGNORE INTO ins_sessions ("
                    "platform, session_id, conversation_id, session_link, project, model_primary, "
                    "start_ts, end_ts, duration_sec, active_sec, turn_count, request_count, "
                    "sidechain_reqs, tool_calls, tool_errors, edit_calls, bash_calls, subagent_spawns, "
                    "input_tokens, output_tokens, cache_read, cache_write, peak_context, compaction_count, "
                    "est_cost_nano, cost_basis, status"
                    ") VALUES ('cursor', ?, ?, 'exact', ?, 'cursor', ?, ?, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'none', 'COMPLETED');",
                    -1, &csSt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(csSt, 1, chatId.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(csSt, 2, chatId.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(csSt, 3, proj.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(csSt, 4, createdSec);
                    sqlite3_bind_int64(csSt, 5, createdSec);
                    sqlite3_step(csSt);
                    sqlite3_finalize(csSt);
                }
            }
        }
    }

    std::string expCursorSrv = PathUtils::expandHome(cursorServerDir);
    std::string cpRoot = expCursorSrv + "/data/User/globalStorage/anysphere.cursor-commits/checkpoints";
    if (!expCursorSrv.empty() && fs::exists(cpRoot)) {
        sqlite3_stmt* cpReq = nullptr;
        sqlite3_prepare_v2(_db,
            "INSERT OR REPLACE INTO ins_requests ("
            "platform, account, session_id, project, request_id, message_id, ts, model, speed, "
            "is_sidechain, input_tokens, output_tokens, cache_read, cache_write_5m, cache_write_1h, "
            "context_tokens, compacted, thinking_tokens, web_search_reqs, est_cost_nano"
            ") VALUES ('cursor', 'default', ?, ?, ?, ?, ?, 'cursor-checkpoint', 'standard', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);",
            -1, &cpReq, nullptr);
        for (const auto& cpEntry : fs::directory_iterator(cpRoot)) {
            if (!cpEntry.is_directory()) continue;
            std::string metaPath = cpEntry.path().string() + "/metadata.json";
            if (!fs::exists(metaPath)) continue;
            std::ifstream cpin(metaPath);
            if (!cpin.is_open()) continue;
            nlohmann::json cj = nlohmann::json::parse(cpin, nullptr, false);
            if (cj.is_discarded() || !cj.is_object()) continue;
            std::string reqId = cj.value("agentRequestId", cpEntry.path().filename().string());
            std::string wsId = cj.value("workspaceId", "");
            std::string proj = cursorWsHashMap.count(wsId) ? cursorWsHashMap[wsId] : decodeProjectSlug(wsId);
            if (proj.empty() || proj == "unknown") proj = "unattributed";
            std::string cpSessionKey = !wsId.empty() ? ("cp-" + wsId.substr(0, 8)) : ("cp-" + proj);
            int64_t tsMs = cj.value("startTrackingDateUnixMilliseconds", int64_t(0));
            int64_t tsSec = (tsMs > 1000000000000LL) ? (tsMs / 1000) : (tsMs > 0 ? tsMs : nowSec);
            if (cpReq && !reqId.empty()) {
                sqlite3_reset(cpReq);
                sqlite3_bind_text(cpReq, 1, cpSessionKey.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(cpReq, 2, proj.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(cpReq, 3, reqId.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(cpReq, 4, cpEntry.path().filename().string().c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(cpReq, 5, tsSec);
                sqlite3_step(cpReq);
            }
        }
        if (cpReq) sqlite3_finalize(cpReq);
    }

    // 5. Roll up ins_requests into ins_sessions and ins_daily
    const char* rollupSql = R"SQL(
        INSERT OR REPLACE INTO ins_sessions (
            platform, session_id, conversation_id, session_link, project, model_primary,
            start_ts, end_ts, duration_sec, active_sec, turn_count, request_count,
            sidechain_reqs, tool_calls, tool_errors, edit_calls, bash_calls, subagent_spawns,
            input_tokens, output_tokens, cache_read, cache_write, peak_context, compaction_count,
            est_cost_nano, cost_basis, status
        )
        SELECT
            r.platform,
            r.session_id,
            CASE WHEN r.platform='antigravity' THEN r.session_id ELSE '' END,
            CASE WHEN r.platform='antigravity' THEN 'prefix' ELSE 'exact' END,
            MAX(r.project),
            MAX(r.model),
            MIN(r.ts),
            MAX(r.ts),
            MAX(r.ts) - MIN(r.ts),
            MAX(r.ts) - MIN(r.ts),
            COUNT(*),
            COUNT(*),
            SUM(r.is_sidechain),
            COALESCE((SELECT COUNT(*) FROM ins_tool_calls t WHERE t.platform = r.platform AND t.session_id = r.session_id), 0),
            COALESCE((SELECT COUNT(*) FROM ins_tool_calls t WHERE t.platform = r.platform AND t.session_id = r.session_id AND t.status != 'OK'), 0),
            COALESCE((SELECT COUNT(*) FROM ins_tool_calls t WHERE t.platform = r.platform AND t.session_id = r.session_id AND t.tool_name IN ('Edit','MultiEdit','Write','CODE_ACTION')), 0),
            COALESCE((SELECT COUNT(*) FROM ins_tool_calls t WHERE t.platform = r.platform AND t.session_id = r.session_id AND t.tool_name IN ('Bash','RUN_COMMAND')), 0),
            SUM(r.is_sidechain),
            SUM(r.input_tokens),
            SUM(r.output_tokens),
            SUM(r.cache_read),
            SUM(r.cache_write_5m + r.cache_write_1h),
            MAX(r.context_tokens),
            SUM(r.compacted),
            SUM(r.est_cost_nano),
            CASE WHEN r.platform='claude' THEN 'list_price' ELSE 'none' END,
            'COMPLETED'
        FROM ins_requests r
        GROUP BY r.platform, r.session_id;

        INSERT OR REPLACE INTO ins_daily (platform, day, model, tokens, est_cost_nano, sessions, requests)
        SELECT
            platform,
            strftime('%Y-%m-%d', datetime(ts, 'unixepoch')) AS d,
            model,
            SUM(input_tokens + output_tokens + cache_read + cache_write_5m + cache_write_1h),
            SUM(est_cost_nano),
            COUNT(DISTINCT session_id),
            COUNT(*)
        FROM ins_requests
        GROUP BY platform, d, model;
    )SQL";
    sqlite3_exec(_db, rollupSql, nullptr, nullptr, nullptr);

    // Mark ambiguous Antigravity session_link if any 8-char prefix had >1 UUID
    for (const auto& kv : agyPrefixToUuids) {
        if (kv.second.size() > 1) {
            for (const auto& u : kv.second) {
                sqlite3_stmt* uSt = nullptr;
                if (sqlite3_prepare_v2(_db,
                    "UPDATE ins_sessions SET session_link='ambiguous' WHERE platform='antigravity' AND session_id=?;",
                    -1, &uSt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(uSt, 1, u.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(uSt);
                    sqlite3_finalize(uSt);
                }
            }
        }
    }

    sqlite3_exec(_db, R"SQL(
        UPDATE ins_prompts
        SET turn_cost_nano = COALESCE((
            SELECT CAST(s.est_cost_nano / MAX(1, s.turn_count) AS INTEGER)
            FROM ins_sessions s
            WHERE s.platform = ins_prompts.platform AND s.session_id = ins_prompts.session_id
        ), 0)
        WHERE turn_cost_nano = 0;
    )SQL", nullptr, nullptr, nullptr);

    int64_t cutoff = nowSec - static_cast<int64_t>(_config.insights.retentionDays) * 86400LL;
    pruneOldRecords(cutoff);

    sqlite3_exec(_db, "COMMIT;", nullptr, nullptr, nullptr);

    lock.unlock();
    return runDetectors(30);
}

void InsightEngine::pruneOldRecords(int64_t cutoffEpoch) {
    if (!_db || cutoffEpoch <= 0) return;
    const char* tables[] = {
        "DELETE FROM ins_requests WHERE ts < ?;",
        "DELETE FROM ins_sessions WHERE start_ts < ?;",
        "DELETE FROM ins_tool_calls WHERE ts < ?;",
        "DELETE FROM ins_quota WHERE ts < ?;",
        "DELETE FROM ins_generations WHERE ts < ?;",
        "DELETE FROM ins_prompts WHERE ts < ?;",
        "DELETE FROM ins_findings WHERE computed_ts < ?;"
    };
    for (const char* sql : tables) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(_db, sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, cutoffEpoch);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
}

void InsightEngine::insertFinding(const InsightFinding& f) {
    if (!_db) return;
    int64_t nowSec = static_cast<int64_t>(std::time(nullptr));
    const char* sql =
        "INSERT INTO ins_findings ("
        "computed_ts, window_days, detector, platform, scope, scope_id, severity, confidence, "
        "n, metric, summary, evidence_json, suggested_experiment, experiment_start, "
        "experiment_metric, first_seen, status"
        ") VALUES (?, 30, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &st, nullptr) == SQLITE_OK) {
        std::string evStr = f.evidenceJson.dump();
        sqlite3_bind_int64(st, 1, nowSec);
        sqlite3_bind_text(st, 2, f.detector.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, f.platform.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, f.scope.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, f.scopeId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 6, f.severity.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 7, f.confidence.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 8, f.n);
        sqlite3_bind_double(st, 9, f.metric);
        sqlite3_bind_text(st, 10, f.summary.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 11, evStr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 12, f.suggestedExperiment.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 13, f.experimentStart > 0 ? f.experimentStart : nowSec);
        sqlite3_bind_text(st, 14, f.experimentMetric.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 15, f.firstSeen > 0 ? f.firstSeen : nowSec);
        sqlite3_bind_text(st, 16, f.status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

bool InsightEngine::runDetectors(int days) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) return false;

    int64_t nowSec = static_cast<int64_t>(std::time(nullptr));
    int64_t sinceTs = nowSec - static_cast<int64_t>(days) * 86400LL;
    int minN = std::max(1, _config.insights.coachingMinSampleSize);

    sqlite3_exec(_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    sqlite3_exec(_db, "DELETE FROM ins_findings;", nullptr, nullptr, nullptr);

    // 1. cache_efficiency / cache_hit_collapse
    {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT platform, session_id, project, input_tokens, cache_read, cache_write, request_count "
            "FROM ins_sessions WHERE start_ts >= ? AND (input_tokens + cache_read + cache_write) > 40000;";
        if (sqlite3_prepare_v2(_db, sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, sinceTs);
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string plat = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                std::string sId = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                std::string proj = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                int64_t inT = sqlite3_column_int64(st, 3);
                int64_t crT = sqlite3_column_int64(st, 4);
                int64_t cwT = sqlite3_column_int64(st, 5);
                int64_t reqs = sqlite3_column_int64(st, 6);
                double denom = static_cast<double>(inT + crT + cwT);
                double ratio = (denom > 0.0) ? (static_cast<double>(crT) / denom) : 1.0;
                if (ratio < _config.insights.cacheEfficiencyThreshold) {
                    InsightFinding f;
                    f.detector = "cache_efficiency";
                    f.platform = plat;
                    f.scope = "session";
                    f.scopeId = sId;
                    f.severity = (ratio < 0.25) ? "warn" : "info";
                    f.confidence = (reqs >= 5) ? "high" : "medium";
                    f.n = reqs;
                    f.metric = ratio;
                    f.summary = "Session " + sId.substr(0, 8) + " (" + proj + ") cache-read ratio is " +
                                std::to_string(static_cast<int>(ratio * 100)) + "% across " + std::to_string(reqs) + " requests.";
                    f.evidenceJson = {{"session_id", sId}, {"project", proj}, {"cache_read_ratio", ratio}, {"requests", reqs}};
                    f.suggestedExperiment = "Keep stable instructions in CLAUDE.md and resume existing sessions within 5 minutes.";
                    f.experimentMetric = "cache_read_ratio";
                    insertFinding(f);
                }
            }
            sqlite3_finalize(st);
        }
    }

    // 2. subagent_share & delegation_style
    {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT session_id, project, COUNT(*) AS total_reqs, SUM(is_sidechain) AS side_reqs, "
            "SUM(CASE WHEN is_sidechain=1 THEN est_cost_nano ELSE 0 END) AS side_cost, "
            "SUM(est_cost_nano) AS total_cost "
            "FROM ins_requests WHERE platform='claude' AND ts >= ? GROUP BY session_id HAVING side_reqs > 0;";
        if (sqlite3_prepare_v2(_db, sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, sinceTs);
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string sId = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                std::string proj = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                int64_t totReqs = sqlite3_column_int64(st, 2);
                int64_t sideReqs = sqlite3_column_int64(st, 3);
                int64_t sideCost = sqlite3_column_int64(st, 4);
                int64_t totCost = sqlite3_column_int64(st, 5);
                double share = (totCost > 0) ? (static_cast<double>(sideCost) / static_cast<double>(totCost))
                                             : (static_cast<double>(sideReqs) / static_cast<double>(std::max<int64_t>(1, totReqs)));
                if (share > _config.insights.subagentShareThreshold) {
                    InsightFinding f;
                    f.detector = "subagent_share";
                    f.platform = "claude";
                    f.scope = "session";
                    f.scopeId = sId;
                    f.severity = (share > 0.60) ? "warn" : "info";
                    f.confidence = (totReqs >= 5) ? "high" : "medium";
                    f.n = totReqs;
                    f.metric = share;
                    f.summary = "Subagent sidechains accounted for " + std::to_string(static_cast<int>(share * 100)) +
                                "% of session " + sId.substr(0, 8) + " (" + std::to_string(sideReqs) + "/" + std::to_string(totReqs) + " requests).";
                    f.evidenceJson = {{"session_id", sId}, {"project", proj}, {"sidechain_share", share}, {"sidechain_requests", sideReqs}};
                    f.suggestedExperiment = "Use direct Grep/Read for targeted lookups instead of spawning subagents.";
                    f.experimentMetric = "sidechain_cost_share";
                    insertFinding(f);

                    InsightFinding df;
                    df.detector = "delegation_style";
                    df.platform = "claude";
                    df.scope = "session";
                    df.scopeId = sId;
                    df.severity = "info";
                    df.confidence = (totReqs >= 5) ? "high" : "medium";
                    df.n = totReqs;
                    df.metric = share;
                    df.summary = "Session " + sId.substr(0, 8) + " (" + proj + ") delegated " +
                                 std::to_string(sideReqs) + " subagent requests (" + std::to_string(totReqs - sideReqs) + " main-thread requests).";
                    df.evidenceJson = {{"session_id", sId}, {"project", proj}, {"sidechain_requests", sideReqs}, {"main_requests", totReqs - sideReqs}};
                    df.suggestedExperiment = "Compare follow-up prompt counts on subagent-delegated tasks vs direct single-agent execution.";
                    df.experimentMetric = "sidechain_cost_share";
                    insertFinding(df);
                }
            }
            sqlite3_finalize(st);
        }
    }

    // 3. long_context, compact_opportunity, long_running_sessions & session_sprawl
    {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT platform, session_id, project, request_count, peak_context, compaction_count, duration_sec "
            "FROM ins_sessions WHERE start_ts >= ?;";
        if (sqlite3_prepare_v2(_db, sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, sinceTs);
            int64_t longSec = static_cast<int64_t>(_config.insights.longSessionHours) * 3600LL;
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string plat = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                std::string sId = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                std::string proj = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                int64_t reqs = sqlite3_column_int64(st, 3);
                int64_t peakCtx = sqlite3_column_int64(st, 4);
                int64_t compCnt = sqlite3_column_int64(st, 5);
                int64_t durSec = sqlite3_column_int64(st, 6);

                if (peakCtx >= _config.insights.longContextThresholdTokens) {
                    InsightFinding f;
                    f.detector = "long_context";
                    f.platform = plat;
                    f.scope = "session";
                    f.scopeId = sId;
                    f.severity = "warn";
                    f.confidence = "high";
                    f.n = reqs;
                    f.metric = static_cast<double>(peakCtx);
                    f.summary = "Session " + sId.substr(0, 8) + " (" + proj + ") reached " +
                                std::to_string(peakCtx / 1000) + "k context tokens across " + std::to_string(reqs) + " requests.";
                    f.evidenceJson = {{"session_id", sId}, {"project", proj}, {"peak_context", peakCtx}, {"requests", reqs}};
                    f.suggestedExperiment = "Run /compact or start a new session when context passes 100k tokens.";
                    f.experimentMetric = "peak_context_tokens";
                    insertFinding(f);
                }

                if (peakCtx >= _config.insights.compactContextThresholdTokens &&
                    compCnt == 0 && reqs >= _config.insights.compactContinueRequests) {
                    InsightFinding f;
                    f.detector = "compact_opportunity";
                    f.platform = plat;
                    f.scope = "session";
                    f.scopeId = sId;
                    f.severity = "info";
                    f.confidence = "high";
                    f.n = reqs;
                    f.metric = static_cast<double>(peakCtx);
                    f.summary = "Session " + sId.substr(0, 8) + " ran " + std::to_string(reqs) +
                                " requests past 100k context without compaction.";
                    f.evidenceJson = {{"session_id", sId}, {"peak_context", peakCtx}, {"requests", reqs}};
                    f.suggestedExperiment = "Trigger /compact after completing each self-contained milestone.";
                    f.experimentMetric = "compaction_rate";
                    insertFinding(f);
                }

                if (durSec >= longSec && longSec > 0) {
                    double hrs = static_cast<double>(durSec) / 3600.0;
                    InsightFinding f;
                    f.detector = "long_running_sessions";
                    f.platform = plat;
                    f.scope = "session";
                    f.scopeId = sId;
                    f.severity = (hrs >= 16.0) ? "warn" : "info";
                    f.confidence = "high";
                    f.n = reqs;
                    f.metric = hrs;
                    f.summary = "Session " + sId.substr(0, 8) + " (" + proj + ") spanned " +
                                std::to_string(static_cast<int>(hrs)) + "h across " + std::to_string(reqs) + " requests.";
                    f.evidenceJson = {{"session_id", sId}, {"project", proj}, {"duration_hours", hrs}, {"requests", reqs}};
                    f.suggestedExperiment = "Start a fresh session per task envelope instead of keeping multi-hour sessions open.";
                    f.experimentMetric = "session_duration_hours";
                    insertFinding(f);

                    if (compCnt == 0 && peakCtx >= _config.insights.compactContextThresholdTokens) {
                        InsightFinding sf = f;
                        sf.detector = "session_sprawl";
                        sf.summary = "Session " + sId.substr(0, 8) + " (" + proj + ") ran " +
                                     std::to_string(static_cast<int>(hrs)) + "h reaching " +
                                     std::to_string(peakCtx / 1000) + "k context with zero compactions.";
                        insertFinding(sf);
                    }
                }
            }
            sqlite3_finalize(st);
        }
    }

    // 4. error_retry_loops & retry_after_error_loops
    {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT platform, session_id, tool_name, COUNT(*) "
            "FROM ins_tool_calls WHERE ts >= ? AND status != 'OK' "
            "GROUP BY platform, session_id, tool_name HAVING COUNT(*) >= ?;";
        if (sqlite3_prepare_v2(_db, sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, sinceTs);
            sqlite3_bind_int(st, 2, _config.insights.errorRetryStreakThreshold);
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string plat = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                std::string sId = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                std::string tName = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                int64_t errCnt = sqlite3_column_int64(st, 3);
                InsightFinding f;
                f.detector = "error_retry_loops";
                f.platform = plat;
                f.scope = "session";
                f.scopeId = sId;
                f.severity = "warn";
                f.confidence = "high";
                f.n = errCnt;
                f.metric = static_cast<double>(errCnt);
                f.summary = "Tool " + tName + " recorded " + std::to_string(errCnt) + " errors in session " + sId.substr(0, 8) + ".";
                f.evidenceJson = {{"session_id", sId}, {"tool_name", tName}, {"error_count", errCnt}};
                f.suggestedExperiment = "After 2 consecutive tool failures, inspect environment state before re-invoking.";
                f.experimentMetric = "tool_error_rate";
                insertFinding(f);
            }
            sqlite3_finalize(st);
        }
    }

    // 5. short_prompt_rate & first_prompt_specificity & drip_vs_batch & time_and_fatigue
    {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT platform, COUNT(*), "
            "SUM(CASE WHEN mixed_paste=0 AND prose_chars < ? THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN mixed_paste=0 AND prose_chars >= ? THEN 1 ELSE 0 END), "
            "SUM(mixed_paste), "
            "AVG(CASE WHEN mixed_paste=0 AND prose_chars < ? THEN reprompt_within_120s ELSE NULL END), "
            "AVG(CASE WHEN mixed_paste=0 AND prose_chars >= ? THEN reprompt_within_120s ELSE NULL END) "
            "FROM ins_prompts WHERE ts >= ? GROUP BY platform HAVING COUNT(*) >= ?;";
        if (sqlite3_prepare_v2(_db, sql, -1, &st, nullptr) == SQLITE_OK) {
            int shortMax = _config.insights.shortPromptMaxProseChars;
            sqlite3_bind_int(st, 1, shortMax);
            sqlite3_bind_int(st, 2, shortMax);
            sqlite3_bind_int(st, 3, shortMax);
            sqlite3_bind_int(st, 4, shortMax);
            sqlite3_bind_int64(st, 5, sinceTs);
            sqlite3_bind_int(st, 6, minN);
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string plat = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                int64_t totalP = sqlite3_column_int64(st, 1);
                int64_t shortP = sqlite3_column_int64(st, 2);
                int64_t longP = sqlite3_column_int64(st, 3);
                int64_t pasteP = sqlite3_column_int64(st, 4);
                double repShort = sqlite3_column_double(st, 5);
                double repLong = sqlite3_column_double(st, 6);

                int64_t nonPaste = std::max<int64_t>(1, totalP - pasteP);
                double shortRatio = static_cast<double>(shortP) / static_cast<double>(nonPaste);
                if (shortRatio >= 0.20 || pasteP >= minN) {
                    InsightFinding f;
                    f.detector = "short_prompt_rate";
                    f.platform = plat;
                    f.scope = "platform";
                    f.scopeId = plat;
                    f.severity = "info";
                    f.confidence = (totalP >= 15) ? "high" : "medium";
                    f.n = totalP;
                    f.metric = shortRatio;
                    f.summary = std::to_string(static_cast<int>(shortRatio * 100)) + "% of non-paste " + plat +
                                " prompts had under " + std::to_string(shortMax) +
                                " prose chars; observed correlation shows " +
                                std::to_string(static_cast<int>(repShort * 100)) + "% quick re-prompt rate (n=" +
                                std::to_string(shortP) + " short) vs " +
                                std::to_string(static_cast<int>(repLong * 100)) + "% (n=" +
                                std::to_string(longP) + " fuller; " +
                                std::to_string(pasteP) + "/" + std::to_string(totalP) + " mixed-paste skipped).";
                    f.evidenceJson = {
                        {"total_prompts", totalP},
                        {"short_prompts", shortP},
                        {"longer_prompts", longP},
                        {"mixed_paste_skipped", pasteP},
                        {"short_ratio", shortRatio},
                        {"reprompt_rate_short", repShort},
                        {"reprompt_rate_longer", repLong}
                    };
                    f.suggestedExperiment = "For the next 10 sessions, include target file names and constraints in prompts (>120 prose chars); compare quick re-prompt rate.";
                    f.experimentMetric = "short_prompt_rate";
                    insertFinding(f);
                }
            }
            sqlite3_finalize(st);
        }

        // first_prompt_specificity
        sqlite3_stmt* fpSt = nullptr;
        const char* fpSql =
            "SELECT p.platform, COUNT(*), "
            "AVG(CASE WHEN p.prose_chars < ? THEN s.turn_count ELSE NULL END), "
            "AVG(CASE WHEN p.prose_chars >= ? THEN s.turn_count ELSE NULL END) "
            "FROM ins_prompts p JOIN ins_sessions s ON p.platform = s.platform AND p.session_id = s.session_id "
            "WHERE p.ts >= ? AND p.is_first_in_session = 1 AND p.mixed_paste = 0 "
            "GROUP BY p.platform HAVING COUNT(*) >= ?;";
        if (sqlite3_prepare_v2(_db, fpSql, -1, &fpSt, nullptr) == SQLITE_OK) {
            int shortMax = _config.insights.shortPromptMaxProseChars;
            sqlite3_bind_int(fpSt, 1, shortMax);
            sqlite3_bind_int(fpSt, 2, shortMax);
            sqlite3_bind_int64(fpSt, 3, sinceTs);
            sqlite3_bind_int(fpSt, 4, minN);
            while (sqlite3_step(fpSt) == SQLITE_ROW) {
                std::string plat = reinterpret_cast<const char*>(sqlite3_column_text(fpSt, 0));
                int64_t nSess = sqlite3_column_int64(fpSt, 1);
                double turnsShort = sqlite3_column_double(fpSt, 2);
                double turnsLong = sqlite3_column_double(fpSt, 3);
                if (turnsShort > 0.0 || turnsLong > 0.0) {
                    InsightFinding f;
                    f.detector = "first_prompt_specificity";
                    f.platform = plat;
                    f.scope = "platform";
                    f.scopeId = plat;
                    f.severity = "info";
                    f.confidence = (nSess >= 10) ? "high" : "medium";
                    f.n = nSess;
                    f.metric = turnsShort;
                    f.summary = "Sessions on " + plat + " opening with <" + std::to_string(shortMax) +
                                " prose chars averaged " + std::to_string(static_cast<int>(turnsShort)) +
                                " turns vs " + std::to_string(static_cast<int>(turnsLong)) + " for fuller openings (n=" +
                                std::to_string(nSess) + ").";
                    f.evidenceJson = {{"sessions", nSess}, {"avg_turns_short_opening", turnsShort}, {"avg_turns_detailed_opening", turnsLong}};
                    f.suggestedExperiment = "Open new sessions with a structured prompt (>300 prose chars) naming target files and acceptance criteria.";
                    f.experimentMetric = "turns_per_session";
                    insertFinding(f);
                }
            }
            sqlite3_finalize(fpSt);
        }

        // drip_vs_batch
        sqlite3_stmt* drSt = nullptr;
        const char* drSql =
            "SELECT platform, strftime('%Y-%m-%dT%H:00', datetime(ts, 'unixepoch')) AS hr, COUNT(*) AS cnt "
            "FROM ins_prompts WHERE ts >= ? AND mixed_paste = 0 AND prose_chars < ? "
            "GROUP BY platform, hr HAVING cnt >= ? ORDER BY cnt DESC LIMIT 5;";
        if (sqlite3_prepare_v2(_db, drSql, -1, &drSt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(drSt, 1, sinceTs);
            sqlite3_bind_int(drSt, 2, _config.insights.dripPromptMaxProseChars);
            sqlite3_bind_int(drSt, 3, _config.insights.dripPromptsPerHour);
            while (sqlite3_step(drSt) == SQLITE_ROW) {
                std::string plat = reinterpret_cast<const char*>(sqlite3_column_text(drSt, 0));
                std::string hr = reinterpret_cast<const char*>(sqlite3_column_text(drSt, 1));
                int64_t cnt = sqlite3_column_int64(drSt, 2);
                InsightFinding f;
                f.detector = "drip_vs_batch";
                f.platform = plat;
                f.scope = "hour";
                f.scopeId = hr;
                f.severity = "info";
                f.confidence = "high";
                f.n = cnt;
                f.metric = static_cast<double>(cnt);
                f.summary = "Hour " + hr + " on " + plat + " had " + std::to_string(cnt) +
                            " short prompts (<" + std::to_string(_config.insights.dripPromptMaxProseChars) + " prose chars).";
                f.evidenceJson = {{"hour", hr}, {"short_prompts_in_hour", cnt}};
                f.suggestedExperiment = "Batch related micro-requests into a single checklist prompt.";
                f.experimentMetric = "short_prompts_per_hour";
                insertFinding(f);
            }
            sqlite3_finalize(drSt);
        }

        // time_and_fatigue
        sqlite3_stmt* tfSt = nullptr;
        const char* tfSql =
            "SELECT platform, COUNT(*), "
            "AVG(CASE WHEN turn_index < 15 THEN reprompt_within_120s ELSE NULL END), "
            "AVG(CASE WHEN turn_index >= 15 THEN reprompt_within_120s ELSE NULL END) "
            "FROM ins_prompts WHERE ts >= ? GROUP BY platform HAVING COUNT(*) >= ?;";
        if (sqlite3_prepare_v2(_db, tfSql, -1, &tfSt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(tfSt, 1, sinceTs);
            sqlite3_bind_int(tfSt, 2, minN);
            while (sqlite3_step(tfSt) == SQLITE_ROW) {
                std::string plat = reinterpret_cast<const char*>(sqlite3_column_text(tfSt, 0));
                int64_t cnt = sqlite3_column_int64(tfSt, 1);
                double earlyRep = sqlite3_column_double(tfSt, 2);
                double lateRep = sqlite3_column_double(tfSt, 3);
                if (lateRep > earlyRep && cnt >= minN) {
                    InsightFinding f;
                    f.detector = "time_and_fatigue";
                    f.platform = plat;
                    f.scope = "platform";
                    f.scopeId = plat;
                    f.severity = "info";
                    f.confidence = (cnt >= 20) ? "high" : "medium";
                    f.n = cnt;
                    f.metric = lateRep;
                    f.summary = "Quick re-prompt rate on " + plat + " is " +
                                std::to_string(static_cast<int>(lateRep * 100)) + "% after turn 15 vs " +
                                std::to_string(static_cast<int>(earlyRep * 100)) + "% in turns 1..14 (n=" + std::to_string(cnt) + ").";
                    f.evidenceJson = {{"early_reprompt_rate", earlyRep}, {"late_reprompt_rate", lateRep}, {"n", cnt}};
                    f.suggestedExperiment = "Reset or compact sessions after 15 turns to keep context sharp.";
                    f.experimentMetric = "late_turn_reprompt_rate";
                    insertFinding(f);
                }
            }
            sqlite3_finalize(tfSt);
        }
    }

    // 6. spend_category_mix, cost_by_project, model_mix, model_choice_fit, idle_gap_cache_expiry,
    //    context_reset_habit, time_of_day, cross_platform_overlap, quota_pressure, burn_rate_forecast
    {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "SELECT category, COUNT(*), SUM(spend_cents) FROM ins_spend_daily "
            "WHERE platform='cursor' GROUP BY category HAVING SUM(spend_cents) > 0;";
        if (sqlite3_prepare_v2(_db, sql, -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string cat = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                int64_t daysCnt = sqlite3_column_int64(st, 1);
                double spendCents = sqlite3_column_double(st, 2);
                InsightFinding f;
                f.detector = "spend_category_mix";
                f.platform = "cursor";
                f.scope = "category";
                f.scopeId = cat;
                f.severity = "info";
                f.confidence = (daysCnt >= 3) ? "high" : "low";
                f.n = daysCnt;
                f.metric = spendCents / 100.0;
                f.summary = "Cursor category '" + cat + "' accrued $" +
                            std::to_string(spendCents / 100.0).substr(0, 5) + " across " + std::to_string(daysCnt) + " active day(s).";
                f.evidenceJson = {{"category", cat}, {"spend_usd", spendCents / 100.0}, {"days", daysCnt}};
                f.suggestedExperiment = "Review Cursor category breakdown weekly against included fast-request quota.";
                f.experimentMetric = "daily_spend_usd";
                insertFinding(f);
            }
            sqlite3_finalize(st);
        }

        // cost_by_project (top projects per platform)
        sqlite3_stmt* cpSt = nullptr;
        const char* cpSql =
            "SELECT platform, project, COUNT(*), SUM(est_cost_nano), SUM(input_tokens + output_tokens + cache_read) "
            "FROM ins_requests WHERE ts >= ? AND project != '' "
            "GROUP BY platform, project ORDER BY SUM(est_cost_nano) DESC, COUNT(*) DESC LIMIT 5;";
        if (sqlite3_prepare_v2(_db, cpSql, -1, &cpSt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(cpSt, 1, sinceTs);
            while (sqlite3_step(cpSt) == SQLITE_ROW) {
                std::string plat = reinterpret_cast<const char*>(sqlite3_column_text(cpSt, 0));
                std::string proj = reinterpret_cast<const char*>(sqlite3_column_text(cpSt, 1));
                int64_t reqs = sqlite3_column_int64(cpSt, 2);
                int64_t costNano = sqlite3_column_int64(cpSt, 3);
                int64_t totTok = sqlite3_column_int64(cpSt, 4);
                double usd = static_cast<double>(costNano) / 1e9;
                InsightFinding f;
                f.detector = "cost_by_project";
                f.platform = plat;
                f.scope = "project";
                f.scopeId = proj;
                f.severity = "info";
                f.confidence = "high";
                f.n = reqs;
                f.metric = (usd > 0.0) ? usd : static_cast<double>(totTok);
                f.summary = "Project '" + proj + "' on " + plat + " used " + std::to_string(reqs) +
                            " requests (" + std::to_string(totTok / 1000) + "k tokens, est. $" +
                            std::to_string(usd).substr(0, 6) + ").";
                f.evidenceJson = {{"project", proj}, {"requests", reqs}, {"tokens", totTok}, {"est_cost_usd", usd}};
                f.suggestedExperiment = "Audit high-volume project sessions for long-context or subagent concentration.";
                f.experimentMetric = "project_est_cost_usd";
                insertFinding(f);
            }
            sqlite3_finalize(cpSt);
        }

        // model_mix & model_choice_fit
        sqlite3_stmt* mmSt = nullptr;
        const char* mmSql =
            "SELECT platform, model, COUNT(*), SUM(CASE WHEN output_tokens > 0 AND output_tokens < 200 THEN 1 ELSE 0 END), "
            "SUM(est_cost_nano) FROM ins_requests WHERE ts >= ? GROUP BY platform, model HAVING COUNT(*) >= ?;";
        if (sqlite3_prepare_v2(_db, mmSql, -1, &mmSt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(mmSt, 1, sinceTs);
            sqlite3_bind_int(mmSt, 2, minN);
            while (sqlite3_step(mmSt) == SQLITE_ROW) {
                std::string plat = reinterpret_cast<const char*>(sqlite3_column_text(mmSt, 0));
                std::string mod = reinterpret_cast<const char*>(sqlite3_column_text(mmSt, 1));
                int64_t reqs = sqlite3_column_int64(mmSt, 2);
                int64_t smallOut = sqlite3_column_int64(mmSt, 3);
                int64_t costNano = sqlite3_column_int64(mmSt, 4);
                InsightFinding f;
                f.detector = "model_mix";
                f.platform = plat;
                f.scope = "model";
                f.scopeId = mod;
                f.severity = "info";
                f.confidence = "high";
                f.n = reqs;
                f.metric = static_cast<double>(reqs);
                f.summary = "Model " + mod + " (" + plat + ") handled " + std::to_string(reqs) +
                            " requests (" + std::to_string(smallOut) + " with <200 output tokens).";
                f.evidenceJson = {{"model", mod}, {"requests", reqs}, {"small_output_requests", smallOut}, {"est_cost_usd", static_cast<double>(costNano) / 1e9}};
                f.suggestedExperiment = "Route simple lookups and formatting turns to Sonnet/Flash tiers.";
                f.experimentMetric = "small_output_opus_share";
                insertFinding(f);

                if (mod.find("opus") != std::string::npos && smallOut >= minN) {
                    InsightFinding mf = f;
                    mf.detector = "model_choice_fit";
                    mf.metric = static_cast<double>(smallOut) / static_cast<double>(std::max<int64_t>(1, reqs));
                    mf.summary = mod + " handled " + std::to_string(smallOut) + "/" + std::to_string(reqs) +
                                 " turns with under 200 output tokens.";
                    insertFinding(mf);
                }
            }
            sqlite3_finalize(mmSt);
        }

        // idle_gap_cache_expiry & context_reset_habit
        sqlite3_stmt* igSt = nullptr;
        const char* igSql =
            "SELECT session_id, ts, cache_write_5m + cache_write_1h, compacted, context_tokens "
            "FROM ins_requests WHERE platform='claude' AND ts >= ? AND is_sidechain=0 ORDER BY session_id ASC, ts ASC;";
        if (sqlite3_prepare_v2(_db, igSql, -1, &igSt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(igSt, 1, sinceTs);
            std::string prevS;
            int64_t prevTs = 0;
            int64_t idleEvents = 0;
            int64_t idleRewriteTokens = 0;
            int64_t compactEvents = 0;
            while (sqlite3_step(igSt) == SQLITE_ROW) {
                std::string sId = reinterpret_cast<const char*>(sqlite3_column_text(igSt, 0));
                int64_t ts = sqlite3_column_int64(igSt, 1);
                int64_t cw = sqlite3_column_int64(igSt, 2);
                int comp = sqlite3_column_int(igSt, 3);
                if (sId == prevS && prevTs > 0 && (ts - prevTs) >= _config.insights.idleGapSeconds && cw >= 5000) {
                    ++idleEvents;
                    idleRewriteTokens += cw;
                }
                if (comp == 1) ++compactEvents;
                prevS = sId;
                prevTs = ts;
            }
            sqlite3_finalize(igSt);

            if (idleEvents > 0) {
                InsightFinding f;
                f.detector = "idle_gap_cache_expiry";
                f.platform = "claude";
                f.scope = "platform";
                f.scopeId = "claude";
                f.severity = "info";
                f.confidence = (idleEvents >= 5) ? "high" : "medium";
                f.n = idleEvents;
                f.metric = static_cast<double>(idleRewriteTokens);
                f.summary = std::to_string(idleEvents) + " idle gap(s) >" + std::to_string(_config.insights.idleGapSeconds) +
                            "s triggered " + std::to_string(idleRewriteTokens / 1000) + "k cache-write tokens.";
                f.evidenceJson = {{"idle_gap_events", idleEvents}, {"cache_write_tokens", idleRewriteTokens}};
                f.suggestedExperiment = "Complete active turns before stepping away >5 minutes or /compact before resuming after a break.";
                f.experimentMetric = "idle_cache_write_tokens";
                insertFinding(f);
            }

            if (compactEvents > 0) {
                InsightFinding f;
                f.detector = "context_reset_habit";
                f.platform = "claude";
                f.scope = "platform";
                f.scopeId = "claude";
                f.severity = "info";
                f.confidence = "high";
                f.n = compactEvents;
                f.metric = static_cast<double>(compactEvents);
                f.summary = "Detected " + std::to_string(compactEvents) + " structural context compaction/reset event(s) (>=30% and >=20k drop).";
                f.evidenceJson = {{"compaction_events", compactEvents}};
                f.suggestedExperiment = "Maintain the habit of compacting when switching sub-tasks.";
                f.experimentMetric = "compaction_events";
                insertFinding(f);
            }
        }

        // time_of_day
        sqlite3_stmt* todSt = nullptr;
        const char* todSql =
            "SELECT platform, strftime('%H', datetime(ts, 'unixepoch')) AS hr, COUNT(*) AS reqs "
            "FROM ins_requests WHERE ts >= ? GROUP BY platform, hr ORDER BY reqs DESC LIMIT 3;";
        if (sqlite3_prepare_v2(_db, todSql, -1, &todSt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(todSt, 1, sinceTs);
            while (sqlite3_step(todSt) == SQLITE_ROW) {
                std::string plat = reinterpret_cast<const char*>(sqlite3_column_text(todSt, 0));
                std::string hr = reinterpret_cast<const char*>(sqlite3_column_text(todSt, 1));
                int64_t reqs = sqlite3_column_int64(todSt, 2);
                InsightFinding f;
                f.detector = "time_of_day";
                f.platform = plat;
                f.scope = "hour_utc";
                f.scopeId = hr + ":00";
                f.severity = "info";
                f.confidence = "high";
                f.n = reqs;
                f.metric = static_cast<double>(reqs);
                f.summary = "Peak activity hour on " + plat + " is " + hr + ":00 UTC (" + std::to_string(reqs) + " requests).";
                f.evidenceJson = {{"hour_utc", hr}, {"requests", reqs}};
                f.suggestedExperiment = "Schedule heavy batch runs outside peak interactive quota windows.";
                f.experimentMetric = "peak_hour_requests";
                insertFinding(f);
            }
            sqlite3_finalize(todSt);
        }

        // cross_platform_overlap
        sqlite3_stmt* ovSt = nullptr;
        const char* ovSql =
            "SELECT strftime('%Y-%m-%dT%H:00', datetime(ts, 'unixepoch')) AS hr, COUNT(DISTINCT platform) AS plats, COUNT(*) AS reqs "
            "FROM ins_requests WHERE ts >= ? GROUP BY hr HAVING plats >= 2 ORDER BY reqs DESC LIMIT 3;";
        if (sqlite3_prepare_v2(_db, ovSql, -1, &ovSt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(ovSt, 1, sinceTs);
            while (sqlite3_step(ovSt) == SQLITE_ROW) {
                std::string hr = reinterpret_cast<const char*>(sqlite3_column_text(ovSt, 0));
                int64_t plats = sqlite3_column_int64(ovSt, 1);
                int64_t reqs = sqlite3_column_int64(ovSt, 2);
                InsightFinding f;
                f.detector = "cross_platform_overlap";
                f.platform = "all";
                f.scope = "hour";
                f.scopeId = hr;
                f.severity = "info";
                f.confidence = "high";
                f.n = reqs;
                f.metric = static_cast<double>(plats);
                f.summary = std::to_string(plats) + " AI platforms were used concurrently during " + hr + " (" + std::to_string(reqs) + " combined requests).";
                f.evidenceJson = {{"hour", hr}, {"platforms", plats}, {"requests", reqs}};
                f.suggestedExperiment = "Consolidate concurrent multi-IDE edits on the same repository to avoid context duplication.";
                f.experimentMetric = "overlapping_hours";
                insertFinding(f);
            }
            sqlite3_finalize(ovSt);
        }

        // quota_pressure
        sqlite3_stmt* qpSt = nullptr;
        const char* qpSql =
            "SELECT platform, metric_key, COUNT(*), MIN(value), MAX(limit_val) "
            "FROM ins_quota WHERE ts >= ? AND ((platform='antigravity' AND limit_val=1.0 AND value <= 0.25) "
            "OR (platform='cursor' AND limit_val > 0 AND (value / limit_val) >= 0.75)) "
            "GROUP BY platform, metric_key;";
        if (sqlite3_prepare_v2(_db, qpSql, -1, &qpSt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(qpSt, 1, sinceTs);
            while (sqlite3_step(qpSt) == SQLITE_ROW) {
                std::string plat = reinterpret_cast<const char*>(sqlite3_column_text(qpSt, 0));
                std::string mKey = reinterpret_cast<const char*>(sqlite3_column_text(qpSt, 1));
                int64_t samples = sqlite3_column_int64(qpSt, 2);
                double minVal = sqlite3_column_double(qpSt, 3);
                double maxLim = sqlite3_column_double(qpSt, 4);
                InsightFinding f;
                f.detector = "quota_pressure";
                f.platform = plat;
                f.scope = "quota_metric";
                f.scopeId = mKey;
                f.severity = (plat == "antigravity" && minVal <= 0.05) ? "warn" : "info";
                f.confidence = "high";
                f.n = samples;
                f.metric = minVal;
                f.summary = "Quota metric '" + mKey + "' on " + plat + " reached " +
                            std::to_string(minVal).substr(0, 4) + " across " + std::to_string(samples) + " low-headroom sample(s).";
                f.evidenceJson = {{"metric_key", mKey}, {"samples", samples}, {"min_value", minVal}, {"limit", maxLim}};
                f.suggestedExperiment = "Switch to secondary model pools when 5-hour or weekly quota buckets drop below 25%.";
                f.experimentMetric = "min_quota_fraction";
                insertFinding(f);
            }
            sqlite3_finalize(qpSt);
        }

        // burn_rate_forecast (per billing cycle, never multi-cycle / all-time sum)
        for (const auto& acct : _config.claude.accounts) {
            if (acct.spendLimitUsd <= 0.0) continue;
            std::string acctName = acct.name.empty() ? "default" : acct.name;
            int resetDay = (acct.cycleResetDay >= 1 && acct.cycleResetDay <= 28) ? acct.cycleResetDay : 1;

            int64_t maxTs = 0;
            {
                sqlite3_stmt* mxSt = nullptr;
                if (sqlite3_prepare_v2(_db,
                    "SELECT MAX(ts) FROM ins_requests WHERE platform='claude' AND account=?;",
                    -1, &mxSt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(mxSt, 1, acctName.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(mxSt) == SQLITE_ROW) {
                        maxTs = sqlite3_column_int64(mxSt, 0);
                    }
                    sqlite3_finalize(mxSt);
                }
            }
            int64_t evalTs = (maxTs > 0) ? std::max(nowSec, maxTs) : nowSec;
            ClaudeCycle cycle;
            if (!claudeCycleBounds(evalTs, resetDay, cycle)) {
                cycle.startEpoch = sinceTs;
                cycle.endEpoch = evalTs + 86400LL;
            }
            if (maxTs > 0 && maxTs < cycle.startEpoch) {
                claudeCycleBounds(maxTs, resetDay, cycle);
                evalTs = maxTs;
            }

            sqlite3_stmt* brSt = nullptr;
            const char* brSql =
                "SELECT COUNT(*), SUM(est_cost_nano) FROM ins_requests "
                "WHERE platform='claude' AND account=? AND ts >= ? AND ts < ?;";
            if (sqlite3_prepare_v2(_db, brSql, -1, &brSt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(brSt, 1, acctName.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(brSt, 2, cycle.startEpoch);
                sqlite3_bind_int64(brSt, 3, cycle.endEpoch);
                if (sqlite3_step(brSt) == SQLITE_ROW) {
                    int64_t reqs = sqlite3_column_int64(brSt, 0);
                    double usd = static_cast<double>(sqlite3_column_int64(brSt, 1)) / 1e9;
                    double frac = usd / acct.spendLimitUsd;
                    double elapsedDays = std::max(1.0, static_cast<double>(evalTs - cycle.startEpoch) / 86400.0);
                    double cycleDays = std::max(1.0, static_cast<double>(cycle.endEpoch - cycle.startEpoch) / 86400.0);
                    double projectedUsd = (evalTs > cycle.startEpoch && elapsedDays >= 1.0)
                                              ? (usd * (cycleDays / elapsedDays))
                                              : usd;
                    double projectedFrac = projectedUsd / acct.spendLimitUsd;
                    if (frac >= _config.insights.burnRateAlertFraction ||
                        projectedFrac >= _config.insights.burnRateAlertFraction) {
                        InsightFinding f;
                        f.detector = "burn_rate_forecast";
                        f.platform = "claude";
                        f.scope = "account";
                        f.scopeId = acctName;
                        f.severity = (frac >= 1.0) ? "warn" : "info";
                        f.confidence = "high";
                        f.n = reqs;
                        f.metric = frac;
                        f.summary = "Claude account '" + acctName + "' current-cycle est. spend ($" +
                                    std::to_string(usd).substr(0, 6) + ") is at " +
                                    std::to_string(static_cast<int>(frac * 100)) + "% of configured $" +
                                    std::to_string(static_cast<int>(acct.spendLimitUsd)) +
                                    " cycle limit (projected cycle-end: $" +
                                    std::to_string(projectedUsd).substr(0, 6) + ", " +
                                    std::to_string(static_cast<int>(projectedFrac * 100)) + "%).";
                        f.evidenceJson = {
                            {"account", acctName},
                            {"cycle_start_epoch", cycle.startEpoch},
                            {"cycle_end_epoch", cycle.endEpoch},
                            {"est_spend_usd", usd},
                            {"projected_spend_usd", projectedUsd},
                            {"spend_limit_usd", acct.spendLimitUsd},
                            {"fraction", frac},
                            {"projected_fraction", projectedFrac}
                        };
                        f.suggestedExperiment = "Monitor daily burn rate and compact long sessions to stay within cycle budget.";
                        f.experimentMetric = "cycle_spend_fraction";
                        insertFinding(f);
                    }
                }
                sqlite3_finalize(brSt);
            }
        }
    }

    sqlite3_exec(_db, "COMMIT;", nullptr, nullptr, nullptr);
    return true;
}

nlohmann::json InsightEngine::getInsights(const std::string& platform,
                                          int days,
                                          const std::string& detector) const {
    std::lock_guard<std::mutex> lock(_mutex);
    nlohmann::json res = {
        {"window_days", days},
        {"platform_filter", platform},
        {"detector_filter", detector},
        {"platforms_skipped", {
            {"compact_opportunity", {"cursor"}},
            {"idle_gap_cache_expiry", {"cursor", "antigravity"}},
            {"context_reset_habit", {"cursor", "antigravity"}},
            {"spend_category_mix", {"claude", "antigravity"}},
            {"short_prompt_rate", {"cursor"}},
            {"first_prompt_specificity", {"cursor"}},
            {"drip_vs_batch", {"cursor"}}
        }},
        {"findings", nlohmann::json::array()}
    };
    if (!_db) return res;

    int64_t sinceTs = static_cast<int64_t>(std::time(nullptr)) - static_cast<int64_t>(days) * 86400LL;
    std::string sql =
        "SELECT detector, platform, scope, scope_id, severity, confidence, n, metric, summary, "
        "evidence_json, suggested_experiment, experiment_start, experiment_metric, first_seen, status "
        "FROM ins_findings WHERE computed_ts >= ?";
    if (!platform.empty()) sql += " AND platform = ?";
    bool isSevFilter = (detector == "info" || detector == "warn" || detector == "crit");
    if (detector == "warn") {
        sql += " AND severity IN ('warn', 'crit')";
    } else if (detector == "crit") {
        sql += " AND severity = 'crit'";
    } else if (!detector.empty() && !isSevFilter) {
        sql += " AND detector = ?";
    }
    sql += " ORDER BY CASE severity WHEN 'crit' THEN 0 WHEN 'warn' THEN 1 ELSE 2 END, n DESC;";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(_db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        int bindIdx = 1;
        sqlite3_bind_int64(st, bindIdx++, sinceTs);
        if (!platform.empty()) sqlite3_bind_text(st, bindIdx++, platform.c_str(), -1, SQLITE_TRANSIENT);
        if (!detector.empty() && !isSevFilter) sqlite3_bind_text(st, bindIdx++, detector.c_str(), -1, SQLITE_TRANSIENT);

        while (sqlite3_step(st) == SQLITE_ROW) {
            InsightFinding f;
            f.detector = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            f.platform = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
            f.scope = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
            f.scopeId = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
            f.severity = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
            f.confidence = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
            f.n = sqlite3_column_int64(st, 6);
            f.metric = sqlite3_column_double(st, 7);
            f.summary = reinterpret_cast<const char*>(sqlite3_column_text(st, 8));
            const char* evC = reinterpret_cast<const char*>(sqlite3_column_text(st, 9));
            f.evidenceJson = evC ? nlohmann::json::parse(evC, nullptr, false) : nlohmann::json::object();
            if (f.evidenceJson.is_discarded()) f.evidenceJson = nlohmann::json::object();
            f.suggestedExperiment = reinterpret_cast<const char*>(sqlite3_column_text(st, 10));
            f.experimentStart = sqlite3_column_int64(st, 11);
            f.experimentMetric = reinterpret_cast<const char*>(sqlite3_column_text(st, 12));
            f.firstSeen = sqlite3_column_int64(st, 13);
            f.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 14));
            res["findings"].push_back(f.toJson());
        }
        sqlite3_finalize(st);
    }

    return res;
}

nlohmann::json InsightEngine::getUsageReport(const std::string& platform,
                                             int days,
                                             const std::string& groupBy) const {
    std::lock_guard<std::mutex> lock(_mutex);
    nlohmann::json res = {
        {"window_days", days},
        {"platform_filter", platform},
        {"group_by", groupBy},
        {"rows", nlohmann::json::array()}
    };
    if (!_db) return res;

    int64_t sinceTs = static_cast<int64_t>(std::time(nullptr)) - static_cast<int64_t>(days) * 86400LL;

    if (groupBy == "tool") {
        std::string sql =
            "SELECT platform, tool_name, COUNT(*) AS calls, "
            "SUM(CASE WHEN status != 'OK' THEN 1 ELSE 0 END) AS errors, AVG(duration_ms) "
            "FROM ins_tool_calls WHERE ts >= ?";
        if (!platform.empty()) sql += " AND platform = ?";
        sql += " GROUP BY platform, tool_name ORDER BY calls DESC;";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(_db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, sinceTs);
            if (!platform.empty()) sqlite3_bind_text(st, 2, platform.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                res["rows"].push_back({
                    {"platform", reinterpret_cast<const char*>(sqlite3_column_text(st, 0))},
                    {"key", reinterpret_cast<const char*>(sqlite3_column_text(st, 1))},
                    {"requests", sqlite3_column_int64(st, 2)},
                    {"errors", sqlite3_column_int64(st, 3)},
                    {"avg_duration_ms", sqlite3_column_double(st, 4)}
                });
            }
            sqlite3_finalize(st);
        }
        return res;
    }

    std::string groupExpr = "project";
    if (groupBy == "model") groupExpr = "model";
    else if (groupBy == "day") groupExpr = "strftime('%Y-%m-%d', datetime(ts, 'unixepoch'))";
    else if (groupBy == "hour_of_day") groupExpr = "strftime('%H', datetime(ts, 'unixepoch'))";
    else if (groupBy == "session") groupExpr = "session_id";

    std::string sql =
        "SELECT platform, " + groupExpr + " AS grp_key, COUNT(*) AS reqs, "
        "SUM(is_sidechain) AS side_reqs, SUM(input_tokens) AS in_tok, SUM(output_tokens) AS out_tok, "
        "SUM(cache_read) AS cr_tok, SUM(cache_write_5m + cache_write_1h) AS cw_tok, "
        "SUM(thinking_tokens) AS think_tok, SUM(est_cost_nano) AS cost_nano "
        "FROM ins_requests WHERE ts >= ?";
    if (!platform.empty()) sql += " AND platform = ?";
    sql += " GROUP BY platform, grp_key ORDER BY cost_nano DESC, reqs DESC;";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(_db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, sinceTs);
        if (!platform.empty()) sqlite3_bind_text(st, 2, platform.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* platC = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            const char* keyC = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
            int64_t reqs = sqlite3_column_int64(st, 2);
            int64_t sideReqs = sqlite3_column_int64(st, 3);
            int64_t inTok = sqlite3_column_int64(st, 4);
            int64_t outTok = sqlite3_column_int64(st, 5);
            int64_t crTok = sqlite3_column_int64(st, 6);
            int64_t cwTok = sqlite3_column_int64(st, 7);
            int64_t thinkTok = sqlite3_column_int64(st, 8);
            int64_t costNano = sqlite3_column_int64(st, 9);
            double denom = static_cast<double>(inTok + crTok + cwTok);
            double cacheRatio = (denom > 0.0) ? (static_cast<double>(crTok) / denom) : 0.0;

            res["rows"].push_back({
                {"platform", platC ? platC : ""},
                {"key", keyC ? keyC : ""},
                {"requests", reqs},
                {"sidechain_requests", sideReqs},
                {"input_tokens", inTok},
                {"output_tokens", outTok},
                {"cache_read_tokens", crTok},
                {"cache_write_tokens", cwTok},
                {"thinking_tokens", thinkTok},
                {"cache_hit_ratio", cacheRatio},
                {"est_cost_usd", static_cast<double>(costNano) / 1e9}
            });
        }
        sqlite3_finalize(st);
    }

    return res;
}

nlohmann::json InsightEngine::compareExperiment(const std::string& detector,
                                                const std::string& platform,
                                                int64_t splitEpoch) const {
    std::lock_guard<std::mutex> lock(_mutex);
    int64_t nowSec = static_cast<int64_t>(std::time(nullptr));
    if (splitEpoch <= 0) {
        splitEpoch = nowSec - 7 * 86400LL;
    }

    nlohmann::json out = {
        {"detector", detector},
        {"platform", platform.empty() ? "all" : platform},
        {"split_epoch", splitEpoch},
        {"before", {{"n", 0}, {"metric", 0.0}}},
        {"after", {{"n", 0}, {"metric", 0.0}}},
        {"delta_pct", 0.0},
        {"verdict", "insufficient_data"}
    };
    if (!_db) return out;

    auto evalWindow = [&](int64_t tStart, int64_t tEnd, int64_t& nOut, double& metricOut, bool& lowerIsBetter) {
        nOut = 0;
        metricOut = 0.0;
        lowerIsBetter = true;

        if (detector == "cache_efficiency" || detector == "cache_hit_collapse") {
            lowerIsBetter = false;
            std::string sql =
                "SELECT COUNT(*), SUM(cache_read), SUM(input_tokens + cache_read + cache_write_5m + cache_write_1h) "
                "FROM ins_requests WHERE ts >= ? AND ts < ?";
            if (!platform.empty()) sql += " AND platform = ?";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(_db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, tStart);
                sqlite3_bind_int64(st, 2, tEnd);
                if (!platform.empty()) sqlite3_bind_text(st, 3, platform.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(st) == SQLITE_ROW) {
                    nOut = sqlite3_column_int64(st, 0);
                    double cr = sqlite3_column_double(st, 1);
                    double denom = sqlite3_column_double(st, 2);
                    metricOut = (denom > 0.0) ? (cr / denom) : 0.0;
                }
                sqlite3_finalize(st);
            }
        } else if (detector == "subagent_share" || detector == "subagent_fanout_overhead") {
            lowerIsBetter = true;
            std::string sql =
                "SELECT COUNT(*), SUM(is_sidechain) FROM ins_requests WHERE ts >= ? AND ts < ?";
            if (!platform.empty()) sql += " AND platform = ?";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(_db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, tStart);
                sqlite3_bind_int64(st, 2, tEnd);
                if (!platform.empty()) sqlite3_bind_text(st, 3, platform.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(st) == SQLITE_ROW) {
                    nOut = sqlite3_column_int64(st, 0);
                    double side = sqlite3_column_double(st, 1);
                    metricOut = (nOut > 0) ? (side / static_cast<double>(nOut)) : 0.0;
                }
                sqlite3_finalize(st);
            }
        } else {
            lowerIsBetter = true;
            std::string sql =
                "SELECT COUNT(*), AVG(CASE WHEN prose_chars < ? THEN 1.0 ELSE 0.0 END) "
                "FROM ins_prompts WHERE ts >= ? AND ts < ?";
            if (!platform.empty()) sql += " AND platform = ?";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(_db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(st, 1, _config.insights.shortPromptMaxProseChars);
                sqlite3_bind_int64(st, 2, tStart);
                sqlite3_bind_int64(st, 3, tEnd);
                if (!platform.empty()) sqlite3_bind_text(st, 4, platform.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(st) == SQLITE_ROW) {
                    nOut = sqlite3_column_int64(st, 0);
                    metricOut = sqlite3_column_double(st, 1);
                }
                sqlite3_finalize(st);
            }
        }
    };

    int64_t bN = 0, aN = 0;
    double bM = 0.0, aM = 0.0;
    bool lowerBetter = true;
    evalWindow(0, splitEpoch, bN, bM, lowerBetter);
    evalWindow(splitEpoch, nowSec + 86400, aN, aM, lowerBetter);

    out["before"] = {{"n", bN}, {"metric", bM}};
    out["after"] = {{"n", aN}, {"metric", aM}};

    double deltaPct = (bM != 0.0) ? ((aM - bM) / std::abs(bM)) * 100.0 : 0.0;
    out["delta_pct"] = deltaPct;

    int minN = std::max(1, _config.insights.coachingMinSampleSize);
    if (bN < minN || aN < minN) {
        out["verdict"] = "insufficient_data";
    } else if (std::abs(deltaPct) < 5.0) {
        out["verdict"] = "unchanged";
    } else {
        bool improved = lowerBetter ? (aM < bM) : (aM > bM);
        out["verdict"] = improved ? "improved" : "regressed";
    }

    return out;
}

nlohmann::json InsightEngine::describeDataset() {
    return {
        {"database", "~/.config/aimon/insights.db"},
        {"privacy_guarantee", "Strictly numeric and categorical metadata only; zero prompt text, assistant text, thinking text, or raw file paths."},
        {"units", {
            {"timestamps", "All ts, start_ts, end_ts, computed_ts, experiment_start, and first_seen columns are Unix epoch seconds."},
            {"durations", "ins_sessions.duration_sec and active_sec are seconds; ins_tool_calls.duration_ms and ins_generations.ttft_ms / duration_ms are milliseconds."},
            {"costs", "est_cost_nano / turn_cost_nano are nano-dollars (1e-9 USD, list-price estimate for claude; 0 for antigravity/cursor). ins_spend_daily.spend_cents is vendor-reported USD cents."}
        }},
        {"token_identities", {
            {"ins_generations", "output_tokens = thinking_tokens + response_tokens; context_tokens = input_tokens + cache_read_tokens + output_tokens"},
            {"ins_requests", "For antigravity, thinking_tokens is a subset of output_tokens and cache_write_5m/1h are 0. For claude, thinking_tokens is 0 and cache writes are split into cache_write_5m and cache_write_1h."},
            {"ins_sessions", "cache_write = cache_write_5m + cache_write_1h; peak_context is maximum single-request context window tokens, whereas input_tokens is cumulative session total."}
        }},
        {"platform_capabilities", {
            {"claude", "Full coverage in ins_requests (with est_cost_nano > 0, cost_basis='list_price'), ins_sessions, ins_daily, ins_tool_calls, ins_prompts, ins_quota, and ins_findings."},
            {"antigravity", "Full token/cache/thinking coverage in ins_generations, ins_requests, ins_sessions, ins_daily, ins_tool_calls, ins_prompts, ins_quota, and ins_findings (est_cost_nano=0, cost_basis='none')."},
            {"cursor", "Vendor daily category spend in ins_spend_daily; hook sessions/events in ins_sessions and ins_tool_calls (cumulative input_tokens, cache_read=0); checkpoint counts in ins_requests (token counts=0); ins_prompts unpopulated unless hook prompt-feature capture is enabled."}
        }},
        {"antigravity_model_enums", {
            {"342", "gemini-2.5-flash"},
            {"1016", "gemini-3.1-pro-low"},
            {"1018", "gemini-3.1-pro-high"},
            {"1026", "claude-sonnet-4-6"},
            {"1035", "claude-opus-4-5"},
            {"1298", "gemini-3.8-pro-high"},
            {"1299", "gemini-3.8-pro-low"},
            {"1318", "gemini-3.8-flash"},
            {"1319", "gemini-3.8-flash-low"},
            {"1404", "claude-opus-4-6-thinking"},
            {"1418", "antigravity-cli-default"}
        }},
        {"tables", {
            {
                {"name", "ins_requests"},
                {"description", "Normalized per-request token, cache, context_tokens, compacted, speed, sidechain, and nano-dollar cost records across platforms."},
                {"primary_key", "(platform, account, session_id, request_id, message_id)"},
                {"join_keys", "Joins to ins_sessions on (platform, session_id); for antigravity, request_id = ins_generations.conversation_id || ':' || ins_generations.idx."},
                {"columns", {"id", "platform", "account", "session_id", "project", "request_id", "message_id", "ts", "model", "speed", "is_sidechain", "input_tokens", "output_tokens", "cache_read", "cache_write_5m", "cache_write_1h", "context_tokens", "compacted", "thinking_tokens", "web_search_reqs", "est_cost_nano"}}
            },
            {
                {"name", "ins_sessions"},
                {"description", "Per-session rollups of duration, active time, turns, tool calls, errors, edits, peak_context, compaction_count, and cost_basis."},
                {"primary_key", "(platform, session_id)"},
                {"join_keys", "For antigravity, hook rows have session_link='exact' and session_id='sess-<8hex>', while generation rollups have session_link='prefix' and conversation_id=<full_uuid> (join to ins_generations on conversation_id)."},
                {"columns", {"platform", "session_id", "conversation_id", "session_link", "project", "model_primary", "start_ts", "end_ts", "duration_sec", "active_sec", "turn_count", "request_count", "sidechain_reqs", "tool_calls", "tool_errors", "edit_calls", "bash_calls", "subagent_spawns", "input_tokens", "output_tokens", "cache_read", "cache_write", "peak_context", "compaction_count", "est_cost_nano", "cost_basis", "status"}}
            },
            {
                {"name", "ins_daily"},
                {"description", "Daily rollups by platform, day (YYYY-MM-DD UTC), and model."},
                {"primary_key", "(platform, day, model)"},
                {"columns", {"platform", "day", "model", "tokens", "est_cost_nano", "sessions", "requests"}}
            },
            {
                {"name", "ins_tool_calls"},
                {"description", "Tool call records with SHA-256 target file hashes (16-hex prefix), duration_ms, and status ('OK' or 'ERROR')."},
                {"primary_key", "id (UNIQUE on platform, session_id, step_index, tool_name, ts)"},
                {"join_keys", "Joins to ins_sessions on (platform, session_id)."},
                {"columns", {"id", "platform", "session_id", "ts", "step_index", "tool_name", "target_hash", "duration_ms", "status"}}
            },
            {
                {"name", "ins_quota"},
                {"description", "Quota fraction and credit balance snapshots across antigravity, cursor, and claude. Polarity: antigravity model/bucket:* keys are remaining fraction [0..1] (negative delta = consumption), credit:* are remaining credits; claude est_cost_usd / total_tokens and cursor fast_requests_used are cumulative usage (positive delta = consumption)."},
                {"primary_key", "(ts, platform, metric_key)"},
                {"columns", {"ts", "platform", "metric_key", "value", "limit_val", "delta"}}
            },
            {
                {"name", "ins_spend_daily"},
                {"description", "Vendor-reported daily spend in USD cents by platform and category (e.g., Cursor GetDailySpendByCategory)."},
                {"primary_key", "(day, platform, category)"},
                {"columns", {"day", "platform", "category", "spend_cents", "requests", "cycle_id"}}
            },
            {
                {"name", "ins_generations"},
                {"description", "Decoded Antigravity gen_metadata protobuf records with input/output/thinking/response/cache tokens and TTFT."},
                {"primary_key", "(conversation_id, idx)"},
                {"join_keys", "Joins to ins_sessions on conversation_id = ins_sessions.conversation_id; joins to ins_requests on (conversation_id || ':' || idx) = ins_requests.request_id."},
                {"columns", {"conversation_id", "session_id", "idx", "ts", "model_enum", "input_tokens", "output_tokens", "thinking_tokens", "response_tokens", "cache_read_tokens", "f1_f4_f6", "context_tokens", "context_limit", "max_output_tokens", "ttft_ms", "duration_ms", "validated"}}
            },
            {
                {"name", "ins_prompts"},
                {"description", "Numeric prompt structure features (prose_chars, fenced_chars, paste_chars, mixed_paste) paired with next-turn outcome proxies. Populated for claude and antigravity; filter is_sidechain=0 and mixed_paste=0 for human prose coaching analysis."},
                {"primary_key", "(platform, session_id, turn_index)"},
                {"join_keys", "Joins to ins_sessions on (platform, session_id)."},
                {"columns", {"platform", "session_id", "turn_index", "ts", "project", "chars", "words", "lines", "prose_chars", "prose_words", "fenced_chars", "fenced_block_count", "paste_chars", "mixed_paste", "file_path_count", "bullet_count", "is_sidechain", "has_code_block", "has_file_ref", "has_constraints", "is_first_in_session", "next_tool_calls", "next_tool_errors", "next_edits", "next_Read_before_Edit", "reprompt_within_120s", "interrupted_turn", "turn_tokens", "turn_cost_nano"}}
            },
            {
                {"name", "ins_findings"},
                {"description", "Programmatic and workflow-coaching findings with severity, confidence, sample size n, and suggested experiments."},
                {"primary_key", "id"},
                {"columns", {"id", "computed_ts", "window_days", "detector", "platform", "scope", "scope_id", "severity", "confidence", "n", "metric", "summary", "evidence_json", "suggested_experiment", "experiment_start", "experiment_metric", "first_seen", "status"}}
            }
        }},
        {"sample_queries", {
            {{"title", "Cache-hit ratio by project (Claude & Antigravity)"}, {"sql", "SELECT platform, project, SUM(cache_read)*1.0/NULLIF(SUM(input_tokens+cache_read+cache_write_5m+cache_write_1h), 0) AS hit_ratio FROM ins_requests WHERE platform IN ('claude','antigravity') GROUP BY platform, project;"}},
            {{"title", "Prompt prose length vs edit success & quick re-prompt rate"}, {"sql", "SELECT CASE WHEN prose_chars < 60 THEN 'short' ELSE 'detailed' END AS bucket, COUNT(*) AS n, AVG(next_edits) AS avg_edits, AVG(reprompt_within_120s) AS reprompt_rate FROM ins_prompts WHERE mixed_paste=0 AND is_sidechain=0 GROUP BY bucket;"}},
            {{"title", "Antigravity thinking token share by model"}, {"sql", "SELECT model_enum, COUNT(*) AS gens, SUM(thinking_tokens)*1.0/NULLIF(SUM(output_tokens), 0) AS thinking_share FROM ins_generations WHERE validated=1 GROUP BY model_enum;"}},
            {{"title", "Sessions with lowest cache reuse at >40k peak context"}, {"sql", "SELECT platform, session_id, project, peak_context, cache_read*1.0/NULLIF(input_tokens+cache_read+cache_write, 0) AS ratio FROM ins_sessions WHERE platform IN ('claude','antigravity') AND peak_context > 40000 ORDER BY ratio ASC LIMIT 20;"}},
            {{"title", "Top tool error retry hotspots by platform"}, {"sql", "SELECT platform, tool_name, COUNT(*) AS errors FROM ins_tool_calls WHERE status='ERROR' GROUP BY platform, tool_name ORDER BY errors DESC LIMIT 20;"}}
        }}
    };
}

nlohmann::json InsightEngine::exportDataset(const std::string& outputDir,
                                            const std::string& format,
                                            int days,
                                            bool anonymizeProjects) const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string outDir = outputDir.empty() ?
        PathUtils::expandHome("~/.local/share/aimon/exports") :
        PathUtils::expandHome(outputDir);
    PathUtils::ensureDirectoryExists(outDir);
    ::chmod(outDir.c_str(), 0700);

    nlohmann::json manifest = {
        {"output_dir", outDir},
        {"format", format},
        {"days", days},
        {"anonymize_projects", anonymizeProjects},
        {"files", nlohmann::json::array()}
    };
    if (!_db) return manifest;

    static constexpr const char* kExportTables[] = {
        "ins_requests", "ins_sessions", "ins_daily", "ins_tool_calls", "ins_quota",
        "ins_spend_daily", "ins_generations", "ins_prompts", "ins_findings"
    };

    if (format == "sqlite") {
        std::string dstPath = outDir + "/usage_dataset.sqlite";
        std::error_code ec;
        fs::remove(dstPath, ec);

        sqlite3* dstDb = nullptr;
        if (sqlite3_open(dstPath.c_str(), &dstDb) == SQLITE_OK) {
            sqlite3_backup* bkp = sqlite3_backup_init(dstDb, "main", _db, "main");
            if (bkp) {
                sqlite3_backup_step(bkp, -1);
                sqlite3_backup_finish(bkp);
            }
            sqlite3_exec(dstDb,
                "CREATE TABLE IF NOT EXISTS README (key TEXT PRIMARY KEY, value TEXT);"
                "INSERT OR REPLACE INTO README VALUES "
                "('schema_version', '1.1'), "
                "('privacy', 'Strictly numeric and categorical metadata only; zero content.'), "
                "('timestamps_unit', 'Unix epoch seconds across all ts/start_ts/end_ts/computed_ts columns.'), "
                "('durations_unit', 'ins_sessions.duration_sec is seconds; ins_tool_calls.duration_ms and ins_generations.ttft_ms/duration_ms are milliseconds.'), "
                "('claude_cost_basis', 'List-price nano-dollars (1e-9 USD), not vendor invoice.'), "
                "('cursor_spend_basis', 'Vendor-reported spend_cents in ins_spend_daily; ins_requests has checkpoint counts (tokens=0) and ins_sessions has cumulative hook input_tokens (cache_read=0).'), "
                "('token_identities', 'In ins_generations: output_tokens = thinking_tokens + response_tokens; context_tokens = input_tokens + cache_read_tokens + output_tokens.'), "
                "('antigravity_joins', 'Join ins_generations to ins_sessions via conversation_id; join ins_generations to ins_requests via (conversation_id || '':'' || idx) = request_id.'), "
                "('antigravity_model_enums', '342=gemini-2.5-flash, 1016=gemini-3.1-pro-low, 1018=gemini-3.1-pro-high, 1026=claude-sonnet-4-6, 1035=claude-opus-4-5, 1298=gemini-3.8-pro-high, 1299=gemini-3.8-pro-low, 1318=gemini-3.8-flash, 1319=gemini-3.8-flash-low, 1404=claude-opus-4-6-thinking, 1418=antigravity-cli-default');"
                "CREATE TABLE IF NOT EXISTS questions (id INTEGER PRIMARY KEY, question TEXT, sample_sql TEXT);"
                "DELETE FROM questions;"
                "INSERT INTO questions VALUES "
                "(1, 'Cache-hit ratio by project (Claude & Antigravity)', 'SELECT platform, project, SUM(cache_read)*1.0/NULLIF(SUM(input_tokens+cache_read+cache_write_5m+cache_write_1h), 0) AS hit_ratio FROM ins_requests WHERE platform IN (''claude'',''antigravity'') GROUP BY platform, project;'),"
                "(2, 'Prompt prose length vs edit success & quick re-prompt rate', 'SELECT CASE WHEN prose_chars < 60 THEN ''short'' ELSE ''detailed'' END AS bucket, COUNT(*) AS n, AVG(next_edits) AS avg_edits, AVG(reprompt_within_120s) AS reprompt_rate FROM ins_prompts WHERE mixed_paste=0 AND is_sidechain=0 GROUP BY bucket;'),"
                "(3, 'Antigravity thinking token share by model', 'SELECT model_enum, COUNT(*) AS gens, SUM(thinking_tokens)*1.0/NULLIF(SUM(output_tokens), 0) AS thinking_share FROM ins_generations WHERE validated=1 GROUP BY model_enum;'),"
                "(4, 'Sessions with lowest cache reuse at >40k peak context', 'SELECT platform, session_id, project, peak_context, cache_read*1.0/NULLIF(input_tokens+cache_read+cache_write, 0) AS ratio FROM ins_sessions WHERE platform IN (''claude'',''antigravity'') AND peak_context > 40000 ORDER BY ratio ASC LIMIT 20;'),"
                "(5, 'Top tool error retry hotspots by platform', 'SELECT platform, tool_name, COUNT(*) AS errors FROM ins_tool_calls WHERE status=''ERROR'' GROUP BY platform, tool_name ORDER BY errors DESC LIMIT 20;');",
                nullptr, nullptr, nullptr);
            if (anonymizeProjects) {
                sqlite3_exec(dstDb, "UPDATE ins_requests SET project = substr(hex(randomblob(4)),1,8) WHERE project != '';", nullptr, nullptr, nullptr);
                sqlite3_exec(dstDb, "UPDATE ins_sessions SET project = substr(hex(randomblob(4)),1,8) WHERE project != '';", nullptr, nullptr, nullptr);
                sqlite3_exec(dstDb, "UPDATE ins_prompts SET project = substr(hex(randomblob(4)),1,8) WHERE project != '';", nullptr, nullptr, nullptr);
            }
            sqlite3_close(dstDb);
            ::chmod(dstPath.c_str(), 0600);
            auto fSize = static_cast<int64_t>(fs::file_size(dstPath, ec));
            manifest["files"].push_back({{"table", "all"}, {"path", dstPath}, {"bytes", fSize}});
        }
        return manifest;
    }

    for (const char* tblC : kExportTables) {
        std::string tbl(tblC);
        std::string ext = (format == "csv") ? ".csv" : ".jsonl";
        std::string fPath = outDir + "/" + tbl + ext;
        std::ofstream out(fPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) continue;

        std::string sql = "SELECT * FROM " + tbl + ";";
        sqlite3_stmt* st = nullptr;
        int64_t rowCount = 0;
        if (sqlite3_prepare_v2(_db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            int colCount = sqlite3_column_count(st);
            if (format == "csv") {
                for (int c = 0; c < colCount; ++c) {
                    if (c > 0) out << ",";
                    out << sqlite3_column_name(st, c);
                }
                out << "\n";
            }

            while (sqlite3_step(st) == SQLITE_ROW) {
                ++rowCount;
                if (format == "csv") {
                    for (int c = 0; c < colCount; ++c) {
                        if (c > 0) out << ",";
                        std::string colName = sqlite3_column_name(st, c);
                        const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(st, c));
                        std::string val = txt ? txt : "";
                        if (anonymizeProjects && colName == "project" && !val.empty()) {
                            val = hashFilePath(val).substr(0, 8);
                        }
                        if (val.find_first_of(",\"\n\r") != std::string::npos) {
                            std::string esc = "\"";
                            for (char ch : val) {
                                if (ch == '"') esc += "\"\"";
                                else esc += ch;
                            }
                            esc += "\"";
                            out << esc;
                        } else {
                            out << val;
                        }
                    }
                    out << "\n";
                } else {
                    nlohmann::json rowObj = nlohmann::json::object();
                    for (int c = 0; c < colCount; ++c) {
                        std::string colName = sqlite3_column_name(st, c);
                        int colType = sqlite3_column_type(st, c);
                        if (colType == SQLITE_INTEGER) {
                            rowObj[colName] = sqlite3_column_int64(st, c);
                        } else if (colType == SQLITE_FLOAT) {
                            rowObj[colName] = sqlite3_column_double(st, c);
                        } else {
                            const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(st, c));
                            std::string val = txt ? txt : "";
                            if (anonymizeProjects && colName == "project" && !val.empty()) {
                                val = hashFilePath(val).substr(0, 8);
                            }
                            rowObj[colName] = val;
                        }
                    }
                    out << rowObj.dump() << "\n";
                }
            }
            sqlite3_finalize(st);
        }
        out.close();
        ::chmod(fPath.c_str(), 0600);
        std::error_code ec;
        auto fSize = static_cast<int64_t>(fs::file_size(fPath, ec));
        manifest["files"].push_back({{"table", tbl}, {"path", fPath}, {"rows", rowCount}, {"bytes", fSize}});
    }

    return manifest;
}

std::string InsightEngine::generateWeeklyDigest(int days) const {
    auto report = getUsageReport("", days, "project");
    auto findings = getInsights("", days, "");

    std::ostringstream ss;
    ss << "# AI Usage & Workflow Digest (Last " << days << " Days)\n\n";

    ss << "## 1. Spend & Token Breakdown by Project\n\n";
    ss << "| Platform | Project | Requests | Input Tok | Output Tok | Cache Hit % | Est. Cost (USD) |\n";
    ss << "| :--- | :--- | ---: | ---: | ---: | ---: | ---: |\n";
    if (report.contains("rows") && report["rows"].is_array()) {
        for (const auto& r : report["rows"]) {
            ss << "| " << r.value("platform", "")
               << " | " << r.value("key", "")
               << " | " << r.value("requests", int64_t(0))
               << " | " << r.value("input_tokens", int64_t(0))
               << " | " << r.value("output_tokens", int64_t(0))
               << " | " << std::fixed << std::setprecision(1) << (r.value("cache_hit_ratio", 0.0) * 100.0) << "%"
               << " | $" << std::fixed << std::setprecision(2) << r.value("est_cost_usd", 0.0) << " |\n";
        }
    }

    ss << "\n## 2. Active Workflow & Usage Findings\n\n";
    if (findings.contains("findings") && findings["findings"].is_array() && !findings["findings"].empty()) {
        for (const auto& f : findings["findings"]) {
            ss << "- **[" << f.value("severity", "info") << " / " << f.value("confidence", "medium")
               << " / n=" << f.value("n", int64_t(0)) << "] `" << f.value("detector", "") << "` ("
               << f.value("platform", "") << ")**: " << f.value("summary", "") << "\n";
            std::string exp = f.value("suggested_experiment", "");
            if (!exp.empty()) {
                ss << "  - *Suggested Experiment*: " << exp << "\n";
            }
        }
    } else {
        ss << "- No waste or workflow anomalies exceeded thresholds in this window.\n";
    }

    return ss.str();
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
