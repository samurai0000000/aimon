/*
 * ClaudeAccount.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "ClaudeAccount.hxx"
#include "PathUtils.hxx"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include <nlohmann/json.hpp>

namespace aimon {

namespace {

const size_t kMaxCredentialsBytes = 64 * 1024;
const size_t kMaxTierStringBytes = 128;

bool printableAscii(const std::string& s) {
    if (s.empty() || s.size() > kMaxTierStringBytes) {
        return false;
    }
    for (unsigned char c : s) {
        if (c < 0x20 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

// Extracts a descriptive string; an absent key yields an empty string and a
// present key of the wrong type or with unusual characters is an error.
bool readTierString(const nlohmann::json& obj, const char* key, std::string& out) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        out.clear();
        return true;
    }
    if (!it->is_string()) {
        return false;
    }
    std::string value = it->get<std::string>();
    if (value.empty()) {
        out.clear();
        return true;
    }
    if (!printableAscii(value)) {
        return false;
    }
    out = value;
    return true;
}

} // namespace

ClaudeTierInfo detectClaudeTier(const std::string& configDir) {
    ClaudeTierInfo info;
    const std::string path = PathUtils::expandHome(configDir) + "/.credentials.json";

    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        info.warning = (errno == ENOENT) ? "credentials file not found"
                                         : std::string("credentials file unreadable: ") + std::strerror(errno);
        return info;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        info.warning = "credentials path is not a regular file";
        return info;
    }
    if ((st.st_mode & 077) != 0) {
        ::close(fd);
        info.warning = "credentials file is readable by group or others; run chmod 600 on it";
        return info;
    }
    if (static_cast<size_t>(st.st_size) > kMaxCredentialsBytes) {
        ::close(fd);
        info.warning = "credentials file is too large";
        return info;
    }

    std::string text(static_cast<size_t>(st.st_size), '\0');
    size_t got = 0;
    while (got < text.size()) {
        ssize_t n = ::read(fd, &text[got], text.size() - got);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        got += static_cast<size_t>(n);
    }
    ::close(fd);
    text.resize(got);

    {
        nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
        std::fill(text.begin(), text.end(), '\0');     // the buffer held token values
        if (doc.is_discarded() || !doc.is_object()) {
            info.warning = "credentials file is not valid JSON";
            return info;
        }
        auto oauth = doc.find("claudeAiOauth");
        if (oauth == doc.end() || !oauth->is_object()) {
            info.warning = "credentials file has an unrecognized shape";
            return info;
        }
        std::string subscription;
        std::string rateTier;
        if (!readTierString(*oauth, "subscriptionType", subscription) ||
            !readTierString(*oauth, "rateLimitTier", rateTier)) {
            info.warning = "credentials tier fields are not plain text";
            return info;
        }
        if (subscription.empty()) {
            info.warning = "credentials file has no subscription type";
            return info;
        }
        info.rawSubscriptionType = subscription;
        info.rawRateLimitTier = rateTier;
        std::string lowered = subscription;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        info.tier = (lowered == "enterprise") ? ClaudeTier::Enterprise : ClaudeTier::Personal;
    }
    return info;
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
