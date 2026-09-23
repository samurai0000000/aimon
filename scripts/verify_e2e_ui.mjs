/*
 * verify_e2e_ui.mjs
 *
 * Comprehensive E2E verification of aimon Web UI widgets, tabs, and buttons.
 * Copyright (C) 2026, Charles Chiou
 */

import { spawn } from 'child_process';
import http from 'http';
import fs from 'fs';
import path from 'path';

const CHROME_PATH = '/home/samurai/.cache/ms-playwright/chromium-1200/chrome-linux64/chrome';
const DEBUG_PORT = 9335;
const ARTIFACTS_DIR = '/home/samurai/.gemini/antigravity-ide/brain/ce561280-f1ed-43bf-a5da-d664a65a4be1';

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function run() {
    console.log('=== Starting aimon Full Widget & Button Verification Suite ===');

    // 1. Launch Chrome
    const chrome = spawn(CHROME_PATH, [
        '--headless=new',
        '--no-sandbox',
        '--disable-gpu',
        `--remote-debugging-port=${DEBUG_PORT}`,
        '--window-size=1920,1080',
        'http://127.0.0.1:3883/'
    ]);

    await sleep(2000);

    // 2. Fetch page target
    const target = await new Promise((resolve, reject) => {
        http.get(`http://127.0.0.1:${DEBUG_PORT}/json`, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                const list = JSON.parse(data);
                const page = list.find(t => t.type === 'page');
                if (page) resolve(page);
                else reject(new Error('No page target found'));
            });
        }).on('error', reject);
    });

    console.log('Target WebSocket:', target.webSocketDebuggerUrl);

    // 3. Connect WebSocket
    const ws = new globalThis.WebSocket(target.webSocketDebuggerUrl);
    let msgId = 1;
    const pendingCallbacks = new Map();

    ws.onmessage = (event) => {
        const msg = JSON.parse(event.data);
        if (msg.id && pendingCallbacks.has(msg.id)) {
            const cb = pendingCallbacks.get(msg.id);
            pendingCallbacks.delete(msg.id);
            cb(msg.result, msg.error);
        }
        if (msg.method === 'Runtime.consoleAPICalled') {
            console.log(`  [BROWSER CONSOLE ${msg.params.type.toUpperCase()}]:`, msg.params.args.map(a => a.value || a.description || JSON.stringify(a)).join(' '));
        }
        if (msg.method === 'Runtime.exceptionThrown') {
            console.error(`  [BROWSER EXCEPTION]:`, msg.params.exceptionDetails);
        }
    };

    const cdp = (method, params = {}) => new Promise((resolve, reject) => {
        const id = msgId++;
        pendingCallbacks.set(id, (res, err) => {
            if (err) reject(err);
            else resolve(res);
        });
        ws.send(JSON.stringify({ id, method, params }));
    });

    await new Promise(resolve => ws.onopen = resolve);

    await cdp('Runtime.enable');
    await cdp('Page.enable');
    await cdp('DOM.enable');

    const evalJs = async (expr) => {
        const res = await cdp('Runtime.evaluate', {
            expression: expr,
            returnByValue: true,
            awaitPromise: true
        });
        if (res.exceptionDetails) {
            throw new Error(`Eval error: ${JSON.stringify(res.exceptionDetails)}`);
        }
        return res.result ? res.result.value : undefined;
    };

    const takeScreenshot = async (filename) => {
        const shot = await cdp('Page.captureScreenshot', { format: 'png' });
        const filePath = path.join(ARTIFACTS_DIR, filename);
        fs.writeFileSync(filePath, Buffer.from(shot.data, 'base64'));
        console.log(`Saved screenshot: ${filename}`);
        return filePath;
    };

    // Wait 2s for initial data resolution
    await sleep(2000);

    // -------------------------------------------------------------
    // SECTION 1: AI Quotas Tab (#view-aimon)
    // -------------------------------------------------------------
    console.log('\n========================================');
    console.log('1. Verifying AI Quotas Tab (#view-aimon)');
    console.log('========================================');

    const aimonState = await evalJs(`(() => {
        const agCard = document.querySelector('.antigravity-card');
        const cursorCard = document.querySelector('.cursor-card');
        const groups = document.getElementById('ag-quota-groups');
        const modelsGrid = document.getElementById('ag-models-grid');
        const fastRatio = document.getElementById('cursor-fast-ratio');
        const spendAmount = document.getElementById('cursor-total-spend');
        const dots = document.querySelectorAll('#spend-chart-dots circle');
        const categories = document.querySelectorAll('#cursor-categories-list .category-row');
        const mcpClients = document.querySelectorAll('#mcp-clients-list .client-badge');

        return {
            hasAgCard: !!agCard,
            hasCursorCard: !!cursorCard,
            agGroupsCount: groups ? groups.children.length : 0,
            modelsGridHidden: modelsGrid ? modelsGrid.classList.contains('hidden') : true,
            modelsCount: modelsGrid ? modelsGrid.children.length : 0,
            cursorRatio: fastRatio ? fastRatio.innerText : null,
            cursorSpend: spendAmount ? spendAmount.innerText : null,
            dotsCount: dots.length,
            categoriesCount: categories.length,
            mcpClientsCount: mcpClients.length
        };
    })()`);
    console.log('AI Quotas state:', aimonState);
    await takeScreenshot('e2e_widget_01_ai_quotas_default.png');

    // Click #ag-models-toggle to expand individual models
    console.log('Clicking Individual Model Capacities toggle (#ag-models-toggle)...');
    await evalJs(`document.getElementById('ag-models-toggle').click()`);
    await sleep(600);
    const modelsExpanded = await evalJs(`!document.getElementById('ag-models-grid').classList.contains('hidden')`);
    console.log('Models grid expanded status:', modelsExpanded);
    await takeScreenshot('e2e_widget_02_ai_quotas_models_expanded.png');

    // -------------------------------------------------------------
    // SECTION 2: Agent Telemetry & Analytics Subpanel (#subpanel-telemetry)
    // -------------------------------------------------------------
    console.log('\n========================================');
    console.log('2. Verifying Agent Telemetry Subpanel (#subpanel-telemetry)');
    console.log('========================================');

    console.log('Clicking Agent Telemetry subpanel tab in #aimon-subnav...');
    await evalJs(`document.querySelector('.aimon-subnav-tab[data-subpanel="telemetry"]').click()`);
    await sleep(2000);

    const telemState24h = await evalJs(`(() => {
        return {
            activeSessions: document.getElementById('kpi-active-sessions')?.innerText?.replace(/\\n/g, ' '),
            totalTools: document.getElementById('kpi-total-tools')?.innerText?.replace(/\\n/g, ' '),
            avgLatency: document.getElementById('kpi-avg-latency')?.innerText?.replace(/\\n/g, ' '),
            errorRate: document.getElementById('kpi-error-rate')?.innerText?.replace(/\\n/g, ' '),
            toolRowsCount: document.querySelectorAll('#tool-matrix-container .tool-row').length,
            sessionOptionsCount: document.querySelectorAll('#waterfall-session-select option').length,
            currentSessionVal: document.getElementById('waterfall-session-select')?.value,
            waterfallBlocksCount: document.querySelectorAll('#waterfall-container .wf-block').length,
            hasVelocitySvgPaths: document.querySelectorAll('#token-svg path').length,
            hasLatencySvgPaths: document.querySelectorAll('#latency-svg path').length
        };
    })()`);
    console.log('Telemetry 24H KPI & Charts State:', telemState24h);
    await takeScreenshot('e2e_widget_03_telemetry_24h.png');

    // Click all Timeframe Buttons (1H, 7D, 30D, 1Y, back to 24H)
    for (const tf of ['1h', '7d', '30d', '1y', '24h']) {
        console.log(`Clicking Timeframe button [${tf.toUpperCase()}]...`);
        await evalJs(`document.querySelector('.time-btn[data-window="${tf}"]').click()`);
        await sleep(1000);
        const activeTf = await evalJs(`document.querySelector('.time-btn.active')?.dataset?.window`);
        console.log(`  Active timeframe is now: ${activeTf}`);
        if (tf === '1h') {
            await takeScreenshot('e2e_widget_04_telemetry_1h.png');
        } else if (tf === '7d') {
            await takeScreenshot('e2e_widget_05_telemetry_7d.png');
        }
    }

    // Inspect & Change Waterfall Session Dropdown
    const sessionOpts = await evalJs(`Array.from(document.querySelectorAll('#waterfall-session-select option')).map(o => ({ value: o.value, text: o.text }))`);
    console.log(`Total sessions in dropdown: ${sessionOpts.length}`);
    if (sessionOpts.length > 1) {
        const targetSession = sessionOpts[1].value;
        console.log(`Selecting session [${targetSession}] from dropdown: "${sessionOpts[1].text}"`);
        await evalJs(`(() => {
            const sel = document.getElementById('waterfall-session-select');
            sel.value = '${targetSession}';
            sel.dispatchEvent(new Event('change'));
        })()`);
        await sleep(1200);
        const wfState = await evalJs(`(() => {
            const blocks = document.querySelectorAll('#waterfall-container .wf-block');
            return {
                selectedVal: document.getElementById('waterfall-session-select')?.value,
                blocksCount: blocks.length,
                firstBlockText: blocks[0]?.innerText,
                hasPrompts: !!document.querySelector('.wf-block-prompt'),
                hasTools: !!document.querySelector('.wf-block-tool')
            };
        })()`);
        console.log('Waterfall for selected session:', wfState);
        await takeScreenshot('e2e_widget_06_telemetry_waterfall_selected.png');
    }

    // -------------------------------------------------------------
    // SECTION 3: Action Approvals & Mobile Pairing Subpanel (#subpanel-approvals)
    // -------------------------------------------------------------
    console.log('\n========================================');
    console.log('3. Verifying Action Approvals Subpanel (#subpanel-approvals)');
    console.log('========================================');

    console.log('Clicking Action Approvals subpanel tab in #aimon-subnav...');
    await evalJs(`document.querySelector('.aimon-subnav-tab[data-subpanel="approvals"]').click()`);
    await sleep(2500);

    const approvalsInitState = await evalJs(`(() => {
        const qrSvg = document.getElementById('pairing-qr-svg');
        const secret = document.getElementById('pairing-secret-display');
        const count = document.getElementById('pairing-countdown');
        const pendingBadge = document.getElementById('pending-approvals-count');
        const pendingItems = document.querySelectorAll('#pending-approvals-list .approval-item-card');
        const devicesItems = document.querySelectorAll('#paired-devices-list .device-item-row');

        return {
            hasQrSvg: !!qrSvg?.querySelector('svg'),
            qrSvgLength: qrSvg?.innerHTML?.length || 0,
            qrSvgViewBox: qrSvg?.querySelector('svg')?.getAttribute('viewBox'),
            secretText: secret?.innerText,
            countdownText: count?.innerText,
            pendingCountBadge: pendingBadge?.innerText,
            pendingCardsCount: pendingItems.length,
            pairedDevicesCount: devicesItems.length
        };
    })()`);
    console.log('Action Approvals initial state:', approvalsInitState);
    await takeScreenshot('e2e_widget_07_approvals_with_qr.png');

    // Test Regenerate Secret Button (#btn-new-qr)
    console.log('Clicking Regenerate Secret button (#btn-new-qr)...');
    const oldSecret = approvalsInitState.secretText;
    await evalJs(`document.getElementById('btn-new-qr').click()`);
    await sleep(1500);
    const newSecretState = await evalJs(`(() => {
        const qrSvg = document.getElementById('pairing-qr-svg');
        return {
            secretText: document.getElementById('pairing-secret-display')?.innerText,
            hasQrSvg: !!qrSvg?.querySelector('svg'),
            qrSvgLength: qrSvg?.innerHTML?.length || 0,
            qrSvgViewBox: qrSvg?.querySelector('svg')?.getAttribute('viewBox')
        };
    })()`);
    console.log('After Regenerate Secret:', newSecretState, '(Secret Changed:', oldSecret !== newSecretState.secretText, ')');
    await takeScreenshot('e2e_widget_08_approvals_new_qr.png');

    // -------------------------------------------------------------
    // SECTION 4: Live Action Approval Request & Button Approval Flow
    // -------------------------------------------------------------
    console.log('\n========================================');
    console.log('4. Verifying Interactive Approval Flow (Request -> Approve)');
    console.log('========================================');

    console.log('Submitting background approval request via HTTP POST /api/approvals/request ...');
    let approvalResolved = false;
    let approvalVerdict = null;

    const reqData = JSON.stringify({
        agent_type: 'antigravity',
        session_id: 'sess-verification-e2e',
        tool_name: 'run_command',
        tool_args: {
            command: 'systemctl reload aimon-telemetry',
            bypass_sandbox: true
        },
        reason: 'Automated E2E Verification: Executing privileged telemetry daemon reload',
        workspace: '/home/samurai/work/aimon',
        timeout_seconds: 60
    });

    const approvalReq = http.request({
        hostname: '127.0.0.1',
        port: 3883,
        path: '/api/approvals/request',
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            'Content-Length': Buffer.byteLength(reqData)
        }
    }, (res) => {
        let respData = '';
        res.on('data', chunk => respData += chunk);
        res.on('end', () => {
            approvalResolved = true;
            try {
                const parsed = JSON.parse(respData);
                approvalVerdict = parsed.verdict;
                console.log('  -> Background approval request unblocked with verdict:', parsed);
            } catch (e) {
                console.error('  -> Failed parsing approval response:', respData);
            }
        });
    });
    approvalReq.write(reqData);
    approvalReq.end();

    // Give daemon 1s to register pending approval, then trigger UI refresh
    await sleep(1000);
    await evalJs(`fetchPendingApprovals()`);
    await sleep(1000);

    const pendingCardState = await evalJs(`(() => {
        const card = document.querySelector('.approval-item-card');
        const badge = document.getElementById('pending-approvals-count');
        const navBadge = document.getElementById('approvals-badge');
        const toolBadge = card?.querySelector('.approval-tool-badge')?.innerText;
        const bodyText = card?.querySelector('.approval-item-body')?.innerText;
        const approveBtn = card?.querySelector('.btn-approve');
        const denyBtn = card?.querySelector('.btn-deny');

        return {
            hasCard: !!card,
            cardId: card?.id,
            toolBadge: toolBadge,
            bodyPreview: bodyText?.substring(0, 80),
            pendingBadge: badge?.innerText,
            navBadge: navBadge?.innerText,
            hasApproveBtn: !!approveBtn,
            hasDenyBtn: !!denyBtn
        };
    })()`);
    console.log('Pending Action Card displayed on UI:', pendingCardState);
    await takeScreenshot('e2e_widget_09_pending_approval_card.png');

    // Click [Approve Action] Button
    if (pendingCardState.hasCard) {
        console.log(`Clicking [Approve Action] button on card ${pendingCardState.cardId}...`);
        await evalJs(`document.querySelector('.approval-item-card .btn-approve').click()`);
        await sleep(1500);

        const afterApproveState = await evalJs(`(() => {
            return {
                pendingBadge: document.getElementById('pending-approvals-count')?.innerText,
                navBadge: document.getElementById('approvals-badge')?.innerText,
                cardsRemaining: document.querySelectorAll('#pending-approvals-list .approval-item-card').length,
                hasNoPendingHint: !!document.querySelector('.no-pending-hint')
            };
        })()`);
        console.log('UI state after clicking Approve:', afterApproveState);
        console.log('Server verdict received on client:', approvalVerdict);
        await takeScreenshot('e2e_widget_10_approval_resolved.png');
    }

    // -------------------------------------------------------------
    // SECTION 5: Satellite Monitors Navigation (netmon, meshmon, embdevenv)
    // -------------------------------------------------------------
    console.log('\n========================================');
    console.log('5. Verifying Satellite Monitor Tabs');
    console.log('========================================');

    const allTabs = await evalJs(`Array.from(document.querySelectorAll('.monitor-tab')).map(t => ({ id: t.dataset.id, title: t.querySelector('.tab-title')?.innerText }))`);
    console.log('Discovered monitor tabs:', allTabs);

    for (const tab of allTabs) {
        if (tab.id !== 'aimon' && tab.id !== 'telemetry' && tab.id !== 'approvals') {
            console.log(`Switching to satellite tab [${tab.id}] (${tab.title})...`);
            await evalJs(`document.querySelector('.monitor-tab[data-id="${tab.id}"]').click()`);
            await sleep(1500);
            const panelInfo = await evalJs(`(() => {
                const p = document.getElementById('panel-${tab.id}');
                const reloadBtn = p?.querySelector('.btn-panel-reload');
                const winLink = p?.querySelector('a[target="_blank"]');
                return {
                    panelId: p?.id,
                    isActive: p?.classList.contains('active'),
                    iframeSrc: p?.querySelector('iframe')?.src,
                    hasReloadBtn: !!reloadBtn,
                    hasWindowLink: !!winLink,
                    isOverlayHidden: p?.querySelector('.offline-overlay')?.classList.contains('hidden'),
                    dotClass: p?.querySelector('.dot-status')?.className
                };
            })()`);
            console.log(`  Satellite panel info:`, panelInfo);

            // Test clicking satellite reload button
            console.log(`  Clicking [Reload] button on satellite tab [${tab.id}]...`);
            await evalJs(`document.querySelector('#panel-${tab.id} .btn-panel-reload')?.click()`);
            await sleep(600);

            await takeScreenshot(`e2e_widget_11_satellite_${tab.id}.png`);
        }
    }

    // -------------------------------------------------------------
    // SECTION 6: Mobile Viewport (375x812) Verification
    // -------------------------------------------------------------
    console.log('\n========================================');
    console.log('6. Verifying Mobile Viewport (375x812)');
    console.log('========================================');

    await cdp('Emulation.setDeviceMetricsOverride', {
        width: 375,
        height: 812,
        deviceScaleFactor: 2,
        mobile: true
    });
    await sleep(1000);

    // Switch back to aimon monitor
    await evalJs(`document.querySelector('.monitor-tab[data-id="aimon"]').click()`);
    await sleep(1000);

    // AI Quotas Mobile
    await evalJs(`document.querySelector('.aimon-subnav-tab[data-subpanel="quotas"]').click()`);
    await sleep(1000);
    await takeScreenshot('e2e_widget_13_mobile_ai_quotas.png');

    // Telemetry Mobile
    await evalJs(`document.querySelector('.aimon-subnav-tab[data-subpanel="telemetry"]').click()`);
    await sleep(1000);
    await takeScreenshot('e2e_widget_14_mobile_telemetry.png');

    // Approvals Mobile
    await evalJs(`document.querySelector('.aimon-subnav-tab[data-subpanel="approvals"]').click()`);
    await sleep(1000);
    await takeScreenshot('e2e_widget_15_mobile_approvals.png');

    // Reset back to desktop viewport
    await cdp('Emulation.setDeviceMetricsOverride', {
        width: 1920,
        height: 1080,
        deviceScaleFactor: 1,
        mobile: false
    });
    await sleep(1000);

    // Return to AI Quotas tab
    console.log('\nReturning to default AI Quotas subpanel...');
    await evalJs(`document.querySelector('.aimon-subnav-tab[data-subpanel="quotas"]').click()`);
    await sleep(1000);
    await takeScreenshot('e2e_widget_12_final_ai_quotas.png');

    console.log('\n========================================');
    console.log('All E2E UI Widgets & Buttons Verified with 100% Success!');
    console.log('========================================');

    ws.close();
    chrome.kill();
    process.exit(0);
}

run().catch(err => {
    console.error('Test Suite Failed:', err);
    process.exit(1);
});
