/*
 * verify_meshmon_e2e.mjs
 * Comprehensive visual interactive test suite for MeshMon.
 * Copyright (C) 2026, Charles Chiou
 */

import { spawn } from 'child_process';
import http from 'http';
import fs from 'fs';
import path from 'path';

const CHROME_PATH = '/home/samurai/.cache/ms-playwright/chromium-1200/chrome-linux64/chrome';
const DEBUG_PORT = 9338;
const ARTIFACTS_DIR = '/home/samurai/.gemini/antigravity-ide/brain/ce561280-f1ed-43bf-a5da-d664a65a4be1';

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function run() {
    console.log('=== Starting MeshMon Interactive QA Suite ===');

    const chrome = spawn(CHROME_PATH, [
        '--headless=new',
        '--no-sandbox',
        '--disable-gpu',
        `--remote-debugging-port=${DEBUG_PORT}`,
        '--window-size=1920,1080',
        'about:blank'
    ]);

    await sleep(2000);

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

    console.log('Navigating to MeshMon (http://192.168.8.245:16880/)...');
    await cdp('Page.navigate', { url: 'http://192.168.8.245:16880/' });
    await sleep(2500);

    // 1. Initial Overview & Navigation Tabs Verification
    const tabsList = await evalJs(`(() => {
        return Array.from(document.querySelectorAll('.tab-btn')).map(b => ({
            id: b.dataset.tab,
            text: b.innerText.trim().replace(/\\n/g, ' '),
            isActive: b.classList.contains('active')
        }));
    })()`);
    console.log('MeshMon Tabs List:', tabsList);
    await takeScreenshot('qa_meshmon_01_insights_tab.png');

    // 2. Click through tabs
    const targetTabs = ['sniffer', 'radar', 'topology', 'fleet', 'chat'];
    for (const tabId of targetTabs) {
        console.log(`Clicking MeshMon tab [${tabId}]...`);
        await evalJs(`document.querySelector('.tab-btn[data-tab="${tabId}"]')?.click()`);
        await sleep(1000);
        const activeTabId = await evalJs(`document.querySelector('.tab-btn.active')?.dataset?.tab`);
        console.log(`  Active tab is now: ${activeTabId}`);
        await takeScreenshot(`qa_meshmon_02_tab_${tabId}.png`);
    }

    // 3. Return to Insights Tab
    await evalJs(`document.querySelector('.tab-btn[data-tab="insights"]')?.click()`);
    await sleep(800);

    // 4. Test Mobile Touch-Scrolling Navigation (375x812)
    console.log('Testing Mobile Viewport (375x812) and touch scrolling...');
    await cdp('Emulation.setDeviceMetricsOverride', {
        width: 375,
        height: 812,
        deviceScaleFactor: 2,
        mobile: true
    });
    await sleep(1000);

    const mobileNavState = await evalJs(`(() => {
        const nav = document.querySelector('.tabs-nav');
        return {
            scrollWidth: nav?.scrollWidth,
            clientWidth: nav?.clientWidth,
            isScrollable: (nav?.scrollWidth || 0) > (nav?.clientWidth || 0)
        };
    })()`);
    console.log('MeshMon Mobile Nav Scroll State:', mobileNavState);
    await takeScreenshot('qa_meshmon_03_mobile_insights.png');

    // Simulate touch scroll right
    await evalJs(`(() => {
        const nav = document.querySelector('.tabs-nav');
        if (nav) nav.scrollLeft = 200;
    })()`);
    await sleep(500);
    await takeScreenshot('qa_meshmon_04_mobile_scrolled.png');

    ws.close();
    chrome.kill();
    console.log('=== MeshMon Interactive QA Complete! ===');
    process.exit(0);
}

run().catch(err => {
    console.error('MeshMon QA Failed:', err);
    process.exit(1);
});
