#
# InitGui.py - FreeCAD Workbench & Startup Hook for aimon-eda-bridge
#
# Copyright (C) 2026, Charles Chiou
#

from AimonFreeCadBridge import start_aimon_freecad_bridge

try:
    start_aimon_freecad_bridge()
except Exception:
    pass
