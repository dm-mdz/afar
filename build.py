#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Afar_C unified build, static gate, and test runner.

Usage:
  python3 build.py [build] [flags...]     Build libs + afar-agent + dmcmd (default)
  python3 build.py check                  Run L0 static gate & self-test fixtures
  python3 build.py test [filter] [flags]  Build and run tests (default / asan / tsan / aqua / stress)
  python3 build.py clean [all]            Remove current config output (or entire build/)
  python3 build.py help                   Show usage

Flags (any order):
  debug | release | mem | asan | tsan | aqua | stress
"""
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent
BUILD_ROOT = ROOT_DIR / "build"
LIBUV_DIR = ROOT_DIR / "public" / "third" / "libuv"
MBEDTLS_DIR = ROOT_DIR / "public" / "third" / "mbedtls"

FLAGS = {"debug", "release", "mem", "asan", "tsan", "aqua", "stress"}


def say(msg: str) -> None:
    print(f"\x1b[1;34m[af]\x1b[0m {msg}", flush=True)


def fail(msg: str) -> None:
    print(f"\x1b[1;31m[af]\x1b[0m {msg}", file=sys.stderr, flush=True)
    sys.exit(1)


def find_cmake() -> str:
    env_cmake = os.environ.get("CMAKE")
    if env_cmake and Path(env_cmake).is_file():
        return env_cmake
    found = shutil.which("cmake")
    if found:
        return found
    for alt in ("/opt/homebrew/bin/cmake", "/usr/local/bin/cmake"):
        if Path(alt).is_file():
            return alt
    fail("cmake not found in PATH (set CMAKE=/path/to/cmake or install via Homebrew)")
    return ""


class Config:
    def __init__(self, flags: set):
        self.mode = "release" if "release" in flags else "debug"
        self.mem = "mem" in flags
        self.asan = "asan" in flags
        self.tsan = "tsan" in flags
        self.aqua = "aqua" in flags
        self.stress = "stress" in flags
        if self.asan and self.tsan:
            fail("asan and tsan are mutually exclusive; run them separately")
        if self.aqua and self.stress:
            fail("aqua and stress are mutually exclusive; run them separately")
        self.os_target = "darwin"
        suffix = ""
        if self.mem:
            suffix += "-mem"
        if self.asan:
            suffix += "-asan"
        if self.tsan:
            suffix += "-tsan"
        if self.stress:
            suffix += "-stress"
        if self.aqua:
            suffix += "-aqua"
        self.out = BUILD_ROOT / f"{self.os_target}-{self.mode}{suffix}"
        self.out_rel = self.out.relative_to(ROOT_DIR).as_posix()
        self.log_dir = self.out / "log"
        self.cmake = find_cmake()
        self.jobs = os.environ.get("JOBS") or str(os.cpu_count() or 4)

    def san_cflags(self) -> str:
        if self.asan:
            return "-fsanitize=address,undefined -fno-omit-frame-pointer"
        if self.tsan:
            return "-fsanitize=thread -fno-omit-frame-pointer"
        return ""

    def make_args(self) -> list:
        args = [
            "make", "-C", str(ROOT_DIR), "--no-print-directory", f"-j{self.jobs}",
            f"OS_TARGET={self.os_target}",
            f"MODE={self.mode}",
            f"MEM={1 if self.mem else 0}",
            f"ASAN={1 if self.asan else 0}",
            f"TSAN={1 if self.tsan else 0}",
            f"STRESS={1 if self.stress else 0}",
            f"AQUA={1 if self.aqua else 0}",
            f"OUT={self.out_rel}",
        ]
        if os.environ.get("CFLAGS"):
            args.append(f"CFLAGS={os.environ['CFLAGS']}")
        if os.environ.get("LDFLAGS"):
            args.append(f"LDFLAGS={os.environ['LDFLAGS']}")
        return args


def run_logged(cfg: Config, name: str, args: list) -> None:
    cfg.log_dir.mkdir(parents=True, exist_ok=True)
    logfile = cfg.log_dir / f"{name}.log"
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert p.stdout is not None
    with open(logfile, "wb") as lf:
        for line in p.stdout:
            lf.write(line)
            sys.stdout.buffer.write(line)
            sys.stdout.buffer.flush()
        rc = p.wait()
    if rc != 0:
        fail(f"{name} failed (exit {rc}); log: {logfile}")


def cmake_configure(cfg: Config, name: str, bdir: Path, args: list) -> None:
    cfg.log_dir.mkdir(parents=True, exist_ok=True)
    logfile = cfg.log_dir / f"{name}.log"
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert p.stdout is not None
    with open(logfile, "wb") as lf:
        for line in p.stdout:
            lf.write(line)
        rc = p.wait()
    if rc != 0:
        shutil.rmtree(bdir, ignore_errors=True)
        sys.stderr.write(logfile.read_text(encoding="utf-8", errors="replace"))
        fail(f"{name} failed (exit {rc}); log: {logfile}")


def build_third_party(cfg: Config) -> None:
    cmake_type = "Release" if cfg.mode == "release" else "Debug"
    san = cfg.san_cflags()
    gen = ["-G", "Ninja"] if shutil.which("ninja") else []

    # 1. libuv
    prefix_map = f"-ffile-prefix-map={ROOT_DIR.as_posix()}=/afar"
    uv_dir = cfg.out / "libuv"
    uv_lib = uv_dir / "libuv.a"
    if not uv_lib.is_file():
        say(f"building libuv ({cmake_type})...")
        uv_flags = f"{prefix_map} {san}".strip()
        c_args = [
            cfg.cmake, *gen, "-S", str(LIBUV_DIR), "-B", str(uv_dir),
            f"-DCMAKE_BUILD_TYPE={cmake_type}",
            "-DLIBUV_BUILD_TESTS=OFF", "-DLIBUV_BUILD_BENCH=OFF",
            "-DBUILD_TESTING=OFF",
            f"-DCMAKE_C_FLAGS={uv_flags}",
        ]
        cmake_configure(cfg, "libuv-configure", uv_dir, c_args)
        run_logged(cfg, "libuv-build", [cfg.cmake, "--build", str(uv_dir), "--target", "uv_a", "-j", cfg.jobs])

    # 2. mbedTLS 3.6.8 (with mbedtls_user_config.h)
    mb_dir = cfg.out / "mbedtls"
    mb_libs = [mb_dir / "library" / f"libmbed{x}.a" for x in ("tls", "x509", "crypto")]
    if not all(p.is_file() for p in mb_libs):
        say(f"building mbedTLS 3.6.8 ({cmake_type})...")
        inc_third = (ROOT_DIR / "public" / "third").as_posix()
        mb_flags = f'-I{inc_third} -DMBEDTLS_USER_CONFIG_FILE=\\"mbedtls_user_config.h\\" {prefix_map}'
        if san:
            mb_flags = f"{mb_flags} {san}"
        c_args = [
            cfg.cmake, *gen, "-S", str(MBEDTLS_DIR), "-B", str(mb_dir),
            f"-DCMAKE_BUILD_TYPE={cmake_type}",
            "-DENABLE_TESTING=OFF", "-DENABLE_PROGRAMS=OFF",
            "-DUSE_SHARED_MBEDTLS_LIBRARY=OFF", "-DUSE_STATIC_MBEDTLS_LIBRARY=ON",
            f"-DCMAKE_C_FLAGS={mb_flags}",
        ]
        cmake_configure(cfg, "mbedtls-configure", mb_dir, c_args)
        run_logged(cfg, "mbedtls-build", [
            cfg.cmake, "--build", str(mb_dir),
            "--target", "mbedtls", "mbedx509", "mbedcrypto", "-j", cfg.jobs
        ])



def cmd_check() -> None:
    say("running L0 static gate (tool/check_bld.py)...")
    r = subprocess.run([sys.executable, str(ROOT_DIR / "tool" / "check_bld.py"), "--self-test"])
    if r.returncode != 0:
        fail("L0 static gate failed")
    say("L0 static gate PASSED")


def cmd_build(cfg: Config) -> None:
    build_third_party(cfg)
    say(f"building all -> {cfg.out_rel}")
    run_logged(cfg, "build", cfg.make_args() + ["all"])
    say("build PASSED")


def cmd_test(cfg: Config, filters: list) -> None:
    build_third_party(cfg)
    if cfg.aqua:
        target, list_target = "aqua-tests", "list-aqua-tests"
    elif cfg.stress:
        target, list_target = "stress-tests", "list-stress-tests"
    else:
        target, list_target = "tests", "list-tests"

    run_logged(cfg, f"build-{target}", cfg.make_args() + [target])
    out = subprocess.check_output(cfg.make_args() + ["-s", list_target], text=True)
    tests = []
    for line in out.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) == 2:
            mod, path = parts
            if not filters or any(f in mod or f in path for f in filters):
                tests.append((mod, ROOT_DIR / path))

    if not tests:
        if filters:
            fail(f"no test binaries matched filters: {filters}")
        say(f"no test binaries registered for target '{target}'")
        return

    timeout = int(os.environ.get("TEST_TIMEOUT", "300"))
    passed, failed = 0, []
    t0 = time.monotonic()
    for mod, exe in tests:
        say(f"run [{mod}] {exe.name}")
        env = os.environ.copy()
        if cfg.asan:
            leak_flag = "0" if sys.platform == "darwin" else "1"
            env.setdefault("ASAN_OPTIONS", f"detect_leaks={leak_flag}:abort_on_error=1")
            env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=1")
        if cfg.tsan:
            env.setdefault("TSAN_OPTIONS", "halt_on_error=1")
        try:
            r = subprocess.run([str(exe)], cwd=str(ROOT_DIR), env=env, timeout=timeout)
            if r.returncode == 0:
                passed += 1
            else:
                failed.append((exe.name, f"exit {r.returncode}"))
        except subprocess.TimeoutExpired:
            failed.append((exe.name, f"timeout {timeout}s"))
    dt = time.monotonic() - t0
    if failed:
        for name, why in failed:
            print(f"  FAIL: {name} ({why})", file=sys.stderr)
        fail(f"tests failed: {len(failed)}/{len(tests)} in {dt:.2f}s")
    say(f"all {passed} test binary(s) PASSED in {dt:.2f}s")


def cmd_clean(cfg: Config, rest: list) -> None:
    if rest and rest[0] == "all":
        shutil.rmtree(BUILD_ROOT, ignore_errors=True)
        say("cleaned build/")
    else:
        shutil.rmtree(cfg.out, ignore_errors=True)
        say(f"cleaned {cfg.out_rel}")


def main() -> None:
    raw = sys.argv[1:]
    flags = {a for a in raw if a in FLAGS}
    pos = [a for a in raw if a not in FLAGS]
    cmd = pos[0] if pos else "build"
    rest = pos[1:]

    if cmd in ("help", "-h", "--help"):
        print(__doc__)
        return
    if cmd == "check":
        cmd_check()
        return
    cfg = Config(flags)
    if cmd == "build":
        cmd_build(cfg)
    elif cmd == "test":
        cmd_test(cfg, rest)
    elif cmd == "clean":
        cmd_clean(cfg, rest)
    else:
        fail(f"unknown command: {cmd}")


if __name__ == "__main__":
    main()
