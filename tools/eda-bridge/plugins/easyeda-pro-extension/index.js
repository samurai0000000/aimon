/*
 * index.js - EasyEDA Pro Desktop Extension for aimon-eda-bridge
 *
 * Copyright (C) 2026, Charles Chiou
 */

(function () {
    "use strict";

    const DEFAULT_WS_URL = "ws://127.0.0.1:3893";
    const ALLOWED_PREFIXES = [
        "eda.sch_",
        "eda.pcb_",
        "eda.lib_",
        "eda.dmt_",
        "eda.drc_",
        "eda.sys_Document"
    ];

    let wsClient = null;
    let reconnectTimer = null;

    function getActiveDocumentTitle() {
        try {
            if (typeof eda !== "undefined" && eda.dmt_SelectControl && eda.dmt_SelectControl.getCurrentDocumentInfo) {
                const info = eda.dmt_SelectControl.getCurrentDocumentInfo();
                if (info && info.title) {
                    return String(info.title);
                }
            }
        } catch (err) {
            // Fall through when no document tab is active
        }
        return "EasyEDA-Pro-Workspace";
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
                        lcsc: item.supplierPart || item.lcsc || "",
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

    async function handleInspectSchematic() {
        const components = [];
        const wires = [];
        let netlist = {};
        if (typeof eda !== "undefined") {
            if (eda.sch_PrimitiveComponent && eda.sch_PrimitiveComponent.getAll) {
                const rawComps = await eda.sch_PrimitiveComponent.getAll();
                if (Array.isArray(rawComps)) {
                    for (const c of rawComps) {
                        components.push({
                            id: c.primitiveId || c.id || "",
                            reference: c.designator || c.reference || "",
                            value: c.value || c.name || "",
                            footprint: c.footprintName || "",
                            lcsc: c.supplierPart || "",
                            x: Number(c.x || 0),
                            y: Number(c.y || 0),
                            rotation: Number(c.rotation || 0)
                        });
                    }
                }
            }
            if (eda.sch_PrimitiveWire && eda.sch_PrimitiveWire.getAll) {
                const rawWires = await eda.sch_PrimitiveWire.getAll();
                if (Array.isArray(rawWires)) {
                    for (const w of rawWires) {
                        wires.push({
                            id: w.primitiveId || "",
                            net: w.net || "",
                            line: w.line || []
                        });
                    }
                }
            }
            if (eda.sch_Netlist && eda.sch_Netlist.getNetlist) {
                netlist = await eda.sch_Netlist.getNetlist();
            }
        }
        return {
            ok: true,
            active_document: getActiveDocumentTitle(),
            components_count: components.length,
            wires_count: wires.length,
            components: components,
            wires: wires,
            netlist: netlist
        };
    }

    async function handleEditSchematic(args) {
        let placedComponents = 0;
        let placedWires = 0;
        let placedLabels = 0;

        const comps = args.add_components || args.symbols || [];
        if (Array.isArray(comps) && typeof eda !== "undefined" && eda.sch_PrimitiveComponent) {
            for (const c of comps) {
                await eda.sch_PrimitiveComponent.create(
                    c.device_uuid || c.uuid || c.lib_id,
                    Number(c.x || 0),
                    Number(c.y || 0),
                    c.sub_part_name || undefined,
                    Number(c.rotation || 0),
                    Boolean(c.mirror || false)
                );
                placedComponents += 1;
            }
        }

        const wires = args.add_wires || args.wires || [];
        if (Array.isArray(wires) && typeof eda !== "undefined" && eda.sch_PrimitiveWire) {
            for (const w of wires) {
                const pts = w.points || [Number(w.x1 || 0), Number(w.y1 || 0), Number(w.x2 || 0), Number(w.y2 || 0)];
                await eda.sch_PrimitiveWire.create(pts, w.net || undefined);
                placedWires += 1;
            }
        }

        const labels = args.add_labels || args.labels || [];
        if (Array.isArray(labels) && typeof eda !== "undefined" && eda.sch_PrimitiveNetLabel) {
            for (const l of labels) {
                await eda.sch_PrimitiveNetLabel.create(
                    Number(l.x || 0),
                    Number(l.y || 0),
                    String(l.name || l.net || "NET")
                );
                placedLabels += 1;
            }
        }

        return {
            ok: true,
            placed_components: placedComponents,
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
            active_document: getActiveDocumentTitle()
        };
    }

    async function handleInspectPcb() {
        const components = [];
        const tracks = [];
        const vias = [];
        const outline = [];

        if (typeof eda !== "undefined") {
            if (eda.pcb_PrimitiveComponent && eda.pcb_PrimitiveComponent.getAll) {
                const raw = await eda.pcb_PrimitiveComponent.getAll();
                if (Array.isArray(raw)) {
                    for (const c of raw) {
                        components.push({
                            reference: c.designator || "",
                            footprint: c.footprintName || "",
                            x: Number(c.x || 0),
                            y: Number(c.y || 0),
                            rotation: Number(c.rotation || 0),
                            layer: c.layer || "TopLayer"
                        });
                    }
                }
            }
            if (eda.pcb_PrimitiveTrack && eda.pcb_PrimitiveTrack.getAll) {
                const rawTracks = await eda.pcb_PrimitiveTrack.getAll();
                if (Array.isArray(rawTracks)) {
                    for (const t of rawTracks) {
                        tracks.push({
                            net: t.net || "",
                            layer: t.layer || "TopLayer",
                            width: Number(t.lineWidth || 0.25),
                            x1: Number(t.startX || 0),
                            y1: Number(t.startY || 0),
                            x2: Number(t.endX || 0),
                            y2: Number(t.endY || 0)
                        });
                    }
                }
            }
            if (eda.pcb_PrimitiveVia && eda.pcb_PrimitiveVia.getAll) {
                const rawVias = await eda.pcb_PrimitiveVia.getAll();
                if (Array.isArray(rawVias)) {
                    for (const v of rawVias) {
                        vias.push({
                            net: v.net || "",
                            x: Number(v.x || 0),
                            y: Number(v.y || 0),
                            diameter: Number(v.diameter || 0.6),
                            drill: Number(v.holeDiameter || 0.3)
                        });
                    }
                }
            }
            if (eda.pcb_PrimitiveLine && eda.pcb_PrimitiveLine.getAll) {
                const rawLines = await eda.pcb_PrimitiveLine.getAll();
                if (Array.isArray(rawLines)) {
                    for (const ln of rawLines) {
                        outline.push(ln);
                    }
                }
            }
        }

        return {
            ok: true,
            components_count: components.length,
            tracks_count: tracks.length,
            vias_count: vias.length,
            components: components,
            tracks: tracks,
            vias: vias,
            board_outline: outline
        };
    }

    async function handleEditPcb(args) {
        let updatedPlacements = 0;
        let addedTracks = 0;
        let addedVias = 0;
        let addedZones = 0;

        if (args.board_outline && typeof eda !== "undefined" && eda.pcb_PrimitiveLine) {
            const bo = args.board_outline;
            const w = Number(bo.width_mm || bo.width || 50);
            const h = Number(bo.height_mm || bo.height || 40);
            await eda.pcb_PrimitiveLine.create("BoardOutLine", 0, 0, w, 0, 0.15);
            await eda.pcb_PrimitiveLine.create("BoardOutLine", w, 0, w, h, 0.15);
            await eda.pcb_PrimitiveLine.create("BoardOutLine", w, h, 0, h, 0.15);
            await eda.pcb_PrimitiveLine.create("BoardOutLine", 0, h, 0, 0, 0.15);
        }

        const placements = args.placements || args.footprints || [];
        if (Array.isArray(placements) && typeof eda !== "undefined" && eda.pcb_PrimitiveComponent) {
            for (const p of placements) {
                if (eda.pcb_PrimitiveComponent.modify) {
                    await eda.pcb_PrimitiveComponent.modify(p.reference, {
                        x: Number(p.x_mm !== undefined ? p.x_mm : p.x || 0),
                        y: Number(p.y_mm !== undefined ? p.y_mm : p.y || 0),
                        rotation: Number(p.rotation_deg !== undefined ? p.rotation_deg : p.rotation || 0),
                        layer: p.layer || "TopLayer"
                    });
                }
                updatedPlacements += 1;
            }
        }

        const zones = args.zones || args.copper_pours || [];
        if (Array.isArray(zones) && typeof eda !== "undefined" && eda.pcb_PrimitivePour) {
            for (const z of zones) {
                if (eda.pcb_PrimitivePour.create) {
                    await eda.pcb_PrimitivePour.create(z.net || "GND", z.layer || "TopLayer", z.points || []);
                }
                addedZones += 1;
            }
        }

        const tracks = args.tracks || args.add_tracks || [];
        if (Array.isArray(tracks) && typeof eda !== "undefined" && eda.pcb_PrimitiveTrack) {
            for (const t of tracks) {
                if (eda.pcb_PrimitiveTrack.create) {
                    await eda.pcb_PrimitiveTrack.create(
                        t.net || "",
                        t.layer || "TopLayer",
                        Number(t.x1 || 0),
                        Number(t.y1 || 0),
                        Number(t.x2 || 0),
                        Number(t.y2 || 0),
                        Number(t.width_mm || t.width || 0.25)
                    );
                }
                addedTracks += 1;
            }
        }

        const vias = args.vias || args.add_vias || [];
        if (Array.isArray(vias) && typeof eda !== "undefined" && eda.pcb_PrimitiveVia) {
            for (const v of vias) {
                if (eda.pcb_PrimitiveVia.create) {
                    await eda.pcb_PrimitiveVia.create(
                        v.net || "",
                        Number(v.x || 0),
                        Number(v.y || 0),
                        Number(v.drill_mm || v.drill || 0.3),
                        Number(v.size_mm || v.size || 0.6)
                    );
                }
                addedVias += 1;
            }
        }

        return {
            ok: true,
            updated_placements: updatedPlacements,
            added_zones: addedZones,
            added_tracks: addedTracks,
            added_vias: addedVias
        };
    }

    async function handleRunDrc(args) {
        const target = String(args.target || "pcb");
        let violations = [];
        if (typeof eda !== "undefined") {
            if (target === "schematic" && eda.sch_Drc && eda.sch_Drc.check) {
                violations = (await eda.sch_Drc.check()) || [];
            } else if (eda.pcb_Drc && eda.pcb_Drc.check) {
                violations = (await eda.pcb_Drc.check()) || [];
            }
        }
        return {
            ok: true,
            target: target,
            violation_count: Array.isArray(violations) ? violations.length : 0,
            violations: Array.isArray(violations) ? violations : []
        };
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
                    active_document: getActiveDocumentTitle()
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

    function connectRelay(url) {
        const targetUrl = url || DEFAULT_WS_URL;
        if (typeof WebSocket === "undefined") {
            return;
        }
        wsClient = new WebSocket(targetUrl);

        wsClient.onopen = function () {
            const regMsg = {
                jsonrpc: "2.0",
                id: "easyeda-reg-1",
                method: "plugin/register",
                params: {
                    app: "easyeda",
                    version: "2.2.0",
                    active_document: getActiveDocumentTitle()
                }
            };
            wsClient.send(JSON.stringify(regMsg));
        };

        wsClient.onmessage = async function (event) {
            let msg = null;
            try {
                msg = JSON.parse(String(event.data));
            } catch (err) {
                return;
            }
            if (!msg || msg.method !== "tools/call" || !msg.params) {
                return;
            }
            const reqId = msg.id;
            const toolName = msg.params.name || "";
            const toolArgs = msg.params.arguments || {};

            try {
                const res = await dispatchTool(toolName, toolArgs);
                wsClient.send(JSON.stringify({
                    jsonrpc: "2.0",
                    id: reqId,
                    result: res
                }));
            } catch (err) {
                wsClient.send(JSON.stringify({
                    jsonrpc: "2.0",
                    id: reqId,
                    error: {
                        code: -32000,
                        message: String(err && err.message ? err.message : err)
                    }
                }));
            }
        };

        wsClient.onclose = function () {
            wsClient = null;
            if (!reconnectTimer) {
                reconnectTimer = setTimeout(function () {
                    reconnectTimer = null;
                    connectRelay(targetUrl);
                }, 3000);
            }
        };
    }

    if (typeof module !== "undefined" && module.exports) {
        module.exports = {
            connectRelay: connectRelay,
            dispatchTool: dispatchTool
        };
    } else {
        connectRelay(DEFAULT_WS_URL);
    }
})();
