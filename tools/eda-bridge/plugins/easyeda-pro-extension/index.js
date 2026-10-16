/*
 * index.js - EasyEDA Pro Desktop Extension for aimon-eda-bridge
 *
 * Copyright (C) 2026, Charles Chiou
 */

var edaEsbuildExportName = (function () {
    "use strict";

    const BRIDGE_VERSION = "1.1.2";
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
                        net: String(callGetter(p, "getState_Net", "net", "", ""))
                    });
                }
            }
        } catch (err) {
            logDiag("getAllPinsByPrimitiveId(" + String(primitiveId) + ") error: " + String(err));
        }
        return pins;
    }

    async function handleInspectSchematic(args) {
        const components = [];
        const wires = [];
        let netlist = {};
        let projectInfo = {};
        let documentInfo = {};
        const includeOtherProp = Boolean(args && args.include_other_property === true);

        if (typeof eda !== "undefined") {
            try {
                if (eda.dmt_SelectControl && typeof eda.dmt_SelectControl.getCurrentDocumentInfo === "function") {
                    documentInfo = (await eda.dmt_SelectControl.getCurrentDocumentInfo()) || {};
                }
            } catch (err) {
            }
            if (!documentInfo || documentInfo.documentType === -1 || documentInfo.tabId === "tab_page1") {
                cachedActiveDocument = "Start Page";
                return {
                    ok: true,
                    active_document: cachedActiveDocument,
                    project: {},
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
                if (eda.dmt_Project && typeof eda.dmt_Project.getCurrentProjectInfo === "function") {
                    projectInfo = (await eda.dmt_Project.getCurrentProjectInfo()) || {};
                }
            } catch (err) {
            }
            try {
                if (eda.sch_PrimitiveComponent && eda.sch_PrimitiveComponent.getAll) {
                    const rawComps = await eda.sch_PrimitiveComponent.getAll();
                    if (Array.isArray(rawComps)) {
                        for (const c of rawComps) {
                            const compId = String(callGetter(c, "getState_PrimitiveId", "primitiveId", "id", ""));
                            const fpRaw = callGetter(c, "getState_Footprint", "footprintName", "footprint", "");
                            const fpStr = (fpRaw && typeof fpRaw === "object")
                                ? String(fpRaw.name || fpRaw.title || fpRaw.uuid || JSON.stringify(fpRaw))
                                : String(fpRaw || "");
                            const compPins = await extractComponentPins(compId);
                            const compRec = {
                                id: compId,
                                unique_id: String(callGetter(c, "getState_UniqueId", "uniqueId", "", "")),
                                component_type: String(callGetter(c, "getState_ComponentType", "componentType", "", "")),
                                reference: String(callGetter(c, "getState_Designator", "designator", "reference", "")),
                                value: String(callGetter(c, "getState_Name", "value", "name", "")),
                                footprint: fpStr,
                                lcsc: String(callGetter(c, "getState_SupplierId", "supplierPart", "supplierId", "")),
                                manufacturer: String(callGetter(c, "getState_Manufacturer", "manufacturer", "", "")),
                                manufacturer_part: String(callGetter(c, "getState_ManufacturerId", "manufacturerId", "", "")),
                                net: String(callGetter(c, "getState_Net", "net", "", "")),
                                x: Number(callGetter(c, "getState_X", "x", "", 0) || 0),
                                y: Number(callGetter(c, "getState_Y", "y", "", 0) || 0),
                                rotation: Number(callGetter(c, "getState_Rotation", "rotation", "", 0) || 0),
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
            if (args && args.include_netlist === true) {
                try {
                    if (eda.sch_Netlist && eda.sch_Netlist.getNetlist) {
                        netlist = (await eda.sch_Netlist.getNetlist("JLCEDA")) || {};
                    }
                } catch (err) {
                }
            }
        }
        return {
            ok: true,
            active_document: cachedActiveDocument,
            project: projectInfo,
            document: documentInfo,
            components_count: components.length,
            wires_count: wires.length,
            components: components,
            wires: wires,
            netlist: netlist
        };
    }

    const MM_TO_MIL = 39.37007874;

    function toCanvasUnits(val, units) {
        const n = Number(val || 0);
        if (String(units || "mm").toLowerCase() === "mil") {
            return n;
        }
        return Math.round(n * MM_TO_MIL * 1000) / 1000;
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
        const looksLikeUuid = typeof devArg === "string" && devArg.length >= 24 && devArg.indexOf(":") < 0;
        if (looksLikeUuid) {
            return libraryUuid ? { uuid: devArg, libraryUuid: libraryUuid } : devArg;
        }
        const searchKey = String(c.lcsc || c.query || c.value || c.lib_id || devArg || "");
        if (searchKey && typeof eda !== "undefined" && eda.lib_Device && typeof eda.lib_Device.search === "function") {
            try {
                const hits = await eda.lib_Device.search(searchKey, libraryUuid);
                if (Array.isArray(hits) && hits.length > 0 && hits[0].uuid) {
                    if (hits[0].libraryUuid) {
                        return { uuid: hits[0].uuid, libraryUuid: hits[0].libraryUuid };
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
        let placedWires = 0;
        let placedLabels = 0;
        const placedComponentDetails = [];
        const pinMap = {};
        const units = (args && args.units) ? String(args.units) : "mm";

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
                const resolvedDevice = await resolveDeviceArg(c);
                const xVal = toCanvasUnits(c.x !== undefined ? c.x : c.x_mm, units);
                const yVal = toCanvasUnits(c.y !== undefined ? c.y : c.y_mm, units);
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
            if (eda.sch_PrimitiveComponent && typeof eda.sch_PrimitiveComponent.getAll === "function") {
                try {
                    const existingComps = await eda.sch_PrimitiveComponent.getAll();
                    if (Array.isArray(existingComps)) {
                        for (const ec of existingComps) {
                            const ecRef = String(callGetter(ec, "getState_Designator", "designator", "reference", ""));
                            const ecId = String(callGetter(ec, "getState_PrimitiveId", "primitiveId", "id", ""));
                            if (ecRef && ecId) {
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
                        toCanvasUnits(w.x1 || 0, units),
                        toCanvasUnits(w.y1 || 0, units),
                        toCanvasUnits(w.x2 || 0, units),
                        toCanvasUnits(w.y2 || 0, units)
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
                    toCanvasUnits(l.x || 0, units),
                    toCanvasUnits(l.y || 0, units),
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
            placed_component_details: placedComponentDetails,
            placed_wires: placedWires,
            placed_labels: placedLabels
        };
    }

    async function handleSyncSchToPcb(args) {
        let synced = false;
        if (typeof eda !== "undefined") {
            if (eda.dmt_Pcb && eda.dmt_Pcb.syncFromSchematic) {
                await eda.dmt_Pcb.syncFromSchematic(args.pcb_uuid || undefined);
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
        const pads = [];
        const tracks = [];
        const vias = [];
        const outline = [];
        const includeOtherProp = Boolean(args && args.include_other_property === true);

        if (typeof eda !== "undefined") {
            try {
                if (eda.pcb_PrimitiveComponent && eda.pcb_PrimitiveComponent.getAll) {
                    const raw = await eda.pcb_PrimitiveComponent.getAll();
                    if (Array.isArray(raw)) {
                        for (const c of raw) {
                            const fpRaw = callGetter(c, "getState_Footprint", "footprintName", "footprint", "");
                            const fpStr = (fpRaw && typeof fpRaw === "object")
                                ? String(fpRaw.name || fpRaw.title || fpRaw.uuid || JSON.stringify(fpRaw))
                                : String(fpRaw || "");
                            const compRec = {
                                id: String(callGetter(c, "getState_PrimitiveId", "primitiveId", "id", "")),
                                reference: String(callGetter(c, "getState_Designator", "designator", "reference", "")),
                                value: String(callGetter(c, "getState_Name", "name", "value", "")),
                                footprint: fpStr,
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
                        pads.push({
                            id: pid,
                            pad: padNum,
                            number: padNum,
                            net: String(callGetter(p, "getState_Net", "net", "", "")),
                            x: Number(callGetter(p, "getState_X", "x", "", 0) || 0),
                            y: Number(callGetter(p, "getState_Y", "y", "", 0) || 0),
                            layer: String(callGetter(p, "getState_Layer", "layer", "", "1")),
                            rotation: Number(callGetter(p, "getState_Rotation", "rotation", "", 0) || 0)
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
        }

        return {
            ok: true,
            active_document: await resolveActiveDocumentTitle(),
            components_count: components.length,
            pads_count: pads.length,
            tracks_count: tracks.length,
            vias_count: vias.length,
            components: components,
            pads: pads,
            tracks: tracks,
            vias: vias,
            board_outline: outline
        };
    }

    async function handleEditPcb(args) {
        let updatedPlacements = 0;
        let createdFootprints = 0;
        let createdPads = 0;
        let addedTracks = 0;
        let addedVias = 0;
        let addedZones = 0;
        const units = (args && args.units) ? String(args.units) : "mm";

        if (args.board_outline && typeof eda !== "undefined" && eda.pcb_PrimitiveLine && eda.pcb_PrimitiveLine.create) {
            const bo = args.board_outline;
            const lineW = toCanvasUnits(0.15, units);
            if (Array.isArray(bo)) {
                for (const seg of bo) {
                    await eda.pcb_PrimitiveLine.create(
                        "BoardOutLine",
                        toCanvasUnits(seg.x1 || 0, units),
                        toCanvasUnits(seg.y1 || 0, units),
                        toCanvasUnits(seg.x2 || 0, units),
                        toCanvasUnits(seg.y2 || 0, units),
                        lineW
                    );
                }
            } else if (Array.isArray(bo.lines) && bo.lines.length > 0) {
                for (const seg of bo.lines) {
                    await eda.pcb_PrimitiveLine.create(
                        "BoardOutLine",
                        toCanvasUnits(seg.x1 || 0, units),
                        toCanvasUnits(seg.y1 || 0, units),
                        toCanvasUnits(seg.x2 || 0, units),
                        toCanvasUnits(seg.y2 || 0, units),
                        lineW
                    );
                }
            } else {
                const x0 = toCanvasUnits(bo.x || 0, units);
                const y0 = toCanvasUnits(bo.y || 0, units);
                const w = toCanvasUnits(bo.width_mm || bo.width || 50, units);
                const h = toCanvasUnits(bo.height_mm || bo.height || 40, units);
                await eda.pcb_PrimitiveLine.create("BoardOutLine", x0, y0, x0 + w, y0, lineW);
                await eda.pcb_PrimitiveLine.create("BoardOutLine", x0 + w, y0, x0 + w, y0 + h, lineW);
                await eda.pcb_PrimitiveLine.create("BoardOutLine", x0 + w, y0 + h, x0, y0 + h, lineW);
                await eda.pcb_PrimitiveLine.create("BoardOutLine", x0, y0 + h, x0, y0, lineW);
            }
        }

        const placements = args.placements || args.footprints || [];
        if (Array.isArray(placements) && typeof eda !== "undefined") {
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
                const ref = String(p.reference || p.ref || "");
                const xRaw = Number(p.x_mm !== undefined ? p.x_mm : p.x || 0);
                const yRaw = Number(p.y_mm !== undefined ? p.y_mm : p.y || 0);
                const xCanvas = toCanvasUnits(xRaw, units);
                const yCanvas = toCanvasUnits(yRaw, units);
                const rot = Number(p.rotation_deg !== undefined ? p.rotation_deg : p.rotation || 0);
                const layer = normalizePcbLayer(p.layer || "TopLayer");
                const existingComp = compByRef[ref];

                if (existingComp || (eda.pcb_PrimitiveComponent && eda.pcb_PrimitiveComponent.modify && !p.pads && !p.footprint)) {
                    const targetId = existingComp
                        ? String(callGetter(existingComp, "getState_PrimitiveId", "primitiveId", "id", ref))
                        : ref;
                    if (eda.pcb_PrimitiveComponent && typeof eda.pcb_PrimitiveComponent.modify === "function") {
                        await eda.pcb_PrimitiveComponent.modify(targetId, {
                            x: xCanvas,
                            y: yCanvas,
                            rotation: rot,
                            layer: layer
                        });
                    } else if (existingComp && typeof existingComp.setState_X === "function") {
                        await existingComp.setState_X(xCanvas);
                        await existingComp.setState_Y(yCanvas);
                    }
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

        const zones = args.zones || args.copper_pours || [];
        if (Array.isArray(zones) && typeof eda !== "undefined" && eda.pcb_PrimitivePour) {
            for (const z of zones) {
                const scaledPts = Array.isArray(z.points)
                    ? z.points.map(function (pt) {
                        if (pt && typeof pt === "object") {
                            return { x: toCanvasUnits(pt.x || 0, units), y: toCanvasUnits(pt.y || 0, units) };
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
                const tX1 = toCanvasUnits(t.x1 || 0, units);
                const tY1 = toCanvasUnits(t.y1 || 0, units);
                const tX2 = toCanvasUnits(t.x2 || 0, units);
                const tY2 = toCanvasUnits(t.y2 || 0, units);
                const tW = toCanvasUnits(t.width_mm || t.width || 0.25, units);
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
                        toCanvasUnits(v.x || 0, units),
                        toCanvasUnits(v.y || 0, units),
                        toCanvasUnits(v.drill_mm || v.drill || 0.3, units),
                        toCanvasUnits(v.size_mm || v.size || 0.6, units)
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
            created_footprints: createdFootprints,
            created_pads: createdPads,
            added_zones: addedZones,
            added_tracks: addedTracks,
            added_vias: addedVias
        };
    }

    async function handleRunDrc(args) {
        const target = String((args && args.target) || "pcb");
        let violations = [];
        let passed = true;
        let rawCategoriesCount = 0;

        if (typeof eda !== "undefined") {
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
            raw_categories: rawCategoriesCount,
            violation_count: violations.length,
            violations: violations
        };

        if (target === "pcb") {
            const pcbSnap = await handleInspectPcb({});
            out.pads = pcbSnap.pads || [];
            out.tracks = pcbSnap.tracks || [];
            out.vias = pcbSnap.vias || [];
            out.components = pcbSnap.components || [];
        }

        return out;
    }

    async function handleExportJlcpcb() {
        let gerberData = "PK\x03\x04JLCPCB_GERBER";
        let bomCsv = "Comment,Designator,Footprint,LCSC Part #\n";
        let cplCsv = "Designator,Mid X,Mid Y,Layer,Rotation\n";
        let dsnContent = "(pcb \"easyeda.dsn\")\n";

        if (typeof eda !== "undefined" && eda.pcb_ManufactureData) {
            if (eda.pcb_ManufactureData.getGerberFile) {
                gerberData = String((await eda.pcb_ManufactureData.getGerberFile()) || gerberData);
            }
            if (eda.pcb_ManufactureData.getBomFile) {
                bomCsv = String((await eda.pcb_ManufactureData.getBomFile()) || bomCsv);
            }
            if (eda.pcb_ManufactureData.getCplFile) {
                cplCsv = String((await eda.pcb_ManufactureData.getCplFile()) || cplCsv);
            }
            if (eda.pcb_ManufactureData.getDsnFile) {
                dsnContent = String((await eda.pcb_ManufactureData.getDsnFile()) || dsnContent);
            }
        }

        return {
            ok: true,
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

    function sendRawJson(obj) {
        const text = JSON.stringify(obj);
        if (useEdaSysWs) {
            if (typeof eda !== "undefined" && eda.sys_WebSocket && typeof eda.sys_WebSocket.send === "function") {
                eda.sys_WebSocket.send(currentWsId, text);
                return true;
            }
            return false;
        }
        if (wsClient && wsClient.readyState === 1) {
            wsClient.send(text);
            return true;
        }
        return false;
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

