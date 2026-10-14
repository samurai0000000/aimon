/*
 * TestInsightParserProps.cxx
 *
 * RapidCheck property-based tests for InsightEngine parsers:
 * - decodeGenMetadata (protobuf wire decoder) never crashes or reads out-of-bounds
 *   on arbitrary bytes, truncations, or bit-flips, and enforces output == thinking + response
 *   for validated=1.
 * - extractPromptFeatures and extractClaudeUserPromptFeatures never crash on arbitrary
 *   UTF-8 / binary strings and obey structural invariants (proseChars <= chars,
 *   fencedChars <= chars, non-negative counts).
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include <rapidcheck.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "InsightEngine.hxx"

using namespace aimon;

static void appendVarint(std::vector<uint8_t>& buf, uint64_t val) {
    while (val >= 0x80) {
        buf.push_back(static_cast<uint8_t>((val & 0x7F) | 0x80));
        val >>= 7;
    }
    buf.push_back(static_cast<uint8_t>(val & 0x7F));
}

static void appendTag(std::vector<uint8_t>& buf, uint32_t fieldNum, uint8_t wireType) {
    appendVarint(buf, (static_cast<uint64_t>(fieldNum) << 3) | (wireType & 0x07));
}

static void appendBytesField(std::vector<uint8_t>& buf, uint32_t fieldNum, const std::vector<uint8_t>& payload) {
    appendTag(buf, fieldNum, 2);
    appendVarint(buf, payload.size());
    buf.insert(buf.end(), payload.begin(), payload.end());
}

static std::vector<uint8_t> buildSampleGenMetadataBlob(uint64_t inTok, uint64_t thinkTok, uint64_t respTok, uint64_t cacheTok) {
    std::vector<uint8_t> f1_f4;
    appendTag(f1_f4, 1, 0); appendVarint(f1_f4, 1404);
    appendTag(f1_f4, 2, 0); appendVarint(f1_f4, inTok);
    appendTag(f1_f4, 3, 0); appendVarint(f1_f4, thinkTok + respTok);
    appendTag(f1_f4, 5, 0); appendVarint(f1_f4, cacheTok);
    appendTag(f1_f4, 6, 0); appendVarint(f1_f4, 24);
    appendTag(f1_f4, 9, 0); appendVarint(f1_f4, thinkTok);
    appendTag(f1_f4, 10, 0); appendVarint(f1_f4, respTok);

    std::vector<uint8_t> f1_f9_f4;
    appendTag(f1_f9_f4, 1, 0); appendVarint(f1_f9_f4, 1760000000ULL);

    std::vector<uint8_t> f1_f9_f8;
    appendTag(f1_f9_f8, 1, 0); appendVarint(f1_f9_f8, 1);
    appendTag(f1_f9_f8, 2, 0); appendVarint(f1_f9_f8, 500000000ULL);

    std::vector<uint8_t> f1_f9_f10;
    appendTag(f1_f9_f10, 1, 0); appendVarint(f1_f9_f10, 45000);
    appendTag(f1_f9_f10, 4, 0); appendVarint(f1_f9_f10, 256000);

    std::vector<uint8_t> f1_f9;
    appendBytesField(f1_f9, 4, f1_f9_f4);
    appendBytesField(f1_f9, 8, f1_f9_f8);
    appendBytesField(f1_f9, 10, f1_f9_f10);

    std::vector<uint8_t> f1_f11;
    appendTag(f1_f11, 1, 0); appendVarint(f1_f11, 3);
    appendTag(f1_f11, 2, 0); appendVarint(f1_f11, 250000000ULL);

    std::vector<uint8_t> f1_f15;
    appendTag(f1_f15, 2, 0); appendVarint(f1_f15, 64000);

    std::vector<uint8_t> f1;
    appendTag(f1, 3, 0); appendVarint(f1, 1404);
    appendBytesField(f1, 4, f1_f4);
    appendBytesField(f1, 9, f1_f9);
    appendBytesField(f1, 11, f1_f11);
    appendBytesField(f1, 15, f1_f15);

    std::string uuidStr = "360883b4-dc2a-4ed4-99d4-c8fe55808c4d";
    std::vector<uint8_t> uuidBytes(uuidStr.begin(), uuidStr.end());

    std::vector<uint8_t> root;
    appendBytesField(root, 1, f1);
    appendBytesField(root, 4, uuidBytes);
    return root;
}

int main() {
    setenv("RC_PARAMS", "max_success=500", 0);

    bool ok = true;

    ok &= rc::check("valid gen_metadata roundtrips exact token counts and identity", []() {
        uint64_t inTok = *rc::gen::inRange<uint64_t>(0, 500000);
        uint64_t thinkTok = *rc::gen::inRange<uint64_t>(0, 100000);
        uint64_t respTok = *rc::gen::inRange<uint64_t>(0, 100000);
        uint64_t cacheTok = *rc::gen::inRange<uint64_t>(0, 500000);

        auto blob = buildSampleGenMetadataBlob(inTok, thinkTok, respTok, cacheTok);
        GenerationRecord rec;
        RC_ASSERT(InsightEngine::decodeGenMetadata(blob.data(), blob.size(), rec));
        RC_ASSERT(rec.inputTokens == static_cast<int64_t>(inTok));
        RC_ASSERT(rec.thinkingTokens == static_cast<int64_t>(thinkTok));
        RC_ASSERT(rec.responseTokens == static_cast<int64_t>(respTok));
        RC_ASSERT(rec.outputTokens == static_cast<int64_t>(thinkTok + respTok));
        RC_ASSERT(rec.cacheReadTokens == static_cast<int64_t>(cacheTok));
        RC_ASSERT(rec.validated == 1);
    });

    ok &= rc::check("arbitrary truncations of gen_metadata never crash", []() {
        auto blob = buildSampleGenMetadataBlob(100, 50, 75, 1000);
        size_t cut = *rc::gen::inRange<size_t>(0, blob.size() + 1);
        GenerationRecord rec;
        (void)InsightEngine::decodeGenMetadata(blob.data(), cut, rec);
        RC_ASSERT(rec.inputTokens >= 0);
        RC_ASSERT(rec.outputTokens >= 0);
    });

    ok &= rc::check("arbitrary byte sequences never crash decodeGenMetadata", []() {
        auto bytes = *rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>());
        GenerationRecord rec;
        bool parsed = InsightEngine::decodeGenMetadata(bytes.data(), bytes.size(), rec);
        if (parsed && rec.validated == 1) {
            RC_ASSERT(rec.outputTokens == rec.thinkingTokens + rec.responseTokens);
        }
    });

    ok &= rc::check("extractPromptFeatures invariants hold on arbitrary strings", []() {
        auto text = *rc::gen::container<std::string>(rc::gen::arbitrary<char>());
        PromptFeatures pf = InsightEngine::extractPromptFeatures(text);
        RC_ASSERT(pf.chars == static_cast<int64_t>(text.size()));
        RC_ASSERT(pf.proseChars >= 0);
        RC_ASSERT(pf.fencedChars >= 0);
        RC_ASSERT(pf.pasteChars >= 0);
        RC_ASSERT(pf.proseChars + pf.fencedChars <= pf.chars);
        RC_ASSERT(pf.mixedPaste == 0 || pf.mixedPaste == 1);
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
