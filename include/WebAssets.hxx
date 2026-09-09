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

                <div class="credits-strip">
                    <div class="credit-pill">
                        <span class="credit-label">Prompt Credits</span>
                        <span id="ag-prompt-credits" class="credit-val">--</span>
                    </div>
                    <div class="credit-pill">
                        <span class="credit-label">Flow Credits</span>
                        <span id="ag-flow-credits" class="credit-val">--</span>
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

                    <div class="details-list">
                        <div class="detail-row">
                            <span class="detail-label">Billing Cycle Reset</span>
                            <span id="cursor-reset-date" class="detail-value">--</span>
                        </div>
                        <div class="detail-row">
                            <span class="detail-label">Days Remaining</span>
                            <span id="cursor-days-remaining" class="detail-value">--</span>
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

                        <!-- Chart Container -->
                        <div class="spend-chart-container">
                            <svg id="cursor-spend-chart" class="spend-chart-svg" viewBox="0 0 420 170">
                                <defs>
                                    <linearGradient id="spendGrad" x1="0" y1="0" x2="0" y2="1">
                                        <stop offset="0%" stop-color="#3b82f6" stop-opacity="0.45"/>
                                        <stop offset="100%" stop-color="#3b82f6" stop-opacity="0.0"/>
                                    </linearGradient>
                                </defs>
                                <!-- Grid Lines & Axis -->
                                <line x1="50" y1="25" x2="410" y2="25" class="chart-grid-line" />
                                <text x="42" y="29" class="chart-axis-lbl" id="chart-lbl-max">$400</text>

                                <line x1="50" y1="65" x2="410" y2="65" class="chart-grid-line" />
                                <text x="42" y="69" class="chart-axis-lbl" id="chart-lbl-mid2">$300</text>

                                <line x1="50" y1="105" x2="410" y2="105" class="chart-grid-line" />
                                <text x="42" y="109" class="chart-axis-lbl" id="chart-lbl-mid1">$200</text>

                                <line x1="50" y1="145" x2="410" y2="145" class="chart-grid-line" />
                                <text x="42" y="149" class="chart-axis-lbl">$0</text>

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
                            <div id="cursor-categories-list" class="categories-list">
                                <!-- Dynamically populated category bars -->
                            </div>
                        </div>
                    </div>
                </div>

                <div id="cursor-error" class="error-banner hidden"></div>
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
    max-width: 1200px;
    padding: 24px 20px 40px;
    display: flex;
    flex-direction: column;
    gap: 28px;
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
    grid-template-columns: repeat(auto-fit, minmax(340px, 1fr));
    gap: 24px;
}

/* Cards */
.glass-card {
    background: var(--bg-card);
    backdrop-filter: blur(16px);
    border: 1px solid var(--border-color);
    border-radius: 18px;
    padding: 24px;
    display: flex;
    flex-direction: column;
    gap: 20px;
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
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 12px;
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
    padding: 20px;
    display: flex;
    flex-direction: column;
    gap: 16px;
    margin-top: 4px;
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
    const promptVal = document.getElementById('ag-prompt-credits');
    const flowVal = document.getElementById('ag-flow-credits');
    const quotaGroupsContainer = document.getElementById('ag-quota-groups');
    const modelsGrid = document.getElementById('ag-models-grid');
    const modelsToggleText = document.getElementById('ag-models-toggle-text');
    const errorBanner = document.getElementById('ag-error');

    if (!ag || !ag.is_running) {
        statusDot.className = 'dot-status dot-offline';
        planBadge.textContent = 'Offline';
        planBadge.className = 'badge';
        promptVal.textContent = '--';
        flowVal.textContent = '--';
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
    promptVal.textContent = (ag.available_prompt_credits || 0).toLocaleString();
    flowVal.textContent = (ag.available_flow_credits || 0).toLocaleString();
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

                    const windowLabel = b.window === 'WEEKLY' ? 'Weekly' : (b.window === 'FIVE_HOUR' ? '5-Hour' : (b.display_name || 'Limit'));

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

document.addEventListener('DOMContentLoaded', () => {
    fetchStatus();

    document.getElementById('refresh-btn').addEventListener('click', () => {
        fetchStatus(true);
    });

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

    // Refresh data every 5 seconds
    setInterval(() => fetchStatus(false), 5000);

    // Update countdown timers every second
    if (countdownInterval) clearInterval(countdownInterval);
    countdownInterval = setInterval(updateCountdowns, 1000);
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
