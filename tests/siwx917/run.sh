#!/bin/sh
set -e

here=$(cd "$(dirname "$0")" && pwd)
plat=$here/../../platform/SiWx917
adapter=$plat/tuyaos_adapter
out=${TMPDIR:-/tmp}/tuya_siwx917_tests
mkdir -p "$out"

cc=${CC:-gcc}
cflags="-O1 -g -Wall -Wextra -Werror"

common_inc="-I$here/fake -I$adapter/include/utilities/include -I$here/../../src/common/include -I$here/stub"

run_one() {
    name=$1
    shift
    # shellcheck disable=SC2086
    $cc $cflags $common_inc "$@" -o "$out/$name" "$here/$name.c"
    "$out/$name"
    echo
}

run_one test_tkl_rtc -I"$adapter/include/rtc" \
    "$adapter/src/tkl_rtc.c" "$here/fake/fake_calendar.c"

run_one test_tkl_pwm_calc -I"$adapter/include/pwm"

run_one test_tkl_pwm -I"$adapter/include/pwm" \
    "$adapter/src/tkl_pwm.c" "$here/fake/fake_pwm.c"

run_one test_tkl_adc -I"$adapter/include/adc" \
    "$adapter/src/tkl_adc.c" "$here/fake/fake_adc.c"

run_one test_tkl_dac -I"$adapter/include/dac" \
    "$adapter/src/tkl_dac.c" "$here/fake/fake_dac.c"

run_one test_tkl_pinmux -I"$adapter/include/pinmux" -I"$adapter/include/gpio" \
    "$adapter/src/tkl_pinmux.c" "$here/fake/fake_gpio.c"

"${PYTHON:-python3}" "$here/test_extract_rte_pinmux.py"
echo

"${PYTHON:-python3}" "$here/test_kconfig2slcp.py" | grep -vE '^(--->|CONFIG_|=====|Updated)'
echo

"${PYTHON:-python3}" "$here/test_check_board_facts.py"
echo
"${PYTHON:-python3}" "$here/check_board_facts.py"
echo

echo "all SiWx917 host tests passed"
