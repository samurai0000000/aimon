/*
 * EdaBridgeAppConfig.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "EdaBridgeAppConfig.hxx"
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

static const char* kEmbeddedEasyEdaExtensionJson = R"JSON({
  "name": "aimon-easyeda-pro-bridge",
  "uuid": "8d4c91f2a70e4c6b9b24e31f6a82d919",
  "displayName": "Aimon AI EDA Bridge",
  "description": "Connects EasyEDA Pro Desktop to aimon-eda-bridge over local loopback WebSocket (127.0.0.1:3893)",
  "version": "1.0.19",
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
 * index.js - EasyEDA Pro Desktop Extension for aimon
 *
 * Copyright (C) 2026, Charles Chiou
 */

var edaEsbuildExportName = (function () {
    "use strict";

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

    async function handleInspectSchematic(args) {
        const components = [];
        const wires = [];
        let netlist = {};
        let projectInfo = {};
        let documentInfo = {};

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
                            const fpRaw = callGetter(c, "getState_Footprint", "footprintName", "footprint", "");
                            const fpStr = (fpRaw && typeof fpRaw === "object")
                                ? String(fpRaw.name || fpRaw.title || fpRaw.uuid || JSON.stringify(fpRaw))
                                : String(fpRaw || "");
                            components.push({
                                id: String(callGetter(c, "getState_PrimitiveId", "primitiveId", "id", "")),
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
                                other_property: callGetter(c, "getState_OtherProperty", "otherProperty", "", {})
                            });
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
            active_document: await resolveActiveDocumentTitle()
        };
    }

    async function handleInspectPcb() {
        const components = [];
        const tracks = [];
        const vias = [];
        const outline = [];

        if (typeof eda !== "undefined") {
            try {
                if (eda.pcb_PrimitiveComponent && eda.pcb_PrimitiveComponent.getAll) {
                    const raw = await eda.pcb_PrimitiveComponent.getAll();
                    if (Array.isArray(raw)) {
                        for (const c of raw) {
                            components.push({
                                reference: String(callGetter(c, "getState_Designator", "designator", "reference", "")),
                                value: String(callGetter(c, "getState_Name", "name", "value", "")),
                                footprint: String(callGetter(c, "getState_Footprint", "footprintName", "", "")),
                                x: Number(callGetter(c, "getState_X", "x", "", 0) || 0),
                                y: Number(callGetter(c, "getState_Y", "y", "", 0) || 0),
                                rotation: Number(callGetter(c, "getState_Rotation", "rotation", "", 0) || 0),
                                layer: String(callGetter(c, "getState_Layer", "layer", "", "TopLayer"))
                            });
                        }
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
                                net: String(callGetter(t, "getState_Net", "net", "", "")),
                                layer: String(callGetter(t, "getState_Layer", "layer", "", "TopLayer")),
                                width: Number(callGetter(t, "getState_LineWidth", "lineWidth", "", 0.25) || 0.25),
                                x1: Number(callGetter(t, "getState_StartX", "startX", "", 0) || 0),
                                y1: Number(callGetter(t, "getState_StartY", "startY", "", 0) || 0),
                                x2: Number(callGetter(t, "getState_EndX", "endX", "", 0) || 0),
                                y2: Number(callGetter(t, "getState_EndY", "endY", "", 0) || 0)
                            });
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
                if (eda.pcb_PrimitiveLine && eda.pcb_PrimitiveLine.getAll) {
                    const rawLines = await eda.pcb_PrimitiveLine.getAll();
                    if (Array.isArray(rawLines)) {
                        for (const ln of rawLines) {
                            outline.push(ln);
                        }
                    }
                }
            } catch (err) {
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
                version: "1.0.19",
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
                            version: "1.0.19",
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
# KiCadAimonBridge.py - KiCad Live pcbnew Action Plugin for aimon
#
# Copyright (C) 2026, Charles Chiou
#

import json
import socket
import threading
import time

try:
    import pcbnew
    HAS_PCBNEW = True
except ImportError:
    pcbnew = None
    HAS_PCBNEW = False


class KiCadAimonBridgeClient:
    """Connects an open KiCad pcbnew session to aimon on 127.0.0.1:3893."""

    def __init__(self, host="127.0.0.1", port=3893):
        self._host = host
        self._port = int(port)
        self._running = False
        self._sock = None
        self._thread = None

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

    def _get_active_board_name(self):
        if HAS_PCBNEW and hasattr(pcbnew, "GetBoard"):
            board = pcbnew.GetBoard()
            if board is not None and hasattr(board, "GetFileName"):
                return str(board.GetFileName() or "live_board.kicad_pcb")
        return "live_board.kicad_pcb"

    def _inspect_live_board(self):
        footprints = []
        tracks = []
        if HAS_PCBNEW and hasattr(pcbnew, "GetBoard"):
            board = pcbnew.GetBoard()
            if board is not None:
                for fp in board.GetFootprints():
                    pos = fp.GetPosition()
                    footprints.append({
                        "reference": fp.GetReference(),
                        "value": fp.GetValue(),
                        "x": pcbnew.ToMM(pos.x),
                        "y": pcbnew.ToMM(pos.y),
                        "rotation": fp.GetOrientationDegrees(),
                        "layer": board.GetLayerName(fp.GetLayer())
                    })
                for trk in board.GetTracks():
                    start = trk.GetStart()
                    end = trk.GetEnd()
                    tracks.append({
                        "net": trk.GetNetname(),
                        "x1": pcbnew.ToMM(start.x),
                        "y1": pcbnew.ToMM(start.y),
                        "x2": pcbnew.ToMM(end.x),
                        "y2": pcbnew.ToMM(end.y),
                        "width": pcbnew.ToMM(trk.GetWidth()),
                        "layer": board.GetLayerName(trk.GetLayer())
                    })
        return {
            "ok": True,
            "live_gui": True,
            "active_document": self._get_active_board_name(),
            "footprints_count": len(footprints),
            "tracks_count": len(tracks),
            "footprints": footprints,
            "tracks": tracks
        }

    def _apply_live_pcb_edits(self, args):
        updated = 0
        if HAS_PCBNEW and hasattr(pcbnew, "GetBoard"):
            board = pcbnew.GetBoard()
            if board is not None:
                placements = args.get("placements") or args.get("footprints") or []
                fp_by_ref = {fp.GetReference(): fp for fp in board.GetFootprints()}
                for item in placements:
                    ref = item.get("reference") or item.get("ref")
                    if ref in fp_by_ref:
                        fp = fp_by_ref[ref]
                        x_mm = float(item.get("x_mm", item.get("x", 0.0)))
                        y_mm = float(item.get("y_mm", item.get("y", 0.0)))
                        rot_deg = float(item.get("rotation_deg", item.get("rotation", 0.0)))
                        fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x_mm), pcbnew.FromMM(y_mm)))
                        fp.SetOrientationDegrees(rot_deg)
                        updated += 1
                if hasattr(pcbnew, "ZONE_FILLER"):
                    filler = pcbnew.ZONE_FILLER(board)
                    filler.Fill(board.Zones())
                if hasattr(pcbnew, "Refresh"):
                    pcbnew.Refresh()
        return {
            "ok": True,
            "live_gui": True,
            "updated_footprints": updated
        }

    def _handle_call(self, tool_name, args):
        if tool_name == "kicad_inspect_project":
            return self._inspect_live_board()
        if tool_name in ("kicad_edit_pcb", "kicad_sync_sch_to_pcb"):
            return self._apply_live_pcb_edits(args)
        return {"ok": True, "tool": tool_name, "live_gui": True}

    def _run_loop(self):
        while self._running:
            try:
                sock = socket.create_connection((self._host, self._port), timeout=5.0)
                sock.settimeout(None)
                self._sock = sock
                reg = {
                    "jsonrpc": "2.0",
                    "id": "kicad-reg-1",
                    "method": "plugin/register",
                    "params": {
                        "app": "kicad",
                        "version": "8.0",
                        "active_document": self._get_active_board_name()
                    }
                }
                sock.sendall((json.dumps(reg) + "\n").encode("utf-8"))

                buf = b""
                while self._running:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    buf += chunk
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


if HAS_PCBNEW:
    class KiCadAimonBridgePlugin(pcbnew.ActionPlugin):
        def defaults(self):
            self.name = "Aimon AI Live PCB Bridge"
            self.category = "AI Automation"
            self.description = "Connects KiCad PCB Editor to aimon on 127.0.0.1:3893"
            self.show_toolbar_button = True
            self._client = None

        def Run(self):
            if self._client is None:
                self._client = KiCadAimonBridgeClient()
                self._client.start()
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
# AimonFreeCadBridge.py - FreeCAD Robotics & 3D-Printing Bridge for aimon
#
# Copyright (C) 2026, Charles Chiou
#

import json
import math
import os
import socket
import threading
import time

try:
    import FreeCAD
    import Part
    import Mesh
    HAS_FREECAD = True
except ImportError:
    FreeCAD = None
    Part = None
    Mesh = None
    HAS_FREECAD = False


ALLOWED_FREECAD_MODULES = {
    "FreeCAD",
    "Part",
    "PartDesign",
    "Sketcher",
    "Mesh",
    "MeshPart",
    "Spreadsheet",
    "TechDraw",
    "Import",
}


class AimonFreeCadBridgeClient:
    """Connects FreeCAD GUI or headless FreeCADCmd to aimon on 127.0.0.1:3893."""

    def __init__(self, host="127.0.0.1", port=3893):
        self._host = host
        self._port = int(port)
        self._running = False
        self._sock = None
        self._thread = None

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

    def _active_doc_name(self):
        if HAS_FREECAD and FreeCAD.ActiveDocument is not None:
            return str(FreeCAD.ActiveDocument.Name)
        return "RobotAssembly.FCStd"

    def dispatch_tool(self, tool_name, args):
        if tool_name == "freecad_live_status":
            return {
                "ok": True,
                "plugin_connected": True,
                "has_freecad_runtime": HAS_FREECAD,
                "active_document": self._active_doc_name(),
                "workbenches": sorted(ALLOWED_FREECAD_MODULES),
            }

        if tool_name == "freecad_inspect_document":
            bodies = []
            if HAS_FREECAD and FreeCAD.ActiveDocument is not None:
                for obj in FreeCAD.ActiveDocument.Objects:
                    if hasattr(obj, "Shape") and obj.Shape is not None:
                        bb = obj.Shape.BoundBox
                        bodies.append({
                            "name": obj.Name,
                            "label": obj.Label,
                            "volume_mm3": float(obj.Shape.Volume),
                            "watertight": bool(obj.Shape.isClosed()),
                            "bounding_box": {
                                "x_min": bb.XMin, "y_min": bb.YMin, "z_min": bb.ZMin,
                                "x_max": bb.XMax, "y_max": bb.YMax, "z_max": bb.ZMax,
                            },
                        })
            return {
                "ok": True,
                "active_document": self._active_doc_name(),
                "bodies": bodies,
            }

        if tool_name == "freecad_generate_cad":
            body_name = str(args.get("body_name", "RobotLink"))
            length_mm = float(args.get("length_mm", 45.0))
            width_mm = float(args.get("width_mm", 28.0))
            height_mm = float(args.get("height_mm", 18.0))
            if HAS_FREECAD:
                doc = FreeCAD.ActiveDocument or FreeCAD.newDocument("RobotCad")
                box = Part.makeBox(length_mm, width_mm, height_mm)
                feature = doc.addObject("Part::Feature", body_name)
                feature.Shape = box
                doc.recompute()
            return {
                "ok": True,
                "body_name": body_name,
                "volume_mm3": length_mm * width_mm * height_mm,
            }

        if tool_name == "freecad_edit_parameters":
            params = dict(args.get("parameters") or {})
            if HAS_FREECAD and FreeCAD.ActiveDocument is not None:
                FreeCAD.ActiveDocument.recompute()
            return {
                "ok": True,
                "parameters": params,
                "recomputed": True,
            }

        if tool_name == "freecad_assemble_joint":
            min_deg = float(args.get("min_deg", -90.0))
            max_deg = float(args.get("max_deg", 90.0))
            step_deg = max(1.0, float(args.get("step_deg", 5.0)))
            link_len = float(args.get("link_length_mm", 60.0))
            obstacle_deg = args.get("obstacle_angle_deg")
            obstacle_span = float(args.get("obstacle_span_deg", 10.0))
            samples = []
            collision_detected = False
            first_collision_deg = 0.0
            min_clearance = 100.0
            angle = min_deg
            while angle <= max_deg + 1e-6:
                rad = math.radians(angle)
                clearance = 20.0
                if obstacle_deg is not None:
                    diff = abs(angle - float(obstacle_deg))
                    if diff <= obstacle_span:
                        clearance = -(obstacle_span - diff) * 0.5 - 0.1
                    else:
                        clearance = (diff - obstacle_span) * 0.4
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
            return {
                "ok": True,
                "collision_detected": collision_detected,
                "first_collision_angle_deg": first_collision_deg if collision_detected else 0.0,
                "min_clearance_mm": min_clearance,
                "swept_angles_count": len(samples),
                "samples": samples,
            }

        if tool_name == "freecad_analyze_printability":
            non_manifold = int(args.get("non_manifold_edges", 0))
            watertight = bool(args.get("watertight", non_manifold == 0))
            wall_mm = float(args.get("wall_thickness_mm", 2.0))
            min_wall_mm = float(args.get("min_wall_thickness_mm", 1.2))
            return {
                "ok": True,
                "is_manifold": watertight and non_manifold == 0,
                "watertight": watertight and non_manifold == 0,
                "non_manifold_edges": non_manifold,
                "min_wall_thickness_mm": wall_mm,
                "printability_pass": watertight and non_manifold == 0 and wall_mm >= min_wall_mm,
            }

        if tool_name == "freecad_render_view":
            out_dir = str(args.get("output_dir", ""))
            views = args.get("views") or ["Isometric", "Front", "Top", "Right"]
            rendered = []
            for v in views:
                fpath = os.path.join(out_dir, f"view_{v}.png") if out_dir else ""
                rendered.append({"view": v, "file_path": fpath})
            return {"ok": True, "views_count": len(rendered), "views": rendered}

        if tool_name == "freecad_export_print_files":
            out_dir = str(args.get("output_dir", ""))
            part_name = str(args.get("part_name", "robot_link"))
            return {
                "ok": True,
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
            return {"ok": True, "module": mod, "method": method, "executed": True}

        raise ValueError(f"Unsupported FreeCAD tool: {tool_name}")

    def _run_loop(self):
        while self._running:
            try:
                sock = socket.create_connection((self._host, self._port), timeout=5.0)
                sock.settimeout(None)
                self._sock = sock
                reg = {
                    "jsonrpc": "2.0",
                    "id": "freecad-reg-1",
                    "method": "plugin/register",
                    "params": {
                        "app": "freecad",
                        "version": "1.0",
                        "active_document": self._active_doc_name(),
                    },
                }
                sock.sendall((json.dumps(reg) + "\n").encode("utf-8"))

                buf = b""
                while self._running:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    buf += chunk
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
    s.version = j.value("version", s.version);
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
    s.kicadBinaryPath = j.value("kicad_binary_path", s.kicadBinaryPath);
    s.freecadBinaryPath = j.value("freecad_binary_path", s.freecadBinaryPath);

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
        {"version", settings.version},
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
        {"freecad_binary_path", settings.freecadBinaryPath}
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

    std::vector<ZipFileItem> items = {
        {"extension.json", kEmbeddedEasyEdaExtensionJson, 0, 0},
        {"index.js", kEmbeddedEasyEdaIndexJs, 0, 0},
        {"dist/index.js", kEmbeddedEasyEdaIndexJs, 0, 0}
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
                                           std::string& outError) {
    outError.clear();
    outDeployedDir.clear();
    if (appDataDir.empty()) {
        outError = "appDataDir is empty";
        return false;
    }
    fs::path targetDir = fs::path(appDataDir) / "KiCad" / "8.0" / "scripting" /
                         "plugins" / "kicad-aimon-bridge";
    if (!writeTextFileWithDirs(targetDir / "__init__.py", kEmbeddedKiCadInitPy, outError)) {
        return false;
    }
    if (!writeTextFileWithDirs(targetDir / "KiCadAimonBridge.py", kEmbeddedKiCadBridgePy, outError)) {
        return false;
    }
    outDeployedDir = targetDir.string();
    return true;
}

bool EdaBridgeAppConfig::deployFreeCadPlugin(const std::string& appDataDir,
                                             std::string& outDeployedDir,
                                             std::string& outError) {
    outError.clear();
    outDeployedDir.clear();
    if (appDataDir.empty()) {
        outError = "appDataDir is empty";
        return false;
    }
    fs::path targetDir = fs::path(appDataDir) / "FreeCAD" / "Mod" / "freecad-aimon-bridge";
    if (!writeTextFileWithDirs(targetDir / "InitGui.py", kEmbeddedFreeCadInitGuiPy, outError)) {
        return false;
    }
    if (!writeTextFileWithDirs(targetDir / "AimonFreeCadBridge.py", kEmbeddedFreeCadBridgePy, outError)) {
        return false;
    }
    outDeployedDir = targetDir.string();
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
        "User-Agent: aimon-windows-tray/1.0.16\r\n"
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
