#!/usr/bin/env python3
"""Fault-inject check_board_facts.py to prove its failure paths run."""

import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
CHECKER = os.path.join(HERE, "check_board_facts.py")
REAL_BOARDS = os.path.join(REPO, "boards", "SiWx917")

PLATFORM = os.path.join(REPO, "platform", "SiWx917")
HAVE_PLATFORM = os.path.isdir(os.path.join(PLATFORM, "tuyaos_adapter"))

failures = []
checks = 0
skipped = 0


def run(boards_dir, baseline, manifests=None):
    cmd = [sys.executable, CHECKER, "--boards", boards_dir,
           "--baseline", baseline]
    env = dict(os.environ)
    env.pop("TUYAOPEN_IDE_MANIFESTS", None)
    if manifests:
        cmd += ["--manifests", manifests]
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return p.returncode, p.stdout + p.stderr


def expect(cond, what):
    global checks
    checks += 1
    if cond:
        print(f"  ok   {what}")
    else:
        print(f"  FAIL {what}")
        failures.append(what)


def fresh(tmp, mutate=None):
    d = os.path.join(tmp, f"boards{len(os.listdir(tmp))}")
    shutil.copytree(REAL_BOARDS, d)
    if mutate:
        mutate(d)
    return d


def set_pin(boards_dir, board, label, value):
    p = os.path.join(boards_dir, board, "Kconfig")
    s = open(p, encoding="utf-8").read()
    new, n = re.subn(rf'(config BOARD_{label}_PIN\b.*?default )\d+',
                     rf'\g<1>{value}', s, count=1, flags=re.S)
    assert n == 1, f"BOARD_{label}_PIN not found in {board}"
    open(p, "w", encoding="utf-8").write(new)


def main():
    manifests = os.environ.get("TUYAOPEN_IDE_MANIFESTS")
    tmp = tempfile.mkdtemp(prefix="boardfacts.")
    try:
        empty = os.path.join(tmp, "empty_baseline.txt")
        open(empty, "w").close()

        base = os.path.join(tmp, "baseline.txt")
        shutil.copy(os.path.join(HERE, "board_facts_baseline.txt"), base)
        clean = fresh(tmp)
        rc, out = run(clean, base, manifests)
        expect(rc == 0, "unmodified tree is clean against its baseline")
        expect("0 new" in out, "unmodified tree reports no new conflicts")

        d = fresh(tmp, lambda b: set_pin(b, "BRD2605A", "SW2", 49))
        rc, out = run(d, base, manifests)
        expect(rc != 0, "collision: exits nonzero")
        expect("BRD2605A/collide/SW1-SW2" in out, "collision: names both pins")

        global skipped
        if HAVE_PLATFORM:
            d = fresh(tmp, lambda b: set_pin(b, "BRD2605A", "SW2", 54))
            rc, out = run(d, base, manifests)
            expect(rc != 0, "PSRAM overlap: exits nonzero")
            expect("BRD2605A/psram/SW2" in out, "PSRAM overlap: names the pin")
            expect("danger" in out, "PSRAM overlap: flagged danger, not warn")

            d = fresh(tmp, lambda b: set_pin(b, "BRD2605A", "SW2", 13))
            rc, out = run(d, base, manifests)
            expect(rc != 0, "unmapped pin: exits nonzero")
            expect("BRD2605A/pinmap/SW2" in out, "unmapped pin: names the pin")
        else:
            skipped += 5
            print("  skip 5 PSRAM/pin-map cases: platform/SiWx917 not checked out")

        def drop_sw3(b):
            p = os.path.join(b, "BRD2605A", "Kconfig")
            s = open(p, encoding="utf-8").read()
            s = re.sub(r'\nconfig BOARD_SW3_PIN\b.*?default \d+\n', '\n', s,
                       count=1, flags=re.S)
            open(p, "w", encoding="utf-8").write(s)
        d = fresh(tmp, drop_sw3)
        rc, out = run(d, base, manifests)
        expect(rc != 0, "stale baseline entry: exits nonzero")
        expect("STALE" in out and "BRD2605A/unused/SW3" in out,
               "stale baseline entry: named as stale, not silently dropped")

        d = fresh(tmp)
        rc, out = run(d, empty, None)
        expect("IDE manifest checks" in out and "skipped" in out,
               "no manifests: manifest checks reported as skipped")
        rc, out = run(d, empty, None)
        expect("manifest/psram" not in out,
               "no manifests: no manifest finding invented")

        p = subprocess.run(
            [sys.executable, CHECKER, "--boards", d, "--baseline", empty,
             "--platform", os.path.join(tmp, "nope")],
            capture_output=True, text=True)
        expect("pin-map checks" in p.stdout and "PSRAM overlap checks" in p.stdout,
               "no platform: both platform-sourced checks reported as skipped")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"\n{checks - len(failures)}/{checks} checks passed"
          + (f", {skipped} skipped" if skipped else ""))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
