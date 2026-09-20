//
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
