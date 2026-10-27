#!/usr/bin/env bash
# QEMU smoke test: build, stage the test ESP, boot in AArch64 QEMU.
#   1. no input: the countdown boots the default entry, which checks every
#      hand-over (DTB patches + overlay, /chosen, injected kit driver, initrd
#      media, variable) and powers off;
#   2. keyboard: Down + Enter picks the second entry;
#   3. degraded: no display device and no config file, so the built-in
#      configuration runs on the text console and its default (Return to
#      firmware) fires when the timer ends;
#   4. block slice: UniXNU on a bare ESP finds its config on a FAT volume
#      at the start of a GPT partition named "userdata" and publishes the
#      rest of that partition as its own block device (needs mtools, or a
#      python with pyfatfs in FAT_PYTHON; skipped otherwise);
#   5. kext staging and the XNU loader protocol revisions;
#   6. layout: a tall 1080x2400 display gets the portrait layout, and a
#      800x1000 display (width/height exactly at the threshold) keeps landscape.
# Logs and screenshots go to Build/Smoke/.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TARGET=${TARGET:-DEBUG}
OUT=$ROOT/Build/Out/AARCH64-$TARGET
SMOKE=$ROOT/Build/Smoke
ESP=$SMOKE/esp
KEXT_TIMEOUT=${KEXT_TIMEOUT:-60}

"$ROOT/build.sh" -b "$TARGET" >/dev/null
mkdir -p "$SMOKE"

python3 "$ROOT/Scripts/stage-esp.py" -o "$ESP" "$ROOT/UniXNUPkg/Test/Qemu/esp" \
  --put "$OUT/UniXNU.efi=EFI/BOOT/BOOTAA64.EFI" \
  --put "$OUT/HelloPayload.efi=EFI/UniXNU/Payloads/HelloPayload.efi" \
  --put "$OUT/SampleDriver.efi=EFI/UniXNU/Kits/SampleDriver.kit/SampleDriver.efi"

echo "== scenario 1: countdown boots the default entry"
python3 "$ROOT/Scripts/qemu-run.py" "$ESP" \
  --log "$SMOKE/default.log" --screenshot "$SMOKE/menu.png" \
  --expect 'UNIXNU: UniXNU [0-9.]+ starting as application' \
  --expect 'UNIXNU: config: loaded' \
  --expect 'UNIXNU: kit SampleDriver 1\.0\.0: loaded' \
  --expect 'UNIXNU: kit NeverMatch 0\.0\.1: not loaded' \
  --expect 'UNIXNU: driver .*SampleDriver\.efi \(kit SampleDriver\): started' \
  --expect 'UNIXNU: ui: graphics' \
  --expect 'UNIXNU: menu: shown 4 entries, default 0' \
  --expect 'UNIXNU: menu: timeout, booting default 0' \
  --expect 'UNIXNU: dt: installed' \
  --expect "UNIXNU-HELLO: check dtb PASS \(bootargs='console=ttyAMA0 unixnu.test=1' marker='patched'\)" \
  --expect 'UNIXNU-HELLO: check overlay PASS' \
  --expect "UNIXNU-HELLO: check driver PASS \(sample driver args='from-kit'\)" \
  --expect 'UNIXNU-HELLO: check initrd PASS .*UNIXNU-INITRD-OK' \
  --expect "UNIXNU-HELLO: check var PASS \(UniXNUTest='hello-variable'" \
  --expect 'UNIXNU-HELLO: RESULT PASS 5 passed 0 failed' \
  --reject 'UNIXNU: config: .*(error|needs|unknown)' \
  --reject 'ASSERT'

echo "== scenario 2: keyboard selects the second entry"
python3 "$ROOT/Scripts/qemu-run.py" "$ESP" \
  --log "$SMOKE/keys.log" --keys down,ret \
  --expect 'UNIXNU: menu: enter, booting 1' \
  --expect "UNIXNU-HELLO: load options 'plain shutdown'" \
  --expect 'UNIXNU-HELLO: RESULT PASS 0 passed 0 failed' \
  --reject 'UNIXNU: menu: timeout' \
  --reject 'ASSERT'

echo "== scenario 3: no GOP, no config file"
python3 "$ROOT/Scripts/stage-esp.py" -o "$SMOKE/esp-bare" "$ROOT/UniXNUPkg/Test/Qemu/empty" \
  --put "$OUT/UniXNU.efi=EFI/BOOT/BOOTAA64.EFI"
python3 "$ROOT/Scripts/qemu-run.py" "$SMOKE/esp-bare" --no-gop --timeout 40 \
  --log "$SMOKE/degraded.log" \
  --expect 'UNIXNU: config: none found, using built-in configuration' \
  --expect 'UNIXNU: config: no boot entry found' \
  --expect 'UNIXNU: ui: text console' \
  --expect "UNIXNU: menu: shown 3 entries, default 0 'exit'" \
  --expect 'UNIXNU: menu: timeout, booting default 0' \
  --expect 'UNIXNU: action: exit' \
  --reject 'ASSERT'

echo "== scenario 4: config on a partition head, tail as a block slice"
FAT_PYTHON=${FAT_PYTHON:-python3}
if command -v mcopy >/dev/null || "$FAT_PYTHON" -W ignore -c 'import pyfatfs.PyFatFS' 2>/dev/null; then
  python3 "$ROOT/Scripts/stage-esp.py" -o "$SMOKE/userdata-tree" "$ROOT/UniXNUPkg/Test/Qemu/userdata" \
    --put "$OUT/HelloPayload.efi=EFI/UniXNU/Payloads/HelloPayload.efi" >/dev/null
  "$FAT_PYTHON" "$ROOT/Scripts/mk-gpt-disk.py" --out "$SMOKE/gpt-disk.img" --tree "$SMOKE/userdata-tree" \
    --size 256 --fat 64 --name userdata
  python3 "$ROOT/Scripts/qemu-run.py" "$SMOKE/esp-bare" --disk "$SMOKE/gpt-disk.img" \
    --log "$SMOKE/slice.log" \
    --expect 'UNIXNU: config: loaded' \
    --expect "UNIXNU: slice passthrough: partition 'userdata' blocks 131072\\+[0-9]+ \\(512-byte\\), read-only" \
    --expect "UNIXNU-HELLO: check slice PASS \\('passthrough' [0-9]+ blocks ro, block 0 'UNIXNU-TAIL-MARKER'" \
    --expect 'UNIXNU-HELLO: RESULT PASS 1 passed 0 failed' \
    --reject 'ASSERT'
else
  echo "   skipped: needs mtools or FAT_PYTHON with pyfatfs"
fi

echo "== scenario 5: synthetic kext staging and XNU protocol compatibility"
python3 "$ROOT/Scripts/kext-fixtures.py" "$SMOKE/kext" "$OUT"
for variant in v2 v1 v1-empty missing-loader unknown nullboot missing-file bad-image truncated-command empty-plist duplicate bad-path bad-type wrong-entry; do
  expected='UNIXNU: kext: invalid entry configuration; boot refused'
  reject='UNIXNU-XNU-TEST: .* handoff'
  case "$variant" in
    v2) expected='UNIXNU-XNU-TEST: v2 handoff PASS call 1'; reject='UNIXNU-XNU-TEST: .* FAIL' ;;
    v1) expected='UNIXNU: xnu: loader revision 1 cannot accept kexts' ;;
    v1-empty) expected='UNIXNU-XNU-TEST: v1 handoff PASS call 1'; reject='UNIXNU-XNU-TEST: .* FAIL' ;;
    missing-loader) expected='UNIXNU: xnu: no loader protocol installed' ;;
    unknown|nullboot) expected='UNIXNU: xnu: unsupported loader revision or missing Boot callback' ;;
    missing-file) expected='UNIXNU: kext .*Code.kext: cannot read executable' ;;
    bad-image|truncated-command) expected='UNIXNU: kext .*Code.kext: invalid AArch64 MH_KEXT_BUNDLE; boot refused' ;;
    empty-plist) expected='UNIXNU: kext .*Code.kext: empty plist; boot refused' ;;
    duplicate) expected='UNIXNU: kext .*code.kext: duplicate bundle; boot refused' ;;
  esac
  python3 "$ROOT/Scripts/qemu-run.py" "$SMOKE/kext/$variant" --timeout "$KEXT_TIMEOUT" \
    --cpus 1 --finish-on-expect \
    --log "$SMOKE/kext-$variant.log" --expect "$expected" --reject "$reject" --reject 'ASSERT'
done

echo "== scenario 6: portrait and landscape layout by aspect ratio"
python3 "$ROOT/Scripts/qemu-run.py" "$ESP" --display-size 1080x2400 --finish-on-expect \
  --log "$SMOKE/portrait.log" --screenshot "$SMOKE/portrait.png" \
  --expect 'UNIXNU: ui: graphics 1080x2400, scale 4, portrait layout' \
  --expect 'UNIXNU: menu: shown 4 entries' \
  --reject 'ASSERT'
python3 "$ROOT/Scripts/qemu-run.py" "$ESP" --display-size 800x1000 --finish-on-expect \
  --log "$SMOKE/threshold.log" \
  --expect 'UNIXNU: ui: graphics 800x1000, scale 1, landscape layout' \
  --reject 'ASSERT'

echo "== smoke test passed; logs and menu.png in $SMOKE"
