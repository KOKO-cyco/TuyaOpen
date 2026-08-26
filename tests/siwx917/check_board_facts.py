#!/usr/bin/env python3
"""Cross-check the copies of SiWx917 board facts against each other."""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
BOARDS_DIR = os.path.join(REPO, "boards", "SiWx917")
BASELINE = os.path.join(HERE, "board_facts_baseline.txt")

PSRAM_SETS = {
    "PSRAM_GPIO_PIN_SET_52_TO_57": range(52, 58),
    "PSRAM_GPIO_PIN_SET_0_TO_5": range(0, 6),
    "PSRAM_GPIO_PIN_SET_46_TO_51_CS_0": range(46, 52),
    "PSRAM_GPIO_PIN_SET_46_TO_51_CS_1": range(46, 52),
}

ROLE_BY_PREFIX = (("LED", "led"), ("SW", "button"), ("KEY", "button"))


class Findings:
    def __init__(self):
        self.items = []
        self.skipped = []
        self.unverifiable = []

    def add(self, severity, key, message):
        self.items.append((severity, key, message))

    def skip(self, what, why, prefixes=()):
        self.skipped.append((what, why))
        self.unverifiable.extend(prefixes)


def boards():
    out = []
    for name in sorted(os.listdir(BOARDS_DIR)):
        kconfig = os.path.join(BOARDS_DIR, name, "Kconfig")
        if not os.path.isfile(kconfig):
            continue
        if re.search(r'^config BOARD_CHOICE\s*$',
                     open(kconfig, encoding="utf-8").read(), re.M):
            out.append(name)
    return out


def board_slug(board):
    return board.lower().replace("_", "-")


def read_kconfig_pins(board):
    path = os.path.join(BOARDS_DIR, board, "Kconfig")
    text = open(path, encoding="utf-8").read()
    pins, names = {}, {}
    for m in re.finditer(r'^config BOARD_(\w+)_PIN\b.*?^\s*default\s+(\d+)\s*$',
                         text, re.M | re.S):
        pins[m.group(1)] = int(m.group(2))
    for m in re.finditer(r'^config BOARD_(\w+)_NAME\b.*?^\s*default\s+"([^"]*)"\s*$',
                         text, re.M | re.S):
        names[m.group(1)] = m.group(2)
    return pins, names


def scan_board_c(board):
    d = os.path.join(BOARDS_DIR, board)
    gpios, refs, macros = {}, {}, {}
    for name in sorted(os.listdir(d)):
        if not name.endswith(".c"):
            continue
        stack, func = [], None
        for i, line in enumerate(open(os.path.join(d, name), encoding="utf-8"), 1):
            m = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*)', line)
            if m:
                kind, cond = m.group(1), m.group(2).split("/*")[0].strip()
                if kind in ("if", "ifdef", "ifndef"):
                    stack.append(cond or kind)
                elif kind == "endif" and stack:
                    stack.pop()
                elif kind in ("elif", "else") and stack:
                    stack[-1] = cond or "else"
                continue
            fm = re.match(r'\s*(?:static\s+)?[\w \t\*]+?(\w+)\s*\([^;]*$', line)
            if fm and "=" not in line:
                func = fm.group(1)
            guard = " && ".join(stack) if stack else None
            site = f"{name}:{i}"
            for mm in re.finditer(r'TUYA_GPIO_NUM_(\d+)\b', line):
                gpios.setdefault(int(mm.group(1)), []).append((site, guard))
            dm = re.match(r'\s*#\s*define\s+BOARD_(\w+)_PIN\s+TUYA_GPIO_NUM_(\d+)\b',
                          line)
            if dm:
                macros[dm.group(1)] = int(dm.group(2))
                continue
            for mm in re.finditer(r'\bBOARD_(\w+)_PIN\b', line):
                if line.lstrip().startswith("#define"):
                    continue
                refs.setdefault(mm.group(1), []).append((site, guard, func))
    return gpios, refs, macros


def read_pin_map(platform_dir):
    path = os.path.join(platform_dir, "tuyaos_adapter", "include", "gpio",
                        "si91x_pin_map.h")
    if not os.path.isfile(path):
        return None
    parts = open(path, encoding="utf-8").read().split("#define SI91X_PIN_MAPPING", 1)
    if len(parts) != 2:
        return None
    gpios = set(int(m.group(1))
                for m in re.finditer(r'X_(?:UULP|HP)\((\d+)\)', parts[1]))
    gpios |= set(int(m.group(1))
                 for m in re.finditer(r'X_ULP\(\s*(\d+)\s*,', parts[1]))
    return gpios or None


def read_psram_pins(platform_dir, board):
    slug = board_slug(board).replace("-", "_")
    candidates = [os.path.join(platform_dir, "mcu", "boards", slug,
                               "sl_si91x_psram_pin_config.h")]
    wise = os.path.join(platform_dir, "sdks", "wiseconnect", "components",
                        "board", "silabs")
    cfg_slcc = os.path.join(wise, "component", f"{slug}_config.slcc")
    if os.path.isfile(cfg_slcc):
        m = re.search(r'file_id:\s*psram_pin_config\s*\n\s*path:\s*(\S+)',
                      open(cfg_slcc, encoding="utf-8").read())
        if m:
            candidates.append(os.path.join(wise, "config", m.group(1)))
    candidates.append(os.path.join(wise, "config", slug,
                                   "sl_si91x_psram_pin_config.h"))
    for path in candidates:
        if not os.path.isfile(path):
            continue
        for line in open(path, encoding="utf-8"):
            m = re.match(r'\s*#define\s+PSRAM_GPIO_PIN_SET_SEL\s+(\S+)', line)
            if m:
                if m.group(1) not in PSRAM_SETS:
                    return None, f"{path}: unknown pin set {m.group(1)}"
                return (set(PSRAM_SETS[m.group(1)]),
                        f"{os.path.relpath(path, platform_dir)} ({m.group(1)})")
    return None, None


def read_manifest(manifests_dir, board):
    if not manifests_dir:
        return None
    path = os.path.join(manifests_dir, "boards-and-chips", "siwx917",
                        f"{board_slug(board)}.json")
    if not os.path.isfile(path):
        return None
    d = json.load(open(path, encoding="utf-8"))
    peripherals = {}
    for group, items in (d.get("peripheralPatterns") or {}).items():
        for item in items:
            gs = [int(p["gpio"])
                  for _iface, lst in (item.get("pins") or {}).items()
                  for p in lst if p.get("gpio") is not None]
            peripherals[str(item.get("id", "")).upper()] = (group, gs)
    expansion = set(int(p["gpio"]) for p in (d.get("expansionPins") or [])
                    if p.get("gpio") is not None)
    return peripherals, expansion


def expected_role(label):
    for prefix, role in ROLE_BY_PREFIX:
        if label.startswith(prefix):
            return role
    return None


def check_board(board, platform_dir, manifests_dir, f):
    kc_pins, kc_names = read_kconfig_pins(board)
    c_gpios, c_refs, c_macros = scan_board_c(board)

    for label, gpio in sorted(kc_pins.items()):
        sites = c_refs.get(label)
        if not sites:
            f.add("warn", f"{board}/unused/{label}",
                  f"Kconfig declares BOARD_{label}_PIN = {gpio} but no board "
                  f"source references it, so nothing is ever registered on "
                  f"that pin")
            continue
        role = expected_role(label)
        if role and not any(role in (fn or "") for _s, _g, fn in sites):
            where = ", ".join(f"{s} in {fn or '?'}" for s, _g, fn in sites)
            f.add("error", f"{board}/role/{label}",
                  f"BOARD_{label}_PIN = {gpio} is named for a {role} but is "
                  f"only used at {where}; the name and the wiring disagree")

    seen = {}
    for label, gpio in sorted(kc_pins.items()):
        if gpio in seen:
            f.add("error", f"{board}/collide/{seen[gpio]}-{label}",
                  f"BOARD_{seen[gpio]}_PIN and BOARD_{label}_PIN are both "
                  f"gpio {gpio}")
        seen[gpio] = label

    pin_map = read_pin_map(platform_dir)
    if pin_map is None:
        f.skip(f"{board}: pin-map checks",
               "si91x_pin_map.h unreadable -- is the platform checked out?",
               (f"{board}/pinmap/",))
    else:
        for label, gpio in sorted(kc_pins.items()):
            if gpio not in pin_map:
                f.add("error", f"{board}/pinmap/{label}",
                      f"BOARD_{label}_PIN = {gpio} is not in si91x_pin_map.h, "
                      f"so tkl_gpio cannot drive it")
        for gpio, sites in sorted(c_gpios.items()):
            if gpio not in pin_map:
                f.add("error", f"{board}/pinmap/c/{gpio}",
                      f"TUYA_GPIO_NUM_{gpio} at "
                      f"{', '.join(s for s, _g in sites)} is not in "
                      f"si91x_pin_map.h")

    psram, psram_src = read_psram_pins(platform_dir, board)
    if psram is None:
        f.skip(f"{board}: PSRAM overlap checks",
               psram_src or "no PSRAM pin set found for this board",
               (f"{board}/psram/", f"{board}/manifest/psram"))
    else:
        for label, gpio in sorted(kc_pins.items()):
            if gpio in psram:
                f.add("danger", f"{board}/psram/{label}",
                      f"BOARD_{label}_PIN = {gpio} is on the PSRAM bus "
                      f"({psram_src}); text and bss execute from PSRAM")
        for gpio, sites in sorted(c_gpios.items()):
            if gpio in psram:
                f.add("danger", f"{board}/psram/c/{gpio}",
                      f"TUYA_GPIO_NUM_{gpio} at "
                      f"{', '.join(s for s, _g in sites)} is on the PSRAM bus "
                      f"({psram_src})")

    man = read_manifest(manifests_dir, board)
    if man is None:
        f.skip(f"{board}: IDE manifest checks",
               "manifest not found; pass --manifests or set "
               "TUYAOPEN_IDE_MANIFESTS",
               (f"{board}/manifest/",))
        return
    peripherals, expansion = man

    if psram is not None and expansion & psram:
        f.add("danger", f"{board}/manifest/psram",
              f"the IDE offers gpio {sorted(expansion & psram)} as free "
              f"expansion pins, but they are the PSRAM bus ({psram_src}). "
              f"Code runs from PSRAM, so muxing one of these away stops the "
              f"CPU fetching the instructions doing it")

    wired = {}
    for label, sites in c_refs.items():
        gpio = kc_pins.get(label, c_macros.get(label))
        if gpio is None:
            continue
        guard = sites[0][1] if all(g for _s, g, _f in sites) else None
        wired[gpio] = (f"BOARD_{label}_PIN", guard)
    for gpio, sites in c_gpios.items():
        site, guard = sites[0]
        wired.setdefault(gpio, (site, guard))

    for gpio in sorted(expansion & set(wired)):
        who, guard = wired[gpio]
        if guard:
            f.add("note", f"{board}/manifest/conditional/{gpio}",
                  f"the IDE offers gpio {gpio} as a free expansion pin; the "
                  f"board claims it at {who} under \"{guard}\", so the two "
                  f"agree only while that is off")
        else:
            f.add("warn", f"{board}/manifest/claimed/{gpio}",
                  f"the IDE offers gpio {gpio} as a free expansion pin, but "
                  f"the board already uses it ({who})")

    for label, gpio in sorted(kc_pins.items()):
        if label not in c_refs:
            continue
        name = kc_names.get(label, label).upper()
        entry = peripherals.get(label) or peripherals.get(name)
        if entry is None:
            f.add("warn", f"{board}/manifest/missing/{label}",
                  f"the board wires BOARD_{label}_PIN = {gpio} but the IDE "
                  f"manifest has no entry for it, so the IDE does not show it")
        elif gpio not in entry[1]:
            f.add("error", f"{board}/manifest/pin/{label}",
                  f"BOARD_{label}_PIN = {gpio} but the IDE manifest puts "
                  f"{entry[0]} on gpio {entry[1]}")


def check_slcc_config_files(platform_dir, f):
    comp_dir = os.path.join(platform_dir, "slc")
    if not os.path.isdir(comp_dir):
        f.skip("platform .slcc config_file checks",
               "platform slc/ directory not present",
               ("slcc/",))
        return
    for root, _dirs, names in os.walk(comp_dir):
        for name in sorted(names):
            if not name.endswith(".slcc"):
                continue
            path = os.path.join(root, name)
            text = open(path, encoding="utf-8").read()
            rel = os.path.relpath(path, platform_dir)
            m = re.search(r'^-?\s*root_path:\s*"?([^"\n]+?)"?\s*$', text, re.M)
            base = os.path.join(platform_dir, m.group(1)) if m else platform_dir
            if m and not os.path.isdir(base):
                f.add("error", f"slcc/{name}/root_path",
                      f"{rel} declares root_path {m.group(1)}, which does not "
                      f"exist, so nothing it lists can be found")
                continue
            missing = [q for q in re.findall(r'^\s+path:\s+(\S+)\s*$', text, re.M)
                       if not os.path.isfile(os.path.join(base, q))]
            if missing:
                f.add("error", f"slcc/{name}/config_file",
                      f"{rel}: {len(missing)} of its config_file paths do not "
                      f"exist, starting with {missing[0]}. SLC resolves these "
                      f"only when the overridden component is in the project, "
                      f"so they fail whenever someone adds one")


def load_baseline():
    if not os.path.isfile(BASELINE):
        return set()
    keys = set()
    for line in open(BASELINE, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if line:
            keys.add(line)
    return keys


def main():
    global BOARDS_DIR, BASELINE
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", default=os.path.join(REPO, "platform", "SiWx917"))
    ap.add_argument("--boards", default=BOARDS_DIR)
    ap.add_argument("--baseline", default=BASELINE)
    ap.add_argument("--manifests", default=os.environ.get("TUYAOPEN_IDE_MANIFESTS"))
    ap.add_argument("--update-baseline", action="store_true")
    args = ap.parse_args()

    BOARDS_DIR, BASELINE = args.boards, args.baseline

    f = Findings()
    board_list = boards()
    if not board_list:
        print(f"no board directories under {BOARDS_DIR}", file=sys.stderr)
        return 2
    for b in board_list:
        check_board(b, args.platform, args.manifests, f)
    check_slcc_config_files(args.platform, f)

    for what, why in f.skipped:
        print(f"  skipped  {what}: {why}")

    baseline = load_baseline()
    keys = set(k for _s, k, _m in f.items)
    new = sorted(k for _s, k, _m in f.items if k not in baseline)
    stale = sorted(k for k in baseline - keys
                   if not any(k.startswith(pre) for pre in f.unverifiable))

    for severity, key, msg in sorted(f.items):
        print(f"  {severity:6s} {'known' if key in baseline else 'NEW':5s} "
              f"{key}\n           {msg}")

    if args.update_baseline:
        with open(BASELINE, "w", encoding="utf-8") as fh:
            for severity, key, msg in sorted(f.items):
                fh.write(f"{key}  # {severity}: {msg}\n")
        print(f"\nbaseline rewritten: {len(f.items)} entries")
        return 0

    unchecked = len(baseline) - len(keys & baseline) - len(stale)
    print(f"\n{len(f.items)} conflict(s): {len(f.items) - len(new)} known, "
          f"{len(new)} new; {len(stale)} baselined but no longer reproducing"
          + (f"; {unchecked} not checkable in this run" if unchecked else ""))
    for k in new:
        print(f"  NEW      {k}")
    for k in stale:
        print(f"  STALE    {k} -- fixed? delete it from the baseline")
    return 1 if (new or stale) else 0


if __name__ == "__main__":
    sys.exit(main())
