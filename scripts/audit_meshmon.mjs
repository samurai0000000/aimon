/*
 * audit_meshmon.mjs
 * Deep audit and screenshot capture of MeshMon web interface.
 * Copyright (C) 2026, Charles Chiou
 */

import { spawn } from 'child_process';
import http from 'http';
import fs from 'fs';

const CHROME_PATH = '/home/samurai/.cache/ms-playwright/chromium-1200/chrome-linux64/chrome';
const DEBUG_PORT = 9350;
const ARTIFACTS_DIR = '/home/samurai/.gemini/antigravity-ide/brain/ce561280-f1ed-43bf-a5da-d664a65a4be1';

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function run() {
    console.log('=== Starting MeshMon Deep Visual & Functional Audit ===');

    const chrome = spawn(CHROME_PATH, [
        '--headless=new',
        '--no-sandbox',
        '--disable-gpu',
        `--remote-debugging-port=${DEBUG_PORT}`,
        '--window-size=1920,1080',
        'http://192.168.8.245:16880/'
    ]);

    await sleep(2500);

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

    const consoleLogs = [];
    const jsExceptions = [];

    ws.onmessage = (event) => {
        const msg = JSON.parse(event.data);
        if (msg.id && pending.has(msg.id)) {
            const cb = pending.get(msg.id);
            pending.delete(msg.id);
            cb(msg.result);
        }
        if (msg.method === 'Runtime.consoleAPICalled') {
            const logStr = `${msg.params.type}: ${msg.params.args.map(a => a.value || a.description).join(' ')}`;
            console.log('CONSOLE:', logStr);
            consoleLogs.push(logStr);
        }
        if (msg.method === 'Runtime.exceptionThrown') {
            const excStr = `${msg.params.exceptionDetails.text} ${msg.params.exceptionDetails.exception?.description}`;
            console.log('EXCEPTION:', excStr);
            jsExceptions.push(excStr);
        }
    };

    await new Promise(r => ws.onopen = r);
    await send('Page.enable');
    await send('Runtime.enable');
    await send('Network.enable');

    console.log('Waiting for initial data...');
    await sleep(3500);

    // 1. Desktop Top View (Default active tab)
    let shot = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(`${ARTIFACTS_DIR}/meshmon_audit_01_desktop_top.png`, Buffer.from(shot.data, 'base64'));

    // Discover all tabs
    const tabsInfo = await send('Runtime.evaluate', {
        expression: `JSON.stringify(Array.from(document.querySelectorAll('.tabs-nav button, .nav-tab, [data-tab]')).map(b => ({
            text: b.textContent.trim().replace(/\\s+/g, ' '),
            dataTab: b.getAttribute('data-tab') || b.id || b.className,
            active: b.classList.contains('active')
        })))`
    });
    console.log('Discovered Tabs:', tabsInfo.result.value);

    // Iterate through all tabs and take screenshots
    const tabsList = JSON.parse(tabsInfo.result.value);
    let tabIdx = 2;
    for (const t of tabsList.slice(0, 8)) {
        if (!t.dataTab) continue;
        console.log(`Clicking tab [${t.text}] (${t.dataTab})...`);
        await send('Runtime.evaluate', {
            expression: `(() => {
                const btns = Array.from(document.querySelectorAll('.tabs-nav button, .nav-tab, [data-tab]'));
                const target = btns.find(b => (b.getAttribute('data-tab') === '${t.dataTab}') || b.textContent.includes('${t.text.slice(0, 10)}'));
                if (target) target.click();
            })()`
        });
        await sleep(1000);
        shot = await send('Page.captureScreenshot', { format: 'png' });
        fs.writeFileSync(`${ARTIFACTS_DIR}/meshmon_audit_${String(tabIdx).padStart(2, '0')}_tab_${t.dataTab.replace(/[^a-zA-Z0-9]/g, '_')}.png`, Buffer.from(shot.data, 'base64'));
        tabIdx++;
    }

    // Mobile Viewport Audit (375x812)
    console.log('\nTesting mobile layout (375x812)...');
    await send('Emulation.setDeviceMetricsOverride', {
        width: 375,
        height: 812,
        deviceScaleFactor: 2,
        mobile: true
    });
    await send('Runtime.evaluate', { expression: 'window.scrollTo(0, 0)' });
    await sleep(800);
    shot = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(`${ARTIFACTS_DIR}/meshmon_audit_mobile_01_top.png`, Buffer.from(shot.data, 'base64'));

    await send('Runtime.evaluate', { expression: 'window.scrollTo(0, 500)' });
    await sleep(500);
    shot = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(`${ARTIFACTS_DIR}/meshmon_audit_mobile_02_mid.png`, Buffer.from(shot.data, 'base64'));

    const mobileOverflow = await send('Runtime.evaluate', {
        expression: `JSON.stringify({
            bodyScrollWidth: document.body.scrollWidth,
            windowInnerWidth: window.innerWidth,
            hasOverflow: document.body.scrollWidth > window.innerWidth,
            docScrollWidth: document.documentElement.scrollWidth
        })`
    });
    console.log('Mobile Overflow Check:', mobileOverflow.result.value);

    // Tablet Viewport Audit (768x1024)
    console.log('\nTesting tablet layout (768x1024)...');
    await send('Emulation.setDeviceMetricsOverride', {
        width: 768,
        height: 1024,
        deviceScaleFactor: 2,
        mobile: false
    });
    await send('Runtime.evaluate', { expression: 'window.scrollTo(0, 0)' });
    await sleep(800);
    shot = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(`${ARTIFACTS_DIR}/meshmon_audit_tablet_01_top.png`, Buffer.from(shot.data, 'base64'));

    const tabletOverflow = await send('Runtime.evaluate', {
        expression: `JSON.stringify({
            bodyScrollWidth: document.body.scrollWidth,
            windowInnerWidth: window.innerWidth,
            hasOverflow: document.body.scrollWidth > window.innerWidth
        })`
    });
    console.log('Tablet Overflow Check:', tabletOverflow.result.value);

    chrome.kill();
    console.log('=== MeshMon Audit Complete ===');
    console.log('Exceptions logged:', jsExceptions.length);
    process.exit(0);
}

run().catch(err => {
    console.error('Audit failed:', err);
    process.exit(1);
});
