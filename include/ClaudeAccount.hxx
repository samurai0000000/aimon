/*
 * ClaudeAccount.hxx
 *
 * Detects the subscription tier of a Claude Code configuration directory from
 * the two descriptive strings in its credentials file. Token values in that
 * file are never read into a result, logged, or written anywhere.
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_CLAUDE_ACCOUNT_HXX
#define AIMON_CLAUDE_ACCOUNT_HXX

#include <string>

#include "Models.hxx"

namespace aimon {

struct ClaudeTierInfo {
    ClaudeTier tier = ClaudeTier::Unknown;
    std::string rawSubscriptionType;
    std::string rawRateLimitTier;
    std::string warning;
};

// Reads <configDir>/.credentials.json (configDir may start with "~"). A
// missing, unreadable, malformed, oversized (> 64 KiB) or group/world
// readable file yields tier Unknown with a warning. "enterprise" (any case)
// maps to Enterprise; any other non-empty subscription type maps to Personal
// and keeps its raw string.
ClaudeTierInfo detectClaudeTier(const std::string& configDir);

} // namespace aimon

#endif // AIMON_CLAUDE_ACCOUNT_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
