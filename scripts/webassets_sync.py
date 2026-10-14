#!/usr/bin/env python3
#
# webassets_sync.py
#
# Synchronize or check parity between web/{index.html,style.css,app.js,qrcode.js}
# and embedded raw string literals in include/WebAssets.hxx.
#
# Copyright (C) 2026, Charles Chiou
#

import argparse
from pathlib import Path
import sys

ASSETS = [
    ("INDEX_HTML", "web/index.html"),
    ("STYLE_CSS", "web/style.css"),
    ("APP_JS", "web/app.js"),
    ("QRCODE_JS", "web/qrcode.js"),
]


def build_header(root: Path) -> str:
    lines = [
        "/*",
        " * WebAssets.hxx",
        " *",
        " * Copyright (C) 2026, Charles Chiou",
        " */",
        "",
        "#ifndef AIMON_WEB_ASSETS_HXX",
        "#define AIMON_WEB_ASSETS_HXX",
        "",
        "namespace aimon {",
        "namespace assets {",
        "",
    ]
    for const_name, rel_path in ASSETS:
        content = (root / rel_path).read_text(encoding="utf-8")
        lines.append(f'inline const char* {const_name} = R"raw_asset({content})raw_asset";')
        lines.append("")
    lines.extend([
        "} // namespace assets",
        "} // namespace aimon",
        "",
        "#endif // AIMON_WEB_ASSETS_HXX",
        "",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Sync or check include/WebAssets.hxx against web/")
    parser.add_argument("--check", action="store_true", help="Exit non-zero if include/WebAssets.hxx is out of sync")
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    target = root / "include" / "WebAssets.hxx"
    expected = build_header(root)

    if args.check:
        if not target.exists():
            print("ERROR: include/WebAssets.hxx does not exist", file=sys.stderr)
            return 1
        actual = target.read_text(encoding="utf-8")
        if actual != expected:
            print("ERROR: include/WebAssets.hxx is out of sync with web/ files", file=sys.stderr)
            return 1
        print("OK: include/WebAssets.hxx is in sync with web/ files")
        return 0

    target.write_text(expected, encoding="utf-8")
    print(f"Synced {target} ({len(expected)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
