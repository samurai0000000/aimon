#
# __init__.py - KiCad pcbnew Action Plugin Registration for aimon-eda-bridge
#
# Copyright (C) 2026, Charles Chiou
#

from .KiCadAimonBridge import KiCadAimonBridgePlugin

try:
    KiCadAimonBridgePlugin().register()
except Exception:
    # Allows importing outside of a live pcbnew GUI process during headless testing
    pass
