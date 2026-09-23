/*
 * capture_current_screens.mjs
 *
 * Copyright (C) 2026, Charles Chiou
 */

import { spawn } from 'child_process';
import http from 'http';
import fs from 'fs';
import path from 'path';

const HOME = process.env.HOME || '/root';
const CHROME_PATH = path.join(HOME, '.cache/ms-playwright/chromium-1200/chrome-linux64/chrome');
const DEBUG_PORT = 9342;
const ARTIFACTS_DIR = process.env.ARTIFACTS_DIR || path.join(HOME, '.gemini/antigravity-ide/brain');

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function main() {
    console.log('Launching chrome...');
    const chrome = spawn(CHROME_PATH, [
        '--headless=new',
        '--no-sandbox',
        '--disable-gpu',
        `--remote-debugging-port=${DEBUG_PORT}`,
        '--window-size=1920,1080',
        'http://127.0.0.1:3883/'
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
    const callbacks = new Map();

    ws.onmessage = (event) => {
        const msg = JSON.parse(event.data);
        if (msg.id && callbacks.has(msg.id)) {
            const cb = callbacks.get(msg.id);
            callbacks.delete(msg.id);
            cb(msg);
        }
    };

    function send(method, params = {}) {
        return new Promise((resolve) => {
            const id = msgId++;
            callbacks.set(id, resolve);
            ws.send(JSON.stringify({ id, method, params }));
        });
    }

    await new Promise(r => ws.onopen = r);

    await send('Page.enable');
    await send('Runtime.enable');
    await send('DOM.enable');

    async function evaluate(expression) {
        const res = await send('Runtime.evaluate', { expression, returnByValue: true });
        return res?.result?.result?.value;
    }

    async function takeScreenshot(filename) {
        const res = await send('Page.captureScreenshot', { format: 'png', fromSurface: true });
        if (res && res.result && res.result.data) {
            const buf = Buffer.from(res.result.data, 'base64');
            const filePath = path.join(ARTIFACTS_DIR, filename);
            fs.writeFileSync(filePath, buf);
            console.log(`Saved screenshot: ${filePath}`);
        }
    }

    await sleep(2000);

    // 1. Capture quotas view
    console.log('Capturing quotas view...');
    await takeScreenshot('screen_quotas.png');

    // 2. Click telemetry subnav tab
    console.log('Switching to telemetry tab...');
    await evaluate(`(document.querySelector('[data-subpanel="telemetry"]') || document.querySelector('[data-id="telemetry"]'))?.click()`);
    await sleep(2000);
    await takeScreenshot('screen_telemetry.png');
    await evaluate(`document.querySelector('.telemetry-charts-grid')?.scrollIntoView({ behavior: 'instant', block: 'center' })`);
    await sleep(1000);
    await takeScreenshot('screen_telemetry_2x2.png');

    // 3. Click approvals subnav tab
    console.log('Switching to approvals tab...');
    await evaluate(`(document.querySelector('[data-subpanel="approvals"]') || document.querySelector('[data-id="approvals"]'))?.click()`);
    await sleep(1500);
    await takeScreenshot('screen_approvals.png');

    // 4. Click netmon satellite tab
    console.log('Switching to netmon tab...');
    await evaluate(`document.querySelector('[data-id="netmon"]')?.click()`);
    await sleep(2500);
    await takeScreenshot('screen_netmon.png');

    // 5. Click meshmon satellite tab
    console.log('Switching to meshmon tab...');
    await evaluate(`document.querySelector('[data-id="meshmon"]')?.click()`);
    await sleep(2500);
    await takeScreenshot('screen_meshmon.png');

    // 6. Click embdevenv satellite tab
    console.log('Switching to embdevenv tab...');
    await evaluate(`document.querySelector('[data-id="embdevenv"]')?.click()`);
    await sleep(2500);
    await takeScreenshot('screen_embdevenv.png');

    ws.close();
    chrome.kill();
    console.log('All screenshots captured successfully!');
}

main().catch(err => {
    console.error('Error:', err);
    process.exit(1);
});
