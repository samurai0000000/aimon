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
    <link rel="stylesheet" href="style.css?v=1.0.5">
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

        <nav class="monitor-nav" id="monitor-tabs" aria-label="System Monitors">
            <button type="button" class="monitor-tab active" data-id="aimon">
                <span class="tab-indicator tab-indicator-online"></span>
                <span class="tab-title">AI Quotas</span>
                <span class="tab-badge">Self</span>
            </button>
        </nav>

        <main class="dashboard-grid view-panel active" id="view-aimon">
            <!-- Google Antigravity Section -->
            <section class="card glass-card antigravity-card">
                <div class="card-header">
                    <div class="provider-title">
                        <span class="dot-status dot-online" id="ag-status-dot"></span>
                        <h2>Google Antigravity</h2>
                    </div>
                    <span id="ag-plan-badge" class="badge badge-cyan">Google AI Ultra</span>
                </div>

                <!-- Credits & Allowances Strip -->
                <div class="credits-strip" id="ag-credits-strip">
                    <div class="credit-pill">
                        <div class="credit-top-row">
                            <span class="credit-label">Google One AI Credits</span>
                            <span class="badge-sub" id="ag-credit-min">Min 50 / req</span>
                        </div>
                        <span id="ag-available-credits" class="credit-val highlight-cyan">--</span>
                    </div>
                    <div class="credit-pill">
                        <div class="credit-top-row">
                            <span class="credit-label">Prompt & Flow Balance</span>
                            <span class="badge-sub" id="ag-credit-tier">Monthly</span>
                        </div>
                        <span id="ag-prompt-flow" class="credit-val">--</span>
                    </div>
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

            <!-- Connected MCP Clients Section -->
            <section class="card glass-card agents-fleet-card">
                <div class="card-header">
                    <div class="provider-title">
                        <span class="dot-status dot-online" id="agents-status-dot"></span>
                        <h2>Connected MCP Clients</h2>
                    </div>
                    <div class="agents-header-actions">
                        <span id="agents-count-badge" class="badge badge-cyan">0 Sessions</span>
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
                            Active Sessions:
                        </span>
                    </div>
                    <div class="mcp-clients-list" id="mcp-clients-list">
                        <span class="client-badge client-badge-empty">
                            <span class="dot-status dot-offline"></span>
                            No active MCP connections
                        </span>
                    </div>
                </div>
            </section>
        </main>

        <!-- Dynamic Discovered Monitor Embedded Panels -->
        <div id="dynamic-panels"></div>

        <footer class="app-footer">
            <p>aimon daemon &bull; Pure C++17 AI Quota Monitor &bull; Local loopback on 127.0.0.1</p>
        </footer>
    </div>

    <script src="app.js?v=1.0.5"></script>
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

.credit-top-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
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

.credit-val.highlight-cyan {
    color: var(--cyan-glow);
    text-shadow: 0 0 16px rgba(0, 242, 254, 0.35);
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

/* Executive KPI Bar */
.exec-kpi-bar {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 12px;
    margin-bottom: 16px;
}

@media (max-width: 900px) {
    .exec-kpi-bar {
        grid-template-columns: repeat(2, 1fr);
    }
}

.kpi-card {
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid var(--border-color);
    border-radius: 10px;
    padding: 10px 14px;
    display: flex;
    flex-direction: column;
    gap: 4px;
}

.kpi-label {
    font-size: 0.72rem;
    color: var(--text-muted);
    text-transform: uppercase;
    letter-spacing: 0.5px;
    font-weight: 600;
}

.kpi-val-row {
    display: flex;
    align-items: baseline;
    gap: 8px;
}

.kpi-val {
    font-size: 1.25rem;
    font-weight: 700;
    font-family: var(--font-mono);
}

.kpi-sub {
    font-size: 0.75rem;
    color: var(--text-secondary);
    font-family: var(--font-mono);
}

.text-cyan { color: #06b6d4; }
.text-green { color: #10b981; }
.text-purple { color: #a855f7; }
.text-magenta { color: #ec4899; }
.text-amber { color: #f59e0b; }

/* Segmented Waterfall Timeline */
.exec-waterfall-section {
    background: rgba(255, 255, 255, 0.02);
    border: 1px solid var(--border-color);
    border-radius: 12px;
    padding: 14px 16px;
    margin-bottom: 16px;
    position: relative;
}

.waterfall-header-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 10px;
    flex-wrap: wrap;
    gap: 8px;
}

.waterfall-title-row {
    display: flex;
    align-items: center;
    gap: 10px;
}

.waterfall-title-row h3 {
    margin: 0;
    font-size: 0.95rem;
    font-weight: 600;
    color: var(--text-primary);
}

.waterfall-legend {
    display: flex;
    align-items: center;
    gap: 14px;
    font-size: 0.75rem;
    color: var(--text-secondary);
}

.legend-item {
    display: flex;
    align-items: center;
    gap: 6px;
}

.legend-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    display: inline-block;
}

.dot-exec, .wf-phase-execution { background: #10b981; }
.dot-review, .wf-phase-review { background: #a855f7; }
.dot-interlock, .wf-phase-interlock { background: #f59e0b; }
.dot-recovery, .wf-phase-recovery { background: #f97316; }
.dot-setup, .wf-phase-setup { background: #64748b; }

.waterfall-bar-track {
    display: flex;
    height: 24px;
    background: rgba(0, 0, 0, 0.35);
    border: 1px solid rgba(255, 255, 255, 0.06);
    border-radius: 6px;
    overflow: hidden;
    position: relative;
}

.waterfall-segment {
    height: 100%;
    position: relative;
    cursor: pointer;
    transition: opacity 0.15s ease, filter 0.15s ease;
    min-width: 4px;
}

.waterfall-segment:hover {
    filter: brightness(1.25);
    z-index: 2;
}

.waterfall-tooltip {
    position: absolute;
    bottom: calc(100% + 8px);
    left: 50%;
    transform: translateX(-50%);
    background: #0f172a;
    border: 1px solid rgba(255, 255, 255, 0.15);
    border-radius: 6px;
    padding: 6px 10px;
    font-size: 0.75rem;
    color: #f8fafc;
    box-shadow: 0 4px 14px rgba(0, 0, 0, 0.5);
    pointer-events: none;
    white-space: nowrap;
    z-index: 10;
}

/* Footer */
.app-footer {
    text-align: center;
    font-size: 0.75rem;
    color: var(--text-muted);
    padding-top: 10px;
}

/* ==========================================================================
   Multi-Monitor Navigation Tabs & Iframe Embed Container
   ========================================================================== */

.monitor-nav {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 8px 12px;
    background: var(--bg-card);
    backdrop-filter: blur(16px);
    border: 1px solid var(--border-color);
    border-radius: 14px;
    overflow-x: auto;
    scrollbar-width: thin;
}

.monitor-tab {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    padding: 8px 16px;
    background: rgba(255, 255, 255, 0.03);
    border: 1px solid transparent;
    border-radius: 10px;
    color: var(--text-secondary);
    font-family: var(--font-sans);
    font-size: 0.85rem;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.2s ease;
    white-space: nowrap;
    user-select: none;
}

.monitor-tab:hover {
    background: rgba(255, 255, 255, 0.07);
    color: var(--text-primary);
    transform: translateY(-1px);
}

.monitor-tab.active {
    background: rgba(0, 242, 254, 0.1);
    border-color: rgba(0, 242, 254, 0.35);
    color: #ffffff;
    box-shadow: 0 0 16px rgba(0, 242, 254, 0.12);
}

.tab-indicator {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    flex-shrink: 0;
}

.tab-indicator-online {
    background: var(--status-online);
    box-shadow: 0 0 8px var(--status-online);
}

.tab-indicator-offline {
    background: var(--status-offline);
    box-shadow: 0 0 8px var(--status-offline);
}

.tab-title {
    font-weight: 600;
}

.tab-badge {
    font-size: 0.7rem;
    font-weight: 500;
    padding: 2px 6px;
    border-radius: 6px;
    background: rgba(255, 255, 255, 0.06);
    color: var(--text-muted);
    font-family: var(--font-mono);
}

.monitor-tab.active .tab-badge {
    background: rgba(0, 242, 254, 0.2);
    color: var(--cyan-glow);
}

/* View panels */
.view-panel {
    width: 100%;
    transition: opacity 0.2s ease;
}

.view-panel.hidden {
    display: none !important;
}

/* Discovered Monitor Frame Container & Panels */
.monitor-frame-container,
.monitor-frame-panel {
    display: flex;
    flex-direction: column;
    background: var(--bg-card);
    backdrop-filter: blur(16px);
    border: 1px solid var(--border-color);
    border-radius: 16px;
    overflow: hidden;
    height: calc(100vh - 190px);
    min-height: 680px;
    box-shadow: 0 16px 36px rgba(0, 0, 0, 0.35);
}

.frame-toolbar {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 12px 18px;
    background: rgba(10, 14, 23, 0.75);
    border-bottom: 1px solid var(--border-color);
    gap: 16px;
}

.frame-info {
    display: flex;
    align-items: center;
    gap: 10px;
    min-width: 0;
}

.frame-title {
    font-size: 0.95rem;
    font-weight: 600;
    color: var(--text-primary);
}

.frame-badge {
    font-size: 0.72rem;
    font-weight: 500;
    padding: 2px 8px;
    border-radius: 6px;
    background: rgba(139, 92, 246, 0.15);
    border: 1px solid rgba(139, 92, 246, 0.3);
    color: var(--purple-glow);
    text-transform: uppercase;
    letter-spacing: 0.03em;
}

.frame-url {
    font-size: 0.78rem;
    font-family: var(--font-mono);
    color: var(--text-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}

.frame-actions {
    display: flex;
    align-items: center;
    gap: 10px;
    flex-shrink: 0;
}

.btn-frame-action {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 6px 12px;
    border-radius: 8px;
    font-size: 0.8rem;
    font-weight: 500;
    font-family: var(--font-sans);
    background: rgba(255, 255, 255, 0.05);
    border: 1px solid var(--border-color);
    color: var(--text-secondary);
    cursor: pointer;
    text-decoration: none;
    transition: all 0.2s ease;
}

.btn-frame-action:hover {
    background: rgba(255, 255, 255, 0.1);
    color: var(--text-primary);
    border-color: rgba(255, 255, 255, 0.15);
}

.frame-content-wrapper {
    position: relative;
    flex: 1;
    display: flex;
    width: 100%;
    height: 100%;
    overflow: hidden;
}

.monitor-iframe {
    width: 100%;
    height: 100%;
    flex: 1;
    border: none;
    background: #0a0e17;
    transition: filter 0.3s ease, opacity 0.3s ease;
}

.monitor-iframe.iframe-grayed-out {
    filter: grayscale(0.85) blur(1.5px);
    opacity: 0.45;
    pointer-events: none;
}

.offline-overlay {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    background: rgba(10, 14, 23, 0.75);
    backdrop-filter: blur(8px);
    z-index: 50;
    animation: fadeIn 0.25s ease forwards;
}

.offline-overlay.hidden {
    display: none !important;
}

.offline-card {
    display: flex;
    flex-direction: column;
    align-items: center;
    text-align: center;
    padding: 32px 40px;
    max-width: 480px;
    background: rgba(17, 24, 39, 0.92);
    border: 1px solid rgba(239, 68, 68, 0.35);
    box-shadow: 0 20px 45px rgba(0, 0, 0, 0.55), 0 0 30px rgba(239, 68, 68, 0.15);
    border-radius: 16px;
    gap: 12px;
}

.offline-icon {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 64px;
    height: 64px;
    border-radius: 50%;
    background: rgba(239, 68, 68, 0.15);
    border: 1px solid rgba(239, 68, 68, 0.35);
    color: #ef4444;
    margin-bottom: 4px;
}

.offline-card h3 {
    font-size: 1.25rem;
    font-weight: 700;
    color: var(--text-primary);
    margin: 0;
}

.offline-desc {
    font-size: 0.88rem;
    color: var(--text-secondary);
    line-height: 1.5;
    margin: 0;
}

.offline-details {
    display: flex;
    flex-direction: column;
    gap: 6px;
    font-size: 0.78rem;
    font-family: var(--font-mono);
    color: var(--text-muted);
    margin-top: 8px;
    background: rgba(0, 0, 0, 0.35);
    padding: 8px 16px;
    border-radius: 8px;
    border: 1px solid rgba(255, 255, 255, 0.05);
}

.offline-details code {
    color: #f87171;
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
        const availableCreditsEl = document.getElementById('ag-available-credits');
        const promptFlowEl = document.getElementById('ag-prompt-flow');
        if (availableCreditsEl) availableCreditsEl.textContent = '--';
        if (promptFlowEl) promptFlowEl.textContent = '--';
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

    // Render Credits & Allowances Strip
    const availableCreditsEl = document.getElementById('ag-available-credits');
    const creditMinEl = document.getElementById('ag-credit-min');
    const promptFlowEl = document.getElementById('ag-prompt-flow');
    const creditTierEl = document.getElementById('ag-credit-tier');

    if (availableCreditsEl) {
        if (ag.available_credits && ag.available_credits.length > 0) {
            const firstCredit = ag.available_credits[0];
            availableCreditsEl.textContent = Number(firstCredit.credit_amount).toLocaleString();
            availableCreditsEl.className = 'credit-val highlight-cyan';
            if (creditMinEl && firstCredit.minimum_credit_amount_for_usage) {
                creditMinEl.textContent = `Min ${firstCredit.minimum_credit_amount_for_usage} / req`;
            }
        } else {
            availableCreditsEl.textContent = '0';
            availableCreditsEl.className = 'credit-val';
        }
    }

    if (promptFlowEl) {
        const availPrompt = ag.available_prompt_credits !== undefined ? ag.available_prompt_credits : 0;
        const availFlow = ag.available_flow_credits !== undefined ? ag.available_flow_credits : 0;
        const monthlyPrompt = ag.monthly_prompt_credits !== undefined ? ag.monthly_prompt_credits : 0;
        const monthlyFlow = ag.monthly_flow_credits !== undefined ? ag.monthly_flow_credits : 0;

        if (availPrompt > 0 || availFlow > 0 || monthlyPrompt > 0) {
            promptFlowEl.textContent = `${availPrompt.toLocaleString()} / ${availFlow.toLocaleString()}`;
            if (creditTierEl && (monthlyPrompt > 0 || monthlyFlow > 0)) {
                creditTierEl.textContent = `Mo: ${(monthlyPrompt / 1000).toFixed(0)}k / ${(monthlyFlow / 1000).toFixed(0)}k`;
            }
        } else {
            promptFlowEl.textContent = '--';
        }
    }

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

//// -------------------------------------------------------------
// MCP Sessions & Client Connections
// -------------------------------------------------------------
let currentSessions = [];

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

function escapeHtml(str) {
    if (!str) return '';
    const div = document.createElement('div');
    div.textContent = str;
    return div.innerHTML;
}

function renderSessions(sessions) {
    currentSessions = sessions || [];
    const listEl = document.getElementById('mcp-clients-list');
    const countBadge = document.getElementById('agents-count-badge');
    const statusDot = document.getElementById('agents-status-dot');

    if (countBadge) {
        countBadge.textContent = `${currentSessions.length} Sessions`;
        countBadge.className = currentSessions.length > 0 ? 'badge badge-cyan' : 'badge';
    }

    if (statusDot) {
        statusDot.className = currentSessions.length > 0 ? 'dot-status dot-online' : 'dot-status';
    }

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

function setupSse() {
    try {
        const source = new EventSource('/sse');
        source.onmessage = (e) => {
            try {
                const data = JSON.parse(e.data);
                if (data.event === 'tools_changed') {
                    fetchSessions();
                    fetchMonitors();
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

// ==========================================================================
// Multi-Monitor Navigation Tabs & Persistent Iframe Controller
// ==========================================================================

let discoveredMonitors = [];
let activeMonitorId = 'aimon';

function formatLastSeen(epoch) {
    if (!epoch || epoch <= 0) return 'Just now';
    const diffSec = Math.floor(Date.now() / 1000) - epoch;
    if (diffSec < 10) return 'Just now';
    if (diffSec < 60) return `${diffSec}s ago`;
    if (diffSec < 3600) return `${Math.floor(diffSec / 60)}m ago`;
    const d = new Date(epoch * 1000);
    return d.toLocaleTimeString();
}

function getResolvedMonitorUrl(m) {
    const protocol = window.location.protocol;
    let host = m.host;
    if (host === '127.0.0.1' || host === 'localhost') {
        host = window.location.hostname;
    }
    const path = m.path ? (m.path.startsWith('/') ? m.path : ('/' + m.path)) : '/';
    return `${protocol}//${host}:${m.port}${path}`;
}

async function fetchMonitors() {
    try {
        const res = await fetch('/api/monitors');
        if (!res.ok) return;
        discoveredMonitors = await res.json();
        renderMonitorTabs(discoveredMonitors);
    } catch (e) {
        console.warn('Failed to fetch monitors:', e);
    }
}

function renderMonitorTabs(monitors) {
    const navEl = document.getElementById('monitor-tabs');
    const dynamicPanelsEl = document.getElementById('dynamic-panels');
    if (!navEl) return;

    // Ensure aimon is present
    let list = Array.isArray(monitors) ? [...monitors] : [];
    if (!list.some(m => m.id === 'aimon')) {
        list.unshift({
            id: 'aimon',
            name: 'AI Quotas',
            short_name: 'AI Quotas',
            subsystem: 'aimon',
            host: '127.0.0.1',
            port: window.location.port || 3883,
            path: '/',
            connected: true,
            reachable: true,
            is_self: true,
            priority: 0
        });
    }

    list.sort((a, b) => {
        const pa = (a.priority !== undefined) ? a.priority : 100;
        const pb = (b.priority !== undefined) ? b.priority : 100;
        if (pa !== pb) return pa - pb;
        const sa = a.subsystem || '';
        const sb = b.subsystem || '';
        if (sa !== sb) return sa.localeCompare(sb);
        const ha = a.host || '';
        const hb = b.host || '';
        if (ha !== hb) return ha.localeCompare(hb);
        return (a.port || 0) - (b.port || 0);
    });

    // Render navigation tabs
    navEl.innerHTML = list.map(m => {
        const isActive = (m.id === activeMonitorId);
        const activeClass = isActive ? ' active' : '';
        const isOnline = Boolean(m.connected && m.reachable);
        const indClass = isOnline ? 'tab-indicator-online' : 'tab-indicator-offline';
        const badgeText = (m.is_self || m.isSelf) ? 'Self' : (m.subsystem || `${m.port}`);
        const displayLabel = m.short_name || m.name || m.subsystem || m.id;

        return `
            <button type="button" class="monitor-tab${activeClass}" data-id="${escapeHtml(m.id)}" title="${escapeHtml(m.name || m.id)} (${m.host}:${m.port})">
                <span class="tab-indicator ${indClass}"></span>
                <span class="tab-title">${escapeHtml(displayLabel)}</span>
                <span class="tab-badge">${escapeHtml(badgeText)}</span>
            </button>
        `;
    }).join('');

    // Attach tab click handlers
    navEl.querySelectorAll('.monitor-tab').forEach(tabBtn => {
        tabBtn.addEventListener('click', () => {
            const id = tabBtn.getAttribute('data-id');
            switchToMonitor(id);
        });
    });

    // Synchronize persistent iframe panels in #dynamic-panels
    if (dynamicPanelsEl) {
        // Prune orphaned panels
        const activePanelIds = new Set(list.filter(m => !m.is_self && !m.isSelf).map(m => `panel-${m.id}`));
        dynamicPanelsEl.querySelectorAll('.monitor-frame-panel').forEach(p => {
            if (!activePanelIds.has(p.id)) {
                p.remove();
            }
        });

        list.forEach(m => {
            if (m.is_self || m.isSelf) return;

            let panel = document.getElementById(`panel-${m.id}`);
            const targetUrl = getResolvedMonitorUrl(m);
            const isOnline = Boolean(m.connected && m.reachable);

            if (!panel) {
                panel = document.createElement('div');
                panel.id = `panel-${m.id}`;
                panel.className = `monitor-frame-panel view-panel${m.id === activeMonitorId ? '' : ' hidden'}`;
                panel.innerHTML = `
                    <div class="frame-toolbar">
                        <div class="frame-info">
                            <span class="dot-status ${isOnline ? 'dot-online' : 'dot-offline'}" id="dot-${m.id}"></span>
                            <span class="frame-title">${escapeHtml(m.name || m.id)}</span>
                            <span class="frame-badge">${escapeHtml(m.subsystem || 'Satellite')}</span>
                            <span class="frame-url">${escapeHtml(targetUrl)}</span>
                        </div>
                        <div class="frame-actions">
                            <button type="button" class="btn-frame-action btn-panel-reload" data-id="${m.id}" title="Reload Tab">
                                <svg viewBox="0 0 24 24" width="14" height="14" stroke="currentColor" stroke-width="2" fill="none">
                                    <path d="M23 4v6h-6"></path>
                                    <path d="M1 20v-6h6"></path>
                                    <path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"></path>
                                </svg>
                                Reload
                            </button>
                            <a href="${escapeHtml(targetUrl)}" target="_blank" rel="noopener noreferrer" class="btn-frame-action" title="Open in new window">
                                <svg viewBox="0 0 24 24" width="14" height="14" stroke="currentColor" stroke-width="2" fill="none">
                                    <path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"></path>
                                    <polyline points="15 3 21 3 21 9"></polyline>
                                    <line x1="10" y1="14" x2="21" y2="3"></line>
                                </svg>
                                Open in New Window
                            </a>
                        </div>
                    </div>
                    <div class="frame-content-wrapper">
                        <iframe class="monitor-iframe ${isOnline ? '' : 'iframe-grayed-out'}" src="${escapeHtml(targetUrl)}" title="${escapeHtml(m.name)}"></iframe>
                        <div class="offline-overlay ${isOnline ? 'hidden' : ''}" id="overlay-${m.id}">
                            <div class="offline-card">
                                <div class="offline-icon">
                                    <svg viewBox="0 0 24 24" width="36" height="36" stroke="currentColor" stroke-width="2" fill="none">
                                        <circle cx="12" cy="12" r="10"></circle>
                                        <line x1="12" y1="8" x2="12" y2="12"></line>
                                        <line x1="12" y1="16" x2="12.01" y2="16"></line>
                                    </svg>
                                </div>
                                <h3>${escapeHtml(m.name)} is Offline</h3>
                                <p class="offline-desc">Subsystem connection to aimon has been disconnected.</p>
                                <div class="offline-details">
                                    <span>Host: <code>${escapeHtml(m.host)}:${m.port}</code></span>
                                    <span class="offline-last-seen" id="last-seen-${m.id}">Last seen: ${formatLastSeen(m.last_seen_epoch)}</span>
                                </div>
                                <button type="button" class="btn-frame-action btn-overlay-reload" data-id="${m.id}" style="margin-top: 8px;">
                                    <svg viewBox="0 0 24 24" width="14" height="14" stroke="currentColor" stroke-width="2" fill="none">
                                        <path d="M23 4v6h-6"></path>
                                        <path d="M1 20v-6h6"></path>
                                        <path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"></path>
                                    </svg>
                                    Reload Tab
                                </button>
                            </div>
                        </div>
                    </div>
                `;
                dynamicPanelsEl.appendChild(panel);

                const attachReload = (btn) => {
                    if (!btn) return;
                    btn.addEventListener('click', () => {
                        const iframe = panel.querySelector('iframe');
                        if (iframe) {
                            try {
                                iframe.contentWindow.location.reload();
                            } catch (_) {
                                const s = iframe.src;
                                iframe.src = '';
                                iframe.src = s;
                            }
                        }
                    });
                };
                attachReload(panel.querySelector('.btn-panel-reload'));
                attachReload(panel.querySelector('.btn-overlay-reload'));
            } else {
                // Update existing panel state
                const dot = panel.querySelector(`#dot-${m.id}`);
                if (dot) {
                    dot.className = isOnline ? 'dot-status dot-online' : 'dot-status dot-offline';
                }
                const iframe = panel.querySelector('iframe');
                if (iframe) {
                    if (isOnline) {
                        iframe.classList.remove('iframe-grayed-out');
                    } else {
                        iframe.classList.add('iframe-grayed-out');
                    }
                }
                const overlay = panel.querySelector(`#overlay-${m.id}`);
                if (overlay) {
                    if (isOnline) {
                        overlay.classList.add('hidden');
                    } else {
                        overlay.classList.remove('hidden');
                    }
                }
                const lastSeenEl = panel.querySelector(`#last-seen-${m.id}`);
                if (lastSeenEl && m.last_seen_epoch) {
                    lastSeenEl.textContent = `Last seen: ${formatLastSeen(m.last_seen_epoch)}`;
                }
            }
        });

        if (window.location.hash) {
            const hash = window.location.hash.replace(/^#/, '');
            const target = monitors.find(m => m.id === hash || m.subsystem === hash);
            if (target && activeMonitorId !== target.id) {
                switchToMonitor(target.id);
            }
        }
    }
}

function switchToMonitor(id) {
    activeMonitorId = id;
    if (window.location.hash !== '#' + id) {
        try {
            history.replaceState(null, '', '#' + id);
        } catch (_) {}
    }

    // Update active tab styles
    const navEl = document.getElementById('monitor-tabs');
    if (navEl) {
        navEl.querySelectorAll('.monitor-tab').forEach(tab => {
            if (tab.getAttribute('data-id') === id) {
                tab.classList.add('active');
            } else {
                tab.classList.remove('active');
            }
        });
    }

    const viewAimon = document.getElementById('view-aimon');
    if (id === 'aimon') {
        if (viewAimon) viewAimon.classList.remove('hidden');
    } else {
        if (viewAimon) viewAimon.classList.add('hidden');
    }

    // Toggle persistent panels
    const dynamicPanelsEl = document.getElementById('dynamic-panels');
    if (dynamicPanelsEl) {
        dynamicPanelsEl.querySelectorAll('.monitor-frame-panel').forEach(panel => {
            if (panel.id === `panel-${id}`) {
                panel.classList.remove('hidden');
            } else {
                panel.classList.add('hidden');
            }
        });
    }
}

window.addEventListener('hashchange', () => {
    const hash = window.location.hash.replace(/^#/, '');
    if (hash) {
        switchToMonitor(hash);
    } else {
        switchToMonitor('aimon');
    }
});

document.addEventListener('DOMContentLoaded', () => {
    fetchStatus();
    fetchSessions();
    fetchMonitors();
    setupSse();

    document.getElementById('refresh-btn').addEventListener('click', () => {
        fetchStatus(true);
        fetchSessions();
        fetchMonitors();
    });

    const reloadBtn = document.getElementById('frame-reload-btn');
    if (reloadBtn) {
        reloadBtn.addEventListener('click', () => {
            const iframe = document.getElementById('monitor-iframe');
            if (iframe) {
                try {
                    iframe.contentWindow.location.reload();
                } catch (_) {
                    const src = iframe.src;
                    iframe.src = '';
                    iframe.src = src;
                }
            }
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

    // Refresh data: status every 5s, sessions every 3s, monitors every 8s
    setInterval(() => fetchStatus(false), 5000);
    setInterval(fetchSessions, 3000);
    setInterval(fetchMonitors, 8000);

    // Update countdown timers and session uptime every second
    if (countdownInterval) clearInterval(countdownInterval);
    countdownInterval = setInterval(() => {
        updateCountdowns();
        updateSessionUptimes();
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
