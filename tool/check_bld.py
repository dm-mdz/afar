#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Afar_C L0 static gate (T-AF-BLD-01 ~ T-AF-BLD-04 & T-V6-L0-01 ~ T-V6-L0-07).

Enforces:
  - Coding standard: UTF-8 no-BOM, LF, Doxygen @file/@brief, file <=800 lines, func <=80 lines.
  - 3-layer unidirectional dependency (incl. src/base foundation, stream/input services) & shell/ header whitelist.
  - Apple framework (incl. ScreenCaptureKit/VideoToolbox/CoreMedia/CoreVideo/Carbon), private API,
    <uv.h>, ipad/core zero-libdm, and ObjC af_objc_undef.h isolation.
  - DM v2.1.0 coding rules:
      * T-RULE-04: no raw malloc/calloc/realloc/free in src/**/*.{c,m} (zero exemptions).
      * T-RULE-08: mandatory dm_in/dm_out/dm_io & zero bare in/out/io in src/ and public/dm/
                   (explicitly excluding zero-DM MIT public/proto/af_direct_proto/).
      * T-RULE-09: control-flow nesting depth <= 4 in src/**/*.{c,m} and public/proto/af_direct_proto/*.c.
      * T-RULE-10: mandatory {} braces on if/else/for/while in src/**/*.{c,m} and public/proto/af_direct_proto/*.c.
      * T-RULE-11: public header function declarations <= 5 params in src/**/*.h and
                   public/proto/af_direct_proto/*.h (exact 7-function frozen legacy whitelist in src/).
  - MIT public/proto/af_direct_proto/ purity gate (LICENSE, SPDX MIT, C11/self headers only, no 'ipad' filename).
  - ipad/ios/*.swift basic hygiene (UTF-8 no-BOM, LF, <=800 lines; OSS-safe when ipad/ absent).
  - Self-contained public/dm/VENDOR.txt SHA-256 lock on all 8 upstream files (T-AF-BLD-03 L0).
  - Frozen ipad/ui/ SHA-256 lock & setup.html contract (T-AF-BLD-04).
"""
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SERVICE_MODS = {"auth", "env", "display", "sidecar", "power", "kbd", "stream", "input"}
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
    "LocalAuthentication|UserNotifications|ServiceManagement|CoreServices|ApplicationServices|"
    "ScreenCaptureKit|VideoToolbox|CoreMedia|CoreVideo|Carbon"
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

RAW_HEAP_RE = re.compile(r"(?<!\.)(?<!->)\b(malloc|calloc|realloc|free)\s*\(")
BARE_DIR_RE = re.compile(r"(?:[(,]\s*)\b(in|out|io)\b\s+[A-Za-z_]\w*", re.S)
HEADER_DECL_RE = re.compile(
    r"^(?!\s*typedef\b)\s*(?:DM_API\s+)?[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*"
    r"\(([^;{}]*?)\)\s*(?:(?:DM_PRINTF_FMT|__attribute__)\s*\([^;{}]*\)\s*)*;",
    re.M,
)
DM_DIR_RE = re.compile(r"^(?:dm_in|dm_out|dm_io)\b")

_EXTERN_C_OPEN_RE = re.compile(
    r'#\s*ifdef\s+__cplusplus\s+extern\s+"[^"]*"\s*\{\s*#\s*endif', re.S
)
_EXTERN_C_CLOSE_RE = re.compile(
    r"#\s*ifdef\s+__cplusplus\s*\}\s*#\s*endif", re.S
)
_TOK_RE = re.compile(r"@[A-Za-z_]\w*|@|[A-Za-z_]\w*|[{}()\[\];=,:?^+-]")
_CTRL_PARENS_KW = frozenset(("if", "for", "while", "switch"))
_NON_FN_KW = frozenset((
    "if", "for", "while", "switch", "else", "do",
    "struct", "union", "enum", "typedef", "return",
    "NS_ENUM", "NS_OPTIONS", "CF_ENUM", "CF_OPTIONS",
))

MAX_NESTING_DEPTH = 4
MAX_FUNC_PARAMS = 5

# Frozen v0.5.1 legacy multi-param functions in src/ (DD v2.1 §8 — strictly frozen at 7, never add!)
LEGACY_MULTI_PARAM_FUNCS = frozenset({
    "af_plat_vd_create",
    "af_plat_run_cmd_cancellable",
    "af_vd_build_modes",
    "af_vd_eval_state",
    "af_phys_guard_recover",
    "af_disp_follow_eval",
    "af_disp_scale_fallback_exec",
})

# ISO/IEC 9899:2011 (C11) standard library headers allowed in MIT public/proto/af_direct_proto/
C11_STD_HEADERS = frozenset({
    "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h",
    "inttypes.h", "iso646.h", "limits.h", "locale.h", "math.h", "setjmp.h",
    "signal.h", "stdalign.h", "stdarg.h", "stdatomic.h", "stdbool.h",
    "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "stdnoreturn.h",
    "string.h", "tgmath.h", "threads.h", "time.h", "uchar.h", "wchar.h",
    "wctype.h",
})

REQUIRED_VENDOR_FILES = {
    "public/dm/libdm/src/transport/conn.c",
    "public/dm/libdm/src/transport/http.c",
    "public/dm/libdm/src/transport/ws.c",
    "public/dm/libdm/include/dm/thread.h",
    "public/dm/libdm/include/dm/loop.h",
    "public/dm/libdm/include/dm/sock.h",
    "public/dm/libdm/include/dm/crypto.h",
    "public/dm/libdm/include/dm/version.h",
}

FROZEN_IPAD_UI = {
    "ipad/ui/index.html": "c9b962ef74c5c7e9a3b2a2256648b1514b1abe5060de505bcce4438af0ec97ca",
    "ipad/ui/i18n.js": "b40f73ee7ab8069a5ae1d39bdc88784ff259f43e464a6999a4151d9ab7169117",
    "ipad/ui/app.js": "294fd4c835e0456e5c782f01b6dbe8e0ca604d8ed358f007369944044e9167ee",
    "ipad/ui/style.css": "da308d6836ea33d5a100a7959a8139752e33787eff20fc3891938a65720b926a",
}
FROZEN_SETUP_HTML_SHA256 = "8dbee47ddd234cfa0bf13e1765d226e67a02d3c14ed93408bfc8147f1097a579"


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


def strip_extern_c(clean_text: str) -> str:
    """Strip #ifdef __cplusplus extern "C" { ... } #endif preserving line numbers."""
    clean_text = _EXTERN_C_OPEN_RE.sub(lambda m: "\n" * m.group(0).count("\n"), clean_text)
    clean_text = _EXTERN_C_CLOSE_RE.sub(lambda m: "\n" * m.group(0).count("\n"), clean_text)
    return clean_text


def strip_preprocessor(clean_text: str) -> str:
    """Strip preprocessor directive lines (including backslash continuations) preserving line numbers."""
    out = []
    in_pp = False
    for line in clean_text.split("\n"):
        if in_pp:
            out.append("")
            in_pp = line.rstrip().endswith("\\")
        elif line.lstrip().startswith("#"):
            out.append("")
            in_pp = line.rstrip().endswith("\\")
        else:
            out.append(line)
    return "\n".join(out)


def mask_brace_bodies(clean_text: str) -> str:
    """Replace contents inside top-level { ... } with spaces while preserving line numbers."""
    top_chars = []
    bdepth = 0
    for ch in clean_text:
        if ch == "{":
            bdepth += 1
            top_chars.append(" ")
        elif ch == "}":
            bdepth = max(0, bdepth - 1)
            top_chars.append(" ")
        elif bdepth == 0:
            top_chars.append(ch)
        elif ch == "\n":
            top_chars.append("\n")
        else:
            top_chars.append(" ")
    return "".join(top_chars)


def split_params(params: str) -> list:
    """Split a C parameter list by top-level commas (respecting nested function-pointer parens)."""
    parts = []
    depth = 0
    cur = []
    for ch in params:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur).strip())
    return [pt for pt in parts if pt]


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
    s = "\n".join(ln for ln in s.splitlines() if not ln.lstrip().startswith("#")).strip()
    if not s:
        return False
    if s.endswith("="):
        return False
    if re.search(r'\bextern\s+"[^"]*"\s*$', s):
        return False
    if re.search(r"(?:^|\n)\s*[+-]\s*\(", s):
        return True
    if s.endswith(")"):
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


def _tokenize_c(clean_text: str) -> list:
    tokens = []
    for line_no, line in enumerate(clean_text.split("\n"), 1):
        for m in _TOK_RE.finditer(line):
            tokens.append((m.group(0), line_no))
    return tokens


def _match_open_paren_back(tokens: list, close_idx: int) -> int:
    depth = 0
    for j in range(close_idx, -1, -1):
        t = tokens[j][0]
        if t == ")":
            depth += 1
        elif t == "(":
            depth -= 1
            if depth == 0:
                return j
    return -1


def _match_close_paren_fwd(tokens: list, open_idx: int) -> int:
    depth = 0
    for j in range(open_idx, len(tokens)):
        t = tokens[j][0]
        if t == "(":
            depth += 1
        elif t == ")":
            depth -= 1
            if depth == 0:
                return j
    return -1


def _is_objc_method_open(tokens: list, brace_idx: int) -> tuple:
    """Detect top-level Objective-C method body '{' (preceded by '- (ret)sel...' or '+ (ret)sel...')."""
    j = brace_idx - 1
    while j >= 0:
        t = tokens[j][0]
        if t in (";", "}", "@end"):
            break
        if t in ("@interface", "@protocol"):
            return False, ""
        if t == "@implementation":
            # Skip '@implementation ClassName' (and optional '(Category)')
            k = j + 2
            if k < brace_idx and tokens[k][0] == "(":
                cp = _match_close_paren_fwd(tokens, k)
                if cp != -1:
                    k = cp + 1
            if k + 1 < brace_idx and tokens[k][0] in ("-", "+") and tokens[k + 1][0] == "(":
                cp = _match_close_paren_fwd(tokens, k + 1)
                sel = tokens[cp + 1][0] if (cp != -1 and cp + 1 < brace_idx) else "objc_method"
                return True, sel
            return False, ""
        j -= 1
    start = j + 1
    if start + 1 < brace_idx and tokens[start][0] in ("-", "+") and tokens[start + 1][0] == "(":
        cp = _match_close_paren_fwd(tokens, start + 1)
        sel = tokens[cp + 1][0] if (cp != -1 and cp + 1 < brace_idx) else "objc_method"
        return True, sel
    return False, ""


def _classify_open_brace(tokens: list, i: int, brace_stack: list, is_objc: bool) -> tuple:
    """Classify '{' at tokens[i] as 'fn' / 'ctrl' / 'ctrl_do' / 'init' / 'bare'."""
    if brace_stack and brace_stack[-1] == "init":
        return "init", ""
    prev = tokens[i - 1][0] if i > 0 else ""
    if not brace_stack:
        if prev == ")":
            open_p = _match_open_paren_back(tokens, i - 1)
            if open_p > 0:
                fn_tok = tokens[open_p - 1][0]
                if fn_tok not in _NON_FN_KW and re.match(r"^[A-Za-z_]\w*$", fn_tok):
                    return "fn", fn_tok
        if is_objc:
            ok, sel = _is_objc_method_open(tokens, i)
            if ok:
                return "fn", sel
        return "init", ""
    if prev == "do":
        return "ctrl_do", ""
    if prev == "else":
        return "ctrl", ""
    if prev == ")":
        open_p = _match_open_paren_back(tokens, i - 1)
        if open_p > 0 and tokens[open_p - 1][0] in _CTRL_PARENS_KW:
            return "ctrl", ""
        if is_objc and open_p > 0:
            head = tokens[open_p - 1][0]
            if head in ("@synchronized", "@catch"):
                return "ctrl", ""
            # ObjC block literal: ^(args){...} or ^RetType(args){...}
            for back in range(1, min(5, open_p + 1)):
                bt = tokens[open_p - back][0]
                if bt == "^":
                    return "bare", ""
                if bt in (";", "{", "}", "=", ",", "(", "[", ":"):
                    break
        return "init", ""
    if is_objc:
        if prev == "@":
            return "init", ""
        if prev == "^" or prev in ("@autoreleasepool", "@try", "@finally"):
            return "bare", ""
    if prev in ("=", ",", "(", "[", "?"):
        return "init", ""
    if prev in ("struct", "union", "enum"):
        return "init", ""
    if i >= 2 and tokens[i - 2][0] in ("struct", "union", "enum"):
        return "init", ""
    return "bare", ""


def check_nesting_and_braces(rel: str, code_only: str, is_objc: bool, errs: list) -> None:
    """Enforce T-RULE-09 (control-flow nesting depth <= 4) and T-RULE-10 (mandatory {} on if/else/for/while)."""
    clean = strip_preprocessor(strip_extern_c(code_only))
    tokens = _tokenize_c(clean)
    brace_stack = []
    fn_name = ""
    max_ctrl = 0
    max_line = 0
    ctrl_depth = 0
    last_closed = None
    n = len(tokens)

    for i, (tok, line_no) in enumerate(tokens):
        if tok == "{":
            kind, name = _classify_open_brace(tokens, i, brace_stack, is_objc)
            brace_stack.append(kind)
            if kind == "fn":
                fn_name = name
                max_ctrl = 0
                max_line = line_no
                ctrl_depth = 0
            elif kind in ("ctrl", "ctrl_do"):
                ctrl_depth += 1
                if ctrl_depth > max_ctrl:
                    max_ctrl = ctrl_depth
                    max_line = line_no
            last_closed = None
            continue
        if tok == "}":
            if brace_stack:
                popped = brace_stack.pop()
                last_closed = popped
                if popped in ("ctrl", "ctrl_do"):
                    ctrl_depth = max(0, ctrl_depth - 1)
                elif popped == "fn":
                    if max_ctrl > MAX_NESTING_DEPTH:
                        errs.append(
                            f"[T-RULE-09] {rel}:{max_line}: control-flow nesting depth "
                            f"{max_ctrl} (limit {MAX_NESTING_DEPTH}) in {fn_name}()"
                        )
                    fn_name = ""
                    max_ctrl = 0
                    ctrl_depth = 0
            else:
                last_closed = None
            continue

        in_fn = bool(brace_stack) and brace_stack[0] == "fn" and brace_stack[-1] != "init"
        if not in_fn:
            last_closed = None
            continue

        if tok in ("if", "for", "while"):
            if i + 1 < n and tokens[i + 1][0] == "(":
                close_p = _match_close_paren_fwd(tokens, i + 1)
                if close_p != -1 and close_p + 1 < n:
                    nxt = tokens[close_p + 1][0]
                    if tok == "while" and last_closed == "ctrl_do" and nxt == ";":
                        last_closed = None
                        continue
                    if nxt != "{":
                        errs.append(
                            f"[T-RULE-10] {rel}:{line_no}: braceless '{tok}' statement (must wrap body in {{}})"
                        )
        elif tok == "else":
            if i + 1 < n and tokens[i + 1][0] not in ("{", "if"):
                errs.append(
                    f"[T-RULE-10] {rel}:{line_no}: braceless 'else' statement (must wrap body in {{}})"
                )
        last_closed = None


def check_raw_heap(rel: str, code_only: str, errs: list) -> None:
    """Enforce T-RULE-04: forbid raw malloc/calloc/realloc/free in src/**/*.{c,m} (zero exemptions)."""
    clean = strip_preprocessor(code_only)
    for line_no, ln in enumerate(clean.splitlines(), 1):
        m = RAW_HEAP_RE.search(ln)
        if m:
            errs.append(f"[T-RULE-04] {rel}:{line_no}: raw heap call '{m.group(1)}()' forbidden in src/")


def check_rule_08_bare_dir(rel: str, code_only: str, errs: list) -> None:
    """Enforce T-RULE-08 bare in/out/io prohibition in parameter qualifier position."""
    clean = strip_preprocessor(strip_extern_c(code_only))
    for m in BARE_DIR_RE.finditer(clean):
        line_no = clean.count("\n", 0, m.start()) + 1
        errs.append(
            f"[T-RULE-08] {rel}:{line_no}: bare '{m.group(1)}' parameter qualifier forbidden "
            f"(use dm_{m.group(1)})"
        )


def check_rule_08_header_dirs(rel: str, code_only: str, errs: list) -> None:
    """Enforce T-RULE-08 mandatory dm_in/dm_out/dm_io on public header function declarations."""
    txt = mask_brace_bodies(strip_preprocessor(strip_extern_c(code_only)))
    for m in HEADER_DECL_RE.finditer(txt):
        sig = m.group(0)
        if re.match(r"^\s*extern\b[^;()]*\(\s*\*", sig):
            continue
        name = m.group(1)
        params = m.group(2).strip()
        if name in ("typedef", "return", "if", "for", "while", "switch") or params in ("", "void"):
            continue
        parts = split_params(params)
        missing = [pt for pt in parts if pt != "..." and not DM_DIR_RE.match(pt)]
        if missing:
            line_no = txt.count("\n", 0, m.start()) + 1
            errs.append(
                f"[T-RULE-08] {rel}:{line_no}: {name}() missing dm_in/dm_out/dm_io on {missing[0]!r}"
            )


def check_rule_11_header_params(rel: str, code_only: str, allow_legacy: bool, errs: list) -> None:
    """Enforce T-RULE-11: header function declarations <= 5 params (7 frozen legacy functions in src/)."""
    txt = mask_brace_bodies(strip_preprocessor(strip_extern_c(code_only)))
    for m in HEADER_DECL_RE.finditer(txt):
        sig = m.group(0)
        if re.match(r"^\s*extern\b[^;()]*\(\s*\*", sig):
            continue
        name = m.group(1)
        params = m.group(2).strip()
        if name in ("typedef", "return", "if", "for", "while", "switch") or params in ("", "void"):
            continue
        parts = split_params(params)
        if len(parts) > MAX_FUNC_PARAMS:
            if allow_legacy and name in LEGACY_MULTI_PARAM_FUNCS:
                continue
            line_no = txt.count("\n", 0, m.start()) + 1
            errs.append(
                f"[T-RULE-11] {rel}:{line_no}: {name}() has {len(parts)} params "
                f"(limit {MAX_FUNC_PARAMS}; pack into struct)"
            )


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
            is_foundation = sub in ("plat", "base") or (len(parts) == 2 and not is_main and not is_cli_or_mig)

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


def check_direct_proto(root: Path, require_exists: bool, errs: list) -> None:
    """Enforce MIT purity, zero DM/OS deps, zero 'ipad' filenames, and T-RULE-09/10/11 on af_direct_proto."""
    proto_dir = root / "public" / "proto" / "af_direct_proto"
    if not proto_dir.is_dir():
        if require_exists:
            errs.append("[T-V6-L0-06] missing MIT protocol directory public/proto/af_direct_proto/")
        return

    lic = proto_dir / "LICENSE"
    if not lic.is_file():
        errs.append("[T-V6-L0-06] public/proto/af_direct_proto/LICENSE missing")
    else:
        ltxt = lic.read_text(encoding="utf-8", errors="replace")
        if "MIT License" not in ltxt or "Permission is hereby granted, free of charge" not in ltxt:
            errs.append("[T-V6-L0-06] public/proto/af_direct_proto/LICENSE missing MIT license text")

    proto_abs = os.path.realpath(str(proto_dir))
    for p in sorted(proto_dir.rglob("*")):
        rel = p.relative_to(root).as_posix()
        if "ipad" in p.name.lower():
            errs.append(f"[T-V6-L0-06] {rel}: forbidden 'ipad' substring in MIT proto filename")
        if not p.is_file():
            continue
        if p.suffix in (".m", ".mm", ".swift"):
            errs.append(f"[T-V6-L0-06] {rel}: non-C source file forbidden in MIT proto directory")
            continue
        if p.suffix not in (".c", ".h"):
            continue

        raw_bytes = p.read_bytes()
        if raw_bytes.startswith(b"\xef\xbb\xbf"):
            errs.append(f"[T-AF-BLD-04] {rel}: UTF-8 BOM forbidden")
        if b"\r\n" in raw_bytes or b"\r" in raw_bytes:
            errs.append(f"[T-AF-BLD-04] {rel}: CRLF/CR line ending forbidden (use LF)")
        text = raw_bytes.decode("utf-8", errors="replace")
        no_comments, code_only = lex_c(text)
        lines = text.splitlines()

        head = "\n".join(lines[:25])
        if "@file" not in head or "@brief" not in head:
            errs.append(f"[T-AF-BLD-04] {rel}: missing Doxygen @file/@brief header banner")
        if "SPDX-License-Identifier: MIT" not in head:
            errs.append(f"[T-V6-L0-06] {rel}: missing 'SPDX-License-Identifier: MIT' header")
        if len(lines) > 800 and "AF_ALLOW_LONG_FILE" not in text:
            errs.append(f"[T-AF-BLD-04] {rel}: file {len(lines)} lines (>800)")

        if APPLE_FW_RE.search(no_comments):
            errs.append(f"[T-V6-L0-06] {rel}: Apple framework forbidden in MIT proto")
        if PRIVATE_API_RE.search(no_comments):
            errs.append(f"[T-V6-L0-06] {rel}: private Apple API forbidden in MIT proto")

        for m in INCLUDE_RE.finditer(no_comments):
            kind, delim, target = m.group(1), m.group(2), m.group(3)
            if kind == "import":
                errs.append(f"[T-V6-L0-06] {rel}: ObjC #import forbidden in MIT proto ('{target}')")
                continue
            if delim == "<":
                if target not in C11_STD_HEADERS:
                    errs.append(
                        f"[T-V6-L0-06] {rel}: forbidden non-C11 header '<{target}>' in MIT proto"
                    )
            else:
                if target == "dm.h" or target.startswith(("dm/", "uv/", "uv.h")) or ".." in target:
                    errs.append(f"[T-V6-L0-06] {rel}: forbidden include '\"{target}\"' in MIT proto")
                    continue
                resolved = os.path.realpath(str(p.parent / target))
                if os.path.commonpath([proto_abs, resolved]) != proto_abs or not Path(resolved).is_file():
                    errs.append(
                        f"[T-V6-L0-06] {rel}: local include '\"{target}\"' escapes MIT proto directory"
                    )

        if p.suffix == ".c":
            check_func_lengths(rel, text, code_only, errs)
            check_nesting_and_braces(rel, code_only, False, errs)
        elif p.suffix == ".h":
            check_rule_11_header_params(rel, code_only, allow_legacy=False, errs=errs)


def check_swift_files(root: Path, errs: list) -> None:
    """Basic hygiene scan on ipad/ios/*.swift (UTF-8 no-BOM, LF, <=800 lines; OSS-safe)."""
    ios_dir = root / "ipad" / "ios"
    if not ios_dir.is_dir():
        return
    for p in sorted(ios_dir.rglob("*.swift")):
        if not p.is_file():
            continue
        rel = p.relative_to(root).as_posix()
        raw_bytes = p.read_bytes()
        if raw_bytes.startswith(b"\xef\xbb\xbf"):
            errs.append(f"[T-AF-BLD-04] {rel}: UTF-8 BOM forbidden")
        if b"\r\n" in raw_bytes or b"\r" in raw_bytes:
            errs.append(f"[T-AF-BLD-04] {rel}: CRLF/CR line ending forbidden (use LF)")
        try:
            text = raw_bytes.decode("utf-8")
        except UnicodeDecodeError:
            errs.append(f"[T-AF-BLD-04] {rel}: invalid UTF-8 encoding")
            continue
        line_cnt = len(text.splitlines())
        if line_cnt > 800:
            errs.append(f"[T-AF-BLD-04] {rel}: Swift file {line_cnt} lines (>800)")


def check_dm_rule_08(root: Path, errs: list) -> None:
    """Enforce T-RULE-08 on public/dm/ (explicitly excluding public/proto/af_direct_proto/)."""
    dm_root = root / "public" / "dm"
    if not dm_root.is_dir():
        return
    pub_inc_dirs = (
        dm_root / "libdm" / "include",
        dm_root / "plug" / "dmc" / "include",
    )
    for p in sorted(dm_root.rglob("*")):
        if not p.is_file() or p.suffix not in (".c", ".h"):
            continue
        rel = p.relative_to(root).as_posix()
        if rel.startswith("public/proto/af_direct_proto/"):
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        _, code_only = lex_c(text)
        check_rule_08_bare_dir(rel, code_only, errs)
        if p.suffix == ".h" and any(
            os.path.commonpath([str(d), str(p)]) == str(d) for d in pub_inc_dirs if d.is_dir()
        ):
            check_rule_08_header_dirs(rel, code_only, errs)


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
            if b"\r" in raw_bytes:
                errs.append(f"[T-AF-BLD-04] {rel}: CR/CRLF line ending forbidden (use LF)")
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

            # src/ isolation & DM v2.1.0 coding rules (T-RULE-04/08/09/10/11)
            if rel.startswith("src/"):
                in_plat = rel.startswith("src/plat/")
                in_priv = rel.startswith("src/plat/private/")
                if p.suffix == ".m" and not in_plat:
                    errs.append(f"[T-AF-BLD-02] {rel}: .m file forbidden outside src/plat/ and shell/")
                if not in_plat and APPLE_FW_RE.search(no_comments):
                    errs.append(f"[T-AF-BLD-02] {rel}: Apple framework import forbidden outside src/plat/")
                if not in_priv and PRIVATE_API_RE.search(no_comments):
                    errs.append(f"[T-AF-BLD-02] {rel}: private Apple API forbidden outside src/plat/private/")

                check_rule_08_bare_dir(rel, code_only, errs)
                check_raw_heap(rel, code_only, errs)
                if p.suffix == ".h":
                    check_rule_08_header_dirs(rel, code_only, errs)
                    check_rule_11_header_params(rel, code_only, allow_legacy=True, errs=errs)
                elif p.suffix in (".c", ".m"):
                    check_nesting_and_braces(rel, code_only, p.suffix == ".m", errs)

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

    # 1b. T-RULE-08 on public/dm/ (explicitly excluding public/proto/af_direct_proto/)
    check_dm_rule_08(root, errs)

    # 1c. MIT public/proto/af_direct_proto/ independent purity & T-RULE-09/10/11 gate
    check_direct_proto(root, require_exists=check_vendor_ui, errs=errs)

    # 1d. ipad/ios/*.swift basic hygiene (OSS-safe when ipad/ absent)
    check_swift_files(root, errs)

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
            if b"\r" in b:
                errs.append(f"[T-AF-BLD-04] {extra.name}: CR/CRLF line ending forbidden")
    for sub in ("tool", "test"):
        d = root / sub
        if d.is_dir():
            for p in sorted(d.rglob("*")):
                if p.is_file() and p.suffix in (".c", ".h", ".m", ".py", ".mk"):
                    b = p.read_bytes()
                    if b.startswith(b"\xef\xbb\xbf"):
                        errs.append(f"[T-AF-BLD-04] {p.relative_to(root).as_posix()}: UTF-8 BOM forbidden")
                    if b"\r" in b:
                        errs.append(f"[T-AF-BLD-04] {p.relative_to(root).as_posix()}: CR/CRLF forbidden")

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
            errs.append("[T-AF-BLD-04] shell/ui/setup.html SHA-256 mismatch (unexpected delta from locked v0.6.0 baseline)")

    return errs


def run_self_tests(repo_root: Path) -> list:
    """Positive & negative fixture tests for T-AF-BLD-01..04 and T-V6-L0-01..07."""
    failures = []
    banner = "/** @file t.c\n *  @brief fixture\n */\n"
    mit_banner = "/** @file p.h\n *  @brief proto fixture\n *  SPDX-License-Identifier: MIT\n */\n"
    mit_license = (
        "MIT License\n\nPermission is hereby granted, free of charge, to any person obtaining a copy.\n"
    )

    def write(td: Path, rel: str, content: bytes) -> None:
        p = td / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(content)

    # Case 1: T-AF-BLD-01 negative (auth -> session, base -> stream, stream -> input, shell bypasses)
    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        write(td, "src/auth/bad.c", (banner + '#include "../session/af_session.h"\n').encode())
        write(td, "src/base/bad_base.c", (banner + '#include "stream/af_stream.h"\n').encode())
        write(td, "src/stream/bad_cross.c", (banner + '#include "input/af_input.h"\n').encode())
        write(td, "src/input/bad_biz.c", (banner + '#include "session/af_session.h"\n').encode())
        write(td, "shell/bad1.m", (banner + '#import "../src/session/af_session.h"\n#include "plat/af_objc_undef.h"\n').encode())
        write(td, "shell/bad2.m", (banner + '#include <session/af_session.h>\n#include <dm.h>\n#include "plat/af_objc_undef.h"\n').encode())
        write(td, "shell/bad3.m", (banner + '#include "../Makefile"\n#include "plat/af_objc_undef.h"\n').encode())
        e = check_repo(td, check_vendor_ui=False)
        for fname in (
            "src/auth/bad.c", "src/base/bad_base.c", "src/stream/bad_cross.c",
            "src/input/bad_biz.c", "shell/bad1.m", "shell/bad2.m", "shell/bad3.m",
        ):
            if not any(fname in x and "T-AF-BLD-01" in x for x in e):
                failures.append(f"self-test T-AF-BLD-01 missed violation in {fname}: {e}")

    # Case 2: T-AF-BLD-01 positive (shell whitelist, sibling header, stream/input -> base/auth)
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
        write(
            td,
            "src/plat/af_plat.h",
            (banner + "int af_plat_sc_check_signatures(void); /* debug 启动校验 SidecarCore 方法签名 */\n").encode(),
        )
        write(td, "src/stream/ok_stream.c", (banner + '#include "base/af_https_srv.h"\n#include "auth/af_auth.h"\n').encode())
        write(td, "src/input/ok_input.c", (banner + '#include "base/af_https_srv.h"\n#include "plat/af_plat.h"\n').encode())
        e = check_repo(td, check_vendor_ui=False)
        if e:
            failures.append(f"self-test T-AF-BLD-01/02 false positive on valid files: {e}")

    # Case 3: T-AF-BLD-02 negative (new Apple frameworks, <uv/...>, private API, missing undef, ipad/core)
    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        write(td, "src/auth/fw.c", (banner + "@import Foundation;\n#include <uv/unix.h>\n").encode())
        write(td, "src/stream/fw_sc.c", (banner + "#include <ScreenCaptureKit/ScreenCaptureKit.h>\n").encode())
        write(td, "src/input/fw_carbon.c", (banner + "#include <Carbon/Carbon.h>\n").encode())
        write(td, "src/base/fw_vt.c", (banner + "#include <VideoToolbox/VideoToolbox.h>\n").encode())
        write(td, "shell/no_undef.m", (banner + "#include <dmc_client.h>\n#import <Foundation/Foundation.h>\n").encode())
        write(td, "src/plat/priv.m", (banner + '#include "plat/af_plat.h"\n#include "plat/af_objc_undef.h"\n#import <Foundation/Foundation.h>\nvoid f(void){ (void)NSClassFromString(@"SidecarDisplayManager"); }\n').encode())
        write(td, "ipad/core/bad.m", (banner + "#include <dm/module.h>\n#import <Foundation/Foundation.h>\nvoid f(void){ CGSMainConnectionID(); }\n").encode())
        e = check_repo(td, check_vendor_ui=False)
        checks = [
            ("src/auth/fw.c: Apple framework", "Apple framework/@import in src/auth/fw.c"),
            ("src/stream/fw_sc.c: Apple framework", "ScreenCaptureKit in src/stream/"),
            ("src/input/fw_carbon.c: Apple framework", "Carbon in src/input/"),
            ("src/base/fw_vt.c: Apple framework", "VideoToolbox in src/base/"),
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

    # Case 5: T-AF-BLD-04 function length precision
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

    # Case 6: T-AF-BLD-03(L0) 8-file VENDOR lock & T-AF-BLD-04 negative (BOM, CRLF, banner, UI & setup.html)
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
        for needle in (
            "BOM", "CRLF", "Doxygen", "T-AF-BLD-03",
            "missing required locked files",
            "dm/crypto.h",
            "frozen UI ipad/ui/app.js",
            "setup.html SHA-256 mismatch",
        ):
            if not any(needle in x for x in e):
                failures.append(f"self-test T-AF-BLD-03/04 missed '{needle}': {e}")

    # Case 7: T-RULE-04 raw malloc/calloc/realloc/free (positive + negative)
    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        write(
            td,
            "src/base/ok_heap.c",
            (
                banner
                + "void ok_fn(void *alc, void *doc, void *ssl, void *p) {\n"
                + "  yyjson_doc_free(doc);\n"
                + "  mbedtls_ssl_free(ssl);\n"
                + "  dm_free(p);\n"
                + "  dm_json_free_str(p);\n"
                + "  CFRelease(p);\n"
                + "  ((struct a *)alc)->free(alc, p);\n"
                + "}\n"
            ).encode(),
        )
        write(
            td,
            "src/base/bad_heap.c",
            (
                banner
                + "void bad_fn(void) {\n"
                + "  void *a = malloc(16);\n"
                + "  void *b = calloc(1, 16);\n"
                + "  a = realloc(a, 32);\n"
                + "  free(a);\n"
                + "  free(b);\n"
                + "}\n"
            ).encode(),
        )
        e = check_repo(td, check_vendor_ui=False)
        if any("ok_heap.c" in x for x in e):
            failures.append(f"self-test T-RULE-04 false positive on ok_heap.c: {e}")
        r04 = [x for x in e if "T-RULE-04" in x and "bad_heap.c" in x]
        if len(r04) != 5:
            failures.append(f"self-test T-RULE-04 expected 5 hits in bad_heap.c, got {len(r04)}: {e}")

    # Case 8: T-RULE-08 bare in/out/io & mandatory dm_in/dm_out/dm_io (excluding MIT proto)
    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        write(
            td,
            "src/stream/ok_dir.h",
            (
                banner
                + "int ok_api(dm_in const uint8_t *in, dm_out uint8_t *out, dm_io size_t *io_len);\n"
            ).encode(),
        )
        write(
            td,
            "src/plat/ok_objc_in.m",
            (
                banner
                + "#import <Foundation/Foundation.h>\n"
                + "void ok_iter(NSArray *arr) { for (id x in arr) { (void)x; } }\n"
            ).encode(),
        )
        write(td, "public/proto/af_direct_proto/LICENSE", mit_license.encode())
        write(
            td,
            "public/proto/af_direct_proto/af_direct_proto.h",
            (
                mit_banner
                + "#include <stddef.h>\n#include <stdint.h>\n"
                + "int proto_ok(const uint8_t *in, size_t len, uint8_t *out);\n"
            ).encode(),
        )
        write(td, "src/stream/bad_bare.c", (banner + "int f(in int a, out int *b, io int *c) { return a; }\n").encode())
        write(td, "src/stream/bad_hdr.h", (banner + "int bad_h(int a, out int *b);\n").encode())
        write(td, "public/dm/libdm/include/dm/bad_dm.h", (banner + "int dm_bad(in int x);\n").encode())
        e = check_repo(td, check_vendor_ui=False)
        for okf in ("ok_dir.h", "ok_objc_in.m", "af_direct_proto.h"):
            if any(okf in x for x in e):
                failures.append(f"self-test T-RULE-08 false positive on {okf}: {e}")
        for badf in ("bad_bare.c", "bad_hdr.h", "bad_dm.h"):
            if not any(badf in x and "T-RULE-08" in x for x in e):
                failures.append(f"self-test T-RULE-08 missed violation in {badf}: {e}")

    # Case 9: T-RULE-09 (nesting <= 4) & T-RULE-10 (mandatory {}) positive + negative
    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        write(
            td,
            "src/input/ok_ctrl.c",
            (
                banner
                + "int ok_ctrl(int a, int b, int c, int d) {\n"
                + "  struct s { int x; struct { int y; } inner; } v = { 1, { 2 } };\n"
                + "  (void)v;\n"
                + "  if (a) {\n"
                + "    for (int i = 0; i < b; i++) {\n"
                + "      while (c > 0) {\n"
                + "        if (d) { c--; } else if (b) { c--; } else { break; }\n"
                + "      }\n"
                + "    }\n"
                + "  }\n"
                + "  do { a--; } while (0);\n"
                + "  return a;\n"
                + "}\n"
            ).encode(),
        )
        write(
            td,
            "src/input/bad_nest.c",
            (
                banner
                + "int bad_nest(int a) {\n"
                + "  if (a) { if (a) { if (a) { if (a) { if (a) { return 1; } } } } }\n"
                + "  return 0;\n"
                + "}\n"
            ).encode(),
        )
        write(
            td,
            "src/input/bad_brace.c",
            (
                banner
                + "int bad_brace(int a) {\n"
                + "  if (a) return 1;\n"
                + "  else return 2;\n"
                + "  for (int i = 0; i < a; i++) a--;\n"
                + "  while (a > 0) a--;\n"
                + "  return a;\n"
                + "}\n"
            ).encode(),
        )
        write(
            td,
            "src/plat/objc_ctrl.m",
            (
                banner
                + "#import <Foundation/Foundation.h>\n"
                + "@implementation AfCtrlTest\n"
                + "- (int)okObjc:(int)a {\n"
                + "  NSDictionary *d = @{ @\"k\": @{ @\"v\": @(a) } };\n"
                + "  (void)d;\n"
                + "  if (a) { return 1; }\n"
                + "  return 0;\n"
                + "}\n"
                + "- (int)badObjc:(int)a {\n"
                + "  if (a) return 1;\n"
                + "  if (a) { if (a) { if (a) { if (a) { if (a) { return 2; } } } } }\n"
                + "  return 0;\n"
                + "}\n"
                + "@end\n"
            ).encode(),
        )
        write(td, "public/proto/af_direct_proto/LICENSE", mit_license.encode())
        write(
            td,
            "public/proto/af_direct_proto/proto_bad.c",
            (
                mit_banner
                + "int proto_bad(int a) {\n"
                + "  if (a) return 1;\n"
                + "  if (a) { if (a) { if (a) { if (a) { if (a) { return 2; } } } } }\n"
                + "  return 0;\n"
                + "}\n"
            ).encode(),
        )
        e = check_repo(td, check_vendor_ui=False)
        if any("ok_ctrl.c" in x for x in e):
            failures.append(f"self-test T-RULE-09/10 false positive on ok_ctrl.c: {e}")
        if not any("bad_nest.c" in x and "T-RULE-09" in x for x in e):
            failures.append(f"self-test T-RULE-09 missed bad_nest.c: {e}")
        b10 = [x for x in e if "bad_brace.c" in x and "T-RULE-10" in x]
        if len(b10) != 4:
            failures.append(f"self-test T-RULE-10 expected 4 hits in bad_brace.c, got {len(b10)}: {e}")
        if not any("objc_ctrl.m" in x and "T-RULE-09" in x for x in e):
            failures.append(f"self-test T-RULE-09 missed objc_ctrl.m: {e}")
        if not any("objc_ctrl.m" in x and "T-RULE-10" in x for x in e):
            failures.append(f"self-test T-RULE-10 missed objc_ctrl.m: {e}")
        if not any("proto_bad.c" in x and "T-RULE-09" in x for x in e):
            failures.append(f"self-test T-RULE-09 missed proto_bad.c: {e}")
        if not any("proto_bad.c" in x and "T-RULE-10" in x for x in e):
            failures.append(f"self-test T-RULE-10 missed proto_bad.c: {e}")

    # Case 10: T-RULE-11 (<= 5 params + exact 7 frozen legacy whitelist)
    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        write(
            td,
            "src/display/ok_params.h",
            (
                banner
                + "int ok5(dm_in int a, dm_in int b, dm_in int c, dm_in int d, dm_in int e);\n"
                + "int af_vd_eval_state(dm_in int a, dm_in int b, dm_in int c, dm_in int d, dm_in int e, dm_in int f, dm_in int g, dm_out int *h);\n"
            ).encode(),
        )
        write(
            td,
            "src/stream/bad_params.h",
            (
                banner
                + "int new_bad6(dm_in int a, dm_in int b, dm_in int c, dm_in int d, dm_in int e, dm_in int f);\n"
            ).encode(),
        )
        write(td, "public/proto/af_direct_proto/LICENSE", mit_license.encode())
        write(
            td,
            "public/proto/af_direct_proto/af_direct_proto.h",
            (
                mit_banner
                + "int af_vd_eval_state(int a, int b, int c, int d, int e, int f);\n"
            ).encode(),
        )
        e = check_repo(td, check_vendor_ui=False)
        if any("ok_params.h" in x for x in e):
            failures.append(f"self-test T-RULE-11 false positive on ok_params.h: {e}")
        if not any("bad_params.h" in x and "T-RULE-11" in x for x in e):
            failures.append(f"self-test T-RULE-11 missed new_bad6 in bad_params.h: {e}")
        if not any("af_direct_proto.h" in x and "T-RULE-11" in x for x in e):
            failures.append(f"self-test T-RULE-11 missed >5 params in MIT proto: {e}")

    # Case 11: MIT public/proto/af_direct_proto/ purity (positive + negative)
    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        write(td, "public/proto/af_direct_proto/LICENSE", mit_license.encode())
        write(
            td,
            "public/proto/af_direct_proto/af_direct_proto.h",
            (mit_banner + "#include <stdint.h>\n#include <stddef.h>\nint dp_ok(uint32_t x);\n").encode(),
        )
        write(
            td,
            "public/proto/af_direct_proto/af_direct_proto.c",
            (mit_banner + '#include "af_direct_proto.h"\n#include <string.h>\nint dp_ok(uint32_t x) { return (int)x; }\n').encode(),
        )
        e = check_repo(td, check_vendor_ui=False)
        if e:
            failures.append(f"self-test MIT proto false positive on clean proto: {e}")

    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        write(
            td,
            "public/proto/af_direct_proto/iPad_leak.h",
            (
                banner
                + "#include <dm/type.h>\n#include <unistd.h>\n#include <Foundation/Foundation.h>\n"
            ).encode(),
        )
        e = check_repo(td, check_vendor_ui=False)
        for needle in (
            "LICENSE missing",
            "forbidden 'ipad'",
            "SPDX-License-Identifier: MIT",
            "<dm/type.h>",
            "<unistd.h>",
            "<Foundation/Foundation.h>",
        ):
            if not any(needle in x for x in e):
                failures.append(f"self-test MIT proto missed '{needle}': {e}")

    # Case 12: ipad/ios/*.swift hygiene (positive + negative)
    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        write(td, "ipad/ios/GoodCrypto.swift", ("import Foundation\n" * 10).encode("utf-8"))
        e = check_repo(td, check_vendor_ui=False)
        if e:
            failures.append(f"self-test Swift hygiene false positive on GoodCrypto.swift: {e}")

    with tempfile.TemporaryDirectory() as raw_td:
        td = Path(raw_td)
        write(td, "ipad/ios/BadBomCrlf.swift", b"\xef\xbb\xbfimport Foundation\r\n")
        write(td, "ipad/ios/TooLong.swift", ("let x = 1\n" * 805).encode("utf-8"))
        e = check_repo(td, check_vendor_ui=False)
        for needle in ("BadBomCrlf.swift: UTF-8 BOM", "BadBomCrlf.swift: CRLF", "TooLong.swift: Swift file 805 lines"):
            if not any(needle in x for x in e):
                failures.append(f"self-test Swift hygiene missed '{needle}': {e}")

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
