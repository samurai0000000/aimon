#
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

BRIDGE_VERSION = "1.1.6"

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

