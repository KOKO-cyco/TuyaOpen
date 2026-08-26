#!/usr/bin/env python3
# coding=utf-8
"""Tests for script/gen/extract_rte_pinmux.py."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SCRIPT = os.path.join(ROOT, 'platform', 'SiWx917', 'script', 'gen', 'extract_rte_pinmux.py')
HEADER = os.path.join(ROOT, 'platform', 'SiWx917', 'mcu', 'boards',
                      'siwx917_ai_dev_kit', 'RTE_Device_917.h')
HEADER_BRD2605A = os.path.join(
    ROOT, 'platform', 'SiWx917', 'sdks', 'wiseconnect', 'components', 'board',
    'silabs', 'config', 'brd2605a', 'RTE_Device_917.h')
GENERATOR = os.path.join(ROOT, 'platform', 'SiWx917', 'script', 'gen', 'gen_pinmux_table.py')
GOLDEN = os.path.join(HERE, 'golden', 'rte_pinmux_ai_dev_kit.txt')

fails = []


def check(what, ok, detail=''):
    print('%-48s %s   %s' % (what, 'ok  ' if ok else 'FAIL', detail))
    if not ok:
        fails.append(what)


def run(*args):
    return subprocess.run([sys.executable, SCRIPT, HEADER] + list(args),
                          capture_output=True, text=True, check=True).stdout


def parse_rows(text):
    rows = {}
    for line in text.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 5:
            rows.setdefault(parts[0], []).append(tuple(parts[1:5]))
    return rows


def main():
    check('extractor is executable', os.access(SCRIPT, os.X_OK))
    check('board header present', os.path.isfile(HEADER))

    rows = parse_rows(run())

    check('PWM_1L -> HP pad 6 mux 10', ('HP', '6', '10', '1') in rows.get('PWM_1L', []))
    check('PWM_1L has a single option', len(rows.get('PWM_1L', [])) == 1,
          'its #elif branch is an #error')
    check('PWM_2H -> HP pad 9 mux 10', ('HP', '9', '10', '4') in rows.get('PWM_2H', []))
    check('PWM_1H has two options', len(rows.get('PWM_1H', [])) == 2, 'pads 7 and 65')
    check('I2C1_SCL -> pad 50 mux 5', ('HP', '50', '5', '14') in rows.get('I2C1_SCL', []))

    muxes = {m for _, _, m, _ in rows.get('PWM_1H', [])}
    check('PWM_1H mux differs per pad', len(muxes) > 1, 'mux is data, not a formula')

    check('ULP_UART_TX resolves its first option',
          ('RTE_ULP_PORT', '71', '3', '0') in rows.get('ULP_UART_TX', []),
          '(7 + GPIO_MAX_PIN) is pin 71')
    check('I2C2_SDA resolves an expression option',
          any(p == '70' for _, p, _, _ in rows.get('I2C2_SDA', [])),
          'ULP pads live at 64 + index')
    check('ULP pads land above the HP range',
          any(int(p) >= 64 for _, p, _, _ in rows.get('ULP_UART_TX', [])), None)

    orphan = 0
    for sig, variants in rows.items():
        for port, pin, mux, pad in variants:
            if not pin or not mux or not port:
                orphan += 1
    check('no variant is missing port, pin or mux', orphan == 0,
          'a gap here means branches were mis-split')

    check('no ADC signals in the mux table', not any(s.startswith('ADC') for s in rows),
          'analog inputs bypass the pin mux')
    check('no DAC signals in the mux table', not any(s.startswith('DAC') for s in rows))

    if os.path.isfile(HEADER_BRD2605A) and os.path.isfile(GENERATOR):
        def table_of(hdr):
            out = subprocess.run([sys.executable, GENERATOR, hdr],
                                 capture_output=True, text=True, check=True).stdout
            return sorted(l.split('/*')[0].strip()
                          for l in out.splitlines() if l.strip().startswith('{TUYA_'))

        a, b = table_of(HEADER), table_of(HEADER_BRD2605A)
        check('both boards yield the same number of rows', len(a) == len(b),
              'ai_dev_kit %d, brd2605a %d' % (len(a), len(b)))
        check('both boards yield an identical pin/mux table', a == b,
              'the mapping is chip-level, not board-level')
    else:
        check('BRD2605A header available for cross-check', False, HEADER_BRD2605A)

    if os.path.isfile(GOLDEN):
        with open(GOLDEN, encoding='utf-8') as fh:
            expect = fh.read().rstrip('\n')
        actual = '\n'.join(l.rstrip() for l in run().rstrip('\n').splitlines())
        check('output matches golden file', actual == expect,
              'regenerate if the board header changed')
    else:
        check('golden file present', False, GOLDEN)

    print('\n%s' % ('FAILED' if fails else 'all passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
