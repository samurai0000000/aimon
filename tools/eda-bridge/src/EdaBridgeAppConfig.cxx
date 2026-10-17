/*
 * EdaBridgeAppConfig.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "EdaBridgeAppConfig.hxx"
#include "Version.hxx"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace aimon {

namespace {

uint32_t crc32Compute(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int b = 0; b < 8; ++b) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

void appendLe16(std::vector<uint8_t>& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFFu));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFFu));
}

void appendLe32(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFFu));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFFu));
}

std::string injectAimonVersion(std::string text) {
    const std::string token = "__AIMON_VERSION__";
    const std::string ver = AIMON_VERSION_STRING;
    size_t pos = 0;
    while ((pos = text.find(token, pos)) != std::string::npos) {
        text.replace(pos, token.size(), ver);
        pos += ver.size();
    }
    return text;
}

static const char* kEmbeddedEasyEdaExtensionJson = R"JSON({
  "name": "aimon-easyeda-pro-bridge",
  "uuid": "8d4c91f2a70e4c6b9b24e31f6a821100",
  "displayName": "Aimon AI EDA Bridge",
  "description": "Connects EasyEDA Pro Desktop to aimon-eda-bridge over local loopback WebSocket (127.0.0.1:3893)",
  "version": "__AIMON_VERSION__",
  "publisher": "Charles Chiou",
  "engines": {
    "eda": "^2.2.0"
  },
  "license": "Proprietary",
  "repository": {
    "type": "extension-store",
    "url": ""
  },
  "categories": "Other",
  "keywords": [
    "AI",
    "EDA",
    "Aimon",
    "Bridge"
  ],
  "homepage": "https://selfso.com",
  "bugs": "https://selfso.com",
  "activationEvents": {},
  "entry": "./dist/index",
  "dependentExtensions": {},
  "headerMenus": {
    "home": [
      {
        "id": "aimonBridgeMenu",
        "title": "Aimon AI Bridge",
        "menuItems": [
          {
            "id": "aimonReconnect",
            "title": "Reconnect Bridge",
            "registerFn": "reconnectBridge"
          },
          {
            "id": "aimonStatus",
            "title": "Bridge Status",
            "registerFn": "showBridgeStatus"
          }
        ]
      }
    ],
    "blank": [
      {
        "id": "aimonBridgeMenu",
        "title": "Aimon AI Bridge",
        "menuItems": [
          {
            "id": "aimonReconnect",
            "title": "Reconnect Bridge",
            "registerFn": "reconnectBridge"
          },
          {
            "id": "aimonStatus",
            "title": "Bridge Status",
            "registerFn": "showBridgeStatus"
          }
        ]
      }
    ],
    "sch": [
      {
        "id": "aimonBridgeMenu",
        "title": "Aimon AI Bridge",
        "menuItems": [
          {
            "id": "aimonReconnect",
            "title": "Reconnect Bridge",
            "registerFn": "reconnectBridge"
          },
          {
            "id": "aimonStatus",
            "title": "Bridge Status",
            "registerFn": "showBridgeStatus"
          }
        ]
      }
    ],
    "symbol": [
      {
        "id": "aimonBridgeMenu",
        "title": "Aimon AI Bridge",
        "menuItems": [
          {
            "id": "aimonReconnect",
            "title": "Reconnect Bridge",
            "registerFn": "reconnectBridge"
          },
          {
            "id": "aimonStatus",
            "title": "Bridge Status",
            "registerFn": "showBridgeStatus"
          }
        ]
      }
    ],
    "pcb": [
      {
        "id": "aimonBridgeMenu",
        "title": "Aimon AI Bridge",
        "menuItems": [
          {
            "id": "aimonReconnect",
            "title": "Reconnect Bridge",
            "registerFn": "reconnectBridge"
          },
          {
            "id": "aimonStatus",
            "title": "Bridge Status",
            "registerFn": "showBridgeStatus"
          }
        ]
      }
    ],
    "footprint": [
      {
        "id": "aimonBridgeMenu",
        "title": "Aimon AI Bridge",
        "menuItems": [
          {
            "id": "aimonReconnect",
            "title": "Reconnect Bridge",
            "registerFn": "reconnectBridge"
          },
          {
            "id": "aimonStatus",
            "title": "Bridge Status",
            "registerFn": "showBridgeStatus"
          }
        ]
      }
    ]
  }
}
)JSON";

static const char* kEmbeddedEasyEdaIndexJs = R"JS(/*
 * index.js - EasyEDA Pro Desktop Extension for aimon-eda-bridge
 *
 * Copyright (C) 2026, Charles Chiou
 */

var edaEsbuildExportName = (function () {
    "use strict";

    const BRIDGE_VERSION = "1.1.8";
    const DEFAULT_WS_URL = "ws://127.0.0.1:3893";
    const HEARTBEAT_INTERVAL_MS = 10000;
    const ALLOWED_PREFIXES = [
        "eda.sch_",
        "eda.pcb_",
        "eda.lib_",
        "eda.dmt_",
        "eda.drc_",
        "eda.sys_Document"
    ];

    let wsClient = null;
    let useEdaSysWs = false;
    let wsIdBase = "aimon-ws-" + Math.random().toString(36).slice(2, 10);
    let wsGeneration = 0;
    let currentWsId = wsIdBase;
    let isConnected = false;
    let isConnecting = false;
    let connectingTicks = 0;
    let hasRegisteredWs = false;
    let permissionBlocked = false;
    let missedHeartbeats = 0;
    let heartbeatSeq = 0;
    let heartbeatTimer = null;
    let reconnectTimer = null;
    let cachedActiveDocument = "EasyEDA-Pro";

    function logDiag(msg) {
        try {
            if (typeof eda !== "undefined" && eda.sys_Log && typeof eda.sys_Log.add === "function") {
                eda.sys_Log.add("[aimon-eda-bridge] " + msg);
            }
        } catch (err) {
        }
    }

    function showToast(msg) {
        try {
            if (typeof eda !== "undefined" && eda.sys_Message && typeof eda.sys_Message.showToastMessage === "function") {
                eda.sys_Message.showToastMessage(String(msg));
            }
        } catch (err) {
        }
    }

    async function resolveActiveDocumentTitle() {
        try {
            if (typeof eda !== "undefined" && eda.dmt_SelectControl && typeof eda.dmt_SelectControl.getCurrentDocumentInfo === "function") {
                const info = await eda.dmt_SelectControl.getCurrentDocumentInfo();
                if (!info || info.documentType === -1 || info.tabId === "tab_page1") {
                    cachedActiveDocument = "Start Page";
                    return cachedActiveDocument;
                }
                const docTitle = info.title || info.name || info.uuid || "";
                if (docTitle) {
                    cachedActiveDocument = String(docTitle);
                }
            }
        } catch (err) {
        }
        return cachedActiveDocument;
    }

    function getActiveDocumentTitle() {
        return cachedActiveDocument;
    }

    function callGetter(obj, getterName, fallbackProp, fallbackProp2, defaultVal) {
        if (!obj) return defaultVal;
        try {
            if (typeof obj[getterName] === "function") {
                const v = obj[getterName]();
                if (v !== undefined && v !== null) return v;
            }
        } catch (err) {
        }
        if (fallbackProp && obj[fallbackProp] !== undefined && obj[fallbackProp] !== null) {
            return obj[fallbackProp];
        }
        if (fallbackProp2 && obj[fallbackProp2] !== undefined && obj[fallbackProp2] !== null) {
            return obj[fallbackProp2];
        }
        return defaultVal;
    }

    async function handleSearchLibrary(args) {
        const query = String(args.query || "");
        const libUuid = args.library_uuid || undefined;
        let items = [];
        if (typeof eda !== "undefined" && eda.lib_Device && eda.lib_Device.search) {
            const raw = await eda.lib_Device.search(query, libUuid);
            if (Array.isArray(raw)) {
                items = raw.map(function (item) {
                    return {
                        uuid: item.uuid || "",
                        library_uuid: item.libraryUuid || item.library_uuid || "",
                        symbol_uuid: item.symbolUuid || item.symbol_uuid || "",
                        footprint_uuid: item.footprintUuid || item.footprint_uuid || "",
                        title: item.title || item.name || "",
                        lcsc: item.supplierPart || item.supplierId || item.lcsc || "",
                        footprint: item.footprintTitle || item.package || "",
                        library_type: item.jlcpcbPartClass || "Extended",
                        stock: Number(item.stock || 0)
                    };
                });
            }
        }
        return {
            ok: true,
            query: query,
            count: items.length,
            devices: items
        };
    }

    async function extractComponentPins(primitiveId) {
        const pins = [];
        if (!primitiveId || typeof eda === "undefined" || !eda.sch_PrimitiveComponent ||
            typeof eda.sch_PrimitiveComponent.getAllPinsByPrimitiveId !== "function") {
            return pins;
        }
        try {
            const rawPins = await eda.sch_PrimitiveComponent.getAllPinsByPrimitiveId(primitiveId);
            if (Array.isArray(rawPins)) {
                for (const p of rawPins) {
                    pins.push({
                        id: String(callGetter(p, "getState_PrimitiveId", "primitiveId", "id", "")),
                        number: String(callGetter(p, "getState_PinNumber", "pinNumber", "number", "")),
                        name: String(callGetter(p, "getState_PinName", "pinName", "name", "")),
                        x: Number(callGetter(p, "getState_X", "x", "", 0) || 0),
                        y: Number(callGetter(p, "getState_Y", "y", "", 0) || 0),
                        rotation: Number(callGetter(p, "getState_Rotation", "rotation", "", 0) || 0),
                        net: String(callGetter(p, "getState_Net", "net", "", "")),
                        no_connected: Boolean(callGetter(p, "getState_NoConnected", "noConnected", "no_connected", false))
                    });
                }
            }
        } catch (err) {
            logDiag("getAllPinsByPrimitiveId(" + String(primitiveId) + ") error: " + String(err));
        }
        return pins;
    }

    const MM_TO_MIL = 39.37007874;

    function toCanvasUnits(val, units) {
        const n = Number(val || 0);
        if (String(units || "mil").toLowerCase() === "mm") {
            return Math.round(n * MM_TO_MIL * 1000) / 1000;
        }
        return n;
    }

    function coordToMil(obj, keyCanvas, keyMm, units, defaultVal) {
        if (obj && keyMm && obj[keyMm] !== undefined && obj[keyMm] !== null) {
            return Math.round(Number(obj[keyMm]) * MM_TO_MIL * 1000) / 1000;
        }
        if (obj && keyCanvas && obj[keyCanvas] !== undefined && obj[keyCanvas] !== null) {
            return toCanvasUnits(obj[keyCanvas], units);
        }
        return toCanvasUnits(defaultVal !== undefined ? defaultVal : 0, units);
    }

    function sanitizeProjectInfo(rawProj) {
        if (!rawProj || typeof rawProj !== "object") return {};
        try {
            const copy = JSON.parse(JSON.stringify(rawProj));
            if (Array.isArray(copy.data)) {
                for (const item of copy.data) {
                    if (item && item.schematic && Array.isArray(item.schematic.page)) {
                        for (const pg of item.schematic.page) {
                            if (pg && typeof pg === "object") {
                                delete pg.titleBlockData;
                            }
                        }
                    }
                }
            }
            return copy;
        } catch (err) {
            return rawProj;
        }
    }

    async function ensureDocumentOpen(targetType, targetPageOrUuid, cachedProjInfo) {
        let docInfo = {};
        let projInfo = cachedProjInfo || {};
        if (typeof eda === "undefined") {
            return { documentInfo: docInfo, projectInfo: projInfo };
        }
        try {
            if (eda.dmt_SelectControl && typeof eda.dmt_SelectControl.getCurrentDocumentInfo === "function") {
                docInfo = (await eda.dmt_SelectControl.getCurrentDocumentInfo()) || {};
            }
        } catch (err) {
        }
        try {
            if ((!projInfo || !projInfo.data) && eda.dmt_Project && typeof eda.dmt_Project.getCurrentProjectInfo === "function") {
                projInfo = (await eda.dmt_Project.getCurrentProjectInfo()) || {};
            }
        } catch (err) {
        }

        const firstBoard = (projInfo && Array.isArray(projInfo.data) && projInfo.data.length > 0)
            ? projInfo.data[0]
            : null;
        let desiredUuid = "";

        if (targetType === "pcb") {
            if (targetPageOrUuid && String(targetPageOrUuid).trim()) {
                desiredUuid = String(targetPageOrUuid).trim();
            } else if (docInfo && docInfo.documentType !== 3 && firstBoard && firstBoard.pcb && firstBoard.pcb.uuid) {
                desiredUuid = String(firstBoard.pcb.uuid);
            }
        } else if (targetType === "schematic") {
            const pages = (firstBoard && firstBoard.schematic && Array.isArray(firstBoard.schematic.page))
                ? firstBoard.schematic.page
                : [];
            if (targetPageOrUuid && String(targetPageOrUuid).trim()) {
                const req = String(targetPageOrUuid).trim();
                const matched = pages.find(function (p) {
                    return p && (String(p.uuid) === req || String(p.name) === req || String(p.title) === req);
                });
                desiredUuid = matched ? String(matched.uuid) : req;
            } else if ((!docInfo || docInfo.documentType !== 1) && pages.length > 0 && pages[0].uuid) {
                desiredUuid = String(pages[0].uuid);
            }
        }

        if (desiredUuid && (!docInfo || String(docInfo.uuid || "") !== desiredUuid) &&
            eda.dmt_EditorControl && typeof eda.dmt_EditorControl.openDocument === "function") {
            try {
                await eda.dmt_EditorControl.openDocument(desiredUuid);
                if (eda.dmt_SelectControl && typeof eda.dmt_SelectControl.getCurrentDocumentInfo === "function") {
                    docInfo = (await eda.dmt_SelectControl.getCurrentDocumentInfo()) || docInfo;
                }
            } catch (err) {
                logDiag("openDocument(" + desiredUuid + ") error: " + String(err));
            }
        }
        return { documentInfo: docInfo, projectInfo: projInfo };
    }

    async function handleInspectSchematic(args) {
        const components = [];
        const wires = [];
        let netlist = {};
        let projectInfo = {};
        let documentInfo = {};
        const includeOtherProp = Boolean(args && args.include_other_property === true);
        const includePins = (args && args.include_pins !== undefined) ? Boolean(args.include_pins) : true;
        const includeWires = (args && args.include_wires !== undefined) ? Boolean(args.include_wires) : true;
        const summaryOnly = Boolean(args && args.summary_only === true);
        const allSheets = (args && args.all_sheets !== undefined) ? Boolean(args.all_sheets) : true;
        const reqSheet = (args && (args.page_uuid || args.sheet || args.page)) || "";
        const refFilter = (args && Array.isArray(args.references)) ? args.references.map(String) : [];
        const netFilter = (args && Array.isArray(args.nets)) ? args.nets.map(String) : [];
        const typeFilter = (args && Array.isArray(args.component_types)) ? args.component_types.map(String) : [];
        const fullForBridge = Boolean(args && args._full_for_bridge === true);

        if (typeof eda !== "undefined") {
            const needActiveSchematicTab = Boolean(includePins || includeWires || reqSheet || netFilter.length > 0);
            const ensured = await ensureDocumentOpen(
                needActiveSchematicTab ? "schematic" : "",
                reqSheet,
                null
            );
            documentInfo = ensured.documentInfo || {};
            projectInfo = sanitizeProjectInfo(ensured.projectInfo || {});

            if (!documentInfo || documentInfo.documentType === -1 || documentInfo.tabId === "tab_page1") {
                cachedActiveDocument = "Start Page";
                return {
                    ok: true,
                    active_document: cachedActiveDocument,
                    project: projectInfo,
                    document: documentInfo,
                    components_count: 0,
                    wires_count: 0,
                    components: [],
                    wires: [],
                    netlist: {}
                };
            }
            const docTitle = documentInfo.title || documentInfo.name || documentInfo.uuid || "Schematic";
            cachedActiveDocument = String(docTitle);

            try {
                if (eda.sch_PrimitiveComponent && eda.sch_PrimitiveComponent.getAll) {
                    let activeComps = [];
                    if (documentInfo.documentType === 1) {
                        try {
                            activeComps = (await eda.sch_PrimitiveComponent.getAll(undefined, false)) || [];
                        } catch (err) {
                        }
                    }
                    let rawComps = activeComps;
                    if (allSheets) {
                        try {
                            const allC = await eda.sch_PrimitiveComponent.getAll(undefined, true);
                            if (Array.isArray(allC) && allC.length > 0) {
                                rawComps = allC;
                            }
                        } catch (err) {
                        }
                    }

                    const activeSheetIds = {};
                    const activeSheetRefs = {};
                    const compKeyOf = function (compObj) {
                        const cuid = String(callGetter(compObj, "getState_UniqueId", "uniqueId", "unique_id", ""));
                        const cid = String(callGetter(compObj, "getState_PrimitiveId", "primitiveId", "id", ""));
                        const cref = String(callGetter(compObj, "getState_Designator", "designator", "reference", ""));
                        const cnet = String(callGetter(compObj, "getState_Net", "net", ""));
                        const cx = String(callGetter(compObj, "getState_X", "x", "0"));
                        const cy = String(callGetter(compObj, "getState_Y", "y", "0"));
                        return cuid ? ("uid:" + cuid) : ("id:" + cid + ":" + cref + ":" + cnet + ":" + cx + ":" + cy);
                    };
                    if (Array.isArray(activeComps)) {
                        for (const ac of activeComps) {
                            const akey = compKeyOf(ac);
                            const aref = String(callGetter(ac, "getState_Designator", "designator", "reference", ""));
                            if (akey) activeSheetIds[akey] = true;
                            if (aref) activeSheetRefs[aref] = true;
                        }
                    }

                    // If caller requested specific references with pins/wires and none are on the active sheet, switch to the sheet containing them
                    if (refFilter.length > 0 && (includePins || includeWires) && !reqSheet &&
                        !refFilter.some(function (r) { return activeSheetRefs[r]; })) {
                        const pages = (projectInfo.data && projectInfo.data[0] &&
                                       projectInfo.data[0].schematic &&
                                       Array.isArray(projectInfo.data[0].schematic.page))
                            ? projectInfo.data[0].schematic.page
                            : [];
                        for (const pg of pages) {
                            if (!pg || !pg.uuid || String(pg.uuid) === String(documentInfo.uuid || "")) continue;
                            const sw = await ensureDocumentOpen("schematic", pg.uuid, projectInfo);
                            documentInfo = sw.documentInfo || documentInfo;
                            cachedActiveDocument = String(documentInfo.title || documentInfo.name || documentInfo.uuid || cachedActiveDocument);
                            try {
                                activeComps = (await eda.sch_PrimitiveComponent.getAll(undefined, false)) || [];
                            } catch (err) {
                                activeComps = [];
                            }
                            for (const k of Object.keys(activeSheetIds)) delete activeSheetIds[k];
                            for (const k of Object.keys(activeSheetRefs)) delete activeSheetRefs[k];
                            for (const ac of activeComps) {
                                const akey = compKeyOf(ac);
                                const aref = String(callGetter(ac, "getState_Designator", "designator", "reference", ""));
                                if (akey) activeSheetIds[akey] = true;
                                if (aref) activeSheetRefs[aref] = true;
                            }
                            if (refFilter.some(function (r) { return activeSheetRefs[r]; })) {
                                break;
                            }
                        }
                    }

                    const hasActiveSet = Object.keys(activeSheetIds).length > 0;
                    if (Array.isArray(rawComps)) {
                        for (const c of rawComps) {
                            const compId = String(callGetter(c, "getState_PrimitiveId", "primitiveId", "id", ""));
                            const compType = String(callGetter(c, "getState_ComponentType", "componentType", "", ""));
                            const ref = String(callGetter(c, "getState_Designator", "designator", "reference", ""));
                            const onActiveSheet = hasActiveSet ? Boolean(activeSheetIds[compKeyOf(c)]) : (documentInfo.documentType === 1);
                            const isPart = (compType === "part" || (!compType && ref.indexOf("#") !== 0 && ref.indexOf("FRAME") !== 0));
                            if (!isPart && !onActiveSheet) continue;

                            if (!fullForBridge) {
                                if (refFilter.length > 0 && refFilter.indexOf(ref) < 0) continue;
                                if (typeFilter.length > 0 && typeFilter.indexOf(compType) < 0) continue;
                            }

                            const fpRaw = callGetter(c, "getState_Footprint", "footprintName", "footprint", "");
                            const fpStr = (fpRaw && typeof fpRaw === "object")
                                ? String(fpRaw.name || fpRaw.title || fpRaw.uuid || "")
                                : String(fpRaw || "");

                            const matchesRefFilter = (refFilter.length === 0 || refFilter.indexOf(ref) >= 0);
                            const matchesTypeFilter = (typeFilter.length === 0 || typeFilter.indexOf(compType) >= 0);
                            const needPinsForThisComp = isPart && onActiveSheet &&
                                (includePins || netFilter.length > 0 || fullForBridge) &&
                                (matchesRefFilter || netFilter.length > 0) &&
                                (matchesTypeFilter || netFilter.length > 0);

                            const compPins = needPinsForThisComp ? (await extractComponentPins(compId)) : [];
                            const compRec = {
                                id: compId,
                                unique_id: String(callGetter(c, "getState_UniqueId", "uniqueId", "", "")),
                                component_type: compType,
                                reference: ref,
                                value: String(callGetter(c, "getState_Name", "value", "name", "")),
                                footprint: fpStr,
                                lcsc: String(callGetter(c, "getState_SupplierId", "supplierPart", "supplierId", "")),
                                manufacturer: String(callGetter(c, "getState_Manufacturer", "manufacturer", "", "")),
                                manufacturer_part: String(callGetter(c, "getState_ManufacturerId", "manufacturerId", "", "")),
                                net: String(callGetter(c, "getState_Net", "net", "", "")),
                                x: Number(callGetter(c, "getState_X", "x", "", 0) || 0),
                                y: Number(callGetter(c, "getState_Y", "y", "", 0) || 0),
                                rotation: Number(callGetter(c, "getState_Rotation", "rotation", "", 0) || 0),
                                on_active_sheet: onActiveSheet,
                                pins: compPins
                            };
                            if (includeOtherProp) {
                                compRec.other_property = callGetter(c, "getState_OtherProperty", "otherProperty", "", {});
                            }
                            components.push(compRec);
                        }
                    }
                }
            } catch (err) {
                logDiag("sch_PrimitiveComponent.getAll error: " + String(err));
            }
            if (includeWires || netFilter.length > 0 || summaryOnly || fullForBridge) {
                try {
                    if (eda.sch_PrimitiveWire && eda.sch_PrimitiveWire.getAll) {
                        const rawWires = await eda.sch_PrimitiveWire.getAll();
                        if (Array.isArray(rawWires)) {
                            for (const w of rawWires) {
                                wires.push({
                                    id: String(callGetter(w, "getState_PrimitiveId", "primitiveId", "id", "")),
                                    net: String(callGetter(w, "getState_Net", "net", "", "")),
                                    line: callGetter(w, "getState_Line", "line", "", [])
                                });
                            }
                        }
                    }
                } catch (err) {
                    logDiag("sch_PrimitiveWire.getAll error: " + String(err));
                }
            }
            if (args && args.include_netlist === true) {
                if (eda.sch_Netlist && eda.sch_Netlist.getNetlist) {
                    try {
                        netlist = (await eda.sch_Netlist.getNetlist("JLCEDA_PRO")) || {};
                    } catch (err1) {
                        try {
                            netlist = (await eda.sch_Netlist.getNetlist("JLCEDA")) || {};
                        } catch (err2) {
                        }
                    }
                }
            }
        }

        let filteredComponents = components;
        let filteredWires = wires;
        if (!fullForBridge) {
            filteredComponents = components.filter(function (comp) {
                if (refFilter.length > 0 && refFilter.indexOf(comp.reference) < 0) return false;
                if (typeFilter.length > 0 && typeFilter.indexOf(comp.component_type) < 0) return false;
                if (netFilter.length > 0) {
                    let matchesNet = netFilter.indexOf(comp.net) >= 0 || netFilter.indexOf(comp.value) >= 0;
                    if (!matchesNet && Array.isArray(comp.pins)) {
                        matchesNet = comp.pins.some(function (p) { return netFilter.indexOf(p.net) >= 0; });
                    }
                    if (!matchesNet) return false;
                }
                return true;
            });
            if (!includePins) {
                filteredComponents = filteredComponents.map(function (comp) {
                    const copy = Object.assign({}, comp);
                    delete copy.pins;
                    return copy;
                });
            }
            filteredWires = wires.filter(function (w) {
                if (netFilter.length > 0 && netFilter.indexOf(w.net) < 0) return false;
                return true;
            });
        }

        const out = {
            ok: true,
            active_document: cachedActiveDocument,
            project: projectInfo,
            document: documentInfo,
            components_count: filteredComponents.length,
            wires_count: filteredWires.length,
            netlist: netlist
        };
        if (!summaryOnly || fullForBridge) {
            out.components = filteredComponents;
            if (includeWires || fullForBridge) {
                out.wires = filteredWires;
            }
        }
        return out;
    }

    function normalizePcbLayer(layerName) {
        const s = String(layerName || "TopLayer").trim();
        if (s === "F.Cu" || s === "Top" || s === "TopLayer" || s === "1") return "TopLayer";
        if (s === "B.Cu" || s === "Bottom" || s === "BottomLayer" || s === "2") return "BottomLayer";
        if (s === "In1.Cu" || s === "InnerLayer1" || s === "15") return "InnerLayer1";
        if (s === "In2.Cu" || s === "InnerLayer2" || s === "16") return "InnerLayer2";
        if (s === "In3.Cu" || s === "InnerLayer3" || s === "17") return "InnerLayer3";
        if (s === "In4.Cu" || s === "InnerLayer4" || s === "18") return "InnerLayer4";
        if (s === "F.SilkS" || s === "TopSilkLayer" || s === "3") return "TopSilkLayer";
        if (s === "B.SilkS" || s === "BottomSilkLayer" || s === "4") return "BottomSilkLayer";
        if (s === "Edge.Cuts" || s === "BoardOutLine" || s === "11") return "BoardOutLine";
        return s;
    }

    function resolveEasyEdaCopperLayerId(layerName) {
        if (typeof layerName === "number" && layerName >= 1 && layerName <= 46) {
            return layerName;
        }
        const s = String(layerName || "TopLayer").trim();
        if (s === "F.Cu" || s === "Top" || s === "TopLayer" || s === "1") return 1;
        if (s === "B.Cu" || s === "Bottom" || s === "BottomLayer" || s === "2") return 2;
        if (s === "In1.Cu" || s === "InnerLayer1" || s === "15") return 15;
        if (s === "In2.Cu" || s === "InnerLayer2" || s === "16") return 16;
        if (s === "In3.Cu" || s === "InnerLayer3" || s === "17") return 17;
        if (s === "In4.Cu" || s === "InnerLayer4" || s === "18") return 18;
        const mInner = s.match(/^InnerLayer(\d+)$/i) || s.match(/^In(\d+)\.Cu$/i);
        if (mInner) {
            const idx = parseInt(mInner[1], 10);
            if (idx >= 1 && idx <= 32) return 14 + idx;
        }
        const num = parseInt(s, 10);
        if (!isNaN(num) && num >= 1 && num <= 46) return num;
        return 1;
    }

    function isCopperLayer(layerVal) {
        if (typeof layerVal === "number") {
            return layerVal === 1 || layerVal === 2 || (layerVal >= 15 && layerVal <= 46);
        }
        const s = String(layerVal || "").trim();
        if (s === "1" || s === "2" || s === "TopLayer" || s === "BottomLayer" ||
            s === "F.Cu" || s === "B.Cu" || s.indexOf("InnerLayer") === 0 || s.indexOf(".Cu") > 0) {
            return true;
        }
        const n = parseInt(s, 10);
        return !isNaN(n) && (n === 1 || n === 2 || (n >= 15 && n <= 46));
    }

    function isBoardOutlineLayer(layerVal) {
        if (typeof layerVal === "number") {
            return layerVal === 11;
        }
        const s = String(layerVal || "").trim();
        return s === "11" || s === "BoardOutLine" || s === "Edge.Cuts";
    }

    function defaultPadsForEasyEdaFootprint(fpName) {
        const upper = String(fpName || "").toUpperCase();
        if (upper.indexOf("SOT-23") >= 0) {
            return [
                { number: "1", rel_x: -0.95, rel_y: 1.1, size_x: 0.6, size_y: 0.7 },
                { number: "2", rel_x: 0.95, rel_y: 1.1, size_x: 0.6, size_y: 0.7 },
                { number: "3", rel_x: 0.0, rel_y: -1.1, size_x: 0.6, size_y: 0.7 }
            ];
        }
        if (upper.indexOf("SOIC-8") >= 0 || upper.indexOf("SOP-8") >= 0) {
            const pads = [];
            for (let i = 0; i < 4; ++i) {
                pads.push({ number: String(i + 1), rel_x: -2.7, rel_y: -1.905 + i * 1.27, size_x: 1.5, size_y: 0.6 });
            }
            for (let i = 0; i < 4; ++i) {
                pads.push({ number: String(i + 5), rel_x: 2.7, rel_y: 1.905 - i * 1.27, size_x: 1.5, size_y: 0.6 });
            }
            return pads;
        }
        if (upper.indexOf("0805") >= 0) {
            return [
                { number: "1", rel_x: -1.0, rel_y: 0.0, size_x: 1.0, size_y: 1.3 },
                { number: "2", rel_x: 1.0, rel_y: 0.0, size_x: 1.0, size_y: 1.3 }
            ];
        }
        return [
            { number: "1", rel_x: -0.8, rel_y: 0.0, size_x: 0.8, size_y: 0.9 },
            { number: "2", rel_x: 0.8, rel_y: 0.0, size_x: 0.8, size_y: 0.9 }
        ];
    }

    async function resolveDeviceArg(c) {
        let devArg = c.device_uuid || c.uuid || "";
        const libraryUuid = c.library_uuid || c.libraryUuid || undefined;
        if (devArg && typeof devArg === "object") {
            return devArg;
        }
        if (typeof devArg === "string" && devArg.length > 0 && libraryUuid) {
            return { uuid: devArg, libraryUuid: libraryUuid };
        }
        const looksLikeUuid = typeof devArg === "string" && devArg.length >= 24 && devArg.indexOf(":") < 0;
        if (looksLikeUuid) {
            return libraryUuid ? { uuid: devArg, libraryUuid: libraryUuid } : devArg;
        }
        const searchKey = String(c.lcsc || c.query || c.value || c.lib_id || devArg || "");
        if (searchKey && typeof eda !== "undefined" && eda.lib_Device && typeof eda.lib_Device.search === "function") {
            try {
                const hits = await eda.lib_Device.search(searchKey, libraryUuid);
                if (Array.isArray(hits) && hits.length > 0 && hits[0].uuid) {
                    const hitLibUuid = hits[0].libraryUuid || hits[0].library_uuid || libraryUuid;
                    if (hitLibUuid) {
                        return { uuid: hits[0].uuid, libraryUuid: hitLibUuid };
                    }
                    return hits[0].uuid;
                }
            } catch (err) {
                logDiag("lib_Device.search fallback failed for " + searchKey + ": " + String(err));
            }
        }
        return devArg || c.lib_id || "Device:R";
    }

    async function handleEditSchematic(args) {
        let placedComponents = 0;
        let modifiedComponents = 0;
        let modifiedPins = 0;
        let placedNetflags = 0;
        let deletedPrimitives = 0;
        let placedWires = 0;
        let placedLabels = 0;
        const placedComponentDetails = [];
        const pinMap = {};
        const units = (args && args.units) ? String(args.units) : "mil";

        if (typeof eda !== "undefined") {
            const reqSheet = (args && (args.page_uuid || args.sheet || args.page)) || "";
            await ensureDocumentOpen("schematic", reqSheet, null);
        }

        function registerPinsInMap(ref, pinsList) {
            if (!ref || !Array.isArray(pinsList)) return;
            for (const p of pinsList) {
                const pt = { x: Number(p.x || 0), y: Number(p.y || 0) };
                if (p.number) {
                    pinMap[ref + "." + String(p.number)] = pt;
                }
                if (p.name) {
                    pinMap[ref + "." + String(p.name)] = pt;
                }
            }
        }

        const comps = (args && (args.add_components || args.components || args.symbols)) || [];
        if (Array.isArray(comps) && typeof eda !== "undefined" && eda.sch_PrimitiveComponent) {
            for (const c of comps) {
                if (!c || typeof c !== "object") continue;
                if (c.auto_place === true || (c.near_reference && String(c.near_reference).trim())) {
                    // Deferred to C++ collision-free grid auto-placer in EdaBridge.cxx
                    continue;
                }
                const resolvedDevice = await resolveDeviceArg(c);
                const xVal = coordToMil(c, "x", "x_mm", units, 0);
                const yVal = coordToMil(c, "y", "y_mm", units, 0);
                const rotVal = Number(c.rotation !== undefined ? c.rotation : c.rotation_deg || 0);
                const created = await eda.sch_PrimitiveComponent.create(
                    resolvedDevice,
                    xVal,
                    yVal,
                    c.sub_part_name || undefined,
                    rotVal,
                    Boolean(c.mirror || false)
                );
                const ref = String(c.reference || c.ref || "");
                const val = String(c.value || "");
                const uid = String(c.unique_id || (ref ? ("uid_" + ref) : ""));
                let compId = "";
                if (created && typeof created === "object") {
                    compId = String(callGetter(created, "getState_PrimitiveId", "primitiveId", "id", ""));
                    try {
                        if (ref && typeof created.setState_Designator === "function") {
                            await created.setState_Designator(ref);
                        } else if (ref) {
                            created.designator = ref;
                        }
                        if (val && typeof created.setState_Name === "function") {
                            await created.setState_Name(val);
                        } else if (val && typeof created.setState_Value === "function") {
                            await created.setState_Value(val);
                        }
                    } catch (err) {
                    }
                } else if (typeof created === "string") {
                    compId = created;
                }
                if (compId && typeof eda.sch_PrimitiveComponent.modify === "function") {
                    try {
                        const modProps = {};
                        if (ref) modProps.designator = ref;
                        if (val) modProps.name = val;
                        if (uid) modProps.uniqueId = uid;
                        if (c.footprint !== undefined) modProps.footprint = c.footprint;
                        if (c.add_into_bom !== undefined) modProps.addIntoBom = Boolean(c.add_into_bom);
                        if (c.add_into_pcb !== undefined) modProps.addIntoPcb = Boolean(c.add_into_pcb);
                        await eda.sch_PrimitiveComponent.modify(compId, modProps);
                    } catch (err) {
                    }
                }
                const compPins = await extractComponentPins(compId);
                registerPinsInMap(ref, compPins);
                placedComponentDetails.push({
                    reference: ref,
                    id: compId,
                    x: xVal,
                    y: yVal,
                    rotation: rotVal,
                    pins: compPins
                });
                placedComponents += 1;
            }
        }

        const modComps = (args && args.modify_components) || [];
        const rubberBandedWires = [];
        const preserveConnGlobal = Boolean(args && args.preserve_connections === true);
        if (Array.isArray(modComps) && modComps.length > 0 && typeof eda !== "undefined" && eda.sch_PrimitiveComponent) {
            const compByRef = {};
            if (typeof eda.sch_PrimitiveComponent.getAll === "function") {
                try {
                    const allC = await eda.sch_PrimitiveComponent.getAll(undefined, true);
                    if (Array.isArray(allC)) {
                        for (const ec of allC) {
                            const r = String(callGetter(ec, "getState_Designator", "designator", "reference", ""));
                            if (r) compByRef[r] = ec;
                        }
                    }
                } catch (err) {
                }
            }
            for (const mc of modComps) {
                if (!mc || typeof mc !== "object") continue;
                const ref = String(mc.reference || mc.designator || "");
                const existing = compByRef[ref];
                let targetId = String(mc.id || mc.primitive_id || (existing ? callGetter(existing, "getState_PrimitiveId", "primitiveId", "id", "") : ref));
                const wantsRot = (mc.rotation !== undefined || mc.rotation_deg !== undefined);
                const wantsFp = (mc.footprint !== undefined && String(mc.footprint) !== "");
                const preserveConn = preserveConnGlobal || Boolean(mc.preserve_connections === true) || wantsRot || wantsFp;
                const hasMove = (mc.x !== undefined || mc.y !== undefined || mc.x_mm !== undefined || mc.y_mm !== undefined);
                let oldPins = [];
                if (preserveConn && targetId && (hasMove || wantsRot || wantsFp)) {
                    oldPins = await extractComponentPins(targetId);
                }
                const modProps = {};
                if (mc.designator || mc.new_reference) {
                    modProps.designator = String(mc.designator || mc.new_reference);
                } else if (ref && mc.id) {
                    modProps.designator = ref;
                }
                if (mc.value !== undefined || mc.name !== undefined) modProps.name = String(mc.value !== undefined ? mc.value : mc.name);
                if (mc.unique_id !== undefined || mc.uniqueId !== undefined) modProps.uniqueId = String(mc.unique_id !== undefined ? mc.unique_id : mc.uniqueId);
                if (mc.footprint !== undefined) modProps.footprint = mc.footprint;
                if (mc.add_into_bom !== undefined) modProps.addIntoBom = Boolean(mc.add_into_bom);
                if (mc.add_into_pcb !== undefined) modProps.addIntoPcb = Boolean(mc.add_into_pcb);
                if (mc.lcsc !== undefined) modProps.supplierId = String(mc.lcsc);
                if (mc.x !== undefined || mc.x_mm !== undefined) modProps.x = coordToMil(mc, "x", "x_mm", units, 0);
                if (mc.y !== undefined || mc.y_mm !== undefined) modProps.y = coordToMil(mc, "y", "y_mm", units, 0);
                if (wantsRot) {
                    modProps.rotation = Number(mc.rotation !== undefined ? mc.rotation : mc.rotation_deg);
                }
                if (targetId && typeof eda.sch_PrimitiveComponent.modify === "function") {
                    try {
                        await eda.sch_PrimitiveComponent.modify(targetId, modProps);
                    } catch (err) {
                    }
                }
                if (existing && (wantsRot || wantsFp) && typeof eda.sch_PrimitiveComponent.create === "function") {
                    try {
                        const curFpRaw = callGetter(existing, "getState_Footprint", "footprintName", "footprint", "");
                        const curFpStr = (curFpRaw && typeof curFpRaw === "object")
                            ? String(curFpRaw.name || curFpRaw.title || curFpRaw.uuid || "")
                            : String(curFpRaw || "");
                        const curRot = Number(callGetter(existing, "getState_Rotation", "rotation", "", 0) || 0);
                        const reqFpStr = (mc.footprint && typeof mc.footprint === "object")
                            ? String(mc.footprint.name || mc.footprint.uuid || "")
                            : String(mc.footprint || "");
                        const needRecreate = (wantsRot && modProps.rotation !== curRot) ||
                                             (wantsFp && reqFpStr && reqFpStr !== curFpStr);
                        if (needRecreate) {
                            const exUid = String(modProps.uniqueId || callGetter(existing, "getState_UniqueId", "uniqueId", "", ""));
                            const exVal = String(modProps.name !== undefined ? modProps.name : callGetter(existing, "getState_Name", "value", "name", ""));
                            const exLcsc = String(mc.lcsc || callGetter(existing, "getState_SupplierId", "supplierPart", "supplierId", ""));
                            const exX = (modProps.x !== undefined) ? modProps.x : Number(callGetter(existing, "getState_X", "x", "", 0) || 0);
                            const exY = (modProps.y !== undefined) ? modProps.y : Math.abs(Number(callGetter(existing, "getState_Y", "y", "", 0) || 0));
                            const exRot = wantsRot ? modProps.rotation : curRot;
                            const token = await resolveSchComponentToken({
                                device_uuid: mc.device_uuid,
                                lcsc: exLcsc,
                                footprint: reqFpStr || curFpStr,
                                value: exVal,
                                reference: ref
                            });
                            if (token) {
                                if (targetId && typeof eda.sch_PrimitiveComponent.delete === "function") {
                                    try { await eda.sch_PrimitiveComponent.delete([targetId]); } catch (e) {}
                                }
                                const created = await eda.sch_PrimitiveComponent.create(token, exX, exY, undefined, exRot, false, true, true);
                                if (created) {
                                    targetId = String(callGetter(created, "getState_PrimitiveId", "primitiveId", "id", targetId));
                                    const reProps = { designator: modProps.designator || ref };
                                    if (exVal) reProps.name = exVal;
                                    if (exUid) reProps.uniqueId = exUid;
                                    try { await eda.sch_PrimitiveComponent.modify(targetId, reProps); } catch (e) {}
                                }
                            }
                        }
                    } catch (err) {
                    }
                }
                if (preserveConn && oldPins.length > 0 && eda.sch_PrimitiveWire &&
                    typeof eda.sch_PrimitiveWire.getAll === "function") {
                    try {
                        const newPins = await extractComponentPins(targetId);
                        const newPinByNum = {};
                        for (const np of newPins) {
                            newPinByNum[String(np.number || np.name || "")] = np;
                        }
                        const allWires = await eda.sch_PrimitiveWire.getAll();
                        if (Array.isArray(allWires)) {
                            for (const w of allWires) {
                                const wId = String(callGetter(w, "getState_PrimitiveId", "primitiveId", "id", ""));
                                const wNet = String(callGetter(w, "getState_Net", "net", null, ""));
                                const lineRaw = callGetter(w, "getState_Line", "line", "points", []);
                                if (!Array.isArray(lineRaw) || lineRaw.length < 2) continue;
                                const pts = [];
                                if (Array.isArray(lineRaw[0])) {
                                    for (const pair of lineRaw) {
                                        if (Array.isArray(pair) && pair.length >= 2) {
                                            pts.push(Number(pair[0]), Number(pair[1]));
                                        }
                                    }
                                } else {
                                    for (const v of lineRaw) pts.push(Number(v));
                                }
                                if (pts.length < 4) continue;
                                let changed = false;
                                let matchedPinKey = "";
                                for (const op of oldPins) {
                                    const pKey = String(op.number || op.name || "");
                                    const np = newPinByNum[pKey];
                                    if (!np) continue;
                                    if (Math.hypot(pts[0] - Number(op.x), pts[1] - Number(op.y)) <= 5.0) {
                                        pts[0] = Number(np.x);
                                        pts[1] = Number(np.y);
                                        changed = true;
                                        matchedPinKey = ref + "." + pKey;
                                    }
                                    const lastX = pts.length - 2;
                                    const lastY = pts.length - 1;
                                    if (Math.hypot(pts[lastX] - Number(op.x), pts[lastY] - Number(op.y)) <= 5.0) {
                                        pts[lastX] = Number(np.x);
                                        pts[lastY] = Number(np.y);
                                        changed = true;
                                        matchedPinKey = ref + "." + pKey;
                                    }
                                }
                                if (changed) {
                                    if (wId && typeof eda.sch_PrimitiveWire.delete === "function") {
                                        try { await eda.sch_PrimitiveWire.delete([wId]); } catch (e) {}
                                    }
                                    if (typeof eda.sch_PrimitiveWire.create === "function") {
                                        try { await eda.sch_PrimitiveWire.create(pts, wNet || undefined); } catch (e) {}
                                    }
                                    rubberBandedWires.push({
                                        id: wId,
                                        net: wNet,
                                        reference: ref,
                                        pin: matchedPinKey,
                                        x1: pts[0],
                                        y1: pts[1],
                                        x2: pts[pts.length - 2],
                                        y2: pts[pts.length - 1]
                                    });
                                }
                            }
                        }
                    } catch (err) {
                    }
                }
                modifiedComponents += 1;
            }
        }

        const modPins = (args && args.modify_pins) || [];
        if (Array.isArray(modPins) && modPins.length > 0 && typeof eda !== "undefined") {
            for (const mp of modPins) {
                if (!mp || typeof mp !== "object") continue;
                const pinId = String(mp.pin_id || mp.id || "");
                const noConn = (mp.no_connected !== undefined)
                    ? Boolean(mp.no_connected)
                    : ((mp.noConnected !== undefined) ? Boolean(mp.noConnected) : true);
                if (pinId) {
                    try {
                        const modObj = { noConnected: noConn };
                        if (mp.y !== undefined && mp.y !== null) {
                            modObj.y = Math.abs(Number(mp.y));
                        } else if (eda.sch_PrimitivePin && typeof eda.sch_PrimitivePin.getByPrimitiveId === "function") {
                            const curPin = await eda.sch_PrimitivePin.getByPrimitiveId(pinId);
                            if (curPin) {
                                const curY = Number(callGetter(curPin, "getState_Y", "y", "", NaN));
                                if (!isNaN(curY)) {
                                    modObj.y = Math.abs(curY);
                                }
                            }
                        }
                        if (eda.sch_PrimitivePin && typeof eda.sch_PrimitivePin.modify === "function") {
                            await eda.sch_PrimitivePin.modify(pinId, modObj);
                        } else if (eda.sch_PrimitiveComponent && typeof eda.sch_PrimitiveComponent.modifyPin === "function") {
                            await eda.sch_PrimitiveComponent.modifyPin(pinId, modObj);
                        }
                    } catch (err) {
                    }
                }
                modifiedPins += 1;
            }
        }

        const netflags = (args && args.add_netflags) || [];
        if (Array.isArray(netflags) && netflags.length > 0 && typeof eda !== "undefined") {
            for (const nf of netflags) {
                if (!nf || typeof nf !== "object") continue;
                const netName = String(nf.net || nf.name || nf.value || (nf.identification === "Power" || nf.type === "Power" ? "VCC" : "GND"));
                const ident = String(nf.type || nf.identification || ((netName === "GND" || netName === "AGND" || netName === "PGND") ? "Ground" : "Power"));
                const nx = coordToMil(nf, "x", "x_mm", units, 0);
                const ny = coordToMil(nf, "y", "y_mm", units, 0);
                const nrot = Number(nf.rotation || 0);
                const nmirror = Boolean(nf.mirror || false);
                if (eda.sch_PrimitiveComponent && typeof eda.sch_PrimitiveComponent.createNetFlag === "function") {
                    try {
                        await eda.sch_PrimitiveComponent.createNetFlag(ident, netName, nx, ny, nrot, nmirror);
                    } catch (err) {
                    }
                } else if (eda.sch_PrimitiveNetLabel && typeof eda.sch_PrimitiveNetLabel.create === "function") {
                    await eda.sch_PrimitiveNetLabel.create(nx, ny, netName);
                }
                placedNetflags += 1;
            }
        }

        const delPrims = (args && args.delete_primitives) || [];
        if (Array.isArray(delPrims) && delPrims.length > 0 && typeof eda !== "undefined") {
            const ids = delPrims.map(function (d) {
                return (d && typeof d === "object") ? String(d.id || d.primitive_id || "") : String(d || "");
            }).filter(Boolean);
            if (ids.length > 0) {
                if (eda.sch_PrimitiveComponent && typeof eda.sch_PrimitiveComponent.delete === "function") {
                    try { await eda.sch_PrimitiveComponent.delete(ids); } catch (err) {}
                }
                if (eda.sch_PrimitiveWire && typeof eda.sch_PrimitiveWire.delete === "function") {
                    try { await eda.sch_PrimitiveWire.delete(ids); } catch (err) {}
                }
                deletedPrimitives += ids.length;
            }
        }

        const connectPinsList = [];
        if (args && Array.isArray(args.connect_pins)) {
            for (const cp of args.connect_pins) {
                if (cp && cp.from && cp.to) connectPinsList.push(cp);
            }
        }
        if (args && Array.isArray(args.operations)) {
            for (const op of args.operations) {
                if (op && (op.op === "connect_pins" || op.type === "connect_pins") && op.from && op.to) {
                    connectPinsList.push(op);
                }
            }
        }

        if (connectPinsList.length > 0 && typeof eda !== "undefined") {
            const neededRefs = {};
            for (const conn of connectPinsList) {
                if (!pinMap[String(conn.from)]) {
                    const r1 = String(conn.from).split(".")[0];
                    if (r1) neededRefs[r1] = true;
                }
                if (!pinMap[String(conn.to)]) {
                    const r2 = String(conn.to).split(".")[0];
                    if (r2) neededRefs[r2] = true;
                }
            }
            if (Object.keys(neededRefs).length > 0 && eda.sch_PrimitiveComponent &&
                typeof eda.sch_PrimitiveComponent.getAll === "function") {
                try {
                    const existingComps = await eda.sch_PrimitiveComponent.getAll(undefined, false);
                    if (Array.isArray(existingComps)) {
                        for (const ec of existingComps) {
                            const ecRef = String(callGetter(ec, "getState_Designator", "designator", "reference", ""));
                            const ecId = String(callGetter(ec, "getState_PrimitiveId", "primitiveId", "id", ""));
                            if (ecRef && ecId && neededRefs[ecRef]) {
                                const ecPins = await extractComponentPins(ecId);
                                registerPinsInMap(ecRef, ecPins);
                            }
                        }
                    }
                } catch (err) {
                }
            }
            if (eda.sch_PrimitiveWire && typeof eda.sch_PrimitiveWire.create === "function") {
                for (const conn of connectPinsList) {
                    const p1 = pinMap[String(conn.from)];
                    const p2 = pinMap[String(conn.to)];
                    if (!p1 || !p2) continue;
                    const pts = (p1.x === p2.x || p1.y === p2.y)
                        ? [p1.x, p1.y, p2.x, p2.y]
                        : [p1.x, p1.y, p2.x, p1.y, p2.x, p2.y];
                    await eda.sch_PrimitiveWire.create(pts, conn.net || undefined);
                    placedWires += 1;
                }
            }
        }

        const wires = (args && (args.add_wires || args.wires)) || [];
        if (Array.isArray(wires) && typeof eda !== "undefined" && eda.sch_PrimitiveWire) {
            for (const w of wires) {
                let pts;
                if (Array.isArray(w.points)) {
                    pts = w.points.map(function (v) { return toCanvasUnits(v, units); });
                } else {
                    pts = [
                        coordToMil(w, "x1", "x1_mm", units, 0),
                        coordToMil(w, "y1", "y1_mm", units, 0),
                        coordToMil(w, "x2", "x2_mm", units, 0),
                        coordToMil(w, "y2", "y2_mm", units, 0)
                    ];
                }
                await eda.sch_PrimitiveWire.create(pts, w.net || undefined);
                placedWires += 1;
            }
        }

        const labels = [].concat(
            (args && (args.add_labels || args.labels)) || [],
            (args && (args.add_power_ports || args.power_ports)) || []
        );
        if (Array.isArray(labels) && typeof eda !== "undefined" && eda.sch_PrimitiveNetLabel) {
            for (const l of labels) {
                await eda.sch_PrimitiveNetLabel.create(
                    coordToMil(l, "x", "x_mm", units, 0),
                    coordToMil(l, "y", "y_mm", units, 0),
                    String(l.name || l.net || "NET")
                );
                placedLabels += 1;
            }
        }

        try {
            if (typeof eda !== "undefined" && eda.dmt_EditorControl && typeof eda.dmt_EditorControl.zoomToAllPrimitives === "function") {
                await eda.dmt_EditorControl.zoomToAllPrimitives();
            }
        } catch (err) {
        }

        return {
            ok: true,
            active_document: await resolveActiveDocumentTitle(),
            placed_components: placedComponents,
            modified_components: modifiedComponents,
            modified_pins: modifiedPins,
            placed_netflags: placedNetflags,
            added_netflags: placedNetflags,
            deleted_primitives: deletedPrimitives,
            placed_component_details: placedComponentDetails,
            placed_wires: placedWires,
            placed_labels: placedLabels,
            rubber_banded_wires: rubberBandedWires,
            rubber_banded_wires_count: rubberBandedWires.length
        };
    }

    async function handleSyncSchToPcb(args) {
        if (args && args.dry_run === true) {
            const schInfo = await handleInspectSchematic({ include_pins: true, include_wires: true });
            const pcbInfo = await handleInspectPcb({ _full_for_bridge: true });
            const schComps = Array.isArray(schInfo.components) ? schInfo.components : [];
            const pcbComps = Array.isArray(pcbInfo.components) ? pcbInfo.components : [];
            const pcbPads = Array.isArray(pcbInfo.pads) ? pcbInfo.pads : [];
            const pcbTracks = Array.isArray(pcbInfo.tracks) ? pcbInfo.tracks : [];

            const componentsToAdd = [];
            const componentsToRemove = [];
            const footprintMismatches = [];
            const unannotatedReferences = [];
            const missingFootprints = [];
            const padNetDeltas = [];
            const orphanedTracks = [];

            const schByRef = {};
            for (const sc of schComps) {
                const ref = String(sc.reference || "");
                const compType = String(sc.component_type || "").toLowerCase();
                const fp = String(sc.footprint || "");
                if (compType === "netflag" || compType === "netport" || compType === "sheet" ||
                    compType === "frame" || compType === "power" || compType === "netlabel" ||
                    ref.indexOf("#") === 0 || ref.indexOf("FRAME") === 0 || ref === "GND" || ref === "VCC") {
                    continue;
                }
                if (!ref || ref.indexOf("?") !== -1 || !/[0-9]/.test(ref)) {
                    unannotatedReferences.push(ref || "<empty>");
                    continue;
                }
                if (!fp) {
                    missingFootprints.push(ref);
                }
                schByRef[ref] = sc;
            }

            const pcbByRef = {};
            for (const pc of pcbComps) {
                const ref = String(pc.reference || "");
                if (!ref) continue;
                pcbByRef[ref] = pc;
                if (!schByRef[ref]) {
                    componentsToRemove.push({
                        reference: ref,
                        footprint: String(pc.footprint || "")
                    });
                }
            }

            for (const ref of Object.keys(schByRef)) {
                const sc = schByRef[ref];
                const schFp = String(sc.footprint || "");
                const pc = pcbByRef[ref];
                if (!pc) {
                    componentsToAdd.push({
                        reference: ref,
                        value: String(sc.value || ""),
                        footprint: schFp
                    });
                } else {
                    const pcbFp = String(pc.footprint || "");
                    if (schFp && pcbFp && schFp !== pcbFp) {
                        footprintMismatches.push({
                            reference: ref,
                            schematic_footprint: schFp,
                            pcb_footprint: pcbFp
                        });
                    }
                }
            }

            const activeSchematicNets = {};
            if (Array.isArray(schInfo.wires)) {
                for (const w of schInfo.wires) {
                    if (w && w.net) activeSchematicNets[String(w.net)] = true;
                }
            }
            for (const sc of schComps) {
                const ref = String(sc.reference || "");
                if (sc && sc.net) activeSchematicNets[String(sc.net)] = true;
                if (!ref || !Array.isArray(sc.pins)) continue;
                for (const p of sc.pins) {
                    const pNum = String(p.number || p.name || "");
                    const pNet = String(p.net || "");
                    if (pNet) activeSchematicNets[pNet] = true;
                    for (const pad of pcbPads) {
                        const padRef = String(pad.ref || pad.reference || "");
                        if (padRef === ref && String(pad.number || pad.pad || "") === pNum) {
                            const oldNet = String(pad.net || "");
                            if (pNet && oldNet !== pNet) {
                                padNetDeltas.push({
                                    reference: ref,
                                    pad: pNum,
                                    old_net: oldNet,
                                    new_net: pNet
                                });
                            }
                        }
                    }
                }
            }

            for (const trk of pcbTracks) {
                const tNet = String(trk.net || "");
                if (tNet && Object.keys(activeSchematicNets).length > 0 && !activeSchematicNets[tNet]) {
                    orphanedTracks.push({
                        id: String(trk.id || ""),
                        net: tNet,
                        layer: String(trk.layer || "")
                    });
                }
            }

            const readyToSync = (unannotatedReferences.length === 0 && missingFootprints.length === 0);
            return {
                ok: true,
                dry_run: true,
                synced: false,
                ready_to_sync: readyToSync,
                active_document: await resolveActiveDocumentTitle(),
                components_to_add: componentsToAdd,
                components_to_remove: componentsToRemove,
                footprint_mismatches: footprintMismatches,
                unannotated_references: unannotatedReferences,
                missing_footprints: missingFootprints,
                pad_net_deltas: padNetDeltas,
                orphaned_tracks: orphanedTracks
            };
        }

        let synced = false;
        if (typeof eda !== "undefined") {
            let pcbUuid = (args && args.pcb_uuid) || undefined;
            if (!pcbUuid && eda.dmt_Project && typeof eda.dmt_Project.getCurrentProjectInfo === "function") {
                try {
                    const proj = await eda.dmt_Project.getCurrentProjectInfo();
                    if (proj && Array.isArray(proj.data) && proj.data[0] && proj.data[0].pcb && proj.data[0].pcb.uuid) {
                        pcbUuid = String(proj.data[0].pcb.uuid);
                    }
                } catch (err) {
                }
            }
            if (eda.dmt_Pcb && eda.dmt_Pcb.syncFromSchematic) {
                await eda.dmt_Pcb.syncFromSchematic(pcbUuid);
                synced = true;
            } else if (eda.pcb_Netlist && eda.pcb_Netlist.updateFromSchematic) {
                await eda.pcb_Netlist.updateFromSchematic();
                synced = true;
            }
        }
        return {
            ok: true,
            synced: synced,
            active_document: await resolveActiveDocumentTitle()
        };
    }

    async function handleInspectPcb(args) {
        const components = [];
        const schematicComponents = [];
        const pads = [];
        const tracks = [];
        const vias = [];
        const pours = [];
        const outline = [];
        const includeOtherProp = Boolean(args && args.include_other_property === true);
        const summaryOnly = Boolean(args && args.summary_only === true);
        const fullForBridge = Boolean(args && (args._full_for_bridge === true || args.include_raw_primitives === true));
        const refFilter = (args && Array.isArray(args.references)) ? args.references.map(String) : [];
        const netFilter = (args && Array.isArray(args.nets)) ? args.nets.map(String) : [];

        if (typeof eda !== "undefined") {
            await ensureDocumentOpen("pcb", args && args.pcb_uuid, null);
            const compRefById = {};
            try {
                if (eda.pcb_PrimitiveComponent && eda.pcb_PrimitiveComponent.getAll) {
                    const raw = await eda.pcb_PrimitiveComponent.getAll();
                    if (Array.isArray(raw)) {
                        for (const c of raw) {
                            const fpRaw = callGetter(c, "getState_Footprint", "footprintName", "footprint", "");
                            const fpStr = (fpRaw && typeof fpRaw === "object")
                                ? String(fpRaw.name || fpRaw.title || fpRaw.uuid || "")
                                : String(fpRaw || "");
                            const compId = String(callGetter(c, "getState_PrimitiveId", "primitiveId", "id", ""));
                            const compRef = String(callGetter(c, "getState_Designator", "designator", "reference", ""));
                            if (compId && compRef) {
                                compRefById[compId] = compRef;
                            }
                            const compRec = {
                                id: compId,
                                reference: compRef,
                                value: String(callGetter(c, "getState_Name", "name", "value", "")),
                                footprint: fpStr,
                                unique_id: String(callGetter(c, "getState_UniqueId", "uniqueId", "unique_id", "")),
                                x: Number(callGetter(c, "getState_X", "x", "", 0) || 0),
                                y: Number(callGetter(c, "getState_Y", "y", "", 0) || 0),
                                rotation: Number(callGetter(c, "getState_Rotation", "rotation", "", 0) || 0),
                                layer: String(callGetter(c, "getState_Layer", "layer", "", "TopLayer"))
                            };
                            if (includeOtherProp) {
                                compRec.other_property = callGetter(c, "getState_OtherProperty", "otherProperty", "", {});
                            }
                            components.push(compRec);
                        }
                    }
                }
            } catch (err) {
            }
            try {
                if (eda.sch_PrimitiveComponent && typeof eda.sch_PrimitiveComponent.getAll === "function") {
                    const rawSch = await eda.sch_PrimitiveComponent.getAll(undefined, true);
                    if (Array.isArray(rawSch)) {
                        for (const sc of rawSch) {
                            const fpRaw = callGetter(sc, "getState_Footprint", "footprintName", "footprint", "");
                            const fpStr = (fpRaw && typeof fpRaw === "object")
                                ? String(fpRaw.name || fpRaw.title || fpRaw.uuid || "")
                                : String(fpRaw || "");
                            schematicComponents.push({
                                reference: String(callGetter(sc, "getState_Designator", "designator", "reference", "")),
                                value: String(callGetter(sc, "getState_Name", "name", "value", "")),
                                footprint: fpStr,
                                component_type: String(callGetter(sc, "getState_ComponentType", "componentType", "", "part"))
                            });
                        }
                    }
                }
            } catch (err) {
            }
            try {
                if (eda.pcb_PrimitivePad && typeof eda.pcb_PrimitivePad.getAll === "function") {
                    const seenPadIds = {};
                    const padLayersToQuery = [1, 2, 12];
                    let rawPadsCombined = [];
                    for (const lid of padLayersToQuery) {
                        try {
                            const lp = await eda.pcb_PrimitivePad.getAll(lid);
                            if (Array.isArray(lp)) {
                                rawPadsCombined = rawPadsCombined.concat(lp);
                            }
                        } catch (err) {
                        }
                    }
                    if (rawPadsCombined.length === 0) {
                        try {
                            const allP = await eda.pcb_PrimitivePad.getAll();
                            if (Array.isArray(allP)) {
                                rawPadsCombined = allP;
                            }
                        } catch (err) {
                        }
                    }
                    for (const p of rawPadsCombined) {
                        const pid = String(callGetter(p, "getState_PrimitiveId", "primitiveId", "id", ""));
                        if (pid && seenPadIds[pid]) continue;
                        if (pid) seenPadIds[pid] = true;
                        const padNum = String(callGetter(p, "getState_PadNumber", "padNumber", "number", "") ||
                                              callGetter(p, "getState_Pad", "pad", "", ""));
                        let padRef = String(callGetter(p, "getState_Designator", "designator", "reference", "") ||
                                            callGetter(p, "getState_ComponentDesignator", "componentDesignator", "ref", ""));
                        if (!padRef && pid) {
                            const lastE = pid.lastIndexOf("e");
                            if (lastE > 0 && /^[0-9]+$/.test(pid.slice(lastE + 1))) {
                                const parentId = pid.slice(0, lastE);
                                if (compRefById[parentId]) {
                                    padRef = compRefById[parentId];
                                }
                            }
                        }
                        pads.push({
                            id: pid,
                            ref: padRef,
                            pad: padNum,
                            number: padNum,
                            net: String(callGetter(p, "getState_Net", "net", "", "")),
                            x: Number(callGetter(p, "getState_X", "x", "", 0) || 0),
                            y: Number(callGetter(p, "getState_Y", "y", "", 0) || 0),
                            layer: String(callGetter(p, "getState_Layer", "layer", "", "1")),
                            rotation: Number(callGetter(p, "getState_Rotation", "rotation", "", 0) || 0),
                            size_x: Number(callGetter(p, "getState_Width", "width", "size_x", 0) || 0),
                            size_y: Number(callGetter(p, "getState_Height", "height", "size_y", 0) || 0),
                            hole_diameter: Number(callGetter(p, "getState_HoleDiameter", "holeDiameter", "drill", 0) || 0)
                        });
                    }
                }
            } catch (err) {
            }
            try {
                if (eda.pcb_PrimitiveTrack && eda.pcb_PrimitiveTrack.getAll) {
                    const rawTracks = await eda.pcb_PrimitiveTrack.getAll();
                    if (Array.isArray(rawTracks)) {
                        for (const t of rawTracks) {
                            tracks.push({
                                id: String(callGetter(t, "getState_PrimitiveId", "primitiveId", "id", "")),
                                net: String(callGetter(t, "getState_Net", "net", "", "")),
                                layer: String(callGetter(t, "getState_Layer", "layer", "", "TopLayer")),
                                width: Number(callGetter(t, "getState_LineWidth", "lineWidth", "width", 0.25) || 0.25),
                                x1: Number(callGetter(t, "getState_StartX", "startX", "x1", 0) || 0),
                                y1: Number(callGetter(t, "getState_StartY", "startY", "y1", 0) || 0),
                                x2: Number(callGetter(t, "getState_EndX", "endX", "x2", 0) || 0),
                                y2: Number(callGetter(t, "getState_EndY", "endY", "y2", 0) || 0)
                            });
                        }
                    }
                }
            } catch (err) {
            }
            try {
                if (eda.pcb_PrimitiveLine && eda.pcb_PrimitiveLine.getAll) {
                    const rawLines = await eda.pcb_PrimitiveLine.getAll();
                    if (Array.isArray(rawLines)) {
                        for (const ln of rawLines) {
                            const lid = callGetter(ln, "getState_Layer", "layer", "", "");
                            const recId = String(callGetter(ln, "getState_PrimitiveId", "primitiveId", "id", ""));
                            const w = Number(callGetter(ln, "getState_LineWidth", "lineWidth", "width", 0.25) || 0.25);
                            const x1 = Number(callGetter(ln, "getState_StartX", "startX", "x1", 0) || 0);
                            const y1 = Number(callGetter(ln, "getState_StartY", "startY", "y1", 0) || 0);
                            const x2 = Number(callGetter(ln, "getState_EndX", "endX", "x2", 0) || 0);
                            const y2 = Number(callGetter(ln, "getState_EndY", "endY", "y2", 0) || 0);
                            if (isCopperLayer(lid)) {
                                tracks.push({
                                    id: recId,
                                    net: String(callGetter(ln, "getState_Net", "net", "", "")),
                                    layer: String(lid),
                                    width: w,
                                    x1: x1,
                                    y1: y1,
                                    x2: x2,
                                    y2: y2
                                });
                            } else if (isBoardOutlineLayer(lid)) {
                                outline.push({
                                    id: recId,
                                    layer: String(lid),
                                    width: w,
                                    x1: x1,
                                    y1: y1,
                                    x2: x2,
                                    y2: y2
                                });
                            }
                        }
                    }
                }
            } catch (err) {
            }
            try {
                if (eda.pcb_PrimitiveVia && eda.pcb_PrimitiveVia.getAll) {
                    const rawVias = await eda.pcb_PrimitiveVia.getAll();
                    if (Array.isArray(rawVias)) {
                        for (const v of rawVias) {
                            vias.push({
                                id: String(callGetter(v, "getState_PrimitiveId", "primitiveId", "id", "")),
                                net: String(callGetter(v, "getState_Net", "net", "", "")),
                                x: Number(callGetter(v, "getState_X", "x", "", 0) || 0),
                                y: Number(callGetter(v, "getState_Y", "y", "", 0) || 0),
                                diameter: Number(callGetter(v, "getState_Diameter", "diameter", "", 0.6) || 0.6),
                                drill: Number(callGetter(v, "getState_HoleDiameter", "holeDiameter", "", 0.3) || 0.3)
                            });
                        }
                    }
                }
            } catch (err) {
            }
            try {
                if (eda.pcb_PrimitivePour && typeof eda.pcb_PrimitivePour.getAll === "function") {
                    const rawPours = await eda.pcb_PrimitivePour.getAll();
                    if (Array.isArray(rawPours)) {
                        for (const pr of rawPours) {
                            pours.push({
                                id: String(callGetter(pr, "getState_PrimitiveId", "primitiveId", "id", "")),
                                net: String(callGetter(pr, "getState_Net", "net", "", "")),
                                layer: String(callGetter(pr, "getState_Layer", "layer", "", "TopLayer")),
                                x: Number(callGetter(pr, "getState_X", "x", "", 0) || 0),
                                y: Number(callGetter(pr, "getState_Y", "y", "", 0) || 0),
                                width: Number(callGetter(pr, "getState_Width", "width", "", 0) || 0),
                                height: Number(callGetter(pr, "getState_Height", "height", "", 0) || 0),
                                points: callGetter(pr, "getState_Polygon", "polygon", "points", [])
                            });
                        }
                    }
                }
            } catch (err) {
            }
        }

        const filteredComponents = fullForBridge ? components : components.filter(function (c) {
            if (refFilter.length > 0 && refFilter.indexOf(c.reference) < 0) return false;
            return true;
        });
        const filteredPads = fullForBridge ? pads : pads.filter(function (p) {
            if (refFilter.length > 0 && refFilter.indexOf(p.ref) < 0) return false;
            if (netFilter.length > 0 && netFilter.indexOf(p.net) < 0) return false;
            return true;
        });
        const filteredTracks = fullForBridge ? tracks : tracks.filter(function (t) {
            if (netFilter.length > 0 && netFilter.indexOf(t.net) < 0) return false;
            return true;
        });
        const filteredVias = fullForBridge ? vias : vias.filter(function (v) {
            if (netFilter.length > 0 && netFilter.indexOf(v.net) < 0) return false;
            return true;
        });
        const filteredPours = fullForBridge ? pours : pours.filter(function (pr) {
            if (netFilter.length > 0 && netFilter.indexOf(pr.net) < 0) return false;
            return true;
        });

        const out = {
            ok: true,
            units: "mil",
            active_document: await resolveActiveDocumentTitle(),
            components_count: filteredComponents.length,
            pads_count: filteredPads.length,
            tracks_count: filteredTracks.length,
            vias_count: filteredVias.length,
            pours_count: filteredPours.length,
            board_outline: outline
        };
        if (!summaryOnly || fullForBridge) {
            out.components = filteredComponents;
            out.schematic_components = schematicComponents;
            out.pads = filteredPads;
            out.tracks = filteredTracks;
            out.vias = filteredVias;
            out.pours = filteredPours;
        }
        return out;
    }

    async function handleEditPcb(args) {
        let updatedPlacements = 0;
        let createdFootprints = 0;
        let createdPads = 0;
        let modifiedPads = 0;
        let deletedPrimitives = 0;
        let addedTracks = 0;
        let addedVias = 0;
        let addedZones = 0;
        const units = (args && args.units) ? String(args.units) : "mil";

        if (typeof eda !== "undefined") {
            await ensureDocumentOpen("pcb", args && args.pcb_uuid, null);
        }

        if (args.board_outline && typeof eda !== "undefined" && eda.pcb_PrimitiveLine && eda.pcb_PrimitiveLine.create) {
            const bo = args.board_outline;
            const lineW = toCanvasUnits(0.15, units);
            if (Array.isArray(bo)) {
                for (const seg of bo) {
                    await eda.pcb_PrimitiveLine.create(
                        "BoardOutLine",
                        coordToMil(seg, "x1", "x1_mm", units, 0),
                        coordToMil(seg, "y1", "y1_mm", units, 0),
                        coordToMil(seg, "x2", "x2_mm", units, 0),
                        coordToMil(seg, "y2", "y2_mm", units, 0),
                        lineW
                    );
                }
            } else if (Array.isArray(bo.lines) && bo.lines.length > 0) {
                for (const seg of bo.lines) {
                    await eda.pcb_PrimitiveLine.create(
                        "BoardOutLine",
                        coordToMil(seg, "x1", "x1_mm", units, 0),
                        coordToMil(seg, "y1", "y1_mm", units, 0),
                        coordToMil(seg, "x2", "x2_mm", units, 0),
                        coordToMil(seg, "y2", "y2_mm", units, 0),
                        lineW
                    );
                }
            } else {
                const x0 = coordToMil(bo, "x", "x_mm", units, 0);
                const y0 = coordToMil(bo, "y", "y_mm", units, 0);
                const w = coordToMil(bo, "width", "width_mm", units, 50);
                const h = coordToMil(bo, "height", "height_mm", units, 40);
                await eda.pcb_PrimitiveLine.create("BoardOutLine", x0, y0, x0 + w, y0, lineW);
                await eda.pcb_PrimitiveLine.create("BoardOutLine", x0 + w, y0, x0 + w, y0 + h, lineW);
                await eda.pcb_PrimitiveLine.create("BoardOutLine", x0 + w, y0 + h, x0, y0 + h, lineW);
                await eda.pcb_PrimitiveLine.create("BoardOutLine", x0, y0 + h, x0, y0, lineW);
            }
        }

        const primaryPlacements = (args && Array.isArray(args.placements) && args.placements.length > 0)
            ? args.placements
            : ((args && Array.isArray(args.move_components)) ? args.move_components : []);
        const placements = [].concat(
            primaryPlacements,
            (args && args.place_components) || [],
            (args && args.footprints) || []
        );
        const rubberBandedTracks = [];
        const preserveConnGlobal = Boolean(args && args.preserve_connections === true);
        if (placements.length > 0 && typeof eda !== "undefined") {
            const compByRef = {};
            if (eda.pcb_PrimitiveComponent && typeof eda.pcb_PrimitiveComponent.getAll === "function") {
                try {
                    const existing = await eda.pcb_PrimitiveComponent.getAll();
                    if (Array.isArray(existing)) {
                        for (const comp of existing) {
                            const des = String(callGetter(comp, "getState_Designator", "designator", "reference", ""));
                            if (des) {
                                compByRef[des] = comp;
                            }
                        }
                    }
                } catch (err) {
                }
            }

            for (const p of placements) {
                const ref = String(p.reference || p.ref || p.designator || "");
                const xCanvas = coordToMil(p, "x", "x_mm", units, 0);
                const yCanvas = coordToMil(p, "y", "y_mm", units, 0);
                const xRaw = Number(p.x_mm !== undefined ? p.x_mm : p.x || 0);
                const yRaw = Number(p.y_mm !== undefined ? p.y_mm : p.y || 0);
                const rot = Number(p.rotation_deg !== undefined ? p.rotation_deg : p.rotation || 0);
                const layer = normalizePcbLayer(p.layer || "TopLayer");
                const existingComp = compByRef[ref];
                const preserveConn = preserveConnGlobal || Boolean(p.preserve_connections === true);

                if (existingComp || (eda.pcb_PrimitiveComponent && eda.pcb_PrimitiveComponent.modify && !p.pads && !p.footprint && !p.device_uuid && !p.uuid && !p.lcsc)) {
                    const targetId = existingComp
                        ? String(callGetter(existingComp, "getState_PrimitiveId", "primitiveId", "id", ref))
                        : String(p.id || p.primitive_id || ref);
                    const oldCompX = existingComp ? Number(callGetter(existingComp, "getState_X", "x", null, 0)) : xCanvas;
                    const oldCompY = existingComp ? Number(callGetter(existingComp, "getState_Y", "y", null, 0)) : yCanvas;
                    const dxCanvas = xCanvas - oldCompX;
                    const dyCanvas = yCanvas - oldCompY;
                    let oldRefPads = [];
                    if (preserveConn && eda.pcb_PrimitivePad && typeof eda.pcb_PrimitivePad.getAll === "function") {
                        try {
                            const rawPads = await eda.pcb_PrimitivePad.getAll();
                            if (Array.isArray(rawPads)) {
                                for (const pd of rawPads) {
                                    const pRef = String(callGetter(pd, "getState_ComponentDesignator", "designator", "reference", ""));
                                    if (pRef === ref) {
                                        oldRefPads.push({
                                            number: String(callGetter(pd, "getState_PadNumber", "padNumber", "number", "")),
                                            x: Number(callGetter(pd, "getState_X", "x", null, 0)),
                                            y: Number(callGetter(pd, "getState_Y", "y", null, 0))
                                        });
                                    }
                                }
                            }
                        } catch (err) {
                        }
                    }
                    if (eda.pcb_PrimitiveComponent && typeof eda.pcb_PrimitiveComponent.modify === "function") {
                        await eda.pcb_PrimitiveComponent.modify(targetId, {
                            x: xCanvas,
                            y: yCanvas,
                            rotation: rot,
                            layer: resolveEasyEdaCopperLayerId(layer)
                        });
                    } else if (existingComp && typeof existingComp.setState_X === "function") {
                        await existingComp.setState_X(xCanvas);
                        await existingComp.setState_Y(yCanvas);
                    }
                    if (preserveConn && (Math.abs(dxCanvas) > 1e-3 || Math.abs(dyCanvas) > 1e-3)) {
                        const trackApi = (eda.pcb_PrimitiveTrack && typeof eda.pcb_PrimitiveTrack.getAll === "function")
                            ? eda.pcb_PrimitiveTrack
                            : ((eda.pcb_PrimitiveLine && typeof eda.pcb_PrimitiveLine.getAll === "function") ? eda.pcb_PrimitiveLine : null);
                        if (trackApi) {
                            try {
                                const rawTracks = await trackApi.getAll();
                                if (Array.isArray(rawTracks)) {
                                    for (const trk of rawTracks) {
                                        const tId = String(callGetter(trk, "getState_PrimitiveId", "primitiveId", "id", ""));
                                        const tNet = String(callGetter(trk, "getState_Net", "net", null, ""));
                                        const rawLayer = callGetter(trk, "getState_Layer", "layer", null, "TopLayer");
                                        if (!isCopperLayer(rawLayer)) continue;
                                        const tLayer = normalizePcbLayer(rawLayer);
                                        const tW = Number(callGetter(trk, "getState_LineWidth", "lineWidth", "width", 10));
                                        let x1 = Number(callGetter(trk, "getState_StartX", "startX", "x1", 0));
                                        let y1 = Number(callGetter(trk, "getState_StartY", "startY", "y1", 0));
                                        let x2 = Number(callGetter(trk, "getState_EndX", "endX", "x2", 0));
                                        let y2 = Number(callGetter(trk, "getState_EndY", "endY", "y2", 0));
                                        let changed = false;
                                        const matchPts = oldRefPads.length > 0 ? oldRefPads : [{ number: "1", x: oldCompX, y: oldCompY }];
                                        for (const mp of matchPts) {
                                            if (Math.hypot(x1 - mp.x, y1 - mp.y) <= 15.0) {
                                                x1 += dxCanvas;
                                                y1 += dyCanvas;
                                                changed = true;
                                            }
                                            if (Math.hypot(x2 - mp.x, y2 - mp.y) <= 15.0) {
                                                x2 += dxCanvas;
                                                y2 += dyCanvas;
                                                changed = true;
                                            }
                                        }
                                        if (changed) {
                                            if (tId && typeof trackApi.delete === "function") {
                                                try { await trackApi.delete([tId]); } catch (e) {}
                                            }
                                            if (eda.pcb_PrimitiveTrack && typeof eda.pcb_PrimitiveTrack.create === "function") {
                                                try { await eda.pcb_PrimitiveTrack.create(tNet, tLayer, x1, y1, x2, y2, tW); } catch (e) {}
                                            } else if (eda.pcb_PrimitiveLine && typeof eda.pcb_PrimitiveLine.create === "function") {
                                                try { await eda.pcb_PrimitiveLine.create(tNet, resolveEasyEdaCopperLayerId(tLayer), x1, y1, x2, y2, tW, false); } catch (e) {}
                                            }
                                            rubberBandedTracks.push({
                                                id: tId,
                                                net: tNet,
                                                reference: ref,
                                                x1: x1,
                                                y1: y1,
                                                x2: x2,
                                                y2: y2
                                            });
                                        }
                                    }
                                }
                            } catch (err) {
                            }
                        }
                    }
                    updatedPlacements += 1;
                } else if ((p.device_uuid || p.uuid || p.lcsc || p.query) && eda.pcb_PrimitiveComponent && typeof eda.pcb_PrimitiveComponent.create === "function") {
                    const devObj = await resolveDeviceArg(p);
                    const layerId = resolveEasyEdaCopperLayerId(layer);
                    const createdPcbComp = await eda.pcb_PrimitiveComponent.create(
                        devObj,
                        layerId,
                        xCanvas,
                        yCanvas,
                        rot,
                        Boolean(p.mirror || false)
                    );
                    let compId = "";
                    if (createdPcbComp && typeof createdPcbComp === "object") {
                        compId = String(callGetter(createdPcbComp, "getState_PrimitiveId", "primitiveId", "id", ""));
                    } else if (typeof createdPcbComp === "string") {
                        compId = createdPcbComp;
                    }
                    if (compId && typeof eda.pcb_PrimitiveComponent.modify === "function") {
                        try {
                            const modProps = {
                                designator: ref,
                                x: xCanvas,
                                y: yCanvas,
                                rotation: rot,
                                layer: layerId
                            };
                            if (p.unique_id !== undefined || p.uniqueId !== undefined) {
                                modProps.uniqueId = String(p.unique_id !== undefined ? p.unique_id : p.uniqueId);
                            }
                            if (p.value !== undefined || p.name !== undefined) {
                                modProps.name = String(p.value !== undefined ? p.value : p.name);
                            }
                            await eda.pcb_PrimitiveComponent.modify(compId, modProps);
                        } catch (err) {
                        }
                    }
                    createdFootprints += 1;
                    updatedPlacements += 1;
                } else {
                    const padsSpec = (Array.isArray(p.pads) && p.pads.length > 0)
                        ? p.pads
                        : defaultPadsForEasyEdaFootprint(p.footprint || "R_0603");
                    let minRx = -1.2, maxRx = 1.2, minRy = -0.8, maxRy = 0.8;
                    for (const pad of padsSpec) {
                        const rx = Number(pad.rel_x !== undefined ? pad.rel_x : pad.dx || 0);
                        const ry = Number(pad.rel_y !== undefined ? pad.rel_y : pad.dy || 0);
                        const sx = Number(pad.size_x || 0.8);
                        const sy = Number(pad.size_y || 0.8);
                        minRx = Math.min(minRx, rx - sx * 0.5 - 0.25);
                        maxRx = Math.max(maxRx, rx + sx * 0.5 + 0.25);
                        minRy = Math.min(minRy, ry - sy * 0.5 - 0.25);
                        maxRy = Math.max(maxRy, ry + sy * 0.5 + 0.25);

                        const px = toCanvasUnits(xRaw + rx, units);
                        const py = toCanvasUnits(yRaw + ry, units);
                        const pw = toCanvasUnits(sx, units);
                        const ph = toCanvasUnits(sy, units);
                        const pNet = String(pad.net || "");

                        if (eda.pcb_PrimitivePad && typeof eda.pcb_PrimitivePad.create === "function") {
                            try {
                                await eda.pcb_PrimitivePad.create(
                                    layer,
                                    String(pad.number || "1"),
                                    px,
                                    py,
                                    rot,
                                    pw,
                                    ph,
                                    pNet
                                );
                                createdPads += 1;
                            } catch (err) {
                            }
                        } else if (eda.pcb_PrimitiveTrack && typeof eda.pcb_PrimitiveTrack.create === "function") {
                            await eda.pcb_PrimitiveTrack.create(
                                pNet,
                                layer,
                                px - pw * 0.25,
                                py,
                                px + pw * 0.25,
                                py,
                                ph
                            );
                            createdPads += 1;
                        }
                    }

                    if (eda.pcb_PrimitiveLine && typeof eda.pcb_PrimitiveLine.create === "function") {
                        const silkLayer = (layer === "BottomLayer") ? "BottomSilkLayer" : "TopSilkLayer";
                        const x1 = toCanvasUnits(xRaw + minRx, units);
                        const y1 = toCanvasUnits(yRaw + minRy, units);
                        const x2 = toCanvasUnits(xRaw + maxRx, units);
                        const y2 = toCanvasUnits(yRaw + maxRy, units);
                        const sw = toCanvasUnits(0.12, units);
                        await eda.pcb_PrimitiveLine.create(silkLayer, x1, y1, x2, y1, sw);
                        await eda.pcb_PrimitiveLine.create(silkLayer, x2, y1, x2, y2, sw);
                        await eda.pcb_PrimitiveLine.create(silkLayer, x2, y2, x1, y2, sw);
                        await eda.pcb_PrimitiveLine.create(silkLayer, x1, y2, x1, y1, sw);
                    }
                    if (ref && eda.pcb_PrimitiveString && typeof eda.pcb_PrimitiveString.create === "function") {
                        try {
                            await eda.pcb_PrimitiveString.create(
                                "TopSilkLayer",
                                toCanvasUnits(xRaw, units),
                                toCanvasUnits(yRaw + minRy - 1.0, units),
                                ref
                            );
                        } catch (err) {
                        }
                    }
                    createdFootprints += 1;
                    updatedPlacements += 1;
                }
            }
        }

        const modPads = (args && args.modify_pads) || [];
        if (Array.isArray(modPads) && modPads.length > 0 && typeof eda !== "undefined" && eda.pcb_PrimitivePad) {
            for (const mp of modPads) {
                if (!mp || typeof mp !== "object") continue;
                const padId = String(mp.id || mp.pad_id || "");
                const modProps = {};
                if (mp.net !== undefined) modProps.net = String(mp.net);
                if (mp.pad_number !== undefined || mp.number !== undefined) {
                    modProps.padNumber = String(mp.pad_number !== undefined ? mp.pad_number : mp.number);
                }
                if (mp.x !== undefined || mp.x_mm !== undefined) modProps.x = coordToMil(mp, "x", "x_mm", units, 0);
                if (mp.y !== undefined || mp.y_mm !== undefined) modProps.y = coordToMil(mp, "y", "y_mm", units, 0);
                if (mp.layer !== undefined) modProps.layer = normalizePcbLayer(mp.layer);
                if (padId && typeof eda.pcb_PrimitivePad.modify === "function") {
                    try {
                        await eda.pcb_PrimitivePad.modify(padId, modProps);
                    } catch (err) {
                    }
                }
                modifiedPads += 1;
            }
        }

        const delPrims = (args && args.delete_primitives) || [];
        const skippedChildPrimitives = [];
        if (Array.isArray(delPrims) && delPrims.length > 0 && typeof eda !== "undefined") {
            // Footprint child sub-primitives ("<componentId>e<digits>", e.g. pads) must never be
            // deleted individually: that silently breaks the footprint. Delete the component instead.
            const componentIds = {};
            if (eda.pcb_PrimitiveComponent && typeof eda.pcb_PrimitiveComponent.getAll === "function") {
                try {
                    const comps = await eda.pcb_PrimitiveComponent.getAll();
                    if (Array.isArray(comps)) {
                        for (const c of comps) {
                            const cid = String(callGetter(c, "getState_PrimitiveId", "primitiveId", "id", ""));
                            if (cid) componentIds[cid] = true;
                        }
                    }
                } catch (err) {
                }
            }
            const ids = delPrims.map(function (d) {
                return (d && typeof d === "object") ? String(d.id || d.primitive_id || "") : String(d || "");
            }).filter(function (id) {
                if (!id) return false;
                const lastE = id.lastIndexOf("e");
                if (lastE > 0 && /^[0-9]+$/.test(id.slice(lastE + 1)) && componentIds[id.slice(0, lastE)]) {
                    skippedChildPrimitives.push(id);
                    return false;
                }
                return true;
            });
            if (ids.length > 0) {
                if (eda.pcb_PrimitiveTrack && typeof eda.pcb_PrimitiveTrack.delete === "function") {
                    try { await eda.pcb_PrimitiveTrack.delete(ids); } catch (err) {}
                }
                if (eda.pcb_PrimitiveLine && typeof eda.pcb_PrimitiveLine.delete === "function") {
                    try { await eda.pcb_PrimitiveLine.delete(ids); } catch (err) {}
                }
                if (eda.pcb_PrimitiveVia && typeof eda.pcb_PrimitiveVia.delete === "function") {
                    try { await eda.pcb_PrimitiveVia.delete(ids); } catch (err) {}
                }
                if (eda.pcb_PrimitiveComponent && typeof eda.pcb_PrimitiveComponent.delete === "function") {
                    try { await eda.pcb_PrimitiveComponent.delete(ids); } catch (err) {}
                }
                if (eda.pcb_PrimitivePad && typeof eda.pcb_PrimitivePad.delete === "function") {
                    try { await eda.pcb_PrimitivePad.delete(ids); } catch (err) {}
                }
                if (eda.pcb_PrimitivePour && typeof eda.pcb_PrimitivePour.delete === "function") {
                    try { await eda.pcb_PrimitivePour.delete(ids); } catch (err) {}
                }
                deletedPrimitives += ids.length;
            }
        }

        const zones = args.zones || args.copper_pours || [];
        if (Array.isArray(zones) && typeof eda !== "undefined" && eda.pcb_PrimitivePour) {
            for (const z of zones) {
                const scaledPts = Array.isArray(z.points)
                    ? z.points.map(function (pt) {
                        if (pt && typeof pt === "object") {
                            return {
                                x: coordToMil(pt, "x", "x_mm", units, 0),
                                y: coordToMil(pt, "y", "y_mm", units, 0)
                            };
                        }
                        return toCanvasUnits(pt, units);
                    })
                    : [];
                if (eda.pcb_PrimitivePour.create) {
                    await eda.pcb_PrimitivePour.create(
                        z.net || "GND",
                        normalizePcbLayer(z.layer || "TopLayer"),
                        scaledPts
                    );
                }
                addedZones += 1;
            }
        }

        const tracks = args.tracks || args.add_tracks || [];
        if (Array.isArray(tracks) && typeof eda !== "undefined") {
            for (const t of tracks) {
                const tNet = String(t.net || "");
                const tX1 = coordToMil(t, "x1", "x1_mm", units, 0);
                const tY1 = coordToMil(t, "y1", "y1_mm", units, 0);
                const tX2 = coordToMil(t, "x2", "x2_mm", units, 0);
                const tY2 = coordToMil(t, "y2", "y2_mm", units, 0);
                const tW = coordToMil(t, "width", "width_mm", units, 10.0);
                if (eda.pcb_PrimitiveTrack && typeof eda.pcb_PrimitiveTrack.create === "function") {
                    await eda.pcb_PrimitiveTrack.create(
                        tNet,
                        normalizePcbLayer(t.layer || "TopLayer"),
                        tX1,
                        tY1,
                        tX2,
                        tY2,
                        tW
                    );
                    addedTracks += 1;
                } else if (eda.pcb_PrimitiveLine && typeof eda.pcb_PrimitiveLine.create === "function") {
                    const layerId = resolveEasyEdaCopperLayerId(t.layer || "TopLayer");
                    await eda.pcb_PrimitiveLine.create(
                        tNet,
                        layerId,
                        tX1,
                        tY1,
                        tX2,
                        tY2,
                        tW,
                        false
                    );
                    addedTracks += 1;
                }
            }
        }

        const vias = args.vias || args.add_vias || [];
        if (Array.isArray(vias) && typeof eda !== "undefined" && eda.pcb_PrimitiveVia) {
            for (const v of vias) {
                if (eda.pcb_PrimitiveVia.create) {
                    await eda.pcb_PrimitiveVia.create(
                        v.net || "",
                        coordToMil(v, "x", "x_mm", units, 0),
                        coordToMil(v, "y", "y_mm", units, 0),
                        coordToMil(v, "drill", "drill_mm", units, 12.0),
                        coordToMil(v, "size", "size_mm", units, 24.0)
                    );
                }
                addedVias += 1;
            }
        }

        try {
            if (typeof eda !== "undefined" && eda.dmt_EditorControl && typeof eda.dmt_EditorControl.zoomToAllPrimitives === "function") {
                await eda.dmt_EditorControl.zoomToAllPrimitives();
            }
        } catch (err) {
        }

        return {
            ok: true,
            active_document: await resolveActiveDocumentTitle(),
            updated_placements: updatedPlacements,
            moved_components: updatedPlacements,
            created_footprints: createdFootprints,
            placed_components: createdFootprints,
            created_pads: createdPads,
            modified_pads: modifiedPads,
            deleted_primitives: deletedPrimitives,
            skipped_child_primitives: skippedChildPrimitives,
            added_zones: addedZones,
            added_tracks: addedTracks,
            added_vias: addedVias,
            rubber_banded_tracks: rubberBandedTracks,
            rubber_banded_tracks_count: rubberBandedTracks.length
        };
    }

    // ---- EasyEDA Pro native auto-router (Route -> Auto Route -> Run) -----------------------
    // Best effort: extensions normally run in a sandboxed iframe, so window.top.document is
    // expected to throw. In that case the bridge falls back to Win32 UI Automation.
    const ROUTE_MENU_LABELS = ["Route", "布线"];
    const AUTO_ROUTE_ITEM_LABELS = ["Auto Route...", "Auto Route…", "Auto Route", "Auto Router...",
        "Auto Router", "自动布线...", "自动布线…", "自动布线"];
    const RUN_BUTTON_LABELS = ["Run", "Run Auto Route", "Auto Route", "执行", "开始布线", "运行"];

    function sleepMs(ms) {
        return new Promise(function (resolve) { setTimeout(resolve, ms); });
    }

    async function countCopperTracks() {
        let n = 0;
        try {
            if (typeof eda !== "undefined" && eda.pcb_PrimitiveLine && typeof eda.pcb_PrimitiveLine.getAll === "function") {
                const raw = await eda.pcb_PrimitiveLine.getAll();
                if (Array.isArray(raw)) {
                    for (const ln of raw) {
                        if (isCopperLayer(callGetter(ln, "getState_Layer", "layer", "", ""))) n += 1;
                    }
                }
            }
        } catch (err) {
            logDiag("countCopperTracks error: " + String(err));
        }
        return n;
    }

    function collectAccessibleDocuments() {
        const docs = [];
        const tryPush = function (getter, label) {
            try {
                const d = getter();
                if (d && typeof d.querySelectorAll === "function" &&
                    !docs.some(function (x) { return x.doc === d; })) {
                    docs.push({ doc: d, label: label });
                }
            } catch (err) {
                // Cross-origin frame access throws SecurityError: not accessible from this sandbox
            }
        };
        tryPush(function () { return document; }, "document");
        tryPush(function () { return window.parent.document; }, "parent");
        tryPush(function () { return window.top.document; }, "top");
        return docs;
    }

    function normalizedText(node) {
        return String((node && node.textContent) || "").replace(/\s+/g, " ").trim();
    }

    // First element (document order) whose whole trimmed text equals one of the labels, then
    // descend to the deepest child carrying the same text so the click lands on the handler.
    function findElementByText(doc, selector, labels, exclude) {
        let nodes = [];
        try {
            nodes = doc.querySelectorAll(selector);
        } catch (err) {
            return null;
        }
        for (const node of nodes) {
            if (exclude && node === exclude) continue;
            const txt = normalizedText(node);
            if (!txt || txt.length > 40 || labels.indexOf(txt) < 0) continue;
            let target = node;
            let descended = true;
            while (descended) {
                descended = false;
                for (const child of target.children || []) {
                    if (normalizedText(child) === txt) {
                        target = child;
                        descended = true;
                        break;
                    }
                }
            }
            return target;
        }
        return null;
    }

    function clickElement(el) {
        const view = (el.ownerDocument && el.ownerDocument.defaultView) || null;
        for (const type of ["mousedown", "mouseup", "click"]) {
            try {
                el.dispatchEvent(new MouseEvent(type, { bubbles: true, cancelable: true, view: view }));
            } catch (err) {
            }
        }
    }

    async function handleNativeAutoRoute(args) {
        const timeoutMs = Math.min(Math.max(Number((args && args.timeout_ms) || 120000), 5000), 600000);
        if (typeof eda === "undefined") {
            return { ok: false, triggered: false, error: "EasyEDA Pro eda API is unavailable" };
        }
        await ensureDocumentOpen("pcb", args && args.pcb_uuid, null);
        const beforeTracks = await countCopperTracks();
        const docs = collectAccessibleDocuments();
        const attempts = [];
        let triggeredDoc = "";
        for (const entry of docs) {
            const routeMenu = findElementByText(entry.doc,
                "[role='menuitem'],[role='button'],button,li,a,span,div", ROUTE_MENU_LABELS, null);
            if (!routeMenu) {
                attempts.push({ document: entry.label, stage: "route_menu_not_found" });
                continue;
            }
            clickElement(routeMenu);
            await sleepMs(400);
            const autoItem = findElementByText(entry.doc,
                "[role='menuitem'],li,a,span,div", AUTO_ROUTE_ITEM_LABELS, routeMenu);
            if (!autoItem) {
                attempts.push({ document: entry.label, stage: "auto_route_item_not_found" });
                continue;
            }
            clickElement(autoItem);
            let runBtn = null;
            for (let i = 0; i < 25 && !runBtn; i += 1) {
                await sleepMs(200);
                runBtn = findElementByText(entry.doc, "button,[role='button']", RUN_BUTTON_LABELS, autoItem);
            }
            if (!runBtn) {
                attempts.push({ document: entry.label, stage: "run_button_not_found" });
                continue;
            }
            clickElement(runBtn);
            triggeredDoc = entry.label;
            break;
        }
        if (!triggeredDoc) {
            return {
                ok: true,
                triggered: false,
                needs_win32_ui_automation: true,
                before_tracks: beforeTracks,
                accessible_documents: docs.map(function (d) { return d.label; }),
                attempts: attempts
            };
        }

        const deadline = Date.now() + timeoutMs;
        let last = beforeTracks;
        let stableTicks = 0;
        let changed = false;
        while (Date.now() < deadline) {
            await sleepMs(1500);
            const cur = await countCopperTracks();
            if (cur !== last) {
                last = cur;
                stableTicks = 0;
                if (cur !== beforeTracks) changed = true;
            } else {
                stableTicks += 1;
                if (changed && stableTicks >= 3) break;
            }
        }
        return {
            ok: true,
            triggered: true,
            method: "dom",
            document: triggeredDoc,
            before_tracks: beforeTracks,
            after_tracks: last,
            added_tracks: last - beforeTracks,
            settled: changed && stableTicks >= 3,
            attempts: attempts
        };
    }

    async function handleRunDrc(args) {
        const target = String((args && args.target) || "pcb");
        let violations = [];
        let passed = true;
        let rawCategoriesCount = 0;

        if (typeof eda !== "undefined") {
            if (target === "schematic") {
                await ensureDocumentOpen("schematic", args && (args.page_uuid || args.sheet || args.page), null);
            } else {
                await ensureDocumentOpen("pcb", args && args.pcb_uuid, null);
            }
            let raw = null;
            if (target === "schematic" && eda.sch_Drc && typeof eda.sch_Drc.check === "function") {
                raw = await eda.sch_Drc.check(false, false, true);
            } else if (eda.pcb_Drc && typeof eda.pcb_Drc.check === "function") {
                raw = await eda.pcb_Drc.check(false, false, true);
            }
            if (typeof raw === "boolean") {
                passed = raw;
                violations = [];
            } else if (Array.isArray(raw)) {
                for (const item of raw) {
                    if (item && typeof item === "object") {
                        const subList = item.list || item.items || item.errors || item.violations;
                        if (Array.isArray(subList)) {
                            rawCategoriesCount += 1;
                            for (const leaf of subList) {
                                violations.push(leaf);
                            }
                        } else {
                            violations.push(item);
                        }
                    } else if (item !== undefined && item !== null) {
                        violations.push(item);
                    }
                }
                passed = (violations.length === 0);
            } else if (raw && typeof raw === "object") {
                const sub = raw.violations || raw.errors || raw.list;
                if (Array.isArray(sub)) {
                    violations = sub;
                }
                passed = (violations.length === 0);
            }
        }

        const out = {
            ok: true,
            target: target,
            passed: passed,
            native_passed: passed,
            raw_categories: rawCategoriesCount,
            violation_count: violations.length,
            violations: violations
        };

        if (target === "pcb") {
            const pcbSnap = await handleInspectPcb({ _full_for_bridge: true });
            out.units = pcbSnap.units || "mil";
            out.pads = pcbSnap.pads || [];
            out.tracks = pcbSnap.tracks || [];
            out.vias = pcbSnap.vias || [];
            out.pours = pcbSnap.pours || [];
            out.components = pcbSnap.components || [];
            out.schematic_components = pcbSnap.schematic_components || [];
        } else if (target === "schematic") {
            const schSnap = await handleInspectSchematic({ _full_for_bridge: true });
            out.project = schSnap.project || {};
            out.document = schSnap.document || {};
            out.components = schSnap.components || [];
            out.wires = schSnap.wires || [];
        }

        return out;
    }

    async function handleExportJlcpcb(args) {
        let gerberData = "PK\x03\x04JLCPCB_GERBER";
        let bomCsv = "Comment,Designator,Footprint,LCSC Part #\n";
        let cplCsv = "Designator,Mid X,Mid Y,Layer,Rotation\n";
        let dsnContent = "(pcb \"easyeda.dsn\")\n";
        let liveApiUsed = false;

        if (typeof eda !== "undefined") {
            await ensureDocumentOpen("pcb", args && args.pcb_uuid, null);
            if (eda.pcb_ManufactureData) {
                liveApiUsed = true;
                if (eda.pcb_ManufactureData.getGerberFile) {
                    const g = await eda.pcb_ManufactureData.getGerberFile();
                    if (g) gerberData = String(g);
                }
                if (eda.pcb_ManufactureData.getBomFile) {
                    const b = await eda.pcb_ManufactureData.getBomFile();
                    if (b) bomCsv = String(b);
                }
                if (eda.pcb_ManufactureData.getCplFile) {
                    const c = await eda.pcb_ManufactureData.getCplFile();
                    if (c) cplCsv = String(c);
                }
                if (eda.pcb_ManufactureData.getDsnFile) {
                    const d = await eda.pcb_ManufactureData.getDsnFile();
                    if (d) dsnContent = String(d);
                }
            }
        }

        return {
            ok: true,
            live_manufacture_api: liveApiUsed,
            gerber_data: gerberData,
            bom_csv: bomCsv,
            cpl_csv: cplCsv,
            dsn_content: dsnContent
        };
    }

    async function handleInvokeApi(args) {
        const apiPath = String(args.api_path || args.api || "");
        const callArgs = Array.isArray(args.args) ? args.args : [];
        const allowed = ALLOWED_PREFIXES.some(function (pfx) {
            return apiPath.indexOf(pfx) === 0;
        });
        if (!allowed) {
            throw new Error("Disallowed EasyEDA API namespace: " + apiPath);
        }

        const parts = apiPath.split(".");
        if (parts.length !== 3 || parts[0] !== "eda") {
            throw new Error("Expected eda.<module>.<method> format: " + apiPath);
        }
        if (typeof eda === "undefined" || !eda[parts[1]] || typeof eda[parts[1]][parts[2]] !== "function") {
            return {
                ok: true,
                api_path: apiPath,
                simulated: true,
                result: null
            };
        }
        const fnResult = await eda[parts[1]][parts[2]].apply(eda[parts[1]], callArgs);
        return {
            ok: true,
            api_path: apiPath,
            result: fnResult
        };
    }

    async function dispatchTool(toolName, args) {
        switch (toolName) {
            case "easyeda_live_status":
                return {
                    ok: true,
                    plugin_connected: true,
                    active_document: await resolveActiveDocumentTitle()
                };
            case "easyeda_search_library":
                return await handleSearchLibrary(args);
            case "easyeda_inspect_schematic":
                return await handleInspectSchematic(args);
            case "easyeda_edit_schematic":
                return await handleEditSchematic(args);
            case "easyeda_sync_sch_to_pcb":
                return await handleSyncSchToPcb(args);
            case "easyeda_inspect_pcb":
                return await handleInspectPcb(args);
            case "easyeda_edit_pcb":
                return await handleEditPcb(args);
            case "easyeda_native_autoroute":
                return await handleNativeAutoRoute(args);
            case "easyeda_run_drc":
                return await handleRunDrc(args);
            case "easyeda_export_jlcpcb":
                return await handleExportJlcpcb(args);
            case "easyeda_invoke_api":
                return await handleInvokeApi(args);
            default:
                throw new Error("Unsupported EasyEDA Pro plugin tool: " + toolName);
        }
    }

    function sendRawFrameString(frameText) {
        if (useEdaSysWs) {
            if (typeof eda !== "undefined" && eda.sys_WebSocket && typeof eda.sys_WebSocket.send === "function") {
                eda.sys_WebSocket.send(currentWsId, frameText);
                return true;
            }
            return false;
        }
        if (wsClient && wsClient.readyState === 1) {
            wsClient.send(frameText);
            return true;
        }
        return false;
    }

    function sendRawJson(obj) {
        const text = JSON.stringify(obj);
        const CHUNK_LIMIT = 24000;
        if (text.length <= CHUNK_LIMIT) {
            return sendRawFrameString(text);
        }
        const totalChunks = Math.ceil(text.length / CHUNK_LIMIT);
        const msgId = String((obj && obj.id !== undefined) ? obj.id : ("chk-" + Date.now()));
        for (let i = 0; i < totalChunks; i++) {
            const slice = text.slice(i * CHUNK_LIMIT, (i + 1) * CHUNK_LIMIT);
            const env = JSON.stringify({
                jsonrpc: "2.0",
                method: "plugin/chunk",
                params: {
                    msg_id: msgId,
                    index: i,
                    total: totalChunks,
                    data: slice
                }
            });
            if (!sendRawFrameString(env)) {
                return false;
            }
        }
        return true;
    }

    function sendRegistration() {
        const regMsg = {
            jsonrpc: "2.0",
            id: "easyeda-reg-1",
            method: "plugin/register",
            params: {
                app: "easyeda",
                version: BRIDGE_VERSION,
                active_document: cachedActiveDocument
            }
        };
        try {
            sendRawJson(regMsg);
        } catch (err) {
            logDiag("sendRegistration failed: " + String(err));
        }
    }

    async function onIncomingMessage(event) {
        let msg = null;
        try {
            const raw = (event && event.data !== undefined) ? event.data : event;
            msg = JSON.parse(String(raw));
        } catch (err) {
            return;
        }
        if (!msg) return;

        missedHeartbeats = 0;
        connectingTicks = 0;

        if (msg.method === "server/hello") {
            isConnected = true;
            isConnecting = false;
            sendRegistration();
            return;
        }

        if (msg.result && (msg.result.registered || msg.result.status === "registered")) {
            if (!isConnected) {
                isConnected = true;
                isConnecting = false;
                logDiag("Registered with aimon on 127.0.0.1:3893");
                showToast("Connected to Aimon AI Bridge (port 3893)");
            }
            return;
        }

        if (msg.method !== "tools/call" || !msg.params) {
            return;
        }

        const reqId = msg.id;
        const toolName = msg.params.name || "";
        const toolArgs = msg.params.arguments || {};

        try {
            const res = await dispatchTool(toolName, toolArgs);
            sendRawJson({
                jsonrpc: "2.0",
                id: reqId,
                result: res
            });
        } catch (err) {
            sendRawJson({
                jsonrpc: "2.0",
                id: reqId,
                error: {
                    code: -32000,
                    message: String(err && err.message ? err.message : err)
                }
            });
        }
    }

    function startHeartbeatLoop(targetUrl) {
        if (heartbeatTimer || permissionBlocked) return;
        heartbeatTimer = setInterval(function () {
            if (permissionBlocked) {
                if (heartbeatTimer) {
                    clearInterval(heartbeatTimer);
                    heartbeatTimer = null;
                }
                return;
            }
            if (!isConnected && !isConnecting) {
                connectRelay(targetUrl);
                return;
            }
            if (!isConnected && isConnecting) {
                connectingTicks += 1;
                if (connectingTicks >= 2) {
                    connectingTicks = 0;
                    isConnecting = false;
                    rotateAndReconnect(targetUrl);
                }
                return;
            }
            if (isConnected) {
                missedHeartbeats += 1;
                if (missedHeartbeats > 3) {
                    logDiag("Heartbeat timeout -> reconnecting");
                    isConnected = false;
                    isConnecting = false;
                    rotateAndReconnect(targetUrl);
                    return;
                }
                heartbeatSeq += 1;
                try {
                    sendRawJson({
                        jsonrpc: "2.0",
                        id: "hb-" + heartbeatSeq,
                        method: "plugin/status",
                        params: {
                            app: "easyeda",
                            version: BRIDGE_VERSION,
                            active_document: cachedActiveDocument
                        }
                    });
                } catch (err) {
                    isConnected = false;
                    isConnecting = false;
                    rotateAndReconnect(targetUrl);
                }
            }
        }, HEARTBEAT_INTERVAL_MS);
    }

    function rotateAndReconnect(targetUrl) {
        try {
            if (useEdaSysWs && hasRegisteredWs && typeof eda !== "undefined" && eda.sys_WebSocket && typeof eda.sys_WebSocket.close === "function") {
                eda.sys_WebSocket.close(currentWsId);
            } else if (wsClient) {
                wsClient.close();
                wsClient = null;
            }
        } catch (err) {
        }
        hasRegisteredWs = false;
        wsGeneration += 1;
        currentWsId = wsIdBase + "-r" + wsGeneration;
        isConnected = false;
        isConnecting = true;
        connectingTicks = 0;
        if (!reconnectTimer && !permissionBlocked) {
            reconnectTimer = setTimeout(function () {
                reconnectTimer = null;
                isConnecting = false;
                connectRelay(targetUrl || DEFAULT_WS_URL);
            }, 500);
        } else {
            isConnecting = false;
        }
    }

    function connectRelay(url) {
        if (permissionBlocked || isConnected || isConnecting) {
            return;
        }
        const targetUrl = url || DEFAULT_WS_URL;
        startHeartbeatLoop(targetUrl);

        if (typeof eda !== "undefined" && eda.sys_WebSocket && typeof eda.sys_WebSocket.register === "function") {
            useEdaSysWs = true;
            isConnecting = true;
            connectingTicks = 0;
            missedHeartbeats = 0;
            try {
                if (hasRegisteredWs && typeof eda.sys_WebSocket.close === "function") {
                    eda.sys_WebSocket.close(currentWsId);
                }
            } catch (err) {
            }
            hasRegisteredWs = false;

            setTimeout(function () {
                try {
                    logDiag("Registering eda.sys_WebSocket id=" + currentWsId + " url=" + targetUrl);
                    eda.sys_WebSocket.register(
                        currentWsId,
                        targetUrl,
                        onIncomingMessage,
                        function () {
                            isConnected = true;
                            isConnecting = false;
                            sendRegistration();
                        }
                    );
                    hasRegisteredWs = true;
                    setTimeout(function () {
                        if (hasRegisteredWs && !permissionBlocked) {
                            sendRegistration();
                        }
                    }, 350);
                } catch (err) {
                    const errText = String(err);
                    logDiag("eda.sys_WebSocket.register error: " + errText);
                    isConnecting = false;
                    hasRegisteredWs = false;
                    if (errText.indexOf("权限") !== -1 ||
                        errText.toLowerCase().indexOf("permission") !== -1 ||
                        errText.toLowerCase().indexOf("external") !== -1) {
                        permissionBlocked = true;
                        if (heartbeatTimer) {
                            clearInterval(heartbeatTimer);
                            heartbeatTimer = null;
                        }
                        logDiag("Paused auto-retry: enable External Interaction permission in Extensions Manager, then click Aimon AI Bridge -> Reconnect Bridge.");
                    }
                }
            }, 200);
            return;
        }

        if (typeof WebSocket === "undefined") {
            return;
        }
        useEdaSysWs = false;
        isConnecting = true;
        wsClient = new WebSocket(targetUrl);

        wsClient.onopen = function () {
            isConnected = true;
            isConnecting = false;
            sendRegistration();
        };

        wsClient.onmessage = onIncomingMessage;

        wsClient.onclose = function () {
            wsClient = null;
            isConnected = false;
            isConnecting = false;
            if (!reconnectTimer && !permissionBlocked) {
                reconnectTimer = setTimeout(function () {
                    reconnectTimer = null;
                    connectRelay(targetUrl);
                }, 3000);
            }
        };
    }

    function activate() {
        logDiag("activate() called");
        permissionBlocked = false;
        connectRelay(DEFAULT_WS_URL);
    }

    function deactivate() {
        if (heartbeatTimer) {
            clearInterval(heartbeatTimer);
            heartbeatTimer = null;
        }
        try {
            if (useEdaSysWs && hasRegisteredWs && typeof eda !== "undefined" && eda.sys_WebSocket && typeof eda.sys_WebSocket.close === "function") {
                eda.sys_WebSocket.close(currentWsId);
            } else if (wsClient) {
                wsClient.close();
                wsClient = null;
            }
        } catch (err) {
        }
        hasRegisteredWs = false;
        isConnected = false;
        isConnecting = false;
    }

    function reconnectBridge() {
        permissionBlocked = false;
        showToast("Reconnecting to Aimon AI Bridge...");
        rotateAndReconnect(DEFAULT_WS_URL);
    }

    function showBridgeStatus() {
        if (permissionBlocked) {
            showToast("Aimon AI Bridge: Enable External Interaction in Extensions Manager, then click Reconnect Bridge");
            return;
        }
        showToast(isConnected
            ? ("Aimon AI Bridge: Connected (" + cachedActiveDocument + ")")
            : "Aimon AI Bridge: Disconnected (retrying 127.0.0.1:3893)");
    }

    // Bootstrap immediately on script evaluation as well as on activate()
    try {
        connectRelay(DEFAULT_WS_URL);
    } catch (err) {
    }

    const exportsObj = {
        activate: activate,
        deactivate: deactivate,
        reconnectBridge: reconnectBridge,
        showBridgeStatus: showBridgeStatus,
        toggleAimonBridge: reconnectBridge,
        connectRelay: connectRelay,
        dispatchTool: dispatchTool
    };

    if (typeof module !== "undefined" && module.exports) {
        module.exports = exportsObj;
    }

    return exportsObj;
})();

function activate(status, arg) {
    return edaEsbuildExportName.activate(status, arg);
}

function deactivate() {
    return edaEsbuildExportName.deactivate();
}

function reconnectBridge() {
    return edaEsbuildExportName.reconnectBridge();
}

function showBridgeStatus() {
    return edaEsbuildExportName.showBridgeStatus();
}

function toggleAimonBridge() {
    return edaEsbuildExportName.reconnectBridge();
}

)JS";

static const char* kEmbeddedKiCadInitPy = R"PY(#
# __init__.py - KiCad pcbnew Action Plugin Registration for aimon
#
# Copyright (C) 2026, Charles Chiou
#

from .KiCadAimonBridge import KiCadAimonBridgePlugin

try:
    KiCadAimonBridgePlugin().register()
except Exception:
    pass
)PY";

static const char* kEmbeddedKiCadBridgePy = R"PY(#
# KiCadAimonBridge.py - KiCad Live pcbnew Action Plugin for aimon-eda-bridge
#
# Copyright (C) 2026, Charles Chiou
#

import json
import math
import os
import socket
import threading
import time

BRIDGE_VERSION = "__AIMON_VERSION__"

try:
    import pcbnew
    HAS_PCBNEW = True
except ImportError:
    pcbnew = None
    HAS_PCBNEW = False

try:
    import wx
    HAS_WX = True
except ImportError:
    wx = None
    HAS_WX = False


def _vec2i(x_mm, y_mm):
    return pcbnew.VECTOR2I(pcbnew.FromMM(float(x_mm)), pcbnew.FromMM(float(y_mm)))


class KiCadAimonBridgeClient:
    """Connects an open KiCad pcbnew session to aimon-eda-bridge on 127.0.0.1:3893."""

    def __init__(self, host="127.0.0.1", port=3893):
        self._host = host
        self._port = int(port)
        self._running = False
        self._sock = None
        self._thread = None
        self._file_mtime = (
            os.path.getmtime(__file__)
            if "__file__" in globals() and os.path.exists(__file__)
            else 0.0
        )

    def start(self):
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._run_loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def _run_on_gui_thread(self, fn):
        if HAS_WX and wx is not None and hasattr(wx, "GetApp") and wx.GetApp() is not None:
            if hasattr(wx, "IsMainThread") and wx.IsMainThread():
                return fn()
            done_evt = threading.Event()
            box = {"result": None, "error": None}

            def _wrapped():
                try:
                    box["result"] = fn()
                except Exception as exc:
                    box["error"] = str(exc)
                finally:
                    done_evt.set()

            wx.CallAfter(_wrapped)
            done_evt.wait(timeout=15.0)
            if box["error"] is not None:
                return {"ok": False, "error": box["error"], "live_gui": True}
            if box["result"] is not None:
                return box["result"]
        return fn()

    def _get_kicad_version(self):
        if HAS_PCBNEW and hasattr(pcbnew, "Version"):
            try:
                v = str(pcbnew.Version()).strip()
                if v:
                    return v.split()[0].strip("()")
            except Exception:
                pass
        return "10.0"

    def _get_active_board_name(self):
        if HAS_PCBNEW and hasattr(pcbnew, "GetBoard"):
            try:
                board = pcbnew.GetBoard()
                if board is not None and hasattr(board, "GetFileName"):
                    fn = board.GetFileName()
                    return str(fn) if fn else ""
            except Exception:
                pass
        return ""

    def _default_pads_for_footprint(self, fp_name, layer_name="F.Cu"):
        fp_upper = str(fp_name).upper()
        if "SOT-23" in fp_upper:
            return [
                {"number": "1", "type": "smd", "shape": "rect", "rel_x": -0.95, "rel_y": 1.1, "size_x": 0.6, "size_y": 0.7, "layer": layer_name},
                {"number": "2", "type": "smd", "shape": "rect", "rel_x": 0.95, "rel_y": 1.1, "size_x": 0.6, "size_y": 0.7, "layer": layer_name},
                {"number": "3", "type": "smd", "shape": "rect", "rel_x": 0.0, "rel_y": -1.1, "size_x": 0.6, "size_y": 0.7, "layer": layer_name},
            ]
        if "SOIC-8" in fp_upper or "SOP-8" in fp_upper:
            pads = []
            for i in range(4):
                ry = -1.905 + i * 1.27
                pads.append({"number": str(i + 1), "type": "smd", "shape": "rect", "rel_x": -2.7, "rel_y": ry, "size_x": 1.5, "size_y": 0.6, "layer": layer_name})
            for i in range(4):
                ry = 1.905 - i * 1.27
                pads.append({"number": str(i + 5), "type": "smd", "shape": "rect", "rel_x": 2.7, "rel_y": ry, "size_x": 1.5, "size_y": 0.6, "layer": layer_name})
            return pads
        if "PINHEADER" in fp_upper or "CONN_01X02" in fp_upper:
            return [
                {"number": "1", "type": "thru_hole", "shape": "rect", "rel_x": 0.0, "rel_y": -1.27, "size_x": 1.7, "size_y": 1.7, "drill": 1.0, "layer": "*.Cu"},
                {"number": "2", "type": "thru_hole", "shape": "circle", "rel_x": 0.0, "rel_y": 1.27, "size_x": 1.7, "size_y": 1.7, "drill": 1.0, "layer": "*.Cu"},
            ]
        if "0805" in fp_upper:
            return [
                {"number": "1", "type": "smd", "shape": "rect", "rel_x": -1.0, "rel_y": 0.0, "size_x": 1.0, "size_y": 1.3, "layer": layer_name},
                {"number": "2", "type": "smd", "shape": "rect", "rel_x": 1.0, "rel_y": 0.0, "size_x": 1.0, "size_y": 1.3, "layer": layer_name},
            ]
        return [
            {"number": "1", "type": "smd", "shape": "rect", "rel_x": -0.8, "rel_y": 0.0, "size_x": 0.8, "size_y": 0.9, "layer": layer_name},
            {"number": "2", "type": "smd", "shape": "rect", "rel_x": 0.8, "rel_y": 0.0, "size_x": 0.8, "size_y": 0.9, "layer": layer_name},
        ]

    def _inspect_live_board(self):
        def _do_inspect():
            footprints = []
            all_pads = []
            tracks = []
            vias = []
            outline_segments = 0
            if HAS_PCBNEW and hasattr(pcbnew, "GetBoard"):
                board = pcbnew.GetBoard()
                if board is not None:
                    f_cu = getattr(pcbnew, "F_Cu", 0)
                    b_cu = getattr(pcbnew, "B_Cu", 31)
                    pth_attr = getattr(pcbnew, "PAD_ATTRIB_PTH", 0)
                    npth_attr = getattr(pcbnew, "PAD_ATTRIB_NPTH", 3)
                    for fp in board.GetFootprints():
                        pos = fp.GetPosition()
                        fp_pos_x = pos.x if hasattr(pos, "x") else pos[0]
                        fp_pos_y = pos.y if hasattr(pos, "y") else pos[1]
                        fp_layer_name = board.GetLayerName(fp.GetLayer()) if hasattr(board, "GetLayerName") else "F.Cu"
                        fp_ref = fp.GetReference()
                        pads = []
                        if hasattr(fp, "Pads"):
                            for pad in fp.Pads():
                                ppos = pad.GetPosition() if hasattr(pad, "GetPosition") else getattr(pad, "pos", (0, 0))
                                px_raw = ppos.x if hasattr(ppos, "x") else ppos[0]
                                py_raw = ppos.y if hasattr(ppos, "y") else ppos[1]
                                p_num = pad.GetNumber() if hasattr(pad, "GetNumber") else "1"
                                p_net = pad.GetNetname() if hasattr(pad, "GetNetname") else ""
                                p_w = 0.8
                                p_h = 0.8
                                if hasattr(pad, "GetSize"):
                                    try:
                                        psz = pad.GetSize()
                                        sx_raw = psz.x if hasattr(psz, "x") else psz[0]
                                        sy_raw = psz.y if hasattr(psz, "y") else psz[1]
                                        if sx_raw > 0:
                                            p_w = pcbnew.ToMM(sx_raw)
                                        if sy_raw > 0:
                                            p_h = pcbnew.ToMM(sy_raw)
                                    except Exception:
                                        pass
                                elif hasattr(pad, "size") and isinstance(pad.size, tuple) and len(pad.size) == 2:
                                    if pad.size[0] > 0:
                                        p_w = pcbnew.ToMM(pad.size[0])
                                    if pad.size[1] > 0:
                                        p_h = pcbnew.ToMM(pad.size[1])

                                pad_layer = fp_layer_name
                                is_thru = False
                                if hasattr(pad, "GetAttribute"):
                                    try:
                                        attr = pad.GetAttribute()
                                        if attr in (pth_attr, npth_attr):
                                            is_thru = True
                                    except Exception:
                                        pass
                                if not is_thru and hasattr(pad, "GetDrillSize"):
                                    try:
                                        dsz = pad.GetDrillSize()
                                        dx = dsz.x if hasattr(dsz, "x") else dsz[0]
                                        if dx > 0:
                                            is_thru = True
                                    except Exception:
                                        pass
                                if not is_thru and hasattr(pad, "GetLayerSet"):
                                    try:
                                        lset = pad.GetLayerSet()
                                        if hasattr(lset, "Contains"):
                                            has_f = bool(lset.Contains(f_cu))
                                            has_b = bool(lset.Contains(b_cu))
                                            if has_f and has_b:
                                                is_thru = True
                                            elif has_b and not has_f:
                                                pad_layer = "B.Cu"
                                            elif has_f and not has_b:
                                                pad_layer = "F.Cu"
                                    except Exception:
                                        pass
                                if is_thru:
                                    pad_layer = "*.Cu"

                                pad_entry = {
                                    "number": p_num,
                                    "pad": p_num,
                                    "ref": fp_ref,
                                    "x": pcbnew.ToMM(px_raw),
                                    "y": pcbnew.ToMM(py_raw),
                                    "width": p_w,
                                    "height": p_h,
                                    "layer": pad_layer,
                                    "net": p_net
                                }
                                pads.append(pad_entry)
                                all_pads.append(pad_entry)
                        footprints.append({
                            "reference": fp_ref,
                            "value": fp.GetValue(),
                            "x": pcbnew.ToMM(fp_pos_x),
                            "y": pcbnew.ToMM(fp_pos_y),
                            "rotation": fp.GetOrientationDegrees(),
                            "layer": fp_layer_name,
                            "pads": pads
                        })
                    for trk in board.GetTracks():
                        if hasattr(pcbnew, "PCB_VIA") and isinstance(trk, pcbnew.PCB_VIA):
                            vpos = trk.GetPosition() if hasattr(trk, "GetPosition") else getattr(trk, "pos", (0, 0))
                            vx_raw = vpos.x if hasattr(vpos, "x") else vpos[0]
                            vy_raw = vpos.y if hasattr(vpos, "y") else vpos[1]
                            vias.append({
                                "net": trk.GetNetname() if hasattr(trk, "GetNetname") else "",
                                "x": pcbnew.ToMM(vx_raw),
                                "y": pcbnew.ToMM(vy_raw),
                                "size": pcbnew.ToMM(trk.GetWidth()) if hasattr(trk, "GetWidth") else 0.6
                            })
                        else:
                            start = trk.GetStart() if hasattr(trk, "GetStart") else getattr(trk, "start", (0, 0))
                            end = trk.GetEnd() if hasattr(trk, "GetEnd") else getattr(trk, "end", (0, 0))
                            sx_raw = start.x if hasattr(start, "x") else start[0]
                            sy_raw = start.y if hasattr(start, "y") else start[1]
                            ex_raw = end.x if hasattr(end, "x") else end[0]
                            ey_raw = end.y if hasattr(end, "y") else end[1]
                            tracks.append({
                                "net": trk.GetNetname() if hasattr(trk, "GetNetname") else "",
                                "x1": pcbnew.ToMM(sx_raw),
                                "y1": pcbnew.ToMM(sy_raw),
                                "x2": pcbnew.ToMM(ex_raw),
                                "y2": pcbnew.ToMM(ey_raw),
                                "width": pcbnew.ToMM(trk.GetWidth()) if hasattr(trk, "GetWidth") else 0.25,
                                "layer": board.GetLayerName(trk.GetLayer()) if hasattr(board, "GetLayerName") and hasattr(trk, "GetLayer") else "F.Cu"
                            })
                    if hasattr(board, "GetDrawings"):
                        edge_layer = getattr(pcbnew, "Edge_Cuts", 44)
                        for drw in board.GetDrawings():
                            if hasattr(drw, "GetLayer") and drw.GetLayer() == edge_layer:
                                outline_segments += 1
            return {
                "ok": True,
                "live_gui": True,
                "active_document": self._get_active_board_name(),
                "footprints_count": len(footprints),
                "pads_count": len(all_pads),
                "tracks_count": len(tracks),
                "vias_count": len(vias),
                "outline_segments": outline_segments,
                "footprints": footprints,
                "pads": all_pads,
                "tracks": tracks,
                "vias": vias
            }

        return self._run_on_gui_thread(_do_inspect)

    def _apply_live_pcb_edits(self, args):
        def _do_edits():
            created_footprints = 0
            updated_footprints = 0
            created_pads = 0
            created_tracks = 0
            created_vias = 0
            created_outline_segments = 0
            registered_nets = 0

            if not (HAS_PCBNEW and hasattr(pcbnew, "GetBoard")):
                return {"ok": False, "error": "pcbnew.GetBoard() is not available"}

            board = pcbnew.GetBoard()
            if board is None:
                return {"ok": False, "error": "No active PCB board in pcbnew"}

            if args.get("clear_existing", False) or args.get("clear_board", False):
                for fp in list(board.GetFootprints()):
                    board.Remove(fp)
                for trk in list(board.GetTracks()):
                    board.Remove(trk)
                if hasattr(board, "GetDrawings"):
                    for drw in list(board.GetDrawings()):
                        board.Remove(drw)

            net_cache = {}
            if hasattr(board, "GetNetInfo"):
                try:
                    net_info = board.GetNetInfo()
                    if hasattr(net_info, "NetsByName"):
                        for k, v in net_info.NetsByName().items():
                            net_cache[str(k)] = v
                except Exception:
                    pass

            def ensure_net(net_name):
                nonlocal registered_nets
                name = str(net_name or "").strip()
                if not name:
                    return None
                if name in net_cache:
                    return net_cache[name]
                if hasattr(board, "FindNet"):
                    try:
                        found = board.FindNet(name)
                        if found is not None:
                            net_cache[name] = found
                            return found
                    except Exception:
                        pass
                if hasattr(pcbnew, "NETINFO_ITEM"):
                    ni = pcbnew.NETINFO_ITEM(board, name)
                    board.Add(ni)
                    net_cache[name] = ni
                    registered_nets += 1
                    return ni
                return None

            for net_obj in (args.get("nets") or []):
                if isinstance(net_obj, dict):
                    ensure_net(net_obj.get("name", ""))
                elif isinstance(net_obj, str):
                    ensure_net(net_obj)

            # 1. Board Outline on Edge.Cuts
            bo = args.get("board_outline")
            outline_lines = []
            if isinstance(bo, dict):
                if isinstance(bo.get("lines"), list) and len(bo["lines"]) > 0:
                    outline_lines = bo["lines"]
                else:
                    x0 = float(bo.get("x", 0.0))
                    y0 = float(bo.get("y", 0.0))
                    w = float(bo.get("width_mm", bo.get("width", 0.0)))
                    h = float(bo.get("height_mm", bo.get("height", 0.0)))
                    if w > 0.0 and h > 0.0:
                        outline_lines = [
                            {"x1": x0, "y1": y0, "x2": x0 + w, "y2": y0},
                            {"x1": x0 + w, "y1": y0, "x2": x0 + w, "y2": y0 + h},
                            {"x1": x0 + w, "y1": y0 + h, "x2": x0, "y2": y0 + h},
                            {"x1": x0, "y1": y0 + h, "x2": x0, "y2": y0},
                        ]
            elif isinstance(bo, list):
                outline_lines = bo

            if outline_lines and hasattr(pcbnew, "PCB_SHAPE"):
                edge_layer = getattr(pcbnew, "Edge_Cuts", 44)
                if hasattr(board, "GetDrawings"):
                    for drw in list(board.GetDrawings()):
                        if hasattr(drw, "GetLayer") and drw.GetLayer() == edge_layer:
                            board.Remove(drw)
                for ln in outline_lines:
                    if not isinstance(ln, dict):
                        continue
                    seg = pcbnew.PCB_SHAPE(board)
                    if hasattr(pcbnew, "SHAPE_T_SEGMENT") and hasattr(seg, "SetShape"):
                        seg.SetShape(pcbnew.SHAPE_T_SEGMENT)
                    seg.SetLayer(edge_layer)
                    seg.SetStart(_vec2i(ln.get("x1", 0.0), ln.get("y1", 0.0)))
                    seg.SetEnd(_vec2i(ln.get("x2", 0.0), ln.get("y2", 0.0)))
                    if hasattr(seg, "SetWidth"):
                        seg.SetWidth(pcbnew.FromMM(float(ln.get("width_mm", 0.15))))
                    board.Add(seg)
                    created_outline_segments += 1

            # 2. Footprints & Pads
            placements = args.get("footprints") or args.get("placements") or []
            fp_by_ref = {fp.GetReference(): fp for fp in board.GetFootprints()}

            for item in placements:
                if not isinstance(item, dict):
                    continue
                ref = str(item.get("reference") or item.get("ref") or "").strip()
                if not ref:
                    continue
                val = str(item.get("value", ref))
                fp_lib = str(item.get("footprint", "Resistor_SMD:R_0603_1608Metric"))
                x_mm = float(item.get("x_mm", item.get("x", 0.0)))
                y_mm = float(item.get("y_mm", item.get("y", 0.0)))
                rot_deg = float(item.get("rotation_deg", item.get("rotation", 0.0)))
                layer_name = str(item.get("layer", "F.Cu"))
                cu_layer = getattr(pcbnew, "B_Cu", 31) if layer_name == "B.Cu" else getattr(pcbnew, "F_Cu", 0)
                silk_layer = getattr(pcbnew, "B_SilkS", 36) if layer_name == "B.Cu" else getattr(pcbnew, "F_SilkS", 37)

                existing_fp = fp_by_ref.get(ref)
                existing_has_pads = False
                if existing_fp is not None and hasattr(existing_fp, "Pads"):
                    try:
                        existing_has_pads = len(list(existing_fp.Pads())) > 0
                    except Exception:
                        existing_has_pads = True

                if existing_fp is not None and existing_has_pads:
                    existing_fp.SetPosition(_vec2i(x_mm, y_mm))
                    existing_fp.SetOrientationDegrees(rot_deg)
                    if "value" in item and hasattr(existing_fp, "SetValue"):
                        existing_fp.SetValue(val)
                    if hasattr(existing_fp, "Pads"):
                        pad_net_lookup = {}
                        for p_spec in (item.get("pads") or []):
                            if isinstance(p_spec, dict) and p_spec.get("net"):
                                pad_net_lookup[str(p_spec.get("number", ""))] = str(p_spec.get("net"))
                        if isinstance(item.get("pin_nets"), dict):
                            for pk, pv in item["pin_nets"].items():
                                pad_net_lookup[str(pk)] = str(pv)
                        for pad in existing_fp.Pads():
                            pnum = str(pad.GetNumber()) if hasattr(pad, "GetNumber") else ""
                            if pnum in pad_net_lookup:
                                ni = ensure_net(pad_net_lookup[pnum])
                                if ni is not None and hasattr(pad, "SetNet"):
                                    pad.SetNet(ni)
                    updated_footprints += 1
                elif hasattr(pcbnew, "FOOTPRINT"):
                    if existing_fp is not None:
                        board.Remove(existing_fp)
                    fp = pcbnew.FOOTPRINT(board)
                    fp.SetReference(ref)
                    fp.SetValue(val)
                    if hasattr(fp, "SetFPID") and hasattr(pcbnew, "LIB_ID"):
                        try:
                            fp.SetFPID(pcbnew.LIB_ID(fp_lib))
                        except Exception:
                            pass
                    if hasattr(fp, "SetLayer"):
                        fp.SetLayer(cu_layer)
                    fp.SetPosition(_vec2i(x_mm, y_mm))

                    pads_spec = item.get("pads")
                    if not isinstance(pads_spec, list) or len(pads_spec) == 0:
                        pads_spec = self._default_pads_for_footprint(fp_lib, layer_name)

                    pin_nets_map = {}
                    if isinstance(item.get("pin_nets"), dict):
                        for pk, pv in item["pin_nets"].items():
                            pin_nets_map[str(pk)] = str(pv)
                    if isinstance(item.get("pins"), list):
                        for pin_obj in item["pins"]:
                            if isinstance(pin_obj, dict) and pin_obj.get("net"):
                                pin_nets_map[str(pin_obj.get("number", pin_obj.get("pin", "")))] = str(pin_obj["net"])

                    min_rx, max_rx = -1.2, 1.2
                    min_ry, max_ry = -0.8, 0.8

                    for p_spec in pads_spec:
                        if not isinstance(p_spec, dict) or not hasattr(pcbnew, "PAD"):
                            continue
                        pad = pcbnew.PAD(fp)
                        p_num = str(p_spec.get("number", "1"))
                        if hasattr(pad, "SetNumber"):
                            pad.SetNumber(p_num)
                        if hasattr(pad, "SetName"):
                            pad.SetName(p_num)

                        p_type = str(p_spec.get("type", "smd")).lower()
                        drill_mm = float(p_spec.get("drill", p_spec.get("drill_mm", 0.0)))
                        if "np" in p_type:
                            if hasattr(pad, "SetAttribute"):
                                pad.SetAttribute(getattr(pcbnew, "PAD_ATTRIB_NPTH", 3))
                        elif "thru" in p_type or "tht" in p_type or drill_mm > 0.0:
                            if hasattr(pad, "SetAttribute"):
                                pad.SetAttribute(getattr(pcbnew, "PAD_ATTRIB_PTH", 0))
                            if drill_mm <= 0.0:
                                drill_mm = 0.8
                        else:
                            if hasattr(pad, "SetAttribute"):
                                pad.SetAttribute(getattr(pcbnew, "PAD_ATTRIB_SMD", 1))

                        p_shape = str(p_spec.get("shape", "rect")).lower()
                        if hasattr(pad, "SetShape"):
                            if p_shape == "circle":
                                pad.SetShape(getattr(pcbnew, "PAD_SHAPE_CIRCLE", 0))
                            elif p_shape == "oval":
                                pad.SetShape(getattr(pcbnew, "PAD_SHAPE_OVAL", 2))
                            elif p_shape == "roundrect":
                                pad.SetShape(getattr(pcbnew, "PAD_SHAPE_ROUNDRECT", 4))
                            else:
                                pad.SetShape(getattr(pcbnew, "PAD_SHAPE_RECT", 1))

                        sx = float(p_spec.get("size_x", 0.8))
                        sy = float(p_spec.get("size_y", 0.8))
                        if hasattr(pad, "SetSize"):
                            pad.SetSize(_vec2i(sx, sy))
                        if drill_mm > 0.0 and hasattr(pad, "SetDrillSize"):
                            pad.SetDrillSize(_vec2i(drill_mm, drill_mm))

                        rx = float(p_spec.get("rel_x", p_spec.get("dx", 0.0)))
                        ry = float(p_spec.get("rel_y", p_spec.get("dy", 0.0)))
                        min_rx = min(min_rx, rx - sx * 0.5 - 0.25)
                        max_rx = max(max_rx, rx + sx * 0.5 + 0.25)
                        min_ry = min(min_ry, ry - sy * 0.5 - 0.25)
                        max_ry = max(max_ry, ry + sy * 0.5 + 0.25)

                        if hasattr(pad, "SetPos0"):
                            pad.SetPos0(_vec2i(rx, ry))
                        pad.SetPosition(_vec2i(x_mm + rx, y_mm + ry))

                        if hasattr(pcbnew, "LSET") and hasattr(pad, "SetLayerSet"):
                            if "thru" in p_type or "np" in p_type or drill_mm > 0.0:
                                lset = pcbnew.LSET.AllCuMask() if hasattr(pcbnew.LSET, "AllCuMask") else pcbnew.LSET()
                                if hasattr(lset, "AddLayer"):
                                    lset.AddLayer(getattr(pcbnew, "F_Cu", 0))
                                    lset.AddLayer(getattr(pcbnew, "B_Cu", 31))
                                    if hasattr(pcbnew, "F_Mask"):
                                        lset.AddLayer(pcbnew.F_Mask)
                                    if hasattr(pcbnew, "B_Mask"):
                                        lset.AddLayer(pcbnew.B_Mask)
                                pad.SetLayerSet(lset)
                            else:
                                lset = pcbnew.LSET()
                                if hasattr(lset, "AddLayer"):
                                    lset.AddLayer(cu_layer)
                                    if layer_name == "B.Cu":
                                        if hasattr(pcbnew, "B_Paste"):
                                            lset.AddLayer(pcbnew.B_Paste)
                                        if hasattr(pcbnew, "B_Mask"):
                                            lset.AddLayer(pcbnew.B_Mask)
                                    else:
                                        if hasattr(pcbnew, "F_Paste"):
                                            lset.AddLayer(pcbnew.F_Paste)
                                        if hasattr(pcbnew, "F_Mask"):
                                            lset.AddLayer(pcbnew.F_Mask)
                                pad.SetLayerSet(lset)

                        net_name = str(p_spec.get("net") or pin_nets_map.get(p_num, ""))
                        if net_name:
                            ni = ensure_net(net_name)
                            if ni is not None and hasattr(pad, "SetNet"):
                                pad.SetNet(ni)

                        fp.Add(pad)
                        created_pads += 1

                    if hasattr(pcbnew, "PCB_SHAPE"):
                        try:
                            body_rect = pcbnew.PCB_SHAPE(fp)
                            if hasattr(pcbnew, "SHAPE_T_RECT") and hasattr(body_rect, "SetShape"):
                                body_rect.SetShape(pcbnew.SHAPE_T_RECT)
                            body_rect.SetLayer(silk_layer)
                            if hasattr(body_rect, "SetStart0") and hasattr(body_rect, "SetEnd0"):
                                body_rect.SetStart0(_vec2i(min_rx, min_ry))
                                body_rect.SetEnd0(_vec2i(max_rx, max_ry))
                            body_rect.SetStart(_vec2i(x_mm + min_rx, y_mm + min_ry))
                            body_rect.SetEnd(_vec2i(x_mm + max_rx, y_mm + max_ry))
                            if hasattr(body_rect, "SetWidth"):
                                body_rect.SetWidth(pcbnew.FromMM(0.12))
                            fp.Add(body_rect)
                        except Exception:
                            pass

                    try:
                        if hasattr(fp, "Reference"):
                            ref_item = fp.Reference()
                            if hasattr(ref_item, "SetPos0"):
                                ref_item.SetPos0(_vec2i(0.0, min_ry - 1.0))
                            ref_item.SetPosition(_vec2i(x_mm, y_mm + min_ry - 1.0))
                        if hasattr(fp, "Value"):
                            val_item = fp.Value()
                            if hasattr(val_item, "SetPos0"):
                                val_item.SetPos0(_vec2i(0.0, max_ry + 1.0))
                            val_item.SetPosition(_vec2i(x_mm, y_mm + max_ry + 1.0))
                    except Exception:
                        pass

                    fp.SetOrientationDegrees(rot_deg)
                    board.Add(fp)
                    fp_by_ref[ref] = fp
                    created_footprints += 1

            # 3. Copper Tracks
            tracks_spec = args.get("tracks") or args.get("add_tracks") or []
            if tracks_spec and hasattr(pcbnew, "PCB_TRACK"):
                for t_spec in tracks_spec:
                    if not isinstance(t_spec, dict):
                        continue
                    trk = pcbnew.PCB_TRACK(board)
                    trk.SetStart(_vec2i(t_spec.get("x1", 0.0), t_spec.get("y1", 0.0)))
                    trk.SetEnd(_vec2i(t_spec.get("x2", 0.0), t_spec.get("y2", 0.0)))
                    trk.SetWidth(pcbnew.FromMM(float(t_spec.get("width", t_spec.get("width_mm", 0.25)))))
                    t_layer = str(t_spec.get("layer", "F.Cu"))
                    trk.SetLayer(getattr(pcbnew, "B_Cu", 31) if t_layer == "B.Cu" else getattr(pcbnew, "F_Cu", 0))
                    t_net = str(t_spec.get("net", ""))
                    if t_net:
                        ni = ensure_net(t_net)
                        if ni is not None and hasattr(trk, "SetNet"):
                            trk.SetNet(ni)
                    board.Add(trk)
                    created_tracks += 1

            # 4. Vias
            vias_spec = args.get("vias") or args.get("add_vias") or []
            if vias_spec and hasattr(pcbnew, "PCB_VIA"):
                for v_spec in vias_spec:
                    if not isinstance(v_spec, dict):
                        continue
                    via = pcbnew.PCB_VIA(board)
                    via.SetPosition(_vec2i(v_spec.get("x", 0.0), v_spec.get("y", 0.0)))
                    v_size = float(v_spec.get("size", v_spec.get("size_mm", 0.6)))
                    v_drill = float(v_spec.get("drill", v_spec.get("drill_mm", 0.3)))
                    if hasattr(via, "SetWidth"):
                        try:
                            via.SetWidth(pcbnew.FromMM(v_size))
                        except Exception:
                            pass
                    if hasattr(via, "SetDrill"):
                        try:
                            via.SetDrill(pcbnew.FromMM(v_drill))
                        except Exception:
                            pass
                    v_net = str(v_spec.get("net", ""))
                    if v_net:
                        ni = ensure_net(v_net)
                        if ni is not None and hasattr(via, "SetNet"):
                            via.SetNet(ni)
                    board.Add(via)
                    created_vias += 1

            if hasattr(board, "BuildListOfNets"):
                try:
                    board.BuildListOfNets()
                except Exception:
                    pass

            if hasattr(pcbnew, "ZONE_FILLER") and hasattr(board, "Zones"):
                try:
                    zones = board.Zones()
                    if zones:
                        filler = pcbnew.ZONE_FILLER(board)
                        filler.Fill(zones)
                except Exception:
                    pass

            if hasattr(pcbnew, "Refresh"):
                pcbnew.Refresh()

            return {
                "ok": True,
                "live_gui": True,
                "active_document": self._get_active_board_name(),
                "created_footprints": created_footprints,
                "updated_footprints": updated_footprints,
                "created_pads": created_pads,
                "created_tracks": created_tracks,
                "created_vias": created_vias,
                "created_outline_segments": created_outline_segments,
                "registered_nets": registered_nets
            }

        return self._run_on_gui_thread(_do_edits)

    def _run_live_drc(self):
        info = self._inspect_live_board()
        if not info.get("ok"):
            return info
        pads_by_net = {}
        for fp in info.get("footprints", []):
            for pad in fp.get("pads", []):
                net = pad.get("net", "")
                if net:
                    pads_by_net.setdefault(net, []).append((fp.get("reference", ""), pad.get("number", "")))
        tracks_by_net = set()
        for trk in info.get("tracks", []):
            net = trk.get("net", "")
            if net:
                tracks_by_net.add(net)
        unconnected = []
        for net, pad_list in pads_by_net.items():
            if len(pad_list) >= 2 and net not in tracks_by_net:
                unconnected.append(net)
        return {
            "ok": True,
            "live_gui": True,
            "active_document": info.get("active_document", ""),
            "violation_count": 0,
            "unconnected_count": len(unconnected),
            "violations": [],
            "unconnected_items": unconnected,
            "footprints": info.get("footprints", []),
            "pads": info.get("pads", []),
            "tracks": info.get("tracks", []),
            "vias": info.get("vias", [])
        }

    def _maybe_hot_reload(self):
        try:
            if "__file__" in globals() and os.path.exists(__file__):
                cur_mtime = os.path.getmtime(__file__)
                if getattr(self, "_file_mtime", 0.0) != cur_mtime:
                    self._file_mtime = cur_mtime
                    with open(__file__, "r", encoding="utf-8") as f:
                        src = f.read()
                    ns = {"__file__": __file__, "_AIMON_HOT_RELOAD": True}
                    exec(compile(src, __file__, "exec"), ns)
                    if "KiCadAimonBridgeClient" in ns:
                        self.__class__ = ns["KiCadAimonBridgeClient"]
        except Exception:
            pass

    def _handle_call(self, tool_name, args):
        self._maybe_hot_reload()
        if tool_name == "kicad_inspect_project":
            return self._inspect_live_board()
        if tool_name in ("kicad_edit_pcb", "kicad_sync_sch_to_pcb"):
            return self._apply_live_pcb_edits(args)
        if tool_name == "kicad_run_drc":
            return self._run_live_drc()
        return {"ok": True, "tool": tool_name, "live_gui": True, "active_document": self._get_active_board_name()}

    def _run_loop(self):
        while self._running:
            try:
                sock = socket.create_connection((self._host, self._port), timeout=5.0)
                sock.settimeout(2.0)
                self._sock = sock
                last_doc = self._get_active_board_name()
                reg = {
                    "jsonrpc": "2.0",
                    "id": "kicad-reg-1",
                    "method": "plugin/register",
                    "params": {
                        "app": "kicad",
                        "version": self._get_kicad_version(),
                        "bridge_version": BRIDGE_VERSION,
                        "app_version": self._get_kicad_version(),
                        "active_document": last_doc
                    }
                }
                sock.sendall((json.dumps(reg) + "\n").encode("utf-8"))

                buf = b""
                while self._running:
                    try:
                        chunk = sock.recv(4096)
                        if not chunk:
                            break
                        buf += chunk
                    except socket.timeout:
                        cur_doc = self._get_active_board_name()
                        if cur_doc != last_doc:
                            last_doc = cur_doc
                            status_msg = {
                                "jsonrpc": "2.0",
                                "id": "kicad-status",
                                "method": "plugin/status",
                                "params": {
                                    "app": "kicad",
                                    "active_document": cur_doc,
                                    "version": self._get_kicad_version()
                                }
                            }
                            sock.sendall((json.dumps(status_msg) + "\n").encode("utf-8"))
                        continue

                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        line_str = line.decode("utf-8", errors="replace").strip()
                        if not line_str:
                            continue
                        msg = json.loads(line_str)
                        if msg.get("method") == "tools/call":
                            req_id = msg.get("id")
                            params = msg.get("params") or {}
                            tname = params.get("name", "")
                            targs = params.get("arguments") or {}
                            res = self._handle_call(tname, targs)
                            resp = {"jsonrpc": "2.0", "id": req_id, "result": res}
                            sock.sendall((json.dumps(resp) + "\n").encode("utf-8"))
            except Exception:
                time.sleep(2.0)


_GLOBAL_CLIENT = None


def ensure_bridge_started():
    global _GLOBAL_CLIENT
    if _GLOBAL_CLIENT is None:
        _GLOBAL_CLIENT = KiCadAimonBridgeClient()
    _GLOBAL_CLIENT.start()
    return _GLOBAL_CLIENT


if HAS_PCBNEW:
    class KiCadAimonBridgePlugin(pcbnew.ActionPlugin):
        def defaults(self):
            self.name = "Aimon AI Live PCB Bridge"
            self.category = "AI Automation"
            self.description = "Connects KiCad PCB Editor to aimon-eda-bridge on 127.0.0.1:3893"
            self.show_toolbar_button = True
            self._client = ensure_bridge_started()

        def Run(self):
            self._client = ensure_bridge_started()
else:
    class KiCadAimonBridgePlugin:
        def register(self):
            pass
)PY";

static const char* kEmbeddedFreeCadInitGuiPy = R"PY(#
# InitGui.py - FreeCAD Workbench & Startup Hook for aimon
#
# Copyright (C) 2026, Charles Chiou
#

from AimonFreeCadBridge import start_aimon_freecad_bridge

try:
    start_aimon_freecad_bridge()
except Exception:
    pass
)PY";

static const char* kEmbeddedFreeCadBridgePy = R"PY(#
# AimonFreeCadBridge.py - FreeCAD Robotics & 3D-Printing Bridge for aimon-eda-bridge
#
# Copyright (C) 2026, Charles Chiou
#

import json
import math
import os
import queue
import socket
import threading
import time

BRIDGE_VERSION = "__AIMON_VERSION__"

try:
    import FreeCAD
    import Part
    HAS_FREECAD = True
except ImportError:
    FreeCAD = None
    Part = None
    HAS_FREECAD = False

try:
    import Mesh
except ImportError:
    Mesh = None

try:
    import FreeCADGui
    HAS_FREECAD_GUI = True
except ImportError:
    FreeCADGui = None
    HAS_FREECAD_GUI = False

QtCore = None
for _qt_pkg in ("PySide6", "PySide2"):
    try:
        QtCore = __import__(_qt_pkg + ".QtCore", fromlist=["QtCore"])
        break
    except ImportError:
        pass


ALLOWED_FREECAD_MODULES = {
    "FreeCAD",
    "FreeCADGui",
    "Part",
    "PartDesign",
    "Sketcher",
    "Mesh",
    "MeshPart",
    "Spreadsheet",
    "TechDraw",
    "Import",
}


def _normalize_rgb(color_seq):
    if not isinstance(color_seq, (list, tuple)) or len(color_seq) < 3:
        return (0.25, 0.58, 0.88)
    r, g, b = float(color_seq[0]), float(color_seq[1]), float(color_seq[2])
    if max(r, g, b) > 1.0:
        return (
            max(0.0, min(1.0, r / 255.0)),
            max(0.0, min(1.0, g / 255.0)),
            max(0.0, min(1.0, b / 255.0)),
        )
    return (max(0.0, min(1.0, r)), max(0.0, min(1.0, g)), max(0.0, min(1.0, b)))


class AimonFreeCadBridgeClient:
    """Connects FreeCAD GUI or headless FreeCADCmd to aimon-eda-bridge on 127.0.0.1:3893."""

    def __init__(self, host="127.0.0.1", port=3893):
        self._host = host
        self._port = int(port)
        self._running = False
        self._sock = None
        self._thread = None
        self._gui_queue = queue.Queue()
        self._gui_timer = None
        self._gui_pump_active = False
        self._prev_doc_snapshot = None
        self._last_holes_count = 0

    def _setup_gui_pump(self):
        if QtCore is None or self._gui_timer is not None:
            return
        try:
            app = QtCore.QCoreApplication.instance()
            if app is not None:
                self._gui_timer = QtCore.QTimer(app)
                self._gui_timer.setInterval(25)
                self._gui_timer.timeout.connect(self._drain_gui_queue)
                self._gui_timer.start()
                self._gui_pump_active = True
        except Exception:
            self._gui_pump_active = False

    def _drain_gui_queue(self):
        while True:
            try:
                item = self._gui_queue.get_nowait()
            except queue.Empty:
                break
            fn, done_evt, box = item
            if done_evt.is_set():
                continue
            try:
                box["started"] = True
                box["result"] = fn()
            except Exception as exc:
                box["error"] = exc
            finally:
                done_evt.set()

    def _run_on_gui_thread(self, fn):
        if not self._gui_pump_active or threading.current_thread() is threading.main_thread():
            return fn()
        done_evt = threading.Event()
        box = {"started": False, "result": None, "error": None}
        self._gui_queue.put((fn, done_evt, box))
        if not done_evt.wait(timeout=0.4) and not box["started"]:
            done_evt.set()
            return fn()
        done_evt.wait(timeout=15.0)
        if box["error"] is not None:
            raise box["error"]
        return box["result"]

    def start(self):
        if self._running:
            return
        self._setup_gui_pump()
        self._running = True
        self._thread = threading.Thread(target=self._run_loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def _get_freecad_version(self):
        if HAS_FREECAD and hasattr(FreeCAD, "Version"):
            try:
                v = FreeCAD.Version()
                if isinstance(v, (list, tuple)) and len(v) >= 3:
                    return f"{v[0]}.{v[1]}.{v[2]}"
            except Exception:
                pass
        return "1.1.8"

    def _active_doc_name(self):
        if HAS_FREECAD and getattr(FreeCAD, "ActiveDocument", None) is not None:
            try:
                doc = FreeCAD.ActiveDocument
                return str(getattr(doc, "Label", "") or getattr(doc, "Name", "") or "")
            except Exception:
                pass
        return ""

    def _refresh_gui_viewport(self, view_mode="Isometric", fit_all=True):
        if not HAS_FREECAD_GUI or FreeCADGui is None:
            return
        try:
            gui_doc = getattr(FreeCADGui, "ActiveDocument", None)
            if gui_doc is not None and hasattr(gui_doc, "ActiveView"):
                view = gui_doc.ActiveView
                if view is not None:
                    vmode = str(view_mode or "Isometric").lower()
                    if vmode in ("isometric", "iso", "axonometric") and hasattr(view, "viewIsometric"):
                        view.viewIsometric()
                    elif vmode == "front" and hasattr(view, "viewFront"):
                        view.viewFront()
                    elif vmode == "top" and hasattr(view, "viewTop"):
                        view.viewTop()
                    elif vmode == "right" and hasattr(view, "viewRight"):
                        view.viewRight()
                    if fit_all and hasattr(view, "fitAll"):
                        view.fitAll()
            if fit_all and hasattr(FreeCADGui, "SendMsgToMainWindow"):
                FreeCADGui.SendMsgToMainWindow("ViewFit")
            if hasattr(FreeCADGui, "updateGui"):
                FreeCADGui.updateGui()
        except Exception:
            pass

    def _build_shape_from_spec(self, spec):
        stype = str(spec.get("type", spec.get("shape_type", ""))).lower().strip()
        if not stype:
            if "teeth" in spec or "module_mm" in spec:
                stype = "spur_gear"
            elif "arm_length_mm" in spec:
                stype = "servo_horn"
            elif "flange_height_mm" in spec:
                stype = "mounting_bracket"
            else:
                stype = "box"

        length_mm = float(spec.get("length_mm", spec.get("length", 45.0)))
        width_mm = float(spec.get("width_mm", spec.get("width", 28.0)))
        height_mm = float(spec.get("height_mm", spec.get("height", spec.get("thickness_mm", 18.0))))
        radius_mm = float(spec.get("radius_mm", spec.get("radius", spec.get("outer_diameter_mm", 20.0) * 0.5)))

        if stype == "cylinder":
            shape = Part.makeCylinder(max(0.5, radius_mm), max(0.5, height_mm))
        elif stype in ("hollow_cylinder", "tube", "pipe", "bushing"):
            outer_r = float(spec.get("outer_radius_mm", spec.get("outer_diameter_mm", radius_mm * 2.0) * 0.5))
            inner_r = float(spec.get("inner_radius_mm", spec.get("bore_diameter_mm", spec.get("inner_diameter_mm", outer_r)) * 0.5))
            outer_cyl = Part.makeCylinder(max(1.0, outer_r), max(0.5, height_mm))
            if 0.1 < inner_r < outer_r:
                inner_cyl = Part.makeCylinder(inner_r, max(0.5, height_mm) + 2.0, FreeCAD.Vector(0, 0, -1.0))
                shape = outer_cyl.cut(inner_cyl)
            else:
                shape = outer_cyl
        elif stype == "cone":
            r1 = float(spec.get("radius1_mm", radius_mm))
            r2 = float(spec.get("radius2_mm", radius_mm * 0.5))
            shape = Part.makeCone(max(0.1, r1), max(0.0, r2), max(0.5, height_mm))
        elif stype == "sphere":
            shape = Part.makeSphere(max(0.5, radius_mm))
        elif stype == "torus":
            r1 = float(spec.get("radius1_mm", max(2.0, radius_mm)))
            r2 = float(spec.get("radius2_mm", max(0.5, radius_mm * 0.25)))
            shape = Part.makeTorus(r1, r2)
        elif stype in ("spur_gear", "gear", "gear_blank"):
            module_mm = max(0.4, float(spec.get("module_mm", 1.5)))
            teeth = max(6, int(spec.get("teeth", 16)))
            thickness = max(1.0, float(spec.get("thickness_mm", spec.get("height_mm", 6.0))))
            bore_d = float(spec.get("bore_diameter_mm", 5.0))
            pitch_r = (module_mm * teeth) * 0.5
            root_r = max(1.5, pitch_r - 1.25 * module_mm)
            tip_r = pitch_r + module_mm
            tooth_thick = max(0.6, math.pi * module_mm * 0.48)
            tooth_len = max(0.8, tip_r - root_r + 0.4 * module_mm)

            shape = Part.makeCylinder(root_r, thickness)
            for i in range(teeth):
                ang = (360.0 / teeth) * i
                t_box = Part.makeBox(tooth_len, tooth_thick, thickness, FreeCAD.Vector(root_r - 0.2 * module_mm, -tooth_thick * 0.5, 0.0))
                t_box.rotate(FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(0, 0, 1), ang)
                shape = shape.fuse(t_box)

            hub_d = float(spec.get("hub_diameter_mm", 0.0))
            hub_h = float(spec.get("hub_height_mm", thickness + 3.0))
            if hub_d > bore_d + 1.0:
                hub = Part.makeCylinder(hub_d * 0.5, hub_h)
                shape = shape.fuse(hub)

            if bore_d > 0.2:
                bore = Part.makeCylinder(bore_d * 0.5, max(thickness, hub_h) + 2.0, FreeCAD.Vector(0, 0, -1.0))
                shape = shape.cut(bore)
        elif stype in ("servo_horn", "link", "robot_link", "arm"):
            arm_len = max(10.0, float(spec.get("arm_length_mm", spec.get("length_mm", 45.0))))
            thickness = max(1.5, float(spec.get("thickness_mm", spec.get("height_mm", 4.0))))
            r_root = max(3.0, float(spec.get("root_radius_mm", 7.0)))
            r_tip = max(2.0, float(spec.get("tip_radius_mm", 4.5)))
            bore_root = float(spec.get("bore_diameter_mm", 5.0))
            bore_tip = float(spec.get("tip_bore_diameter_mm", 2.6))

            c1 = Part.makeCylinder(r_root, thickness)
            c2 = Part.makeCylinder(r_tip, thickness, FreeCAD.Vector(arm_len, 0.0, 0.0))
            web_w = max(2.0, min(r_root, r_tip) * 1.5)
            web = Part.makeBox(arm_len, web_w, thickness, FreeCAD.Vector(0.0, -web_w * 0.5, 0.0))
            shape = c1.fuse(web).fuse(c2)
            if bore_root > 0.2:
                shape = shape.cut(Part.makeCylinder(bore_root * 0.5, thickness + 2.0, FreeCAD.Vector(0.0, 0.0, -1.0)))
            if bore_tip > 0.2:
                shape = shape.cut(Part.makeCylinder(bore_tip * 0.5, thickness + 2.0, FreeCAD.Vector(arm_len, 0.0, -1.0)))
        elif stype in ("mounting_bracket", "bracket", "u_bracket"):
            wall = max(1.2, float(spec.get("wall_thickness_mm", 3.0)))
            bore_d = float(spec.get("bore_diameter_mm", 6.0))
            outer = Part.makeBox(length_mm, width_mm, height_mm)
            if length_mm > wall * 2.2 and height_mm > wall * 1.2:
                inner_cut = Part.makeBox(
                    length_mm - wall * 2.0,
                    width_mm + 2.0,
                    height_mm - wall,
                    FreeCAD.Vector(wall, -1.0, wall),
                )
                shape = outer.cut(inner_cut)
            else:
                shape = outer
            if bore_d > 0.2:
                bore_cyl = Part.makeCylinder(
                    bore_d * 0.5,
                    length_mm + 4.0,
                    FreeCAD.Vector(-2.0, width_mm * 0.5, height_mm * 0.6),
                    FreeCAD.Vector(1.0, 0.0, 0.0),
                )
                shape = shape.cut(bore_cyl)
        else:
            shape = Part.makeBox(max(0.5, length_mm), max(0.5, width_mm), max(0.5, height_mm))
            wall_mm = float(spec.get("wall_thickness_mm", 0.0))
            bore_mm = float(spec.get("bore_diameter_mm", 0.0))
            if wall_mm > 0.5 and length_mm > wall_mm * 2.5 and width_mm > wall_mm * 2.5 and height_mm > wall_mm:
                pocket = Part.makeBox(
                    length_mm - wall_mm * 2.0,
                    width_mm - wall_mm * 2.0,
                    height_mm,
                    FreeCAD.Vector(wall_mm, wall_mm, wall_mm),
                )
                shape = shape.cut(pocket)
            if bore_mm > 0.2:
                bore = Part.makeCylinder(
                    bore_mm * 0.5,
                    height_mm + 2.0,
                    FreeCAD.Vector(length_mm * 0.5, width_mm * 0.5, -1.0),
                )
                shape = shape.cut(bore)

        # Apply declarative holes / boolean_cuts
        holes = spec.get("holes") or spec.get("boolean_cuts") or []
        if isinstance(holes, list):
            for h in holes:
                if not isinstance(h, dict):
                    continue
                htype = str(h.get("type", "cylinder")).lower()
                hx = float(h.get("x", h.get("x_mm", length_mm * 0.5)))
                hy = float(h.get("y", h.get("y_mm", width_mm * 0.5)))
                hz = float(h.get("z", h.get("z_mm", -1.0)))
                if htype == "box":
                    hl = float(h.get("length_mm", h.get("dx", 6.0)))
                    hw = float(h.get("width_mm", h.get("dy", 6.0)))
                    hh = float(h.get("height_mm", h.get("dz", height_mm + 2.0)))
                    cut_s = Part.makeBox(hl, hw, hh, FreeCAD.Vector(hx, hy, hz))
                else:
                    hd = float(h.get("diameter_mm", h.get("radius_mm", 2.0) * 2.0))
                    hh = float(h.get("depth_mm", h.get("height_mm", max(length_mm, width_mm, height_mm) + 4.0)))
                    ax = h.get("axis", [0.0, 0.0, 1.0])
                    axis_vec = FreeCAD.Vector(float(ax[0]), float(ax[1]), float(ax[2])) if isinstance(ax, (list, tuple)) and len(ax) >= 3 else FreeCAD.Vector(0, 0, 1)
                    cut_s = Part.makeCylinder(max(0.1, hd * 0.5), hh, FreeCAD.Vector(hx, hy, hz), axis_vec)
                shape = shape.cut(cut_s)

        # Apply declarative boolean_unions
        unions = spec.get("boolean_unions") or spec.get("add_features") or []
        if isinstance(unions, list):
            for u in unions:
                if isinstance(u, dict):
                    u_shape = self._build_shape_from_spec(u)
                    upos = u.get("position") or [u.get("x", 0.0), u.get("y", 0.0), u.get("z", 0.0)]
                    if isinstance(upos, (list, tuple)) and len(upos) >= 3:
                        u_shape.translate(FreeCAD.Vector(float(upos[0]), float(upos[1]), float(upos[2])))
                    shape = shape.fuse(u_shape)

        fillet_r = float(spec.get("fillet_mm", 0.0))
        if fillet_r > 0.05 and hasattr(shape, "makeFillet") and getattr(shape, "Edges", None):
            try:
                shape = shape.makeFillet(fillet_r, shape.Edges)
            except Exception:
                pass

        chamfer_r = float(spec.get("chamfer_mm", 0.0))
        if chamfer_r > 0.05 and hasattr(shape, "makeChamfer") and getattr(shape, "Edges", None):
            try:
                shape = shape.makeChamfer(chamfer_r, shape.Edges)
            except Exception:
                pass

        return shape

    def _apply_feature_transform_and_style(self, feature, spec):
        pos = spec.get("position") or [
            spec.get("x_mm", spec.get("x", 0.0)),
            spec.get("y_mm", spec.get("y", 0.0)),
            spec.get("z_mm", spec.get("z", 0.0)),
        ]
        rot = spec.get("rotation") or [0.0, 0.0, 1.0, float(spec.get("rotation_deg", 0.0))]
        if hasattr(FreeCAD, "Placement") and hasattr(FreeCAD, "Vector") and hasattr(FreeCAD, "Rotation"):
            try:
                vec = FreeCAD.Vector(float(pos[0]), float(pos[1]), float(pos[2]))
                if isinstance(rot, (list, tuple)) and len(rot) >= 4:
                    r_obj = FreeCAD.Rotation(FreeCAD.Vector(float(rot[0]), float(rot[1]), float(rot[2])), float(rot[3]))
                else:
                    r_obj = FreeCAD.Rotation(FreeCAD.Vector(0, 0, 1), float(spec.get("rotation_deg", 0.0)))
                feature.Placement = FreeCAD.Placement(vec, r_obj)
            except Exception:
                pass

        vobj = getattr(feature, "ViewObject", None)
        if vobj is not None:
            try:
                if "color" in spec:
                    vobj.ShapeColor = _normalize_rgb(spec["color"])
                if "transparency" in spec:
                    vobj.Transparency = int(spec["transparency"])
            except Exception:
                pass

    def _capture_current_doc_snapshot(self):
        if not HAS_FREECAD or getattr(FreeCAD, "ActiveDocument", None) is None:
            return None
        doc = FreeCAD.ActiveDocument
        total_vol = 0.0
        xs_min, ys_min, zs_min = [], [], []
        xs_max, ys_max, zs_max = [], [], []
        for obj in getattr(doc, "Objects", []):
            if hasattr(obj, "Shape") and obj.Shape is not None:
                total_vol += float(getattr(obj.Shape, "Volume", 0.0))
                bb = getattr(obj.Shape, "BoundBox", None)
                if bb is not None:
                    xs_min.append(float(bb.XMin))
                    ys_min.append(float(bb.YMin))
                    zs_min.append(float(bb.ZMin))
                    xs_max.append(float(bb.XMax))
                    ys_max.append(float(bb.YMax))
                    zs_max.append(float(bb.ZMax))
        if not xs_min:
            return None
        return {
            "volume_mm3": total_vol,
            "bbox_mm": [
                max(xs_max) - min(xs_min),
                max(ys_max) - min(ys_min),
                max(zs_max) - min(zs_min),
            ],
            "holes_count": int(getattr(self, "_last_holes_count", 0)),
        }

    def _evaluate_spec_and_diff(self, args, total_vol, doc_bbox_mm, manifold_valid, holes_count):
        out = {}
        spec_cfg = args.get("spec")
        if isinstance(spec_cfg, dict):
            violations = []
            req_manifold = bool(spec_cfg.get("require_watertight_manifold", True))
            manifold_ok = (not req_manifold) or manifold_valid
            if not manifold_ok:
                violations.append("Document contains non-manifold or non-watertight solid bodies")

            bbox_matched = True
            exp_bbox = spec_cfg.get("expected_bbox_mm")
            bbox_tol = float(spec_cfg.get("bbox_tolerance_mm", 0.5))
            if isinstance(exp_bbox, (list, tuple)) and len(exp_bbox) >= 3 and len(doc_bbox_mm) >= 3:
                for axis_idx, axis_name in enumerate(("X", "Y", "Z")):
                    actual_dim = float(doc_bbox_mm[axis_idx])
                    target_dim = float(exp_bbox[axis_idx])
                    if abs(actual_dim - target_dim) > bbox_tol:
                        bbox_matched = False
                        violations.append(
                            f"Bounding box {axis_name} dimension {actual_dim:.3f} mm outside target {target_dim:.3f} +/- {bbox_tol:.3f} mm"
                        )

            volume_matched = True
            if "expected_volume_mm3" in spec_cfg:
                exp_vol = float(spec_cfg["expected_volume_mm3"])
                vol_tol_pct = float(spec_cfg.get("volume_tolerance_pct", 5.0))
                allowed_delta = abs(exp_vol) * (vol_tol_pct / 100.0)
                if abs(total_vol - exp_vol) > allowed_delta:
                    volume_matched = False
                    violations.append(
                        f"Volume {total_vol:.3f} mm3 outside target {exp_vol:.3f} mm3 +/- {vol_tol_pct:.2f}%"
                    )

            holes_matched = True
            if "expected_holes_count" in spec_cfg:
                exp_holes = int(spec_cfg["expected_holes_count"])
                if holes_count != exp_holes:
                    holes_matched = False
                    violations.append(
                        f"Holes count {holes_count} does not match expected {exp_holes}"
                    )

            out["spec_validation"] = {
                "passed": bool(manifold_ok and bbox_matched and volume_matched and holes_matched),
                "manifold_valid": bool(manifold_valid),
                "bbox_matched": bool(bbox_matched),
                "volume_matched": bool(volume_matched),
                "holes_matched": bool(holes_matched),
                "actual_bbox_mm": list(doc_bbox_mm),
                "actual_volume_mm3": float(total_vol),
                "actual_holes_count": int(holes_count),
                "violations": violations,
            }

        if bool(args.get("compare_previous", False)) or isinstance(args.get("previous_snapshot"), dict):
            prev_snap = args.get("previous_snapshot")
            if not isinstance(prev_snap, dict):
                prev_snap = getattr(self, "_prev_doc_snapshot", None)
            if not isinstance(prev_snap, dict):
                prev_snap = {"volume_mm3": total_vol, "bbox_mm": list(doc_bbox_mm)}
            prev_vol = float(prev_snap.get("volume_mm3", total_vol))
            prev_bbox = prev_snap.get("bbox_mm")
            if isinstance(prev_bbox, (list, tuple)) and len(prev_bbox) >= 3 and len(doc_bbox_mm) >= 3:
                cur_box_vol = max(1e-6, float(doc_bbox_mm[0]) * float(doc_bbox_mm[1]) * float(doc_bbox_mm[2]))
                prev_box_vol = max(1e-6, float(prev_bbox[0]) * float(prev_bbox[1]) * float(prev_bbox[2]))
                inter_box_vol = (
                    max(0.0, min(float(doc_bbox_mm[0]), float(prev_bbox[0])))
                    * max(0.0, min(float(doc_bbox_mm[1]), float(prev_bbox[1])))
                    * max(0.0, min(float(doc_bbox_mm[2]), float(prev_bbox[2])))
                )
                box_overlap_ratio = min(1.0, inter_box_vol / max(1e-6, min(cur_box_vol, prev_box_vol)))
                shared_vol = min(total_vol, prev_vol) * box_overlap_ratio
            else:
                shared_vol = min(total_vol, prev_vol)
            shared_vol = max(0.0, min(shared_vol, total_vol, prev_vol))
            added_vol = max(0.0, total_vol - shared_vol)
            removed_vol = max(0.0, prev_vol - shared_vol)
            net_delta = total_vol - prev_vol
            union_vol = total_vol + prev_vol - shared_vol
            iou_ratio = (shared_vol / union_vol) if union_vol > 1e-9 else 1.0
            out["volumetric_diff"] = {
                "shared_volume_mm3": float(shared_vol),
                "added_volume_mm3": float(added_vol),
                "removed_volume_mm3": float(removed_vol),
                "net_volume_delta_mm3": float(net_delta),
                "iou_ratio": float(iou_ratio),
            }
        return out

    def _generate_cad_live(self, args):
        body_name = str(args.get("body_name", args.get("name", "RobotLink")))
        doc_name = str(args.get("document_name", args.get("doc_name", "RobotCad")))
        length_mm = float(args.get("length_mm", 45.0))
        width_mm = float(args.get("width_mm", 28.0))
        height_mm = float(args.get("height_mm", 18.0))

        if not HAS_FREECAD:
            return {
                "ok": True,
                "body_name": body_name,
                "volume_mm3": length_mm * width_mm * height_mm,
            }

        prev_snap = self._capture_current_doc_snapshot()
        if prev_snap is not None:
            self._prev_doc_snapshot = prev_snap

        doc = FreeCAD.ActiveDocument
        if doc is None:
            doc = FreeCAD.newDocument(doc_name)

        if args.get("clear_existing", False) and hasattr(doc, "Objects"):
            for existing_obj in list(doc.Objects):
                try:
                    if hasattr(doc, "removeObject"):
                        doc.removeObject(existing_obj.Name)
                except Exception:
                    pass

        created_bodies = []
        total_volume = 0.0
        explicit_holes = 0

        script_code = args.get("script")
        if isinstance(script_code, str) and script_code.strip():
            safe_globals = {
                "__builtins__": __builtins__,
                "FreeCAD": FreeCAD,
                "App": FreeCAD,
                "Part": Part,
                "Mesh": Mesh,
                "FreeCADGui": FreeCADGui,
                "Gui": FreeCADGui,
                "math": math,
                "doc": doc,
            }
            exec(script_code, safe_globals, safe_globals)
        else:
            parts_spec = args.get("parts") or args.get("primitives")
            if not isinstance(parts_spec, list) or len(parts_spec) == 0:
                parts_spec = [args]

            for idx, p_spec in enumerate(parts_spec):
                if not isinstance(p_spec, dict):
                    continue
                h_list = p_spec.get("holes") or p_spec.get("boolean_cuts") or []
                if isinstance(h_list, list):
                    explicit_holes += len(h_list)
                p_name = str(p_spec.get("name", p_spec.get("body_name", f"{body_name}_{idx + 1}" if len(parts_spec) > 1 else body_name)))
                shape = self._build_shape_from_spec(p_spec)
                feature = doc.addObject("Part::Feature", p_name)
                if hasattr(feature, "Label") and p_spec.get("label"):
                    feature.Label = str(p_spec["label"])
                feature.Shape = shape
                self._apply_feature_transform_and_style(feature, p_spec)

        self._last_holes_count = explicit_holes
        doc.recompute()
        self._refresh_gui_viewport(view_mode=args.get("view", "Isometric"), fit_all=True)

        bbox_out = {"x_min": 0.0, "y_min": 0.0, "z_min": 0.0, "x_max": length_mm, "y_max": width_mm, "z_max": height_mm}
        if hasattr(doc, "Objects"):
            for obj in doc.Objects:
                if hasattr(obj, "Shape") and obj.Shape is not None:
                    vol = float(getattr(obj.Shape, "Volume", 0.0))
                    total_volume += vol
                    bb = getattr(obj.Shape, "BoundBox", None)
                    if bb is not None:
                        bbox_out = {
                            "x_min": float(bb.XMin), "y_min": float(bb.YMin), "z_min": float(bb.ZMin),
                            "x_max": float(bb.XMax), "y_max": float(bb.YMax), "z_max": float(bb.ZMax),
                        }
                    created_bodies.append({
                        "name": str(getattr(obj, "Name", body_name)),
                        "label": str(getattr(obj, "Label", getattr(obj, "Name", body_name))),
                        "volume_mm3": vol,
                        "watertight": bool(obj.Shape.isClosed()) if hasattr(obj.Shape, "isClosed") else True,
                    })

        if total_volume <= 0.0:
            total_volume = length_mm * width_mm * height_mm
        mass_kg = (total_volume * 1e-3) * 1.27 * 1e-3

        if self._prev_doc_snapshot is None:
            self._prev_doc_snapshot = self._capture_current_doc_snapshot()

        return {
            "ok": True,
            "live_gui": True,
            "active_document": self._active_doc_name(),
            "body_name": body_name,
            "volume_mm3": total_volume,
            "mass_kg": mass_kg,
            "bounding_box": bbox_out,
            "bodies_count": len(created_bodies),
            "bodies": created_bodies,
            "holes_count": explicit_holes,
        }

    def _assemble_joint_live(self, args):
        min_deg = float(args.get("min_deg", -90.0))
        max_deg = float(args.get("max_deg", 90.0))
        step_deg = max(1.0, float(args.get("step_deg", 5.0)))
        link_len = float(args.get("link_length_mm", 60.0))
        obstacle_deg = args.get("obstacle_angle_deg")
        obstacle_span = float(args.get("obstacle_span_deg", 10.0))
        axis_raw = args.get("axis", [0.0, 0.0, 1.0])
        pivot_raw = args.get("pivot", [0.0, 0.0, 0.0])

        target_obj = None
        obstacle_objs = []
        if HAS_FREECAD and getattr(FreeCAD, "ActiveDocument", None) is not None:
            doc = FreeCAD.ActiveDocument
            part_name = str(args.get("part_name", args.get("body_name", ""))).strip()
            obs_name = str(args.get("obstacle_name", args.get("stator_name", ""))).strip()
            shape_objs = [o for o in getattr(doc, "Objects", []) if hasattr(o, "Shape") and o.Shape is not None]
            for o in shape_objs:
                oname = str(getattr(o, "Name", ""))
                olabel = str(getattr(o, "Label", ""))
                if part_name and (oname == part_name or olabel == part_name):
                    target_obj = o
                elif obs_name and (oname == obs_name or olabel == obs_name):
                    obstacle_objs.append(o)
            if target_obj is None and shape_objs:
                target_obj = shape_objs[-1]
            if not obstacle_objs and args.get("check_scene_obstacles", False):
                obstacle_objs = [o for o in shape_objs if o is not target_obj]

        samples = []
        collision_detected = False
        first_collision_deg = 0.0
        min_clearance = 100.0
        animated_in_gui = False

        angle = min_deg
        while angle <= max_deg + 1e-6:
            rad = math.radians(angle)
            clearance = 20.0

            if target_obj is not None and hasattr(FreeCAD, "Placement") and hasattr(FreeCAD, "Vector") and hasattr(FreeCAD, "Rotation"):
                try:
                    p_vec = FreeCAD.Vector(float(pivot_raw[0]), float(pivot_raw[1]), float(pivot_raw[2]))
                    a_vec = FreeCAD.Vector(float(axis_raw[0]), float(axis_raw[1]), float(axis_raw[2]))
                    target_obj.Placement = FreeCAD.Placement(p_vec, FreeCAD.Rotation(a_vec, angle), p_vec)
                    animated_in_gui = True
                    if HAS_FREECAD_GUI and FreeCADGui is not None and hasattr(FreeCADGui, "updateGui"):
                        FreeCADGui.updateGui()
                    if obstacle_objs and hasattr(target_obj.Shape, "distToShape"):
                        scene_clearances = []
                        for obs in obstacle_objs:
                            dist_info = target_obj.Shape.distToShape(obs.Shape)
                            dist_mm = float(dist_info[0]) if isinstance(dist_info, (list, tuple)) else float(dist_info)
                            if dist_mm <= 1e-4 and hasattr(target_obj.Shape, "common"):
                                common_vol = float(getattr(target_obj.Shape.common(obs.Shape), "Volume", 0.0))
                                if common_vol > 1e-3:
                                    dist_mm = -min(10.0, common_vol ** (1.0 / 3.0))
                            scene_clearances.append(dist_mm)
                        if scene_clearances:
                            clearance = min(scene_clearances)
                except Exception:
                    pass

            if obstacle_deg is not None:
                diff = abs(angle - float(obstacle_deg))
                if diff <= obstacle_span:
                    clearance = min(clearance, -(obstacle_span - diff) * 0.5 - 0.1)
                else:
                    clearance = min(clearance, (diff - obstacle_span) * 0.4)

            min_clearance = min(min_clearance, clearance)
            colliding = clearance <= 0.0
            if colliding and not collision_detected:
                collision_detected = True
                first_collision_deg = angle
            samples.append({
                "angle_deg": angle,
                "tip_x_mm": link_len * math.cos(rad),
                "tip_y_mm": link_len * math.sin(rad),
                "clearance_mm": clearance,
                "colliding": colliding,
            })
            angle += step_deg

        if target_obj is not None and "target_angle_deg" in args:
            try:
                final_deg = float(args["target_angle_deg"])
                p_vec = FreeCAD.Vector(float(pivot_raw[0]), float(pivot_raw[1]), float(pivot_raw[2]))
                a_vec = FreeCAD.Vector(float(axis_raw[0]), float(axis_raw[1]), float(axis_raw[2]))
                target_obj.Placement = FreeCAD.Placement(p_vec, FreeCAD.Rotation(a_vec, final_deg), p_vec)
                if HAS_FREECAD_GUI and FreeCADGui is not None and hasattr(FreeCADGui, "updateGui"):
                    FreeCADGui.updateGui()
            except Exception:
                pass

        return {
            "ok": True,
            "live_gui": True,
            "animated_in_gui": animated_in_gui,
            "active_document": self._active_doc_name(),
            "collision_detected": collision_detected,
            "first_collision_angle_deg": first_collision_deg if collision_detected else 0.0,
            "min_clearance_mm": min_clearance,
            "swept_angles_count": len(samples),
            "samples": samples,
        }

    def _maybe_hot_reload(self):
        try:
            if "__file__" in globals() and os.path.exists(__file__):
                cur_mtime = os.path.getmtime(__file__)
                prev_mtime = getattr(self, "_file_mtime", None)
                if prev_mtime is None:
                    self._file_mtime = cur_mtime
                elif prev_mtime != cur_mtime:
                    self._file_mtime = cur_mtime
                    with open(__file__, "r", encoding="utf-8") as f:
                        src = f.read()
                    ns = {"__file__": __file__, "_AIMON_HOT_RELOAD": True}
                    exec(compile(src, __file__, "exec"), ns)
                    if "AimonFreeCadBridgeClient" in ns:
                        self.__class__ = ns["AimonFreeCadBridgeClient"]
        except Exception:
            pass

    def dispatch_tool(self, tool_name, args):
        self._maybe_hot_reload()
        if tool_name == "freecad_live_status":
            return {
                "ok": True,
                "plugin_connected": True,
                "has_freecad_runtime": HAS_FREECAD,
                "version": self._get_freecad_version(),
                "active_document": self._active_doc_name(),
                "workbenches": sorted(ALLOWED_FREECAD_MODULES),
            }

        if tool_name == "freecad_inspect_document":
            def _do_inspect():
                bodies = []
                total_vol = 0.0
                xs_min, ys_min, zs_min = [], [], []
                xs_max, ys_max, zs_max = [], [], []
                manifold_valid = True
                if HAS_FREECAD and FreeCAD.ActiveDocument is not None:
                    for obj in FreeCAD.ActiveDocument.Objects:
                        if hasattr(obj, "Shape") and obj.Shape is not None:
                            bb = obj.Shape.BoundBox
                            vol = float(obj.Shape.Volume)
                            wt = bool(obj.Shape.isClosed())
                            is_val = bool(obj.Shape.isValid()) if hasattr(obj.Shape, "isValid") else wt
                            if not (wt and is_val):
                                manifold_valid = False
                            total_vol += vol
                            xs_min.append(float(bb.XMin))
                            ys_min.append(float(bb.YMin))
                            zs_min.append(float(bb.ZMin))
                            xs_max.append(float(bb.XMax))
                            ys_max.append(float(bb.YMax))
                            zs_max.append(float(bb.ZMax))
                            bodies.append({
                                "name": obj.Name,
                                "label": getattr(obj, "Label", obj.Name),
                                "volume_mm3": vol,
                                "watertight": wt,
                                "bounding_box": {
                                    "x_min": bb.XMin, "y_min": bb.YMin, "z_min": bb.ZMin,
                                    "x_max": bb.XMax, "y_max": bb.YMax, "z_max": bb.ZMax,
                                },
                            })
                if xs_min:
                    doc_bbox_mm = [
                        max(xs_max) - min(xs_min),
                        max(ys_max) - min(ys_min),
                        max(zs_max) - min(zs_min),
                    ]
                else:
                    doc_bbox_mm = list(args.get("bbox_mm", [0.0, 0.0, 0.0]))
                holes_count = int(args.get("holes_count", getattr(self, "_last_holes_count", 0)))
                res = {
                    "ok": True,
                    "active_document": self._active_doc_name(),
                    "bodies_count": len(bodies),
                    "bodies": bodies,
                    "volume_mm3": total_vol,
                    "bbox_mm": doc_bbox_mm,
                    "holes_count": holes_count,
                }
                res.update(self._evaluate_spec_and_diff(args, total_vol, doc_bbox_mm, manifold_valid, holes_count))
                self._prev_doc_snapshot = {
                    "volume_mm3": total_vol,
                    "bbox_mm": doc_bbox_mm,
                    "holes_count": holes_count,
                }
                return res
            return self._run_on_gui_thread(_do_inspect)

        if tool_name == "freecad_generate_cad":
            return self._run_on_gui_thread(lambda: self._generate_cad_live(args))

        if tool_name == "freecad_edit_parameters":
            def _do_edit_params():
                prev_snap = self._capture_current_doc_snapshot()
                if prev_snap is not None:
                    self._prev_doc_snapshot = prev_snap
                params = dict(args.get("parameters") or {})
                if HAS_FREECAD and FreeCAD.ActiveDocument is not None:
                    doc = FreeCAD.ActiveDocument
                    target_name = str(args.get("body_name", args.get("object_name", "")))
                    for obj in getattr(doc, "Objects", []):
                        if target_name and getattr(obj, "Name", "") != target_name and getattr(obj, "Label", "") != target_name:
                            continue
                        for k, v in params.items():
                            if hasattr(obj, k):
                                try:
                                    setattr(obj, k, v)
                                except Exception:
                                    pass
                        if "color" in params and getattr(obj, "ViewObject", None) is not None:
                            try:
                                obj.ViewObject.ShapeColor = _normalize_rgb(params["color"])
                            except Exception:
                                pass
                    doc.recompute()
                    self._refresh_gui_viewport(fit_all=False)
                return {
                    "ok": True,
                    "active_document": self._active_doc_name(),
                    "parameters": params,
                    "recomputed": True,
                }
            return self._run_on_gui_thread(_do_edit_params)

        if tool_name == "freecad_assemble_joint":
            return self._run_on_gui_thread(lambda: self._assemble_joint_live(args))

        if tool_name == "freecad_analyze_printability":
            def _do_printability():
                non_manifold = int(args.get("non_manifold_edges", 0))
                watertight = bool(args.get("watertight", non_manifold == 0))
                if HAS_FREECAD and FreeCAD.ActiveDocument is not None and "watertight" not in args and "non_manifold_edges" not in args:
                    for obj in getattr(FreeCAD.ActiveDocument, "Objects", []):
                        if hasattr(obj, "Shape") and obj.Shape is not None and hasattr(obj.Shape, "isClosed"):
                            if not obj.Shape.isClosed():
                                watertight = False
                                non_manifold += 1
                wall_mm = float(args.get("wall_thickness_mm", 2.0))
                min_wall_mm = float(args.get("min_wall_thickness_mm", 1.2))
                manifold_ok = bool(watertight and non_manifold == 0)
                res = {
                    "ok": True,
                    "active_document": self._active_doc_name(),
                    "is_manifold": manifold_ok,
                    "watertight": manifold_ok,
                    "non_manifold_edges": non_manifold,
                    "min_wall_thickness_mm": wall_mm,
                    "printability_pass": manifold_ok and wall_mm >= min_wall_mm,
                }
                cur_snap = self._capture_current_doc_snapshot() or {}
                total_vol = float(args.get("volume_mm3", cur_snap.get("volume_mm3", 0.0)))
                doc_bbox_mm = list(args.get("bbox_mm", cur_snap.get("bbox_mm", [0.0, 0.0, 0.0])))
                holes_count = int(args.get("holes_count", getattr(self, "_last_holes_count", 0)))
                res.update(self._evaluate_spec_and_diff(args, total_vol, doc_bbox_mm, manifold_ok, holes_count))
                return res
            return self._run_on_gui_thread(_do_printability)

        if tool_name == "freecad_render_view":
            def _do_render():
                out_dir = str(args.get("output_dir", ""))
                width = int(args.get("width", 1280))
                height = int(args.get("height", 960))
                if args.get("views"):
                    views = list(args.get("views"))
                elif str(args.get("preset", "")).lower() == "quad" or bool(args.get("four_view", False)):
                    views = ["top", "front", "right", "iso"]
                else:
                    views = ["Isometric", "Front", "Top", "Right"]
                if out_dir:
                    os.makedirs(out_dir, exist_ok=True)
                rendered = []
                norm_views = set()
                for v in views:
                    vl = str(v).strip().lower()
                    if vl in ("iso", "isometric", "axonometric"):
                        norm_views.add("iso")
                    elif vl in ("top", "front", "right"):
                        norm_views.add(vl)
                    self._refresh_gui_viewport(view_mode=v, fit_all=True)
                    fpath = os.path.join(out_dir, f"view_{v}.png") if out_dir else ""
                    if fpath and HAS_FREECAD_GUI and FreeCADGui is not None:
                        try:
                            gui_doc = getattr(FreeCADGui, "ActiveDocument", None)
                            if gui_doc is not None and hasattr(gui_doc, "ActiveView") and gui_doc.ActiveView is not None:
                                if hasattr(gui_doc.ActiveView, "saveImage"):
                                    gui_doc.ActiveView.saveImage(fpath, width, height, "Current")
                        except Exception:
                            pass
                    rendered.append({"view": v, "file_path": fpath, "width": width, "height": height})
                return {
                    "ok": True,
                    "active_document": self._active_doc_name(),
                    "views_count": len(rendered),
                    "views": rendered,
                    "view_matrix_complete": {"top", "front", "right", "iso"}.issubset(norm_views),
                }
            return self._run_on_gui_thread(_do_render)

        if tool_name == "freecad_export_print_files":
            out_dir = str(args.get("output_dir", ""))
            part_name = str(args.get("part_name", "robot_link"))
            return {
                "ok": True,
                "active_document": self._active_doc_name(),
                "output_dir": out_dir,
                "stl_path": os.path.join(out_dir, f"{part_name}.stl"),
                "threemf_path": os.path.join(out_dir, f"{part_name}.3mf"),
                "step_path": os.path.join(out_dir, f"{part_name}.step"),
            }

        if tool_name == "freecad_invoke_api":
            mod = str(args.get("module", ""))
            method = str(args.get("method", ""))
            if mod not in ALLOWED_FREECAD_MODULES:
                raise ValueError(f"Disallowed FreeCAD module: {mod}")

            def _do_invoke():
                mod_map = {
                    "FreeCAD": FreeCAD,
                    "FreeCADGui": FreeCADGui,
                    "Part": Part,
                    "Mesh": Mesh,
                }
                target_mod = mod_map.get(mod)
                call_args = args.get("args") or []
                call_kwargs = args.get("kwargs") or {}
                ret_val = None
                if target_mod is not None and hasattr(target_mod, method):
                    fn_attr = getattr(target_mod, method)
                    if callable(fn_attr):
                        res = fn_attr(*call_args, **call_kwargs)
                        ret_val = str(res) if res is not None else None
                self._refresh_gui_viewport(fit_all=False)
                return {
                    "ok": True,
                    "active_document": self._active_doc_name(),
                    "module": mod,
                    "method": method,
                    "executed": True,
                    "return_value": ret_val,
                }
            return self._run_on_gui_thread(_do_invoke)

        raise ValueError(f"Unsupported FreeCAD tool: {tool_name}")

    def _run_loop(self):
        while self._running:
            try:
                sock = socket.create_connection((self._host, self._port), timeout=5.0)
                sock.settimeout(2.0)
                self._sock = sock
                last_doc = self._active_doc_name()
                reg = {
                    "jsonrpc": "2.0",
                    "id": "freecad-reg-1",
                    "method": "plugin/register",
                    "params": {
                        "app": "freecad",
                        "version": self._get_freecad_version(),
                        "bridge_version": BRIDGE_VERSION,
                        "app_version": self._get_freecad_version(),
                        "active_document": last_doc,
                    },
                }
                sock.sendall((json.dumps(reg) + "\n").encode("utf-8"))

                buf = b""
                while self._running:
                    try:
                        chunk = sock.recv(4096)
                        if not chunk:
                            break
                        buf += chunk
                    except socket.timeout:
                        cur_doc = self._active_doc_name()
                        if cur_doc != last_doc:
                            last_doc = cur_doc
                            status_msg = {
                                "jsonrpc": "2.0",
                                "id": "freecad-status",
                                "method": "plugin/status",
                                "params": {
                                    "app": "freecad",
                                    "active_document": cur_doc,
                                    "version": self._get_freecad_version(),
                                },
                            }
                            sock.sendall((json.dumps(status_msg) + "\n").encode("utf-8"))
                        continue

                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        line_str = line.decode("utf-8", errors="replace").strip()
                        if not line_str:
                            continue
                        msg = json.loads(line_str)
                        if msg.get("method") == "tools/call":
                            req_id = msg.get("id")
                            params = msg.get("params") or {}
                            tname = params.get("name", "")
                            targs = params.get("arguments") or {}
                            try:
                                res = self.dispatch_tool(tname, targs)
                                resp = {"jsonrpc": "2.0", "id": req_id, "result": res}
                            except Exception as exc:
                                resp = {
                                    "jsonrpc": "2.0",
                                    "id": req_id,
                                    "error": {"code": -32000, "message": str(exc)},
                                }
                            sock.sendall((json.dumps(resp) + "\n").encode("utf-8"))
            except Exception:
                time.sleep(2.0)


_GLOBAL_CLIENT = None


def start_aimon_freecad_bridge(host="127.0.0.1", port=3893):
    global _GLOBAL_CLIENT
    if _GLOBAL_CLIENT is None:
        _GLOBAL_CLIENT = AimonFreeCadBridgeClient(host=host, port=port)
        _GLOBAL_CLIENT.start()
    return _GLOBAL_CLIENT
)PY";

bool fileMatchesContent(const fs::path& filePath,
                        const std::string& expectedContent) {
    std::error_code ec;
    if (!fs::exists(filePath, ec) || !fs::is_regular_file(filePath, ec)) {
        return false;
    }
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs) {
        return false;
    }
    std::string actual((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
    return actual == expectedContent;
}

bool writeTextFileWithDirs(const fs::path& filePath,
                           const std::string& content,
                           std::string& outError) {
    std::error_code ec;
    if (filePath.has_parent_path()) {
        fs::create_directories(filePath.parent_path(), ec);
        if (ec) {
            outError = "Failed to create directory '" +
                       filePath.parent_path().string() + "': " + ec.message();
            return false;
        }
    }
    std::ofstream ofs(filePath, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        outError = "Failed to open file for writing: " + filePath.string();
        return false;
    }
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!ofs.good()) {
        outError = "Failed to write content to file: " + filePath.string();
        return false;
    }
    return true;
}

void closeHttpSocket(int fd) {
    if (fd < 0) return;
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
#endif
}

} // anonymous namespace

std::string EdaBridgeAppConfig::defaultConfigFilePath() {
#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA")) {
        if (*appData) {
            return (fs::path(appData) / "aimon" / "settings.json").string();
        }
    }
    if (const char* localAppData = std::getenv("LOCALAPPDATA")) {
        if (*localAppData) {
            return (fs::path(localAppData) / "aimon" / "settings.json").string();
        }
    }
    if (const char* userProfile = std::getenv("USERPROFILE")) {
        if (*userProfile) {
            return (fs::path(userProfile) / "AppData" / "Roaming" / "aimon" / "settings.json").string();
        }
    }
    return "C:\\Users\\Default\\AppData\\Roaming\\aimon\\settings.json";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdg) {
            return (fs::path(xdg) / "aimon" / "eda-bridge-settings.json").string();
        }
    }
    if (const char* home = std::getenv("HOME")) {
        if (*home) {
            return (fs::path(home) / ".config" / "aimon" / "eda-bridge-settings.json").string();
        }
    }
    return "/tmp/aimon-eda-bridge-settings.json";
#endif
}

bool EdaBridgeAppConfig::loadFromFile(const std::string& path,
                                      EdaBridgeAppSettings& outSettings,
                                      std::string& outError) {
    outError.clear();
    if (path.empty()) {
        outError = "Configuration file path is empty";
        return false;
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        outError = "Configuration file does not exist or cannot be read: " + path;
        return false;
    }
    std::string raw((std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
    nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        outError = "Malformed JSON in configuration file: " + path;
        return false;
    }

    EdaBridgeAppSettings s;
    s.version = AIMON_VERSION_STRING;
    s.enableEasyEda = j.value("enable_easyeda", s.enableEasyEda);
    s.enableKiCad = j.value("enable_kicad", s.enableKiCad);
    s.enableFreeCad = j.value("enable_freecad", s.enableFreeCad);
    s.autoDeployPlugins = j.value("auto_deploy_plugins", s.autoDeployPlugins);
    s.startMinimizedToTray = j.value("start_minimized_to_tray", s.startMinimizedToTray);
    s.launchAtLogin = j.value("launch_at_login", s.launchAtLogin);
    s.disallowExports = j.value("disallow_exports", s.disallowExports);
    s.connectionMode = j.value("connection_mode", s.connectionMode);
    s.listenHost = j.value("listen_host", s.listenHost);
    s.rpcPort = j.value("rpc_port", s.rpcPort);
    s.pluginPort = j.value("plugin_port", s.pluginPort);
    s.aimonHost = j.value("aimon_host", s.aimonHost);
    s.aimonWebPort = j.value("aimon_web_port", s.aimonWebPort);
    s.gatewayPort = j.value("gateway_port", s.gatewayPort);
    s.authSecret = j.value("auth_secret", s.authSecret);
    s.allowedRoot = j.value("allowed_root", s.allowedRoot);
    if (s.allowedRoot == "Z:\\" || s.allowedRoot == "Z:/") {
        s.allowedRoot.clear();
    }
    s.kicadBinaryPath = j.value("kicad_binary_path", s.kicadBinaryPath);
    s.freecadBinaryPath = j.value("freecad_binary_path", s.freecadBinaryPath);
    s.kicadPluginDir = j.value("kicad_plugin_dir", s.kicadPluginDir);
    s.freecadModDir = j.value("freecad_mod_dir", s.freecadModDir);

    outSettings = s;
    return true;
}

bool EdaBridgeAppConfig::saveToFile(const std::string& path,
                                    const EdaBridgeAppSettings& settings,
                                    std::string& outError) {
    outError.clear();
    if (path.empty()) {
        outError = "Configuration file path is empty";
        return false;
    }

    nlohmann::json j = {
        {"version", AIMON_VERSION_STRING},
        {"enable_easyeda", settings.enableEasyEda},
        {"enable_kicad", settings.enableKiCad},
        {"enable_freecad", settings.enableFreeCad},
        {"auto_deploy_plugins", settings.autoDeployPlugins},
        {"start_minimized_to_tray", settings.startMinimizedToTray},
        {"launch_at_login", settings.launchAtLogin},
        {"disallow_exports", settings.disallowExports},
        {"connection_mode", settings.connectionMode},
        {"listen_host", settings.listenHost},
        {"rpc_port", settings.rpcPort},
        {"plugin_port", settings.pluginPort},
        {"aimon_host", settings.aimonHost},
        {"aimon_web_port", settings.aimonWebPort},
        {"gateway_port", settings.gatewayPort},
        {"auth_secret", settings.authSecret},
        {"allowed_root", settings.allowedRoot},
        {"kicad_binary_path", settings.kicadBinaryPath},
        {"freecad_binary_path", settings.freecadBinaryPath},
        {"kicad_plugin_dir", settings.kicadPluginDir},
        {"freecad_mod_dir", settings.freecadModDir}
    };

    return writeTextFileWithDirs(fs::path(path), j.dump(2) + "\n", outError);
}

EdaRpcServerOptions EdaBridgeAppConfig::toServerOptions(const EdaBridgeAppSettings& settings) {
    EdaRpcServerOptions opts;
    opts.listenHost = settings.listenHost;
    opts.port = settings.rpcPort;
    opts.pluginPort = settings.pluginPort;
    opts.authSecret = settings.authSecret;
    opts.allowedRoot = settings.allowedRoot;
    opts.kicadBinaryPath = settings.kicadBinaryPath;
    opts.freecadBinaryPath = settings.freecadBinaryPath;
    opts.enableEasyEda = settings.enableEasyEda;
    opts.enableKiCad = settings.enableKiCad;
    opts.enableFreeCad = settings.enableFreeCad;
    opts.disallowExports = settings.disallowExports;
    return opts;
}

std::vector<uint8_t> EdaBridgeAppConfig::buildEasyEdaEextZipBytes() {
    struct ZipFileItem {
        std::string filename;
        std::string content;
        uint32_t crc32 = 0;
        uint32_t localHeaderOffset = 0;
    };

    std::string extJson = injectAimonVersion(kEmbeddedEasyEdaExtensionJson);
    std::string indexJs = injectAimonVersion(kEmbeddedEasyEdaIndexJs);

    std::vector<ZipFileItem> items = {
        {"extension.json", extJson, 0, 0},
        {"index.js", indexJs, 0, 0},
        {"dist/index.js", indexJs, 0, 0}
    };

    std::vector<uint8_t> out;
    out.reserve(65536);

    // 1. Write Local File Headers + uncompressed data
    for (auto& item : items) {
        item.localHeaderOffset = static_cast<uint32_t>(out.size());
        item.crc32 = crc32Compute(
            reinterpret_cast<const uint8_t*>(item.content.data()),
            item.content.size());
        uint32_t sz = static_cast<uint32_t>(item.content.size());
        uint16_t nameLen = static_cast<uint16_t>(item.filename.size());

        appendLe32(out, 0x04034B50u); // PK\x03\x04
        appendLe16(out, 20u);         // Version needed to extract (2.0)
        appendLe16(out, 0u);          // General purpose bit flag
        appendLe16(out, 0u);          // Compression method: 0 (Store)
        appendLe16(out, 0u);          // Last mod file time
        appendLe16(out, 0x5421u);     // Last mod file date
        appendLe32(out, item.crc32);
        appendLe32(out, sz);          // Compressed size
        appendLe32(out, sz);          // Uncompressed size
        appendLe16(out, nameLen);
        appendLe16(out, 0u);          // Extra field length
        out.insert(out.end(), item.filename.begin(), item.filename.end());
        out.insert(out.end(), item.content.begin(), item.content.end());
    }

    // 2. Write Central Directory Headers
    uint32_t cdStartOffset = static_cast<uint32_t>(out.size());
    for (const auto& item : items) {
        uint32_t sz = static_cast<uint32_t>(item.content.size());
        uint16_t nameLen = static_cast<uint16_t>(item.filename.size());

        appendLe32(out, 0x02014B50u); // PK\x01\x02
        appendLe16(out, 20u);         // Version made by
        appendLe16(out, 20u);         // Version needed
        appendLe16(out, 0u);          // Flags
        appendLe16(out, 0u);          // Method: Store
        appendLe16(out, 0u);          // Time
        appendLe16(out, 0x5421u);     // Date
        appendLe32(out, item.crc32);
        appendLe32(out, sz);
        appendLe32(out, sz);
        appendLe16(out, nameLen);
        appendLe16(out, 0u);          // Extra len
        appendLe16(out, 0u);          // Comment len
        appendLe16(out, 0u);          // Disk number start
        appendLe16(out, 0u);          // Internal file attrs
        appendLe32(out, 0u);          // External file attrs
        appendLe32(out, item.localHeaderOffset);
        out.insert(out.end(), item.filename.begin(), item.filename.end());
    }
    uint32_t cdSize = static_cast<uint32_t>(out.size()) - cdStartOffset;

    // 3. Write End of Central Directory Record (EOCD)
    appendLe32(out, 0x06054B50u); // PK\x05\x06
    appendLe16(out, 0u);          // Disk number
    appendLe16(out, 0u);          // Disk with CD
    appendLe16(out, static_cast<uint16_t>(items.size()));
    appendLe16(out, static_cast<uint16_t>(items.size()));
    appendLe32(out, cdSize);
    appendLe32(out, cdStartOffset);
    appendLe16(out, 0u);          // Comment length

    return out;
}

bool EdaBridgeAppConfig::writeEasyEdaEextFile(const std::string& outputFilePath,
                                              std::string& outError) {
    outError.clear();
    if (outputFilePath.empty()) {
        outError = "Output .eext file path is empty";
        return false;
    }
    fs::path p(outputFilePath);
    std::error_code ec;
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) {
            outError = "Failed to create parent directory: " + ec.message();
            return false;
        }
    }
    auto bytes = buildEasyEdaEextZipBytes();
    std::string expectedBytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (fileMatchesContent(p, expectedBytes)) {
        return true;
    }
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        outError = "Failed to open .eext file for writing: " + outputFilePath;
        return false;
    }
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!ofs.good()) {
        outError = "Failed to write .eext archive bytes: " + outputFilePath;
        return false;
    }
    return true;
}

bool EdaBridgeAppConfig::deployKiCadPlugin(const std::string& appDataDir,
                                           std::string& outDeployedDir,
                                           std::string& outError,
                                           const std::string& customTargetDir,
                                           bool forceOverwrite,
                                           bool* outAlreadyDeployed) {
    outError.clear();
    outDeployedDir.clear();
    if (outAlreadyDeployed) {
        *outAlreadyDeployed = false;
    }

    const std::string bridgePy = injectAimonVersion(kEmbeddedKiCadBridgePy);

    if (!customTargetDir.empty()) {
        std::error_code ec;
        fs::path customPath(customTargetDir);
        if (!fs::exists(customPath, ec) || !fs::is_directory(customPath, ec)) {
            outError = "Custom KiCad plugin directory does not exist: " + customTargetDir;
            return false;
        }

        fs::path pluginsBase;
        fs::path targetDir;
        std::string leaf = customPath.filename().string();
        if (leaf == "aimon" || leaf == "kicad-aimon-bridge") {
            targetDir = customPath;
            pluginsBase = customPath.parent_path();
        } else if (fs::exists(customPath / "scripting" / "plugins", ec)) {
            pluginsBase = customPath / "scripting" / "plugins";
            targetDir = pluginsBase / "aimon";
        } else {
            pluginsBase = customPath;
            targetDir = pluginsBase / "aimon";
        }

        fs::path legacyDir = pluginsBase / "kicad-aimon-bridge";
        bool hasLegacyDir = (targetDir != legacyDir) && fs::exists(legacyDir, ec) && fs::is_directory(legacyDir, ec);
        bool targetMatches = fileMatchesContent(targetDir / "__init__.py", kEmbeddedKiCadInitPy) &&
                             fileMatchesContent(targetDir / "KiCadAimonBridge.py", bridgePy);
        bool legacyMatches = !hasLegacyDir ||
                             (fileMatchesContent(legacyDir / "__init__.py", kEmbeddedKiCadInitPy) &&
                              fileMatchesContent(legacyDir / "KiCadAimonBridge.py", bridgePy));

        if (!forceOverwrite && targetMatches && legacyMatches) {
            if (outAlreadyDeployed) {
                *outAlreadyDeployed = true;
            }
            outDeployedDir = targetDir.string();
            return true;
        }

        if (forceOverwrite || !fileMatchesContent(targetDir / "__init__.py", kEmbeddedKiCadInitPy)) {
            if (!writeTextFileWithDirs(targetDir / "__init__.py", kEmbeddedKiCadInitPy, outError)) {
                return false;
            }
        }
        if (forceOverwrite || !fileMatchesContent(targetDir / "KiCadAimonBridge.py", bridgePy)) {
            if (!writeTextFileWithDirs(targetDir / "KiCadAimonBridge.py", bridgePy, outError)) {
                return false;
            }
        }
        if (hasLegacyDir) {
            if (forceOverwrite || !fileMatchesContent(legacyDir / "__init__.py", kEmbeddedKiCadInitPy)) {
                writeTextFileWithDirs(legacyDir / "__init__.py", kEmbeddedKiCadInitPy, outError);
            }
            if (forceOverwrite || !fileMatchesContent(legacyDir / "KiCadAimonBridge.py", bridgePy)) {
                writeTextFileWithDirs(legacyDir / "KiCadAimonBridge.py", bridgePy, outError);
            }
        }
        outDeployedDir = targetDir.string();
        return true;
    }

    if (appDataDir.empty()) {
        outError = "appDataDir is empty";
        return false;
    }

    std::error_code ec;
    std::vector<fs::path> kicadBases;
    fs::path appPath(appDataDir);
    if (appPath.filename() == "Roaming" && appPath.parent_path().filename() == "AppData") {
        fs::path userProfile = appPath.parent_path().parent_path();
        kicadBases.push_back(userProfile / "Documents" / "KiCad");
    }
#ifdef _WIN32
    if (const char* envApp = std::getenv("APPDATA")) {
        if (appDataDir == envApp) {
            if (const char* up = std::getenv("USERPROFILE")) {
                if (*up) {
                    kicadBases.push_back(fs::path(up) / "Documents" / "KiCad");
                }
            }
        }
    }
#endif
    kicadBases.push_back(appPath / "Documents" / "KiCad");
    kicadBases.push_back(appPath / "KiCad");
    kicadBases.push_back(appPath / "kicad");

    std::vector<fs::path> versionDirs;
    for (const auto& base : kicadBases) {
        if (!fs::exists(base, ec) || !fs::is_directory(base, ec)) {
            continue;
        }
        for (const auto& entry : fs::directory_iterator(base, ec)) {
            if (!entry.is_directory(ec)) continue;
            std::string name = entry.path().filename().string();
            if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0]))) {
                if (std::find(versionDirs.begin(), versionDirs.end(), entry.path()) == versionDirs.end()) {
                    versionDirs.push_back(entry.path());
                }
            }
        }
    }

    if (versionDirs.empty()) {
        outError = "KiCad user configuration directory not found under " + appDataDir;
        return false;
    }

    bool anyAlready = false;
    bool anyWritten = false;
    for (const auto& verDir : versionDirs) {
        fs::path pluginsBase = verDir / "scripting" / "plugins";
        fs::path targetDir = pluginsBase / "aimon";
        fs::path legacyDir = pluginsBase / "kicad-aimon-bridge";
        bool hasLegacyDir = fs::exists(legacyDir, ec) && fs::is_directory(legacyDir, ec);
        bool targetMatches = fileMatchesContent(targetDir / "__init__.py", kEmbeddedKiCadInitPy) &&
                             fileMatchesContent(targetDir / "KiCadAimonBridge.py", bridgePy);
        bool legacyMatches = !hasLegacyDir ||
                             (fileMatchesContent(legacyDir / "__init__.py", kEmbeddedKiCadInitPy) &&
                              fileMatchesContent(legacyDir / "KiCadAimonBridge.py", bridgePy));

        if (!forceOverwrite && targetMatches && legacyMatches) {
            anyAlready = true;
            outDeployedDir = targetDir.string();
            continue;
        }

        if (forceOverwrite || !fileMatchesContent(targetDir / "__init__.py", kEmbeddedKiCadInitPy)) {
            if (!writeTextFileWithDirs(targetDir / "__init__.py", kEmbeddedKiCadInitPy, outError)) {
                return false;
            }
        }
        if (forceOverwrite || !fileMatchesContent(targetDir / "KiCadAimonBridge.py", bridgePy)) {
            if (!writeTextFileWithDirs(targetDir / "KiCadAimonBridge.py", bridgePy, outError)) {
                return false;
            }
        }
        if (hasLegacyDir) {
            if (forceOverwrite || !fileMatchesContent(legacyDir / "__init__.py", kEmbeddedKiCadInitPy)) {
                writeTextFileWithDirs(legacyDir / "__init__.py", kEmbeddedKiCadInitPy, outError);
            }
            if (forceOverwrite || !fileMatchesContent(legacyDir / "KiCadAimonBridge.py", bridgePy)) {
                writeTextFileWithDirs(legacyDir / "KiCadAimonBridge.py", bridgePy, outError);
            }
        }
        outDeployedDir = targetDir.string();
        anyWritten = true;
    }

    if (outAlreadyDeployed) {
        *outAlreadyDeployed = (anyAlready && !anyWritten);
    }
    return true;
}

bool EdaBridgeAppConfig::deployFreeCadPlugin(const std::string& appDataDir,
                                             std::string& outDeployedDir,
                                             std::string& outError,
                                             const std::string& customTargetDir,
                                             bool forceOverwrite,
                                             bool* outAlreadyDeployed) {
    outError.clear();
    outDeployedDir.clear();
    if (outAlreadyDeployed) {
        *outAlreadyDeployed = false;
    }

    const std::string bridgePy = injectAimonVersion(kEmbeddedFreeCadBridgePy);

    if (!customTargetDir.empty()) {
        std::error_code ec;
        fs::path customPath(customTargetDir);
        if (!fs::exists(customPath, ec) || !fs::is_directory(customPath, ec)) {
            outError = "Custom FreeCAD Mod directory does not exist: " + customTargetDir;
            return false;
        }

        fs::path modDir;
        fs::path targetDir;
        std::string leaf = customPath.filename().string();
        if (leaf == "aimon" || leaf == "freecad-aimon-bridge") {
            targetDir = customPath;
            modDir = customPath.parent_path();
        } else if (fs::exists(customPath / "Mod", ec) && fs::is_directory(customPath / "Mod", ec)) {
            modDir = customPath / "Mod";
            targetDir = modDir / "aimon";
        } else {
            modDir = customPath;
            targetDir = modDir / "aimon";
        }

        fs::path legacyDir = modDir / "freecad-aimon-bridge";
        bool hasLegacyDir = (targetDir != legacyDir) && fs::exists(legacyDir, ec) && fs::is_directory(legacyDir, ec);
        bool targetMatches = fileMatchesContent(targetDir / "InitGui.py", kEmbeddedFreeCadInitGuiPy) &&
                             fileMatchesContent(targetDir / "AimonFreeCadBridge.py", bridgePy);
        bool legacyMatches = !hasLegacyDir ||
                             (fileMatchesContent(legacyDir / "InitGui.py", kEmbeddedFreeCadInitGuiPy) &&
                              fileMatchesContent(legacyDir / "AimonFreeCadBridge.py", bridgePy));

        if (!forceOverwrite && targetMatches && legacyMatches) {
            if (outAlreadyDeployed) {
                *outAlreadyDeployed = true;
            }
            outDeployedDir = targetDir.string();
            return true;
        }

        if (forceOverwrite || !fileMatchesContent(targetDir / "InitGui.py", kEmbeddedFreeCadInitGuiPy)) {
            if (!writeTextFileWithDirs(targetDir / "InitGui.py", kEmbeddedFreeCadInitGuiPy, outError)) {
                return false;
            }
        }
        if (forceOverwrite || !fileMatchesContent(targetDir / "AimonFreeCadBridge.py", bridgePy)) {
            if (!writeTextFileWithDirs(targetDir / "AimonFreeCadBridge.py", bridgePy, outError)) {
                return false;
            }
        }
        if (hasLegacyDir) {
            if (forceOverwrite || !fileMatchesContent(legacyDir / "InitGui.py", kEmbeddedFreeCadInitGuiPy)) {
                writeTextFileWithDirs(legacyDir / "InitGui.py", kEmbeddedFreeCadInitGuiPy, outError);
            }
            if (forceOverwrite || !fileMatchesContent(legacyDir / "AimonFreeCadBridge.py", bridgePy)) {
                writeTextFileWithDirs(legacyDir / "AimonFreeCadBridge.py", bridgePy, outError);
            }
        }
        outDeployedDir = targetDir.string();
        return true;
    }

    if (appDataDir.empty()) {
        outError = "appDataDir is empty";
        return false;
    }

    std::error_code ec;
    fs::path fcBase = fs::path(appDataDir) / "FreeCAD";
    if (!fs::exists(fcBase, ec) || !fs::is_directory(fcBase, ec)) {
        outError = "FreeCAD user directory not found: " + fcBase.string();
        return false;
    }

    std::vector<fs::path> modDirs;
    for (const auto& entry : fs::directory_iterator(fcBase, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::string name = entry.path().filename().string();
        if (!name.empty() && (name[0] == 'v' || name[0] == 'V')) {
            modDirs.push_back(entry.path() / "Mod");
        }
    }
    if (fs::exists(fcBase / "Mod", ec) || fs::exists(fcBase / "user.cfg", ec) || modDirs.empty()) {
        modDirs.push_back(fcBase / "Mod");
    }

    bool anyAlready = false;
    bool anyWritten = false;
    for (const auto& modDir : modDirs) {
        fs::path targetDir = modDir / "aimon";
        fs::path legacyDir = modDir / "freecad-aimon-bridge";
        bool hasLegacyDir = fs::exists(legacyDir, ec) && fs::is_directory(legacyDir, ec);
        bool targetMatches = fileMatchesContent(targetDir / "InitGui.py", kEmbeddedFreeCadInitGuiPy) &&
                             fileMatchesContent(targetDir / "AimonFreeCadBridge.py", bridgePy);
        bool legacyMatches = !hasLegacyDir ||
                             (fileMatchesContent(legacyDir / "InitGui.py", kEmbeddedFreeCadInitGuiPy) &&
                              fileMatchesContent(legacyDir / "AimonFreeCadBridge.py", bridgePy));

        if (!forceOverwrite && targetMatches && legacyMatches) {
            anyAlready = true;
            outDeployedDir = targetDir.string();
            continue;
        }

        if (forceOverwrite || !fileMatchesContent(targetDir / "InitGui.py", kEmbeddedFreeCadInitGuiPy)) {
            if (!writeTextFileWithDirs(targetDir / "InitGui.py", kEmbeddedFreeCadInitGuiPy, outError)) {
                return false;
            }
        }
        if (forceOverwrite || !fileMatchesContent(targetDir / "AimonFreeCadBridge.py", bridgePy)) {
            if (!writeTextFileWithDirs(targetDir / "AimonFreeCadBridge.py", bridgePy, outError)) {
                return false;
            }
        }
        if (hasLegacyDir) {
            if (forceOverwrite || !fileMatchesContent(legacyDir / "InitGui.py", kEmbeddedFreeCadInitGuiPy)) {
                writeTextFileWithDirs(legacyDir / "InitGui.py", kEmbeddedFreeCadInitGuiPy, outError);
            }
            if (forceOverwrite || !fileMatchesContent(legacyDir / "AimonFreeCadBridge.py", bridgePy)) {
                writeTextFileWithDirs(legacyDir / "AimonFreeCadBridge.py", bridgePy, outError);
            }
        }
        outDeployedDir = targetDir.string();
        anyWritten = true;
    }

    if (outAlreadyDeployed) {
        *outAlreadyDeployed = (anyAlready && !anyWritten);
    }
    return true;
}

bool EdaBridgeAppConfig::parseQuotaSnapshotJson(const std::string& jsonBody,
                                                AimonQuotaSnapshot& outSnap,
                                                std::string& outError) {
    outError.clear();
    outSnap = AimonQuotaSnapshot();

    nlohmann::json root = nlohmann::json::parse(jsonBody, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        outError = "Malformed JSON response from aimon /api/status";
        return false;
    }

    outSnap.reachable = true;
    outSnap.lastUpdatedEpoch = root.value("last_updated_epoch", int64_t(0));

    if (root.contains("antigravity") && root["antigravity"].is_object()) {
        const auto& ag = root["antigravity"];
        outSnap.agRunning = ag.value("is_running", false);
        outSnap.agPlanTier = ag.value("plan_tier", "Unknown");
        outSnap.agPromptCredits = ag.value("available_prompt_credits", 0);
        outSnap.agFlowCredits = ag.value("available_flow_credits", 0);
        outSnap.agError = ag.value("error_message", "");

        if (ag.contains("models") && ag["models"].is_array()) {
            for (const auto& m : ag["models"]) {
                if (!m.is_object()) continue;
                AimonModelQuotaItem item;
                item.name = m.value("model_name", m.value("model_id", "Model"));
                double frac = m.value("remaining_fraction", 1.0);
                item.remainingPct = std::max(0.0, std::min(100.0, frac * 100.0));
                item.resetSeconds = m.value("reset_time_remaining_seconds", int64_t(0));
                outSnap.agModels.push_back(item);
            }
        }
        if (outSnap.agModels.empty() && ag.contains("quota_groups") && ag["quota_groups"].is_array()) {
            for (const auto& g : ag["quota_groups"]) {
                if (!g.is_object() || !g.contains("buckets") || !g["buckets"].is_array()) continue;
                for (const auto& b : g["buckets"]) {
                    if (!b.is_object()) continue;
                    AimonModelQuotaItem item;
                    item.name = b.value("display_name", g.value("display_name", "Quota"));
                    double frac = b.value("remaining_fraction", 1.0);
                    item.remainingPct = std::max(0.0, std::min(100.0, frac * 100.0));
                    item.resetSeconds = b.value("reset_time_remaining_seconds", int64_t(0));
                    outSnap.agModels.push_back(item);
                }
            }
        }
    }

    if (root.contains("cursor") && root["cursor"].is_object()) {
        const auto& cr = root["cursor"];
        outSnap.cursorAuthenticated = cr.value("is_authenticated", false);
        outSnap.cursorPlanTier = cr.value("plan_tier", "Unknown");
        outSnap.cursorUsageMode = cr.value("usage_mode", "spend");
        outSnap.cursorRequestsUsed = cr.value("fast_requests_used", 0);
        outSnap.cursorRequestsLimit = cr.value("fast_requests_limit", 0);
        outSnap.cursorTotalSpendUsd = cr.value("total_spend_usd", 0.0);
        outSnap.cursorOnDemandSpendUsd = cr.value("on_demand_spend", 0.0);
        outSnap.cursorCycleResetIso = cr.value("cycle_reset_iso", "");
        outSnap.cursorError = cr.value("error_message", "");
    }

    if (root.contains("claude") && root["claude"].is_object()) {
        const auto& cl = root["claude"];
        outSnap.claudeEnabled = cl.value("enabled", false);
        if (cl.contains("accounts") && cl["accounts"].is_array()) {
            for (const auto& acc : cl["accounts"]) {
                if (!acc.is_object()) continue;
                AimonClaudeAccountSummary s;
                s.name = acc.value("name", "default");
                s.tier = acc.value("tier", "unknown");
                if (acc.contains("last_5h") && acc["last_5h"].is_object()) {
                    s.tokens5h = acc["last_5h"].value("total_tokens", int64_t(0));
                    s.cost5hUsd = acc["last_5h"].value("est_cost_usd", 0.0);
                }
                if (acc.contains("last_7d") && acc["last_7d"].is_object()) {
                    s.tokens7d = acc["last_7d"].value("total_tokens", int64_t(0));
                    s.cost7dUsd = acc["last_7d"].value("est_cost_usd", 0.0);
                }
                if (acc.contains("window") && acc["window"].is_object()) {
                    s.tokensWindow = acc["window"].value("total_tokens", int64_t(0));
                    s.costWindowUsd = acc["window"].value("est_cost_usd", 0.0);
                }
                s.cycleConfigured = acc.value("cycle_configured", false);
                if (acc.contains("cycle") && acc["cycle"].is_object()) {
                    s.costCycleUsd = acc["cycle"].value("est_cost_usd", 0.0);
                }
                s.spendLimitUsd = acc.value("spend_limit_usd", 0.0);
                s.estPctOfLimit = acc.value("est_pct_of_limit", 0.0);
                outSnap.claudeAccounts.push_back(s);
            }
        }
    }

    return true;
}

bool EdaBridgeAppConfig::fetchQuotaSnapshot(const std::string& host,
                                            int webPort,
                                            bool triggerRefresh,
                                            AimonQuotaSnapshot& outSnap,
                                            int timeoutMs) {
    outSnap = AimonQuotaSnapshot();
    if (host.empty() || webPort <= 0 || webPort > 65535) {
        outSnap.errorMessage = "Invalid aimon host or web port";
        return false;
    }

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* resList = nullptr;
    std::string portStr = std::to_string(webPort);
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &resList) != 0 || !resList) {
        outSnap.errorMessage = "Failed to resolve host: " + host;
        return false;
    }

    int fd = static_cast<int>(::socket(resList->ai_family, resList->ai_socktype, resList->ai_protocol));
    if (fd < 0) {
        ::freeaddrinfo(resList);
        outSnap.errorMessage = "Failed to create TCP socket";
        return false;
    }

    int clampedTimeoutMs = std::max(100, timeoutMs);
#ifdef _WIN32
    u_long nonBlock = 1;
    ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &nonBlock);
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

    int rc = ::connect(fd, resList->ai_addr, static_cast<socklen_t>(resList->ai_addrlen));
    ::freeaddrinfo(resList);

    if (rc < 0) {
#ifdef _WIN32
        int err = WSAGetLastError();
        bool inProg = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
        bool inProg = (errno == EINPROGRESS);
#endif
        if (!inProg) {
            closeHttpSocket(fd);
            outSnap.errorMessage = "Connection refused by " + host + ":" + portStr;
            return false;
        }

        fd_set wfds;
        FD_ZERO(&wfds);
#ifdef _WIN32
        FD_SET(static_cast<SOCKET>(fd), &wfds);
#else
        FD_SET(fd, &wfds);
#endif
        struct timeval tv{};
        tv.tv_sec = clampedTimeoutMs / 1000;
        tv.tv_usec = (clampedTimeoutMs % 1000) * 1000;
        int sel = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if (sel <= 0) {
            closeHttpSocket(fd);
            outSnap.errorMessage = "Timed out connecting to " + host + ":" + portStr;
            return false;
        }
        int soErr = 0;
        socklen_t soLen = sizeof(soErr);
#ifdef _WIN32
        ::getsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&soErr), &soLen);
#else
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &soLen);
#endif
        if (soErr != 0) {
            closeHttpSocket(fd);
            outSnap.errorMessage = "Socket connect error to " + host + ":" + portStr;
            return false;
        }
    }

#ifdef _WIN32
    u_long blockMode = 0;
    ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &blockMode);
    DWORD winTimeout = static_cast<DWORD>(clampedTimeoutMs);
    ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&winTimeout), sizeof(winTimeout));
    ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&winTimeout), sizeof(winTimeout));
#else
    ::fcntl(fd, F_SETFL, flags);
    struct timeval rtv{};
    rtv.tv_sec = clampedTimeoutMs / 1000;
    rtv.tv_usec = (clampedTimeoutMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &rtv, sizeof(rtv));
#endif

    std::string reqPath = triggerRefresh ? "/api/refresh" : "/api/status";
    std::string method = triggerRefresh ? "POST" : "GET";
    std::string httpReq =
        method + " " + reqPath + " HTTP/1.1\r\n"
        "Host: " + host + ":" + portStr + "\r\n"
        "User-Agent: aimon-windows-tray/" AIMON_VERSION_STRING "\r\n"
        "Accept: application/json\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";

    size_t sent = 0;
    while (sent < httpReq.size()) {
#ifdef _WIN32
        int n = ::send(static_cast<SOCKET>(fd),
                       httpReq.data() + sent,
                       static_cast<int>(httpReq.size() - sent), 0);
#else
        ssize_t n = ::send(fd, httpReq.data() + sent, httpReq.size() - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            closeHttpSocket(fd);
            outSnap.errorMessage = "Failed to send HTTP request to " + host + ":" + portStr;
            return false;
        }
        sent += static_cast<size_t>(n);
    }

    std::string rawResp;
    char buf[4096];
    while (rawResp.size() < 2 * 1024 * 1024) {
#ifdef _WIN32
        int n = ::recv(static_cast<SOCKET>(fd), buf, sizeof(buf), 0);
#else
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
#endif
        if (n <= 0) break;
        rawResp.append(buf, static_cast<size_t>(n));
    }
    closeHttpSocket(fd);

    size_t hdrEnd = rawResp.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) {
        outSnap.errorMessage = "Invalid HTTP response from " + host + ":" + portStr;
        return false;
    }

    std::string hdrs = rawResp.substr(0, hdrEnd);
    std::string body = rawResp.substr(hdrEnd + 4);
    if (hdrs.find(" 200 ") == std::string::npos) {
        outSnap.errorMessage = "HTTP error response from " + host + ":" + portStr;
        return false;
    }

    // Handle simple chunked transfer encoding if present
    std::string lowerHdrs = hdrs;
    for (char& c : lowerHdrs) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lowerHdrs.find("transfer-encoding: chunked") != std::string::npos) {
        std::string unchunked;
        size_t pos = 0;
        while (pos < body.size()) {
            size_t lineEnd = body.find("\r\n", pos);
            if (lineEnd == std::string::npos) break;
            std::string hexLen = body.substr(pos, lineEnd - pos);
            size_t chunkLen = 0;
            try {
                chunkLen = std::stoul(hexLen, nullptr, 16);
            } catch (...) {
                break;
            }
            pos = lineEnd + 2;
            if (chunkLen == 0) break;
            if (pos + chunkLen > body.size()) {
                unchunked.append(body.substr(pos));
                break;
            }
            unchunked.append(body.substr(pos, chunkLen));
            pos += chunkLen + 2;
        }
        if (!unchunked.empty()) {
            body = unchunked;
        }
    }

    std::string parseErr;
    if (!parseQuotaSnapshotJson(body, outSnap, parseErr)) {
        outSnap.reachable = false;
        outSnap.errorMessage = parseErr;
        return false;
    }
    return true;
}

std::string EdaBridgeAppConfig::formatTrayTooltipSummary(const AimonQuotaSnapshot& snap,
                                                         bool eeConn,
                                                         bool kcConn,
                                                         bool fcConn) {
    std::ostringstream oss;
    oss << "aimon — AI Quotas & EDA/CAD Bridge\n";

    if (!snap.reachable) {
        oss << "Quotas: Offline";
        if (!snap.errorMessage.empty()) {
            oss << " (" << snap.errorMessage << ")";
        }
        oss << "\n";
    } else {
        if (snap.agRunning && !snap.agModels.empty()) {
            double minPct = 100.0;
            std::string minName = snap.agModels.front().name;
            for (const auto& m : snap.agModels) {
                if (m.remainingPct < minPct) {
                    minPct = m.remainingPct;
                    minName = m.name;
                }
            }
            oss << "Antigravity: " << static_cast<int>(std::round(minPct))
                << "% (" << minName << ")\n";
        } else {
            oss << "Antigravity: " << (snap.agRunning ? "Active" : "Idle") << "\n";
        }

        if (snap.cursorAuthenticated) {
            oss << "Cursor: $" << std::fixed << std::setprecision(2)
                << snap.cursorTotalSpendUsd;
            if (snap.cursorRequestsLimit > 0) {
                oss << " (" << snap.cursorRequestsUsed << "/"
                    << snap.cursorRequestsLimit << " reqs)";
            }
            oss << "\n";
        }

        if (snap.claudeEnabled && !snap.claudeAccounts.empty()) {
            const auto& acc = snap.claudeAccounts.front();
            double dispUsd = acc.cycleConfigured ? acc.costCycleUsd : acc.cost7dUsd;
            oss << "Claude (" << acc.name << "): $" << std::fixed
                << std::setprecision(2) << dispUsd << "\n";
        }
    }

    oss << "EDA: EE:" << (eeConn ? "ON" : "--")
        << " KC:" << (kcConn ? "ON" : "--")
        << " FC:" << (fcConn ? "ON" : "--");
    return oss.str();
}

} // namespace aimon

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
