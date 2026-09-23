/*
 * qrcode.js - Pure Vanilla JavaScript QR Code SVG Generator
 *
 * Copyright (C) 2026, Charles Chiou
 */

(function(global) {
    'use strict';

    // GF(256) Math
    var EXP_TABLE = new Array(256);
    var LOG_TABLE = new Array(256);

    (function initGF() {
        var x = 1;
        for (var i = 0; i < 255; i++) {
            EXP_TABLE[i] = x;
            LOG_TABLE[x] = i;
            x <<= 1;
            if (x & 256) x ^= 0x11d;
        }
        EXP_TABLE[255] = EXP_TABLE[0];
    })();

    function glog(n) {
        if (n < 1) throw new Error("glog(" + n + ")");
        return LOG_TABLE[n];
    }

    function gexp(n) {
        while (n < 0) n += 255;
        while (n >= 256) n -= 255;
        return EXP_TABLE[n];
    }

    function polyMultiply(p1, p2) {
        var res = new Array(p1.length + p2.length - 1);
        for (var i = 0; i < res.length; i++) res[i] = 0;
        for (var i = 0; i < p1.length; i++) {
            for (var j = 0; j < p2.length; j++) {
                res[i + j] ^= gexp(glog(p1[i]) + glog(p2[j]));
            }
        }
        return res;
    }

    function getRsGeneratorPoly(ecLen) {
        var poly = [1];
        for (var i = 0; i < ecLen; i++) {
            poly = polyMultiply(poly, [1, gexp(i)]);
        }
        return poly;
    }

    function calcErrorCorrection(data, ecLen) {
        var gen = getRsGeneratorPoly(ecLen);
        var msg = data.slice();
        for (var i = 0; i < ecLen; i++) msg.push(0);

        for (var i = 0; i < data.length; i++) {
            var lead = msg[i];
            if (lead !== 0) {
                var leadLog = glog(lead);
                for (var j = 0; j < gen.length; j++) {
                    msg[i + j] ^= gexp(leadLog + glog(gen[j]));
                }
            }
        }
        return msg.slice(data.length);
    }

    // Version parameters for Byte mode + Error Correction Level M
    var VERSION_PARAMS = [
        null,
        { version: 1, totalBytes: 26, dataBytes: 16, ecBytes: 10, alignPos: [] },
        { version: 2, totalBytes: 44, dataBytes: 28, ecBytes: 16, alignPos: [6, 18] },
        { version: 3, totalBytes: 70, dataBytes: 44, ecBytes: 26, alignPos: [6, 22] },
        { version: 4, totalBytes: 100, dataBytes: 64, ecBytes: 36, alignPos: [6, 26] },
        { version: 5, totalBytes: 134, dataBytes: 86, ecBytes: 48, alignPos: [6, 30] },
        { version: 6, totalBytes: 172, dataBytes: 108, ecBytes: 64, alignPos: [6, 34] }
    ];

    function createBitBuffer() {
        var buffer = [];
        var length = 0;
        return {
            put: function(num, len) {
                for (var i = 0; i < len; i++) {
                    buffer.push((num >>> (len - i - 1)) & 1);
                    length++;
                }
            },
            getBuffer: function() { return buffer; },
            getLength: function() { return length; }
        };
    }

    function QRCode(text, options) {
        options = options || {};
        var utf8Bytes = [];
        for (var i = 0; i < text.length; i++) {
            var code = text.charCodeAt(i);
            if (code < 0x80) {
                utf8Bytes.push(code);
            } else if (code < 0x800) {
                utf8Bytes.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
            } else {
                utf8Bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
            }
        }

        var textLen = utf8Bytes.length;
        var ver = 1;
        while (ver < VERSION_PARAMS.length && VERSION_PARAMS[ver].dataBytes < textLen + 3) {
            ver++;
        }
        if (ver >= VERSION_PARAMS.length) {
            ver = VERSION_PARAMS.length - 1;
        }

        var p = VERSION_PARAMS[ver];
        var bb = createBitBuffer();
        bb.put(4, 4); // Byte Mode
        bb.put(textLen, 8); // Length
        for (var i = 0; i < textLen; i++) {
            bb.put(utf8Bytes[i], 8);
        }

        // Terminator
        var totalDataBits = p.dataBytes * 8;
        if (bb.getLength() + 4 <= totalDataBits) {
            bb.put(0, 4);
        } else {
            bb.put(0, totalDataBits - bb.getLength());
        }

        // Pad to byte
        while (bb.getLength() % 8 !== 0) {
            bb.put(0, 1);
        }

        // Pad bytes
        var bits = bb.getBuffer();
        var data = [];
        for (var i = 0; i < bits.length; i += 8) {
            var b = 0;
            for (var j = 0; j < 8; j++) b = (b << 1) | bits[i + j];
            data.push(b);
        }

        var padByte = 0xEC;
        while (data.length < p.dataBytes) {
            data.push(padByte);
            padByte = (padByte === 0xEC) ? 0x11 : 0xEC;
        }

        var ec = calcErrorCorrection(data, p.ecBytes);
        var finalCodewords = data.concat(ec);

        // Matrix construction
        var size = ver * 4 + 17;
        var matrix = [];
        var reserved = [];
        for (var r = 0; r < size; r++) {
            matrix[r] = new Array(size);
            reserved[r] = new Array(size);
            for (var c = 0; c < size; c++) {
                matrix[r][c] = false;
                reserved[r][c] = false;
            }
        }

        function setFinder(startR, startC) {
            for (var r = -1; r <= 7; r++) {
                for (var c = -1; c <= 7; c++) {
                    var mr = startR + r, mc = startC + c;
                    if (mr >= 0 && mr < size && mc >= 0 && mc < size) {
                        reserved[mr][mc] = true;
                        if ((r >= 0 && r <= 6 && (c === 0 || c === 6)) ||
                            (c >= 0 && c <= 6 && (r === 0 || r === 6)) ||
                            (r >= 2 && r <= 4 && c >= 2 && c <= 4)) {
                            matrix[mr][mc] = true;
                        } else {
                            matrix[mr][mc] = false;
                        }
                    }
                }
            }
        }

        setFinder(0, 0);
        setFinder(0, size - 7);
        setFinder(size - 7, 0);

        // Alignment patterns
        if (p.alignPos && p.alignPos.length > 0) {
            for (var i = 0; i < p.alignPos.length; i++) {
                for (var j = 0; j < p.alignPos.length; j++) {
                    var ar = p.alignPos[i], ac = p.alignPos[j];
                    if (reserved[ar][ac]) continue;
                    for (var r = -2; r <= 2; r++) {
                        for (var c = -2; c <= 2; c++) {
                            reserved[ar + r][ac + c] = true;
                            if (Math.abs(r) === 2 || Math.abs(c) === 2 || (r === 0 && c === 0)) {
                                matrix[ar + r][ac + c] = true;
                            } else {
                                matrix[ar + r][ac + c] = false;
                            }
                        }
                    }
                }
            }
        }

        // Timing patterns
        for (var i = 8; i < size - 8; i++) {
            if (!reserved[6][i]) {
                reserved[6][i] = true;
                matrix[6][i] = (i % 2 === 0);
            }
            if (!reserved[i][6]) {
                reserved[i][6] = true;
                matrix[i][6] = (i % 2 === 0);
            }
        }

        // Dark module
        reserved[4 * ver + 9][8] = true;
        matrix[4 * ver + 9][8] = true;

        // Reserved format bits
        for (var i = 0; i < 9; i++) {
            reserved[8][i] = true;
            reserved[i][8] = true;
        }
        for (var i = size - 8; i < size; i++) {
            reserved[8][i] = true;
            reserved[i][8] = true;
        }

        // Place data bits
        var allBits = [];
        for (var i = 0; i < finalCodewords.length; i++) {
            for (var b = 7; b >= 0; b--) {
                allBits.push((finalCodewords[i] >>> b) & 1);
            }
        }

        var bitIdx = 0;
        var dir = -1;
        var row = size - 1;
        var col = size - 1;

        while (col > 0) {
            if (col === 6) col--;
            for (var count = 0; count < size; count++) {
                for (var c = 0; c < 2; c++) {
                    var currC = col - c;
                    if (!reserved[row][currC]) {
                        var bit = (bitIdx < allBits.length) ? allBits[bitIdx++] : 0;
                        // Mask 0: (row + col) % 2 == 0
                        var mask = ((row + currC) % 2 === 0);
                        matrix[row][currC] = (bit === 1) ? !mask : mask;
                    }
                }
                row += dir;
            }
            dir = -dir;
            row += dir;
            col -= 2;
        }

        // Format info for EC Level M (00), Mask 0 (000) -> Format bits: 101010000010010 (XOR with 101010000010010)
        // Standard Format Info: Level M (0), Mask 0 (0) with mask 0x5412 is 0x504B (101000001001011)
        var formatBits = [1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1];
        for (var i = 0; i < 6; i++) matrix[8][i] = (formatBits[i] === 1);
        matrix[8][7] = (formatBits[6] === 1);
        matrix[8][8] = (formatBits[7] === 1);
        matrix[7][8] = (formatBits[8] === 1);
        for (var i = 9; i < 15; i++) matrix[14 - i][8] = (formatBits[i] === 1);

        for (var i = 0; i < 8; i++) matrix[size - 1 - i][8] = (formatBits[i] === 1);
        for (var i = 8; i < 15; i++) matrix[8][size - 15 + i] = (formatBits[i] === 1);

        return {
            size: size,
            isDark: function(r, c) { return matrix[r][c]; },
            toSvg: function(opt) {
                opt = opt || {};
                var margin = opt.margin !== undefined ? opt.margin : 2;
                var fg = opt.fg || '#38bdf8';
                var bg = opt.bg || '#0b1120';
                var total = size + margin * 2;

                var path = '';
                for (var r = 0; r < size; r++) {
                    for (var c = 0; c < size; c++) {
                        if (matrix[r][c]) {
                            path += 'M' + (c + margin) + ',' + (r + margin) + 'h1v1h-1z ';
                        }
                    }
                }

                return '<svg viewBox="0 0 ' + total + ' ' + total + '" class="qr-svg-matrix" xmlns="http://www.w3.org/2000/svg" style="width:100%;height:100%;border-radius:12px;border:1px solid rgba(56,189,248,0.25);">' +
                    '<rect width="' + total + '" height="' + total + '" fill="' + bg + '"/>' +
                    '<path d="' + path + '" fill="' + fg + '"/>' +
                    '</svg>';
            }
        };
    }

    global.generateQrSvg = function(text, options) {
        try {
            var qr = QRCode(text, options);
            return qr.toSvg(options);
        } catch (e) {
            console.error('QR code generation error:', e);
            return '<div class="qr-error">QR Generation Error</div>';
        }
    };

})(typeof window !== 'undefined' ? window : this);
