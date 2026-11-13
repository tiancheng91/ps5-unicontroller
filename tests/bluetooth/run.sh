#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 tools/embed_controllers.py
D=src/drivers/bluetooth
T=tests/bluetooth
B=$(mktemp -d /tmp/uc-bt-tests.XXXXXX)
trap 'rm -rf "$B"' EXIT
CC=${CC:-cc}
common=(-std=c11 -Wall -Wextra -Werror -O1 -g -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -I"$D" -Isrc/core -Isrc/drivers/protocols -Isrc/drivers/common -Ibuild)
core=("src/drivers/config/overrides.c" "src/drivers/config/json.c" "src/drivers/common/util.c" "src/logging.c" "vendor/rxi-log/log.c" "src/drivers/common/crc32.c" "$D/smp_crypto.c" "src/drivers/protocols/profiles.c" "src/drivers/protocols/generic.c" "$D/host.c" "$D/le.c")
"$CC" "${common[@]}" "${core[@]}" "$T/sim.c" "$T/test_host.c" -o "$B/host"
"$CC" "${common[@]}" "${core[@]}" "$T/sim_le.c" "$T/test_le.c" -o "$B/le"
"$CC" "${common[@]}" "${core[@]}" "$T/test_profiles.c" -o "$B/profiles"
"$CC" "${common[@]}" "$D/usb_desc.c" "$D/evstream.c" "$T/test_usb.c" -o "$B/usb"
"$CC" "${common[@]}" "$D/smp_crypto.c" "$T/test_crypto.c" -o "$B/crypto"
mkdir -p "$B/build" "$B/tests/bluetooth"
cp -R "$T/fixtures" "$B/tests/bluetooth/fixtures"
cd "$B"
for test in host le profiles usb crypto; do "$B/$test"; done
