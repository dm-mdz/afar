#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Afar_C L0 static gate (T-AF-BLD-01 ~ T-AF-BLD-04).

Enforces:
  - Coding standard: UTF-8 no-BOM, LF, Doxygen @file/@brief, file <=800 lines, func <=80 lines.
  - 3-layer unidirectional dependency & shell/ header whitelist (DD-00 §3.2).
  - Apple framework, private API, <uv.h>, ipad/core zero-libdm, and ObjC af_objc_undef.h isolation.
  - Self-contained public/dm/VENDOR.txt SHA-256 lock on conn.c/http.c/ws.c (T-AF-BLD-03 L0).
  - Frozen ipad/ui/ SHA-256 lock & setup.html exact 5-line helper removal contract (T-AF-BLD-04).
"""
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SERVICE_MODS = {"auth", "env", "display", "sidecar", "power", "kbd"}
BIZ_MODS = {"session", "setup"}
NON_FOUNDATION_MODS = SERVICE_MODS | BIZ_MODS

SHELL_ALLOWED_LOCAL = {
    "af_ui_ipc.h",
    "af_modules.h",
    "af_events.h",
    "af_store.h",
    "plat/af_plat.h",
    "plat/af_objc_undef.h",
}
SHELL_ALLOWED_SYS = {
    "dm/link.h",
    "dm/module.h",
    "dmc_client.h",
}

APPLE_FW_NAMES = (
    "Foundation|Cocoa|AppKit|CoreGraphics|CoreFoundation|IOBluetooth|CoreBluetooth|"
    "OpenDirectory|Security|WebKit|SystemConfiguration|IOKit|CFNetwork|UIKit|Network|"
    "LocalAuthentication|UserNotifications|ServiceManagement|CoreServices|ApplicationServices"
)
APPLE_FW_RE = re.compile(
    rf"(?:^\s*#\s*(?:import|include)\s*<(?:{APPLE_FW_NAMES})/|(?<!\w)@import\s+(?:{APPLE_FW_NAMES})\b)",
    re.M,
)

PRIVATE_API_RE = re.compile(
    r"\b(?:SidecarCore|SidecarDisplayManager|SidecarDevice|"
    r"CGVirtualDisplay|CGVirtualDisplayDescriptor|CGVirtualDisplayMode|CGVirtualDisplaySettings|"
    r"CGSConfigureDisplayMode|CGSGetCurrentDisplayMode|CGSGetDisplayModeDescriptionOfLength|"
    r"CGSMainConnectionID|SLSConfigureDisplayMode)\b|/System/Library/PrivateFrameworks"
)

UV_INC_RE = re.compile(r'^\s*#\s*(?:include|import)\s*[<"]uv(?:\.h|/)', re.M)
DM_INC_RE = re.compile(r'^\s*#\s*(?:include|import)\s*[<"]dm(?:\.h|/)', re.M)
INCLUDE_RE = re.compile(r'^\s*#\s*(include|import)\s*([<"])([^>"]+)[>"]', re.M)

REQUIRED_VENDOR_FILES = {
    "public/dm/libdm/src/transport/conn.c",
    "public/dm/libdm/src/transport/http.c",
    "public/dm/libdm/src/transport/ws.c",
}

FROZEN_IPAD_UI = {
    "ipad/ui/index.html": "c9b962ef74c5c7e9a3b2a2256648b1514b1abe5060de505bcce4438af0ec97ca",
    "ipad/ui/i18n.js": "4d50914ca4fa07f49c3e7c4de2c64cd68013b9bd621c2498a52904d640782809",
    "ipad/ui/app.js": "3f9ad03e8510a1bcf93f5a263eff29b2794a9c312e4ba10b82bd6983430590e3",
    "ipad/ui/style.css": "72bd1e9ecaf0446ffbac53b64658bd3d59291b8ce17e683f1358dc56d8287e02",
}
FROZEN_SETUP_HTML_SHA256 = "15b9c046e57db7cbc80c5b4b3c2bfcf8ade7b72673dd5d35bbd13bc2516bea1d"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def lex_c(text: str) -> tuple:
    """Single-pass C/ObjC lexer preserving newlines.

    Returns:
      no_comments: comments replaced by spaces; string/char literals intact.
      code_only:   both comments and contents of string/char literals replaced by spaces.
    """
    n = len(text)
    nc = []
    co = []
    i = 0
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            nc.append("  ")
            co.append("  ")
            i += 2
            while i < n and text[i] != "\n":
                nc.append(" ")
                co.append(" ")
                i += 1
        elif ch == "/" and nxt == "*":
            nc.append("  ")
            co.append("  ")
            i += 2
            while i < n:
                if text[i] == "*" and i + 1 < n and text[i + 1] == "/":
                    nc.append("  ")
                    co.append("  ")
                    i += 2
                    break
                c = "\n" if text[i] == "\n" else " "
                nc.append(c)
                co.append(c)
                i += 1
        elif ch in ('"', "'"):
            quote = ch
            nc.append(quote)
            co.append(quote)
            i += 1
            while i < n:
                c = text[i]
                if c == "\\" and i + 1 < n:
                    nc.append(c)
                    nc.append(text[i + 1])
                    co.append(" ")
                    co.append("\n" if text[i + 1] == "\n" else " ")
                    i += 2
                    continue
                nc.append(c)
                if c == quote:
                    co.append(quote)
                    i += 1
                    break
                co.append("\n" if c == "\n" else " ")
                i += 1
        else:
            nc.append(ch)
            co.append(ch)
            i += 1
    return "".join(nc), "".join(co)


def norm_inc(rel_file: Path, inc: str) -> str:
    if inc.startswith(("../", "./")):
        resolved = os.path.normpath((rel_file.parent / inc).as_posix())
        return resolved[4:] if resolved.startswith("src/") else resolved
    return inc[4:] if inc.startswith("src/") else inc


def is_func_header(prefix: str) -> bool:
    """Return True iff the top-level text preceding '{' is a C function or ObjC method signature."""
    s = prefix.strip()
    if not s:
        return False
    # Strip leading preprocessor lines if any remained in segment
    s = "\n".join(ln for ln in s.splitlines() if not ln.lstrip().startswith("#")).strip()
    if not s:
        return False
    if s.endswith("="):
        return False
    if re.search(r'\bextern\s+"[^"]*"\s*$', s):
        return False
    # ObjC method definition (- (void)foo: or + (id)bar, including first method after @implementation)
    if re.search(r"(?:^|\n)\s*[+-]\s*\(", s):
        return True
    # C function definition ends with ')' (after parameter list or attribute)
    if s.endswith(")"):
        # Exclude typedef/struct/enum/union/NS_ENUM definitions if no function signature
        if re.match(r"^(?:typedef\s+)?(?:struct|enum|union)\b", s) and "(" not in s:
            return False
        if re.match(r"^(?:typedef\s+)?(?:NS_ENUM|NS_OPTIONS)\s*\(", s):
            return False
        return True
    return False


def check_func_lengths(rel: str, raw_text: str, code_only: str, errs: list) -> None:
    raw_lines = raw_text.splitlines()
    depth = 0
    fn_start = None
    fn_exempt = False
    line_no = 1
    seg = []

    for ch in code_only:
        if ch == "\n":
            line_no += 1
            if depth == 0:
                seg.append(ch)
            continue
        if ch == "{":
            if depth == 0:
                header = "".join(seg)
                if is_func_header(header):
                    fn_start = line_no
                    # Per-function AF_ALLOW_LONG_FUNC scope: only within 5 lines before '{' or same line
                    win = "\n".join(raw_lines[max(0, fn_start - 6):fn_start])
                    fn_exempt = "AF_ALLOW_LONG_FUNC" in win
                seg.clear()
            depth += 1
        elif ch == "}":
            if depth > 0:
                depth -= 1
            if depth == 0:
                if fn_start is not None:
                    length = line_no - fn_start + 1
                    if length > 80 and not fn_exempt:
                        errs.append(f"[T-AF-BLD-04] {rel}:{fn_start}: function {length} lines (>80)")
                    fn_start = None
                    fn_exempt = False
                seg.clear()
        elif ch == ";" and depth == 0:
            seg.clear()
        elif depth == 0:
            seg.append(ch)


def is_c_or_dm_header(delim: str, target: str) -> bool:
    if delim == '"':
        return True
    return target == "dm.h" or target.startswith(("dm/", "dmc"))


def check_objc_undef(rel: str, no_comments: str, errs: list) -> None:
    """Every Mac .m file that includes any C/dm/dmc header must close them with plat/af_objc_undef.h."""
    saw_c_hdr = False
    saw_undef = False
    for idx, ln in enumerate(no_comments.splitlines(), 1):
        m = INCLUDE_RE.match(ln)
        if not m:
            continue
        kind, delim, target = m.group(1), m.group(2), m.group(3)
        if target.endswith("af_objc_undef.h"):
            saw_undef = True
            continue
        if is_c_or_dm_header(delim, target):
            if kind == "import":
                errs.append(
                    f"[T-AF-BLD-02] {rel}:{idx}: use #include (not #import) for C/DM header '{target}'"
                )
            if saw_undef:
                errs.append(
                    f"[T-AF-BLD-02] {rel}:{idx}: C/DM header '{target}' included after af_objc_undef.h"
                )
                return
            saw_c_hdr = True
        elif kind == "import":
            if saw_c_hdr and not saw_undef:
                errs.append(
                    f"[T-AF-BLD-02] {rel}:{idx}: #import <{target}> before #include \"plat/af_objc_undef.h\""
                )
                return
    if saw_c_hdr and not saw_undef:
        errs.append(f"[T-AF-BLD-02] {rel}: missing #include \"plat/af_objc_undef.h\" after C/DM headers")


def check_deps_for_file(root: Path, rel_path: Path, no_comments: str, errs: list) -> None:
    rel = rel_path.as_posix()
    parts = rel_path.parts
    if not parts:
        return
    top = parts[0]

    for m in INCLUDE_RE.finditer(no_comments):
        _, delim, raw_inc = m.group(1), m.group(2), m.group(3)
        inc = norm_inc(rel_path, raw_inc)
        inc_top = inc.split("/", 1)[0]

        if top == "src":
            sub = parts[1] if len(parts) > 2 else ""
            is_main = rel == "src/main.c"
            is_cli_or_mig = rel in ("src/af_cli.c", "src/af_migrate.c", "src/af_cli.h", "src/af_migrate.h")
            is_foundation = sub == "plat" or (len(parts) == 2 and not is_main and not is_cli_or_mig)

            if is_foundation:
                if inc_top in NON_FOUNDATION_MODS or inc_top == "shell":
                    errs.append(f"[T-AF-BLD-01] {rel}: foundation cannot include '{raw_inc}'")
            elif sub in SERVICE_MODS:
                if inc_top in BIZ_MODS or inc_top == "shell":
                    errs.append(f"[T-AF-BLD-01] {rel}: service/{sub} cannot include '{raw_inc}'")
                elif inc_top in SERVICE_MODS and inc_top != sub and inc != "auth/af_auth.h":
                    errs.append(
                        f"[T-AF-BLD-01] {rel}: cross-service include forbidden ('{raw_inc}'; only auth/af_auth.h allowed)"
                    )
            elif sub in BIZ_MODS:
                if inc_top == "shell":
                    errs.append(f"[T-AF-BLD-01] {rel}: business/{sub} cannot include shell ('{raw_inc}')")
                elif inc_top in BIZ_MODS and inc_top != sub:
                    errs.append(f"[T-AF-BLD-01] {rel}: business modules cannot include each other ('{raw_inc}')")

        elif top == "shell":
            if delim == '"':
                if inc_top in NON_FOUNDATION_MODS:
                    errs.append(f"[T-AF-BLD-01] {rel}: shell cannot include business/service header '{raw_inc}'")
                elif inc in SHELL_ALLOWED_LOCAL:
                    continue
                elif "/" not in raw_inc and ".." not in raw_inc and (root / rel_path.parent / raw_inc).is_file():
                    continue
                else:
                    errs.append(f"[T-AF-BLD-01] {rel}: shell header '{raw_inc}' not in whitelist")
            else:
                if inc_top in NON_FOUNDATION_MODS or raw_inc == "dm.h":
                    errs.append(f"[T-AF-BLD-01] {rel}: shell forbidden system header '<{raw_inc}>'")
                elif raw_inc.startswith(("dm/", "dmc")) and raw_inc not in SHELL_ALLOWED_SYS:
                    errs.append(f"[T-AF-BLD-01] {rel}: shell DM header '<{raw_inc}>' not in whitelist")


def check_repo(root: Path, check_vendor_ui: bool = True) -> list:
    errs = []

    code_roots = [
        root / "src",
        root / "shell",
        root / "ipad" / "core",
        root / "ipad" / "ios",
    ]
    for base in code_roots:
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if not p.is_file() or p.suffix not in (".c", ".h", ".m"):
                continue
            rel_path = p.relative_to(root)
            rel = rel_path.as_posix()
            raw_bytes = p.read_bytes()
            if raw_bytes.startswith(b"\xef\xbb\xbf"):
                errs.append(f"[T-AF-BLD-04] {rel}: UTF-8 BOM forbidden")
            if b"\r\n" in raw_bytes:
                errs.append(f"[T-AF-BLD-04] {rel}: CRLF line ending forbidden (use LF)")
            text = raw_bytes.decode("utf-8", errors="replace")
            no_comments, code_only = lex_c(text)
            line_cnt = len(text.splitlines())

            # Doxygen banner check
            head = "\n".join(text.splitlines()[:20])
            if "@file" not in head or "@brief" not in head:
                errs.append(f"[T-AF-BLD-04] {rel}: missing Doxygen @file/@brief header banner")

            # File length <= 800
            if line_cnt > 800 and "AF_ALLOW_LONG_FILE" not in text:
                errs.append(f"[T-AF-BLD-04] {rel}: file {line_cnt} lines (>800)")

            if p.suffix in (".c", ".m"):
                check_func_lengths(rel, text, code_only, errs)

            # <uv.h> / <uv/...> zero leak
            if UV_INC_RE.search(no_comments):
                errs.append(f"[T-AF-BLD-02] {rel}: <uv.h> is forbidden outside libdm internals")

            # src/ isolation
            if rel.startswith("src/"):
                in_plat = rel.startswith("src/plat/")
                in_priv = rel.startswith("src/plat/private/")
                if p.suffix == ".m" and not in_plat:
                    errs.append(f"[T-AF-BLD-02] {rel}: .m file forbidden outside src/plat/ and shell/")
                if not in_plat and APPLE_FW_RE.search(no_comments):
                    errs.append(f"[T-AF-BLD-02] {rel}: Apple framework import forbidden outside src/plat/")
                if not in_priv and PRIVATE_API_RE.search(no_comments):
                    errs.append(f"[T-AF-BLD-02] {rel}: private Apple API forbidden outside src/plat/private/")

            # shell/ & ipad/ private API isolation
            if rel.startswith(("shell/", "ipad/")) and PRIVATE_API_RE.search(no_comments):
                errs.append(f"[T-AF-BLD-02] {rel}: private Apple API forbidden in {rel.split('/', 1)[0]}/")

            # ipad/core/ pure C, zero libdm, zero Apple framework isolation
            if rel.startswith("ipad/core/"):
                if p.suffix == ".m":
                    errs.append(f"[T-AF-BLD-02] {rel}: .m file forbidden in ipad/core/")
                if DM_INC_RE.search(no_comments):
                    errs.append(f"[T-AF-BLD-02] {rel}: libdm header forbidden in ipad/core/")
                if APPLE_FW_RE.search(no_comments):
                    errs.append(f"[T-AF-BLD-02] {rel}: Apple framework forbidden in ipad/core/")

            # ObjC af_objc_undef.h enforcement on Mac .m files
            if p.suffix == ".m" and (rel.startswith("src/plat/") or rel.startswith("shell/")):
                check_objc_undef(rel, no_comments, errs)

            # Layer dependency check
            if rel.startswith(("src/", "shell/")):
                check_deps_for_file(root, rel_path, no_comments, errs)

    # 2. <uv.h> zero leak in libdm public headers
    dm_inc = root / "public" / "dm" / "libdm" / "include"
    if dm_inc.is_dir():
        for p in sorted(dm_inc.rglob("*.h")):
            nc, _ = lex_c(p.read_text(encoding="utf-8", errors="replace"))
            if UV_INC_RE.search(nc):
                errs.append(f"[T-AF-BLD-02] {p.relative_to(root).as_posix()}: <uv.h> leaked in public dm header")

    # 3. LF & no-BOM check on tool/, test/, Makefile, build.py
    for extra in [root / "Makefile", root / "build.py"]:
        if extra.is_file():
            b = extra.read_bytes()
            if b.startswith(b"\xef\xbb\xbf"):
                errs.append(f"[T-AF-BLD-04] {extra.name}: UTF-8 BOM forbidden")
            if b"\r\n" in b:
                errs.append(f"[T-AF-BLD-04] {extra.name}: CRLF line ending forbidden")
    for sub in ("tool", "test"):
        d = root / sub
        if d.is_dir():
            for p in sorted(d.rglob("*")):
                if p.is_file() and p.suffix in (".c", ".h", ".m", ".py", ".mk"):
                    b = p.read_bytes()
                    if b.startswith(b"\xef\xbb\xbf"):
                        errs.append(f"[T-AF-BLD-04] {p.relative_to(root).as_posix()}: UTF-8 BOM forbidden")
                    if b"\r\n" in b:
                        errs.append(f"[T-AF-BLD-04] {p.relative_to(root).as_posix()}: CRLF forbidden")

    if not check_vendor_ui:
        return errs

    # 4. Self-contained VENDOR.txt SHA-256 check (T-AF-BLD-03 L0)
    vendor = root / "public" / "dm" / "VENDOR.txt"
    if not vendor.is_file():
        errs.append("[T-AF-BLD-03] missing public/dm/VENDOR.txt")
    else:
        seen = set()
        for ln in vendor.read_text(encoding="utf-8").splitlines():
            m = re.match(r"^sha256\s+(\S+)\s+([0-9a-f]{64})$", ln.strip())
            if not m:
                continue
            rel_f, exp_sha = m.group(1), m.group(2)
            seen.add(rel_f)
            fp = root / rel_f
            if not fp.is_file():
                errs.append(f"[T-AF-BLD-03] VENDOR.txt locked file missing: {rel_f}")
            elif sha256_file(fp) != exp_sha:
                errs.append(f"[T-AF-BLD-03] {rel_f} SHA-256 mismatch (zero-intrusion violated!)")
        missing_req = REQUIRED_VENDOR_FILES - seen
        if missing_req:
            errs.append(f"[T-AF-BLD-03] VENDOR.txt missing required locked files: {sorted(missing_req)}")

    # 5. Frozen iPad UI SHA-256 + setup.html contract (T-AF-BLD-04)
    if (root / "ipad" / "ui").is_dir():
        for rel_f, exp_sha in FROZEN_IPAD_UI.items():
            fp = root / rel_f
            if not fp.is_file():
                errs.append(f"[T-AF-BLD-04] missing frozen UI file: {rel_f}")
            elif sha256_file(fp) != exp_sha:
                errs.append(f"[T-AF-BLD-04] frozen UI {rel_f} SHA-256 mismatch!")

    setup_html = root / "shell" / "ui" / "setup.html"
    if not setup_html.is_file():
        errs.append("[T-AF-BLD-04] missing shell/ui/setup.html")
    else:
        stxt = setup_html.read_text(encoding="utf-8")
        if "V.helper" in stxt or "重启助手" in stxt:
            errs.append("[T-AF-BLD-04] shell/ui/setup.html still contains removed helper references")
        if "下面 3 项" not in stxt:
            errs.append("[T-AF-BLD-04] shell/ui/setup.html missing '下面 3 项' wording update")
        if sha256_file(setup_html) != FROZEN_SETUP_HTML_SHA256:
            errs.append("[T-AF-BLD-04] shell/ui/setup.html SHA-256 mismatch (unexpected delta outside 5 locked lines)")

    return errs


def run_self_tests(repo_root: Path) -> list:
    """Positive & negative fixture tests for T-AF-BLD-01, 02, 03(L0), 04."""
    failures = []
    banner = "/** @file t.c\n *  @brief fixture\n */\n"

    def write(td: Path, rel: str, content: bytes) -> None:
        p = td / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(content)

    # Case 1: T-AF-BLD-01 negative (auth -> session, shell #include/#import/angle bypasses)
    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        write(td, "src/auth/bad.c", (banner + '#include "../session/af_session.h"\n').encode())
        write(td, "shell/bad1.m", (banner + '#import "../src/session/af_session.h"\n#include "plat/af_objc_undef.h"\n').encode())
        write(td, "shell/bad2.m", (banner + '#include <session/af_session.h>\n#include <dm.h>\n#include "plat/af_objc_undef.h"\n').encode())
        write(td, "shell/bad3.m", (banner + '#include "../Makefile"\n#include "plat/af_objc_undef.h"\n').encode())
        e = check_repo(td, check_vendor_ui=False)
        for fname in ("src/auth/bad.c", "shell/bad1.m", "shell/bad2.m", "shell/bad3.m"):
            if not any(fname in x and "T-AF-BLD-01" in x for x in e):
                failures.append(f"self-test T-AF-BLD-01 missed violation in {fname}: {e}")

    # Case 2: T-AF-BLD-01 positive (shell whitelist + sibling header independent of CWD)
    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        write(td, "shell/af_shell_int.h", (banner + "void af_sh_int(void);\n").encode())
        write(
            td,
            "shell/good.m",
            (
                banner
                + '#include <dm/link.h>\n#include <dm/module.h>\n#include <dmc_client.h>\n'
                + '#include "af_modules.h"\n#include "af_ui_ipc.h"\n#include "af_events.h"\n'
                + '#include "af_store.h"\n#include "plat/af_plat.h"\n#include "af_shell_int.h"\n'
                + '#include "plat/af_objc_undef.h"\n'
                + '#import <Foundation/Foundation.h>\n'
            ).encode(),
        )
        # Positive private API comment check (DD-01 af_plat.h must NOT false-positive!)
        write(
            td,
            "src/plat/af_plat.h",
            (banner + "int af_plat_sc_check_signatures(void); /* debug 启动校验 SidecarCore 方法签名 */\n").encode(),
        )
        e = check_repo(td, check_vendor_ui=False)
        if e:
            failures.append(f"self-test T-AF-BLD-01/02 false positive on valid files: {e}")

    # Case 3: T-AF-BLD-02 negative (Foundation/@import, <uv.h>/<uv/...>, private API, missing undef on <dmc_client.h>, ipad/core)
    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        write(td, "src/auth/fw.c", (banner + "@import Foundation;\n#include <uv/unix.h>\n").encode())
        write(td, "shell/no_undef.m", (banner + "#include <dmc_client.h>\n#import <Foundation/Foundation.h>\n").encode())
        write(td, "src/plat/priv.m", (banner + '#include "plat/af_plat.h"\n#include "plat/af_objc_undef.h"\n#import <Foundation/Foundation.h>\nvoid f(void){ (void)NSClassFromString(@"SidecarDisplayManager"); }\n').encode())
        write(td, "ipad/core/bad.m", (banner + "#include <dm/module.h>\n#import <Foundation/Foundation.h>\nvoid f(void){ CGSMainConnectionID(); }\n").encode())
        e = check_repo(td, check_vendor_ui=False)
        checks = [
            ("src/auth/fw.c: Apple framework", "Apple framework/@import in src/auth/fw.c"),
            ("src/auth/fw.c: <uv.h>", "<uv/unix.h>"),
            ("af_objc_undef.h", "missing af_objc_undef.h on <dmc_client.h>"),
            ("src/plat/priv.m", "private API in string literal"),
            ("ipad/core/bad.m: .m file", "ipad/core .m"),
            ("libdm header forbidden", "ipad/core libdm"),
            ("private Apple API forbidden in ipad/", "ipad/ private API"),
        ]
        for needle, label in checks:
            if not any(needle in x for x in e):
                failures.append(f"self-test T-AF-BLD-02 missed {label}: {e}")

    # Case 4: T-AF-BLD-02 real Apple Clang ObjC compile check with dm/type.h + af_objc_undef.h + for (id x in arr)
    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        (td / "plat").mkdir()
        (td / "plat" / "af_objc_undef.h").write_text(
            "#ifdef __OBJC__\n#undef in\n#undef out\n#undef io\n#endif\n", encoding="utf-8"
        )
        (td / "objc_test.m").write_text(
            '#include <dm/type.h>\n'
            '#include "plat/af_objc_undef.h"\n'
            '#import <Foundation/Foundation.h>\n'
            'void af_check_objc(NSArray *arr) { for (id x in arr) { (void)x; } }\n',
            encoding="utf-8",
        )
        r = subprocess.run(
            [
                "clang", "-x", "objective-c", "-fobjc-arc",
                "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                "-I", str(td),
                "-I", str(repo_root / "public" / "dm" / "libdm" / "include"),
                str(td / "objc_test.m"),
            ],
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            failures.append(f"self-test T-AF-BLD-02 clang ObjC macro test failed: {r.stderr}")

    # Case 5: T-AF-BLD-04 function length precision (struct param long func caught, 90-line array NOT false-positive, per-func exemption)
    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        body85 = "".join(f"  x += {i};\n" for i in range(85))
        arr90 = "".join(f'  "item{i}",\n' for i in range(90))
        write(
            td,
            "src/auth/len.c",
            (
                banner
                + f"static const char *g_tbl[] = {{\n{arr90}}};\n"
                + f"/* AF_ALLOW_LONG_FUNC */\nint ok_exempt(void) {{\n  int x = 0;\n  const char *u = \"https://a.b\"; (void)u;\n{body85}  return x;\n}}\n"
                + f"int bad_struct_fn(const struct foo *p) {{\n  (void)p; int x = 0;\n{body85}  return x;\n}}\n"
            ).encode(),
        )
        write(
            td,
            "src/plat/objc_len.m",
            (
                banner
                + "#import <Foundation/Foundation.h>\n"
                + "@implementation AfHelper\n"
                + f"- (int)firstLongMethod:(int)a {{\n  int x = a;\n{body85}  return x;\n}}\n"
                + "@end\n"
            ).encode(),
        )
        e = check_repo(td, check_vendor_ui=False)
        func_errs = [x for x in e if "function" in x and "(>80)" in x]
        if len(func_errs) != 2 or not any("len.c" in x for x in func_errs) or not any("objc_len.m" in x for x in func_errs):
            failures.append(f"self-test T-AF-BLD-04 func length expected 2 errors (bad_struct_fn + firstLongMethod), got: {func_errs}")

    # Case 6: T-AF-BLD-03(L0) & T-AF-BLD-04 negative (BOM, CRLF, missing banner, tampered VENDOR & UI)
    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        write(td, "src/auth/crlf.c", b"\xef\xbb\xbfint f(void) { return 0; }\r\n")
        write(td, "public/dm/libdm/src/transport/conn.c", b"tampered\n")
        write(td, "public/dm/libdm/src/transport/http.c", b"ok\n")
        write(td, "public/dm/libdm/src/transport/ws.c", b"ok\n")
        write(
            td,
            "public/dm/VENDOR.txt",
            (
                "sha256 public/dm/libdm/src/transport/conn.c " + ("0" * 64) + "\n"
                "sha256 public/dm/libdm/src/transport/http.c " + ("0" * 64) + "\n"
                "sha256 public/dm/libdm/src/transport/ws.c " + ("0" * 64) + "\n"
            ).encode(),
        )
        write(td, "ipad/ui/index.html", b"tampered")
        write(td, "ipad/ui/i18n.js", b"tampered")
        write(td, "ipad/ui/app.js", b"tampered")
        write(td, "ipad/ui/style.css", b"tampered")
        write(td, "shell/ui/setup.html", "下面 3 项 tampered".encode("utf-8"))
        e = check_repo(td, check_vendor_ui=True)
        for needle in ("BOM", "CRLF", "Doxygen", "T-AF-BLD-03", "frozen UI ipad/ui/app.js", "setup.html SHA-256 mismatch"):
            if not any(needle in x for x in e):
                failures.append(f"self-test T-AF-BLD-03/04 missed '{needle}': {e}")

    return failures


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    do_self = "--self-test" in sys.argv[1:]

    errs = check_repo(root, check_vendor_ui=True)
    if do_self:
        errs.extend(run_self_tests(root))

    if errs:
        for e in errs:
            print(f"ERROR: {e}", file=sys.stderr)
        print(f"check_bld: FAILED ({len(errs)} error(s))", file=sys.stderr)
        return 1

    tag = " (repo + self-test fixtures)" if do_self else " (repo)"
    print(f"check_bld: PASSED{tag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
