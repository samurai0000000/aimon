/*
 * TestPriceCatalogProps.cxx
 *
 * RapidCheck properties for the pricing page parser: arbitrary truncations
 * and byte flips of the real page never crash it and never yield an accepted
 * catalog that violates the validation invariants; parsing is deterministic;
 * price cells round-trip; model id resolution never mis-prices.
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <rapidcheck.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "PriceCatalog.hxx"

using namespace aimon;

#ifndef AIMON_SOURCE_DIR
#error "AIMON_SOURCE_DIR must be defined by the build"
#endif

static std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static int64_t absDiff(int64_t a, int64_t b) {
    return a > b ? a - b : b - a;
}

static int64_t roundHalfUp(int64_t a, int64_t b, int64_t c) {
    return (a * b + c / 2) / c;
}

static bool validSet(const ModelPrice& m) {
    if (m.inputNano <= 0 || m.write5mNano <= 0 || m.write1hNano <= 0 || m.readNano <= 0 || m.outputNano <= 0) {
        return false;
    }
    if (absDiff(4 * m.write5mNano, 5 * m.inputNano) * 100 > 5 * m.inputNano) {
        return false;
    }
    if (absDiff(m.write1hNano, 2 * m.inputNano) * 100 > 2 * m.inputNano) {
        return false;
    }
    return m.readNano <= m.inputNano;
}

// An accepted catalog must satisfy every validation rule; a rejected one must
// carry an error and no models.
static bool consistent(const PriceParseResult& r) {
    if (!r.ok) {
        return r.models.empty() && !r.error.empty();
    }
    if (r.models.size() < 3) {
        return false;
    }
    for (const auto& q : r.quarantined) {
        if (r.models.count(q.modelId) != 0 || q.reason.empty()) {
            return false;
        }
    }
    for (const auto& kv : r.models) {
        const ModelPrice& m = kv.second;
        if (m.modelId != kv.first || kv.first.rfind("claude-", 0) != 0 || !validSet(m)) {
            return false;
        }
        if (m.upperTier) {
            if (m.tierThresholdTokens <= 0 || !validSet(*m.upperTier) || m.upperTier->hasFast ||
                m.upperTier->upperTier != nullptr) {
                return false;
            }
        } else if (m.tierThresholdTokens != 0) {
            return false;
        }
        if (m.hasFast) {
            if (m.fastInputNano <= 0 || m.fastOutputNano <= 0) {
                return false;
            }
            if (m.fastWrite5mNano != roundHalfUp(m.fastInputNano, m.write5mNano, m.inputNano) ||
                m.fastWrite1hNano != roundHalfUp(m.fastInputNano, m.write1hNano, m.inputNano) ||
                m.fastReadNano != roundHalfUp(m.fastInputNano, m.readNano, m.inputNano)) {
                return false;
            }
        }
    }
    return true;
}

static std::string mutate(const std::string& page, size_t limit) {
    std::string mutated = page;
    const int flips = *rc::gen::inRange(1, 9);
    for (int i = 0; i < flips; ++i) {
        const size_t pos = *rc::gen::inRange<size_t>(0, limit);
        mutated[pos] = static_cast<char>(*rc::gen::arbitrary<uint8_t>());
    }
    return mutated;
}

int main() {
    // More cases than RapidCheck's default of 100; an explicit RC_PARAMS wins.
    setenv("RC_PARAMS", "max_success=1000", 0);
    const std::string base = std::string(AIMON_SOURCE_DIR) + "/test/fixtures/";
    const std::vector<std::pair<std::string, std::string>> pages = {
        {"v1", readFile(base + "pricing_page.md")}, {"v2 (tiered Haiku)", readFile(base + "pricing_page_v2.md")}};
    bool ok = true;
    for (const auto& entry : pages) {
        const std::string& page = entry.second;
        const std::string label = " [page " + entry.first + "]";
        if (page.size() < 40000) {
            std::cerr << "fixture page missing or too small: " << entry.first << std::endl;
            return 2;
        }
        size_t tableLimit = page.find("### Fast mode pricing");
        if (tableLimit == std::string::npos) {
            std::cerr << "fixture page has no fast mode section: " << entry.first << std::endl;
            return 2;
        }
        tableLimit += 700;  // covers both tables

        ok &= rc::check(std::string("any prefix of the real page parses to a consistent result") + label, [&]() {
            const size_t n = *rc::gen::inRange<size_t>(0, page.size() + 1);
            RC_ASSERT(consistent(PriceCatalog::parsePage(page.substr(0, n))));
        });

        ok &= rc::check(std::string("byte flips anywhere in the page never break consistency") + label, [&]() {
            RC_ASSERT(consistent(PriceCatalog::parsePage(mutate(page, page.size()))));
        });

        ok &= rc::check(std::string("byte flips inside the two price tables never break consistency") + label, [&]() {
            RC_ASSERT(consistent(PriceCatalog::parsePage(mutate(page, tableLimit))));
        });

        ok &= rc::check(std::string("parsing is deterministic on mutated input") + label, [&]() {
            const std::string text = mutate(page, tableLimit);
            const PriceParseResult a = PriceCatalog::parsePage(text);
            const PriceParseResult b = PriceCatalog::parsePage(text);
            RC_ASSERT(a.ok == b.ok);
            RC_ASSERT(a.error == b.error);
            RC_ASSERT(a.models == b.models);
        });

    }

    ok &= rc::check("arbitrary bytes never crash the parser", [&]() {
        const auto text = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());
        RC_ASSERT(consistent(PriceCatalog::parsePage(text)));
    });

    ok &= rc::check("valid price cells round-trip to nano-dollars", [&]() {
        const int64_t whole = *rc::gen::inRange<int64_t>(0, 1000000);
        const int64_t frac = *rc::gen::inRange<int64_t>(0, 1000);
        const bool footnote = *rc::gen::arbitrary<bool>();
        std::string digits = std::to_string(1000 + frac).substr(1);  // zero-padded to 3
        while (!digits.empty() && digits.back() == '0') {
            digits.pop_back();
        }
        std::string cell = "$" + std::to_string(whole) + (digits.empty() ? "" : "." + digits) + " / MTok";
        if (footnote) {
            cell += "<sup>3</sup>";
        }
        int64_t nano = -1;
        RC_ASSERT(PriceCatalog::parsePriceCell(cell, nano));
        RC_ASSERT(nano == whole * 1000 + frac);
    });

    ok &= rc::check("arbitrary price cells never crash and stay in range", [&]() {
        const auto text = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());
        int64_t nano = -1;
        if (PriceCatalog::parsePriceCell(text, nano)) {
            RC_ASSERT(nano >= 0 && nano <= 999999999);
        }
    });

    ok &= rc::check("model id resolution only accepts exact or dated ids", [&]() {
        const PriceParseResult r = PriceCatalog::parsePage(pages[1].second);
        RC_ASSERT(r.ok);
        const auto text = *rc::gen::container<std::string>(rc::gen::elementOf(
            std::string("abcdefghijklmnopqrstuvwxyz0123456789-<>")));
        const std::string base = (*rc::gen::elementOf(
            std::vector<std::string>{"claude-opus-5", "claude-opus-5-5", "claude-sonnet-5", "claude-haiku-4-5", ""}));
        const std::string probe = base + (*rc::gen::arbitrary<bool>() ? "-" : "") + text;
        const std::string got = PriceCatalog::resolveModelId(r.models, probe);
        if (!got.empty()) {
            RC_ASSERT(r.models.count(got) == 1);
            const bool exact = probe == got;
            const bool dated = probe.size() == got.size() + 9 && probe.compare(0, got.size(), got) == 0 &&
                               probe[got.size()] == '-' &&
                               probe.find_first_not_of("0123456789", got.size() + 1) == std::string::npos;
            RC_ASSERT(exact || dated);
        }
    });

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
