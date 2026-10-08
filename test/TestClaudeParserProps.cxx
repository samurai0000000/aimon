/*
 * TestClaudeParserProps.cxx
 *
 * RapidCheck properties for the transcript line parser: arbitrary truncations,
 * byte flips and random bytes never crash it, and every row it accepts is
 * valid for the store (the store rejects none of them).
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <rapidcheck.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "ClaudeCollector.hxx"
#include "ClaudeUsageStore.hxx"

using namespace aimon;

static const char* const kSamples[] = {
    "{\"type\":\"assistant\",\"message\":{\"id\":\"msg_1\",\"model\":\"claude-opus-5-5\",\"role\":\"assistant\","
    "\"content\":\"hello\",\"usage\":{\"input_tokens\":12,\"output_tokens\":34,\"cache_read_input_tokens\":56,"
    "\"speed\":\"fast\",\"cache_creation\":{\"ephemeral_5m_input_tokens\":7,\"ephemeral_1h_input_tokens\":8}}},"
    "\"requestId\":\"req_1\",\"timestamp\":\"2026-10-08T12:34:56.789Z\",\"isSidechain\":true,\"sessionId\":\"s\"}",
    "{\"type\":\"assistant\",\"message\":{\"id\":\"msg_2\",\"model\":\"claude-haiku-4-5-20251001\",\"usage\":"
    "{\"input_tokens\":1,\"output_tokens\":2,\"cache_creation_input_tokens\":40}},\"requestId\":\"req_2\","
    "\"timestamp\":\"2026-10-08T00:00:00Z\"}"};

int main() {
    setenv("RC_PARAMS", "max_success=1000", 0);

    char tmpl[] = "/tmp/aimon_parser_props_XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) {
        std::cerr << "mkdtemp failed" << std::endl;
        return 2;
    }
    const std::string dbPath = std::string(dir) + "/history.db";
    ClaudeUsageStore store;
    if (!store.open(dbPath)) {
        std::cerr << "cannot open store" << std::endl;
        return 2;
    }

    // Every accepted row must be accepted by the store as well.
    auto accepted = [&](const std::string& line) {
        ClaudeUsageRow row;
        ClaudeLineResult r = ClaudeCollector::parseLine(line, "acct", row);
        if (r != ClaudeLineResult::Row) {
            return true;
        }
        ClaudeIngestResult res = store.ingest({row});
        return res.ok && res.rejected == 0;
    };

    bool ok = true;
    ok &= rc::check("any prefix of a usage line never crashes and accepted rows are storable", [&]() {
        const std::string sample = kSamples[*rc::gen::inRange<size_t>(0, 2)];
        const size_t n = *rc::gen::inRange<size_t>(0, sample.size() + 1);
        RC_ASSERT(accepted(sample.substr(0, n)));
    });

    ok &= rc::check("byte flips in a usage line never crash and accepted rows are storable", [&]() {
        std::string sample = kSamples[*rc::gen::inRange<size_t>(0, 2)];
        const int flips = *rc::gen::inRange(1, 9);
        for (int i = 0; i < flips; ++i) {
            const size_t pos = *rc::gen::inRange<size_t>(0, sample.size());
            sample[pos] = static_cast<char>(*rc::gen::arbitrary<uint8_t>());
        }
        RC_ASSERT(accepted(sample));
    });

    ok &= rc::check("arbitrary bytes never crash the parser", [&]() {
        const auto text = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());
        RC_ASSERT(accepted(text));
    });

    ok &= rc::check("numeric field values are range checked", [&]() {
        const int64_t v = *rc::gen::arbitrary<int64_t>();
        std::string line = std::string("{\"type\":\"assistant\",\"message\":{\"id\":\"m\",\"model\":\"claude-opus-5-5\",\"usage\":"
                                       "{\"input_tokens\":") + std::to_string(v) +
                           ",\"output_tokens\":1}},\"requestId\":\"r\",\"timestamp\":\"2026-10-08T12:00:00Z\"}";
        ClaudeUsageRow row;
        ClaudeLineResult r = ClaudeCollector::parseLine(line, "acct", row);
        const bool inRange = v >= 0 && v <= ClaudeUsageStore::kMaxTokensPerField;
        RC_ASSERT((r == ClaudeLineResult::Row) == inRange);
    });

    store.close();
    std::string cmd = std::string("rm -rf '") + dir + "'";
    int rc = std::system(cmd.c_str());
    (void)rc;
    return ok ? 0 : 1;
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
