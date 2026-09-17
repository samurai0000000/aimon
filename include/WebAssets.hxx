/*
 * WebAssets.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_WEB_ASSETS_HXX
#define AIMON_WEB_ASSETS_HXX

namespace aimon {
namespace assets {

inline const char* INDEX_HTML = R"raw_asset(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>aimon | Unified AI Quota Monitor</title>
    <link rel="stylesheet" href="style.css">
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&family=JetBrains+Mono:wght@400;500;600&display=swap" rel="stylesheet">
</head>
<body>
    <div class="app-container">
        <header class="navbar">
            <div class="brand">
                <div class="logo-pulse"></div>
                <h1>aimon <span class="badge-sub">Unified AI Quotas</span></h1>
            </div>
            <div class="actions">
                <span id="last-updated" class="last-updated">Updated: Just now</span>
                <button id="refresh-btn" class="btn-refresh" title="Force Refresh">
                    <svg viewBox="0 0 24 24" width="16" height="16" stroke="currentColor" stroke-width="2" fill="none">
                        <path d="M23 4v6h-6"></path>
                        <path d="M1 20v-6h6"></path>
                        <path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"></path>
                    </svg>
                    Refresh
                </button>
            </div>
        </header>

        <main class="dashboard-grid">
            <!-- Google Antigravity Section -->
            <section class="card glass-card antigravity-card">
                <div class="card-header">
                    <div class="provider-title">
                        <span class="dot-status dot-online" id="ag-status-dot"></span>
                        <h2>Google Antigravity</h2>
                    </div>
                    <span id="ag-plan-badge" class="badge badge-cyan">Google AI Ultra</span>
                </div>


                <!-- Quota Groups (Gemini Models, Claude & GPT models) -->
                <div class="quota-groups-section">
                    <div class="section-title-row">
                        <h3>Model Quota Limits</h3>
                    </div>
                    <div id="ag-quota-groups" class="quota-groups-list">
                        <div class="gauge-loading">Scanning quota groups...</div>
                    </div>
                </div>

                <!-- Collapsible Individual Models -->
                <div class="models-collapsible-section">
                    <button type="button" id="ag-models-toggle" class="btn-toggle-models">
                        <span id="ag-models-toggle-text">Individual Model Capacities</span>
                        <svg class="toggle-icon" id="ag-models-chevron" viewBox="0 0 24 24" width="14" height="14" stroke="currentColor" stroke-width="2" fill="none">
                            <path d="M6 9l6 6 6-6"></path>
                        </svg>
                    </button>
                    <div id="ag-models-grid" class="gauges-grid hidden">
                        <!-- Dynamically populated model gauges -->
                    </div>
                </div>

                <div id="ag-error" class="error-banner hidden"></div>
            </section>

            <!-- Cursor Section -->
            <section class="card glass-card cursor-card">
                <div class="card-header">
                    <div class="provider-title">
                        <span class="dot-status dot-online" id="cursor-status-dot"></span>
                        <h2>Cursor</h2>
                    </div>
                    <span id="cursor-plan-badge" class="badge badge-magenta">Pro</span>
                </div>

                <div class="cursor-metrics">
                    <div class="cursor-top-row">
                        <div class="usage-stat-box">
                            <div class="stat-top">
                                <span class="stat-label">Fast Requests Pool</span>
                                <span id="cursor-fast-ratio" class="stat-ratio">-- / --</span>
                            </div>
                            <div class="progress-track">
                                <div id="cursor-progress-bar" class="progress-fill" style="width: 0%;"></div>
                            </div>
                            <div class="stat-bottom">
                                <span id="cursor-remaining-txt" class="stat-sub">Calculating...</span>
                                <span id="cursor-percent-txt" class="stat-pct">0%</span>
                            </div>
                        </div>

                        <div class="details-box">
                            <div class="detail-row">
                                <span class="detail-label">Billing Cycle Reset</span>
                                <span id="cursor-reset-date" class="detail-value">--</span>
                            </div>
                            <div class="detail-row">
                                <span class="detail-label">Days Remaining</span>
                                <span id="cursor-days-remaining" class="detail-value">--</span>
                            </div>
                        </div>
                    </div>

                    <!-- Personal Usage & Cumulative Spend Card -->
                    <div class="spend-section" id="cursor-spend-section">
                        <div class="spend-header">
                            <div>
                                <span class="spend-subtitle">Personal Usage</span>
                                <h3 class="spend-title">Cumulative Spend</h3>
                                <p class="spend-desc">Track your spend against last month.</p>
                            </div>
                            <div class="spend-totals">
                                <span id="cursor-total-spend" class="spend-amount">$0.00</span>
                                <span id="cursor-prev-spend-pill" class="spend-prev-pill hidden">vs $0.00 last month</span>
                            </div>
                        </div>

                        <div class="spend-content-grid">
                            <!-- Chart Container -->
                            <div class="spend-chart-container">
                                <svg id="cursor-spend-chart" class="spend-chart-svg" viewBox="0 0 420 160">
                                    <defs>
                                        <linearGradient id="spendGrad" x1="0" y1="0" x2="0" y2="1">
                                            <stop offset="0%" stop-color="#3b82f6" stop-opacity="0.45"/>
                                            <stop offset="100%" stop-color="#3b82f6" stop-opacity="0.0"/>
                                        </linearGradient>
                                    </defs>
                                    <!-- Grid Lines & Axis -->
                                    <line x1="50" y1="20" x2="410" y2="20" class="chart-grid-line" />
                                    <text x="42" y="24" class="chart-axis-lbl" id="chart-lbl-max">$400</text>

                                    <line x1="50" y1="58" x2="410" y2="58" class="chart-grid-line" />
                                    <text x="42" y="62" class="chart-axis-lbl" id="chart-lbl-mid2">$300</text>

                                    <line x1="50" y1="96" x2="410" y2="96" class="chart-grid-line" />
                                    <text x="42" y="100" class="chart-axis-lbl" id="chart-lbl-mid1">$200</text>

                                    <line x1="50" y1="135" x2="410" y2="135" class="chart-grid-line" />
                                    <text x="42" y="139" class="chart-axis-lbl">$0</text>

                                    <!-- Gradient Area -->
                                    <path id="spend-chart-area" d="" fill="url(#spendGrad)"></path>
                                    <!-- Trend Line -->
                                    <path id="spend-chart-line" d="" fill="none" stroke="#3b82f6" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"></path>
                                    <!-- Interactive Dots -->
                                    <g id="spend-chart-dots"></g>
                                </svg>
                                <div id="chart-dates-axis" class="chart-dates-row">
                                    <!-- Day labels populated dynamically -->
                                </div>
                            </div>

                            <!-- Spend by Model Breakdown -->
                            <div class="categories-breakdown" id="cursor-categories-breakdown">
                                <div class="breakdown-header">Spend by Model</div>
                                <div id="cursor-categories-list" class="categories-list scrollable-categories">
                                    <!-- Dynamically populated category bars -->
                                </div>
                            </div>
                        </div>
                    </div>
                </div>

                <div id="cursor-error" class="error-banner hidden"></div>
            </section>

            <!-- Active Agent Fleet Section -->
            <section class="card glass-card agents-fleet-card">
                <div class="card-header">
                    <div class="provider-title">
                        <span class="dot-status dot-online" id="agents-status-dot"></span>
                        <h2>Active Agent Fleet</h2>
                    </div>
                    <div class="agents-header-actions">
                        <label class="toggle-switch-label" title="Show completed or failed tasks">
                            <input type="checkbox" id="toggle-completed-tasks">
                            <span class="toggle-switch-slider"></span>
                            <span class="toggle-switch-text">Show Completed</span>
                        </label>
                        <span id="agents-count-badge" class="badge badge-cyan">0 Active</span>
                    </div>
                </div>

                <!-- Live Connected MCP Clients Bar -->
                <div class="mcp-clients-bar" id="mcp-clients-bar">
                    <div class="mcp-clients-left">
                        <span class="mcp-clients-title">
                            <svg viewBox="0 0 24 24" width="14" height="14" stroke="currentColor" stroke-width="2" fill="none">
                                <circle cx="12" cy="12" r="10"></circle>
                                <line x1="2" y1="12" x2="22" y2="12"></line>
                                <path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"></path>
                            </svg>
                            Connected IDEs:
                        </span>
                    </div>
                    <div class="mcp-clients-list" id="mcp-clients-list">
                        <span class="client-badge client-badge-empty">
                            <span class="dot-status dot-offline"></span>
                            No active MCP connections
                        </span>
                    </div>
                </div>

                <div class="table-responsive" id="agents-table-container">
                    <table class="agents-table" id="agents-table">
                        <thead>
                            <tr>
                                <th style="width: 130px;">Status</th>
                                <th style="width: 150px;">Agent</th>
                                <th>Task / Goal</th>
                                <th style="width: 200px;">Active Action</th>
                                <th style="width: 100px;">Duration</th>
                                <th style="width: 100px;">Last Seen</th>
                            </tr>
                        </thead>
                        <tbody id="agents-table-body">
                            <!-- Populated dynamically by app.js -->
                        </tbody>
                    </table>
                </div>

                <div id="agents-empty-state" class="agents-empty-state">
                    <div class="empty-icon">🤖</div>
                    <p class="empty-title">No AI agents currently active</p>
                    <p class="empty-desc">Agents reporting via MCP (<code>register_agent_task</code>) or HTTP will appear here in real time.</p>
                </div>
            </section>

            <!-- Execution Runs & Interlock Section -->
            <section class="card glass-card exec-runs-card" id="exec-runs-section">
                <div class="card-header">
                    <div class="provider-title">
                        <span class="dot-status dot-online" id="exec-status-dot"></span>
                        <h2>Execution Runs &amp; Interlock</h2>
                    </div>
                    <div class="exec-header-actions">
                        <span id="exec-run-badge" class="badge">No Active Run</span>
                    </div>
                </div>

                <!-- Pending Interlock Banner -->
                <div id="interlock-banner" class="interlock-banner hidden">
                    <div class="interlock-header">
                        <div class="interlock-icon-title">
                            <span class="interlock-icon">⚠️</span>
                            <div>
                                <h3 class="interlock-title">Operator Approval Required</h3>
                                <div class="interlock-sub" id="interlock-sub">Checkpoint action awaiting human sign-off</div>
                            </div>
                        </div>
                        <span class="badge badge-amber" id="interlock-id-badge">intk-...</span>
                    </div>

                    <div class="interlock-body">
                        <div class="interlock-box">
                            <span class="interlock-label">Description / Reason</span>
                            <div class="interlock-val" id="interlock-desc-text">--</div>
                        </div>
                        <div class="interlock-box" id="interlock-action-box">
                            <span class="interlock-label">Proposed Action</span>
                            <pre class="interlock-code-val" id="interlock-action-text">--</pre>
                        </div>
                    </div>

                    <div class="interlock-actions-row" id="interlock-buttons-row">
                        <button type="button" class="btn-interlock btn-approve" id="btn-interlock-approve">
                            <svg viewBox="0 0 24 24" width="16" height="16" stroke="currentColor" stroke-width="2.5" fill="none"><polyline points="20 6 9 17 4 12"></polyline></svg>
                            Approve Action
                        </button>
                        <button type="button" class="btn-interlock btn-reject" id="btn-interlock-reject">
                            <svg viewBox="0 0 24 24" width="16" height="16" stroke="currentColor" stroke-width="2.5" fill="none"><line x1="18" y1="6" x2="6" y2="18"></line><line x1="6" y1="6" x2="18" y2="18"></line></svg>
                            Reject Action
                        </button>
                    </div>

                    <div id="interlock-reject-box" class="interlock-reject-box hidden">
                        <input type="text" id="interlock-reject-reason" class="input-reject-reason" placeholder="Enter reason for rejection (optional)...">
                        <button type="button" class="btn-action-confirm" id="btn-confirm-reject">Confirm Reject</button>
                        <button type="button" class="btn-action-cancel" id="btn-cancel-reject">Cancel</button>
                    </div>
                </div>

                <!-- Active Run Details -->
                <div id="exec-run-details" class="exec-run-details hidden">
                    <div class="exec-meta-grid">
                        <div class="exec-meta-item">
                            <span class="meta-label">Target Alias</span>
                            <span class="meta-val" id="exec-meta-target">--</span>
                        </div>
                        <div class="exec-meta-item">
                            <span class="meta-label">Plan Document</span>
                            <span class="meta-val" id="exec-meta-plan">--</span>
                        </div>
                        <div class="exec-meta-item">
                            <span class="meta-label">Actors</span>
                            <span class="meta-val" id="exec-meta-actors">--</span>
                        </div>
                        <div class="exec-meta-item">
                            <span class="meta-label">Elapsed Time</span>
                            <span class="meta-val" id="exec-meta-duration">00m 00s</span>
                        </div>
                    </div>
                </div>

                <!-- Transcript Events Stream -->
                <div class="exec-transcript-section">
                    <div class="transcript-header-row">
                        <h3>Event Transcript</h3>
                        <span class="transcript-count-badge" id="transcript-count">0 events</span>
                    </div>
                    <div class="transcript-stream-container" id="transcript-stream">
                        <div class="transcript-empty" id="transcript-empty">No execution runs or events recorded yet</div>
                    </div>
                </div>
            </section>
        </main>

        <footer class="app-footer">
            <p>aimon daemon &bull; Pure C++17 AI Quota Monitor &bull; Local loopback on 127.0.0.1</p>
        </footer>
    </div>

    <script src="app.js"></script>
</body>
</html>
)raw_asset";

inline const char* STYLE_CSS = R"raw_asset(:root {
    --bg-main: #0a0e17;
    --bg-card: rgba(17, 24, 39, 0.72);
    --border-color: rgba(255, 255, 255, 0.08);
    --text-primary: #f3f4f6;
    --text-secondary: #9ca3af;
    --text-muted: #6b7280;

    --cyan-glow: #00f2fe;
    --cyan-deep: #0284c7;
    --magenta-glow: #f43f5e;
    --purple-glow: #8b5cf6;

    --status-online: #10b981;
    --status-offline: #ef4444;

    --gauge-green: #10b981;
    --gauge-yellow: #f59e0b;
    --gauge-red: #ef4444;

    --font-sans: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    --font-mono: 'JetBrains Mono', monospace;
}

* {
    box-sizing: border-box;
    margin: 0;
    padding: 0;
}

body {
    background-color: var(--bg-main);
    background-image: 
        radial-gradient(circle at 15% 20%, rgba(0, 242, 254, 0.05) 0%, transparent 40%),
        radial-gradient(circle at 85% 30%, rgba(139, 92, 246, 0.06) 0%, transparent 45%);
    color: var(--text-primary);
    font-family: var(--font-sans);
    min-height: 100vh;
    display: flex;
    justify-content: center;
    -webkit-font-smoothing: antialiased;
}

.app-container {
    width: 100%;
    max-width: 1540px;
    padding: 20px 24px 36px;
    display: flex;
    flex-direction: column;
    gap: 20px;
}

/* Header */
.navbar {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 16px 24px;
    background: var(--bg-card);
    backdrop-filter: blur(16px);
    border: 1px solid var(--border-color);
    border-radius: 16px;
}

.brand {
    display: flex;
    align-items: center;
    gap: 14px;
}

.logo-pulse {
    width: 12px;
    height: 12px;
    border-radius: 50%;
    background: var(--cyan-glow);
    box-shadow: 0 0 12px var(--cyan-glow);
    animation: pulse 2s infinite ease-in-out;
}

@keyframes pulse {
    0%, 100% { transform: scale(1); opacity: 0.8; }
    50% { transform: scale(1.3); opacity: 1; }
}

.brand h1 {
    font-size: 1.25rem;
    font-weight: 700;
    letter-spacing: -0.02em;
    display: flex;
    align-items: center;
    gap: 8px;
}

.badge-sub {
    font-size: 0.75rem;
    font-weight: 500;
    color: var(--text-muted);
    border: 1px solid var(--border-color);
    padding: 2px 8px;
    border-radius: 9999px;
}

.actions {
    display: flex;
    align-items: center;
    gap: 16px;
}

.last-updated {
    font-size: 0.82rem;
    color: var(--text-secondary);
    font-family: var(--font-mono);
}

.btn-refresh {
    display: flex;
    align-items: center;
    gap: 8px;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid var(--border-color);
    color: var(--text-primary);
    padding: 8px 16px;
    border-radius: 8px;
    font-size: 0.82rem;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.2s ease;
}

.btn-refresh:hover {
    background: rgba(255, 255, 255, 0.1);
    border-color: rgba(255, 255, 255, 0.2);
}

.btn-refresh.spinning svg {
    animation: spin 0.8s linear infinite;
}

@keyframes spin {
    from { transform: rotate(0deg); }
    to { transform: rotate(360deg); }
}

/* Grid */
.dashboard-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 20px;
}

@media (max-width: 1080px) {
    .dashboard-grid {
        grid-template-columns: 1fr;
    }
}

/* Cards */
.glass-card {
    background: var(--bg-card);
    backdrop-filter: blur(16px);
    border: 1px solid var(--border-color);
    border-radius: 18px;
    padding: 20px 22px;
    display: flex;
    flex-direction: column;
    gap: 16px;
    box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
}

.card-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
}

.provider-title {
    display: flex;
    align-items: center;
    gap: 10px;
}

.provider-title h2 {
    font-size: 1.15rem;
    font-weight: 600;
    letter-spacing: -0.01em;
}

.dot-status {
    width: 9px;
    height: 9px;
    border-radius: 50%;
}

.dot-online {
    background: var(--status-online);
    box-shadow: 0 0 8px var(--status-online);
}

.dot-offline {
    background: var(--status-offline);
    box-shadow: 0 0 8px var(--status-offline);
}

.badge {
    padding: 3px 10px;
    border-radius: 6px;
    font-size: 0.75rem;
    font-weight: 600;
    letter-spacing: 0.04em;
    text-transform: uppercase;
}

.badge-cyan {
    background: rgba(0, 242, 254, 0.12);
    color: var(--cyan-glow);
    border: 1px solid rgba(0, 242, 254, 0.3);
}

.badge-magenta {
    background: rgba(244, 63, 94, 0.12);
    color: var(--magenta-glow);
    border: 1px solid rgba(244, 63, 94, 0.3);
}

/* Credits Strip */
.credits-strip {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px;
}

.credit-pill {
    background: rgba(255, 255, 255, 0.03);
    border: 1px solid var(--border-color);
    padding: 12px 14px;
    border-radius: 12px;
    display: flex;
    flex-direction: column;
    gap: 4px;
}

.credit-label {
    font-size: 0.75rem;
    color: var(--text-secondary);
    text-transform: uppercase;
    letter-spacing: 0.03em;
}

.credit-val {
    font-size: 1.3rem;
    font-weight: 700;
    font-family: var(--font-mono);
    color: var(--text-primary);
}

/* Quota Groups */
.quota-groups-section {
    display: flex;
    flex-direction: column;
    gap: 14px;
}

.section-title-row h3 {
    font-size: 0.82rem;
    font-weight: 600;
    color: var(--text-secondary);
    text-transform: uppercase;
    letter-spacing: 0.04em;
}

.quota-groups-list {
    display: flex;
    flex-direction: column;
    gap: 14px;
}

.quota-group-card {
    background: rgba(255, 255, 255, 0.025);
    border: 1px solid var(--border-color);
    border-radius: 14px;
    padding: 16px;
    display: flex;
    flex-direction: column;
    gap: 12px;
    transition: border-color 0.2s ease;
}

.quota-group-card:hover {
    border-color: rgba(255, 255, 255, 0.15);
}

.group-title-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
}

.group-title {
    font-size: 1.0rem;
    font-weight: 600;
    color: var(--text-primary);
    display: flex;
    align-items: center;
    gap: 8px;
}

.group-desc {
    font-size: 0.78rem;
    color: var(--text-secondary);
    line-height: 1.4;
}

.buckets-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 10px;
}

.bucket-card {
    background: rgba(0, 0, 0, 0.2);
    border: 1px solid rgba(255, 255, 255, 0.06);
    border-radius: 10px;
    padding: 12px 14px;
    display: flex;
    flex-direction: column;
    gap: 8px;
}

.bucket-top {
    display: flex;
    justify-content: space-between;
    align-items: center;
}

.bucket-label {
    font-size: 0.78rem;
    font-weight: 500;
    color: var(--text-secondary);
}

.bucket-pct {
    font-family: var(--font-mono);
    font-size: 0.92rem;
    font-weight: 700;
}

.bucket-track {
    width: 100%;
    height: 7px;
    background: rgba(255, 255, 255, 0.08);
    border-radius: 9999px;
    overflow: hidden;
}

.bucket-fill {
    height: 100%;
    border-radius: 9999px;
    transition: width 0.6s cubic-bezier(0.4, 0, 0.2, 1), background-color 0.4s ease;
}

.bucket-bottom {
    display: flex;
    justify-content: space-between;
    align-items: center;
}

.bucket-window-badge {
    font-size: 0.68rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    padding: 2px 6px;
    border-radius: 4px;
    background: rgba(255, 255, 255, 0.05);
    color: var(--text-secondary);
}

.bucket-timer {
    font-family: var(--font-mono);
    font-size: 0.74rem;
    color: var(--cyan-glow);
}

/* Collapsible Models Section */
.models-collapsible-section {
    display: flex;
    flex-direction: column;
    gap: 12px;
    border-top: 1px solid rgba(255, 255, 255, 0.06);
    padding-top: 12px;
}

.btn-toggle-models {
    background: transparent;
    border: none;
    color: var(--text-secondary);
    font-size: 0.78rem;
    font-weight: 500;
    display: flex;
    align-items: center;
    gap: 6px;
    cursor: pointer;
    padding: 4px 0;
    transition: color 0.2s ease;
}

.btn-toggle-models:hover {
    color: var(--text-primary);
}

.toggle-icon {
    transition: transform 0.2s ease;
}

.toggle-icon.expanded {
    transform: rotate(180deg);
}

/* Models & Gauges Grid */
.gauges-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
    gap: 16px;
}

.gauge-card {
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid var(--border-color);
    border-radius: 14px;
    padding: 14px 10px;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 8px;
    text-align: center;
}

.gauge-svg-box {
    position: relative;
    width: 90px;
    height: 90px;
}

.gauge-svg {
    transform: rotate(-90deg);
}

.gauge-bg-circle {
    stroke: rgba(255, 255, 255, 0.08);
    stroke-width: 6;
    fill: none;
}

.gauge-fill-circle {
    stroke-width: 6;
    stroke-linecap: round;
    fill: none;
    transition: stroke-dashoffset 0.8s ease, stroke 0.4s ease;
}

.gauge-inner-txt {
    position: absolute;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    font-family: var(--font-mono);
    font-size: 0.95rem;
    font-weight: 700;
}

.gauge-model-name {
    font-size: 0.8rem;
    font-weight: 600;
    color: var(--text-primary);
    line-height: 1.2;
}

.gauge-timer {
    font-family: var(--font-mono);
    font-size: 0.72rem;
    color: var(--cyan-glow);
}

/* Cursor Metrics */
.cursor-metrics {
    display: flex;
    flex-direction: column;
    gap: 20px;
}

.usage-stat-box {
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid var(--border-color);
    padding: 18px;
    border-radius: 14px;
    display: flex;
    flex-direction: column;
    gap: 10px;
}

.stat-top {
    display: flex;
    justify-content: space-between;
    align-items: center;
}

.stat-label {
    font-size: 0.82rem;
    font-weight: 600;
    color: var(--text-secondary);
}

.stat-ratio {
    font-family: var(--font-mono);
    font-size: 0.95rem;
    font-weight: 600;
    color: var(--text-primary);
}

.progress-track {
    width: 100%;
    height: 10px;
    background: rgba(255, 255, 255, 0.06);
    border-radius: 9999px;
    overflow: hidden;
}

.progress-fill {
    height: 100%;
    background: linear-gradient(90deg, var(--purple-glow), var(--magenta-glow));
    border-radius: 9999px;
    transition: width 0.8s ease;
}

.stat-bottom {
    display: flex;
    justify-content: space-between;
    font-size: 0.75rem;
    color: var(--text-muted);
}

.stat-pct {
    font-family: var(--font-mono);
    font-weight: 600;
    color: var(--magenta-glow);
}

.cursor-top-row {
    display: grid;
    grid-template-columns: 1.35fr 1fr;
    gap: 12px;
    align-items: stretch;
}

.details-box {
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid var(--border-color);
    border-radius: 12px;
    padding: 10px 14px;
    display: flex;
    flex-direction: column;
    justify-content: center;
    gap: 6px;
}

.details-box .detail-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 4px 0;
    border-bottom: 1px solid rgba(255, 255, 255, 0.04);
}

.details-box .detail-row:last-child {
    border-bottom: none;
}

.details-list {
    display: flex;
    flex-direction: column;
    gap: 12px;
}

.detail-row {
    display: flex;
    justify-content: space-between;
    padding: 8px 0;
    border-bottom: 1px solid rgba(255, 255, 255, 0.04);
}

.detail-label {
    font-size: 0.82rem;
    color: var(--text-secondary);
}

.detail-value {
    font-size: 0.85rem;
    font-family: var(--font-mono);
    font-weight: 500;
}

.error-banner {
    background: rgba(239, 68, 68, 0.1);
    border: 1px solid rgba(239, 68, 68, 0.3);
    color: #f87171;
    font-size: 0.8rem;
    padding: 10px 14px;
    border-radius: 8px;
}

.hidden {
    display: none;
}

/* Cumulative Spend & Personal Usage */
.spend-section {
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid var(--border-color);
    border-radius: 14px;
    padding: 16px;
    display: flex;
    flex-direction: column;
    gap: 12px;
    margin-top: 4px;
}

.spend-content-grid {
    display: grid;
    grid-template-columns: 1.25fr 1fr;
    gap: 16px;
    align-items: start;
}

.scrollable-categories {
    max-height: 185px;
    overflow-y: auto;
    padding-right: 6px;
}

.scrollable-categories::-webkit-scrollbar {
    width: 4px;
}

.scrollable-categories::-webkit-scrollbar-track {
    background: rgba(255, 255, 255, 0.02);
    border-radius: 4px;
}

.scrollable-categories::-webkit-scrollbar-thumb {
    background: rgba(255, 255, 255, 0.15);
    border-radius: 4px;
}

.spend-header {
    display: flex;
    justify-content: space-between;
    align-items: flex-start;
    flex-wrap: wrap;
    gap: 10px;
}

.spend-subtitle {
    font-size: 0.72rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-muted);
}

.spend-title {
    font-size: 1.05rem;
    font-weight: 700;
    color: var(--text-primary);
    margin-top: 2px;
}

.spend-desc {
    font-size: 0.78rem;
    color: var(--text-secondary);
}

.spend-totals {
    display: flex;
    flex-direction: column;
    align-items: flex-end;
}

.spend-amount {
    font-family: var(--font-mono);
    font-size: 1.5rem;
    font-weight: 700;
    color: #38bdf8;
}

.spend-prev-pill {
    font-size: 0.72rem;
    color: var(--text-muted);
    font-family: var(--font-mono);
}

.spend-chart-container {
    width: 100%;
    position: relative;
    padding-top: 6px;
}

.spend-chart-svg {
    width: 100%;
    height: 160px;
    overflow: visible;
}

.chart-grid-line {
    stroke: rgba(255, 255, 255, 0.06);
    stroke-dasharray: 4 4;
}

.chart-axis-lbl {
    font-family: var(--font-mono);
    font-size: 10px;
    fill: var(--text-muted);
    text-anchor: end;
}

.chart-dates-row {
    display: flex;
    justify-content: space-between;
    padding: 6px 10px 0 52px;
    font-family: var(--font-mono);
    font-size: 0.72rem;
    color: var(--text-muted);
}

.chart-dot {
    fill: #3b82f6;
    stroke: #0a0e17;
    stroke-width: 2;
    cursor: pointer;
    transition: r 0.2s ease, fill 0.2s ease;
}

.chart-dot:hover {
    r: 6;
    fill: #60a5fa;
}

/* Category Breakdown */
.categories-breakdown {
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding-top: 12px;
    border-top: 1px solid rgba(255, 255, 255, 0.05);
}

.breakdown-header {
    font-size: 0.75rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-secondary);
}

.categories-list {
    display: flex;
    flex-direction: column;
    gap: 8px;
}

.cat-row {
    display: flex;
    flex-direction: column;
    gap: 4px;
}

.cat-info {
    display: flex;
    justify-content: space-between;
    font-size: 0.78rem;
}

.cat-name {
    color: var(--text-primary);
    font-weight: 500;
}

.cat-val {
    font-family: var(--font-mono);
    color: var(--text-secondary);
}

.cat-bar-track {
    width: 100%;
    height: 6px;
    background: rgba(255, 255, 255, 0.05);
    border-radius: 9999px;
    overflow: hidden;
}

.cat-bar-fill {
    height: 100%;
    border-radius: 9999px;
    transition: width 0.6s ease;
}

/* Agents Fleet Section */
.agents-fleet-card {
    grid-column: 1 / -1;
}

.agents-header-actions {
    display: flex;
    align-items: center;
    gap: 16px;
}

.toggle-switch-label {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    cursor: pointer;
    font-size: 0.78rem;
    color: var(--text-secondary);
    user-select: none;
}

.toggle-switch-label input[type="checkbox"] {
    display: none;
}

.toggle-switch-slider {
    position: relative;
    width: 32px;
    height: 18px;
    background: rgba(255, 255, 255, 0.12);
    border-radius: 9999px;
    transition: background 0.25s ease;
}

.toggle-switch-slider::before {
    content: '';
    position: absolute;
    width: 12px;
    height: 12px;
    left: 3px;
    top: 3px;
    background: #ffffff;
    border-radius: 50%;
    transition: transform 0.25s ease;
}

.toggle-switch-label input[type="checkbox"]:checked + .toggle-switch-slider {
    background: var(--cyan-deep);
}

.toggle-switch-label input[type="checkbox"]:checked + .toggle-switch-slider::before {
    transform: translateX(14px);
}

.toggle-switch-text {
    font-weight: 500;
}

/* Agents Table */
.table-responsive {
    width: 100%;
    overflow-x: auto;
    margin-top: 14px;
}

.agents-table {
    width: 100%;
    border-collapse: collapse;
    font-size: 0.82rem;
}

.agents-table th {
    text-align: left;
    padding: 10px 12px;
    font-size: 0.72rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    color: var(--text-muted);
    border-bottom: 1px solid var(--border-color);
}

.agents-table td {
    padding: 12px 12px;
    border-bottom: 1px solid rgba(255, 255, 255, 0.04);
    vertical-align: middle;
}

.agents-table tr:last-child td {
    border-bottom: none;
}

.agents-table tbody tr {
    transition: background 0.15s ease;
}

.agents-table tbody tr:hover {
    background: rgba(255, 255, 255, 0.025);
}

/* Status Pills */
.status-pill {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 3px 10px;
    border-radius: 9999px;
    font-size: 0.74rem;
    font-weight: 600;
    white-space: nowrap;
}

.status-pill .dot {
    width: 7px;
    height: 7px;
    border-radius: 50%;
}

.status-pill.running {
    background: rgba(16, 185, 129, 0.14);
    color: #34d399;
    border: 1px solid rgba(16, 185, 129, 0.35);
}

.status-pill.running .dot {
    background: #34d399;
    box-shadow: 0 0 8px #34d399;
    animation: pulse 1.8s infinite ease-in-out;
}

.status-pill.waiting_for_user {
    background: rgba(245, 158, 11, 0.14);
    color: #fbbf24;
    border: 1px solid rgba(245, 158, 11, 0.35);
}

.status-pill.waiting_for_user .dot {
    background: #fbbf24;
}

.status-pill.completed {
    background: rgba(59, 130, 246, 0.14);
    color: #60a5fa;
    border: 1px solid rgba(59, 130, 246, 0.35);
}

.status-pill.completed .dot {
    background: #60a5fa;
}

.status-pill.failed {
    background: rgba(239, 68, 68, 0.14);
    color: #f87171;
    border: 1px solid rgba(239, 68, 68, 0.35);
}

.status-pill.failed .dot {
    background: #f87171;
}

.status-pill.stale,
.status-pill.disconnected {
    background: rgba(156, 163, 175, 0.12);
    color: #9ca3af;
    border: 1px solid rgba(156, 163, 175, 0.22);
}

.status-pill.stale .dot,
.status-pill.disconnected .dot {
    background: #9ca3af;
}

/* Agent Platform Badge */
.badge-agent {
    font-size: 0.74rem;
    font-weight: 600;
    padding: 3px 8px;
    border-radius: 6px;
    display: inline-flex;
    align-items: center;
    gap: 4px;
    white-space: nowrap;
}

.badge-agent.cursor {
    background: rgba(121, 40, 202, 0.18);
    color: #d8b4fe;
    border: 1px solid rgba(121, 40, 202, 0.35);
}

.badge-agent.antigravity {
    background: rgba(0, 242, 254, 0.14);
    color: #67e8f9;
    border: 1px solid rgba(0, 242, 254, 0.3);
}

.badge-agent.cli {
    background: rgba(16, 185, 129, 0.14);
    color: #6ee7b7;
    border: 1px solid rgba(16, 185, 129, 0.3);
}

/* Task cell */
.task-title {
    font-weight: 500;
    color: var(--text-primary);
    line-height: 1.35;
    word-break: break-word;
}

.task-workspace {
    font-size: 0.72rem;
    color: var(--text-muted);
    font-family: var(--font-mono);
    margin-top: 3px;
}

/* Action chip */
.action-chip {
    font-family: var(--font-mono);
    font-size: 0.74rem;
    background: rgba(0, 0, 0, 0.25);
    border: 1px solid var(--border-color);
    padding: 3px 7px;
    border-radius: 5px;
    color: #a5f3fc;
    display: inline-block;
    max-width: 220px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}

/* Duration & Last Seen */
.duration-counter {
    font-family: var(--font-mono);
    font-size: 0.8rem;
    font-weight: 600;
    color: var(--text-primary);
    white-space: nowrap;
}

.last-seen-text {
    font-size: 0.75rem;
    color: var(--text-muted);
    white-space: nowrap;
}

/* Empty State */
.agents-empty-state {
    text-align: center;
    padding: 32px 16px;
    color: var(--text-muted);
}

.agents-empty-state .empty-icon {
    font-size: 2rem;
    margin-bottom: 8px;
    opacity: 0.75;
}

.agents-empty-state .empty-title {
    font-size: 0.92rem;
    font-weight: 500;
    color: var(--text-secondary);
    margin-bottom: 4px;
}

.agents-empty-state .empty-desc {
    font-size: 0.78rem;
    max-width: 440px;
    margin: 0 auto;
    line-height: 1.4;
}

.agents-empty-state.hidden,
.table-responsive.hidden {
    display: none;
}

/* MCP Clients Bar */
.mcp-clients-bar {
    display: flex;
    align-items: center;
    justify-content: flex-start;
    gap: 12px;
    padding: 10px 14px;
    margin-bottom: 14px;
    background: rgba(15, 23, 42, 0.45);
    border: 1px solid rgba(255, 255, 255, 0.07);
    border-radius: 8px;
    flex-wrap: wrap;
}

.mcp-clients-left {
    display: flex;
    align-items: center;
    gap: 6px;
}

.mcp-clients-title {
    font-size: 0.78rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    color: var(--text-muted);
    display: flex;
    align-items: center;
    gap: 6px;
    white-space: nowrap;
}

.mcp-clients-list {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
}

.client-badge {
    display: inline-flex;
    align-items: center;
    gap: 7px;
    padding: 4px 10px;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid var(--border-color);
    border-radius: 6px;
    font-size: 0.78rem;
    color: var(--text-primary);
    transition: all 0.2s ease;
}

.client-badge-empty {
    color: var(--text-muted);
    font-style: italic;
    font-size: 0.76rem;
    background: transparent;
    border: 1px dashed rgba(255, 255, 255, 0.12);
}

.client-badge .dot-status {
    width: 7px;
    height: 7px;
    border-radius: 50%;
}

.client-badge .client-name {
    font-weight: 600;
}

.client-badge .client-ver {
    font-size: 0.72rem;
    color: var(--text-muted);
    font-family: var(--font-mono);
}

.client-badge .client-ip {
    font-family: var(--font-mono);
    font-size: 0.7rem;
    color: var(--text-muted);
    background: rgba(0, 0, 0, 0.25);
    padding: 1px 5px;
    border-radius: 4px;
}

.client-badge .client-uptime {
    font-family: var(--font-mono);
    font-size: 0.7rem;
    color: var(--cyan);
}

/* Brand specific badge styles */
.client-badge.client-cursor {
    background: rgba(121, 40, 202, 0.12);
    border-color: rgba(121, 40, 202, 0.35);
}

.client-badge.client-cursor .client-name {
    color: #e9d5ff;
}

.client-badge.client-antigravity {
    background: rgba(0, 242, 254, 0.1);
    border-color: rgba(0, 242, 254, 0.3);
}

.client-badge.client-antigravity .client-name {
    color: #67e8f9;
}

.client-badge.client-claude {
    background: rgba(245, 158, 11, 0.12);
    border-color: rgba(245, 158, 11, 0.35);
}

.client-badge.client-claude .client-name {
    color: #fcd34d;
}

.client-badge.client-generic {
    background: rgba(99, 102, 241, 0.12);
    border-color: rgba(99, 102, 241, 0.3);
}

.client-badge.client-generic .client-name {
    color: #a5b4fc;
}

/* -------------------------------------------------------------
 * Execution Runs & Interlock UI
 * ------------------------------------------------------------- */
.badge-amber {
    background: rgba(245, 158, 11, 0.15);
    color: #fbbf24;
    border: 1px solid rgba(245, 158, 11, 0.35);
}

.exec-runs-card {
    grid-column: 1 / -1;
    display: flex;
    flex-direction: column;
    gap: 16px;
}

.interlock-banner {
    background: linear-gradient(135deg, rgba(245, 158, 11, 0.12), rgba(20, 20, 30, 0.85));
    border: 1px solid rgba(245, 158, 11, 0.45);
    box-shadow: 0 0 25px rgba(245, 158, 11, 0.15);
    border-radius: 12px;
    padding: 16px 20px;
    display: flex;
    flex-direction: column;
    gap: 14px;
    animation: pulse-glow 2.5s infinite ease-in-out;
}

@keyframes pulse-glow {
    0%, 100% { box-shadow: 0 0 15px rgba(245, 158, 11, 0.15); }
    50% { box-shadow: 0 0 25px rgba(245, 158, 11, 0.35); }
}

.interlock-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
}

.interlock-icon-title {
    display: flex;
    align-items: center;
    gap: 12px;
}

.interlock-icon {
    font-size: 1.5rem;
    animation: bounce 1.2s infinite ease-in-out;
}

@keyframes bounce {
    0%, 100% { transform: translateY(0); }
    50% { transform: translateY(-3px); }
}

.interlock-title {
    font-size: 1.05rem;
    font-weight: 700;
    color: #fbbf24;
    margin: 0;
}

.interlock-sub {
    font-size: 0.8rem;
    color: var(--text-secondary);
}

.interlock-body {
    display: grid;
    grid-template-columns: 1fr 1.2fr;
    gap: 14px;
}

.interlock-box {
    background: rgba(0, 0, 0, 0.35);
    border: 1px solid rgba(255, 255, 255, 0.06);
    border-radius: 8px;
    padding: 10px 14px;
    display: flex;
    flex-direction: column;
    gap: 6px;
}

.interlock-label {
    font-size: 0.72rem;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-muted);
}

.interlock-val {
    font-size: 0.88rem;
    color: var(--text-primary);
    line-height: 1.4;
}

.interlock-code-val {
    font-family: var(--font-mono);
    font-size: 0.82rem;
    color: #a5f3fc;
    background: rgba(0, 0, 0, 0.4);
    padding: 6px 10px;
    border-radius: 6px;
    margin: 0;
    white-space: pre-wrap;
    word-break: break-all;
    max-height: 90px;
    overflow-y: auto;
}

.interlock-actions-row {
    display: flex;
    gap: 12px;
    align-items: center;
}

.btn-interlock {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    font-size: 0.88rem;
    font-weight: 600;
    padding: 9px 18px;
    border-radius: 8px;
    border: none;
    cursor: pointer;
    transition: all 0.2s ease;
}

.btn-approve {
    background: linear-gradient(135deg, #059669, #10b981);
    color: #ffffff;
    box-shadow: 0 4px 14px rgba(16, 185, 129, 0.35);
}

.btn-approve:hover {
    transform: translateY(-1px);
    box-shadow: 0 6px 18px rgba(16, 185, 129, 0.45);
}

.btn-reject {
    background: linear-gradient(135deg, #dc2626, #ef4444);
    color: #ffffff;
    box-shadow: 0 4px 14px rgba(239, 68, 68, 0.35);
}

.btn-reject:hover {
    transform: translateY(-1px);
    box-shadow: 0 6px 18px rgba(239, 68, 68, 0.45);
}

.interlock-reject-box {
    display: flex;
    gap: 10px;
    align-items: center;
    background: rgba(0, 0, 0, 0.4);
    padding: 10px 14px;
    border-radius: 8px;
    border: 1px solid rgba(239, 68, 68, 0.3);
}

.input-reject-reason {
    flex: 1;
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid rgba(255, 255, 255, 0.15);
    border-radius: 6px;
    padding: 7px 12px;
    color: #ffffff;
    font-size: 0.85rem;
    outline: none;
}

.input-reject-reason:focus {
    border-color: #ef4444;
}

.btn-action-confirm {
    background: #ef4444;
    color: white;
    border: none;
    padding: 7px 14px;
    border-radius: 6px;
    font-weight: 600;
    font-size: 0.82rem;
    cursor: pointer;
}

.btn-action-cancel {
    background: transparent;
    color: var(--text-secondary);
    border: 1px solid rgba(255, 255, 255, 0.15);
    padding: 7px 12px;
    border-radius: 6px;
    font-size: 0.82rem;
    cursor: pointer;
}

.exec-run-details {
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid var(--border-color);
    border-radius: 12px;
    padding: 12px 18px;
}

.exec-meta-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
    gap: 12px;
}

.exec-meta-item {
    display: flex;
    flex-direction: column;
    gap: 4px;
}

.exec-meta-item .meta-label {
    font-size: 0.72rem;
    color: var(--text-muted);
    text-transform: uppercase;
}

.exec-meta-item .meta-val {
    font-size: 0.88rem;
    font-family: var(--font-mono);
    color: var(--text-primary);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
}

.exec-transcript-section {
    display: flex;
    flex-direction: column;
    gap: 10px;
}

.transcript-header-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
}

.transcript-header-row h3 {
    font-size: 0.95rem;
    font-weight: 600;
    color: var(--text-primary);
    margin: 0;
}

.transcript-count-badge {
    font-size: 0.75rem;
    font-family: var(--font-mono);
    color: var(--text-muted);
    background: rgba(255, 255, 255, 0.04);
    padding: 2px 8px;
    border-radius: 6px;
}

.transcript-stream-container {
    max-height: 420px;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding-right: 4px;
}

.transcript-empty {
    text-align: center;
    color: var(--text-muted);
    font-size: 0.85rem;
    padding: 30px 10px;
    border: 1px dashed rgba(255, 255, 255, 0.08);
    border-radius: 8px;
}

.tx-event-card {
    background: rgba(0, 0, 0, 0.25);
    border: 1px solid rgba(255, 255, 255, 0.07);
    border-left: 3px solid #3b82f6;
    border-radius: 8px;
    padding: 10px 14px;
    display: flex;
    flex-direction: column;
    gap: 8px;
    transition: background 0.15s ease;
}

.tx-event-card:hover {
    background: rgba(255, 255, 255, 0.03);
}

.tx-event-card.kind-start { border-left-color: #10b981; }
.tx-event-card.kind-result { border-left-color: #06b6d4; }
.tx-event-card.kind-review { border-left-color: #a855f7; }
.tx-event-card.kind-interlock { border-left-color: #f59e0b; background: rgba(245, 158, 11, 0.04); }
.tx-event-card.kind-interlock_resolved { border-left-color: #10b981; }
.tx-event-card.kind-recover { border-left-color: #f97316; }
.tx-event-card.kind-end { border-left-color: #8b5cf6; }
.tx-event-card.kind-abort { border-left-color: #ef4444; }

.tx-event-top {
    display: flex;
    justify-content: space-between;
    align-items: center;
}

.tx-event-left {
    display: flex;
    align-items: center;
    gap: 8px;
}

.tx-seq {
    font-family: var(--font-mono);
    font-size: 0.75rem;
    color: var(--text-muted);
    background: rgba(255, 255, 255, 0.05);
    padding: 1px 5px;
    border-radius: 4px;
}

.tx-badge-actor {
    font-size: 0.72rem;
    font-weight: 600;
    padding: 1px 7px;
    border-radius: 4px;
    text-transform: uppercase;
}

.tx-actor-gemini { background: rgba(6, 182, 212, 0.15); color: #22d3ee; }
.tx-actor-cursor { background: rgba(168, 85, 247, 0.15); color: #c084fc; }
.tx-actor-human { background: rgba(245, 158, 11, 0.15); color: #fbbf24; }
.tx-actor-aimon { background: rgba(99, 102, 241, 0.15); color: #a5b4fc; }

.tx-badge-kind {
    font-size: 0.72rem;
    font-family: var(--font-mono);
    color: var(--text-secondary);
}

.tx-checkpoint-pill {
    font-size: 0.72rem;
    font-family: var(--font-mono);
    color: #67e8f9;
    background: rgba(0, 242, 254, 0.08);
    padding: 1px 6px;
    border-radius: 4px;
}

.tx-event-time {
    font-size: 0.72rem;
    color: var(--text-muted);
    font-family: var(--font-mono);
}

.tx-decision-pill {
    font-size: 0.75rem;
    font-weight: 600;
    padding: 2px 8px;
    border-radius: 4px;
}

.tx-dec-continue { background: rgba(16, 185, 129, 0.15); color: #34d399; }
.tx-dec-wait_human { background: rgba(245, 158, 11, 0.15); color: #fbbf24; }
.tx-dec-recover { background: rgba(249, 115, 22, 0.15); color: #fb923c; }
.tx-dec-stop { background: rgba(239, 68, 68, 0.15); color: #f87171; }
.tx-dec-approved { background: rgba(16, 185, 129, 0.2); color: #10b981; }
.tx-dec-rejected { background: rgba(239, 68, 68, 0.2); color: #ef4444; }

.tx-commands-list {
    margin: 0;
    padding-left: 18px;
    font-family: var(--font-mono);
    font-size: 0.78rem;
    color: #e2e8f0;
}

.tx-commands-list li {
    margin: 2px 0;
}

.tx-code-exit {
    font-size: 0.7rem;
    padding: 0 4px;
    border-radius: 3px;
    margin-left: 6px;
}

.tx-code-exit.exit-ok { background: rgba(16, 185, 129, 0.2); color: #34d399; }
.tx-code-exit.exit-err { background: rgba(239, 68, 68, 0.2); color: #f87171; }

.tx-dmesg-box {
    background: rgba(0, 0, 0, 0.4);
    border: 1px solid rgba(255, 255, 255, 0.05);
    border-radius: 6px;
    padding: 6px 10px;
    font-family: var(--font-mono);
    font-size: 0.72rem;
    color: #94a3b8;
    max-height: 80px;
    overflow-y: auto;
    white-space: pre-wrap;
    margin: 0;
}

.tx-notes-text {
    font-size: 0.82rem;
    color: var(--text-secondary);
    line-height: 1.35;
}

/* Footer */
.app-footer {
    text-align: center;
    font-size: 0.75rem;
    color: var(--text-muted);
    padding-top: 10px;
}
)raw_asset";

inline const char* APP_JS = R"raw_asset(//
// app.js - aimon client dynamics & countdown timers
//

let currentStatus = null;
let countdownInterval = null;

function formatDuration(ms) {
    if (ms <= 0) return 'Ready';
    const totalSeconds = Math.floor(ms / 1000);
    const days = Math.floor(totalSeconds / 86400);
    const hours = Math.floor((totalSeconds % 86400) / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;

    const pad = (n) => String(n).padStart(2, '0');
    if (days > 0) {
        return `${days}d ${hours}h ${pad(minutes)}m`;
    }
    if (hours > 0) {
        return `${hours}h ${pad(minutes)}m ${pad(seconds)}s`;
    }
    return `${minutes}m ${pad(seconds)}s`;
}

function updateCountdowns() {
    if (!currentStatus || !currentStatus.antigravity) {
        return;
    }

    const now = Date.now();

    // 1. Quota group buckets
    if (currentStatus.antigravity.quota_groups) {
        currentStatus.antigravity.quota_groups.forEach((group, gIdx) => {
            if (!group.buckets) return;
            group.buckets.forEach((bucket, bIdx) => {
                const timerEl = document.getElementById(`timer-bucket-${gIdx}-${bIdx}`);
                if (!timerEl) return;
                if (!bucket.reset_time_iso) {
                    timerEl.textContent = 'Active';
                    return;
                }
                const diff = new Date(bucket.reset_time_iso).getTime() - now;
                timerEl.textContent = diff > 0 ? `Resets in ${formatDuration(diff)}` : 'Ready to reset';
            });
        });
    }

    // 2. Individual models
    if (currentStatus.antigravity.models) {
        currentStatus.antigravity.models.forEach((m, idx) => {
            const timerEl = document.getElementById(`timer-model-${idx}`);
            if (!timerEl) return;

            if (!m.reset_time_iso) {
                timerEl.textContent = 'Active';
                return;
            }

            const resetTime = new Date(m.reset_time_iso).getTime();
            const diff = resetTime - now;
            timerEl.textContent = diff > 0 ? `Resets in ${formatDuration(diff)}` : 'Ready to reset';
        });
    }
}

function renderAntigravity(ag) {
    const statusDot = document.getElementById('ag-status-dot');
    const planBadge = document.getElementById('ag-plan-badge');
    const quotaGroupsContainer = document.getElementById('ag-quota-groups');
    const modelsGrid = document.getElementById('ag-models-grid');
    const modelsToggleText = document.getElementById('ag-models-toggle-text');
    const errorBanner = document.getElementById('ag-error');

    if (!ag || !ag.is_running) {
        statusDot.className = 'dot-status dot-offline';
        planBadge.textContent = 'Offline';
        planBadge.className = 'badge';
        if (quotaGroupsContainer) {
            quotaGroupsContainer.innerHTML = '<div class="gauge-loading">Antigravity language server offline</div>';
        }
        if (modelsGrid) {
            modelsGrid.innerHTML = '';
        }
        if (ag && ag.error_message) {
            errorBanner.textContent = ag.error_message;
            errorBanner.classList.remove('hidden');
        }
        return;
    }

    statusDot.className = 'dot-status dot-online';
    planBadge.textContent = ag.plan_tier || 'Google AI Ultra';
    planBadge.className = 'badge badge-cyan';
    errorBanner.classList.add('hidden');

    // Render Quota Groups (Gemini Models, Claude and GPT models)
    if (quotaGroupsContainer) {
        if (ag.quota_groups && ag.quota_groups.length > 0) {
            quotaGroupsContainer.innerHTML = '';
            ag.quota_groups.forEach((group, gIdx) => {
                const groupCard = document.createElement('div');
                groupCard.className = 'quota-group-card';

                let bucketsHtml = '';
                (group.buckets || []).forEach((b, bIdx) => {
                    const frac = Math.max(0, Math.min(1, b.remaining_fraction !== undefined ? b.remaining_fraction : 1.0));
                    const pct = Math.round(frac * 100);

                    let fillColor = '#10b981';
                    let textColor = '#10b981';
                    if (frac <= 0.20) {
                        fillColor = '#ef4444';
                        textColor = '#ef4444';
                    } else if (frac <= 0.50) {
                        fillColor = '#f59e0b';
                        textColor = '#f59e0b';
                    }

                    const win = (b.window || '').toLowerCase();
                    const windowLabel = (win === 'weekly') ? 'Weekly' : ((win === '5h' || win === 'five_hour') ? '5-Hour' : (b.display_name || 'Limit'));

                    bucketsHtml += `
                        <div class="bucket-card">
                            <div class="bucket-top">
                                <span class="bucket-label">${b.display_name || 'Capacity'}</span>
                                <span class="bucket-pct" style="color: ${textColor};">${pct}%</span>
                            </div>
                            <div class="bucket-track">
                                <div class="bucket-fill" style="width: ${pct}%; background: ${fillColor};"></div>
                            </div>
                            <div class="bucket-bottom">
                                <span class="bucket-window-badge">${windowLabel}</span>
                                <span id="timer-bucket-${gIdx}-${bIdx}" class="bucket-timer">Calculating...</span>
                            </div>
                        </div>
                    `;
                });

                groupCard.innerHTML = `
                    <div class="group-title-row">
                        <h4 class="group-title">${group.display_name}</h4>
                    </div>
                    ${group.description ? `<p class="group-desc">${group.description}</p>` : ''}
                    <div class="buckets-grid">
                        ${bucketsHtml}
                    </div>
                `;
                quotaGroupsContainer.appendChild(groupCard);
            });
        } else {
            quotaGroupsContainer.innerHTML = '<div class="gauge-loading">No quota groups reported</div>';
        }
    }

    // Render Individual Models
    if (modelsGrid) {
        if (!ag.models || ag.models.length === 0) {
            modelsGrid.innerHTML = '<div class="gauge-loading">No active model quotas found</div>';
        } else {
            if (modelsToggleText) {
                modelsToggleText.textContent = `Individual Model Capacities (${ag.models.length})`;
            }
            modelsGrid.innerHTML = '';
            const circumference = 220; // 2 * pi * r (r=35)

            ag.models.forEach((m, idx) => {
                const frac = Math.max(0, Math.min(1, m.remaining_fraction !== undefined ? m.remaining_fraction : 1.0));
                const pct = Math.round(frac * 100);
                const offset = circumference - (circumference * frac);

                let strokeColor = '#10b981'; // Green
                if (frac <= 0.20) {
                    strokeColor = '#ef4444'; // Red
                } else if (frac <= 0.50) {
                    strokeColor = '#f59e0b'; // Yellow
                }

                const card = document.createElement('div');
                card.className = 'gauge-card';
                card.innerHTML = `
                    <div class="gauge-svg-box">
                        <svg class="gauge-svg" width="90" height="90" viewBox="0 0 90 90">
                            <circle class="gauge-bg-circle" cx="45" cy="45" r="35"></circle>
                            <circle class="gauge-fill-circle" cx="45" cy="45" r="35"
                                style="stroke: ${strokeColor}; stroke-dasharray: ${circumference}; stroke-dashoffset: ${offset};">
                            </circle>
                        </svg>
                        <div class="gauge-inner-txt">${pct}%</div>
                    </div>
                    <div class="gauge-model-name">${m.model_name || m.model_id}</div>
                    <div id="timer-model-${idx}" class="gauge-timer">Calculating...</div>
                `;
                modelsGrid.appendChild(card);
            });
        }
    }

    updateCountdowns();
}

function renderCursor(cr) {
    const statusDot = document.getElementById('cursor-status-dot');
    const planBadge = document.getElementById('cursor-plan-badge');
    const ratioEl = document.getElementById('cursor-fast-ratio');
    const progressBar = document.getElementById('cursor-progress-bar');
    const remainingTxt = document.getElementById('cursor-remaining-txt');
    const percentTxt = document.getElementById('cursor-percent-txt');
    const resetDateEl = document.getElementById('cursor-reset-date');
    const daysRemainingEl = document.getElementById('cursor-days-remaining');
    const errorBanner = document.getElementById('cursor-error');

    if (!cr || !cr.is_authenticated) {
        statusDot.className = 'dot-status dot-offline';
        planBadge.textContent = cr ? (cr.plan_tier || 'Free') : 'Unauthenticated';
        planBadge.className = 'badge';
        ratioEl.textContent = '-- / --';
        progressBar.style.width = '0%';
        remainingTxt.textContent = 'Unauthenticated';
        percentTxt.textContent = '0%';
        resetDateEl.textContent = '--';
        daysRemainingEl.textContent = '--';

        if (cr && cr.error_message) {
            errorBanner.textContent = cr.error_message;
            errorBanner.classList.remove('hidden');
        }
        return;
    }

    statusDot.className = 'dot-status dot-online';
    planBadge.textContent = cr.plan_tier || 'Pro';
    planBadge.className = 'badge badge-magenta';
    errorBanner.classList.add('hidden');

    const used = cr.fast_requests_used || 0;
    const limit = cr.fast_requests_limit || 0;
    ratioEl.textContent = `${used.toLocaleString()} / ${limit.toLocaleString()}`;

    const frac = limit > 0 ? Math.min(1, used / limit) : 0;
    const pct = Math.round(frac * 100);
    progressBar.style.width = `${pct}%`;
    percentTxt.textContent = `${pct}% used`;

    const remaining = Math.max(0, limit - used);
    remainingTxt.textContent = `${remaining.toLocaleString()} fast requests remaining`;

    if (cr.cycle_reset_iso) {
        const resetDate = new Date(cr.cycle_reset_iso);
        resetDateEl.textContent = resetDate.toLocaleDateString();

        const diffDays = Math.ceil((resetDate.getTime() - Date.now()) / (1000 * 60 * 60 * 24));
        daysRemainingEl.textContent = diffDays > 0 ? `${diffDays} days left` : 'Reset pending';
    } else {
        resetDateEl.textContent = 'N/A';
        daysRemainingEl.textContent = '--';
    }

    renderCursorSpend(cr);
}

function renderCursorSpend(cr) {
    const spendSection = document.getElementById('cursor-spend-section');
    if (!spendSection) return;

    if (!cr || !cr.is_authenticated || cr.total_spend_usd === undefined) {
        spendSection.classList.add('hidden');
        return;
    }

    spendSection.classList.remove('hidden');

    // Total spend
    const totalEl = document.getElementById('cursor-total-spend');
    const prevPill = document.getElementById('cursor-prev-spend-pill');
    if (totalEl) {
        totalEl.textContent = `$${cr.total_spend_usd.toFixed(2)}`;
    }

    if (prevPill) {
        if (cr.prev_cycle_spend_usd && cr.prev_cycle_spend_usd > 0) {
            prevPill.textContent = `vs $${cr.prev_cycle_spend_usd.toFixed(2)} last month`;
            prevPill.classList.remove('hidden');
        } else {
            prevPill.classList.add('hidden');
        }
    }

    // Spend SVG Chart
    const areaPath = document.getElementById('spend-chart-area');
    const linePath = document.getElementById('spend-chart-line');
    const dotsGroup = document.getElementById('spend-chart-dots');
    const datesAxis = document.getElementById('chart-dates-axis');
    const lblMax = document.getElementById('chart-lbl-max');
    const lblMid2 = document.getElementById('chart-lbl-mid2');
    const lblMid1 = document.getElementById('chart-lbl-mid1');

    const history = cr.daily_spend || [];
    if (history.length === 0) {
        if (areaPath) areaPath.setAttribute('d', '');
        if (linePath) linePath.setAttribute('d', '');
        if (dotsGroup) dotsGroup.innerHTML = '';
        if (datesAxis) datesAxis.innerHTML = '';
        return;
    }

    // Find max cumulative spend for Y axis scaling
    const maxCum = Math.max(...history.map(d => d.cumulative_usd || 0), 10);
    let yMax = Math.ceil(maxCum / 50) * 50;
    if (yMax < 50) yMax = 50;

    if (lblMax) lblMax.textContent = `$${yMax}`;
    if (lblMid2) lblMid2.textContent = `$${Math.round(yMax * 0.75)}`;
    if (lblMid1) lblMid1.textContent = `$${Math.round(yMax * 0.5)}`;

    // Chart bounds (viewBox 0 0 420 170)
    const chartLeft = 55;
    const chartRight = 405;
    const chartBottom = 145;
    const chartTop = 25;
    const width = chartRight - chartLeft;
    const height = chartBottom - chartTop;

    const n = history.length;
    let pathD = '';
    let areaD = '';
    dotsGroup.innerHTML = '';
    datesAxis.innerHTML = '';

    const points = history.map((pt, i) => {
        const x = n === 1 ? chartLeft + width / 2 : chartLeft + (i / (n - 1)) * width;
        const cum = pt.cumulative_usd || 0;
        const y = chartBottom - (cum / yMax) * height;
        return { x, y, pt };
    });

    if (points.length > 0) {
        pathD = `M ${points[0].x.toFixed(1)} ${points[0].y.toFixed(1)}`;
        areaD = `M ${points[0].x.toFixed(1)} ${chartBottom} L ${points[0].x.toFixed(1)} ${points[0].y.toFixed(1)}`;

        points.forEach((p, idx) => {
            if (idx > 0) {
                pathD += ` L ${p.x.toFixed(1)} ${p.y.toFixed(1)}`;
                areaD += ` L ${p.x.toFixed(1)} ${p.y.toFixed(1)}`;
            }

            // Dot element
            const circle = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
            circle.setAttribute('cx', p.x.toFixed(1));
            circle.setAttribute('cy', p.y.toFixed(1));
            circle.setAttribute('r', '4');
            circle.setAttribute('class', 'chart-dot');

            const title = document.createElementNS('http://www.w3.org/2000/svg', 'title');
            const dayLabel = p.pt.day_str || '';
            title.textContent = `${dayLabel}\nDaily: $${(p.pt.spend_usd || 0).toFixed(2)}\nCumulative: $${(p.pt.cumulative_usd || 0).toFixed(2)}`;
            circle.appendChild(title);
            dotsGroup.appendChild(circle);

            // Date label on X axis
            const shouldShowLabel = (n <= 7) || (idx === 0) || (idx === n - 1) || (idx % Math.ceil(n / 5) === 0);
            if (shouldShowLabel) {
                const dateLbl = document.createElement('span');
                dateLbl.className = 'chart-date-item';
                if (dayLabel) {
                    const parts = dayLabel.split('-');
                    if (parts.length === 3) {
                        const monthNames = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
                        const mIdx = parseInt(parts[1], 10) - 1;
                        const dayNum = parseInt(parts[2], 10);
                        dateLbl.textContent = `${monthNames[mIdx]} ${dayNum}`;
                    } else {
                        dateLbl.textContent = dayLabel;
                    }
                }
                datesAxis.appendChild(dateLbl);
            }
        });

        areaD += ` L ${points[points.length - 1].x.toFixed(1)} ${chartBottom} Z`;

        linePath.setAttribute('d', pathD);
        areaPath.setAttribute('d', areaD);
    }

    // Categories Breakdown
    const catList = document.getElementById('cursor-categories-list');
    const categories = cr.spend_by_category || [];
    if (catList) {
        catList.innerHTML = '';
        const colorPalette = [
            'linear-gradient(90deg, #8b5cf6, #ec4899)',
            'linear-gradient(90deg, #3b82f6, #06b6d4)',
            'linear-gradient(90deg, #10b981, #3b82f6)',
            'linear-gradient(90deg, #f59e0b, #ef4444)',
            'linear-gradient(90deg, #6366f1, #a855f7)',
            'linear-gradient(90deg, #14b8a6, #10b981)'
        ];

        categories.forEach((cat, idx) => {
            const row = document.createElement('div');
            row.className = 'cat-row';
            const color = colorPalette[idx % colorPalette.length];
            const pct = Math.max(0, Math.min(100, cat.percentage || 0));

            row.innerHTML = `
                <div class="cat-info">
                    <span class="cat-name">${cat.category}</span>
                    <span class="cat-val">$${(cat.spend_usd || 0).toFixed(2)} (${pct.toFixed(1)}%)</span>
                </div>
                <div class="cat-bar-track">
                    <div class="cat-bar-fill" style="width: ${pct}%; background: ${color};"></div>
                </div>
            `;
            catList.appendChild(row);
        });
    }
}

async function fetchStatus(isManual = false) {
    const refreshBtn = document.getElementById('refresh-btn');
    if (isManual) {
        refreshBtn.classList.add('spinning');
    }

    try {
        const url = isManual ? '/api/refresh' : '/api/status';
        const method = isManual ? 'POST' : 'GET';
        const res = await fetch(url, { method });
        if (res.ok) {
            const data = await res.json();
            currentStatus = data;
            renderAntigravity(data.antigravity);
            renderCursor(data.cursor);

            const updatedEl = document.getElementById('last-updated');
            const nowTime = new Date().toLocaleTimeString();
            updatedEl.textContent = `Updated: ${nowTime}`;
        }
    } catch (e) {
        console.error('Failed to fetch status:', e);
    } finally {
        if (isManual) {
            setTimeout(() => refreshBtn.classList.remove('spinning'), 500);
        }
    }
}

// -------------------------------------------------------------
// Agent Fleet Tasks Logic
// -------------------------------------------------------------
let currentTasks = [];
let showCompletedTasks = false;

function formatElapsed(startSec, endSec) {
    if (!startSec) return '00m 00s';
    const end = endSec > 0 ? endSec : Math.floor(Date.now() / 1000);
    let diff = Math.max(0, end - startSec);
    const hours = Math.floor(diff / 3600);
    const minutes = Math.floor((diff % 3600) / 60);
    const seconds = diff % 60;
    const pad = (n) => String(n).padStart(2, '0');
    if (hours > 0) {
        return `${pad(hours)}h ${pad(minutes)}m ${pad(seconds)}s`;
    }
    return `${pad(minutes)}m ${pad(seconds)}s`;
}

function formatRelativeTime(epochSec) {
    if (!epochSec) return '-';
    const nowSec = Math.floor(Date.now() / 1000);
    const diff = Math.max(0, nowSec - epochSec);
    if (diff < 5) return 'Just now';
    if (diff < 60) return `${diff}s ago`;
    const min = Math.floor(diff / 60);
    if (min < 60) return `${min}m ago`;
    const hrs = Math.floor(min / 60);
    return `${hrs}h ago`;
}

function escapeHtml(str) {
    if (!str) return '';
    const div = document.createElement('div');
    div.textContent = str;
    return div.innerHTML;
}

function renderTasks(tasks) {
    currentTasks = tasks || [];
    const tbody = document.getElementById('agents-table-body');
    const emptyState = document.getElementById('agents-empty-state');
    const tableContainer = document.getElementById('agents-table-container');
    const countBadge = document.getElementById('agents-count-badge');
    const statusDot = document.getElementById('agents-status-dot');

    if (!tbody) return;

    const visibleTasks = currentTasks.filter(t => {
        if (!showCompletedTasks && (t.status === 'completed' || t.status === 'failed')) {
            return false;
        }
        return true;
    });

    const activeCount = currentTasks.filter(t => t.status === 'running' || t.status === 'waiting_for_user').length;
    if (countBadge) {
        countBadge.textContent = `${activeCount} Active`;
        countBadge.className = activeCount > 0 ? 'badge badge-cyan' : 'badge';
    }

    if (statusDot) {
        if (activeCount > 0) {
            statusDot.className = 'dot-status dot-online';
        } else {
            statusDot.className = 'dot-status';
        }
    }

    if (visibleTasks.length === 0) {
        if (tableContainer) tableContainer.classList.add('hidden');
        if (emptyState) emptyState.classList.remove('hidden');
        tbody.innerHTML = '';
        return;
    }

    if (tableContainer) tableContainer.classList.remove('hidden');
    if (emptyState) emptyState.classList.add('hidden');

    tbody.innerHTML = visibleTasks.map(t => {
        let statusClass = t.status || 'running';
        let statusLabel = 'Running';
        if (t.status === 'waiting_for_user') statusLabel = 'Waiting';
        else if (t.status === 'completed') statusLabel = 'Completed';
        else if (t.status === 'failed') statusLabel = 'Failed';
        else if (t.status === 'stale') statusLabel = 'Stale';
        else if (t.status === 'disconnected') statusLabel = 'Disconnected';

        let agentClass = 'cli';
        const nameLower = (t.agent_name || '').toLowerCase();
        if (nameLower.includes('cursor')) agentClass = 'cursor';
        else if (nameLower.includes('antigravity') || nameLower.includes('gemini')) agentClass = 'antigravity';

        const workspaceHtml = t.workspace ? `<div class="task-workspace">${escapeHtml(t.workspace)}</div>` : '';
        const actionHtml = t.current_action ? `<span class="action-chip" title="${escapeHtml(t.current_action)}">${escapeHtml(t.current_action)}</span>` : '<span style="color:var(--text-muted);">-</span>';
        const durationStr = formatElapsed(t.start_time_epoch, t.completed_time_epoch);
        const lastSeenStr = formatRelativeTime(t.last_heartbeat_epoch);

        return `
            <tr data-task-id="${escapeHtml(t.task_id)}" data-start-epoch="${t.start_time_epoch || 0}" data-end-epoch="${t.completed_time_epoch || 0}">
                <td>
                    <span class="status-pill ${statusClass}">
                        <span class="dot"></span>
                        ${statusLabel}
                    </span>
                </td>
                <td>
                    <span class="badge-agent ${agentClass}">
                        ${escapeHtml(t.agent_name || 'Agent')}
                    </span>
                </td>
                <td class="task-desc-cell">
                    <div class="task-title">${escapeHtml(t.task_description || 'Untitled Task')}</div>
                    ${workspaceHtml}
                </td>
                <td>
                    ${actionHtml}
                </td>
                <td>
                    <span class="duration-counter">${durationStr}</span>
                </td>
                <td>
                    <span class="last-seen-text">${lastSeenStr}</span>
                </td>
            </tr>
        `;
    }).join('');
}

function updateTaskDurations() {
    const rows = document.querySelectorAll('#agents-table-body tr');
    rows.forEach(row => {
        const startEpoch = parseInt(row.getAttribute('data-start-epoch'), 10);
        const endEpoch = parseInt(row.getAttribute('data-end-epoch'), 10);
        if (startEpoch && (!endEpoch || endEpoch <= 0)) {
            const counterEl = row.querySelector('.duration-counter');
            if (counterEl) {
                counterEl.textContent = formatElapsed(startEpoch, 0);
            }
        }
    });
}

async function fetchTasks() {
    try {
        const url = `/api/tasks?include_completed=true`;
        const res = await fetch(url);
        if (!res.ok) return;
        const tasks = await res.json();
        renderTasks(tasks);
    } catch (e) {
        console.warn('Failed to fetch tasks:', e);
    }
}

let currentSessions = [];

function renderSessions(sessions) {
    currentSessions = sessions || [];
    const listEl = document.getElementById('mcp-clients-list');
    if (!listEl) return;

    if (currentSessions.length === 0) {
        listEl.innerHTML = `
            <span class="client-badge client-badge-empty">
                <span class="dot-status dot-offline"></span>
                No active MCP connections
            </span>
        `;
        return;
    }

    listEl.innerHTML = currentSessions.map(s => {
        const nameLower = (s.client_name || '').toLowerCase();
        let brandClass = 'client-generic';
        if (nameLower.includes('cursor')) brandClass = 'client-cursor';
        else if (nameLower.includes('antigravity') || nameLower.includes('gemini')) brandClass = 'client-antigravity';
        else if (nameLower.includes('claude')) brandClass = 'client-claude';

        const verHtml = s.client_version ? `<span class="client-ver">v${escapeHtml(s.client_version)}</span>` : '';
        const ipClean = s.remote_ip ? s.remote_ip.replace(/^::ffff:/, '') : '';
        const ipHtml = ipClean ? `<span class="client-ip" title="Remote IP">${escapeHtml(ipClean)}</span>` : '';
        const uptimeStr = s.connected_time_epoch ? formatElapsed(s.connected_time_epoch, 0) : '';
        const uptimeHtml = uptimeStr ? `<span class="client-uptime" data-connect-epoch="${s.connected_time_epoch}" title="Connected duration">⚡ ${uptimeStr}</span>` : '';

        return `
            <div class="client-badge ${brandClass}" title="Session: ${escapeHtml(s.session_id)}">
                <span class="dot-status dot-online"></span>
                <span class="client-name">${escapeHtml(s.client_name || 'MCP Client')}</span>
                ${verHtml}
                ${ipHtml}
                ${uptimeHtml}
            </div>
        `;
    }).join('');
}

function updateSessionUptimes() {
    const uptimes = document.querySelectorAll('#mcp-clients-list .client-uptime');
    uptimes.forEach(el => {
        const epoch = parseInt(el.getAttribute('data-connect-epoch'), 10);
        if (epoch) {
            el.textContent = `⚡ ${formatElapsed(epoch, 0)}`;
        }
    });
}

async function fetchSessions() {
    try {
        const res = await fetch('/api/sessions');
        if (!res.ok) return;
        const sessions = await res.json();
        renderSessions(sessions);
    } catch (e) {
        console.warn('Failed to fetch sessions:', e);
    }
}

// -------------------------------------------------------------
// Execution Runs & Interlock Logic
// -------------------------------------------------------------
let currentActiveRun = null;
let currentPendingInterlock = null;
let currentTranscriptEvents = [];
let activeRunStartEpoch = 0;

async function fetchExecState() {
    try {
        // 1. Fetch runs list & active run
        const runsRes = await fetch('/api/exec/runs');
        if (runsRes.ok) {
            const runsData = await runsRes.json();
            const activeRunId = runsData.active_run_id;
            const runs = runsData.runs || [];

            if (activeRunId) {
                currentActiveRun = runs.find(r => r.run_id === activeRunId) || { run_id: activeRunId };
            } else if (runs.length > 0) {
                currentActiveRun = runs[0];
            } else {
                currentActiveRun = null;
            }
            renderActiveRun(currentActiveRun, activeRunId);
        }

        // 2. Fetch pending interlocks
        const intkRes = await fetch('/api/exec/interlocks');
        if (intkRes.ok) {
            const interlocks = await intkRes.json();
            currentPendingInterlock = interlocks.length > 0 ? interlocks[0] : null;
            renderInterlockBanner(currentPendingInterlock);
        }

        // 3. Fetch transcript for current run
        if (currentActiveRun && currentActiveRun.run_id) {
            const txRes = await fetch(`/api/exec/runs/${encodeURIComponent(currentActiveRun.run_id)}/transcript`);
            if (txRes.ok) {
                const txData = await txRes.json();
                currentTranscriptEvents = txData.events || [];
                renderTranscript(currentTranscriptEvents);
            }
        } else {
            renderTranscript([]);
        }
    } catch (e) {
        console.warn('Failed to fetch exec state:', e);
    }
}

function renderActiveRun(run, activeRunId) {
    const badgeEl = document.getElementById('exec-run-badge');
    const detailsEl = document.getElementById('exec-run-details');
    const dotEl = document.getElementById('exec-status-dot');

    if (!run) {
        if (badgeEl) {
            badgeEl.textContent = 'No Active Run';
            badgeEl.className = 'badge';
        }
        if (detailsEl) detailsEl.classList.add('hidden');
        if (dotEl) dotEl.className = 'dot-status dot-offline';
        activeRunStartEpoch = 0;
        return;
    }

    const isActive = (run.run_id === activeRunId && (!run.terminal_status || run.terminal_status === 'running'));
    if (dotEl) {
        dotEl.className = isActive ? 'dot-status dot-online' : 'dot-status';
    }

    if (badgeEl) {
        badgeEl.textContent = isActive ? `Active: ${run.run_id}` : `Run: ${run.run_id} (${run.terminal_status || 'ended'})`;
        badgeEl.className = isActive ? 'badge badge-cyan' : 'badge';
    }

    if (detailsEl) {
        detailsEl.classList.remove('hidden');
        const targetEl = document.getElementById('exec-meta-target');
        const planEl = document.getElementById('exec-meta-plan');
        const actorsEl = document.getElementById('exec-meta-actors');
        const durationEl = document.getElementById('exec-meta-duration');

        if (targetEl) targetEl.textContent = run.target_alias || '--';
        if (planEl) planEl.textContent = run.plan_file || '--';
        if (actorsEl) {
            const init = run.initiator_id ? run.initiator_id.replace('agent-', '') : '';
            const exec = run.executor_id ? run.executor_id.replace('agent-', '') : '';
            actorsEl.textContent = `${init} ➔ ${exec}`;
        }
        activeRunStartEpoch = run.start_epoch || 0;
        if (durationEl) {
            durationEl.textContent = formatElapsed(run.start_epoch, run.end_epoch);
        }
    }
}

function renderInterlockBanner(intk) {
    const banner = document.getElementById('interlock-banner');
    if (!banner) return;

    if (!intk) {
        banner.classList.add('hidden');
        return;
    }

    banner.classList.remove('hidden');
    const badgeId = document.getElementById('interlock-id-badge');
    const descText = document.getElementById('interlock-desc-text');
    const actionText = document.getElementById('interlock-action-text');
    const actionBox = document.getElementById('interlock-action-box');
    const rejectBox = document.getElementById('interlock-reject-box');
    const buttonsRow = document.getElementById('interlock-buttons-row');

    if (badgeId) badgeId.textContent = intk.interlock_id || 'intk';
    if (descText) descText.textContent = intk.description || 'No description provided';
    if (actionText) {
        if (intk.proposed_action) {
            actionText.textContent = intk.proposed_action;
            if (actionBox) actionBox.classList.remove('hidden');
        } else {
            if (actionBox) actionBox.classList.add('hidden');
        }
    }

    if (rejectBox) rejectBox.classList.add('hidden');
    if (buttonsRow) buttonsRow.classList.remove('hidden');
}

async function resolveInterlock(approved, reason = '') {
    if (!currentPendingInterlock || !currentPendingInterlock.interlock_id) return;
    const id = currentPendingInterlock.interlock_id;

    try {
        const res = await fetch(`/api/exec/interlocks/${encodeURIComponent(id)}/resolve`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ approved, reason })
        });
        if (res.ok) {
            currentPendingInterlock = null;
            renderInterlockBanner(null);
            fetchExecState();
        } else {
            alert('Failed to resolve interlock: HTTP ' + res.status);
        }
    } catch (e) {
        console.error('Error resolving interlock:', e);
        alert('Network error resolving interlock: ' + e.message);
    }
}

function renderTranscript(events) {
    const countBadge = document.getElementById('transcript-count');
    const stream = document.getElementById('transcript-stream');
    if (!stream) return;

    if (countBadge) {
        countBadge.textContent = `${events.length} event${events.length === 1 ? '' : 's'}`;
    }

    if (!events || events.length === 0) {
        stream.innerHTML = '<div class="transcript-empty">No execution runs or events recorded yet</div>';
        return;
    }

    stream.innerHTML = events.map(ev => {
        const kind = (ev.kind || '').toLowerCase();
        const actor = (ev.actor || '').toLowerCase();
        let actorClass = 'tx-actor-aimon';
        if (actor.includes('gemini') || actor.includes('antigravity')) actorClass = 'tx-actor-gemini';
        else if (actor.includes('cursor')) actorClass = 'tx-actor-cursor';
        else if (actor.includes('human')) actorClass = 'tx-actor-human';

        const seqStr = `#${ev.seq}`;
        const timeStr = ev.ts_epoch ? new Date(ev.ts_epoch * 1000).toLocaleTimeString() : '';
        const cpBadge = ev.checkpoint_id ? `<span class="tx-checkpoint-pill">${escapeHtml(ev.checkpoint_id)}</span>` : '';

        // Decision pill
        let decHtml = '';
        if (ev.decision) {
            const decLower = ev.decision.toLowerCase();
            let decClass = 'tx-dec-continue';
            if (decLower.includes('human')) decClass = 'tx-dec-wait_human';
            else if (decLower.includes('stop')) decClass = 'tx-dec-stop';
            else if (decLower.includes('recover')) decClass = 'tx-dec-recover';
            else if (decLower.includes('approved')) decClass = 'tx-dec-approved';
            else if (decLower.includes('rejected')) decClass = 'tx-dec-rejected';
            decHtml = `<span class="tx-decision-pill ${decClass}">${escapeHtml(ev.decision)}</span>`;
        }

        // Commands list
        let cmdsHtml = '';
        if (Array.isArray(ev.commands) && ev.commands.length > 0) {
            const items = ev.commands.map((cmd, i) => {
                const exitCode = (Array.isArray(ev.exit_codes) && i < ev.exit_codes.length) ? ev.exit_codes[i] : null;
                let exitHtml = '';
                if (exitCode !== null) {
                    const exitClass = exitCode === 0 ? 'exit-ok' : 'exit-err';
                    exitHtml = `<span class="tx-code-exit ${exitClass}">[${exitCode}]</span>`;
                }
                return `<li><code>${escapeHtml(cmd)}</code>${exitHtml}</li>`;
            }).join('');
            cmdsHtml = `<ul class="tx-commands-list">${items}</ul>`;
        }

        // Dmesg / diagnostic excerpt
        let dmesgHtml = '';
        if (ev.dmesg_excerpt) {
            dmesgHtml = `<pre class="tx-dmesg-box">${escapeHtml(ev.dmesg_excerpt)}</pre>`;
        }

        // Notes
        let notesHtml = '';
        if (ev.notes) {
            notesHtml = `<div class="tx-notes-text">${escapeHtml(ev.notes)}</div>`;
        }

        // Proposed next
        let nextHtml = '';
        if (ev.proposal_next) {
            nextHtml = `<div class="tx-notes-text" style="color: #67e8f9;"><strong>Next:</strong> ${escapeHtml(ev.proposal_next)}</div>`;
        }

        return `
            <div class="tx-event-card kind-${escapeHtml(kind)}">
                <div class="tx-event-top">
                    <div class="tx-event-left">
                        <span class="tx-seq">${seqStr}</span>
                        <span class="tx-badge-actor ${actorClass}">${escapeHtml(ev.actor || 'agent')}</span>
                        <span class="tx-badge-kind">${escapeHtml(kind.toUpperCase())}</span>
                        ${cpBadge}
                    </div>
                    <div style="display: flex; align-items: center; gap: 8px;">
                        ${decHtml}
                        <span class="tx-event-time">${timeStr}</span>
                    </div>
                </div>
                ${cmdsHtml}
                ${dmesgHtml}
                ${notesHtml}
                ${nextHtml}
            </div>
        `;
    }).join('');
}

function updateExecDuration() {
    if (activeRunStartEpoch > 0) {
        const durationEl = document.getElementById('exec-meta-duration');
        if (durationEl && currentActiveRun && (!currentActiveRun.terminal_status || currentActiveRun.terminal_status === 'running')) {
            durationEl.textContent = formatElapsed(activeRunStartEpoch, 0);
        }
    }
}

function setupSse() {
    try {
        const source = new EventSource('/sse');
        source.onmessage = (e) => {
            try {
                const data = JSON.parse(e.data);
                if (data.event === 'interlock_request' || data.event === 'interlock_resolved') {
                    fetchExecState();
                }
            } catch (_) {}
        };
        source.onerror = () => {
            // Auto reconnects
        };
    } catch (e) {
        console.warn('SSE connection error:', e);
    }
}

document.addEventListener('DOMContentLoaded', () => {
    fetchStatus();
    fetchTasks();
    fetchSessions();
    fetchExecState();
    setupSse();

    document.getElementById('refresh-btn').addEventListener('click', () => {
        fetchStatus(true);
        fetchTasks();
        fetchSessions();
        fetchExecState();
    });

    // Interlock Buttons
    const btnApprove = document.getElementById('btn-interlock-approve');
    const btnReject = document.getElementById('btn-interlock-reject');
    const btnConfirmReject = document.getElementById('btn-confirm-reject');
    const btnCancelReject = document.getElementById('btn-cancel-reject');
    const rejectBox = document.getElementById('interlock-reject-box');
    const buttonsRow = document.getElementById('interlock-buttons-row');
    const rejectInput = document.getElementById('interlock-reject-reason');

    if (btnApprove) {
        btnApprove.addEventListener('click', () => {
            resolveInterlock(true, '');
        });
    }

    if (btnReject) {
        btnReject.addEventListener('click', () => {
            if (rejectBox) rejectBox.classList.remove('hidden');
            if (buttonsRow) buttonsRow.classList.add('hidden');
            if (rejectInput) {
                rejectInput.value = '';
                rejectInput.focus();
            }
        });
    }

    if (btnConfirmReject) {
        btnConfirmReject.addEventListener('click', () => {
            const reason = rejectInput ? rejectInput.value.trim() : '';
            resolveInterlock(false, reason);
        });
    }

    if (btnCancelReject) {
        btnCancelReject.addEventListener('click', () => {
            if (rejectBox) rejectBox.classList.add('hidden');
            if (buttonsRow) buttonsRow.classList.remove('hidden');
        });
    }

    const toggleCompletedEl = document.getElementById('toggle-completed-tasks');
    if (toggleCompletedEl) {
        toggleCompletedEl.addEventListener('change', (e) => {
            showCompletedTasks = e.target.checked;
            renderTasks(currentTasks);
        });
    }

    const modelsToggleBtn = document.getElementById('ag-models-toggle');
    const modelsChevron = document.getElementById('ag-models-chevron');
    const modelsGridEl = document.getElementById('ag-models-grid');
    if (modelsToggleBtn && modelsGridEl) {
        modelsToggleBtn.addEventListener('click', () => {
            const isHidden = modelsGridEl.classList.toggle('hidden');
            if (modelsChevron) {
                if (isHidden) {
                    modelsChevron.classList.remove('expanded');
                } else {
                    modelsChevron.classList.add('expanded');
                }
            }
        });
    }

    // Refresh data: status every 5s, tasks every 2.5s, sessions every 3s, exec state every 2.5s
    setInterval(() => fetchStatus(false), 5000);
    setInterval(fetchTasks, 2500);
    setInterval(fetchSessions, 3000);
    setInterval(fetchExecState, 2500);

    // Update countdown timers, task stopwatch, session uptime, and exec duration every second
    if (countdownInterval) clearInterval(countdownInterval);
    countdownInterval = setInterval(() => {
        updateCountdowns();
        updateTaskDurations();
        updateSessionUptimes();
        updateExecDuration();
    }, 1000);
});
)raw_asset";

} // namespace assets
} // namespace aimon

#endif // AIMON_WEB_ASSETS_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
