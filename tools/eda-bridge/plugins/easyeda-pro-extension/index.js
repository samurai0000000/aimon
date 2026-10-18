/*
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
        const isMm = String(units || "mil").toLowerCase() === "mm";
        if (!isMm && obj && keyCanvas && obj[keyCanvas] !== undefined && obj[keyCanvas] !== null) {
            return Number(obj[keyCanvas]);
        }
        if (obj && keyMm && obj[keyMm] !== undefined && obj[keyMm] !== null) {
            return Math.round(Number(obj[keyMm]) * MM_TO_MIL * 10000) / 10000;
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

    // documentType 3 is a PCB document (verified live via eda.dmt_SelectControl.getCurrentDocumentInfo).
    // Returns null when the requested (or current) PCB is active, else an { ok: false, error } result,
    // so PCB tools never silently read the Start Page or another document as an empty board.
    async function requirePcbDocument(args) {
        const wanted = (args && args.pcb_uuid) ? String(args.pcb_uuid).trim() : "";
        const ensured = await ensureDocumentOpen("pcb", wanted, null);
        const info = (ensured && ensured.documentInfo) || {};
        const activeTitle = await resolveActiveDocumentTitle();
        if (Number(info.documentType) !== 3) {
            return {
                ok: false,
                error: (wanted ? "PCB document " + wanted + " could not be opened" : "No PCB document is open") +
                    " (active document: " + activeTitle + ", documentType: " +
                    String(info.documentType === undefined ? "unknown" : info.documentType) +
                    "). Open the project and its PCB in EasyEDA Pro.",
                active_document: activeTitle
            };
        }
        if (wanted && String(info.uuid || "") !== wanted) {
            return {
                ok: false,
                error: "Requested PCB " + wanted + " is not active (active PCB: " + String(info.uuid || "") +
                    "). It may belong to a project that is not open.",
                active_document: activeTitle
            };
        }
        return null;
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
            const pcbErr = await requirePcbDocument(args);
            if (pcbErr) return pcbErr;
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
                        const rawPadNum = callGetter(p, "getState_PadNumber", "padNumber", "number", "");
                        const rawPadField = callGetter(p, "getState_Pad", "pad", "", null);
                        const padNum = String(
                            (rawPadNum !== undefined && rawPadNum !== null && rawPadNum !== "")
                                ? rawPadNum
                                : ((!Array.isArray(rawPadField) && rawPadField !== null && typeof rawPadField !== "object")
                                    ? rawPadField
                                    : "")
                        );
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
                        let sizeX = Number(callGetter(p, "getState_Width", "width", "size_x", 0) || 0);
                        let sizeY = Number(callGetter(p, "getState_Height", "height", "size_y", 0) || 0);
                        let padShape = String(callGetter(p, "getState_Shape", "shape", "", "") || "");
                        let padPolygon = null;
                        if (Array.isArray(rawPadField) && rawPadField.length >= 2) {
                            const kind = String(rawPadField[0] || "").toUpperCase();
                            if (!padShape) padShape = kind;
                            if ((kind === "OVAL" || kind === "RECT" || kind === "ELLIPSE" || kind === "OCTAGON") &&
                                rawPadField.length >= 3) {
                                const wVal = Number(rawPadField[1] || 0);
                                const hVal = Number(rawPadField[2] || 0);
                                if (sizeX <= 0 && wVal > 0) sizeX = wVal;
                                if (sizeY <= 0 && hVal > 0) sizeY = hVal;
                            } else if ((kind === "ROUND" || kind === "CIRCLE") && rawPadField.length >= 2) {
                                const dVal = Number(rawPadField[1] || 0);
                                if (sizeX <= 0 && dVal > 0) sizeX = dVal;
                                if (sizeY <= 0 && dVal > 0) sizeY = dVal;
                            } else if (kind === "POLYGON") {
                                const coords = Array.isArray(rawPadField[1]) ? rawPadField[1] : rawPadField.slice(1);
                                const numPts = [];
                                for (let k = 0; k + 1 < coords.length; k += 2) {
                                    const vx = Number(coords[k]);
                                    const vy = Number(coords[k + 1]);
                                    if (!isNaN(vx) && !isNaN(vy)) {
                                        numPts.push([vx, vy]);
                                    }
                                }
                                if (numPts.length >= 3) {
                                    padPolygon = numPts;
                                    let minPx = numPts[0][0], maxPx = numPts[0][0];
                                    let minPy = numPts[0][1], maxPy = numPts[0][1];
                                    for (let k = 1; k < numPts.length; ++k) {
                                        if (numPts[k][0] < minPx) minPx = numPts[k][0];
                                        if (numPts[k][0] > maxPx) maxPx = numPts[k][0];
                                        if (numPts[k][1] < minPy) minPy = numPts[k][1];
                                        if (numPts[k][1] > maxPy) maxPy = numPts[k][1];
                                    }
                                    if (sizeX <= 0) sizeX = Math.max(0, maxPx - minPx);
                                    if (sizeY <= 0) sizeY = Math.max(0, maxPy - minPy);
                                }
                            }
                        }
                        let holeDiameter = Number(callGetter(p, "getState_HoleDiameter", "holeDiameter", "drill", 0) || 0);
                        const rawHoleField = callGetter(p, "getState_Hole", "hole", "", null);
                        if (holeDiameter <= 0 && Array.isArray(rawHoleField) && rawHoleField.length >= 2) {
                            const hKind = String(rawHoleField[0] || "").toUpperCase();
                            if (hKind === "ROUND" || hKind === "CIRCLE") {
                                const hd = Number(rawHoleField[1] || 0);
                                if (hd > 0) holeDiameter = hd;
                            } else if (hKind === "SLOT" && rawHoleField.length >= 3) {
                                const hw = Number(rawHoleField[1] || 0);
                                const hh = Number(rawHoleField[2] || 0);
                                const hd = Math.min(hw > 0 ? hw : hh, hh > 0 ? hh : hw);
                                if (hd > 0) holeDiameter = hd;
                            } else {
                                const hd = Number(rawHoleField[1] || 0);
                                if (hd > 0) holeDiameter = hd;
                            }
                        } else if (holeDiameter <= 0 && typeof rawHoleField === "number" && rawHoleField > 0) {
                            holeDiameter = rawHoleField;
                        }
                        let padLayer = String(callGetter(p, "getState_Layer", "layer", "", "1"));
                        if (holeDiameter > 0 && (padLayer === "1" || padLayer === "2" || padLayer === "TopLayer" || padLayer === "BottomLayer")) {
                            padLayer = "12";
                        }
                        const padRec = {
                            id: pid,
                            ref: padRef,
                            pad: padNum,
                            number: padNum,
                            net: String(callGetter(p, "getState_Net", "net", "", "")),
                            x: Number(callGetter(p, "getState_X", "x", "", 0) || 0),
                            y: Number(callGetter(p, "getState_Y", "y", "", 0) || 0),
                            layer: padLayer,
                            rotation: Number(callGetter(p, "getState_Rotation", "rotation", "", 0) || 0),
                            size_x: sizeX,
                            size_y: sizeY,
                            hole_diameter: holeDiameter
                        };
                        if (padShape) padRec.shape = padShape;
                        if (padPolygon) padRec.polygon = padPolygon;
                        pads.push(padRec);
                    }
                }
            } catch (err) {
            }
            function appendPolygonOutlineSegments(recId, lid, widthVal, rawPoly) {
                if (!rawPoly) return;
                const pts = [];
                let src = [];
                if (Array.isArray(rawPoly)) {
                    src = rawPoly;
                } else if (typeof rawPoly === "object") {
                    if (Array.isArray(rawPoly.polygon)) src = rawPoly.polygon;
                    else if (Array.isArray(rawPoly.Source)) src = rawPoly.Source;
                    else if (Array.isArray(rawPoly.source)) src = rawPoly.source;
                    else if (Array.isArray(rawPoly.points)) src = rawPoly.points;
                }
                for (let i = 0; i < src.length; ++i) {
                    const item = src[i];
                    if (typeof item === "string" && item.toUpperCase() === "ARC") {
                        if (i + 1 < src.length && typeof src[i + 1] === "number") {
                            i += 1;
                        }
                        continue;
                    }
                    if (item && typeof item === "object" && !Array.isArray(item) &&
                        item.x !== undefined && item.y !== undefined) {
                        const px = Number(item.x);
                        const py = Number(item.y);
                        if (!isNaN(px) && !isNaN(py)) pts.push([px, py]);
                    } else if (Array.isArray(item) && item.length >= 2) {
                        const px = Number(item[0]);
                        const py = Number(item[1]);
                        if (!isNaN(px) && !isNaN(py)) pts.push([px, py]);
                    } else if (typeof item === "number" && i + 1 < src.length && typeof src[i + 1] === "number") {
                        pts.push([Number(item), Number(src[i + 1])]);
                        i += 1;
                    }
                }
                for (let i = 0; i + 1 < pts.length; ++i) {
                    outline.push({
                        id: recId,
                        layer: String(lid),
                        width: widthVal,
                        x1: pts[i][0],
                        y1: pts[i][1],
                        x2: pts[i + 1][0],
                        y2: pts[i + 1][1]
                    });
                }
                if (pts.length >= 3) {
                    const first = pts[0];
                    const last = pts[pts.length - 1];
                    if (Math.hypot(first[0] - last[0], first[1] - last[1]) > 1e-3) {
                        outline.push({
                            id: recId,
                            layer: String(lid),
                            width: widthVal,
                            x1: last[0],
                            y1: last[1],
                            x2: first[0],
                            y2: first[1]
                        });
                    }
                }
            }
            try {
                if (eda.pcb_PrimitiveTrack && eda.pcb_PrimitiveTrack.getAll) {
                    const rawTracks = await eda.pcb_PrimitiveTrack.getAll();
                    if (Array.isArray(rawTracks)) {
                        for (const t of rawTracks) {
                            const lid = callGetter(t, "getState_Layer", "layer", "", "TopLayer");
                            const recId = String(callGetter(t, "getState_PrimitiveId", "primitiveId", "id", ""));
                            const w = Number(callGetter(t, "getState_LineWidth", "lineWidth", "width", 0.25) || 0.25);
                            const x1 = Number(callGetter(t, "getState_StartX", "startX", "x1", 0) || 0);
                            const y1 = Number(callGetter(t, "getState_StartY", "startY", "y1", 0) || 0);
                            const x2 = Number(callGetter(t, "getState_EndX", "endX", "x2", 0) || 0);
                            const y2 = Number(callGetter(t, "getState_EndY", "endY", "y2", 0) || 0);
                            if (isBoardOutlineLayer(lid)) {
                                outline.push({
                                    id: recId,
                                    layer: String(lid),
                                    width: w,
                                    x1: x1,
                                    y1: y1,
                                    x2: x2,
                                    y2: y2
                                });
                            } else {
                                tracks.push({
                                    id: recId,
                                    net: String(callGetter(t, "getState_Net", "net", "", "")),
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
                if (eda.pcb_PrimitivePolyline && typeof eda.pcb_PrimitivePolyline.getAll === "function") {
                    const rawPolys = await eda.pcb_PrimitivePolyline.getAll();
                    if (Array.isArray(rawPolys)) {
                        for (const pl of rawPolys) {
                            const lid = callGetter(pl, "getState_Layer", "layer", "", "");
                            if (!isBoardOutlineLayer(lid)) continue;
                            const recId = String(callGetter(pl, "getState_PrimitiveId", "primitiveId", "id", ""));
                            const w = Number(callGetter(pl, "getState_LineWidth", "lineWidth", "width", 0.25) || 0.25);
                            const poly = callGetter(pl, "getState_Polygon", "polygon", "points", []);
                            appendPolygonOutlineSegments(recId, lid, w, poly);
                        }
                    }
                }
            } catch (err) {
            }
            try {
                if (eda.pcb_PrimitiveArc && typeof eda.pcb_PrimitiveArc.getAll === "function") {
                    const rawArcs = await eda.pcb_PrimitiveArc.getAll();
                    if (Array.isArray(rawArcs)) {
                        for (const ar of rawArcs) {
                            const lid = callGetter(ar, "getState_Layer", "layer", "", "");
                            if (!isBoardOutlineLayer(lid)) continue;
                            const recId = String(callGetter(ar, "getState_PrimitiveId", "primitiveId", "id", ""));
                            const w = Number(callGetter(ar, "getState_LineWidth", "lineWidth", "width", 0.25) || 0.25);
                            const x1 = Number(callGetter(ar, "getState_StartX", "startX", "x1", 0) || 0);
                            const y1 = Number(callGetter(ar, "getState_StartY", "startY", "y1", 0) || 0);
                            const x2 = Number(callGetter(ar, "getState_EndX", "endX", "x2", 0) || 0);
                            const y2 = Number(callGetter(ar, "getState_EndY", "endY", "y2", 0) || 0);
                            if (Math.hypot(x2 - x1, y2 - y1) > 1e-3) {
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
                if (eda.pcb_PrimitiveRegion && typeof eda.pcb_PrimitiveRegion.getAll === "function") {
                    const rawRegions = await eda.pcb_PrimitiveRegion.getAll();
                    if (Array.isArray(rawRegions)) {
                        for (const rg of rawRegions) {
                            const lid = callGetter(rg, "getState_Layer", "layer", "", "");
                            if (!isBoardOutlineLayer(lid)) continue;
                            const recId = String(callGetter(rg, "getState_PrimitiveId", "primitiveId", "id", ""));
                            const poly = callGetter(rg, "getState_Polygon", "polygon", "points", []);
                            appendPolygonOutlineSegments(recId, lid, 0.15, poly);
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

        let boardDesignRules = null;
        if (typeof eda !== "undefined") {
            const ruleNamespaces = [eda.pcb_DesignRule, eda.pcb_Drc, eda.dmt_Pcb];
            const ruleMethods = ["getDesignRule", "getDesignRules", "getCurrentDesignRule", "getRule", "getRules", "getRuleConfiguration"];
            for (const ns of ruleNamespaces) {
                if (!ns || typeof ns !== "object") continue;
                for (const mName of ruleMethods) {
                    if (typeof ns[mName] !== "function") continue;
                    try {
                        const rawRule = await ns[mName]();
                        if (rawRule && typeof rawRule === "object") {
                            boardDesignRules = rawRule;
                            break;
                        }
                    } catch (err) {
                    }
                }
                if (boardDesignRules) break;
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
        if (boardDesignRules) {
            out.board_design_rules = boardDesignRules;
        }
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
            const pcbErr = await requirePcbDocument(args);
            if (pcbErr) return pcbErr;
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

        const createdTrackIds = [];
        const createdViaIds = [];
        function extractCreatedPrimitiveId(createdObj) {
            if (!createdObj) return "";
            if (typeof createdObj === "string") return createdObj;
            if (typeof createdObj === "object") {
                return String(callGetter(createdObj, "getState_PrimitiveId", "primitiveId", "id", "") || "");
            }
            return "";
        }

        const tracks = args.tracks || args.add_tracks || [];
        const preTrackIds = {};
        if (Array.isArray(tracks) && tracks.length > 0 && typeof eda !== "undefined") {
            try {
                if (eda.pcb_PrimitiveTrack && typeof eda.pcb_PrimitiveTrack.getAll === "function") {
                    const existingT = await eda.pcb_PrimitiveTrack.getAll();
                    if (Array.isArray(existingT)) {
                        for (const et of existingT) {
                            const tid = String(callGetter(et, "getState_PrimitiveId", "primitiveId", "id", ""));
                            if (tid) preTrackIds[tid] = true;
                        }
                    }
                }
                if (eda.pcb_PrimitiveLine && typeof eda.pcb_PrimitiveLine.getAll === "function") {
                    const existingL = await eda.pcb_PrimitiveLine.getAll();
                    if (Array.isArray(existingL)) {
                        for (const el of existingL) {
                            const lid = String(callGetter(el, "getState_PrimitiveId", "primitiveId", "id", ""));
                            if (lid) preTrackIds[lid] = true;
                        }
                    }
                }
            } catch (err) {
            }
            for (const t of tracks) {
                const tNet = String(t.net || "");
                const tX1 = coordToMil(t, "x1", "x1_mm", units, 0);
                const tY1 = coordToMil(t, "y1", "y1_mm", units, 0);
                const tX2 = coordToMil(t, "x2", "x2_mm", units, 0);
                const tY2 = coordToMil(t, "y2", "y2_mm", units, 0);
                const tW = coordToMil(t, "width", "width_mm", units, 10.0);
                if (eda.pcb_PrimitiveTrack && typeof eda.pcb_PrimitiveTrack.create === "function") {
                    const createdTrk = await eda.pcb_PrimitiveTrack.create(
                        tNet,
                        normalizePcbLayer(t.layer || "TopLayer"),
                        tX1,
                        tY1,
                        tX2,
                        tY2,
                        tW
                    );
                    const newTid = extractCreatedPrimitiveId(createdTrk);
                    if (newTid && createdTrackIds.indexOf(newTid) < 0) createdTrackIds.push(newTid);
                    addedTracks += 1;
                } else if (eda.pcb_PrimitiveLine && typeof eda.pcb_PrimitiveLine.create === "function") {
                    const layerId = resolveEasyEdaCopperLayerId(t.layer || "TopLayer");
                    const createdLn = await eda.pcb_PrimitiveLine.create(
                        tNet,
                        layerId,
                        tX1,
                        tY1,
                        tX2,
                        tY2,
                        tW,
                        false
                    );
                    const newLid = extractCreatedPrimitiveId(createdLn);
                    if (newLid && createdTrackIds.indexOf(newLid) < 0) createdTrackIds.push(newLid);
                    addedTracks += 1;
                }
            }
            try {
                if (eda.pcb_PrimitiveTrack && typeof eda.pcb_PrimitiveTrack.getAll === "function") {
                    const postT = await eda.pcb_PrimitiveTrack.getAll();
                    if (Array.isArray(postT)) {
                        for (const pt of postT) {
                            const tid = String(callGetter(pt, "getState_PrimitiveId", "primitiveId", "id", ""));
                            if (tid && !preTrackIds[tid] && createdTrackIds.indexOf(tid) < 0) {
                                createdTrackIds.push(tid);
                            }
                        }
                    }
                }
                if (eda.pcb_PrimitiveLine && typeof eda.pcb_PrimitiveLine.getAll === "function") {
                    const postL = await eda.pcb_PrimitiveLine.getAll();
                    if (Array.isArray(postL)) {
                        for (const pl of postL) {
                            const lid = String(callGetter(pl, "getState_PrimitiveId", "primitiveId", "id", ""));
                            if (lid && !preTrackIds[lid] && createdTrackIds.indexOf(lid) < 0) {
                                createdTrackIds.push(lid);
                            }
                        }
                    }
                }
            } catch (err) {
            }
        }

        const vias = args.vias || args.add_vias || [];
        const preViaIds = {};
        if (Array.isArray(vias) && vias.length > 0 && typeof eda !== "undefined" && eda.pcb_PrimitiveVia) {
            try {
                if (typeof eda.pcb_PrimitiveVia.getAll === "function") {
                    const existingV = await eda.pcb_PrimitiveVia.getAll();
                    if (Array.isArray(existingV)) {
                        for (const ev of existingV) {
                            const vid = String(callGetter(ev, "getState_PrimitiveId", "primitiveId", "id", ""));
                            if (vid) preViaIds[vid] = true;
                        }
                    }
                }
            } catch (err) {
            }
            for (const v of vias) {
                if (eda.pcb_PrimitiveVia.create) {
                    const viaDrill = coordToMil(v, "drill", "drill_mm", units, 12.0);
                    const viaDia = (v.size !== undefined || v.size_mm !== undefined)
                        ? coordToMil(v, "size", "size_mm", units, 24.0)
                        : coordToMil(v, "diameter", "diameter_mm", units, 24.0);
                    const createdVia = await eda.pcb_PrimitiveVia.create(
                        v.net || "",
                        coordToMil(v, "x", "x_mm", units, 0),
                        coordToMil(v, "y", "y_mm", units, 0),
                        viaDrill,
                        viaDia
                    );
                    const newVid = extractCreatedPrimitiveId(createdVia);
                    if (newVid && createdViaIds.indexOf(newVid) < 0) createdViaIds.push(newVid);
                }
                addedVias += 1;
            }
            try {
                if (typeof eda.pcb_PrimitiveVia.getAll === "function") {
                    const postV = await eda.pcb_PrimitiveVia.getAll();
                    if (Array.isArray(postV)) {
                        for (const pv of postV) {
                            const vid = String(callGetter(pv, "getState_PrimitiveId", "primitiveId", "id", ""));
                            if (vid && !preViaIds[vid] && createdViaIds.indexOf(vid) < 0) {
                                createdViaIds.push(vid);
                            }
                        }
                    }
                }
            } catch (err) {
            }
        }

        try {
            if (typeof eda !== "undefined" && eda.dmt_EditorControl && typeof eda.dmt_EditorControl.zoomToAllPrimitives === "function") {
                await eda.dmt_EditorControl.zoomToAllPrimitives();
            }
        } catch (err) {
        }

        const committedIds = createdTrackIds.concat(createdViaIds);
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
            created_track_ids: createdTrackIds,
            created_via_ids: createdViaIds,
            committed_ids: committedIds,
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
        const pcbErr = await requirePcbDocument(args);
        if (pcbErr) return Object.assign({ triggered: false }, pcbErr);
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
                const pcbErr = await requirePcbDocument(args);
                if (pcbErr) return pcbErr;
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
                const flattenDrcItems = function (arr) {
                    for (const item of arr) {
                        if (item && typeof item === "object") {
                            const subList = item.list || item.items || item.errors || item.violations;
                            if (Array.isArray(subList)) {
                                rawCategoriesCount += 1;
                                flattenDrcItems(subList);
                            } else {
                                violations.push(item);
                            }
                        } else if (item !== undefined && item !== null) {
                            violations.push(item);
                        }
                    }
                };
                flattenDrcItems(raw);
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

