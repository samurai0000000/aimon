/*
 * verify_embdev_e2e.mjs
 * Comprehensive visual interactive test suite for EmbDev.
 * Copyright (C) 2026, Charles Chiou
 */

import { spawn } from 'child_process';
import http from 'http';
import fs from 'fs';
import path from 'path';

const CHROME_PATH = '/home/samurai/.cache/ms-playwright/chromium-1200/chrome-linux64/chrome';
const DEBUG_PORT = 9339;
const ARTIFACTS_DIR = '/home/samurai/.gemini/antigravity-ide/brain/ce561280-f1ed-43bf-a5da-d664a65a4be1';

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function run() {
    console.log('=== Starting EmbDev Interactive QA Suite ===');

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

    console.log('Navigating to EmbDev (http://192.168.8.30:3886/)...');
    await cdp('Page.navigate', { url: 'http://192.168.8.30:3886/' });
    await sleep(2500);

    // 1. Initial Overview & Target Switcher
    const initialTargets = await evalJs(`(() => {
        return Array.from(document.querySelectorAll('.target-btn')).map(b => ({
            text: b.innerText.trim(),
            isActive: b.classList.contains('active')
        }));
    })()`);
    console.log('EmbDev Targets:', initialTargets);
    await takeScreenshot('qa_embdev_01_target_devkit.png');

    // 2. Switch Target Board (e.g. n1-655-pro if available)
    const targetBtns = await evalJs(`document.querySelectorAll('.target-btn').length`);
    if (targetBtns > 1) {
        console.log('Switching to second target board...');
        await evalJs(`document.querySelectorAll('.target-btn')[1]?.click()`);
        await sleep(1500);
        await takeScreenshot('qa_embdev_02_target_pro.png');
        // Switch back
        await evalJs(`document.querySelectorAll('.target-btn')[0]?.click()`);
        await sleep(1000);
    }

    // 3. Scroll to Statistics and Lifecycle Events
    console.log('Scrolling to Statistics & Lifecycle section...');
    await evalJs(`(() => {
        const el = document.querySelector('.section-stats') || document.querySelector('.card-stats') || document.getElementById('recentEventsTable');
        if (el) el.scrollIntoView({ behavior: 'instant', block: 'center' });
    })()`);
    await sleep(800);
    await takeScreenshot('qa_embdev_03_lifecycle_stats.png');

    ws.close();
    chrome.kill();
    console.log('=== EmbDev Interactive QA Complete! ===');
    process.exit(0);
}

run().catch(err => {
    console.error('EmbDev QA Failed:', err);
    process.exit(1);
});
