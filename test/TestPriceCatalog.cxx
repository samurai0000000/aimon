/*
 * TestPriceCatalog.cxx
 *
 * Qualification tests for PriceCatalog: parsing of the official pricing page,
 * strict validation, model id resolution, disk cache behavior and failure
 * handling. The only stand-in is the HTTP fetch function (a network boundary).
 *
 * Extra command line options handled before CppUTest sees the arguments:
 *   --expect FILE            expected-values JSON used by the full-page test
 *                            (default test/fixtures/pricing_expected.json)
 *   --live --out FILE --page-out FILE
 *                            fetch the real page once, write the parsed prices
 *                            as JSON lines to FILE and the raw page to
 *                            --page-out, skip the unit tests
 *   --seed-cache FILE --page FILE --now EPOCH
 *                            write a price cache file from a saved page, as if
 *                            it had been fetched at EPOCH (no network), skip
 *                            the unit tests
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "PriceCatalog.hxx"

// CppUTest's memory leak macros redefine `new`; its headers must come last.
#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/TestHarness.h>

using namespace aimon;

#ifndef AIMON_SOURCE_DIR
#error "AIMON_SOURCE_DIR must be defined by the build"
#endif

static std::string g_expectPath = std::string(AIMON_SOURCE_DIR) + "/test/fixtures/pricing_expected.json";

static std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static void writeFile(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << data;
}

static std::string pagePath() {
    return std::string(AIMON_SOURCE_DIR) + "/test/fixtures/pricing_page.md";
}

static std::string makeTempDir() {
    char tmpl[] = "/tmp/aimon_price_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    return dir ? std::string(dir) : std::string();
}

static void removeTree(const std::string& dir) {
    if (dir.rfind("/tmp/aimon_price_test_", 0) != 0) {
        return;
    }
    std::string cmd = "rm -rf '" + dir + "'";
    int rc = std::system(cmd.c_str());
    (void)rc;
}

// Replace the first occurrence of `from`; fails the test if it is absent.
static std::string replaceOnce(const std::string& text, const std::string& from, const std::string& to) {
    size_t pos = text.find(from);
    CHECK_TRUE(pos != std::string::npos);
    std::string out = text;
    out.replace(pos, from.size(), to);
    return out;
}

// Rewrite one cell of the first table row whose line starts with `rowPrefix`.
static std::string mutateRow(const std::string& page, const std::string& rowPrefix,
                             size_t cellIndex, const std::string& newCell) {
    size_t start = page.find("\n" + rowPrefix);
    CHECK_TRUE(start != std::string::npos);
    start += 1;
    size_t end = page.find('\n', start);
    std::string line = page.substr(start, end - start);
    std::vector<std::string> cells;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, '|')) {
        cells.push_back(cell);
    }
    // cells[0] is the empty text before the first '|'
    CHECK_TRUE(cellIndex + 1 < cells.size());
    cells[cellIndex + 1] = " " + newCell + " ";
    std::string rebuilt;
    for (size_t i = 1; i < cells.size(); ++i) {
        rebuilt += "|" + cells[i];
    }
    rebuilt += "|";
    return page.substr(0, start) + rebuilt + page.substr(end);
}

static PriceCatalog::FetchFn fixedFetch(const std::string& body, int status = 200,
                                        const std::string& error = std::string()) {
    return [body, status, error](const std::string&) {
        PriceFetchResult r;
        r.status = status;
        r.body = body;
        r.error = error;
        return r;
    };
}

static const ModelPrice* findModel(const PriceParseResult& r, const std::string& id) {
    auto it = r.models.find(id);
    return it == r.models.end() ? nullptr : &it->second;
}

//------------------------------------------------------------------------
// Parsing the real page
//------------------------------------------------------------------------

TEST_GROUP(PriceCatalogParse) {
    std::string page;
    void setup() { page = readFile(pagePath()); CHECK_TRUE(page.size() > 40000); }
};

static void checkRateSet(const nlohmann::json& e, const std::string& prefix, const ModelPrice& m) {
    LONGS_EQUAL(e.at(prefix + "input_nano").get<long>(), static_cast<long>(m.inputNano));
    LONGS_EQUAL(e.at(prefix + "write5m_nano").get<long>(), static_cast<long>(m.write5mNano));
    LONGS_EQUAL(e.at(prefix + "write1h_nano").get<long>(), static_cast<long>(m.write1hNano));
    LONGS_EQUAL(e.at(prefix + "read_nano").get<long>(), static_cast<long>(m.readNano));
    LONGS_EQUAL(e.at(prefix + "output_nano").get<long>(), static_cast<long>(m.outputNano));
}

static void checkAgainstExpected(const PriceParseResult& r, const nlohmann::json& expected) {
    CHECK_TRUE(r.ok);
    STRCMP_EQUAL("", r.error.c_str());
    CHECK_FALSE(expected.is_discarded());
    LONGS_EQUAL(static_cast<long>(expected.size()), static_cast<long>(r.models.size()));
    for (auto it = expected.begin(); it != expected.end(); ++it) {
        const ModelPrice* m = findModel(r, it.key());
        CHECK_TRUE(m != nullptr);
        const nlohmann::json& e = it.value();
        checkRateSet(e, "", *m);
        CHECK_EQUAL(e.contains("fast_input_nano"), m->hasFast);
        if (m->hasFast) {
            LONGS_EQUAL(e.at("fast_input_nano").get<long>(), static_cast<long>(m->fastInputNano));
            LONGS_EQUAL(e.at("fast_write5m_nano").get<long>(), static_cast<long>(m->fastWrite5mNano));
            LONGS_EQUAL(e.at("fast_write1h_nano").get<long>(), static_cast<long>(m->fastWrite1hNano));
            LONGS_EQUAL(e.at("fast_read_nano").get<long>(), static_cast<long>(m->fastReadNano));
            LONGS_EQUAL(e.at("fast_output_nano").get<long>(), static_cast<long>(m->fastOutputNano));
        }
        CHECK_EQUAL(e.contains("tier_threshold_tokens"), m->upperTier != nullptr);
        if (m->upperTier) {
            LONGS_EQUAL(e.at("tier_threshold_tokens").get<long>(), static_cast<long>(m->tierThresholdTokens));
            checkRateSet(e, "upper_", *m->upperTier);
            CHECK_FALSE(m->upperTier->hasFast);
        } else {
            LONGS_EQUAL(0, static_cast<long>(m->tierThresholdTokens));
        }
    }
}

TEST(PriceCatalogParse, RealPageMatchesExpectedValuesForEveryModelAndField) {
    checkAgainstExpected(PriceCatalog::parsePage(page),
                         nlohmann::json::parse(readFile(g_expectPath), nullptr, false));
}

TEST(PriceCatalogParse, ChangedLivePageWithTieredHaikuMatchesExpectedValues) {
    std::string v2 = readFile(std::string(AIMON_SOURCE_DIR) + "/test/fixtures/pricing_page_v2.md");
    PriceParseResult r = PriceCatalog::parsePage(v2);
    checkAgainstExpected(r, nlohmann::json::parse(readFile(std::string(AIMON_SOURCE_DIR) +
                                                           "/test/fixtures/pricing_expected_v2.json"), nullptr, false));
    LONGS_EQUAL(0, static_cast<long>(r.quarantined.size()));
    LONGS_EQUAL(0, r.skippedRows);

    const ModelPrice* h = findModel(r, "claude-haiku-5-5");
    CHECK_TRUE(h != nullptr && h->upperTier != nullptr);
    LONGS_EQUAL(100000, static_cast<long>(h->tierThresholdTokens));
    LONGS_EQUAL(100, static_cast<long>(h->inputNano));          // $0.10 up to 100k
    LONGS_EQUAL(125, static_cast<long>(h->write5mNano));
    LONGS_EQUAL(200, static_cast<long>(h->write1hNano));
    LONGS_EQUAL(10, static_cast<long>(h->readNano));
    LONGS_EQUAL(500, static_cast<long>(h->outputNano));
    LONGS_EQUAL(500, static_cast<long>(h->upperTier->inputNano));   // $0.50 over 100k
    LONGS_EQUAL(625, static_cast<long>(h->upperTier->write5mNano));
    LONGS_EQUAL(1000, static_cast<long>(h->upperTier->write1hNano));
    LONGS_EQUAL(50, static_cast<long>(h->upperTier->readNano));
    LONGS_EQUAL(2500, static_cast<long>(h->upperTier->outputNano));
    CHECK_FALSE(h->hasFast);                                    // no fast row for it
    LONGS_EQUAL(100, static_cast<long>(findModel(r, "claude-sonnet-5-5")->readNano));   // changed from 200
}

TEST(PriceCatalogParse, HandVerifiedValuesFromThePage) {
    PriceParseResult r = PriceCatalog::parsePage(page);
    CHECK_TRUE(r.ok);

    const ModelPrice* o55 = findModel(r, "claude-opus-5-5");
    CHECK_TRUE(o55 != nullptr);
    LONGS_EQUAL(4000, static_cast<long>(o55->inputNano));
    LONGS_EQUAL(5000, static_cast<long>(o55->write5mNano));
    LONGS_EQUAL(8000, static_cast<long>(o55->write1hNano));
    LONGS_EQUAL(200, static_cast<long>(o55->readNano));      // footnote marker stripped
    LONGS_EQUAL(20000, static_cast<long>(o55->outputNano));
    CHECK_TRUE(o55->hasFast);
    LONGS_EQUAL(8000, static_cast<long>(o55->fastInputNano));
    LONGS_EQUAL(40000, static_cast<long>(o55->fastOutputNano));
    LONGS_EQUAL(10000, static_cast<long>(o55->fastWrite5mNano));  // 8000 * 5000 / 4000
    LONGS_EQUAL(16000, static_cast<long>(o55->fastWrite1hNano));  // 8000 * 8000 / 4000
    LONGS_EQUAL(400, static_cast<long>(o55->fastReadNano));       // 8000 * 200 / 4000

    const ModelPrice* s5 = findModel(r, "claude-sonnet-5");
    CHECK_TRUE(s5 != nullptr);
    LONGS_EQUAL(2000, static_cast<long>(s5->inputNano));
    LONGS_EQUAL(10000, static_cast<long>(s5->outputNano));
    LONGS_EQUAL(200, static_cast<long>(s5->readNano));
    CHECK_FALSE(s5->hasFast);

    const ModelPrice* h45 = findModel(r, "claude-haiku-4-5");
    CHECK_TRUE(h45 != nullptr);
    LONGS_EQUAL(1000, static_cast<long>(h45->inputNano));
    LONGS_EQUAL(1250, static_cast<long>(h45->write5mNano));
    LONGS_EQUAL(100, static_cast<long>(h45->readNano));

    const ModelPrice* f51 = findModel(r, "claude-fable-5-1");
    CHECK_TRUE(f51 != nullptr);
    LONGS_EQUAL(250, static_cast<long>(f51->readNano));      // $0.25 with a footnote marker
}

TEST(PriceCatalogParse, CombinedFastRowAppliesToEveryNamedModel) {
    PriceParseResult r = PriceCatalog::parsePage(page);
    CHECK_TRUE(r.ok);
    const ModelPrice* o5 = findModel(r, "claude-opus-5");
    const ModelPrice* o48 = findModel(r, "claude-opus-4-8");
    CHECK_TRUE(o5 != nullptr && o48 != nullptr);
    CHECK_TRUE(o5->hasFast);
    CHECK_TRUE(o48->hasFast);
    LONGS_EQUAL(10000, static_cast<long>(o5->fastInputNano));
    LONGS_EQUAL(10000, static_cast<long>(o48->fastInputNano));
    LONGS_EQUAL(50000, static_cast<long>(o5->fastOutputNano));
    LONGS_EQUAL(50000, static_cast<long>(o48->fastOutputNano));
    LONGS_EQUAL(0, r.skippedFastEntries);
}

TEST(PriceCatalogParse, ParentheticalNamesProduceCleanIds) {
    PriceParseResult r = PriceCatalog::parsePage(page);
    CHECK_TRUE(r.ok);
    CHECK_TRUE(findModel(r, "claude-opus-4-1") != nullptr);   // "(retired, except ...)" stripped
    CHECK_TRUE(findModel(r, "claude-opus-4") != nullptr);
    CHECK_TRUE(findModel(r, "claude-mythos-5-1") != nullptr);  // "(limited availability)" stripped
    for (const auto& kv : r.models) {
        CHECK_TRUE(kv.first.find('(') == std::string::npos);
        CHECK_TRUE(kv.first.find(' ') == std::string::npos);
        CHECK_TRUE(kv.first.rfind("claude-", 0) == 0);
    }
}

TEST(PriceCatalogParse, ParsingIsDeterministic) {
    PriceParseResult a = PriceCatalog::parsePage(page);
    PriceParseResult b = PriceCatalog::parsePage(page);
    CHECK_TRUE(a.ok && b.ok);
    CHECK_TRUE(a.models == b.models);
}

TEST(PriceCatalogParse, UnknownModelInFastTableIsSkippedNotFatal) {
    std::string mutated = replaceOnce(page, "Claude Opus 5 / Claude Opus 4.8", "Claude Opus 5 / Claude Opus 9.9");
    PriceParseResult r = PriceCatalog::parsePage(mutated);
    CHECK_TRUE(r.ok);
    LONGS_EQUAL(1, r.skippedFastEntries);
    CHECK_TRUE(findModel(r, "claude-opus-5")->hasFast);
    CHECK_FALSE(findModel(r, "claude-opus-4-8")->hasFast);
}

//------------------------------------------------------------------------
// Rejection rules
//------------------------------------------------------------------------

TEST_GROUP(PriceCatalogReject) {
    std::string page;
    void setup() { page = readFile(pagePath()); }
    void expectRejected(const std::string& text) {
        PriceParseResult r = PriceCatalog::parsePage(text);
        CHECK_FALSE(r.ok);
        CHECK_TRUE(!r.error.empty());
        CHECK_TRUE(r.models.empty());
    }
};

TEST(PriceCatalogReject, MissingModelPricingHeading) {
    expectRejected(replaceOnce(page, "## Model pricing", "## Model prices"));
}

TEST(PriceCatalogReject, RenamedColumnHeader) {
    expectRejected(replaceOnce(page, "5m cache writes", "5m cache write"));
}

TEST(PriceCatalogReject, MissingFastModeTable) {
    expectRejected(replaceOnce(page, "### Fast mode pricing", "### Fast mode"));
}

TEST(PriceCatalogReject, FewerThanThreeModels) {
    std::string mini =
        "## Model pricing\n\n"
        "| Model | Base input tokens | 5m cache writes | 1h cache writes | Cache hits and refreshes | Output tokens |\n"
        "| :-- | :-- | :-- | :-- | :-- | :-- |\n"
        "| Claude Sonnet 5 | $2 / MTok | $2.50 / MTok | $4 / MTok | $0.20 / MTok | $10 / MTok |\n"
        "| Claude Haiku 4.5 | $1 / MTok | $1.25 / MTok | $2 / MTok | $0.10 / MTok | $5 / MTok |\n\n"
        "### Fast mode pricing\n\n| Model | Input | Output |\n| :-- | :-- | :-- |\n"
        "| Claude Sonnet 5 | $4 / MTok | $20 / MTok |\n";
    expectRejected(mini);
}

TEST(PriceCatalogReject, TruncatedPage) {
    expectRejected(page.substr(0, page.size() / 4));
    expectRejected(page.substr(0, 700));
}

TEST(PriceCatalogReject, EmptyWhitespaceAndGarbage) {
    expectRejected("");
    expectRejected("   \n\n\t  \n");
    std::string garbage;
    for (int i = 0; i < 4096; ++i) {
        garbage.push_back(static_cast<char>((i * 131 + 7) & 0xff));
    }
    expectRejected(garbage);
}

TEST(PriceCatalogReject, WrongColumnCountInRow) {
    size_t start = page.find("\n| Claude Opus 5.5 ") + 1;
    size_t end = page.find('\n', start);
    std::string row = page.substr(start, end - start);
    std::string shortRow = row.substr(0, row.rfind('|', row.size() - 2)) + "|";
    expectRejected(page.substr(0, start) + shortRow + page.substr(end));
}

//------------------------------------------------------------------------
// Row-level problems quarantine one model; the rest of the page is kept
//------------------------------------------------------------------------

TEST_GROUP(PriceCatalogQuarantine) {
    std::string page;
    void setup() { page = readFile(pagePath()); }
    void expectQuarantined(const std::string& text, const std::string& id) {
        PriceParseResult r = PriceCatalog::parsePage(text);
        CHECK_TRUE(r.ok);
        CHECK_TRUE(r.models.count(id) == 0);
        LONGS_EQUAL(18, static_cast<long>(r.models.size()));
        bool listed = false;
        for (const auto& q : r.quarantined) {
            if (q.modelId == id) {
                listed = !q.reason.empty();
            }
        }
        CHECK_TRUE(listed);
        CHECK_TRUE(r.models.count("claude-sonnet-5") == 1);   // others still priced
    }
};

TEST(PriceCatalogQuarantine, ZeroPrice) {
    expectQuarantined(mutateRow(page, "| Claude Opus 5.5 ", 1, "$0 / MTok"), "claude-opus-5-5");
}

TEST(PriceCatalogQuarantine, FiveMinuteWriteNotOneAndAQuarterTimesInput) {
    expectQuarantined(mutateRow(page, "| Claude Opus 5.5 ", 2, "$9 / MTok"), "claude-opus-5-5");
}

TEST(PriceCatalogQuarantine, OneHourWriteNotTwiceInput) {
    expectQuarantined(mutateRow(page, "| Claude Opus 5.5 ", 3, "$9 / MTok"), "claude-opus-5-5");
}

TEST(PriceCatalogQuarantine, CacheReadAboveInput) {
    expectQuarantined(mutateRow(page, "| Claude Opus 5.5 ", 4, "$5 / MTok"), "claude-opus-5-5");
}

TEST(PriceCatalogQuarantine, PriceWithMoreThanThreeDecimals) {
    expectQuarantined(mutateRow(page, "| Claude Opus 5.5 ", 5, "$20.0001 / MTok"), "claude-opus-5-5");
}

TEST(PriceCatalogQuarantine, NonPriceCell) {
    expectQuarantined(mutateRow(page, "| Claude Opus 5.5 ", 5, "N/A"), "claude-opus-5-5");
}

TEST(PriceCatalogQuarantine, DuplicateModelRowQuarantinesTheModel) {
    size_t start = page.find("\n| Claude Opus 5.5 ") + 1;
    size_t end = page.find('\n', start);
    std::string row = page.substr(start, end - start);
    expectQuarantined(page.substr(0, end + 1) + row + "\n" + page.substr(end + 1), "claude-opus-5-5");
}

TEST(PriceCatalogQuarantine, InvalidBytesInsideAModelNameSkipTheRowOnly) {
    PriceParseResult r = PriceCatalog::parsePage(
        mutateRow(page, "| Claude Opus 5.5 ", 0, std::string("Claude Opus 5.5\xff\xfe")));
    CHECK_TRUE(r.ok);
    LONGS_EQUAL(1, r.skippedRows);
    CHECK_TRUE(r.models.count("claude-opus-5-5") == 0);
    LONGS_EQUAL(18, static_cast<long>(r.models.size()));
}

TEST(PriceCatalogQuarantine, FastModeRowProblemsSkipOnlyThatRow) {
    PriceParseResult r = PriceCatalog::parsePage(
        replaceOnce(page, "| $8 / MTok  | $40 / MTok |", "| garbage    | $40 / MTok |"));
    CHECK_TRUE(r.ok);
    CHECK_TRUE(r.skippedFastEntries >= 1);
    CHECK_FALSE(findModel(r, "claude-opus-5-5")->hasFast);   // its fast row was the broken one
    CHECK_TRUE(findModel(r, "claude-opus-5")->hasFast);      // the other fast row is intact
}

TEST(PriceCatalogQuarantine, TooManyAnomalousRowsRejectsThePage) {
    std::string mini =
        "## Model pricing\n\n"
        "| Model | Base input tokens | 5m cache writes | 1h cache writes | Cache hits and refreshes | Output tokens |\n"
        "| :-- | :-- | :-- | :-- | :-- | :-- |\n"
        "| Claude Sonnet 5 | $2 / MTok | $2.50 / MTok | $4 / MTok | $0.20 / MTok | $10 / MTok |\n"
        "| Claude Haiku 4.5 | $1 / MTok | $1.25 / MTok | $2 / MTok | $0.10 / MTok | $5 / MTok |\n"
        "| Claude Opus 5 | $5 / MTok | $6.25 / MTok | $10 / MTok | $0.50 / MTok | $25 / MTok |\n"
        "| Claude Bad One | n/a | n/a | n/a | n/a | n/a |\n"
        "| Claude Bad Two | n/a | n/a | n/a | n/a | n/a |\n\n"
        "### Fast mode pricing\n\n| Model | Input | Output |\n| :-- | :-- | :-- |\n"
        "| Claude Opus 5 | $10 / MTok | $50 / MTok |\n";
    PriceParseResult r = PriceCatalog::parsePage(mini);
    CHECK_FALSE(r.ok);                       // 2 of 5 rows anomalous: over the 25% limit
    CHECK_TRUE(r.error.find("anomal") != std::string::npos);
    // one bad row of four is exactly 25% and is tolerated; the bad model is quarantined
    std::string tolerated = replaceOnce(mini, "| Claude Bad Two | n/a | n/a | n/a | n/a | n/a |\n", "");
    PriceParseResult ok = PriceCatalog::parsePage(tolerated);
    CHECK_TRUE(ok.ok);
    LONGS_EQUAL(3, static_cast<long>(ok.models.size()));
    LONGS_EQUAL(1, static_cast<long>(ok.quarantined.size()));
    STRCMP_EQUAL("claude-bad-one", ok.quarantined[0].modelId.c_str());
}

TEST(PriceCatalogQuarantine, TieredRowsMustComeAsAConsistentPair) {
    std::string v2 = readFile(std::string(AIMON_SOURCE_DIR) + "/test/fixtures/pricing_page_v2.md");
    auto quarantinedHaiku = [](const std::string& text) {
        PriceParseResult r = PriceCatalog::parsePage(text);
        CHECK_TRUE(r.ok);
        CHECK_TRUE(r.models.count("claude-haiku-5-5") == 0);
        bool listed = false;
        for (const auto& q : r.quarantined) {
            listed = listed || q.modelId == "claude-haiku-5-5";
        }
        CHECK_TRUE(listed);
        CHECK_TRUE(r.models.count("claude-sonnet-5-5") == 1);
    };
    // missing the upper row
    {
        size_t s = v2.find("| Claude Haiku 5.5 (for prompts over 100,000 tokens)");
        size_t e = v2.find('\n', s);
        quarantinedHaiku(v2.substr(0, s) + v2.substr(e + 1));
    }
    // thresholds disagree
    quarantinedHaiku(replaceOnce(v2, "(for prompts over 100,000 tokens)", "(for prompts over 200,000 tokens)"));
    // duplicate lower row
    quarantinedHaiku(replaceOnce(v2, "(for prompts over 100,000 tokens)", "(for prompts up to 100,000 tokens)"));
    // unrecognized annotation
    quarantinedHaiku(replaceOnce(v2, "(for prompts over 100,000 tokens)", "(for prompts of many tokens)"));
    // a tiered row mixed with a plain row for the same model
    quarantinedHaiku(replaceOnce(v2, "| Claude Haiku 5.5 (for prompts over 100,000 tokens)", "| Claude Haiku 5.5"));
    // a broken price in one tier quarantines the model
    quarantinedHaiku(mutateRow(v2, "| Claude Haiku 5.5 (for prompts over 100,000 tokens)", 5, "N/A"));
}

//------------------------------------------------------------------------
// Cells, names, resolution
//------------------------------------------------------------------------

TEST_GROUP(PriceCatalogCells) {};

TEST(PriceCatalogCells, ValidPriceCells) {
    int64_t n = 0;
    CHECK_TRUE(PriceCatalog::parsePriceCell("$4 / MTok", n));
    LONGS_EQUAL(4000, static_cast<long>(n));
    CHECK_TRUE(PriceCatalog::parsePriceCell("$0.20 / MTok<sup>2</sup>", n));
    LONGS_EQUAL(200, static_cast<long>(n));
    CHECK_TRUE(PriceCatalog::parsePriceCell("$12.50 / MTok", n));
    LONGS_EQUAL(12500, static_cast<long>(n));
    CHECK_TRUE(PriceCatalog::parsePriceCell("$0.001 / MTok", n));
    LONGS_EQUAL(1, static_cast<long>(n));
    CHECK_TRUE(PriceCatalog::parsePriceCell("  $ 10/MTok  ", n));
    LONGS_EQUAL(10000, static_cast<long>(n));
    CHECK_TRUE(PriceCatalog::parsePriceCell("$999999.999 / MTok", n));
    LONGS_EQUAL(999999999, static_cast<long>(n));
}

TEST(PriceCatalogCells, InvalidPriceCells) {
    int64_t n = 0;
    const char* bad[] = {"", "$", "4 / MTok", "$4 / MTok extra", "$.5 / MTok", "$1.0001 / MTok",
                         "$-1 / MTok", "$1e3 / MTok", "$4 / MTok<sup>2", "$4 / GTok", "$1,000 / MTok",
                         "$9999999 / MTok", "$4 /", "N/A"};
    for (const char* cell : bad) {
        CHECK_FALSE(PriceCatalog::parsePriceCell(cell, n));
    }
}

TEST(PriceCatalogCells, ModelNameToId) {
    STRCMP_EQUAL("claude-opus-5-5", PriceCatalog::modelNameToId("Claude Opus 5.5").c_str());
    STRCMP_EQUAL("claude-haiku-4-5", PriceCatalog::modelNameToId("  Claude Haiku 4.5  ").c_str());
    STRCMP_EQUAL("claude-opus-4-1",
                 PriceCatalog::modelNameToId("Claude Opus 4.1 ([retired, except on Bedrock](https://x.y/z))").c_str());
    STRCMP_EQUAL("", PriceCatalog::modelNameToId("").c_str());
    STRCMP_EQUAL("", PriceCatalog::modelNameToId("GPT 5").c_str());
    STRCMP_EQUAL("", PriceCatalog::modelNameToId("Claude Opus 5.5!").c_str());
    STRCMP_EQUAL("", PriceCatalog::modelNameToId("Claude").c_str());
}

TEST(PriceCatalogCells, ResolveModelIdAcceptsExactAndDatedOnly) {
    PriceParseResult r = PriceCatalog::parsePage(readFile(pagePath()));
    CHECK_TRUE(r.ok);
    STRCMP_EQUAL("claude-opus-5-5", PriceCatalog::resolveModelId(r.models, "claude-opus-5-5").c_str());
    STRCMP_EQUAL("claude-opus-5", PriceCatalog::resolveModelId(r.models, "claude-opus-5").c_str());
    STRCMP_EQUAL("claude-haiku-4-5", PriceCatalog::resolveModelId(r.models, "claude-haiku-4-5-20251001").c_str());
    STRCMP_EQUAL("claude-opus-5", PriceCatalog::resolveModelId(r.models, "claude-opus-5-20260101").c_str());
    STRCMP_EQUAL("claude-opus-5-5", PriceCatalog::resolveModelId(r.models, "claude-opus-5-5-20260101").c_str());
    STRCMP_EQUAL("", PriceCatalog::resolveModelId(r.models, "claude-opus-5-6").c_str());
    STRCMP_EQUAL("", PriceCatalog::resolveModelId(r.models, "claude-opus-5-5x").c_str());
    STRCMP_EQUAL("", PriceCatalog::resolveModelId(r.models, "claude-opus-5-2026010").c_str());
    STRCMP_EQUAL("", PriceCatalog::resolveModelId(r.models, "claude-unknown-9").c_str());
    STRCMP_EQUAL("", PriceCatalog::resolveModelId(r.models, "").c_str());
    STRCMP_EQUAL("", PriceCatalog::resolveModelId(r.models, "<synthetic>").c_str());
}

TEST(PriceCatalogCells, Sha256KnownVectors) {
    STRCMP_EQUAL("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                 PriceCatalog::sha256Hex("").c_str());
    STRCMP_EQUAL("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                 PriceCatalog::sha256Hex("abc").c_str());
}

//------------------------------------------------------------------------
// Refresh, cache and failure behavior
//------------------------------------------------------------------------

TEST_GROUP(PriceCatalogCache) {
    std::string dir;
    std::string cachePath;
    std::string page;
    void setup() {
        dir = makeTempDir();
        CHECK_TRUE(!dir.empty());
        cachePath = dir + "/claude_prices.json";
        page = readFile(pagePath());
    }
    void teardown() { removeTree(dir); }
};

TEST(PriceCatalogCache, RefreshLoadsModelsAndWritesPrivateCache) {
    PriceCatalog catalog(cachePath, "https://example.invalid/pricing.md", fixedFetch(page));
    CHECK_TRUE(catalog.refresh(1000000));

    auto price = catalog.lookup("claude-opus-5-5");
    CHECK_TRUE(price.has_value());
    LONGS_EQUAL(4000, static_cast<long>(price->inputNano));
    CHECK_TRUE(catalog.lookup("claude-haiku-4-5-20251001").has_value());
    CHECK_FALSE(catalog.lookup("claude-opus-5-6").has_value());

    struct stat st;
    LONGS_EQUAL(0, stat(cachePath.c_str(), &st));
    LONGS_EQUAL(0600, static_cast<long>(st.st_mode & 0777));
    CHECK_TRUE(access((cachePath + ".tmp").c_str(), F_OK) != 0);

    PriceCatalogStatus s = catalog.status(1000000, 24);
    LONGS_EQUAL(19, static_cast<long>(s.modelsLoaded));
    CHECK_FALSE(s.stale);
    STRCMP_EQUAL(PriceCatalog::sha256Hex(page).c_str(), s.pageSha256.c_str());
    STRCMP_EQUAL("https://example.invalid/pricing.md", s.sourceUrl.c_str());
}

TEST(PriceCatalogCache, NewObjectRestoresTheCache) {
    {
        PriceCatalog first(cachePath, "u", fixedFetch(page));
        CHECK_TRUE(first.refresh(1234567));
    }
    PriceCatalog second(cachePath, "u", fixedFetch(""));
    CHECK_TRUE(second.loadCache());
    PriceCatalogStatus s = second.status(1234567, 24);
    LONGS_EQUAL(1234567, static_cast<long>(s.fetchedAtEpoch));
    LONGS_EQUAL(19, static_cast<long>(s.modelsLoaded));
    CHECK_TRUE(second.snapshot() == PriceCatalog::parsePage(page).models);
}

TEST(PriceCatalogCache, StaleAfterThreeRefreshIntervals) {
    PriceCatalog catalog(cachePath, "u", fixedFetch(page));
    CHECK_TRUE(catalog.refresh(1000));
    const int64_t window = 3LL * 24 * 3600;
    CHECK_FALSE(catalog.status(1000 + window, 24).stale);
    CHECK_TRUE(catalog.status(1000 + window + 1, 24).stale);
}

TEST(PriceCatalogCache, FailedFetchKeepsPreviousPrices) {
    bool failing = false;
    int status = 200;
    PriceCatalog catalog(cachePath, "u", [&](const std::string&) {
        PriceFetchResult r;
        if (failing) {
            r.status = status;
            r.error = status == 0 ? "timeout" : "";
            r.body = "not found";
        } else {
            r.status = 200;
            r.body = page;
        }
        return r;
    });
    CHECK_TRUE(catalog.refresh(5000));
    std::map<std::string, ModelPrice> before = catalog.snapshot();

    failing = true;
    for (int code : {404, 500, 503, 0}) {
        status = code;
        CHECK_FALSE(catalog.refresh(6000));
        CHECK_TRUE(catalog.snapshot() == before);
        PriceCatalogStatus s = catalog.status(6000, 24);
        CHECK_TRUE(!s.error.empty());
        LONGS_EQUAL(5000, static_cast<long>(s.fetchedAtEpoch));
    }
}

TEST(PriceCatalogCache, NoCacheAndFailedFetchMeansUnpricedWithError) {
    PriceCatalog catalog(cachePath, "u", fixedFetch("", 0, "connection refused"));
    CHECK_FALSE(catalog.loadCache());
    CHECK_FALSE(catalog.refresh(100));
    CHECK_FALSE(catalog.lookup("claude-opus-5-5").has_value());
    PriceCatalogStatus s = catalog.status(100, 24);
    LONGS_EQUAL(0, static_cast<long>(s.modelsLoaded));
    CHECK_TRUE(s.stale);
    CHECK_TRUE(!s.error.empty());
}

TEST(PriceCatalogCache, StructurallyBrokenPageIsRejectedAndPreviousCacheKept) {
    std::string current = page;
    PriceCatalog catalog(cachePath, "u", [&](const std::string&) {
        PriceFetchResult r;
        r.status = 200;
        r.body = current;
        return r;
    });
    CHECK_TRUE(catalog.refresh(100));
    std::map<std::string, ModelPrice> before = catalog.snapshot();
    std::string cacheBefore = readFile(cachePath);

    current = replaceOnce(page, "5m cache writes", "5m cache write");
    CHECK_FALSE(catalog.refresh(200));
    CHECK_TRUE(catalog.snapshot() == before);
    STRCMP_EQUAL(cacheBefore.c_str(), readFile(cachePath).c_str());
    CHECK_TRUE(!catalog.status(200, 24).error.empty());
}

TEST(PriceCatalogCache, RowLevelProblemQuarantinesOneModelAndKeepsTheRest) {
    std::string current = mutateRow(page, "| Claude Opus 5.5 ", 2, "$9 / MTok");
    PriceCatalog catalog(cachePath, "u", fixedFetch(current));
    CHECK_TRUE(catalog.refresh(100));
    CHECK_FALSE(catalog.lookup("claude-opus-5-5").has_value());
    CHECK_TRUE(catalog.lookup("claude-sonnet-5").has_value());
    LONGS_EQUAL(18, static_cast<long>(catalog.status(100, 24).modelsLoaded));
    LONGS_EQUAL(1, static_cast<long>(catalog.status(100, 24).quarantinedCount));

    PriceCatalog reload(cachePath, "u", fixedFetch(""));
    CHECK_TRUE(reload.loadCache());
    LONGS_EQUAL(1, static_cast<long>(reload.status(100, 24).quarantinedCount));
    CHECK_FALSE(reload.lookup("claude-opus-5-5").has_value());
}

TEST(PriceCatalogCache, CacheRoundTripKeepsTiersAndFastRates) {
    std::string v2 = readFile(std::string(AIMON_SOURCE_DIR) + "/test/fixtures/pricing_page_v2.md");
    {
        PriceCatalog first(cachePath, "u", fixedFetch(v2));
        CHECK_TRUE(first.refresh(777));
    }
    PriceCatalog second(cachePath, "u", fixedFetch(""));
    CHECK_TRUE(second.loadCache());
    CHECK_TRUE(second.snapshot() == PriceCatalog::parsePage(v2).models);
    auto h = second.lookup("claude-haiku-5-5-20260101");
    CHECK_TRUE(h.has_value() && h->upperTier != nullptr);
    LONGS_EQUAL(500, static_cast<long>(h->upperTier->inputNano));
    CHECK_TRUE(second.lookup("claude-opus-5-5")->hasFast);
}

TEST(PriceCatalogCache, ModelsJsonRoundTripAndRejectsBadTierData) {
    std::string v2 = readFile(std::string(AIMON_SOURCE_DIR) + "/test/fixtures/pricing_page_v2.md");
    PriceParseResult r = PriceCatalog::parsePage(v2);
    std::string json = PriceCatalog::modelsToJson(r.models);
    std::map<std::string, ModelPrice> back;
    std::string error;
    CHECK_TRUE(PriceCatalog::modelsFromJson(json, back, error));
    CHECK_TRUE(back == r.models);

    nlohmann::json doc = nlohmann::json::parse(json);
    doc["claude-haiku-5-5"]["upper"]["input_nano"] = 0;
    CHECK_FALSE(PriceCatalog::modelsFromJson(doc.dump(), back, error));
    CHECK_TRUE(!error.empty());
    doc = nlohmann::json::parse(json);
    doc["claude-haiku-5-5"].erase("tier_threshold_tokens");
    CHECK_FALSE(PriceCatalog::modelsFromJson(doc.dump(), back, error));
    CHECK_FALSE(PriceCatalog::modelsFromJson("not json", back, error));
    CHECK_FALSE(PriceCatalog::modelsFromJson("{\"claude-a\":{}}", back, error));
}

TEST(PriceCatalogCache, RefreshReportsWhetherThePageChanged) {
    std::string current = page;
    PriceCatalog catalog(cachePath, "u", [&](const std::string&) {
        PriceFetchResult r;
        r.status = 200;
        r.body = current;
        return r;
    });
    bool changed = false;
    CHECK_TRUE(catalog.refresh(100, &changed));
    CHECK_TRUE(changed);                       // first page is a change
    CHECK_TRUE(catalog.refresh(200, &changed));
    CHECK_FALSE(changed);                      // same bytes
    current = readFile(std::string(AIMON_SOURCE_DIR) + "/test/fixtures/pricing_page_v2.md");
    CHECK_TRUE(catalog.refresh(300, &changed));
    CHECK_TRUE(changed);
    current = "garbage";
    changed = true;
    CHECK_FALSE(catalog.refresh(400, &changed));
    CHECK_FALSE(changed);                      // a failed fetch is not a change
}

TEST(PriceCatalogCache, ChangedPriceOnTheNextRefreshIsPickedUp) {
    std::string current = page;
    PriceCatalog catalog(cachePath, "u", [&](const std::string&) {
        PriceFetchResult r;
        r.status = 200;
        r.body = current;
        return r;
    });
    CHECK_TRUE(catalog.refresh(100));
    LONGS_EQUAL(4000, static_cast<long>(catalog.lookup("claude-opus-5-5")->inputNano));
    std::string oldSha = catalog.status(100, 24).pageSha256;

    // Opus 5.5 input 4 -> 6 with consistent cache columns (7.5 / 12 / 0.30 read).
    current = mutateRow(page, "| Claude Opus 5.5 ", 1, "$6 / MTok");
    current = mutateRow(current, "| Claude Opus 5.5 ", 2, "$7.50 / MTok");
    current = mutateRow(current, "| Claude Opus 5.5 ", 3, "$12 / MTok");
    current = mutateRow(current, "| Claude Opus 5.5 ", 4, "$0.30 / MTok");
    CHECK_TRUE(catalog.refresh(300));
    LONGS_EQUAL(6000, static_cast<long>(catalog.lookup("claude-opus-5-5")->inputNano));
    CHECK_TRUE(catalog.status(300, 24).pageSha256 != oldSha);
}

TEST(PriceCatalogCache, OversizedBodyIsRejected) {
    std::string huge(PriceCatalog::kMaxBodyBytes + 1, 'x');
    PriceCatalog catalog(cachePath, "u", fixedFetch(huge));
    CHECK_FALSE(catalog.refresh(100));
    CHECK_TRUE(catalog.status(100, 24).error.find("large") != std::string::npos);
    CHECK_TRUE(access(cachePath.c_str(), F_OK) != 0);
}

TEST(PriceCatalogCache, CorruptCacheIsRejectedWithoutCrashing) {
    writeFile(cachePath, "{not json");
    PriceCatalog catalog(cachePath, "u", fixedFetch(""));
    CHECK_FALSE(catalog.loadCache());
    CHECK_FALSE(catalog.lookup("claude-opus-5-5").has_value());
    CHECK_TRUE(catalog.status(1, 24).error.find("cache") != std::string::npos);

    writeFile(cachePath, "{\"models\":{\"claude-opus-5-5\":{\"input_nano\":-5}}}");
    PriceCatalog other(cachePath, "u", fixedFetch(""));
    CHECK_FALSE(other.loadCache());
    CHECK_FALSE(other.lookup("claude-opus-5-5").has_value());
}

TEST(PriceCatalogCache, LeftoverTempFileFromACrashIsReplaced) {
    writeFile(cachePath + ".tmp", "garbage from a killed writer");
    PriceCatalog catalog(cachePath, "u", fixedFetch(page));
    CHECK_TRUE(catalog.refresh(100));
    CHECK_TRUE(access((cachePath + ".tmp").c_str(), F_OK) != 0);
    PriceCatalog reload(cachePath, "u", fixedFetch(""));
    CHECK_TRUE(reload.loadCache());
}

TEST(PriceCatalogCache, CacheIsAlwaysCompleteWhenTheWriterIsKilled) {
    PriceCatalog seed(cachePath, "u", fixedFetch(page));
    CHECK_TRUE(seed.refresh(1));

    for (int round = 0; round < 5; ++round) {
        pid_t child = fork();
        CHECK_TRUE(child >= 0);
        if (child == 0) {
            PriceCatalog writer(cachePath, "u", fixedFetch(page));
            for (int64_t t = 2;; ++t) {
                writer.refresh(t);
            }
        }
        usleep(40000 + round * 13000);
        kill(child, SIGKILL);
        int st = 0;
        waitpid(child, &st, 0);

        PriceCatalog reader(cachePath, "u", fixedFetch(""));
        CHECK_TRUE(reader.loadCache());
        LONGS_EQUAL(19, static_cast<long>(reader.snapshot().size()));
    }
}

//------------------------------------------------------------------------
// Live mode and entry point
//------------------------------------------------------------------------

static int runLive(const std::string& outPath, const std::string& pageOutPath) {
    PriceCatalog::FetchFn fetch = PriceCatalog::httpFetcher();
    PriceFetchResult res = fetch(PriceCatalog::kDefaultUrl);
    if (res.status != 200 || !res.error.empty()) {
        std::cerr << "live fetch failed: status " << res.status << " " << res.error << std::endl;
        return 1;
    }
    PriceParseResult parsed = PriceCatalog::parsePage(res.body);
    if (!parsed.ok) {
        std::cerr << "live page rejected: " << parsed.error << std::endl;
        return 1;
    }
    writeFile(pageOutPath, res.body);
    std::ofstream out(outPath, std::ios::trunc);
    auto emit = [&](const std::string& model, const char* field, int64_t value) {
        nlohmann::json row;
        row["model"] = model;
        row["field"] = field;
        row["cpp"] = value;
        out << row.dump() << "\n";
    };
    auto emitSet = [&](const std::string& model, const std::string& prefix, const ModelPrice& m) {
        emit(model, (prefix + "input_nano").c_str(), m.inputNano);
        emit(model, (prefix + "write5m_nano").c_str(), m.write5mNano);
        emit(model, (prefix + "write1h_nano").c_str(), m.write1hNano);
        emit(model, (prefix + "read_nano").c_str(), m.readNano);
        emit(model, (prefix + "output_nano").c_str(), m.outputNano);
    };
    for (const auto& kv : parsed.models) {
        const ModelPrice& m = kv.second;
        emitSet(kv.first, "", m);
        if (m.hasFast) {
            emit(kv.first, "fast_input_nano", m.fastInputNano);
            emit(kv.first, "fast_write5m_nano", m.fastWrite5mNano);
            emit(kv.first, "fast_write1h_nano", m.fastWrite1hNano);
            emit(kv.first, "fast_read_nano", m.fastReadNano);
            emit(kv.first, "fast_output_nano", m.fastOutputNano);
        }
        if (m.upperTier) {
            emit(kv.first, "tier_threshold_tokens", m.tierThresholdTokens);
            emitSet(kv.first, "upper_", *m.upperTier);
        }
    }
    for (const auto& q : parsed.quarantined) {
        emit(q.modelId, "quarantined", 1);
    }
    std::cout << "live: " << parsed.models.size() << " models, page sha256 "
              << PriceCatalog::sha256Hex(res.body) << std::endl;
    return 0;
}

static int runSeedCache(const std::string& cachePath, const std::string& pagePath, int64_t now) {
    std::string page = readFile(pagePath);
    PriceCatalog catalog(cachePath, "https://seeded.invalid/pricing.md", fixedFetch(page));
    if (!catalog.refresh(now)) {
        std::cerr << "seed failed: " << catalog.status(now, 24).error << std::endl;
        return 1;
    }
    std::cout << "seeded " << catalog.snapshot().size() << " models into the cache" << std::endl;
    return 0;
}

int main(int ac, char** av) {
    bool live = false;
    std::string seedCache;
    std::string seedPage;
    int64_t seedNow = 0;
    std::string outPath;
    std::string pageOutPath;
    std::vector<const char*> passthrough;
    for (int i = 0; i < ac; ++i) {
        std::string arg = av[i];
        if (arg == "--expect" && i + 1 < ac) {
            g_expectPath = av[++i];
        } else if (arg == "--live") {
            live = true;
        } else if (arg == "--out" && i + 1 < ac) {
            outPath = av[++i];
        } else if (arg == "--page-out" && i + 1 < ac) {
            pageOutPath = av[++i];
        } else if (arg == "--seed-cache" && i + 1 < ac) {
            seedCache = av[++i];
        } else if (arg == "--page" && i + 1 < ac) {
            seedPage = av[++i];
        } else if (arg == "--now" && i + 1 < ac) {
            seedNow = std::atoll(av[++i]);
        } else {
            passthrough.push_back(av[i]);
        }
    }
    if (!seedCache.empty()) {
        if (seedPage.empty() || seedNow <= 0) {
            std::cerr << "--seed-cache requires --page and --now" << std::endl;
            return 2;
        }
        return runSeedCache(seedCache, seedPage, seedNow);
    }
    if (live) {
        if (outPath.empty() || pageOutPath.empty()) {
            std::cerr << "--live requires --out and --page-out" << std::endl;
            return 2;
        }
        return runLive(outPath, pageOutPath);
    }
    return CommandLineTestRunner::RunAllTests(static_cast<int>(passthrough.size()), passthrough.data());
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
