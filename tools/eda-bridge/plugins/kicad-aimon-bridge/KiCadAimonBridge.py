#
# KiCadAimonBridge.py - KiCad Live pcbnew Action Plugin for aimon-eda-bridge
#
# Copyright (C) 2026, Charles Chiou
#

import json
import socket
import threading
import time

BRIDGE_VERSION = "1.1.0"

try:
    import pcbnew
    HAS_PCBNEW = True
except ImportError:
    pcbnew = None
    HAS_PCBNEW = False


class KiCadAimonBridgeClient:
    """Connects an open KiCad pcbnew session to aimon-eda-bridge on 127.0.0.1:3893."""

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
                        "version": BRIDGE_VERSION,
                        "bridge_version": BRIDGE_VERSION,
                        "app_version": "8.0",
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
            self.description = "Connects KiCad PCB Editor to aimon-eda-bridge on 127.0.0.1:3893"
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
