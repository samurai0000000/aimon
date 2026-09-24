//
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
                fg: '#000000',
                bg: '#ffffff',
                margin: 4
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

