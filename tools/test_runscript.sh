#!/bin/sh
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
#  run-kosmos.sh, held to the QEMU command line it builds.
#
#  It is the one file that travels beside a released image, and nothing ran
#  it: `-b "wm blocks"`, its own documented example, reached QEMU split in
#  two for months, because every single-word `-b` worked. So a stand-in
#  `qemu-system-aarch64` on the PATH prints each argument it is handed on a
#  line of its own, in brackets, and this reads the lines.
#
#  Run by `make host-check`; takes well under a second.
#
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir "$work/bin"
cat > "$work/bin/qemu-system-aarch64" <<'QEMU'
#!/bin/sh
for a in "$@"; do printf '[%s]\n' "$a"; done
QEMU
chmod +x "$work/bin/qemu-system-aarch64"
: > "$work/kosmos-test.elf"

checks=0
fails=0

check() {
    if [ "$1" = "yes" ]; then
        checks=$((checks + 1))
    else
        fails=$((fails + 1))
        echo "  not ok: $2"
    fi
}

# The script with the image named first, so an option left without its
# word is left without it rather than taking the image's path.
run() {
    PATH="$work/bin:$PATH" DISK_MB=1 \
        sh "$here/run-kosmos.sh" "$work/kosmos-test.elf" "$@" 2>"$work/err"
}

has() {
    if grep -qxF -- "[$1]" "$work/out"; then echo yes; else echo no; fi
}

# A plain run: the two flags nobody could guess, and the image last.
run > "$work/out"
check "$(has virtio-mmio.force-legacy=false)" \
      "the modern virtio-mmio interface, or there is no keyboard"
check "$(has virtio-tablet-device)" "an absolute pointer"
check "$(if tail -2 "$work/out" | head -1 | grep -qxF '[-kernel]' &&
            tail -1 "$work/out" | grep -qxF "[$work/kosmos-test.elf]"
         then echo yes; else echo no; fi)" "the image, last"
check "$(if grep -q 'opt/kosmos/camera' "$work/out"; then echo no
         else echo yes; fi)" "no camera unless one is asked for"

# `-b` with a space in it is one argument, not two.
run -b "wm blocks" > "$work/out"
check "$(has 'name=opt/kosmos/boot,string=wm blocks')" \
      "-b \"wm blocks\" reaches QEMU as one argument"

# `-camera pattern`: the driver's test pattern, through fw_cfg.
run -camera pattern > "$work/out"
check "$(if grep -A1 -xF -- '[-fw_cfg]' "$work/out" |
            grep -qxF '[name=opt/kosmos/camera,string=pattern]'
         then echo yes; else echo no; fi)" \
      "-camera pattern asks the driver for its test pattern"

# Anything else is refused, and QEMU is not started.
status=0
run -camera 046d:08e5 > "$work/out" || status=$?
check "$(if [ "$status" = 2 ] && [ ! -s "$work/out" ]; then echo yes
         else echo no; fi)" "-camera with a USB camera's numbers is refused"
check "$(if grep -q 'tools/camera.sh' "$work/err"; then echo yes
         else echo no; fi)" "and says where a real camera is"

status=0
run -camera > "$work/out" || status=$?
check "$(if [ "$status" = 2 ] && [ ! -s "$work/out" ]; then echo yes
         else echo no; fi)" "-camera with nothing after it is refused"

if [ "$fails" -ne 0 ]; then
    echo "FAIL: $fails of $((checks + fails)) checks on run-kosmos.sh"
    exit 1
fi

echo "PASS: $checks checks on run-kosmos.sh (the command line it gives QEMU:" \
     "the flags nobody guesses, -b as one argument, -camera pattern)"
