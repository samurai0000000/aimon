# Plan: Display Google One AI Credits & Prioritize Newest Language Server in `aimon`

## 1. Motivation & Problem Statement
When a user upgrades their Google AI subscription (e.g., to Google AI Ultra), `aimon`'s `check_antigravity_quota` and `get_combined_ai_status` MCP tools exhibited two limitations:
1. **Missing Credit Balances**: The RPC response from the IDE's Language Server (`/exa.language_server_pb.LanguageServerService/GetUserStatus`) contains explicit credit fields:
   - `availableCredits`: e.g. `[{creditType: "GOOGLE_ONE_AI", creditAmount: "3024", minimumCreditAmountForUsage: "50"}]`
   - `planStatus`: e.g. `availablePromptCredits: 500, availableFlowCredits: 100, monthlyPromptCredits: 50000, monthlyFlowCredits: 150000`
   Currently, `aimon`'s `AntigravityCollector` only parsed `userTier["name"]` ("Google AI Ultra") and rate-limit buckets from `RetrieveUserQuotaSummary`, completely omitting the raw credit balances from tool output.
2. **Indefinite Pinning to Stale Language Server PIDs**:
   - Multiple VS Code / Antigravity IDE workspaces create multiple `language_server_linux_x64` instances with differing PIDs and lifetimes.
   - `AntigravityCollector` caches `_cachedPort` and `_cachedCsrf` indefinitely as long as `probePort` returns true on the old port, which can delay reflecting newly refreshed quota from an upgraded session or active workspace.
   - The `/proc` scanner iterates in arbitrary filesystem order rather than sorting PIDs descending to prioritize the newest running instance.

---

## 2. Architectural Decisions & Scope

### Decision 1: Model Enhancements (`include/Models.hxx`)
- Add `UserCredit` struct:
  ```cpp
  struct UserCredit {
      std::string creditType;
      int creditAmount = 0;
      int minimumCreditAmountForUsage = 0;
      nlohmann::json toJson() const;
  };
  ```
- Enhance `AntigravityStatus`:
  - `std::vector<UserCredit> availableCredits;`
  - `int availablePromptCredits = 0;`
  - `int availableFlowCredits = 0;`
  - `int monthlyPromptCredits = 0;`
  - `int monthlyFlowCredits = 0;`
  - Update `toJson()` to serialize `available_credits`, `available_prompt_credits`, `available_flow_credits`, `monthly_prompt_credits`, and `monthly_flow_credits`.

### Decision 2: Parsing in `AntigravityCollector.cxx`
- In `fetchStatus()`:
  - Parse `userStatus["availableCredits"]` (or `userTier["availableCredits"]`). Extract `creditType` (e.g., `GOOGLE_ONE_AI` -> format as `Google One AI`), `creditAmount`, and `minimumCreditAmountForUsage`.
  - Parse `userStatus["planStatus"]`: Extract `availablePromptCredits`, `availableFlowCredits`, `monthlyPromptCredits`, and `monthlyFlowCredits`.

### Decision 3: Process Discovery Optimization (`AntigravityCollector.cxx`)
- Collect all valid `language_server` PIDs from `/proc` into a list and sort in descending order (highest/newest PID first).
- Probe candidate PIDs starting from the newest PID.
- In `discoverProcess()`: If multiple language server instances are alive, periodically or when quota buckets show 0%, re-evaluate candidates to ensure the active/freshest instance is used.

### Decision 4: Output Formatting in `McpServer.cxx`
- In `formatAntigravityStatus(const AntigravityStatus& ag)`:
  - Display plan tier: `- **Plan Tier**: Google AI Ultra`
  - Display available credits:
    - If `availableCredits` is not empty:
      - e.g. `- **Available Credits**: **3,024** Google One AI credits`
  - Display prompt/flow credits if available:
    - e.g. `- **Prompt / Flow Credits**: 500 / 100 available (Monthly: 50,000 / 150,000)`
  - Retain clean table of quota groups (Gemini Models, Claude and GPT models).

### Decision 5: Non-Disruption Guarantee
- The currently running `aimon` process inside screen `aimon` will NOT be restarted, killed, or disrupted during development and testing.
- We will compile the updated binary on `builder` and test it via standalone execution (`./build/aimon mcp` stdio check).
- Redeployment into the active screen session `aimon` will strictly wait for the user's explicit `deploy` instruction.

---

## 3. Staged Implementation Plan

### Stage 1: Data Model Updates
- **[include/Models.hxx](file:///home/samurai/work/aimon/include/Models.hxx)**: Add `UserCredit` struct and credit balance fields to `AntigravityStatus`.

### Stage 2: Collector Enhancements
- **[src/AntigravityCollector.cxx](file:///home/samurai/work/aimon/src/AntigravityCollector.cxx)**:
  - Implement sorting of `/proc` PIDs in descending order to target the newest language server.
  - Parse `availableCredits` and `planStatus` credits from `userStatus`.

### Stage 3: MCP Output Formatting
- **[src/McpServer.cxx](file:///home/samurai/work/aimon/src/McpServer.cxx)**:
  - Update `formatAntigravityStatus()` to format credit balances cleanly in markdown.

### Stage 4: Compilation & Standalone Verification
- Compile on `builder`: `make -j$(nproc)` using top-level `Makefile`.
- Verify with standalone stdio command:
  `echo '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"check_antigravity_quota"}}' | ./build/aimon mcp --profile=core --gateway-disable`
- Verify that the output includes the 3,024 Google One AI credits and 100% quota cleanly.
- Keep the running `aimon` screen session untouched.
