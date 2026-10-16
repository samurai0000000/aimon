#!/usr/bin/env python3
#
# pack_windows_installer.py
#
# Copyright (C) 2026, Charles Chiou
#
# Stages the cross-compiled Qt6 Windows System Tray application (aimon.exe),
# EasyEDA Pro extension archive (aimon-easyeda-bridge.eext), and required
# Qt6 / MinGW runtime DLLs into build/windows/app/, then packages the versioned
# self-contained Windows installer at build/windows/aimon-setup-<version>.exe.
#

import argparse
import io
import os
import re
import shutil
import struct
import sys
import time
import zipfile
from pathlib import Path


INSTALLER_MAGIC = b"AIMON_INST_V1"


def detect_project_version(root_dir: Path) -> str:
    cmake_file = root_dir / "CMakeLists.txt"
    if cmake_file.is_file():
        content = cmake_file.read_text(encoding="utf-8", errors="replace")
        m = re.search(r"project\s*\(\s*aimon\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", content)
        if m:
            return m.group(1)
    return "1.0.17"


def build_easyeda_eext(plugin_dir: Path, out_eext: Path) -> None:
    ext_json = plugin_dir / "extension.json"
    index_js = plugin_dir / "index.js"
    if not ext_json.is_file() or not index_js.is_file():
        raise FileNotFoundError(f"Missing EasyEDA extension sources in {plugin_dir}")

    out_eext.parent.mkdir(parents=True, exist_ok=True)
    index_bytes = index_js.read_bytes()
    with zipfile.ZipFile(out_eext, "w", compression=zipfile.ZIP_STORED) as zf:
        zf.writestr("extension.json", ext_json.read_bytes())
        zf.writestr("index.js", index_bytes)
        zf.writestr("dist/index.js", index_bytes)


def find_mingw_dll(dll_name: str, qt_bin_dir: Path) -> Path:
    # Prefer the official prebuilt Qt mingw_64/bin runtime DLLs (cleanly linked,
    # 2.2 MB release build matching Qt6Core/Qt6Gui/Qt6Widgets without post-link
    # strip corruption; aimon.exe is compiled with
    # -D_GLIBCXX_USE_INIT_PRIORITY_ATTRIBUTE=0 so it does not require GCC 13's
    # _ZSt21ios_base_library_initv).
    candidate_dirs = [
        qt_bin_dir,
        Path("/usr/lib/gcc/x86_64-w64-mingw32/13-posix"),
        Path("/usr/x86_64-w64-mingw32/lib"),
        Path("/usr/x86_64-w64-mingw32/bin"),
    ]
    for d in candidate_dirs:
        p = d / dll_name
        if p.is_file():
            return p
    gcc_root = Path("/usr/lib/gcc/x86_64-w64-mingw32")
    if gcc_root.is_dir():
        for sub in sorted(gcc_root.glob("*posix*"), reverse=True):
            p = sub / dll_name
            if p.is_file():
                return p
    raise FileNotFoundError(f"Required MinGW runtime DLL not found: {dll_name}")


def update_pe_timestamp_and_checksum(pe_bytes: bytearray) -> None:
    if len(pe_bytes) < 0x40 or pe_bytes[:2] != b"MZ":
        return
    pe_off = struct.unpack_from("<I", pe_bytes, 0x3C)[0]
    if pe_off + 24 + 68 > len(pe_bytes) or pe_bytes[pe_off : pe_off + 4] != b"PE\x00\x00":
        return
    struct.pack_into("<I", pe_bytes, pe_off + 8, int(time.time()) & 0xFFFFFFFF)
    cs_off = pe_off + 24 + 64
    struct.pack_into("<I", pe_bytes, cs_off, 0)
    import array
    total_len = len(pe_bytes)
    pad = total_len % 2
    if pad:
        pe_bytes.append(0)
    words = array.array("H")
    words.frombytes(pe_bytes)
    if sys.byteorder != "little":
        words.byteswap()
    s = sum(words)
    while s > 0xFFFF:
        s = (s & 0xFFFF) + (s >> 16)
    if pad:
        pe_bytes.pop()
    checksum = (s + total_len) & 0xFFFFFFFF
    struct.pack_into("<I", pe_bytes, cs_off, checksum)


import subprocess
import tempfile

# Standard Windows OS system DLLs provided by Windows 10/11 (case-insensitive)
WINDOWS_SYSTEM_DLLS = {
    "advapi32.dll",
    "authz.dll",
    "comctl32.dll",
    "comdlg32.dll",
    "crypt32.dll",
    "d3d9.dll",
    "d3d11.dll",
    "d3d12.dll",
    "dwmapi.dll",
    "dwrite.dll",
    "dxgi.dll",
    "gdi32.dll",
    "imm32.dll",
    "iphlpapi.dll",
    "kernel32.dll",
    "mpr.dll",
    "msvcrt.dll",
    "netapi32.dll",
    "ntdll.dll",
    "ole32.dll",
    "oleaut32.dll",
    "opengl32.dll",
    "rpcrt4.dll",
    "secur32.dll",
    "setupapi.dll",
    "shcore.dll",
    "shell32.dll",
    "shlwapi.dll",
    "ucrtbase.dll",
    "user32.dll",
    "userenv.dll",
    "uxtheme.dll",
    "version.dll",
    "winhttp.dll",
    "winmm.dll",
    "ws2_32.dll",
    "wtsapi32.dll",
}


def is_windows_system_dll(dll_name: str) -> bool:
    low = dll_name.lower()
    if low in WINDOWS_SYSTEM_DLLS:
        return True
    if low.startswith("api-ms-win-") or low.startswith("ext-ms-win-"):
        return True
    return False


def get_pe_imported_dlls(pe_path: Path) -> list[str]:
    objdump = shutil.which("x86_64-w64-mingw32-objdump") or "objdump"
    out = subprocess.check_output([objdump, "-p", str(pe_path)], text=True, errors="replace")
    return re.findall(r"DLL Name:\s+(\S+)", out)


def write_bytes_fsync(path: Path, data: bytes, mode: int = 0o755) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.chmod(path, mode)


def copy_file_fsync(src: Path, dst: Path, mode: int = 0o755) -> None:
    write_bytes_fsync(dst, src.read_bytes(), mode=mode)


def stage_windows_app(
    root_dir: Path,
    build_dir: Path,
    qt_prefix: Path,
    stage_dir: Path,
    out_root: Path,
) -> None:
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    stage_dir.mkdir(parents=True, exist_ok=True)

    built_exe = build_dir / "aimon-eda-bridge.exe"
    if not built_exe.is_file():
        raise FileNotFoundError(f"Built executable not found: {built_exe}")

    copy_file_fsync(built_exe, stage_dir / "aimon.exe", mode=0o755)

    # Build and stage EasyEDA Pro .eext archive
    eext_src_dir = root_dir / "tools" / "eda-bridge" / "plugins" / "easyeda-pro-extension"
    staged_eext = stage_dir / "aimon-easyeda-bridge.eext"
    build_easyeda_eext(eext_src_dir, staged_eext)
    copy_file_fsync(staged_eext, out_root / "aimon-easyeda-bridge.eext", mode=0o644)

    qt_bin = qt_prefix / "bin"
    qt_plugins = qt_prefix / "plugins"

    # Qt platform and style plugins
    qwindows_dll = qt_plugins / "platforms" / "qwindows.dll"
    if not qwindows_dll.is_file():
        raise FileNotFoundError(f"Missing Qt platform plugin: {qwindows_dll}")
    copy_file_fsync(qwindows_dll, stage_dir / "platforms" / "qwindows.dll", mode=0o755)

    qstyle_dll = qt_plugins / "styles" / "qmodernwindowsstyle.dll"
    if qstyle_dll.is_file():
        copy_file_fsync(qstyle_dll, stage_dir / "styles" / "qmodernwindowsstyle.dll", mode=0o755)

    # Recursively discover and copy all non-OS transitive DLL dependencies
    queue: list[Path] = [
        stage_dir / "aimon.exe",
        stage_dir / "platforms" / "qwindows.dll",
    ]
    if (stage_dir / "styles" / "qmodernwindowsstyle.dll").is_file():
        queue.append(stage_dir / "styles" / "qmodernwindowsstyle.dll")

    staged_dlls_lower: set[str] = set()
    while queue:
        current_pe = queue.pop(0)
        for dll_name in get_pe_imported_dlls(current_pe):
            low = dll_name.lower()
            if is_windows_system_dll(low) or low in staged_dlls_lower:
                continue
            src_dll = find_mingw_dll(dll_name, qt_bin)
            dst_dll = stage_dir / dll_name
            copy_file_fsync(src_dll, dst_dll, mode=0o755)
            staged_dlls_lower.add(low)
            queue.append(dst_dll)


def patch_embedded_zip_central_directory(
    exe_ba: bytearray,
    zip_offset: int,
    zip_len: int,
    footer_len: int,
) -> None:
    eocd_pos = zip_offset + zip_len - 22
    if exe_ba[eocd_pos : eocd_pos + 4] != b"PK\x05\x06":
        raise RuntimeError("Embedded RCDATA ZIP does not end with a 22-byte EOCD record")

    cd_size = struct.unpack_from("<I", exe_ba, eocd_pos + 12)[0]
    cd_rel_off = struct.unpack_from("<I", exe_ba, eocd_pos + 16)[0]
    cd_abs_off = zip_offset + cd_rel_off

    pos = cd_abs_off
    cd_end = cd_abs_off + cd_size
    while pos < cd_end:
        if exe_ba[pos : pos + 4] != b"PK\x01\x02":
            raise RuntimeError("Corrupt Central Directory entry in embedded RCDATA ZIP")
        name_len = struct.unpack_from("<H", exe_ba, pos + 28)[0]
        extra_len = struct.unpack_from("<H", exe_ba, pos + 30)[0]
        comment_len = struct.unpack_from("<H", exe_ba, pos + 32)[0]
        local_hdr_off = struct.unpack_from("<I", exe_ba, pos + 42)[0]
        struct.pack_into("<I", exe_ba, pos + 42, local_hdr_off + zip_offset)
        pos += 46 + name_len + extra_len + comment_len

    trailing_pe_bytes = len(exe_ba) - (eocd_pos + 22)
    total_comment_len = trailing_pe_bytes + footer_len
    if total_comment_len > 0xFFFF:
        raise RuntimeError(f"Trailing PE bytes after RCDATA too large for ZIP comment: {total_comment_len}")

    struct.pack_into("<I", exe_ba, eocd_pos + 16, cd_abs_off)
    struct.pack_into("<H", exe_ba, eocd_pos + 20, total_comment_len)


def build_self_extracting_installer(
    root_dir: Path,
    stub_exe: Path,
    stage_dir: Path,
    installer_out: Path,
) -> None:
    if not stub_exe.is_file():
        raise FileNotFoundError(f"Installer stub executable not found: {stub_exe}")

    # Verify stub_exe has zero dynamic MinGW runtime DLL dependencies
    stub_imports = get_pe_imported_dlls(stub_exe)
    non_sys_stub = [d for d in stub_imports if not is_windows_system_dll(d)]
    if non_sys_stub:
        raise RuntimeError(
            f"Installer stub {stub_exe} must be statically linked, but imports: {non_sys_stub}"
        )

    stub_bytes = stub_exe.read_bytes()

    with tempfile.TemporaryDirectory(prefix="aimon-rcdata-") as tmp_rc_dir_str:
        tmp_rc_dir = Path(tmp_rc_dir_str)
        payload_zip = tmp_rc_dir / "payload.zip"

        with zipfile.ZipFile(payload_zip, "w", compression=zipfile.ZIP_STORED) as zf:
            for file_path in sorted(stage_dir.rglob("*")):
                if file_path.is_file():
                    rel_name = file_path.relative_to(stage_dir).as_posix()
                    zf.writestr(rel_name, file_path.read_bytes())
            # Bundle the pure 1.1 MB installer stub as aimon-uninstall.exe inside the archive
            zf.writestr("aimon-uninstall.exe", stub_bytes)

        payload_bytes = payload_zip.read_bytes()
        if len(payload_bytes) < 22 or payload_bytes[-22:-18] != b"PK\x05\x06":
            raise RuntimeError("Generated ZIP archive does not end with a standard 22-byte EOCD record")

        src_dir = root_dir / "tools" / "eda-bridge" / "src"
        aimon_rc_path = (src_dir / "aimon.rc").as_posix()
        payload_zip_posix = payload_zip.as_posix()

        installer_rc = tmp_rc_dir / "installer_payload.rc"
        installer_rc.write_text(
            f'#include "{aimon_rc_path}"\n101 RCDATA "{payload_zip_posix}"\n',
            encoding="utf-8",
        )

        windres = shutil.which("x86_64-w64-mingw32-windres") or "x86_64-w64-mingw32-windres"
        cxx = shutil.which("x86_64-w64-mingw32-g++-posix") or "x86_64-w64-mingw32-g++"
        res_obj = tmp_rc_dir / "installer_payload.res.o"
        linked_exe = tmp_rc_dir / "aimon-setup-linked.exe"

        subprocess.run(
            [windres, "-I", str(src_dir), str(installer_rc), "-O", "coff", "-o", str(res_obj)],
            check=True,
        )
        subprocess.run(
            [
                cxx,
                "-O2",
                "-s",
                "-std=c++17",
                "-mwindows",
                "-static",
                "-static-libgcc",
                "-static-libstdc++",
                str(src_dir / "WindowsInstallerMain.cxx"),
                str(res_obj),
                "-lole32",
                "-lshell32",
                "-luuid",
                "-ladvapi32",
                "-luser32",
                "-o",
                str(linked_exe),
            ],
            check=True,
        )

        exe_ba = bytearray(linked_exe.read_bytes())
        zip_offset = exe_ba.find(payload_bytes[:64])
        if zip_offset < 0:
            raise RuntimeError("Could not locate embedded RCDATA ZIP inside linked installer PE")

        footer_len = len(INSTALLER_MAGIC) + 8
        patch_embedded_zip_central_directory(
            exe_ba, zip_offset, len(payload_bytes), footer_len
        )

        # Fallback footer records the span from zip_offset to the start of the footer
        span_to_footer = len(exe_ba) - zip_offset
        footer = INSTALLER_MAGIC + struct.pack("<Q", span_to_footer)
        exe_ba.extend(footer)
        update_pe_timestamp_and_checksum(exe_ba)

        write_bytes_fsync(installer_out, bytes(exe_ba), mode=0o755)


def main() -> int:
    parser = argparse.ArgumentParser(description="Package versioned Windows aimon installer")
    parser.add_argument("--root-dir", required=True, help="Repository root directory")
    parser.add_argument("--build-dir", required=True, help="CMake cross-build directory")
    parser.add_argument("--qt-prefix", required=True, help="Prebuilt Qt6 mingw_64 prefix")
    parser.add_argument("--output-dir", required=True, help="Output directory (e.g. build/windows)")
    args = parser.parse_args()

    root_dir = Path(args.root_dir).resolve()
    build_dir = Path(args.build_dir).resolve()
    qt_prefix = Path(args.qt_prefix).resolve()
    out_dir = Path(args.output_dir).resolve()

    version = detect_project_version(root_dir)
    stub_exe = build_dir / "aimon-installer-stub.exe"

    # Build the entire output directory inside /tmp first (with explicit fsync + close)
    # so Samba / Windows SMB client never sees partially written files or in-place
    # truncation on the shared filesystem.
    tmp_pkg_root = Path(tempfile.mkdtemp(prefix="aimon-win-pkg-"))
    try:
        tmp_out_dir = tmp_pkg_root / "windows"
        tmp_stage_dir = tmp_out_dir / "app"
        tmp_installer_exe = tmp_out_dir / f"aimon-setup-{version}.exe"

        stage_windows_app(root_dir, build_dir, qt_prefix, tmp_stage_dir, tmp_out_dir)
        build_self_extracting_installer(root_dir, stub_exe, tmp_stage_dir, tmp_installer_exe)

        out_dir.parent.mkdir(parents=True, exist_ok=True)
        fresh_dir = out_dir.parent / f".{out_dir.name}.staging-{os.getpid()}"
        if fresh_dir.exists():
            shutil.rmtree(fresh_dir)
        shutil.move(str(tmp_out_dir), str(fresh_dir))

        old_dir = out_dir.parent / f".{out_dir.name}.old-{os.getpid()}"
        if old_dir.exists():
            shutil.rmtree(old_dir)
        if out_dir.exists():
            out_dir.rename(old_dir)
        fresh_dir.rename(out_dir)
        if old_dir.exists():
            shutil.rmtree(old_dir)
    finally:
        if tmp_pkg_root.exists():
            shutil.rmtree(tmp_pkg_root, ignore_errors=True)

    stage_dir = out_dir / "app"
    installer_exe = out_dir / f"aimon-setup-{version}.exe"
    print(f"[aimon] Staged Windows System Tray app: {stage_dir / 'aimon.exe'}")
    print(f"[aimon] Packaged versioned Windows installer: {installer_exe}")
    print(f"[aimon] Packaged EasyEDA Pro extension archive: {out_dir / 'aimon-easyeda-bridge.eext'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
