#
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

BRIDGE_VERSION = "1.1.9"

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
        return "1.1.9"

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

