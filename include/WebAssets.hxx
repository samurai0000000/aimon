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
    <link rel="stylesheet" href="style.css?v=1.0.8">
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&family=JetBrains+Mono:wght@400;500;600&display=swap" rel="stylesheet">
</head>
<body>
    <div class="app-container">
        <nav class="monitor-nav" id="monitor-tabs" aria-label="System Monitors">
            <button type="button" class="monitor-tab active" data-id="aimon">
                <span class="tab-indicator tab-indicator-online"></span>
                <span class="tab-title">aimon</span>
            </button>
            <div id="satellite-tabs" class="satellite-tabs-row"></div>
        </nav>

        <div class="main-viewport" id="main-viewport">
            <!-- Native Aimon Monitor Panel -->
            <main class="view-panel active" id="view-aimon">
            <!-- Aimon Sub-Navigation Tab Bar -->
            <div class="aimon-subnav-bar" id="aimon-subnav" aria-label="aimon Subpanels">
                <button type="button" class="aimon-subnav-tab active" data-subpanel="quotas">
                    <span class="subnav-indicator"></span>
                    <span class="subnav-title">ai quotas</span>
                </button>
                <button type="button" class="aimon-subnav-tab" data-subpanel="telemetry">
                    <span class="subnav-indicator"></span>
                    <span class="subnav-title">agent telemetry</span>
                </button>
                <button type="button" class="aimon-subnav-tab" data-subpanel="approvals">
                    <span class="subnav-indicator"></span>
                    <span class="subnav-title">action approvals</span>
                    <span class="tab-badge badge-amber hidden" id="approvals-badge">0</span>
                </button>
            </div>

            <!-- Subpanel 1: AI Quotas (Antigravity, Cursor, MCP Clients) -->
            <div class="aimon-subpanel dashboard-grid active" id="subpanel-quotas">
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
            </div>

            <!-- Subpanel 2: Agent Telemetry & Analytics -->
            <div class="aimon-subpanel hidden" id="subpanel-telemetry">
                <!-- Telemetry Header & Timeframe Bar -->
                <div class="telemetry-header-bar glass-card">
                    <div class="telemetry-title-group">
                        <h2>Agent Execution Telemetry & Historical Analytics</h2>
                        <span class="badge badge-cyan" id="telem-db-badge">SQLite WAL (Rolling)</span>
                    </div>
                    <div class="timeframe-selector" id="telem-timeframe-selector">
                        <button type="button" class="time-btn" data-window="1h">1H</button>
                        <button type="button" class="time-btn active" data-window="24h">24H</button>
                        <button type="button" class="time-btn" data-window="7d">7D</button>
                        <button type="button" class="time-btn" data-window="30d">30D</button>
                        <button type="button" class="time-btn" data-window="1y">1Y</button>
                    </div>
                </div>

                <!-- KPI Summary Cards -->
                <div class="telemetry-kpi-grid">
                    <div class="glass-card kpi-card">
                        <div class="kpi-label">Active Agent Sessions</div>
                        <div class="kpi-val" id="kpi-active-sessions">0 <span class="kpi-sub">running</span></div>
                    </div>
                    <div class="glass-card kpi-card">
                        <div class="kpi-label">Total Tool Calls</div>
                        <div class="kpi-val highlight-cyan" id="kpi-total-tools">0 <span class="kpi-sub">calls</span></div>
                    </div>
                    <div class="glass-card kpi-card">
                        <div class="kpi-label">Avg Step Duration</div>
                        <div class="kpi-val" id="kpi-avg-latency">0.00 <span class="kpi-sub">sec</span></div>
                    </div>
                    <div class="glass-card kpi-card">
                        <div class="kpi-label">Tool Error Rate</div>
                        <div class="kpi-val highlight-amber" id="kpi-error-rate">0.0 <span class="kpi-sub">%</span></div>
                    </div>
                </div>

                <!-- Telemetry Charts Grid (2x2 Layout) -->
                <div class="telemetry-charts-grid">
                    <!-- Chart 1: Tool Invocations & Active Workload Velocity -->
                    <div class="glass-card chart-card">
                        <div class="chart-header">
                            <div class="chart-title">Tool Invocations & Workload Velocity</div>
                            <div class="chart-legend">
                                <span class="legend-item"><i class="dot dot-cyan"></i> Tool Calls/s</span>
                                <span class="legend-item"><i class="dot dot-magenta"></i> Active Sessions</span>
                            </div>
                        </div>
                        <div class="svg-chart-container" id="token-velocity-chart">
                            <svg class="metric-svg" id="token-svg" viewBox="0 0 800 220"></svg>
                        </div>
                    </div>

                    <!-- Chart 2: Step Duration & Peak Envelope -->
                    <div class="glass-card chart-card">
                        <div class="chart-header">
                            <div>
                                <div class="chart-title">Step Duration & Latency Envelope</div>
                                <p class="chart-subtitle">P95 variance (&lt;3s Normal, 3-8s Moderate, &gt;8s Degraded)</p>
                            </div>
                            <div class="chart-legend">
                                <span class="legend-item"><i class="dot dot-blue"></i> Avg Duration</span>
                                <span class="legend-item"><i class="dot dot-amber"></i> P95 Envelope</span>
                            </div>
                        </div>
                        <div class="svg-chart-container" id="latency-chart">
                            <svg class="metric-svg" id="latency-svg" viewBox="0 0 800 220"></svg>
                        </div>
                    </div>

                    <!-- Chart 3: Agent Activity & Task Execution Timeline (Gantt Swimlanes) -->
                    <div class="glass-card chart-card">
                        <div class="chart-header">
                            <div>
                                <div class="chart-title">Agent Activity & Task Execution Timeline</div>
                                <p class="chart-subtitle">Session run intervals, active execution times & status</p>
                            </div>
                            <div class="chart-legend">
                                <span class="legend-item"><i class="dot dot-emerald"></i> Active</span>
                                <span class="legend-item"><i class="dot dot-cyan"></i> Done</span>
                                <span class="legend-item"><i class="dot dot-coral"></i> Error</span>
                            </div>
                        </div>
                        <div class="svg-chart-container" id="activity-gantt-chart">
                            <svg class="metric-svg" id="activity-gantt-svg" viewBox="0 0 800 220"></svg>
                        </div>
                    </div>

                    <!-- Chart 4: Agent Busyness & Concurrency Stack -->
                    <div class="glass-card chart-card">
                        <div class="chart-header">
                            <div>
                                <div class="chart-title">Agent Busyness & Concurrency Stack</div>
                                <p class="chart-subtitle">Concurrent active agents and active workload duty cycle</p>
                            </div>
                            <div class="chart-legend">
                                <span class="legend-item"><i class="dot dot-cyan"></i> Antigravity</span>
                                <span class="legend-item"><i class="dot dot-purple"></i> Cursor</span>
                            </div>
                        </div>
                        <div class="svg-chart-container" id="busyness-stack-chart">
                            <svg class="metric-svg" id="busyness-stack-svg" viewBox="0 0 800 220"></svg>
                        </div>
                    </div>
                </div>

                <!-- Tool Invocation Matrix Card -->
                <div class="glass-card tool-matrix-card">
                    <div class="card-header">
                        <h2>Tool Invocations by Category</h2>
                        <span class="badge badge-cyan" id="tools-total-badge">0 Calls</span>
                    </div>
                    <div class="tool-matrix-body" id="tool-matrix-container">
                        <div class="loading-placeholder">Loading tool execution metrics...</div>
                    </div>
                </div>

                <!-- Live Agent Lifecycle Waterfall -->
                <div class="glass-card waterfall-card">
                    <div class="card-header">
                        <h2>Live Agent Lifecycle Waterfall & Turns</h2>
                        <div class="waterfall-controls">
                            <select id="waterfall-session-select" class="session-dropdown">
                                <option value="">Select an Agent Session...</option>
                            </select>
                        </div>
                    </div>
                    <div class="waterfall-timeline-container" id="waterfall-container">
                        <div class="waterfall-empty-hint">Select a session above to inspect turn breakdown, tool executions, and approval wait times.</div>
                    </div>
                </div>
            </div>

            <!-- Subpanel 3: Action Approvals & Mobile Companion -->
            <div class="aimon-subpanel hidden" id="subpanel-approvals">
                <div class="approvals-grid">
                    <!-- Pending Approvals Card -->
                    <div class="glass-card pending-approvals-card">
                        <div class="card-header">
                            <h2>Pending Action Approvals</h2>
                            <span class="badge badge-amber" id="pending-approvals-count">0 Pending</span>
                        </div>
                        <div class="pending-list" id="pending-approvals-list">
                            <div class="no-pending-hint">
                                <svg viewBox="0 0 24 24" width="28" height="28" stroke="currentColor" stroke-width="1.8" fill="none">
                                    <circle cx="12" cy="12" r="10"></circle>
                                    <polyline points="12 6 12 12 14 14"></polyline>
                                </svg>
                                <p>No actions currently waiting for approval.</p>
                            </div>
                        </div>
                    </div>

                    <!-- Mobile Companion Pairing Card -->
                    <div class="glass-card mobile-pairing-card">
                        <div class="card-header">
                            <h2>Mobile Companion Pairing</h2>
                            <span class="badge badge-cyan">WireGuard / Tailscale / LAN</span>
                        </div>
                        <div class="pairing-content">
                            <div class="qr-placeholder-box" id="qr-container-box">
                                <div class="qr-code-canvas-box" id="pairing-qr-svg"></div>
                                <div class="secret-code-display" id="pairing-secret-display">Generating...</div>
                                <div class="pairing-countdown" id="pairing-countdown">Valid for 300s</div>
                            </div>
                            <div class="pairing-instructions">
                                <h3>How to Pair Your Android Phone:</h3>
                                <ol>
                                    <li>Launch <strong>aimon Companion</strong> on your Android device.</li>
                                    <li>Tap <strong>Scan QR / Enter Secret</strong> and point your camera at the QR code above.</li>
                                    <li>Approvals for sensitive tools (<code>run_command</code>, <code>write_to_file</code>) will vibrate directly on your lock screen with <code>[Approve]</code> and <code>[Deny]</code> action buttons.</li>
                                </ol>
                                <div class="pairing-action-row">
                                    <button type="button" class="btn-refresh" id="btn-new-qr">Regenerate Secret</button>
                                    <a href="/download/aimon-companion.apk" class="btn-download-apk" download title="Download compiled Android companion app">
                                        <svg viewBox="0 0 24 24" width="16" height="16" stroke="currentColor" stroke-width="2" fill="none">
                                            <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"></path>
                                            <polyline points="7 10 12 15 17 10"></polyline>
                                            <line x1="12" y1="15" x2="12" y2="3"></line>
                                        </svg>
                                        Download Android APK
                                    </a>
                                </div>
                            </div>
                        </div>
                    </div>

                    <!-- Connected Devices Card -->
                    <div class="glass-card devices-card">
                        <div class="card-header">
                            <h2>Paired Mobile Devices</h2>
                        </div>
                        <div class="devices-list" id="paired-devices-list">
                            <div class="loading-placeholder">Loading paired devices...</div>
                        </div>
                    </div>
                </div>
            </div>
        </main>

        <!-- Dynamic Discovered Satellite Panels -->
        <div id="dynamic-panels"></div>
    </div>

        <footer class="app-footer">
            <p>aimon daemon &bull; Pure C++17 AI Quota Monitor &bull; Local loopback on 127.0.0.1</p>
        </footer>
    </div>

    <script src="qrcode.js"></script>
    <script src="app.js?v=1.0.8"></script>
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
    height: 100vh;
    margin: 0;
    padding: 0;
    display: flex;
    justify-content: center;
    -webkit-font-smoothing: antialiased;
    overflow: hidden;
}

.app-container {
    width: 100%;
    max-width: 1600px;
    height: 100vh;
    padding: 12px 20px 6px;
    display: flex;
    flex-direction: column;
    box-sizing: border-box;
    overflow: hidden;
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

/* ==========================================================================
   Top Navigation Bar & Multi-Monitor Switching
   ========================================================================== */

.monitor-nav {
    flex-shrink: 0;
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 8px 12px;
    background: var(--bg-card);
    backdrop-filter: blur(16px);
    border: 1px solid var(--border-color);
    border-radius: 14px;
    margin-bottom: 14px;
    overflow-x: auto;
    white-space: nowrap;
    -webkit-overflow-scrolling: touch;
    scrollbar-width: none;
}

.monitor-nav::-webkit-scrollbar {
    display: none;
}

.monitor-tab {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    padding: 7px 16px;
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
    flex-shrink: 0;
    text-transform: lowercase;
}

.monitor-tab:hover {
    background: rgba(255, 255, 255, 0.07);
    color: var(--text-primary);
    transform: translateY(-1px);
}

.monitor-tab.active {
    background: rgba(0, 242, 254, 0.12);
    border-color: rgba(0, 242, 254, 0.35);
    color: #ffffff;
    box-shadow: 0 0 14px rgba(0, 242, 254, 0.12);
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
    text-transform: lowercase;
}

.tab-badge {
    font-size: 0.68rem;
    font-weight: 600;
    padding: 1px 5px;
    border-radius: 4px;
    background: rgba(255, 255, 255, 0.08);
    color: var(--text-muted);
    font-family: var(--font-mono);
    text-transform: lowercase;
}

.monitor-tab.active .tab-badge {
    background: rgba(0, 242, 254, 0.2);
    color: var(--cyan-glow);
}

/* Aimon Sub-Navigation Tab Bar & Subpanels */
.aimon-subnav-bar {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 6px 10px;
    background: rgba(13, 19, 33, 0.65);
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 12px;
    margin-bottom: 18px;
    overflow-x: auto;
    white-space: nowrap;
    -webkit-overflow-scrolling: touch;
    scrollbar-width: thin;
    flex-wrap: nowrap;
}

.aimon-subnav-tab {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    padding: 7px 14px;
    background: rgba(255, 255, 255, 0.03);
    border: 1px solid transparent;
    border-radius: 8px;
    color: var(--text-secondary);
    font-family: var(--font-sans);
    font-size: 0.82rem;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.2s ease;
    white-space: nowrap;
    user-select: none;
    flex-shrink: 0;
    text-transform: lowercase;
}

.aimon-subnav-tab:hover {
    background: rgba(255, 255, 255, 0.07);
    color: var(--text-primary);
    transform: translateY(-1px);
}

.aimon-subnav-tab.active {
    background: rgba(0, 242, 254, 0.12);
    border-color: rgba(0, 242, 254, 0.35);
    color: #ffffff;
    box-shadow: 0 0 14px rgba(0, 242, 254, 0.12);
}

.aimon-subnav-tab .subnav-indicator {
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: rgba(255, 255, 255, 0.25);
    transition: all 0.2s ease;
}

.aimon-subnav-tab.active .subnav-indicator {
    background: var(--cyan-glow);
    box-shadow: 0 0 8px var(--cyan-glow);
}

.aimon-subnav-tab .tab-badge {
    font-size: 0.68rem;
    padding: 1px 5px;
    border-radius: 4px;
    background: rgba(255, 255, 255, 0.06);
    color: var(--text-muted);
}

.aimon-subnav-tab.active .tab-badge {
    background: rgba(0, 242, 254, 0.2);
    color: var(--cyan-glow);
}

.aimon-subpanel {
    display: none;
    width: 100%;
}

.aimon-subpanel.active {
    display: block;
}

.aimon-subpanel.dashboard-grid.active {
    display: grid;
}

.aimon-subpanel.hidden,
.aimon-subpanel:not(.active) {
    display: none !important;
}

/* ==========================================================================
   Main Viewport & View Panels Architecture
   ========================================================================== */

.main-viewport {
    flex: 1;
    min-height: 0;
    position: relative;
    overflow: hidden;
    width: 100%;
}

.view-panel {
    display: none;
    width: 100%;
    height: 100%;
    overflow-y: auto;
    overflow-x: hidden;
    padding-right: 4px;
    scrollbar-width: thin;
}

.view-panel.active {
    display: block;
}

.view-panel.hidden {
    display: none !important;
}

.view-panel::-webkit-scrollbar {
    width: 6px;
}

.view-panel::-webkit-scrollbar-thumb {
    background: rgba(255, 255, 255, 0.15);
    border-radius: 3px;
}

.view-panel::-webkit-scrollbar-track {
    background: transparent;
}

#dynamic-panels {
    width: 100%;
    height: 100%;
}

/* Discovered Monitor Frame Container & Panels (Zero Parent Scrollbars) */
.monitor-frame-panel {
    display: none;
    flex-direction: column;
    width: 100%;
    height: 100%;
    background: var(--bg-card);
    backdrop-filter: blur(16px);
    border: 1px solid var(--border-color);
    border-radius: 14px;
    overflow: hidden;
    box-shadow: 0 16px 36px rgba(0, 0, 0, 0.35);
}

.monitor-frame-panel.active {
    display: flex !important;
}

.monitor-frame-panel.hidden,
.monitor-frame-panel:not(.active) {
    display: none !important;
}

.frame-toolbar {
    flex-shrink: 0;
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 8px 14px;
    background: rgba(10, 14, 23, 0.75);
    border-bottom: 1px solid var(--border-color);
    gap: 12px;
    height: 38px;
    box-sizing: border-box;
}

.frame-info {
    display: flex;
    align-items: center;
    gap: 8px;
    min-width: 0;
}

.frame-title {
    font-size: 0.88rem;
    font-weight: 600;
    color: var(--text-primary);
    text-transform: lowercase;
}

.frame-badge {
    font-size: 0.68rem;
    font-weight: 500;
    padding: 1px 6px;
    border-radius: 4px;
    background: rgba(139, 92, 246, 0.15);
    border: 1px solid rgba(139, 92, 246, 0.3);
    color: var(--purple-glow);
    text-transform: lowercase;
    letter-spacing: 0.02em;
}

.frame-url {
    font-size: 0.74rem;
    font-family: var(--font-mono);
    color: var(--text-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}

.frame-actions {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-shrink: 0;
}

.btn-frame-action {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    padding: 4px 10px;
    border-radius: 6px;
    font-size: 0.76rem;
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
    min-height: 0;
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
    display: block;
}

.monitor-iframe.iframe-grayed-out {
    filter: grayscale(0.85) blur(1.5px);
    opacity: 0.45;
    pointer-events: none;
}

/* Footer */
.app-footer {
    flex-shrink: 0;
    text-align: center;
    font-size: 0.72rem;
    color: var(--text-muted);
    padding: 4px 0 2px;
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

/* ==========================================================================
   Agent Telemetry & Historical Analytics Styles
   ========================================================================== */

.telemetry-header-bar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 16px 24px;
    margin-bottom: 20px;
    background: rgba(15, 23, 42, 0.65);
    backdrop-filter: blur(12px);
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 14px;
}

.telemetry-title-group {
    display: flex;
    align-items: center;
    gap: 12px;
}

.telemetry-title-group h2 {
    font-size: 1.15rem;
    font-weight: 700;
    color: var(--text-primary);
    margin: 0;
}

.timeframe-selector {
    display: flex;
    gap: 4px;
    background: rgba(0, 0, 0, 0.35);
    padding: 4px;
    border-radius: 10px;
    border: 1px solid rgba(255, 255, 255, 0.06);
}

.time-btn {
    background: transparent;
    border: none;
    color: var(--text-muted);
    font-family: var(--font-mono);
    font-size: 0.8rem;
    font-weight: 600;
    padding: 6px 14px;
    border-radius: 6px;
    cursor: pointer;
    transition: all 0.2s ease;
}

.time-btn:hover {
    color: var(--text-primary);
    background: rgba(255, 255, 255, 0.05);
}

.time-btn.active {
    background: rgba(6, 182, 212, 0.25);
    color: #38bdf8;
    border: 1px solid rgba(56, 189, 248, 0.4);
    box-shadow: 0 0 12px rgba(6, 182, 212, 0.2);
}

/* KPI Summary Cards */
.telemetry-kpi-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
    gap: 16px;
    margin-bottom: 20px;
}

.kpi-card {
    padding: 18px 20px;
    background: rgba(15, 23, 42, 0.6);
    backdrop-filter: blur(12px);
    border: 1px solid rgba(255, 255, 255, 0.07);
    border-radius: 14px;
    display: flex;
    flex-direction: column;
    gap: 8px;
}

.kpi-label {
    font-size: 0.82rem;
    font-weight: 500;
    color: var(--text-muted);
    text-transform: uppercase;
    letter-spacing: 0.05em;
}

.kpi-val {
    font-size: 1.65rem;
    font-weight: 700;
    font-family: var(--font-mono);
    color: var(--text-primary);
    display: flex;
    align-items: baseline;
    gap: 6px;
}

.kpi-sub {
    font-size: 0.85rem;
    font-weight: 400;
    color: var(--text-muted);
}

/* Charts Grid (2x2 Layout) */
.telemetry-charts-grid {
    display: grid;
    grid-template-columns: repeat(2, 1fr);
    gap: 16px;
    margin-bottom: 20px;
}

@media (max-width: 1024px) {
    .telemetry-charts-grid {
        grid-template-columns: 1fr;
    }
}

.chart-card {
    padding: 18px 20px;
    background: rgba(15, 23, 42, 0.6);
    backdrop-filter: blur(12px);
    border: 1px solid rgba(255, 255, 255, 0.07);
    border-radius: 14px;
}

.chart-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 12px;
}

.chart-title {
    font-size: 0.95rem;
    font-weight: 600;
    color: var(--text-primary);
}

.chart-subtitle {
    font-size: 0.74rem;
    color: var(--text-muted);
    margin-top: 2px;
}

.chart-legend {
    display: flex;
    gap: 14px;
    font-size: 0.78rem;
    color: var(--text-secondary);
}

.legend-item {
    display: flex;
    align-items: center;
    gap: 6px;
}

.dot {
    display: inline-block;
    width: 8px;
    height: 8px;
    border-radius: 50%;
    flex-shrink: 0;
}

.dot-cyan { background: #38bdf8; box-shadow: 0 0 8px rgba(56, 189, 248, 0.7); }
.dot-blue { background: #3b82f6; box-shadow: 0 0 8px rgba(59, 130, 246, 0.7); }
.dot-amber { background: #f59e0b; box-shadow: 0 0 8px rgba(245, 158, 11, 0.7); }
.dot-magenta { background: #e879f9; box-shadow: 0 0 8px rgba(232, 121, 249, 0.7); }
.dot-emerald { background: #10b981; box-shadow: 0 0 8px rgba(16, 185, 129, 0.7); }
.dot-purple { background: #a855f7; box-shadow: 0 0 8px rgba(168, 85, 247, 0.7); }
.dot-coral { background: #f43f5e; box-shadow: 0 0 8px rgba(244, 63, 94, 0.7); }

.pairing-action-row {
    display: flex;
    align-items: center;
    gap: 12px;
    margin-top: 14px;
    flex-wrap: wrap;
}

.btn-download-apk {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    padding: 8px 16px;
    background: linear-gradient(135deg, rgba(56, 189, 248, 0.18), rgba(139, 92, 246, 0.18));
    border: 1px solid rgba(56, 189, 248, 0.4);
    border-radius: 8px;
    color: #38bdf8;
    font-size: 0.85rem;
    font-weight: 500;
    text-decoration: none;
    transition: all 0.2s ease;
}

.btn-download-apk:hover {
    background: linear-gradient(135deg, rgba(56, 189, 248, 0.3), rgba(139, 92, 246, 0.3));
    border-color: #38bdf8;
    box-shadow: 0 0 14px rgba(56, 189, 248, 0.35);
    transform: translateY(-1px);
    color: #fff;
}

.svg-chart-container {
    width: 100%;
    height: 220px;
    background: rgba(0, 0, 0, 0.25);
    border-radius: 10px;
    border: 1px solid rgba(255, 255, 255, 0.04);
    overflow: hidden;
    position: relative;
}

.metric-svg {
    width: 100%;
    height: 100%;
    display: block;
}

/* Tool Invocations Matrix */
.tool-matrix-card {
    padding: 20px;
    margin-bottom: 20px;
    background: rgba(15, 23, 42, 0.6);
    backdrop-filter: blur(12px);
    border: 1px solid rgba(255, 255, 255, 0.07);
    border-radius: 14px;
}

.tool-matrix-body {
    display: flex;
    flex-direction: column;
    gap: 12px;
    margin-top: 14px;
}

.tool-row {
    display: grid;
    grid-template-columns: 180px 1fr 100px 80px;
    align-items: center;
    gap: 14px;
    font-size: 0.84rem;
}

.tool-name-col {
    font-family: var(--font-mono);
    color: #38bdf8;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}

.tool-bar-track {
    height: 8px;
    background: rgba(255, 255, 255, 0.06);
    border-radius: 4px;
    overflow: hidden;
}

.tool-bar-fill {
    height: 100%;
    border-radius: 4px;
    background: linear-gradient(90deg, #06b6d4, #3b82f6);
    transition: width 0.4s ease;
}

.tool-bar-fill.error-fill {
    background: linear-gradient(90deg, #f59e0b, #ef4444);
}

.tool-count-col {
    font-family: var(--font-mono);
    color: var(--text-primary);
    text-align: right;
}

.tool-duration-col {
    font-family: var(--font-mono);
    font-size: 0.76rem;
    color: var(--text-muted);
    text-align: right;
}

/* Live Agent Lifecycle Waterfall */
.waterfall-card {
    padding: 20px;
    background: rgba(15, 23, 42, 0.6);
    backdrop-filter: blur(12px);
    border: 1px solid rgba(255, 255, 255, 0.07);
    border-radius: 14px;
    margin-bottom: 20px;
}

.waterfall-controls {
    display: flex;
    align-items: center;
    gap: 12px;
}

.session-dropdown {
    background: rgba(0, 0, 0, 0.45);
    border: 1px solid rgba(255, 255, 255, 0.12);
    color: var(--text-primary);
    font-size: 0.84rem;
    padding: 6px 14px;
    border-radius: 8px;
    outline: none;
    cursor: pointer;
}

.waterfall-timeline-container {
    margin-top: 18px;
    display: flex;
    flex-direction: column;
    gap: 10px;
}

.waterfall-turn-row {
    display: flex;
    flex-direction: column;
    gap: 8px;
    background: rgba(0, 0, 0, 0.25);
    padding: 12px 16px;
    border-radius: 10px;
    border: 1px solid rgba(255, 255, 255, 0.04);
}

.turn-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    font-size: 0.82rem;
    font-weight: 600;
}

.turn-timeline-track {
    display: flex;
    gap: 8px;
    flex-wrap: wrap;
    align-items: center;
    max-height: 240px;
    overflow-y: auto;
    padding-right: 4px;
}

.turn-timeline-track::-webkit-scrollbar {
    width: 6px;
}

.turn-timeline-track::-webkit-scrollbar-thumb {
    background: rgba(255, 255, 255, 0.15);
    border-radius: 3px;
}

.wf-block {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 6px 12px;
    border-radius: 6px;
    font-size: 0.78rem;
    font-family: var(--font-mono);
    border: 1px solid transparent;
}

.wf-block-prompt {
    background: rgba(168, 85, 247, 0.15);
    border-color: rgba(168, 85, 247, 0.35);
    color: #c084fc;
}

.wf-block-think {
    background: rgba(6, 182, 212, 0.15);
    border-color: rgba(6, 182, 212, 0.35);
    color: #38bdf8;
}

.wf-block-tool {
    background: rgba(34, 197, 94, 0.15);
    border-color: rgba(34, 197, 94, 0.35);
    color: #4ade80;
}

.wf-block-approval {
    background: rgba(245, 158, 11, 0.15);
    border-color: rgba(245, 158, 11, 0.45);
    color: #fbbf24;
    animation: pulseGlow 2s infinite ease-in-out;
}

.wf-block-error {
    background: rgba(239, 68, 68, 0.15);
    border-color: rgba(239, 68, 68, 0.35);
    color: #f87171;
}

@keyframes pulseGlow {
    0%, 100% { box-shadow: 0 0 6px rgba(245, 158, 11, 0.2); }
    50% { box-shadow: 0 0 16px rgba(245, 158, 11, 0.5); }
}

/* ==========================================================================
   Action Approvals & Mobile Companion Styles
   ========================================================================== */

.approvals-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(440px, 1fr));
    gap: 20px;
    margin-bottom: 20px;
}

.pending-approvals-card,
.mobile-pairing-card,
.devices-card {
    padding: 20px;
    background: rgba(15, 23, 42, 0.6);
    backdrop-filter: blur(12px);
    border: 1px solid rgba(255, 255, 255, 0.07);
    border-radius: 14px;
}

.pending-list {
    margin-top: 14px;
    display: flex;
    flex-direction: column;
    gap: 12px;
}

.no-pending-hint {
    text-align: center;
    padding: 36px 16px;
    color: var(--text-muted);
}

.no-pending-hint svg {
    opacity: 0.6;
    margin-bottom: 8px;
}

.approval-item-card {
    background: rgba(0, 0, 0, 0.35);
    border: 1px solid rgba(245, 158, 11, 0.35);
    border-radius: 10px;
    padding: 16px;
    display: flex;
    flex-direction: column;
    gap: 12px;
}

.approval-item-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
}

.approval-tool-badge {
    font-family: var(--font-mono);
    font-size: 0.85rem;
    font-weight: 700;
    color: #fbbf24;
}

.approval-item-body {
    font-family: var(--font-mono);
    font-size: 0.8rem;
    background: rgba(0, 0, 0, 0.4);
    padding: 10px 14px;
    border-radius: 6px;
    color: var(--text-secondary);
    white-space: pre-wrap;
    word-break: break-all;
}

.approval-actions {
    display: flex;
    gap: 10px;
    justify-content: flex-end;
}

.btn-approve {
    background: rgba(34, 197, 94, 0.25);
    color: #4ade80;
    border: 1px solid rgba(34, 197, 94, 0.45);
    padding: 8px 18px;
    border-radius: 8px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.2s;
}

.btn-approve:hover {
    background: rgba(34, 197, 94, 0.4);
    box-shadow: 0 0 14px rgba(34, 197, 94, 0.35);
}

.btn-deny {
    background: rgba(239, 68, 68, 0.2);
    color: #f87171;
    border: 1px solid rgba(239, 68, 68, 0.4);
    padding: 8px 18px;
    border-radius: 8px;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.2s;
}

.btn-deny:hover {
    background: rgba(239, 68, 68, 0.35);
}

/* Pairing Card */
.pairing-content {
    display: flex;
    flex-wrap: wrap;
    gap: 24px;
    margin-top: 14px;
    align-items: center;
}

.qr-placeholder-box {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    min-width: 190px;
    background: rgba(0, 0, 0, 0.5);
    border: 1px solid rgba(56, 189, 248, 0.35);
    border-radius: 14px;
    padding: 16px;
    text-align: center;
    box-shadow: 0 4px 20px rgba(0, 0, 0, 0.35), inset 0 0 16px rgba(56, 189, 248, 0.04);
}

.qr-code-canvas-box {
    width: 160px;
    height: 160px;
    margin-bottom: 12px;
    display: flex;
    align-items: center;
    justify-content: center;
}

.qr-code-canvas-box svg {
    width: 100%;
    height: 100%;
    border-radius: 8px;
}

.secret-code-display {
    font-family: var(--font-mono);
    font-size: 0.92rem;
    font-weight: 700;
    color: #38bdf8;
    word-break: break-all;
    letter-spacing: 0.05em;
}

.pairing-countdown {
    font-size: 0.74rem;
    color: var(--text-muted);
    margin-top: 8px;
}

.pairing-instructions {
    display: flex;
    flex-direction: column;
    gap: 8px;
    font-size: 0.84rem;
    color: var(--text-secondary);
}

.pairing-instructions h3 {
    font-size: 0.95rem;
    color: var(--text-primary);
    margin: 0;
}

.pairing-instructions ol {
    margin: 0;
    padding-left: 20px;
    display: flex;
    flex-direction: column;
    gap: 4px;
}

.devices-list {
    margin-top: 14px;
    display: flex;
    flex-direction: column;
    gap: 8px;
}

.device-item-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    background: rgba(0, 0, 0, 0.3);
    padding: 10px 14px;
    border-radius: 8px;
    border: 1px solid rgba(255, 255, 255, 0.05);
}

.device-info {
    display: flex;
    flex-direction: column;
    gap: 2px;
}

.device-name {
    font-weight: 600;
    color: var(--text-primary);
    font-size: 0.86rem;
}

.device-meta {
    font-size: 0.74rem;
    color: var(--text-muted);
    font-family: var(--font-mono);
}

.btn-revoke {
    background: transparent;
    border: 1px solid rgba(239, 68, 68, 0.3);
    color: #f87171;
    font-size: 0.76rem;
    padding: 4px 10px;
    border-radius: 6px;
    cursor: pointer;
    transition: all 0.2s;
}

.btn-revoke:hover {
    background: rgba(239, 68, 68, 0.2);
}

/* ==========================================================================
   Responsive Viewport & Mobile Breakpoints (< 600px & < 900px)
   ========================================================================== */

@media (max-width: 600px) {
    .app-container {
        padding: 12px 10px 24px;
        gap: 14px;
    }

    .navbar {
        padding: 12px 14px;
        flex-wrap: wrap;
        gap: 10px;
    }

    .brand h1 {
        font-size: 1.05rem;
    }

    .badge-sub {
        display: none;
    }

    .actions {
        gap: 8px;
    }

    .last-updated {
        font-size: 0.74rem;
        white-space: nowrap;
    }

    .btn-refresh {
        padding: 6px 10px;
        font-size: 0.76rem;
    }

    .telemetry-header-bar {
        flex-direction: column;
        align-items: flex-start;
        gap: 12px;
    }

    .timeframe-selector {
        width: 100%;
        overflow-x: auto;
        justify-content: space-between;
    }

    .time-btn {
        flex: 1;
        padding: 4px 8px;
        font-size: 0.74rem;
        text-align: center;
    }

    .telemetry-kpi-grid {
        grid-template-columns: repeat(2, 1fr);
        gap: 10px;
    }

    .chart-header {
        flex-direction: column;
        align-items: flex-start;
        gap: 8px;
    }

    .chart-legend {
        flex-wrap: wrap;
        gap: 8px;
    }

    .tool-row {
        grid-template-columns: 110px 1fr 65px;
        gap: 8px;
    }

    .tool-duration-col {
        display: none;
    }

    .tool-bar-track {
        min-width: 50px;
        flex: 1 1 auto;
    }

    .waterfall-turn-row .turn-header {
        flex-direction: column;
        align-items: flex-start;
        gap: 6px;
    }

    .waterfall-turn-row .badge {
        max-width: 100%;
        text-overflow: ellipsis;
        overflow: hidden;
        white-space: nowrap;
        font-size: 0.7rem;
    }

    .details-box {
        min-width: 120px;
    }

    .details-box .detail-value {
        white-space: nowrap;
    }

    .spend-content-grid {
        grid-template-columns: 1fr;
    }

    .approvals-grid {
        grid-template-columns: 1fr;
    }
}


)raw_asset";

inline const char* APP_JS = R"raw_asset(//
// app.js - aimon client dynamics & countdown timers
//

let currentStatus = null;
let countdownInterval = null;
let statusFetchedAt = Date.now();
let serverTimeOffset = 0;

function formatCountdown(ms) {
    if (ms <= 0) return 'Ready';
    const totalSeconds = Math.max(0, Math.floor(ms / 1000));
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

    const elapsedSinceFetch = Date.now() - statusFetchedAt;
    const serverNow = Date.now() + serverTimeOffset;

    // 1. Quota group buckets
    if (currentStatus.antigravity.quota_groups) {
        currentStatus.antigravity.quota_groups.forEach((group, gIdx) => {
            if (!group.buckets) return;
            group.buckets.forEach((bucket, bIdx) => {
                const timerEl = document.getElementById(`timer-bucket-${gIdx}-${bIdx}`);
                if (!timerEl) return;

                const frac = bucket.remaining_fraction !== undefined ? bucket.remaining_fraction : 1.0;
                if (frac >= 0.999 && (!bucket.reset_time_remaining_seconds || bucket.reset_time_remaining_seconds <= 0)) {
                    timerEl.textContent = 'Active';
                    return;
                }

                let diff = 0;
                if (bucket.reset_time_remaining_seconds !== undefined && bucket.reset_time_remaining_seconds > 0) {
                    diff = Math.max(0, (bucket.reset_time_remaining_seconds * 1000) - elapsedSinceFetch);
                } else if (bucket.reset_time_iso) {
                    const resetTime = new Date(bucket.reset_time_iso).getTime();
                    diff = resetTime - serverNow;
                }

                // Sanity check: Antigravity quota window is at most weekly (7 days = 604,800,000 ms)
                // If diff is greater than 8 days or negative due to clock skew, fallback immediately to description
                if (diff > 8 * 86400 * 1000 || diff < 0) {
                    if (bucket.description && bucket.description.includes('refresh in ')) {
                        const parsed = bucket.description.split('refresh in ')[1].replace(/\.$/, '');
                        timerEl.textContent = `Resets in ${parsed}`;
                        return;
                    }
                }

                if (diff > 0) {
                    timerEl.textContent = `Resets in ${formatCountdown(diff)}`;
                } else {
                    timerEl.textContent = frac >= 0.999 ? 'Active' : 'Ready to reset';
                }
            });
        });
    }

    // 2. Individual models
    if (currentStatus.antigravity.models) {
        currentStatus.antigravity.models.forEach((m, idx) => {
            const timerEl = document.getElementById(`timer-model-${idx}`);
            if (!timerEl) return;

            const frac = m.remaining_fraction !== undefined ? m.remaining_fraction : 1.0;
            if (frac >= 0.999 && (!m.reset_time_remaining_seconds || m.reset_time_remaining_seconds <= 0)) {
                timerEl.textContent = 'Active';
                return;
            }

            let diff = 0;
            if (m.reset_time_remaining_seconds !== undefined && m.reset_time_remaining_seconds > 0) {
                diff = Math.max(0, (m.reset_time_remaining_seconds * 1000) - elapsedSinceFetch);
            } else if (m.reset_time_iso) {
                const resetTime = new Date(m.reset_time_iso).getTime();
                diff = resetTime - serverNow;
            }

            if (diff > 8 * 86400 * 1000 || diff < 0) {
                timerEl.textContent = frac >= 0.999 ? 'Active' : 'Ready to reset';
                return;
            }

            if (diff > 0) {
                timerEl.textContent = `Resets in ${formatCountdown(diff)}`;
            } else {
                timerEl.textContent = frac >= 0.999 ? 'Active' : 'Ready to reset';
            }
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
        if (statusDot) statusDot.className = 'dot-status dot-offline';
        if (planBadge) {
            planBadge.textContent = cr ? (cr.plan_tier || 'Free') : 'Unauthenticated';
            planBadge.className = 'badge';
        }
        if (ratioEl) ratioEl.textContent = '-- / --';
        if (progressBar) progressBar.style.width = '0%';
        if (remainingTxt) remainingTxt.textContent = 'Unauthenticated';
        if (percentTxt) percentTxt.textContent = '0%';
        if (resetDateEl) resetDateEl.textContent = '--';
        if (daysRemainingEl) daysRemainingEl.textContent = '--';

        if (cr && cr.error_message) {
            if (errorBanner) {
                errorBanner.textContent = cr.error_message;
                errorBanner.classList.remove('hidden');
            }
        }
        return;
    }

    if (statusDot) statusDot.className = 'dot-status dot-online';
    if (planBadge) {
        planBadge.textContent = cr.plan_tier || 'Pro';
        planBadge.className = 'badge badge-magenta';
    }
    if (errorBanner) errorBanner.classList.add('hidden');

    const used = cr.fast_requests_used || 0;
    const limit = cr.fast_requests_limit || 0;
    if (ratioEl) ratioEl.textContent = `${used.toLocaleString()} / ${limit.toLocaleString()}`;

    const frac = limit > 0 ? Math.min(1, used / limit) : 0;
    const pct = Math.round(frac * 100);
    if (progressBar) progressBar.style.width = `${pct}%`;
    if (percentTxt) percentTxt.textContent = `${pct}% used`;

    const remaining = Math.max(0, limit - used);
    if (remainingTxt) remainingTxt.textContent = `${remaining.toLocaleString()} fast requests remaining`;

    if (resetDateEl && daysRemainingEl) {
        if (cr.cycle_reset_iso) {
            const resetDate = new Date(cr.cycle_reset_iso);
            resetDateEl.textContent = resetDate.toLocaleDateString();

            const diffDays = Math.ceil((resetDate.getTime() - Date.now()) / (1000 * 60 * 60 * 24));
            daysRemainingEl.textContent = diffDays > 0 ? `${diffDays} days left` : 'Reset pending';
        } else {
            resetDateEl.textContent = 'N/A';
            daysRemainingEl.textContent = '--';
        }
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

    // Chart bounds (viewBox 0 0 420 160)
    const chartLeft = 50;
    const chartRight = 410;
    const chartBottom = 135;
    const chartTop = 20;
    const width = chartRight - chartLeft;
    const height = chartBottom - chartTop;

    const n = history.length;
    let pathD = '';
    let areaD = '';
    if (dotsGroup) dotsGroup.innerHTML = '';
    if (datesAxis) datesAxis.innerHTML = '';

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
            if (dotsGroup) {
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
            }

            // Date label on X axis
            if (datesAxis) {
                const shouldShowLabel = (n <= 7) || (idx === 0) || (idx === n - 1) || (idx % Math.ceil(n / 5) === 0);
                if (shouldShowLabel) {
                    const dateLbl = document.createElement('span');
                    dateLbl.className = 'chart-date-item';
                    const dayLabel = p.pt.day_str || '';
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
            }
        });

        areaD += ` L ${points[points.length - 1].x.toFixed(1)} ${chartBottom} Z`;

        if (linePath) linePath.setAttribute('d', pathD);
        if (areaPath) areaPath.setAttribute('d', areaD);
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
    if (isManual && refreshBtn) {
        refreshBtn.classList.add('spinning');
    }

    try {
        const url = isManual ? '/api/refresh' : '/api/status';
        const method = isManual ? 'POST' : 'GET';
        const res = await fetch(url, { method });
        if (res.ok) {
            const data = await res.json();
            currentStatus = data;
            statusFetchedAt = Date.now();
            if (data.server_timestamp_ms) {
                serverTimeOffset = data.server_timestamp_ms - statusFetchedAt;
            }
            renderAntigravity(data.antigravity);
            renderCursor(data.cursor);
            updateCountdowns();

            const updatedEl = document.getElementById('last-updated');
            if (updatedEl) {
                const nowTime = new Date().toLocaleTimeString();
                updatedEl.textContent = `Updated: ${nowTime}`;
            }
        }
    } catch (e) {
        console.error('Failed to fetch status:', e);
    } finally {
        if (isManual && refreshBtn) {
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
    if (window.location.search.includes('no_sse')) {
        return;
    }
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

function formatFullDateTime(epochSec) {
    if (!epochSec || epochSec <= 0) return 'N/A';
    const d = new Date(epochSec * 1000);
    const year = d.getFullYear();
    const month = String(d.getMonth() + 1).padStart(2, '0');
    const day = String(d.getDate()).padStart(2, '0');
    const hours = String(d.getHours()).padStart(2, '0');
    const minutes = String(d.getMinutes()).padStart(2, '0');
    const seconds = String(d.getSeconds()).padStart(2, '0');
    return `${year}/${month}/${day} ${hours}:${minutes}:${seconds}`;
}

function formatDurationSeconds(sec) {
    if (!sec || sec <= 0) return '0s';
    if (sec < 60) return `${Math.round(sec)}s`;
    const m = Math.floor(sec / 60);
    const s = Math.round(sec % 60);
    if (m < 60) return s > 0 ? `${m}m ${s}s` : `${m}m`;
    const h = Math.floor(m / 60);
    const remM = m % 60;
    return `${h}h ${remM}m`;
}

function formatLastSeen(epoch) {
    if (!epoch || epoch <= 0) return 'Just now';
    const diffSec = Math.floor(Date.now() / 1000) - epoch;
    if (diffSec < 10) return 'Just now';
    if (diffSec < 60) return `${diffSec}s ago`;
    if (diffSec < 3600) return `${Math.floor(diffSec / 60)}m ago`;
    return formatFullDateTime(epoch);
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

let activeSubpanelId = 'quotas';

function renderMonitorTabs(monitors) {
    const satelliteNavEl = document.getElementById('satellite-tabs');
    const dynamicPanelsEl = document.getElementById('dynamic-panels');
    const dividerEl = document.getElementById('nav-divider');
    if (!satelliteNavEl) return;

    let externalMonitors = Array.isArray(monitors) ? monitors.filter(m => m.id !== 'aimon' && m.id !== 'telemetry' && m.id !== 'approvals' && m.id !== 'quotas') : [];
    externalMonitors.sort((a, b) => {
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

    if (dividerEl) {
        dividerEl.style.display = externalMonitors.length > 0 ? 'block' : 'none';
    }

    // Render satellite tabs strictly in lowercase
    satelliteNavEl.innerHTML = externalMonitors.map(m => {
        const sub = (m.subsystem || m.short_name || m.name || m.id || '').toLowerCase();
        const fullId = m.id;
        const isActive = (activeMonitorId === fullId || activeMonitorId === sub);
        const activeClass = isActive ? ' active' : '';
        const isOnline = Boolean(m.connected && m.reachable);
        const indClass = isOnline ? 'tab-indicator-online' : 'tab-indicator-offline';

        return `
            <button type="button" class="monitor-tab${activeClass}" data-id="${escapeHtml(sub)}" data-full-id="${escapeHtml(fullId)}" title="${escapeHtml(m.name || m.id)}">
                <span class="tab-indicator ${indClass}"></span>
                <span class="tab-title">${escapeHtml(sub)}</span>
            </button>
        `;
    }).join('');

    // Attach tab click handlers
    satelliteNavEl.querySelectorAll('.monitor-tab').forEach(tabBtn => {
        tabBtn.addEventListener('click', () => {
            const id = tabBtn.getAttribute('data-id');
            switchToMonitor(id);
        });
    });

    // Synchronize persistent iframe panels in #dynamic-panels
    if (dynamicPanelsEl) {
        // Prune orphaned panels
        const activePanelIds = new Set(externalMonitors.map(m => `panel-${m.id}`));
        dynamicPanelsEl.querySelectorAll('.monitor-frame-panel').forEach(p => {
            if (!activePanelIds.has(p.id)) {
                p.remove();
            }
        });

        externalMonitors.forEach(m => {
            if (m.is_self || m.isSelf) return;

            let panel = document.getElementById(`panel-${m.id}`);
            const targetUrl = getResolvedMonitorUrl(m);
            const isOnline = Boolean(m.connected && m.reachable);

            if (!panel) {
                panel = document.createElement('div');
                panel.id = `panel-${m.id}`;
                panel.setAttribute('data-subsystem', m.subsystem || m.id);
                const isSelected = (m.id === activeMonitorId || (m.subsystem && m.subsystem === activeMonitorId));
                panel.className = `monitor-frame-panel view-panel${isSelected ? ' active' : ' hidden'}`;
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
            const target = externalMonitors.find(m => m.id === hash || m.subsystem === hash);
            if (target && activeMonitorId !== target.id) {
                switchToMonitor(target.id);
            }
        }
    }
}

// ==========================================================================
// Agent Telemetry, Historical Analytics & Approvals Frontend Engine
// ==========================================================================

let activeTelemetryWindow = '24h';
let telemetryTimer = null;
let approvalsTimer = null;

async function fetchTelemetryOverview() {
    try {
        const res = await fetch('/api/telemetry/overview');
        if (!res.ok) return;
        const data = await res.json();

        const activeSessionsEl = document.getElementById('kpi-active-sessions');
        const totalToolsEl = document.getElementById('kpi-total-tools');
        const avgLatencyEl = document.getElementById('kpi-avg-latency');
        const errorRateEl = document.getElementById('kpi-error-rate');

        if (activeSessionsEl) activeSessionsEl.innerHTML = `${data.active_sessions || 0} <span class="kpi-sub">running</span>`;
        if (totalToolsEl) {
            totalToolsEl.innerHTML = `${(data.total_tool_calls || 0).toLocaleString()} <span class="kpi-sub">calls</span>`;
        }
        if (avgLatencyEl) {
            const avgSec = ((data.avg_turn_latency_ms || 0) / 1000.0).toFixed(2);
            avgLatencyEl.innerHTML = `${avgSec} <span class="kpi-sub">sec</span>`;
        }
        if (errorRateEl) {
            const errPct = (data.error_rate_pct || 0).toFixed(1);
            errorRateEl.innerHTML = `${errPct} <span class="kpi-sub">%</span>`;
        }
    } catch (_) {}
}

function formatChartValue(val, isLatency) {
    if (isLatency) {
        if (val < 1000) return `${Math.round(val)}ms`;
        if (val < 60000) return `${(val / 1000).toFixed(1)}s`;
        return `${(val / 60000).toFixed(1)}m`;
    }
    if (val >= 1000) return `${(val / 1000).toFixed(1)}k`;
    if (val === Math.round(val)) return `${val}`;
    return val.toFixed(1);
}

function renderSvgLineChart(svgId, timestamps, series1, series2, color1, color2, isDualAxis = false) {
    const svg = document.getElementById(svgId);
    if (!svg) return;

    if (!timestamps || timestamps.length < 2) {
        svg.innerHTML = '<text x="400" y="120" fill="rgba(255,255,255,0.3)" font-size="12" text-anchor="middle" font-family="sans-serif">Accumulating telemetry data points...</text>';
        return;
    }

    const width = 800;
    const height = 240;
    const padL = 65;
    const padR = isDualAxis ? 50 : 25;
    const padT = 20;
    const padB = 35;

    const plotW = width - padL - padR;
    const plotH = height - padT - padB;
    const pointsCount = timestamps.length;
    const isLatency = (svgId === 'latency-svg');

    let maxVal1 = Math.max(...series1, 0);
    if (maxVal1 <= 0) maxVal1 = 1;
    maxVal1 = maxVal1 * 1.15;

    let maxVal2 = Math.max(...series2, 0);
    if (maxVal2 <= 0) maxVal2 = 1;
    maxVal2 = maxVal2 * 1.15;

    if (!isDualAxis) {
        const combinedMax = Math.max(maxVal1, maxVal2);
        maxVal1 = combinedMax;
        maxVal2 = combinedMax;
    }

    const getX = (idx) => padL + (idx / (pointsCount - 1)) * plotW;
    const getY1 = (val) => padT + plotH - (Math.max(0, val) / maxVal1) * plotH;
    const getY2 = (val) => padT + plotH - (Math.max(0, val) / maxVal2) * plotH;

    let path1 = '';
    let path2 = '';
    let area1 = `M ${getX(0)} ${padT + plotH} `;
    let area2 = `M ${getX(0)} ${padT + plotH} `;

    for (let i = 0; i < pointsCount; ++i) {
        const x = getX(i);
        const y1 = getY1(series1[i] || 0);
        const y2 = getY2(series2[i] || 0);

        if (i === 0) {
            path1 += `M ${x} ${y1} `;
            path2 += `M ${x} ${y2} `;
        } else {
            path1 += `L ${x} ${y1} `;
            path2 += `L ${x} ${y2} `;
        }
        area1 += `L ${x} ${y1} `;
        area2 += `L ${x} ${y2} `;
    }

    area1 += `L ${getX(pointsCount - 1)} ${padT + plotH} Z`;
    area2 += `L ${getX(pointsCount - 1)} ${padT + plotH} Z`;

    // Grid lines & Y-axis labels
    let gridSvg = '';
    const gridLines = 4;
    for (let g = 0; g <= gridLines; ++g) {
        const gy = padT + (g / gridLines) * plotH;
        const gVal1 = ((gridLines - g) / gridLines) * maxVal1;
        const gValStr1 = formatChartValue(gVal1, isLatency);

        gridSvg += `<line x1="${padL}" y1="${gy}" x2="${width - padR}" y2="${gy}" stroke="rgba(255,255,255,0.06)" stroke-width="1" />`;
        gridSvg += `<text x="${padL - 8}" y="${gy + 4}" fill="${color1}" fill-opacity="0.65" font-size="10" font-family="monospace" text-anchor="end">${gValStr1}</text>`;

        if (isDualAxis) {
            const gVal2 = Math.round(((gridLines - g) / gridLines) * maxVal2);
            gridSvg += `<text x="${width - padR + 8}" y="${gy + 4}" fill="${color2}" fill-opacity="0.65" font-size="10" font-family="monospace" text-anchor="start">${gVal2}</text>`;
        }
    }

    // X-Axis Timestamp Ticks (4-5 evenly spaced intervals)
    let xTicksSvg = '';
    const numXTicks = Math.min(5, pointsCount);
    for (let t = 0; t < numXTicks; ++t) {
        const idx = Math.floor(t * (pointsCount - 1) / (numXTicks - 1));
        const tx = getX(idx);
        const epoch = timestamps[idx];
        const dateObj = new Date(epoch * 1000);
        let timeStr = dateObj.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', hour12: false });
        if (activeTelemetryWindow === '7d' || activeTelemetryWindow === '30d' || activeTelemetryWindow === '1y') {
            timeStr = `${dateObj.getMonth() + 1}/${dateObj.getDate()} ${timeStr}`;
        }
        xTicksSvg += `<line x1="${tx}" y1="${padT + plotH}" x2="${tx}" y2="${padT + plotH + 5}" stroke="rgba(255,255,255,0.15)" stroke-width="1" />`;
        xTicksSvg += `<text x="${tx}" y="${padT + plotH + 18}" fill="rgba(255,255,255,0.4)" font-size="10" font-family="monospace" text-anchor="middle">${timeStr}</text>`;
    }

    svg.innerHTML = `
        <defs>
            <linearGradient id="grad-${svgId}-1" x1="0" y1="0" x2="0" y2="1">
                <stop offset="0%" stop-color="${color1}" stop-opacity="0.25"/>
                <stop offset="100%" stop-color="${color1}" stop-opacity="0.0"/>
            </linearGradient>
            <linearGradient id="grad-${svgId}-2" x1="0" y1="0" x2="0" y2="1">
                <stop offset="0%" stop-color="${color2}" stop-opacity="0.2"/>
                <stop offset="100%" stop-color="${color2}" stop-opacity="0.0"/>
            </linearGradient>
        </defs>
        ${gridSvg}
        ${xTicksSvg}
        <path d="${area2}" fill="url(#grad-${svgId}-2)" />
        <path d="${area1}" fill="url(#grad-${svgId}-1)" />
        <path d="${path2}" fill="none" stroke="${color2}" stroke-width="2" stroke-linecap="round" />
        <path d="${path1}" fill="none" stroke="${color1}" stroke-width="2.2" stroke-linecap="round" />
        <g id="tooltip-${svgId}" class="chart-tooltip-group" style="display: none;">
            <line id="crosshair-${svgId}" x1="0" y1="${padT}" x2="0" y2="${padT + plotH}" stroke="rgba(255,255,255,0.35)" stroke-dasharray="3,3" stroke-width="1" />
            <circle id="dot1-${svgId}" r="4" fill="${color1}" stroke="#ffffff" stroke-width="1.5" />
            <circle id="dot2-${svgId}" r="4" fill="${color2}" stroke="#ffffff" stroke-width="1.5" />
            <rect id="tip-bg-${svgId}" width="155" height="52" rx="6" fill="rgba(10, 14, 23, 0.92)" stroke="rgba(255,255,255,0.18)" stroke-width="1" />
            <text id="tip-t-${svgId}" x="0" y="0" fill="#9ca3af" font-size="9" font-family="monospace"></text>
            <text id="tip-v1-${svgId}" x="0" y="0" fill="${color1}" font-size="10" font-family="monospace" font-weight="bold"></text>
            <text id="tip-v2-${svgId}" x="0" y="0" fill="${color2}" font-size="10" font-family="monospace" font-weight="bold"></text>
        </g>
        <rect class="svg-overlay-catcher" x="${padL}" y="${padT}" width="${plotW}" height="${plotH}" fill="transparent" style="cursor: crosshair;" />
    `;

    // Interactive crosshair & tooltip
    const catcher = svg.querySelector('.svg-overlay-catcher');
    const tipGroup = svg.querySelector(`#tooltip-${svgId}`);
    const crosshair = svg.querySelector(`#crosshair-${svgId}`);
    const dot1 = svg.querySelector(`#dot1-${svgId}`);
    const dot2 = svg.querySelector(`#dot2-${svgId}`);
    const tipBg = svg.querySelector(`#tip-bg-${svgId}`);
    const tipT = svg.querySelector(`#tip-t-${svgId}`);
    const tipV1 = svg.querySelector(`#tip-v1-${svgId}`);
    const tipV2 = svg.querySelector(`#tip-v2-${svgId}`);

    if (catcher && tipGroup) {
        catcher.addEventListener('mousemove', (e) => {
            const rect = svg.getBoundingClientRect();
            const mouseSvgX = ((e.clientX - rect.left) / rect.width) * width;
            const normX = Math.max(0, Math.min(1, (mouseSvgX - padL) / plotW));
            const idx = Math.round(normX * (pointsCount - 1));

            const curX = getX(idx);
            const val1 = series1[idx] || 0;
            const val2 = series2[idx] || 0;
            const curY1 = getY1(val1);
            const curY2 = getY2(val2);

            crosshair.setAttribute('x1', curX);
            crosshair.setAttribute('x2', curX);
            dot1.setAttribute('cx', curX);
            dot1.setAttribute('cy', curY1);
            dot2.setAttribute('cx', curX);
            dot2.setAttribute('cy', curY2);

            const epoch = timestamps[idx];
            const tsStr = new Date(epoch * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false });
            tipT.textContent = `Time: ${tsStr}`;

            if (isLatency) {
                tipV1.textContent = `Avg: ${formatChartValue(val1, true)}`;
                tipV2.textContent = `P95: ${formatChartValue(val2, true)}`;
            } else {
                tipV1.textContent = `Rate: ${val1.toFixed(1)} calls/s`;
                tipV2.textContent = `Sessions: ${Math.round(val2)}`;
            }

            let boxX = curX + 10;
            if (boxX + 160 > width - padR) {
                boxX = curX - 165;
            }
            const boxY = padT + 8;
            tipBg.setAttribute('x', boxX);
            tipBg.setAttribute('y', boxY);
            tipT.setAttribute('x', boxX + 8);
            tipT.setAttribute('y', boxY + 14);
            tipV1.setAttribute('x', boxX + 8);
            tipV1.setAttribute('y', boxY + 29);
            tipV2.setAttribute('x', boxX + 8);
            tipV2.setAttribute('y', boxY + 44);

            tipGroup.style.display = 'block';
        });

        catcher.addEventListener('mouseleave', () => {
            tipGroup.style.display = 'none';
        });
    }
}

async function fetchTelemetryTimeseries() {
    try {
        const res = await fetch(`/api/telemetry/timeseries?window=${activeTelemetryWindow}&max_points=100`);
        if (!res.ok) return;
        const data = await res.json();
        if (!data || !data.series) return;

        const s = data.series;
        const ts = s.timestamps || [];
        const toolCalls = s.tool_calls_sec || [];
        const activeAgents = s.active_agents || [];
        const avgLat = s.avg_turn_latency_ms || [];
        const p95Lat = s.p95_turn_latency_ms || [];

        renderSvgLineChart('token-svg', ts, toolCalls, activeAgents, '#38bdf8', '#e879f9', true);
        renderSvgLineChart('latency-svg', ts, avgLat, p95Lat, '#3b82f6', '#f59e0b', false);
    } catch (_) {}
}

async function fetchActivityTimeline() {
    try {
        const res = await fetch(`/api/telemetry/activity_timeline?window=${activeTelemetryWindow}&max_sessions=40`);
        if (!res.ok) return;
        const data = await res.json();
        if (!data) return;

        renderAgentActivityGantt('activity-gantt-svg', data);
        renderAgentBusynessStack('busyness-stack-svg', data);
    } catch (_) {}
}

function renderAgentActivityGantt(svgId, data) {
    const svg = document.getElementById(svgId);
    if (!svg) return;

    const width = 800;
    const height = 220;
    const padL = 75;
    const padR = 25;
    const padT = 18;
    const padB = 30;
    const plotW = width - padL - padR;
    const plotH = height - padT - padB;

    const startTime = data.start_time || (Math.floor(Date.now() / 1000) - 86400);
    const endTime = data.end_time || Math.floor(Date.now() / 1000);
    const totalDuration = Math.max(1, endTime - startTime);

    const rawSessions = data.sessions || [];
    const sessions = rawSessions.slice(0, 6);

    if (sessions.length === 0) {
        svg.innerHTML = `<text x="${width / 2}" y="${height / 2}" fill="#6b7280" font-size="12" text-anchor="middle" font-family="sans-serif">No agent task activity recorded in this timeframe</text>`;
        return;
    }

    // Time Axis Ticks & Grid
    const tickCount = 6;
    let xTicksSvg = '';
    let gridSvg = '';
    for (let i = 0; i <= tickCount; i++) {
        const x = padL + (i / tickCount) * plotW;
        const tickEpoch = startTime + (i / tickCount) * totalDuration;
        const d = new Date(tickEpoch * 1000);
        const label = `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
        xTicksSvg += `<text x="${x}" y="${height - 10}" fill="#9ca3af" font-size="9.5" text-anchor="middle" font-family="monospace">${label}</text>`;
        gridSvg += `<line x1="${x}" y1="${padT}" x2="${x}" y2="${padT + plotH}" stroke="rgba(255,255,255,0.06)" stroke-width="1" />`;
    }

    const rowHeight = plotH / sessions.length;
    let barsSvg = '';
    let tooltipsData = [];

    sessions.forEach((s, idx) => {
        const y = padT + idx * rowHeight + 3;
        const barH = Math.max(16, rowHeight - 6);

        const sStart = Math.max(startTime, s.start_timestamp);
        const sEnd = (s.end_timestamp && s.end_timestamp > 0) ? Math.min(endTime, s.end_timestamp) : endTime;
        const barX = padL + ((sStart - startTime) / totalDuration) * plotW;
        const barW = Math.max(8, ((sEnd - sStart) / totalDuration) * plotW);

        const isRunning = (!s.end_timestamp || s.end_timestamp === 0 || s.status === 'RUNNING' || s.status === 'active');
        const isError = (s.status === 'ERROR' || s.total_errors > 0);
        let barColor = isRunning ? '#10b981' : (isError ? '#f43f5e' : '#38bdf8');
        let bgOpacity = isRunning ? '0.85' : '0.65';

        const agentName = (s.agent_type || 'agent').toLowerCase();
        let shortId = s.session_id || 'sess';
        if (shortId.startsWith('sess-')) shortId = shortId.substring(5);
        if (shortId.length > 8) shortId = shortId.substring(0, 8);

        barsSvg += `<text x="${padL - 8}" y="${y + barH / 2 + 3.5}" fill="#cbd5e1" font-size="9" text-anchor="end" font-family="monospace">${escapeHtml(shortId)}</text>`;

        barsSvg += `
            <g class="gantt-bar-group" data-idx="${idx}" style="cursor: pointer;">
                <rect x="${barX}" y="${y}" width="${barW}" height="${barH}" rx="4" fill="${barColor}" fill-opacity="${bgOpacity}" stroke="${barColor}" stroke-width="1.2" />
                <text x="${barX + 6}" y="${y + barH / 2 + 3}" fill="#ffffff" font-size="8.5" font-family="sans-serif" font-weight="500">${escapeHtml(s.model_name || agentName)} (${s.total_tool_calls || 0} tools)</text>
            </g>
        `;

        tooltipsData.push({
            barX, y,
            session_id: s.session_id,
            model_name: s.model_name || agentName,
            status: isRunning ? 'Active (Running)' : (s.status || 'Done'),
            tools: s.total_tool_calls || 0,
            turns: s.total_turns || 0,
            duration: formatDurationSeconds(s.duration_s || 0),
            startTimeStr: formatFullDateTime(s.start_timestamp)
        });
    });

    svg.innerHTML = `
        ${gridSvg}
        ${xTicksSvg}
        ${barsSvg}
        <g id="gantt-tip-${svgId}" style="display: none; pointer-events: none;">
            <rect id="gantt-tip-bg-${svgId}" width="190" height="58" rx="6" fill="rgba(10, 14, 23, 0.95)" stroke="rgba(56, 189, 248, 0.5)" stroke-width="1" />
            <text id="gantt-tip-title-${svgId}" x="0" y="0" fill="#38bdf8" font-size="9.5" font-weight="bold" font-family="monospace"></text>
            <text id="gantt-tip-line1-${svgId}" x="0" y="0" fill="#e2e8f0" font-size="9" font-family="sans-serif"></text>
            <text id="gantt-tip-line2-${svgId}" x="0" y="0" fill="#94a3b8" font-size="8.5" font-family="monospace"></text>
        </g>
    `;

    const tipG = svg.querySelector(`#gantt-tip-${svgId}`);
    const tipBg = svg.querySelector(`#gantt-tip-bg-${svgId}`);
    const tipTitle = svg.querySelector(`#gantt-tip-title-${svgId}`);
    const tipL1 = svg.querySelector(`#gantt-tip-line1-${svgId}`);
    const tipL2 = svg.querySelector(`#gantt-tip-line2-${svgId}`);

    svg.querySelectorAll('.gantt-bar-group').forEach(el => {
        el.addEventListener('mouseenter', () => {
            const idx = parseInt(el.getAttribute('data-idx'), 10);
            const item = tooltipsData[idx];
            if (!item || !tipG) return;

            tipTitle.textContent = `${item.session_id} [${item.status}]`;
            tipL1.textContent = `${item.model_name} • ${item.turns}t / ${item.tools} tools (${item.duration})`;
            tipL2.textContent = `Started: ${item.startTimeStr}`;

            let tipX = item.barX + 10;
            if (tipX + 195 > width - padR) tipX = item.barX - 200;
            const tipY = Math.max(padT, item.y - 10);

            tipBg.setAttribute('x', tipX);
            tipBg.setAttribute('y', tipY);
            tipTitle.setAttribute('x', tipX + 8);
            tipTitle.setAttribute('y', tipY + 16);
            tipL1.setAttribute('x', tipX + 8);
            tipL1.setAttribute('y', tipY + 32);
            tipL2.setAttribute('x', tipX + 8);
            tipL2.setAttribute('y', tipY + 48);

            tipG.style.display = 'block';
        });

        el.addEventListener('mouseleave', () => {
            if (tipG) tipG.style.display = 'none';
        });
    });
}

function renderAgentBusynessStack(svgId, data) {
    const svg = document.getElementById(svgId);
    if (!svg) return;

    const width = 800;
    const height = 220;
    const padL = 45;
    const padR = 25;
    const padT = 18;
    const padB = 30;
    const plotW = width - padL - padR;
    const plotH = height - padT - padB;

    const buckets = data.buckets || [];
    if (buckets.length === 0) {
        svg.innerHTML = `<text x="${width / 2}" y="${height / 2}" fill="#6b7280" font-size="12" text-anchor="middle" font-family="sans-serif">No concurrency telemetry available</text>`;
        return;
    }

    let maxAgents = 1;
    buckets.forEach(b => {
        const total = (b.antigravity_active || 0) + (b.cursor_active || 0);
        if (total > maxAgents) maxAgents = total;
    });
    maxAgents = Math.max(2, maxAgents);

    // Y Axis Grid
    let yGridSvg = '';
    for (let yVal = 0; yVal <= maxAgents; yVal++) {
        const y = padT + plotH - (yVal / maxAgents) * plotH;
        yGridSvg += `
            <line x1="${padL}" y1="${y}" x2="${padL + plotW}" y2="${y}" stroke="rgba(255,255,255,0.06)" stroke-width="1" />
            <text x="${padL - 8}" y="${y + 3.5}" fill="#9ca3af" font-size="9" text-anchor="end" font-family="monospace">${yVal}</text>
        `;
    }

    const barSlotW = plotW / buckets.length;
    const barW = Math.max(4, barSlotW * 0.75);
    let barsSvg = '';
    let xTicksSvg = '';
    let tooltipBuckets = [];

    buckets.forEach((b, idx) => {
        const x = padL + idx * barSlotW + (barSlotW - barW) / 2;
        const agy = b.antigravity_active || 0;
        const cur = b.cursor_active || 0;
        const tools = b.tool_calls || 0;

        const hAgy = (agy / maxAgents) * plotH;
        const hCur = (cur / maxAgents) * plotH;

        const yAgy = padT + plotH - hAgy;
        const yCur = yAgy - hCur;

        if (hAgy > 0) {
            barsSvg += `<rect x="${x}" y="${yAgy}" width="${barW}" height="${hAgy}" rx="2" fill="#38bdf8" fill-opacity="0.8" />`;
        }
        if (hCur > 0) {
            barsSvg += `<rect x="${x}" y="${yCur}" width="${barW}" height="${hCur}" rx="2" fill="#a855f7" fill-opacity="0.8" />`;
        }

        if (idx % Math.max(1, Math.floor(buckets.length / 6)) === 0) {
            const d = new Date(b.timestamp * 1000);
            const label = `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
            xTicksSvg += `<text x="${x + barW / 2}" y="${height - 10}" fill="#9ca3af" font-size="9.5" text-anchor="middle" font-family="monospace">${label}</text>`;
        }

        barsSvg += `<rect class="busyness-bar-hitbox" data-idx="${idx}" x="${padL + idx * barSlotW}" y="${padT}" width="${barSlotW}" height="${plotH}" fill="transparent" style="cursor: crosshair;" />`;

        tooltipBuckets.push({
            x: x + barW / 2,
            timestamp: b.timestamp,
            antigravity: agy,
            cursor: cur,
            total: agy + cur,
            tools: tools
        });
    });

    svg.innerHTML = `
        ${yGridSvg}
        ${xTicksSvg}
        ${barsSvg}
        <g id="busy-tip-${svgId}" style="display: none; pointer-events: none;">
            <rect id="busy-tip-bg-${svgId}" width="165" height="50" rx="6" fill="rgba(10, 14, 23, 0.95)" stroke="rgba(255, 255, 255, 0.2)" stroke-width="1" />
            <text id="busy-tip-time-${svgId}" x="0" y="0" fill="#9ca3af" font-size="9" font-family="monospace"></text>
            <text id="busy-tip-val1-${svgId}" x="0" y="0" fill="#38bdf8" font-size="9.5" font-family="monospace" font-weight="bold"></text>
            <text id="busy-tip-val2-${svgId}" x="0" y="0" fill="#a855f7" font-size="9.5" font-family="monospace" font-weight="bold"></text>
        </g>
    `;

    const tipG = svg.querySelector(`#busy-tip-${svgId}`);
    const tipBg = svg.querySelector(`#busy-tip-bg-${svgId}`);
    const tipTime = svg.querySelector(`#busy-tip-time-${svgId}`);
    const tipVal1 = svg.querySelector(`#busy-tip-val1-${svgId}`);
    const tipVal2 = svg.querySelector(`#busy-tip-val2-${svgId}`);

    svg.querySelectorAll('.busyness-bar-hitbox').forEach(el => {
        el.addEventListener('mousemove', () => {
            const idx = parseInt(el.getAttribute('data-idx'), 10);
            const item = tooltipBuckets[idx];
            if (!item || !tipG) return;

            tipTime.textContent = `Time: ${formatFullDateTime(item.timestamp)}`;
            tipVal1.textContent = `Antigravity: ${item.antigravity} active (${item.tools} tools)`;
            tipVal2.textContent = `Cursor: ${item.cursor} active`;

            let tipX = item.x + 10;
            if (tipX + 170 > width - padR) tipX = item.x - 175;
            const tipY = padT + 10;

            tipBg.setAttribute('x', tipX);
            tipBg.setAttribute('y', tipY);
            tipTime.setAttribute('x', tipX + 8);
            tipTime.setAttribute('y', tipY + 14);
            tipVal1.setAttribute('x', tipX + 8);
            tipVal1.setAttribute('y', tipY + 28);
            tipVal2.setAttribute('x', tipX + 8);
            tipVal2.setAttribute('y', tipY + 42);

            tipG.style.display = 'block';
        });

        el.addEventListener('mouseleave', () => {
            if (tipG) tipG.style.display = 'none';
        });
    });
}

async function fetchTelemetryTools() {
    try {
        const res = await fetch('/api/telemetry/tools');
        if (!res.ok) return;
        const tools = await res.json();
        const container = document.getElementById('tool-matrix-container');
        const badge = document.getElementById('tools-total-badge');
        if (!container) return;

        if (!tools || tools.length === 0) {
            container.innerHTML = '<div class="loading-placeholder">No tool execution telemetry recorded yet.</div>';
            if (badge) badge.textContent = '0 Calls';
            return;
        }

        let totalInvocations = 0;
        let maxInvocations = 1;
        tools.forEach(t => {
            totalInvocations += (t.invocations || 0);
            if ((t.invocations || 0) > maxInvocations) {
                maxInvocations = t.invocations;
            }
        });
        if (badge) badge.textContent = `${totalInvocations} Calls`;

        container.innerHTML = tools.map(t => {
            const inv = t.invocations || 0;
            const pct = Math.round((inv / maxInvocations) * 100);
            const errRate = (t.error_rate_pct || 0).toFixed(1);
            const avgMs = Math.round(t.avg_duration_ms || 0);
            const isError = t.error_count > 0;

            return `
                <div class="tool-row">
                    <span class="tool-name-col" title="${escapeHtml(t.tool_name)}">${escapeHtml(t.tool_name)}</span>
                    <div class="tool-bar-track">
                        <div class="tool-bar-fill ${isError ? 'error-fill' : ''}" style="width: ${pct}%;"></div>
                    </div>
                    <span class="tool-count-col">${inv} calls ${errRate > 0 ? `<span class="text-amber">(${errRate}% err)</span>` : ''}</span>
                    <span class="tool-duration-col">${avgMs} ms</span>
                </div>
            `;
        }).join('');
    } catch (_) {}
}

async function fetchTelemetrySessions() {
    try {
        const res = await fetch('/api/telemetry/sessions');
        if (!res.ok) return;
        const sessions = await res.json();
        const select = document.getElementById('waterfall-session-select');
        if (!select) return;

        const currentVal = select.value;
        if (!sessions || sessions.length === 0) {
            select.innerHTML = '<option value="">No Active or Recent Sessions</option>';
            return;
        }

        // Sort latest on top
        sessions.sort((a, b) => (b.start_timestamp || 0) - (a.start_timestamp || 0));

        const chosenSessionId = currentVal || sessions[0].session_id;
        select.innerHTML = sessions.map(s => {
            const dtStr = formatFullDateTime(s.start_timestamp);
            const turns = s.total_turns || 0;
            const tools = s.total_tool_calls || 0;
            const agentLabel = (s.agent_type || 'agent').toLowerCase();
            return `<option value="${s.session_id}" ${s.session_id === chosenSessionId ? 'selected' : ''}>[${agentLabel}] ${s.session_id} - ${s.model_name} (${turns} turns, ${tools} tools &bull; ${dtStr})</option>`;
        }).join('');

        select.value = chosenSessionId;
        fetchSessionWaterfall(chosenSessionId);
    } catch (_) {}
}

async function fetchSessionWaterfall(sessionId) {
    if (!sessionId) return;
    const container = document.getElementById('waterfall-container');
    if (!container) return;

    try {
        const res = await fetch(`/api/telemetry/session_events?sessionId=${sessionId}`);
        if (!res.ok) return;
        const events = await res.json();

        if (!events || events.length === 0) {
            container.innerHTML = `<div class="waterfall-empty-hint">Session <code>${escapeHtml(sessionId)}</code> is active, but 0 lifecycle events recorded yet.</div>`;
            return;
        }

        const maxDisplay = 100;
        const displayEvents = events.length > maxDisplay ? events.slice(-maxDisplay) : events;
        const badgeLabel = events.length > maxDisplay ? `Recent ${maxDisplay} / ${events.length} Events` : `${events.length} Events`;

        container.innerHTML = `
            <div class="waterfall-turn-row">
                <div class="turn-header">
                    <span>Session Lifecycle Timeline (Chronological Execution Flow)</span>
                    <span class="badge badge-cyan">${badgeLabel}</span>
                </div>
                <div class="turn-timeline-track">
                    ${displayEvents.map(ev => {
                        let blockClass = 'wf-block-tool';
                        if (ev.event_type === 'SESSION_START' || ev.event_type === 'TURN_START' || ev.event_type === 'USER_TURN') blockClass = 'wf-block-prompt';
                        else if (ev.event_type === 'THINKING') blockClass = 'wf-block-think';
                        else if (ev.event_type === 'APPROVAL_WAIT') blockClass = 'wf-block-approval';
                        else if (ev.status === 'ERROR') blockClass = 'wf-block-error';

                        const durationStr = ev.duration_ms > 0 ? ` (${Math.round(ev.duration_ms)}ms)` : '';
                        const label = ev.tool_name ? `${ev.event_type}: ${ev.tool_name}` : ev.event_type;

                        return `<div class="wf-block ${blockClass}">${label}${durationStr}</div>`;
                    }).join('')}
                </div>
            </div>
        `;
    } catch (_) {}
}

// --- Action Approvals & Mobile Companion Client ---

async function fetchPendingApprovals() {
    try {
        const res = await fetch('/api/approvals/pending');
        if (!res.ok) return;
        const list = await res.json();

        const badge = document.getElementById('pending-approvals-count');
        const navBadge = document.getElementById('approvals-badge');
        const container = document.getElementById('pending-approvals-list');

        if (badge) badge.textContent = `${list.length} Pending`;
        if (navBadge) navBadge.textContent = list.length > 0 ? `${list.length} Action` : 'Mobile';

        if (!container) return;
        if (!list || list.length === 0) {
            container.innerHTML = `
                <div class="no-pending-hint">
                    <svg viewBox="0 0 24 24" width="28" height="28" stroke="currentColor" stroke-width="1.8" fill="none">
                        <circle cx="12" cy="12" r="10"></circle>
                        <polyline points="12 6 12 14 14 14"></polyline>
                    </svg>
                    <p>No actions currently waiting for approval.</p>
                </div>
            `;
            return;
        }

        container.innerHTML = list.map(item => `
            <div class="approval-item-card" id="approval-card-${item.approval_id}">
                <div class="approval-item-header">
                    <span class="approval-tool-badge">[${item.agent_type.toUpperCase()}] ${item.tool_name}</span>
                    <span class="badge badge-amber">⏱️ ${item.remaining_seconds}s left</span>
                </div>
                <div class="approval-item-body">${item.reason || ''}\nWorkspace: ${item.workspace}\nArgs: ${JSON.stringify(item.tool_args, null, 2)}</div>
                <div class="approval-actions">
                    <button class="btn-deny" onclick="submitApprovalDecision('${item.approval_id}', 'deny')">Deny</button>
                    <button class="btn-approve" onclick="submitApprovalDecision('${item.approval_id}', 'allow')">Approve Action</button>
                </div>
            </div>
        `).join('');
    } catch (_) {}
}

async function submitApprovalDecision(approvalId, decision) {
    try {
        const res = await fetch('/api/approvals/decision', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ approval_id: approvalId, decision: decision })
        });
        if (res.ok) {
            fetchPendingApprovals();
        }
    } catch (_) {}
}

async function fetchMobileQr() {
    try {
        const res = await fetch('/api/mobile/qr');
        if (!res.ok) return;
        const data = await res.json();
        const secretEl = document.getElementById('pairing-secret-display');
        const countEl = document.getElementById('pairing-countdown');
        const qrContainer = document.getElementById('pairing-qr-svg');

        const secret = data.secret || '';
        if (secretEl) secretEl.textContent = secret || 'ERR_SECRET';
        if (countEl) countEl.textContent = `Valid for ${data.expires_in || 300}s`;

        if (qrContainer && secret && typeof window.generateQrSvg === 'function') {
            let host = window.location.hostname || '127.0.0.1';
            if ((host === '127.0.0.1' || host === 'localhost' || host === '0.0.0.0') && data.lan_ip) {
                host = data.lan_ip;
            }
            const port = window.location.port || 3883;
            const pairingUri = `aimon://pair?host=${host}&port=${port}&secret=${secret}`;
            qrContainer.innerHTML = window.generateQrSvg(pairingUri, {
                fg: '#38bdf8',
                bg: '#0f172a',
                margin: 2
            });
        }
    } catch (_) {}
}

async function fetchMobileDevices() {
    try {
        const res = await fetch('/api/mobile/devices');
        if (!res.ok) return;
        const list = await res.json();
        const container = document.getElementById('paired-devices-list');
        if (!container) return;

        if (!list || list.length === 0) {
            container.innerHTML = '<div class="loading-placeholder">No mobile devices paired yet. Scan the code above to link your phone.</div>';
            return;
        }

        container.innerHTML = list.map(d => `
            <div class="device-item-row">
                <div class="device-info">
                    <span class="device-name">${d.device_name || 'Android Device'}</span>
                    <span class="device-meta">ID: ${d.device_id} &bull; Paired: ${new Date(d.paired_at * 1000).toLocaleDateString()}</span>
                </div>
                <button class="btn-revoke" onclick="revokeDevice('${d.device_id}')">Revoke</button>
            </div>
        `).join('');
    } catch (_) {}
}

async function revokeDevice(deviceId) {
    if (!confirm('Revoke access for this device?')) return;
    try {
        const res = await fetch('/api/mobile/revoke', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ device_id: deviceId })
        });
        if (res.ok) {
            fetchMobileDevices();
        }
    } catch (_) {}
}

function switchToSubpanel(subpanelId) {
    if (!subpanelId) subpanelId = 'quotas';
    activeSubpanelId = subpanelId;
    activeMonitorId = 'aimon';

    try {
        history.replaceState(null, '', '#' + subpanelId);
    } catch (_) {}

    // 1. Activate aimon top tab, deactivate satellite tabs
    const navEl = document.getElementById('monitor-tabs');
    if (navEl) {
        navEl.querySelectorAll('.monitor-tab').forEach(tab => {
            const isAimon = (tab.getAttribute('data-id') === 'aimon');
            tab.classList.toggle('active', isAimon);
        });
    }

    // 2. Show aimon view panel, hide dynamic satellite panels
    const viewAimon = document.getElementById('view-aimon');
    if (viewAimon) {
        viewAimon.classList.add('active');
        viewAimon.classList.remove('hidden');
    }
    const dynamicPanelsEl = document.getElementById('dynamic-panels');
    if (dynamicPanelsEl) {
        dynamicPanelsEl.querySelectorAll('.monitor-frame-panel').forEach(p => {
            p.classList.remove('active');
            p.classList.add('hidden');
        });
    }

    // 3. Update aimon subnav tabs
    const subnavEl = document.getElementById('aimon-subnav');
    if (subnavEl) {
        subnavEl.querySelectorAll('.aimon-subnav-tab').forEach(tab => {
            const isMatch = (tab.getAttribute('data-subpanel') === subpanelId);
            tab.classList.toggle('active', isMatch);
        });
    }

    // 4. Show active subpanel, hide other subpanels
    const subpanels = {
        'quotas': document.getElementById('subpanel-quotas'),
        'telemetry': document.getElementById('subpanel-telemetry'),
        'approvals': document.getElementById('subpanel-approvals')
    };
    Object.keys(subpanels).forEach(key => {
        const el = subpanels[key];
        if (el) {
            const isMatch = (key === subpanelId);
            el.classList.toggle('active', isMatch);
            el.classList.toggle('hidden', !isMatch);
        }
    });

    // 5. Fetch subpanel data
    if (subpanelId === 'quotas') {
        fetchStatus();
        fetchSessions();
    } else if (subpanelId === 'telemetry') {
        fetchTelemetryOverview();
        fetchTelemetryTimeseries();
        fetchActivityTimeline();
        fetchTelemetryTools();
        fetchTelemetrySessions();
    } else if (subpanelId === 'approvals') {
        fetchPendingApprovals();
        fetchMobileQr();
        fetchMobileDevices();
    }
}

function switchToMonitor(monitorId) {
    if (!monitorId || monitorId === 'aimon') {
        switchToSubpanel(activeSubpanelId || 'quotas');
        return;
    }

    activeMonitorId = monitorId;

    try {
        history.replaceState(null, '', '#' + monitorId);
    } catch (_) {}

    // 1. Update top monitor tab active states
    const navEl = document.getElementById('monitor-tabs');
    if (navEl) {
        navEl.querySelectorAll('.monitor-tab').forEach(tab => {
            const tabId = tab.getAttribute('data-id');
            const fullId = tab.getAttribute('data-full-id');
            const isMatch = (tabId === monitorId || fullId === monitorId || (fullId && fullId.startsWith(monitorId + '-')));
            tab.classList.toggle('active', isMatch);
        });
    }

    // 2. Hide aimon view panel
    const viewAimon = document.getElementById('view-aimon');
    if (viewAimon) {
        viewAimon.classList.remove('active');
        viewAimon.classList.add('hidden');
    }

    // 3. Show matching satellite frame panel
    const dynamicPanelsEl = document.getElementById('dynamic-panels');
    if (dynamicPanelsEl) {
        dynamicPanelsEl.querySelectorAll('.monitor-frame-panel').forEach(panel => {
            const panelId = panel.id;
            const subsystem = panel.getAttribute('data-subsystem');
            const isMatch = (panelId === `panel-${monitorId}` || panelId.startsWith(`panel-${monitorId}-`) || (subsystem && (subsystem === monitorId || monitorId.startsWith(subsystem))));
            panel.classList.toggle('active', isMatch);
            panel.classList.toggle('hidden', !isMatch);
        });
    }
}

function switchToTab(id) {
    if (!id || id === 'aimon') {
        switchToSubpanel(activeSubpanelId || 'quotas');
    } else if (id === 'quotas' || id === 'telemetry' || id === 'approvals') {
        switchToSubpanel(id);
    } else {
        switchToMonitor(id);
    }
}

window.addEventListener('hashchange', () => {
    const hash = window.location.hash.replace(/^#/, '');
    if (hash) {
        if (hash.startsWith('aimon/')) {
            switchToSubpanel(hash.split('/')[1]);
        } else if (hash === 'quotas' || hash === 'telemetry' || hash === 'approvals') {
            switchToSubpanel(hash);
        } else {
            switchToMonitor(hash);
        }
    } else {
        switchToSubpanel('quotas');
    }
});

document.addEventListener('DOMContentLoaded', () => {
    fetchStatus();
    fetchSessions();
    fetchMonitors();
    fetchMobileQr();
    setupSse();

    if (window.location.hash) {
        const hash = window.location.hash.replace(/^#/, '');
        if (hash) {
            if (hash.startsWith('aimon/')) {
                switchToSubpanel(hash.split('/')[1]);
            } else if (hash === 'quotas' || hash === 'telemetry' || hash === 'approvals') {
                switchToSubpanel(hash);
            } else {
                switchToMonitor(hash);
            }
        }
    } else {
        switchToSubpanel('quotas');
    }

    // Top-level monitor tabs listener
    const monitorTabsEl = document.getElementById('monitor-tabs');
    if (monitorTabsEl) {
        monitorTabsEl.addEventListener('click', (e) => {
            const btn = e.target.closest('.monitor-tab');
            if (btn) {
                const monitorId = btn.getAttribute('data-id');
                switchToMonitor(monitorId);
            }
        });
    }

    // Aimon sub-navigation tabs listener
    const aimonSubnavEl = document.getElementById('aimon-subnav');
    if (aimonSubnavEl) {
        aimonSubnavEl.addEventListener('click', (e) => {
            const btn = e.target.closest('.aimon-subnav-tab');
            if (btn && btn.dataset.subpanel) {
                switchToSubpanel(btn.dataset.subpanel);
            }
        });
    }

    // Timeframe selector listeners
    const tfSelector = document.getElementById('telem-timeframe-selector');
    if (tfSelector) {
        tfSelector.addEventListener('click', (e) => {
            const btn = e.target.closest('.time-btn');
            if (btn && btn.dataset.window) {
                tfSelector.querySelectorAll('.time-btn').forEach(b => b.classList.remove('active'));
                btn.classList.add('active');
                activeTelemetryWindow = btn.dataset.window;
                fetchTelemetryTimeseries();
                fetchActivityTimeline();
            }
        });
    }

    // Session dropdown listener
    const sessionSelect = document.getElementById('waterfall-session-select');
    if (sessionSelect) {
        sessionSelect.addEventListener('change', (e) => {
            fetchSessionWaterfall(e.target.value);
        });
    }

    // Regenerate QR button
    const btnQr = document.getElementById('btn-new-qr');
    if (btnQr) {
        btnQr.addEventListener('click', fetchMobileQr);
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

    // Periodic refresh timers (backed by reactive SSE streams)
    setInterval(() => fetchStatus(false), 12000);
    setInterval(fetchSessions, 10000);
    setInterval(fetchMonitors, 15000);

    // Refresh telemetry & approvals periodically when respective tab is active
    setInterval(() => {
        if (activeTabId === 'telemetry') {
            fetchTelemetryOverview();
            fetchTelemetryTimeseries();
            fetchActivityTimeline();
            fetchTelemetryTools();
        } else if (activeTabId === 'approvals') {
            fetchPendingApprovals();
        }
    }, 8000);

    // Update countdown timers and session uptime every second
    if (countdownInterval) clearInterval(countdownInterval);
    countdownInterval = setInterval(() => {
        updateCountdowns();
        updateSessionUptimes();
    }, 1000);
});

)raw_asset";

inline const char* QRCODE_JS = R"raw_asset(/*
 * qrcode.js - Standards-compliant JIS X 0510 QR Code Generator
 *
 * Copyright (c) 2009 Kazuhiko Arase (MIT License)
 * Enhanced & Wrapped (C) 2026, Charles Chiou
 */

var qrcode = function() {

  //---------------------------------------------------------------------
  // qrcode
  //---------------------------------------------------------------------

  /**
   * qrcode
   * @param typeNumber 1 to 40
   * @param errorCorrectionLevel 'L','M','Q','H'
   */
  var qrcode = function(typeNumber, errorCorrectionLevel) {

    var PAD0 = 0xEC;
    var PAD1 = 0x11;

    var _typeNumber = typeNumber;
    var _errorCorrectionLevel = QRErrorCorrectionLevel[errorCorrectionLevel];
    var _modules = null;
    var _moduleCount = 0;
    var _dataCache = null;
    var _dataList = [];

    var _this = {};

    var makeImpl = function(test, maskPattern) {

      _moduleCount = _typeNumber * 4 + 17;
      _modules = function(moduleCount) {
        var modules = new Array(moduleCount);
        for (var row = 0; row < moduleCount; row += 1) {
          modules[row] = new Array(moduleCount);
          for (var col = 0; col < moduleCount; col += 1) {
            modules[row][col] = null;
          }
        }
        return modules;
      }(_moduleCount);

      setupPositionProbePattern(0, 0);
      setupPositionProbePattern(_moduleCount - 7, 0);
      setupPositionProbePattern(0, _moduleCount - 7);
      setupPositionAdjustPattern();
      setupTimingPattern();
      setupTypeInfo(test, maskPattern);

      if (_typeNumber >= 7) {
        setupTypeNumber(test);
      }

      if (_dataCache == null) {
        _dataCache = createData(_typeNumber, _errorCorrectionLevel, _dataList);
      }

      mapData(_dataCache, maskPattern);
    };

    var setupPositionProbePattern = function(row, col) {

      for (var r = -1; r <= 7; r += 1) {

        if (row + r <= -1 || _moduleCount <= row + r) continue;

        for (var c = -1; c <= 7; c += 1) {

          if (col + c <= -1 || _moduleCount <= col + c) continue;

          if ( (0 <= r && r <= 6 && (c == 0 || c == 6) )
              || (0 <= c && c <= 6 && (r == 0 || r == 6) )
              || (2 <= r && r <= 4 && 2 <= c && c <= 4) ) {
            _modules[row + r][col + c] = true;
          } else {
            _modules[row + r][col + c] = false;
          }
        }
      }
    };

    var getBestMaskPattern = function() {

      var minLostPoint = 0;
      var pattern = 0;

      for (var i = 0; i < 8; i += 1) {

        makeImpl(true, i);

        var lostPoint = QRUtil.getLostPoint(_this);

        if (i == 0 || minLostPoint > lostPoint) {
          minLostPoint = lostPoint;
          pattern = i;
        }
      }

      return pattern;
    };

    var setupTimingPattern = function() {

      for (var r = 8; r < _moduleCount - 8; r += 1) {
        if (_modules[r][6] != null) {
          continue;
        }
        _modules[r][6] = (r % 2 == 0);
      }

      for (var c = 8; c < _moduleCount - 8; c += 1) {
        if (_modules[6][c] != null) {
          continue;
        }
        _modules[6][c] = (c % 2 == 0);
      }
    };

    var setupPositionAdjustPattern = function() {

      var pos = QRUtil.getPatternPosition(_typeNumber);

      for (var i = 0; i < pos.length; i += 1) {

        for (var j = 0; j < pos.length; j += 1) {

          var row = pos[i];
          var col = pos[j];

          if (_modules[row][col] != null) {
            continue;
          }

          for (var r = -2; r <= 2; r += 1) {

            for (var c = -2; c <= 2; c += 1) {

              if (r == -2 || r == 2 || c == -2 || c == 2
                  || (r == 0 && c == 0) ) {
                _modules[row + r][col + c] = true;
              } else {
                _modules[row + r][col + c] = false;
              }
            }
          }
        }
      }
    };

    var setupTypeNumber = function(test) {

      var bits = QRUtil.getBCHTypeNumber(_typeNumber);

      for (var i = 0; i < 18; i += 1) {
        var mod = (!test && ( (bits >> i) & 1) == 1);
        _modules[Math.floor(i / 3)][i % 3 + _moduleCount - 8 - 3] = mod;
      }

      for (var i = 0; i < 18; i += 1) {
        var mod = (!test && ( (bits >> i) & 1) == 1);
        _modules[i % 3 + _moduleCount - 8 - 3][Math.floor(i / 3)] = mod;
      }
    };

    var setupTypeInfo = function(test, maskPattern) {

      var data = (_errorCorrectionLevel << 3) | maskPattern;
      var bits = QRUtil.getBCHTypeInfo(data);

      // vertical
      for (var i = 0; i < 15; i += 1) {

        var mod = (!test && ( (bits >> i) & 1) == 1);

        if (i < 6) {
          _modules[i][8] = mod;
        } else if (i < 8) {
          _modules[i + 1][8] = mod;
        } else {
          _modules[_moduleCount - 15 + i][8] = mod;
        }
      }

      // horizontal
      for (var i = 0; i < 15; i += 1) {

        var mod = (!test && ( (bits >> i) & 1) == 1);

        if (i < 8) {
          _modules[8][_moduleCount - i - 1] = mod;
        } else if (i < 9) {
          _modules[8][15 - i - 1 + 1] = mod;
        } else {
          _modules[8][15 - i - 1] = mod;
        }
      }

      // fixed module
      _modules[_moduleCount - 8][8] = (!test);
    };

    var mapData = function(data, maskPattern) {

      var inc = -1;
      var row = _moduleCount - 1;
      var bitIndex = 7;
      var byteIndex = 0;
      var maskFunc = QRUtil.getMaskFunction(maskPattern);

      for (var col = _moduleCount - 1; col > 0; col -= 2) {

        if (col == 6) col -= 1;

        while (true) {

          for (var c = 0; c < 2; c += 1) {

            if (_modules[row][col - c] == null) {

              var dark = false;

              if (byteIndex < data.length) {
                dark = ( ( (data[byteIndex] >>> bitIndex) & 1) == 1);
              }

              var mask = maskFunc(row, col - c);

              if (mask) {
                dark = !dark;
              }

              _modules[row][col - c] = dark;
              bitIndex -= 1;

              if (bitIndex == -1) {
                byteIndex += 1;
                bitIndex = 7;
              }
            }
          }

          row += inc;

          if (row < 0 || _moduleCount <= row) {
            row -= inc;
            inc = -inc;
            break;
          }
        }
      }
    };

    var createBytes = function(buffer, rsBlocks) {

      var offset = 0;

      var maxDcCount = 0;
      var maxEcCount = 0;

      var dcdata = new Array(rsBlocks.length);
      var ecdata = new Array(rsBlocks.length);

      for (var r = 0; r < rsBlocks.length; r += 1) {

        var dcCount = rsBlocks[r].dataCount;
        var ecCount = rsBlocks[r].totalCount - dcCount;

        maxDcCount = Math.max(maxDcCount, dcCount);
        maxEcCount = Math.max(maxEcCount, ecCount);

        dcdata[r] = new Array(dcCount);

        for (var i = 0; i < dcdata[r].length; i += 1) {
          dcdata[r][i] = 0xff & buffer.getBuffer()[i + offset];
        }
        offset += dcCount;

        var rsPoly = QRUtil.getErrorCorrectPolynomial(ecCount);
        var rawPoly = qrPolynomial(dcdata[r], rsPoly.getLength() - 1);

        var modPoly = rawPoly.mod(rsPoly);
        ecdata[r] = new Array(rsPoly.getLength() - 1);
        for (var i = 0; i < ecdata[r].length; i += 1) {
          var modIndex = i + modPoly.getLength() - ecdata[r].length;
          ecdata[r][i] = (modIndex >= 0)? modPoly.getAt(modIndex) : 0;
        }
      }

      var totalCodeCount = 0;
      for (var i = 0; i < rsBlocks.length; i += 1) {
        totalCodeCount += rsBlocks[i].totalCount;
      }

      var data = new Array(totalCodeCount);
      var index = 0;

      for (var i = 0; i < maxDcCount; i += 1) {
        for (var r = 0; r < rsBlocks.length; r += 1) {
          if (i < dcdata[r].length) {
            data[index] = dcdata[r][i];
            index += 1;
          }
        }
      }

      for (var i = 0; i < maxEcCount; i += 1) {
        for (var r = 0; r < rsBlocks.length; r += 1) {
          if (i < ecdata[r].length) {
            data[index] = ecdata[r][i];
            index += 1;
          }
        }
      }

      return data;
    };

    var createData = function(typeNumber, errorCorrectionLevel, dataList) {

      var rsBlocks = QRRSBlock.getRSBlocks(typeNumber, errorCorrectionLevel);

      var buffer = qrBitBuffer();

      for (var i = 0; i < dataList.length; i += 1) {
        var data = dataList[i];
        buffer.put(data.getMode(), 4);
        buffer.put(data.getLength(), QRUtil.getLengthInBits(data.getMode(), typeNumber) );
        data.write(buffer);
      }

      // calc num max data.
      var totalDataCount = 0;
      for (var i = 0; i < rsBlocks.length; i += 1) {
        totalDataCount += rsBlocks[i].dataCount;
      }

      if (buffer.getLengthInBits() > totalDataCount * 8) {
        throw 'code length overflow. ('
          + buffer.getLengthInBits()
          + '>'
          + totalDataCount * 8
          + ')';
      }

      // end code
      if (buffer.getLengthInBits() + 4 <= totalDataCount * 8) {
        buffer.put(0, 4);
      }

      // padding
      while (buffer.getLengthInBits() % 8 != 0) {
        buffer.putBit(false);
      }

      // padding
      while (true) {

        if (buffer.getLengthInBits() >= totalDataCount * 8) {
          break;
        }
        buffer.put(PAD0, 8);

        if (buffer.getLengthInBits() >= totalDataCount * 8) {
          break;
        }
        buffer.put(PAD1, 8);
      }

      return createBytes(buffer, rsBlocks);
    };

    _this.addData = function(data, mode) {

      mode = mode || 'Byte';

      var newData = null;

      switch(mode) {
      case 'Numeric' :
        newData = qrNumber(data);
        break;
      case 'Alphanumeric' :
        newData = qrAlphaNum(data);
        break;
      case 'Byte' :
        newData = qr8BitByte(data);
        break;
      case 'Kanji' :
        newData = qrKanji(data);
        break;
      default :
        throw 'mode:' + mode;
      }

      _dataList.push(newData);
      _dataCache = null;
    };

    _this.isDark = function(row, col) {
      if (row < 0 || _moduleCount <= row || col < 0 || _moduleCount <= col) {
        throw row + ',' + col;
      }
      return _modules[row][col];
    };

    _this.getModuleCount = function() {
      return _moduleCount;
    };

    _this.make = function() {
      if (_typeNumber < 1) {
        var typeNumber = 1;

        for (; typeNumber < 40; typeNumber++) {
          var rsBlocks = QRRSBlock.getRSBlocks(typeNumber, _errorCorrectionLevel);
          var buffer = qrBitBuffer();

          for (var i = 0; i < _dataList.length; i++) {
            var data = _dataList[i];
            buffer.put(data.getMode(), 4);
            buffer.put(data.getLength(), QRUtil.getLengthInBits(data.getMode(), typeNumber) );
            data.write(buffer);
          }

          var totalDataCount = 0;
          for (var i = 0; i < rsBlocks.length; i++) {
            totalDataCount += rsBlocks[i].dataCount;
          }

          if (buffer.getLengthInBits() <= totalDataCount * 8) {
            break;
          }
        }

        _typeNumber = typeNumber;
      }

      makeImpl(false, getBestMaskPattern() );
    };

    _this.createTableTag = function(cellSize, margin) {

      cellSize = cellSize || 2;
      margin = (typeof margin == 'undefined')? cellSize * 4 : margin;

      var qrHtml = '';

      qrHtml += '<table style="';
      qrHtml += ' border-width: 0px; border-style: none;';
      qrHtml += ' border-collapse: collapse;';
      qrHtml += ' padding: 0px; margin: ' + margin + 'px;';
      qrHtml += '">';
      qrHtml += '<tbody>';

      for (var r = 0; r < _this.getModuleCount(); r += 1) {

        qrHtml += '<tr>';

        for (var c = 0; c < _this.getModuleCount(); c += 1) {
          qrHtml += '<td style="';
          qrHtml += ' border-width: 0px; border-style: none;';
          qrHtml += ' border-collapse: collapse;';
          qrHtml += ' padding: 0px; margin: 0px;';
          qrHtml += ' width: ' + cellSize + 'px;';
          qrHtml += ' height: ' + cellSize + 'px;';
          qrHtml += ' background-color: ';
          qrHtml += _this.isDark(r, c)? '#000000' : '#ffffff';
          qrHtml += ';';
          qrHtml += '"/>';
        }

        qrHtml += '</tr>';
      }

      qrHtml += '</tbody>';
      qrHtml += '</table>';

      return qrHtml;
    };

    _this.createSvgTag = function(cellSize, margin, alt, title) {

      var opts = {};
      if (typeof arguments[0] == 'object') {
        // Called by options.
        opts = arguments[0];
        // overwrite cellSize and margin.
        cellSize = opts.cellSize;
        margin = opts.margin;
        alt = opts.alt;
        title = opts.title;
      }

      cellSize = cellSize || 2;
      margin = (typeof margin == 'undefined')? cellSize * 4 : margin;

      // Compose alt property surrogate
      alt = (typeof alt === 'string') ? {text: alt} : alt || {};
      alt.text = alt.text || null;
      alt.id = (alt.text) ? alt.id || 'qrcode-description' : null;

      // Compose title property surrogate
      title = (typeof title === 'string') ? {text: title} : title || {};
      title.text = title.text || null;
      title.id = (title.text) ? title.id || 'qrcode-title' : null;

      var size = _this.getModuleCount() * cellSize + margin * 2;
      var c, mc, r, mr, qrSvg='', rect;

      rect = 'l' + cellSize + ',0 0,' + cellSize +
        ' -' + cellSize + ',0 0,-' + cellSize + 'z ';

      qrSvg += '<svg version="1.1" xmlns="http://www.w3.org/2000/svg"';
      qrSvg += !opts.scalable ? ' width="' + size + 'px" height="' + size + 'px"' : '';
      qrSvg += ' viewBox="0 0 ' + size + ' ' + size + '" ';
      qrSvg += ' preserveAspectRatio="xMinYMin meet"';
      qrSvg += (title.text || alt.text) ? ' role="img" aria-labelledby="' +
          escapeXml([title.id, alt.id].join(' ').trim() ) + '"' : '';
      qrSvg += '>';
      qrSvg += (title.text) ? '<title id="' + escapeXml(title.id) + '">' +
          escapeXml(title.text) + '</title>' : '';
      qrSvg += (alt.text) ? '<description id="' + escapeXml(alt.id) + '">' +
          escapeXml(alt.text) + '</description>' : '';
      qrSvg += '<rect width="100%" height="100%" fill="white" cx="0" cy="0"/>';
      qrSvg += '<path d="';

      for (r = 0; r < _this.getModuleCount(); r += 1) {
        mr = r * cellSize + margin;
        for (c = 0; c < _this.getModuleCount(); c += 1) {
          if (_this.isDark(r, c) ) {
            mc = c*cellSize+margin;
            qrSvg += 'M' + mc + ',' + mr + rect;
          }
        }
      }

      qrSvg += '" stroke="transparent" fill="black"/>';
      qrSvg += '</svg>';

      return qrSvg;
    };

    _this.createDataURL = function(cellSize, margin) {

      cellSize = cellSize || 2;
      margin = (typeof margin == 'undefined')? cellSize * 4 : margin;

      var size = _this.getModuleCount() * cellSize + margin * 2;
      var min = margin;
      var max = size - margin;

      return createDataURL(size, size, function(x, y) {
        if (min <= x && x < max && min <= y && y < max) {
          var c = Math.floor( (x - min) / cellSize);
          var r = Math.floor( (y - min) / cellSize);
          return _this.isDark(r, c)? 0 : 1;
        } else {
          return 1;
        }
      } );
    };

    _this.createImgTag = function(cellSize, margin, alt) {

      cellSize = cellSize || 2;
      margin = (typeof margin == 'undefined')? cellSize * 4 : margin;

      var size = _this.getModuleCount() * cellSize + margin * 2;

      var img = '';
      img += '<img';
      img += '\u0020src="';
      img += _this.createDataURL(cellSize, margin);
      img += '"';
      img += '\u0020width="';
      img += size;
      img += '"';
      img += '\u0020height="';
      img += size;
      img += '"';
      if (alt) {
        img += '\u0020alt="';
        img += escapeXml(alt);
        img += '"';
      }
      img += '/>';

      return img;
    };

    var escapeXml = function(s) {
      var escaped = '';
      for (var i = 0; i < s.length; i += 1) {
        var c = s.charAt(i);
        switch(c) {
        case '<': escaped += '&lt;'; break;
        case '>': escaped += '&gt;'; break;
        case '&': escaped += '&amp;'; break;
        case '"': escaped += '&quot;'; break;
        default : escaped += c; break;
        }
      }
      return escaped;
    };

    var _createHalfASCII = function(margin) {
      var cellSize = 1;
      margin = (typeof margin == 'undefined')? cellSize * 2 : margin;

      var size = _this.getModuleCount() * cellSize + margin * 2;
      var min = margin;
      var max = size - margin;

      var y, x, r1, r2, p;

      var blocks = {
        '██': '█',
        '█ ': '▀',
        ' █': '▄',
        '  ': ' '
      };

      var blocksLastLineNoMargin = {
        '██': '▀',
        '█ ': '▀',
        ' █': ' ',
        '  ': ' '
      };

      var ascii = '';
      for (y = 0; y < size; y += 2) {
        r1 = Math.floor((y - min) / cellSize);
        r2 = Math.floor((y + 1 - min) / cellSize);
        for (x = 0; x < size; x += 1) {
          p = '█';

          if (min <= x && x < max && min <= y && y < max && _this.isDark(r1, Math.floor((x - min) / cellSize))) {
            p = ' ';
          }

          if (min <= x && x < max && min <= y+1 && y+1 < max && _this.isDark(r2, Math.floor((x - min) / cellSize))) {
            p += ' ';
          }
          else {
            p += '█';
          }

          // Output 2 characters per pixel, to create full square. 1 character per pixels gives only half width of square.
          ascii += (margin < 1 && y+1 >= max) ? blocksLastLineNoMargin[p] : blocks[p];
        }

        ascii += '\n';
      }

      if (size % 2 && margin > 0) {
        return ascii.substring(0, ascii.length - size - 1) + Array(size+1).join('▀');
      }

      return ascii.substring(0, ascii.length-1);
    };

    _this.createASCII = function(cellSize, margin) {
      cellSize = cellSize || 1;

      if (cellSize < 2) {
        return _createHalfASCII(margin);
      }

      cellSize -= 1;
      margin = (typeof margin == 'undefined')? cellSize * 2 : margin;

      var size = _this.getModuleCount() * cellSize + margin * 2;
      var min = margin;
      var max = size - margin;

      var y, x, r, p;

      var white = Array(cellSize+1).join('██');
      var black = Array(cellSize+1).join('  ');

      var ascii = '';
      var line = '';
      for (y = 0; y < size; y += 1) {
        r = Math.floor( (y - min) / cellSize);
        line = '';
        for (x = 0; x < size; x += 1) {
          p = 1;

          if (min <= x && x < max && min <= y && y < max && _this.isDark(r, Math.floor((x - min) / cellSize))) {
            p = 0;
          }

          // Output 2 characters per pixel, to create full square. 1 character per pixels gives only half width of square.
          line += p ? white : black;
        }

        for (r = 0; r < cellSize; r += 1) {
          ascii += line + '\n';
        }
      }

      return ascii.substring(0, ascii.length-1);
    };

    _this.renderTo2dContext = function(context, cellSize) {
      cellSize = cellSize || 2;
      var length = _this.getModuleCount();
      for (var row = 0; row < length; row++) {
        for (var col = 0; col < length; col++) {
          context.fillStyle = _this.isDark(row, col) ? 'black' : 'white';
          context.fillRect(row * cellSize, col * cellSize, cellSize, cellSize);
        }
      }
    }

    return _this;
  };

  //---------------------------------------------------------------------
  // qrcode.stringToBytes
  //---------------------------------------------------------------------

  qrcode.stringToBytesFuncs = {
    'default' : function(s) {
      var bytes = [];
      for (var i = 0; i < s.length; i += 1) {
        var c = s.charCodeAt(i);
        bytes.push(c & 0xff);
      }
      return bytes;
    }
  };

  qrcode.stringToBytes = qrcode.stringToBytesFuncs['default'];

  //---------------------------------------------------------------------
  // qrcode.createStringToBytes
  //---------------------------------------------------------------------

  /**
   * @param unicodeData base64 string of byte array.
   * [16bit Unicode],[16bit Bytes], ...
   * @param numChars
   */
  qrcode.createStringToBytes = function(unicodeData, numChars) {

    // create conversion map.

    var unicodeMap = function() {

      var bin = base64DecodeInputStream(unicodeData);
      var read = function() {
        var b = bin.read();
        if (b == -1) throw 'eof';
        return b;
      };

      var count = 0;
      var unicodeMap = {};
      while (true) {
        var b0 = bin.read();
        if (b0 == -1) break;
        var b1 = read();
        var b2 = read();
        var b3 = read();
        var k = String.fromCharCode( (b0 << 8) | b1);
        var v = (b2 << 8) | b3;
        unicodeMap[k] = v;
        count += 1;
      }
      if (count != numChars) {
        throw count + ' != ' + numChars;
      }

      return unicodeMap;
    }();

    var unknownChar = '?'.charCodeAt(0);

    return function(s) {
      var bytes = [];
      for (var i = 0; i < s.length; i += 1) {
        var c = s.charCodeAt(i);
        if (c < 128) {
          bytes.push(c);
        } else {
          var b = unicodeMap[s.charAt(i)];
          if (typeof b == 'number') {
            if ( (b & 0xff) == b) {
              // 1byte
              bytes.push(b);
            } else {
              // 2bytes
              bytes.push(b >>> 8);
              bytes.push(b & 0xff);
            }
          } else {
            bytes.push(unknownChar);
          }
        }
      }
      return bytes;
    };
  };

  //---------------------------------------------------------------------
  // QRMode
  //---------------------------------------------------------------------

  var QRMode = {
    MODE_NUMBER :    1 << 0,
    MODE_ALPHA_NUM : 1 << 1,
    MODE_8BIT_BYTE : 1 << 2,
    MODE_KANJI :     1 << 3
  };

  //---------------------------------------------------------------------
  // QRErrorCorrectionLevel
  //---------------------------------------------------------------------

  var QRErrorCorrectionLevel = {
    L : 1,
    M : 0,
    Q : 3,
    H : 2
  };

  //---------------------------------------------------------------------
  // QRMaskPattern
  //---------------------------------------------------------------------

  var QRMaskPattern = {
    PATTERN000 : 0,
    PATTERN001 : 1,
    PATTERN010 : 2,
    PATTERN011 : 3,
    PATTERN100 : 4,
    PATTERN101 : 5,
    PATTERN110 : 6,
    PATTERN111 : 7
  };

  //---------------------------------------------------------------------
  // QRUtil
  //---------------------------------------------------------------------

  var QRUtil = function() {

    var PATTERN_POSITION_TABLE = [
      [],
      [6, 18],
      [6, 22],
      [6, 26],
      [6, 30],
      [6, 34],
      [6, 22, 38],
      [6, 24, 42],
      [6, 26, 46],
      [6, 28, 50],
      [6, 30, 54],
      [6, 32, 58],
      [6, 34, 62],
      [6, 26, 46, 66],
      [6, 26, 48, 70],
      [6, 26, 50, 74],
      [6, 30, 54, 78],
      [6, 30, 56, 82],
      [6, 30, 58, 86],
      [6, 34, 62, 90],
      [6, 28, 50, 72, 94],
      [6, 26, 50, 74, 98],
      [6, 30, 54, 78, 102],
      [6, 28, 54, 80, 106],
      [6, 32, 58, 84, 110],
      [6, 30, 58, 86, 114],
      [6, 34, 62, 90, 118],
      [6, 26, 50, 74, 98, 122],
      [6, 30, 54, 78, 102, 126],
      [6, 26, 52, 78, 104, 130],
      [6, 30, 56, 82, 108, 134],
      [6, 34, 60, 86, 112, 138],
      [6, 30, 58, 86, 114, 142],
      [6, 34, 62, 90, 118, 146],
      [6, 30, 54, 78, 102, 126, 150],
      [6, 24, 50, 76, 102, 128, 154],
      [6, 28, 54, 80, 106, 132, 158],
      [6, 32, 58, 84, 110, 136, 162],
      [6, 26, 54, 82, 110, 138, 166],
      [6, 30, 58, 86, 114, 142, 170]
    ];
    var G15 = (1 << 10) | (1 << 8) | (1 << 5) | (1 << 4) | (1 << 2) | (1 << 1) | (1 << 0);
    var G18 = (1 << 12) | (1 << 11) | (1 << 10) | (1 << 9) | (1 << 8) | (1 << 5) | (1 << 2) | (1 << 0);
    var G15_MASK = (1 << 14) | (1 << 12) | (1 << 10) | (1 << 4) | (1 << 1);

    var _this = {};

    var getBCHDigit = function(data) {
      var digit = 0;
      while (data != 0) {
        digit += 1;
        data >>>= 1;
      }
      return digit;
    };

    _this.getBCHTypeInfo = function(data) {
      var d = data << 10;
      while (getBCHDigit(d) - getBCHDigit(G15) >= 0) {
        d ^= (G15 << (getBCHDigit(d) - getBCHDigit(G15) ) );
      }
      return ( (data << 10) | d) ^ G15_MASK;
    };

    _this.getBCHTypeNumber = function(data) {
      var d = data << 12;
      while (getBCHDigit(d) - getBCHDigit(G18) >= 0) {
        d ^= (G18 << (getBCHDigit(d) - getBCHDigit(G18) ) );
      }
      return (data << 12) | d;
    };

    _this.getPatternPosition = function(typeNumber) {
      return PATTERN_POSITION_TABLE[typeNumber - 1];
    };

    _this.getMaskFunction = function(maskPattern) {

      switch (maskPattern) {

      case QRMaskPattern.PATTERN000 :
        return function(i, j) { return (i + j) % 2 == 0; };
      case QRMaskPattern.PATTERN001 :
        return function(i, j) { return i % 2 == 0; };
      case QRMaskPattern.PATTERN010 :
        return function(i, j) { return j % 3 == 0; };
      case QRMaskPattern.PATTERN011 :
        return function(i, j) { return (i + j) % 3 == 0; };
      case QRMaskPattern.PATTERN100 :
        return function(i, j) { return (Math.floor(i / 2) + Math.floor(j / 3) ) % 2 == 0; };
      case QRMaskPattern.PATTERN101 :
        return function(i, j) { return (i * j) % 2 + (i * j) % 3 == 0; };
      case QRMaskPattern.PATTERN110 :
        return function(i, j) { return ( (i * j) % 2 + (i * j) % 3) % 2 == 0; };
      case QRMaskPattern.PATTERN111 :
        return function(i, j) { return ( (i * j) % 3 + (i + j) % 2) % 2 == 0; };

      default :
        throw 'bad maskPattern:' + maskPattern;
      }
    };

    _this.getErrorCorrectPolynomial = function(errorCorrectLength) {
      var a = qrPolynomial([1], 0);
      for (var i = 0; i < errorCorrectLength; i += 1) {
        a = a.multiply(qrPolynomial([1, QRMath.gexp(i)], 0) );
      }
      return a;
    };

    _this.getLengthInBits = function(mode, type) {

      if (1 <= type && type < 10) {

        // 1 - 9

        switch(mode) {
        case QRMode.MODE_NUMBER    : return 10;
        case QRMode.MODE_ALPHA_NUM : return 9;
        case QRMode.MODE_8BIT_BYTE : return 8;
        case QRMode.MODE_KANJI     : return 8;
        default :
          throw 'mode:' + mode;
        }

      } else if (type < 27) {

        // 10 - 26

        switch(mode) {
        case QRMode.MODE_NUMBER    : return 12;
        case QRMode.MODE_ALPHA_NUM : return 11;
        case QRMode.MODE_8BIT_BYTE : return 16;
        case QRMode.MODE_KANJI     : return 10;
        default :
          throw 'mode:' + mode;
        }

      } else if (type < 41) {

        // 27 - 40

        switch(mode) {
        case QRMode.MODE_NUMBER    : return 14;
        case QRMode.MODE_ALPHA_NUM : return 13;
        case QRMode.MODE_8BIT_BYTE : return 16;
        case QRMode.MODE_KANJI     : return 12;
        default :
          throw 'mode:' + mode;
        }

      } else {
        throw 'type:' + type;
      }
    };

    _this.getLostPoint = function(qrcode) {

      var moduleCount = qrcode.getModuleCount();

      var lostPoint = 0;

      // LEVEL1

      for (var row = 0; row < moduleCount; row += 1) {
        for (var col = 0; col < moduleCount; col += 1) {

          var sameCount = 0;
          var dark = qrcode.isDark(row, col);

          for (var r = -1; r <= 1; r += 1) {

            if (row + r < 0 || moduleCount <= row + r) {
              continue;
            }

            for (var c = -1; c <= 1; c += 1) {

              if (col + c < 0 || moduleCount <= col + c) {
                continue;
              }

              if (r == 0 && c == 0) {
                continue;
              }

              if (dark == qrcode.isDark(row + r, col + c) ) {
                sameCount += 1;
              }
            }
          }

          if (sameCount > 5) {
            lostPoint += (3 + sameCount - 5);
          }
        }
      };

      // LEVEL2

      for (var row = 0; row < moduleCount - 1; row += 1) {
        for (var col = 0; col < moduleCount - 1; col += 1) {
          var count = 0;
          if (qrcode.isDark(row, col) ) count += 1;
          if (qrcode.isDark(row + 1, col) ) count += 1;
          if (qrcode.isDark(row, col + 1) ) count += 1;
          if (qrcode.isDark(row + 1, col + 1) ) count += 1;
          if (count == 0 || count == 4) {
            lostPoint += 3;
          }
        }
      }

      // LEVEL3

      for (var row = 0; row < moduleCount; row += 1) {
        for (var col = 0; col < moduleCount - 6; col += 1) {
          if (qrcode.isDark(row, col)
              && !qrcode.isDark(row, col + 1)
              &&  qrcode.isDark(row, col + 2)
              &&  qrcode.isDark(row, col + 3)
              &&  qrcode.isDark(row, col + 4)
              && !qrcode.isDark(row, col + 5)
              &&  qrcode.isDark(row, col + 6) ) {
            lostPoint += 40;
          }
        }
      }

      for (var col = 0; col < moduleCount; col += 1) {
        for (var row = 0; row < moduleCount - 6; row += 1) {
          if (qrcode.isDark(row, col)
              && !qrcode.isDark(row + 1, col)
              &&  qrcode.isDark(row + 2, col)
              &&  qrcode.isDark(row + 3, col)
              &&  qrcode.isDark(row + 4, col)
              && !qrcode.isDark(row + 5, col)
              &&  qrcode.isDark(row + 6, col) ) {
            lostPoint += 40;
          }
        }
      }

      // LEVEL4

      var darkCount = 0;

      for (var col = 0; col < moduleCount; col += 1) {
        for (var row = 0; row < moduleCount; row += 1) {
          if (qrcode.isDark(row, col) ) {
            darkCount += 1;
          }
        }
      }

      var ratio = Math.abs(100 * darkCount / moduleCount / moduleCount - 50) / 5;
      lostPoint += ratio * 10;

      return lostPoint;
    };

    return _this;
  }();

  //---------------------------------------------------------------------
  // QRMath
  //---------------------------------------------------------------------

  var QRMath = function() {

    var EXP_TABLE = new Array(256);
    var LOG_TABLE = new Array(256);

    // initialize tables
    for (var i = 0; i < 8; i += 1) {
      EXP_TABLE[i] = 1 << i;
    }
    for (var i = 8; i < 256; i += 1) {
      EXP_TABLE[i] = EXP_TABLE[i - 4]
        ^ EXP_TABLE[i - 5]
        ^ EXP_TABLE[i - 6]
        ^ EXP_TABLE[i - 8];
    }
    for (var i = 0; i < 255; i += 1) {
      LOG_TABLE[EXP_TABLE[i] ] = i;
    }

    var _this = {};

    _this.glog = function(n) {

      if (n < 1) {
        throw 'glog(' + n + ')';
      }

      return LOG_TABLE[n];
    };

    _this.gexp = function(n) {

      while (n < 0) {
        n += 255;
      }

      while (n >= 256) {
        n -= 255;
      }

      return EXP_TABLE[n];
    };

    return _this;
  }();

  //---------------------------------------------------------------------
  // qrPolynomial
  //---------------------------------------------------------------------

  function qrPolynomial(num, shift) {

    if (typeof num.length == 'undefined') {
      throw num.length + '/' + shift;
    }

    var _num = function() {
      var offset = 0;
      while (offset < num.length && num[offset] == 0) {
        offset += 1;
      }
      var _num = new Array(num.length - offset + shift);
      for (var i = 0; i < num.length - offset; i += 1) {
        _num[i] = num[i + offset];
      }
      return _num;
    }();

    var _this = {};

    _this.getAt = function(index) {
      return _num[index];
    };

    _this.getLength = function() {
      return _num.length;
    };

    _this.multiply = function(e) {

      var num = new Array(_this.getLength() + e.getLength() - 1);

      for (var i = 0; i < _this.getLength(); i += 1) {
        for (var j = 0; j < e.getLength(); j += 1) {
          num[i + j] ^= QRMath.gexp(QRMath.glog(_this.getAt(i) ) + QRMath.glog(e.getAt(j) ) );
        }
      }

      return qrPolynomial(num, 0);
    };

    _this.mod = function(e) {

      if (_this.getLength() - e.getLength() < 0) {
        return _this;
      }

      var ratio = QRMath.glog(_this.getAt(0) ) - QRMath.glog(e.getAt(0) );

      var num = new Array(_this.getLength() );
      for (var i = 0; i < _this.getLength(); i += 1) {
        num[i] = _this.getAt(i);
      }

      for (var i = 0; i < e.getLength(); i += 1) {
        num[i] ^= QRMath.gexp(QRMath.glog(e.getAt(i) ) + ratio);
      }

      // recursive call
      return qrPolynomial(num, 0).mod(e);
    };

    return _this;
  };

  //---------------------------------------------------------------------
  // QRRSBlock
  //---------------------------------------------------------------------

  var QRRSBlock = function() {

    var RS_BLOCK_TABLE = [

      // L
      // M
      // Q
      // H

      // 1
      [1, 26, 19],
      [1, 26, 16],
      [1, 26, 13],
      [1, 26, 9],

      // 2
      [1, 44, 34],
      [1, 44, 28],
      [1, 44, 22],
      [1, 44, 16],

      // 3
      [1, 70, 55],
      [1, 70, 44],
      [2, 35, 17],
      [2, 35, 13],

      // 4
      [1, 100, 80],
      [2, 50, 32],
      [2, 50, 24],
      [4, 25, 9],

      // 5
      [1, 134, 108],
      [2, 67, 43],
      [2, 33, 15, 2, 34, 16],
      [2, 33, 11, 2, 34, 12],

      // 6
      [2, 86, 68],
      [4, 43, 27],
      [4, 43, 19],
      [4, 43, 15],

      // 7
      [2, 98, 78],
      [4, 49, 31],
      [2, 32, 14, 4, 33, 15],
      [4, 39, 13, 1, 40, 14],

      // 8
      [2, 121, 97],
      [2, 60, 38, 2, 61, 39],
      [4, 40, 18, 2, 41, 19],
      [4, 40, 14, 2, 41, 15],

      // 9
      [2, 146, 116],
      [3, 58, 36, 2, 59, 37],
      [4, 36, 16, 4, 37, 17],
      [4, 36, 12, 4, 37, 13],

      // 10
      [2, 86, 68, 2, 87, 69],
      [4, 69, 43, 1, 70, 44],
      [6, 43, 19, 2, 44, 20],
      [6, 43, 15, 2, 44, 16],

      // 11
      [4, 101, 81],
      [1, 80, 50, 4, 81, 51],
      [4, 50, 22, 4, 51, 23],
      [3, 36, 12, 8, 37, 13],

      // 12
      [2, 116, 92, 2, 117, 93],
      [6, 58, 36, 2, 59, 37],
      [4, 46, 20, 6, 47, 21],
      [7, 42, 14, 4, 43, 15],

      // 13
      [4, 133, 107],
      [8, 59, 37, 1, 60, 38],
      [8, 44, 20, 4, 45, 21],
      [12, 33, 11, 4, 34, 12],

      // 14
      [3, 145, 115, 1, 146, 116],
      [4, 64, 40, 5, 65, 41],
      [11, 36, 16, 5, 37, 17],
      [11, 36, 12, 5, 37, 13],

      // 15
      [5, 109, 87, 1, 110, 88],
      [5, 65, 41, 5, 66, 42],
      [5, 54, 24, 7, 55, 25],
      [11, 36, 12, 7, 37, 13],

      // 16
      [5, 122, 98, 1, 123, 99],
      [7, 73, 45, 3, 74, 46],
      [15, 43, 19, 2, 44, 20],
      [3, 45, 15, 13, 46, 16],

      // 17
      [1, 135, 107, 5, 136, 108],
      [10, 74, 46, 1, 75, 47],
      [1, 50, 22, 15, 51, 23],
      [2, 42, 14, 17, 43, 15],

      // 18
      [5, 150, 120, 1, 151, 121],
      [9, 69, 43, 4, 70, 44],
      [17, 50, 22, 1, 51, 23],
      [2, 42, 14, 19, 43, 15],

      // 19
      [3, 141, 113, 4, 142, 114],
      [3, 70, 44, 11, 71, 45],
      [17, 47, 21, 4, 48, 22],
      [9, 39, 13, 16, 40, 14],

      // 20
      [3, 135, 107, 5, 136, 108],
      [3, 67, 41, 13, 68, 42],
      [15, 54, 24, 5, 55, 25],
      [15, 43, 15, 10, 44, 16],

      // 21
      [4, 144, 116, 4, 145, 117],
      [17, 68, 42],
      [17, 50, 22, 6, 51, 23],
      [19, 46, 16, 6, 47, 17],

      // 22
      [2, 139, 111, 7, 140, 112],
      [17, 74, 46],
      [7, 54, 24, 16, 55, 25],
      [34, 37, 13],

      // 23
      [4, 151, 121, 5, 152, 122],
      [4, 75, 47, 14, 76, 48],
      [11, 54, 24, 14, 55, 25],
      [16, 45, 15, 14, 46, 16],

      // 24
      [6, 147, 117, 4, 148, 118],
      [6, 73, 45, 14, 74, 46],
      [11, 54, 24, 16, 55, 25],
      [30, 46, 16, 2, 47, 17],

      // 25
      [8, 132, 106, 4, 133, 107],
      [8, 75, 47, 13, 76, 48],
      [7, 54, 24, 22, 55, 25],
      [22, 45, 15, 13, 46, 16],

      // 26
      [10, 142, 114, 2, 143, 115],
      [19, 74, 46, 4, 75, 47],
      [28, 50, 22, 6, 51, 23],
      [33, 46, 16, 4, 47, 17],

      // 27
      [8, 152, 122, 4, 153, 123],
      [22, 73, 45, 3, 74, 46],
      [8, 53, 23, 26, 54, 24],
      [12, 45, 15, 28, 46, 16],

      // 28
      [3, 147, 117, 10, 148, 118],
      [3, 73, 45, 23, 74, 46],
      [4, 54, 24, 31, 55, 25],
      [11, 45, 15, 31, 46, 16],

      // 29
      [7, 146, 116, 7, 147, 117],
      [21, 73, 45, 7, 74, 46],
      [1, 53, 23, 37, 54, 24],
      [19, 45, 15, 26, 46, 16],

      // 30
      [5, 145, 115, 10, 146, 116],
      [19, 75, 47, 10, 76, 48],
      [15, 54, 24, 25, 55, 25],
      [23, 45, 15, 25, 46, 16],

      // 31
      [13, 145, 115, 3, 146, 116],
      [2, 74, 46, 29, 75, 47],
      [42, 54, 24, 1, 55, 25],
      [23, 45, 15, 28, 46, 16],

      // 32
      [17, 145, 115],
      [10, 74, 46, 23, 75, 47],
      [10, 54, 24, 35, 55, 25],
      [19, 45, 15, 35, 46, 16],

      // 33
      [17, 145, 115, 1, 146, 116],
      [14, 74, 46, 21, 75, 47],
      [29, 54, 24, 19, 55, 25],
      [11, 45, 15, 46, 46, 16],

      // 34
      [13, 145, 115, 6, 146, 116],
      [14, 74, 46, 23, 75, 47],
      [44, 54, 24, 7, 55, 25],
      [59, 46, 16, 1, 47, 17],

      // 35
      [12, 151, 121, 7, 152, 122],
      [12, 75, 47, 26, 76, 48],
      [39, 54, 24, 14, 55, 25],
      [22, 45, 15, 41, 46, 16],

      // 36
      [6, 151, 121, 14, 152, 122],
      [6, 75, 47, 34, 76, 48],
      [46, 54, 24, 10, 55, 25],
      [2, 45, 15, 64, 46, 16],

      // 37
      [17, 152, 122, 4, 153, 123],
      [29, 74, 46, 14, 75, 47],
      [49, 54, 24, 10, 55, 25],
      [24, 45, 15, 46, 46, 16],

      // 38
      [4, 152, 122, 18, 153, 123],
      [13, 74, 46, 32, 75, 47],
      [48, 54, 24, 14, 55, 25],
      [42, 45, 15, 32, 46, 16],

      // 39
      [20, 147, 117, 4, 148, 118],
      [40, 75, 47, 7, 76, 48],
      [43, 54, 24, 22, 55, 25],
      [10, 45, 15, 67, 46, 16],

      // 40
      [19, 148, 118, 6, 149, 119],
      [18, 75, 47, 31, 76, 48],
      [34, 54, 24, 34, 55, 25],
      [20, 45, 15, 61, 46, 16]
    ];

    var qrRSBlock = function(totalCount, dataCount) {
      var _this = {};
      _this.totalCount = totalCount;
      _this.dataCount = dataCount;
      return _this;
    };

    var _this = {};

    var getRsBlockTable = function(typeNumber, errorCorrectionLevel) {

      switch(errorCorrectionLevel) {
      case QRErrorCorrectionLevel.L :
        return RS_BLOCK_TABLE[(typeNumber - 1) * 4 + 0];
      case QRErrorCorrectionLevel.M :
        return RS_BLOCK_TABLE[(typeNumber - 1) * 4 + 1];
      case QRErrorCorrectionLevel.Q :
        return RS_BLOCK_TABLE[(typeNumber - 1) * 4 + 2];
      case QRErrorCorrectionLevel.H :
        return RS_BLOCK_TABLE[(typeNumber - 1) * 4 + 3];
      default :
        return undefined;
      }
    };

    _this.getRSBlocks = function(typeNumber, errorCorrectionLevel) {

      var rsBlock = getRsBlockTable(typeNumber, errorCorrectionLevel);

      if (typeof rsBlock == 'undefined') {
        throw 'bad rs block @ typeNumber:' + typeNumber +
            '/errorCorrectionLevel:' + errorCorrectionLevel;
      }

      var length = rsBlock.length / 3;

      var list = [];

      for (var i = 0; i < length; i += 1) {

        var count = rsBlock[i * 3 + 0];
        var totalCount = rsBlock[i * 3 + 1];
        var dataCount = rsBlock[i * 3 + 2];

        for (var j = 0; j < count; j += 1) {
          list.push(qrRSBlock(totalCount, dataCount) );
        }
      }

      return list;
    };

    return _this;
  }();

  //---------------------------------------------------------------------
  // qrBitBuffer
  //---------------------------------------------------------------------

  var qrBitBuffer = function() {

    var _buffer = [];
    var _length = 0;

    var _this = {};

    _this.getBuffer = function() {
      return _buffer;
    };

    _this.getAt = function(index) {
      var bufIndex = Math.floor(index / 8);
      return ( (_buffer[bufIndex] >>> (7 - index % 8) ) & 1) == 1;
    };

    _this.put = function(num, length) {
      for (var i = 0; i < length; i += 1) {
        _this.putBit( ( (num >>> (length - i - 1) ) & 1) == 1);
      }
    };

    _this.getLengthInBits = function() {
      return _length;
    };

    _this.putBit = function(bit) {

      var bufIndex = Math.floor(_length / 8);
      if (_buffer.length <= bufIndex) {
        _buffer.push(0);
      }

      if (bit) {
        _buffer[bufIndex] |= (0x80 >>> (_length % 8) );
      }

      _length += 1;
    };

    return _this;
  };

  //---------------------------------------------------------------------
  // qrNumber
  //---------------------------------------------------------------------

  var qrNumber = function(data) {

    var _mode = QRMode.MODE_NUMBER;
    var _data = data;

    var _this = {};

    _this.getMode = function() {
      return _mode;
    };

    _this.getLength = function(buffer) {
      return _data.length;
    };

    _this.write = function(buffer) {

      var data = _data;

      var i = 0;

      while (i + 2 < data.length) {
        buffer.put(strToNum(data.substring(i, i + 3) ), 10);
        i += 3;
      }

      if (i < data.length) {
        if (data.length - i == 1) {
          buffer.put(strToNum(data.substring(i, i + 1) ), 4);
        } else if (data.length - i == 2) {
          buffer.put(strToNum(data.substring(i, i + 2) ), 7);
        }
      }
    };

    var strToNum = function(s) {
      var num = 0;
      for (var i = 0; i < s.length; i += 1) {
        num = num * 10 + chatToNum(s.charAt(i) );
      }
      return num;
    };

    var chatToNum = function(c) {
      if ('0' <= c && c <= '9') {
        return c.charCodeAt(0) - '0'.charCodeAt(0);
      }
      throw 'illegal char :' + c;
    };

    return _this;
  };

  //---------------------------------------------------------------------
  // qrAlphaNum
  //---------------------------------------------------------------------

  var qrAlphaNum = function(data) {

    var _mode = QRMode.MODE_ALPHA_NUM;
    var _data = data;

    var _this = {};

    _this.getMode = function() {
      return _mode;
    };

    _this.getLength = function(buffer) {
      return _data.length;
    };

    _this.write = function(buffer) {

      var s = _data;

      var i = 0;

      while (i + 1 < s.length) {
        buffer.put(
          getCode(s.charAt(i) ) * 45 +
          getCode(s.charAt(i + 1) ), 11);
        i += 2;
      }

      if (i < s.length) {
        buffer.put(getCode(s.charAt(i) ), 6);
      }
    };

    var getCode = function(c) {

      if ('0' <= c && c <= '9') {
        return c.charCodeAt(0) - '0'.charCodeAt(0);
      } else if ('A' <= c && c <= 'Z') {
        return c.charCodeAt(0) - 'A'.charCodeAt(0) + 10;
      } else {
        switch (c) {
        case ' ' : return 36;
        case '$' : return 37;
        case '%' : return 38;
        case '*' : return 39;
        case '+' : return 40;
        case '-' : return 41;
        case '.' : return 42;
        case '/' : return 43;
        case ':' : return 44;
        default :
          throw 'illegal char :' + c;
        }
      }
    };

    return _this;
  };

  //---------------------------------------------------------------------
  // qr8BitByte
  //---------------------------------------------------------------------

  var qr8BitByte = function(data) {

    var _mode = QRMode.MODE_8BIT_BYTE;
    var _data = data;
    var _bytes = qrcode.stringToBytes(data);

    var _this = {};

    _this.getMode = function() {
      return _mode;
    };

    _this.getLength = function(buffer) {
      return _bytes.length;
    };

    _this.write = function(buffer) {
      for (var i = 0; i < _bytes.length; i += 1) {
        buffer.put(_bytes[i], 8);
      }
    };

    return _this;
  };

  //---------------------------------------------------------------------
  // qrKanji
  //---------------------------------------------------------------------

  var qrKanji = function(data) {

    var _mode = QRMode.MODE_KANJI;
    var _data = data;

    var stringToBytes = qrcode.stringToBytesFuncs['SJIS'];
    if (!stringToBytes) {
      throw 'sjis not supported.';
    }
    !function(c, code) {
      // self test for sjis support.
      var test = stringToBytes(c);
      if (test.length != 2 || ( (test[0] << 8) | test[1]) != code) {
        throw 'sjis not supported.';
      }
    }('\u53cb', 0x9746);

    var _bytes = stringToBytes(data);

    var _this = {};

    _this.getMode = function() {
      return _mode;
    };

    _this.getLength = function(buffer) {
      return ~~(_bytes.length / 2);
    };

    _this.write = function(buffer) {

      var data = _bytes;

      var i = 0;

      while (i + 1 < data.length) {

        var c = ( (0xff & data[i]) << 8) | (0xff & data[i + 1]);

        if (0x8140 <= c && c <= 0x9FFC) {
          c -= 0x8140;
        } else if (0xE040 <= c && c <= 0xEBBF) {
          c -= 0xC140;
        } else {
          throw 'illegal char at ' + (i + 1) + '/' + c;
        }

        c = ( (c >>> 8) & 0xff) * 0xC0 + (c & 0xff);

        buffer.put(c, 13);

        i += 2;
      }

      if (i < data.length) {
        throw 'illegal char at ' + (i + 1);
      }
    };

    return _this;
  };

  //=====================================================================
  // GIF Support etc.
  //

  //---------------------------------------------------------------------
  // byteArrayOutputStream
  //---------------------------------------------------------------------

  var byteArrayOutputStream = function() {

    var _bytes = [];

    var _this = {};

    _this.writeByte = function(b) {
      _bytes.push(b & 0xff);
    };

    _this.writeShort = function(i) {
      _this.writeByte(i);
      _this.writeByte(i >>> 8);
    };

    _this.writeBytes = function(b, off, len) {
      off = off || 0;
      len = len || b.length;
      for (var i = 0; i < len; i += 1) {
        _this.writeByte(b[i + off]);
      }
    };

    _this.writeString = function(s) {
      for (var i = 0; i < s.length; i += 1) {
        _this.writeByte(s.charCodeAt(i) );
      }
    };

    _this.toByteArray = function() {
      return _bytes;
    };

    _this.toString = function() {
      var s = '';
      s += '[';
      for (var i = 0; i < _bytes.length; i += 1) {
        if (i > 0) {
          s += ',';
        }
        s += _bytes[i];
      }
      s += ']';
      return s;
    };

    return _this;
  };

  //---------------------------------------------------------------------
  // base64EncodeOutputStream
  //---------------------------------------------------------------------

  var base64EncodeOutputStream = function() {

    var _buffer = 0;
    var _buflen = 0;
    var _length = 0;
    var _base64 = '';

    var _this = {};

    var writeEncoded = function(b) {
      _base64 += String.fromCharCode(encode(b & 0x3f) );
    };

    var encode = function(n) {
      if (n < 0) {
        // error.
      } else if (n < 26) {
        return 0x41 + n;
      } else if (n < 52) {
        return 0x61 + (n - 26);
      } else if (n < 62) {
        return 0x30 + (n - 52);
      } else if (n == 62) {
        return 0x2b;
      } else if (n == 63) {
        return 0x2f;
      }
      throw 'n:' + n;
    };

    _this.writeByte = function(n) {

      _buffer = (_buffer << 8) | (n & 0xff);
      _buflen += 8;
      _length += 1;

      while (_buflen >= 6) {
        writeEncoded(_buffer >>> (_buflen - 6) );
        _buflen -= 6;
      }
    };

    _this.flush = function() {

      if (_buflen > 0) {
        writeEncoded(_buffer << (6 - _buflen) );
        _buffer = 0;
        _buflen = 0;
      }

      if (_length % 3 != 0) {
        // padding
        var padlen = 3 - _length % 3;
        for (var i = 0; i < padlen; i += 1) {
          _base64 += '=';
        }
      }
    };

    _this.toString = function() {
      return _base64;
    };

    return _this;
  };

  //---------------------------------------------------------------------
  // base64DecodeInputStream
  //---------------------------------------------------------------------

  var base64DecodeInputStream = function(str) {

    var _str = str;
    var _pos = 0;
    var _buffer = 0;
    var _buflen = 0;

    var _this = {};

    _this.read = function() {

      while (_buflen < 8) {

        if (_pos >= _str.length) {
          if (_buflen == 0) {
            return -1;
          }
          throw 'unexpected end of file./' + _buflen;
        }

        var c = _str.charAt(_pos);
        _pos += 1;

        if (c == '=') {
          _buflen = 0;
          return -1;
        } else if (c.match(/^\s$/) ) {
          // ignore if whitespace.
          continue;
        }

        _buffer = (_buffer << 6) | decode(c.charCodeAt(0) );
        _buflen += 6;
      }

      var n = (_buffer >>> (_buflen - 8) ) & 0xff;
      _buflen -= 8;
      return n;
    };

    var decode = function(c) {
      if (0x41 <= c && c <= 0x5a) {
        return c - 0x41;
      } else if (0x61 <= c && c <= 0x7a) {
        return c - 0x61 + 26;
      } else if (0x30 <= c && c <= 0x39) {
        return c - 0x30 + 52;
      } else if (c == 0x2b) {
        return 62;
      } else if (c == 0x2f) {
        return 63;
      } else {
        throw 'c:' + c;
      }
    };

    return _this;
  };

  //---------------------------------------------------------------------
  // gifImage (B/W)
  //---------------------------------------------------------------------

  var gifImage = function(width, height) {

    var _width = width;
    var _height = height;
    var _data = new Array(width * height);

    var _this = {};

    _this.setPixel = function(x, y, pixel) {
      _data[y * _width + x] = pixel;
    };

    _this.write = function(out) {

      //---------------------------------
      // GIF Signature

      out.writeString('GIF87a');

      //---------------------------------
      // Screen Descriptor

      out.writeShort(_width);
      out.writeShort(_height);

      out.writeByte(0x80); // 2bit
      out.writeByte(0);
      out.writeByte(0);

      //---------------------------------
      // Global Color Map

      // black
      out.writeByte(0x00);
      out.writeByte(0x00);
      out.writeByte(0x00);

      // white
      out.writeByte(0xff);
      out.writeByte(0xff);
      out.writeByte(0xff);

      //---------------------------------
      // Image Descriptor

      out.writeString(',');
      out.writeShort(0);
      out.writeShort(0);
      out.writeShort(_width);
      out.writeShort(_height);
      out.writeByte(0);

      //---------------------------------
      // Local Color Map

      //---------------------------------
      // Raster Data

      var lzwMinCodeSize = 2;
      var raster = getLZWRaster(lzwMinCodeSize);

      out.writeByte(lzwMinCodeSize);

      var offset = 0;

      while (raster.length - offset > 255) {
        out.writeByte(255);
        out.writeBytes(raster, offset, 255);
        offset += 255;
      }

      out.writeByte(raster.length - offset);
      out.writeBytes(raster, offset, raster.length - offset);
      out.writeByte(0x00);

      //---------------------------------
      // GIF Terminator
      out.writeString(';');
    };

    var bitOutputStream = function(out) {

      var _out = out;
      var _bitLength = 0;
      var _bitBuffer = 0;

      var _this = {};

      _this.write = function(data, length) {

        if ( (data >>> length) != 0) {
          throw 'length over';
        }

        while (_bitLength + length >= 8) {
          _out.writeByte(0xff & ( (data << _bitLength) | _bitBuffer) );
          length -= (8 - _bitLength);
          data >>>= (8 - _bitLength);
          _bitBuffer = 0;
          _bitLength = 0;
        }

        _bitBuffer = (data << _bitLength) | _bitBuffer;
        _bitLength = _bitLength + length;
      };

      _this.flush = function() {
        if (_bitLength > 0) {
          _out.writeByte(_bitBuffer);
        }
      };

      return _this;
    };

    var getLZWRaster = function(lzwMinCodeSize) {

      var clearCode = 1 << lzwMinCodeSize;
      var endCode = (1 << lzwMinCodeSize) + 1;
      var bitLength = lzwMinCodeSize + 1;

      // Setup LZWTable
      var table = lzwTable();

      for (var i = 0; i < clearCode; i += 1) {
        table.add(String.fromCharCode(i) );
      }
      table.add(String.fromCharCode(clearCode) );
      table.add(String.fromCharCode(endCode) );

      var byteOut = byteArrayOutputStream();
      var bitOut = bitOutputStream(byteOut);

      // clear code
      bitOut.write(clearCode, bitLength);

      var dataIndex = 0;

      var s = String.fromCharCode(_data[dataIndex]);
      dataIndex += 1;

      while (dataIndex < _data.length) {

        var c = String.fromCharCode(_data[dataIndex]);
        dataIndex += 1;

        if (table.contains(s + c) ) {

          s = s + c;

        } else {

          bitOut.write(table.indexOf(s), bitLength);

          if (table.size() < 0xfff) {

            if (table.size() == (1 << bitLength) ) {
              bitLength += 1;
            }

            table.add(s + c);
          }

          s = c;
        }
      }

      bitOut.write(table.indexOf(s), bitLength);

      // end code
      bitOut.write(endCode, bitLength);

      bitOut.flush();

      return byteOut.toByteArray();
    };

    var lzwTable = function() {

      var _map = {};
      var _size = 0;

      var _this = {};

      _this.add = function(key) {
        if (_this.contains(key) ) {
          throw 'dup key:' + key;
        }
        _map[key] = _size;
        _size += 1;
      };

      _this.size = function() {
        return _size;
      };

      _this.indexOf = function(key) {
        return _map[key];
      };

      _this.contains = function(key) {
        return typeof _map[key] != 'undefined';
      };

      return _this;
    };

    return _this;
  };

  var createDataURL = function(width, height, getPixel) {
    var gif = gifImage(width, height);
    for (var y = 0; y < height; y += 1) {
      for (var x = 0; x < width; x += 1) {
        gif.setPixel(x, y, getPixel(x, y) );
      }
    }

    var b = byteArrayOutputStream();
    gif.write(b);

    var base64 = base64EncodeOutputStream();
    var bytes = b.toByteArray();
    for (var i = 0; i < bytes.length; i += 1) {
      base64.writeByte(bytes[i]);
    }
    base64.flush();

    return 'data:image/gif;base64,' + base64;
  };

  //---------------------------------------------------------------------
  // returns qrcode function.

  return qrcode;
}();

// multibyte support
!function() {

  qrcode.stringToBytesFuncs['UTF-8'] = function(s) {
    // http://stackoverflow.com/questions/18729405/how-to-convert-utf8-string-to-byte-array
    function toUTF8Array(str) {
      var utf8 = [];
      for (var i=0; i < str.length; i++) {
        var charcode = str.charCodeAt(i);
        if (charcode < 0x80) utf8.push(charcode);
        else if (charcode < 0x800) {
          utf8.push(0xc0 | (charcode >> 6),
              0x80 | (charcode & 0x3f));
        }
        else if (charcode < 0xd800 || charcode >= 0xe000) {
          utf8.push(0xe0 | (charcode >> 12),
              0x80 | ((charcode>>6) & 0x3f),
              0x80 | (charcode & 0x3f));
        }
        // surrogate pair
        else {
          i++;
          // UTF-16 encodes 0x10000-0x10FFFF by
          // subtracting 0x10000 and splitting the
          // 20 bits of 0x0-0xFFFFF into two halves
          charcode = 0x10000 + (((charcode & 0x3ff)<<10)
            | (str.charCodeAt(i) & 0x3ff));
          utf8.push(0xf0 | (charcode >>18),
              0x80 | ((charcode>>12) & 0x3f),
              0x80 | ((charcode>>6) & 0x3f),
              0x80 | (charcode & 0x3f));
        }
      }
      return utf8;
    }
    return toUTF8Array(s);
  };

}();

(function (factory) {
  if (typeof define === 'function' && define.amd) {
      define([], factory);
  } else if (typeof exports === 'object') {
      module.exports = factory();
  }
}(function () {
    return qrcode;
}));




(function(global) {
    global.generateQrSvg = function(text, options) {
        options = options || {};
        var margin = options.margin !== undefined ? options.margin : 4;
        var fg = options.fg || '#000000';
        var bg = options.bg || '#ffffff';
        var ecLevel = options.ecLevel || 'M';

        try {
            var qr = qrcode(0, ecLevel);
            qr.addData(text, 'Byte');
            qr.make();
            var count = qr.getModuleCount();
            var total = count + margin * 2;

            var path = '';
            for (var r = 0; r < count; r++) {
                for (var c = 0; c < count; c++) {
                    if (qr.isDark(r, c)) {
                        path += 'M' + (c + margin) + ',' + (r + margin) + 'h1v1h-1z ';
                    }
                }
            }

            return '<svg viewBox="0 0 ' + total + ' ' + total + '" class="qr-svg-matrix" xmlns="http://www.w3.org/2000/svg" style="width:100%;height:100%;border-radius:12px;background:' + bg + ';">' +
                '<rect width="' + total + '" height="' + total + '" fill="' + bg + '"/>' +
                '<path d="' + path + '" fill="' + fg + '"/>' +
                '</svg>';
        } catch (e) {
            console.error('QR generation failed:', e);
            return '<div class="qr-error">QR Generation Error</div>';
        }
    };
})(typeof window !== 'undefined' ? window : this);
)raw_asset";

} // namespace assets
} // namespace aimon

#endif // AIMON_WEB_ASSETS_HXX
