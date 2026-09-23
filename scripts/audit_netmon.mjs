/*
 * audit_netmon.mjs
 * Deep audit and screenshot capture of netmon web interface.
 * Copyright (C) 2026, Charles Chiou
 */

import { spawn } from 'child_process';
import http from 'http';
import fs from 'fs';

const CHROME_PATH = '/home/samurai/.cache/ms-playwright/chromium-1200/chrome-linux64/chrome';
const DEBUG_PORT = 9340;
const ARTIFACTS_DIR = '/home/samurai/.gemini/antigravity-ide/brain/ce561280-f1ed-43bf-a5da-d664a65a4be1';

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function run() {
    console.log('=== Starting NetMon Deep Visual & Functional Audit ===');

    const chrome = spawn(CHROME_PATH, [
        '--headless=new',
        '--no-sandbox',
        '--disable-gpu',
        `--remote-debugging-port=${DEBUG_PORT}`,
        '--window-size=1920,1080',
        'http://192.168.8.30:3884/'
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
    const pending = new Map();
    const send = (method, params = {}) => new Promise((resolve) => {
        const id = msgId++;
        pending.set(id, resolve);
        ws.send(JSON.stringify({ id, method, params }));
    });

    ws.onmessage = (event) => {
        const msg = JSON.parse(event.data);
        if (msg.id && pending.has(msg.id)) {
            const cb = pending.get(msg.id);
            pending.delete(msg.id);
            cb(msg.result);
        }
        if (msg.method === 'Runtime.consoleAPICalled') {
            console.log('CONSOLE:', msg.params.type, msg.params.args.map(a => a.value || a.description).join(' '));
        }
        if (msg.method === 'Runtime.exceptionThrown') {
            console.log('EXCEPTION:', msg.params.exceptionDetails.text, msg.params.exceptionDetails.exception?.description);
        }
    };

    await new Promise(r => ws.onopen = r);
    await send('Page.enable');
    await send('Runtime.enable');
    await send('Network.enable');

    console.log('Waiting for initial data...');
    await sleep(3000);

    // 1. Desktop Top View
    let shot = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(`${ARTIFACTS_DIR}/netmon_audit_desktop_top.png`, Buffer.from(shot.data, 'base64'));

    // 2. Desktop Scrolled (Traffic & Device Table)
    await send('Runtime.evaluate', { expression: 'window.scrollTo(0, 500)' });
    await sleep(500);
    shot = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(`${ARTIFACTS_DIR}/netmon_audit_desktop_mid.png`, Buffer.from(shot.data, 'base64'));

    await send('Runtime.evaluate', { expression: 'window.scrollTo(0, 1000)' });
    await sleep(500);
    shot = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(`${ARTIFACTS_DIR}/netmon_audit_desktop_table.png`, Buffer.from(shot.data, 'base64'));

    // 3. Test Year Timeframe Button
    console.log('Testing Year timeframe button...');
    await send('Runtime.evaluate', {
        expression: `Array.from(document.querySelectorAll('.time-btn')).find(b => b.textContent.trim() === 'Year')?.click()`
    });
    await sleep(1500);
    await send('Runtime.evaluate', { expression: 'window.scrollTo(0, 0)' });
    await sleep(500);
    shot = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(`${ARTIFACTS_DIR}/netmon_audit_timeframe_year.png`, Buffer.from(shot.data, 'base64'));

    // 4. Test Device Search
    console.log('Testing device search for fox...');
    await send('Runtime.evaluate', {
        expression: `(() => {
            const inp = document.getElementById('device-search');
            if (inp) {
                inp.value = 'fox';
                inp.dispatchEvent(new Event('input'));
            }
        })()`
    });
    await sleep(500);
    const searchRes = await send('Runtime.evaluate', {
        expression: 'JSON.stringify(Array.from(document.querySelectorAll("#devices-tbody tr")).map(r => r.textContent.trim().replace(/\\s+/g, " ")))'
    });
    console.log('Search result:', searchRes.result.value);

    // 5. Test Device Table Sort
    console.log('Testing sort by vendor...');
    await send('Runtime.evaluate', {
        expression: `Array.from(document.querySelectorAll('.data-table th.sortable')).find(th => th.getAttribute('data-sort') === 'vendor')?.click()`
    });
    await sleep(500);
    const sortRes = await send('Runtime.evaluate', {
        expression: 'JSON.stringify(Array.from(document.querySelectorAll("#devices-tbody tr")).slice(0, 5).map(r => r.textContent.trim().replace(/\\s+/g, " ")))'
    });
    console.log('Sort result:', sortRes.result.value);

    // 6. Test Mobile Viewport
    console.log('Testing mobile layout (375x812)...');
    await send('Emulation.setDeviceMetricsOverride', {
        width: 375,
        height: 812,
        deviceScaleFactor: 2,
        mobile: true
    });
    await send('Runtime.evaluate', { expression: 'window.scrollTo(0, 0)' });
    await sleep(600);
    shot = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(`${ARTIFACTS_DIR}/netmon_audit_mobile_top.png`, Buffer.from(shot.data, 'base64'));

    await send('Runtime.evaluate', { expression: 'window.scrollTo(0, 600)' });
    await sleep(600);
    shot = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(`${ARTIFACTS_DIR}/netmon_audit_mobile_mid.png`, Buffer.from(shot.data, 'base64'));

    await send('Runtime.evaluate', { expression: 'window.scrollTo(0, 1300)' });
    await sleep(600);
    shot = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(`${ARTIFACTS_DIR}/netmon_audit_mobile_bottom.png`, Buffer.from(shot.data, 'base64'));

    // 7. Check for layout overflow issues on mobile and desktop
    const overflowCheck = await send('Runtime.evaluate', {
        expression: `JSON.stringify({
            bodyScrollWidth: document.body.scrollWidth,
            windowInnerWidth: window.innerWidth,
            hasHorizontalOverflow: document.body.scrollWidth > window.innerWidth
        })`
    });
    console.log('Overflow check (mobile):', overflowCheck.result.value);

    chrome.kill();
    console.log('=== NetMon Audit Complete ===');
    process.exit(0);
}

run().catch(err => {
    console.error('Audit failed:', err);
    process.exit(1);
});
