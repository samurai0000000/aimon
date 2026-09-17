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
