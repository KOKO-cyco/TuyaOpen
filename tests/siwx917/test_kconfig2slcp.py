#!/usr/bin/env python3
"""Exercise the platform's build_kconfig2slcp.py against synthetic inputs."""

import importlib.util
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
GEN = os.path.join(REPO, "platform", "SiWx917", "build_kconfig2slcp.py")

failures = []
checks = 0


def expect(cond, what):
    global checks
    checks += 1
    if cond:
        print(f"  ok   {what}")
    else:
        print(f"  FAIL {what}")
        failures.append(what)


TEMPLATE = """project_name: tuyaopen_si91x_template
component:
  - id: sl_system
    from: wiseconnect3_sdk
configuration:
- {name: SL_ALREADY_THERE, value: '7'}

define:
  - name: ALREADY_DEFINED
    value: 1

toolchain_settings:
- {value: -Wall, option: gcc_compiler_option}
"""


def gen(mod, config_lines, template=TEMPLATE):
    d = tempfile.mkdtemp(prefix="k2s.")
    cfg = os.path.join(d, "using.config")
    slcp = os.path.join(d, "out.slcp")
    open(cfg, "w").write("\n".join(config_lines) + "\n")
    open(slcp, "w").write(template)
    mod.kconfig2slcp(cfg, slcp)
    return open(slcp).read()


def main():
    if not os.path.isfile(GEN):
        print(f"  skip build_kconfig2slcp.py not found at {GEN}")
        print("       (platform/SiWx917 not checked out)")
        return 0

    spec = importlib.util.spec_from_file_location("k2s", GEN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    out = gen(mod, ["CONFIG_ENABLE_ADC=y"])
    expect("- id: sl_adc" in out, "bool symbol adds its component")
    expect(out.count("- id: sl_system") == 1,
           "the template's own component survives once")

    out = gen(mod, ["# CONFIG_ENABLE_ADC is not set"])
    expect("sl_adc" not in out, "an unset symbol adds nothing")

    out = gen(mod, ["CONFIG_ENABLE_ADC_SOMETHING_ELSE=y"])
    expect("sl_adc" not in out,
           "matching is anchored: a longer symbol does not fire sl_adc")

    out = gen(mod, ["CONFIG_SIWX917_FREERTOS_HEAP_SIZE=98304"])
    expect("{name: configTOTAL_HEAP_SIZE, value: '98304'}" in out,
           "an int symbol's value reaches the configuration")

    out = gen(mod, ["CONFIG_SIWX917_TFLITE_ARENA_SIZE=0"])
    expect("SL_TFLITE_MICRO_ARENA_SIZE" not in out,
           "a skip_values match emits nothing rather than a zero")

    out = gen(mod, ["CONFIG_SIWX917_FREERTOS_HEAP_SIZE_PSRAM=4194304"])
    expect("{name: configTOTAL_HEAP_SIZE_PSRAM, value: '4194304'}" in out,
           "a non-skipped value is emitted")

    out = gen(mod, ["CONFIG_ENABLE_EXT_RAM=y"])
    expect("- name: SLI_SI91X_MCU_ENABLE_PSRAM_FEATURE" in out,
           "a valueless define is emitted bare")
    expect("SLI_SI91X_MCU_ENABLE_PSRAM_FEATURE\n    value" not in out,
           "a valueless define gets no value line")
    expect("- name: TKL_MEMORY\n    value: MEMORY_FREERTOS_HEAP_PSRAM" in out,
           "a define with a value is emitted with it")
    expect("- id: psram_core" in out and "- id: psram_aps6404l_sqh" in out,
           "PSRAM pulls in both the driver and the fitted device")

    out = gen(mod, ["CONFIG_ENABLE_SPI=y"])
    expect("id: sl_ssi_instance" in out and "instance: [primary]" in out,
           "instance components are emitted in instance form")
    expect(out.count("id: sl_ssi_instance") == 3,
           "all three SSI instances are emitted, not deduplicated together")
    out = gen(mod, ["CONFIG_ENABLE_AUDIO=y"])
    expect("instance: [i2s0]" in out, "audio brings in the I2S0 instance")

    out = gen(mod, ["CONFIG_ENABLE_ADC=y"],
              TEMPLATE.replace("  - id: sl_system\n    from: wiseconnect3_sdk\n",
                               "  - id: sl_adc\n    from: wiseconnect3_sdk\n"))
    expect(out.count("id: sl_adc") == 1,
           "a component already in the template is not added twice")

    out = gen(mod, ["CONFIG_ENABLE_EXT_RAM=y"],
              TEMPLATE.replace("  - name: ALREADY_DEFINED\n    value: 1\n",
                               "  - name: TKL_MEMORY\n    value: SOMETHING\n"))
    expect(out.count("TKL_MEMORY") == 1,
           "a define already in the template is not added twice")
    expect("SOMETHING" in out,
           "and the template's value is the one that survives")

    ycb = gen(mod, ["CONFIG_ENABLE_EXT_RAM=y",
                    "CONFIG_SIWX917_FREERTOS_HEAP_SIZE=98304",
                    "CONFIG_SIWX917_FREERTOS_HEAP_SIZE_PSRAM=4194304",
                    "CONFIG_SIWX917_TFLITE_ARENA_SIZE=2048"])
    for want in ("psram_core", "psram_aps6404l_sqh",
                 "SLI_SI91X_MCU_ENABLE_PSRAM_FEATURE",
                 "MEMORY_FREERTOS_HEAP_PSRAM",
                 "value: '98304'", "value: '4194304'", "value: '2048'"):
        expect(want in ycb, f"your_chat_bot.slcp's {want} is reproducible")

    print(f"\n{checks - len(failures)}/{checks} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
