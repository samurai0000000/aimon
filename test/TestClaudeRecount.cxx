/*
 * TestClaudeRecount.cxx
 *
 * Ground-truth tool for the Claude usage plan. Walks a transcript tree with a
 * test-only loader, ingests the rows into a fresh ClaudeUsageStore, aggregates
 * by (day, model) with the catalog parsed from a saved pricing page, and
 * writes JSON lines that the independent oracle compares field by field.
 *
 *   test_claude_recount --root DIR (--page FILE | --versions FILE) --out FILE
 *
 * A versions file is JSON: [{"effective_from": EPOCH, "page": PATH}, ...].
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "ClaudeUsageStore.hxx"
#include "PriceCatalog.hxx"

using namespace aimon;
namespace fs = std::filesystem;

static bool parseTimestamp(const std::string& ts, int64_t& epoch) {
    static const std::regex re("^([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})(\\.[0-9]+)?Z$");
    std::smatch m;
    if (!std::regex_match(ts, m, re)) {
        return false;
    }
    struct tm tmv;
    std::memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = std::stoi(m[1]) - 1900;
    tmv.tm_mon = std::stoi(m[2]) - 1;
    tmv.tm_mday = std::stoi(m[3]);
    tmv.tm_hour = std::stoi(m[4]);
    tmv.tm_min = std::stoi(m[5]);
    tmv.tm_sec = std::stoi(m[6]);
    epoch = static_cast<int64_t>(timegm(&tmv));
    return true;
}

static int64_t intField(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) {
        return 0;
    }
    return it->get<int64_t>();
}

static bool lineToRow(const std::string& line, ClaudeUsageRow& row) {
    nlohmann::json d = nlohmann::json::parse(line, nullptr, false);
    if (d.is_discarded() || !d.is_object() || d.value("type", "") != "assistant") {
        return false;
    }
    auto mit = d.find("message");
    if (mit == d.end() || !mit->is_object()) {
        return false;
    }
    const nlohmann::json& m = *mit;
    auto uit = m.find("usage");
    if (uit == m.end() || !uit->is_object()) {
        return false;
    }
    const nlohmann::json& u = *uit;
    if (!m.contains("model") || !m["model"].is_string() || m["model"].get<std::string>() == "<synthetic>") {
        return false;
    }
    if (!m.contains("id") || !m["id"].is_string() || !d.contains("requestId") || !d["requestId"].is_string() ||
        !d.contains("timestamp") || !d["timestamp"].is_string()) {
        return false;
    }
    int64_t epoch = 0;
    if (!parseTimestamp(d["timestamp"].get<std::string>(), epoch)) {
        return false;
    }
    row = ClaudeUsageRow();
    row.account = "recount";
    row.sessionId = d.value("sessionId", "unknown");
    row.messageId = m["id"].get<std::string>();
    row.requestId = d["requestId"].get<std::string>();
    row.timestamp = epoch;
    row.model = m["model"].get<std::string>();
    row.speed = u.value("speed", "standard");
    row.input = intField(u, "input_tokens");
    row.output = intField(u, "output_tokens");
    row.cacheRead = intField(u, "cache_read_input_tokens");
    auto cc = u.find("cache_creation");
    if (cc != u.end() && cc->is_object()) {
        row.cacheWrite5m = intField(*cc, "ephemeral_5m_input_tokens");
        row.cacheWrite1h = intField(*cc, "ephemeral_1h_input_tokens");
    } else {
        row.cacheWrite5m = intField(u, "cache_creation_input_tokens");
        row.creationSplitMissing = true;
    }
    row.sidechain = d.value("isSidechain", false);
    return true;
}

int main(int argc, char** argv) {
    std::string root, page, versionsPath, out;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        if (k == "--root") root = argv[i + 1];
        else if (k == "--page") page = argv[i + 1];
        else if (k == "--versions") versionsPath = argv[i + 1];
        else if (k == "--out") out = argv[i + 1];
    }
    if (root.empty() || (page.empty() == versionsPath.empty()) || out.empty()) {
        std::cerr << "usage: test_claude_recount --root DIR (--page FILE | --versions FILE) --out FILE" << std::endl;
        return 2;
    }
    auto readText = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    std::vector<PriceVersion> versions;
    if (!page.empty()) {
        PriceParseResult catalog = PriceCatalog::parsePage(readText(page));
        if (!catalog.ok) {
            std::cerr << "pricing page rejected: " << catalog.error << std::endl;
            return 1;
        }
        PriceVersion v;
        v.models = catalog.models;
        versions.push_back(v);
    } else {
        nlohmann::json spec = nlohmann::json::parse(readText(versionsPath), nullptr, false);
        if (spec.is_discarded() || !spec.is_array() || spec.empty()) {
            std::cerr << "versions file is not a non-empty JSON array" << std::endl;
            return 1;
        }
        for (const auto& item : spec) {
            PriceParseResult catalog = PriceCatalog::parsePage(readText(item.at("page").get<std::string>()));
            if (!catalog.ok) {
                std::cerr << "pricing page rejected: " << catalog.error << std::endl;
                return 1;
            }
            PriceVersion v;
            v.effectiveFrom = item.at("effective_from").get<int64_t>();
            v.models = catalog.models;
            versions.push_back(v);
        }
    }

    char tmpl[] = "/tmp/aimon_recount_XXXXXX";
    char* tmp = mkdtemp(tmpl);
    if (!tmp) {
        std::cerr << "mkdtemp failed" << std::endl;
        return 1;
    }
    std::string dbPath = std::string(tmp) + "/history.db";
    ClaudeUsageStore store;
    if (!store.open(dbPath)) {
        std::cerr << "cannot open store" << std::endl;
        return 1;
    }

    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".jsonl") {
            files.push_back(entry.path());
        }
    }
    int64_t lines = 0, rowsParsed = 0, rejected = 0;
    for (const auto& file : files) {
        std::ifstream in(file);
        std::string line;
        std::vector<ClaudeUsageRow> batch;
        while (std::getline(in, line)) {
            ++lines;
            ClaudeUsageRow row;
            if (lineToRow(line, row)) {
                ++rowsParsed;
                batch.push_back(row);
            }
            if (batch.size() >= 1000) {
                ClaudeIngestResult r = store.ingest(batch);
                if (!r.ok) { std::cerr << "ingest failed: " << r.error << std::endl; return 1; }
                rejected += r.rejected;
                batch.clear();
            }
        }
        if (!batch.empty()) {
            ClaudeIngestResult r = store.ingest(batch);
            if (!r.ok) { std::cerr << "ingest failed: " << r.error << std::endl; return 1; }
            rejected += r.rejected;
        }
    }

    std::vector<ClaudeUsageBucket> buckets;
    std::string err;
    if (!store.aggregate("recount", 0, 4102444800LL, versions, buckets, err)) {
        std::cerr << "aggregate failed: " << err << std::endl;
        return 1;
    }
    std::ofstream o(out, std::ios::trunc);
    for (const auto& b : buckets) {
        nlohmann::json j;
        j["day"] = b.day;
        j["model"] = b.model;
        j["messages"] = b.messages;
        j["input"] = b.input;
        j["output"] = b.output;
        j["cache_read"] = b.cacheRead;
        j["cache_w5m"] = b.cacheWrite5m;
        j["cache_w1h"] = b.cacheWrite1h;
        j["cost_nano"] = b.costNano;
        j["unpriced_messages"] = b.unpricedMessages;
        o << j.dump() << "\n";
    }
    std::cout << "recount: " << files.size() << " files, " << lines << " lines, " << rowsParsed
              << " usable rows, " << rejected << " rejected by store, " << buckets.size() << " buckets" << std::endl;
    std::string cmd = std::string("rm -rf '") + tmp + "'";
    int rc = std::system(cmd.c_str());
    (void)rc;
    return 0;
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
