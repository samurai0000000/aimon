/*
 * verify_satellites_direct.mjs
 * Direct automated browser validation for NetMon, MeshMon, and EmbDev.
 * Copyright (C) 2026, Charles Chiou
 */

import { spawn } from 'child_process';
import http from 'http';
import fs from 'fs';
import path from 'path';

const CHROME_PATH = '/home/samurai/.cache/ms-playwright/chromium-1200/chrome-linux64/chrome';
const DEBUG_PORT = 9336;
const ARTIFACTS_DIR = '/home/samurai/.gemini/antigravity-ide/brain/ce561280-f1ed-43bf-a5da-d664a65a4be1';

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function run() {
    console.log('=== Starting Direct Satellite Verification Suite ===');

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

    // 1. NetMon Verification
    console.log('\nTesting NetMon (http://192.168.8.30:3884/)...');
    await cdp('Page.navigate', { url: 'http://192.168.8.30:3884/' });
    await sleep(2500);

    const netmonState = await evalJs(`(() => {
        const ppp11 = document.getElementById('canvas-ppp11');
        const ppp12 = document.getElementById('canvas-ppp12');
        const rows = document.querySelectorAll('#devices-tbody tr');
        const pageInfo = document.getElementById('devices-page-info');
        const pageSize = document.getElementById('page-size-select');
        return {
            hasCanvas11: !!ppp11,
            hasCanvas12: !!ppp12,
            displayedRows: rows.length,
            pageInfoText: pageInfo?.innerText,
            pageSizeVal: pageSize?.value
        };
    })()`);
    console.log('NetMon state:', netmonState);
    await takeScreenshot('e2e_sat_01_netmon_overview.png');

    // Test pagination: click next
    console.log('Clicking NetMon next page button (#btn-page-next)...');
    await evalJs(`document.getElementById('btn-page-next')?.click()`);
    await sleep(500);
    const page2Info = await evalJs(`document.getElementById('devices-page-info')?.innerText`);
    console.log('NetMon page 2 info:', page2Info);
    await takeScreenshot('e2e_sat_02_netmon_page2.png');

    // 2. MeshMon Verification
    console.log('\nTesting MeshMon (http://192.168.8.245:16880/)...');
    await cdp('Page.navigate', { url: 'http://192.168.8.245:16880/' });
    await sleep(2500);

    const meshmonState = await evalJs(`(() => {
        const tabs = Array.from(document.querySelectorAll('.tab-btn')).map(b => b.innerText.trim().replace(/\\n/g, ' '));
        const activeTab = document.querySelector('.tab-btn.active')?.innerText?.trim()?.replace(/\\n/g, ' ');
        return {
            tabsCount: tabs.length,
            tabNames: tabs,
            activeTab: activeTab
        };
    })()`);
    console.log('MeshMon tabs state:', meshmonState);
    await takeScreenshot('e2e_sat_03_meshmon_overview.png');

    // Test mobile viewport on MeshMon
    await cdp('Emulation.setDeviceMetricsOverride', {
        width: 375,
        height: 812,
        deviceScaleFactor: 2,
        mobile: true
    });
    await sleep(1000);
    await takeScreenshot('e2e_sat_04_meshmon_mobile.png');

    // 3. EmbDev Verification
    console.log('\nTesting EmbDev (http://192.168.8.30:3886/)...');
    await cdp('Emulation.setDeviceMetricsOverride', {
        width: 1920,
        height: 1080,
        deviceScaleFactor: 1,
        mobile: false
    });
    await cdp('Page.navigate', { url: 'http://192.168.8.30:3886/' });
    await sleep(2500);

    const embdevState = await evalJs(`(() => {
        const terminal = document.getElementById('terminalScreen');
        const powerOnBtn = document.querySelector('.btn-power-on');
        const powerOffBtn = document.querySelector('.btn-power-off');
        return {
            hasTerminal: !!terminal,
            hasPowerOn: !!powerOnBtn,
            hasPowerOff: !!powerOffBtn
        };
    })()`);
    console.log('EmbDev state:', embdevState);
    await takeScreenshot('e2e_sat_05_embdev_overview.png');

    ws.close();
    chrome.kill();
    console.log('\nSatellite verification complete!');
    process.exit(0);
}

run().catch(err => {
    console.error('Satellite Test Failed:', err);
    process.exit(1);
});
