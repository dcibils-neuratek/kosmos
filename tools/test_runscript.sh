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
check "$(has virtio-sound-device,audiodev=snd0)" \
      "a sound card, or Music, Groove and Doom have no device"
check "$(if [ "$(uname -s)" != Darwin ] || grep -qxF '[coreaudio,id=snd0]' "$work/out"
         then echo yes; else echo no; fi)" "out through the Mac's own output"

# And none when asked for none.
run -nosound > "$work/out"
check "$(if grep -q 'virtio-sound' "$work/out"; then echo no
         else echo yes; fi)" "-nosound leaves the sound card out"

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

# `-r`: the screen's size, handed to the machine rather than chosen as a
# file (`roadmap.md` 6zt) - so an image of another size is used at this one.
run -r 3840x2160 > "$work/out"
check "$(if grep -A1 -xF -- '[-fw_cfg]' "$work/out" |
            grep -qxF '[name=opt/kosmos/fb,string=3840x2160]'
         then echo yes; else echo no; fi)" \
      "-r 3840x2160 asks the machine for a screen that size"
check "$(has 1G)" "a screen that large gets a gigabyte"
check "$(if tail -1 "$work/out" | grep -qxF "[$work/kosmos-test.elf]"
         then echo yes; else echo no; fi)" "and the image it was given, at that size"

run -r 3840x2160 -m 768M > "$work/out"
check "$(if [ "$(has 768M)" = yes ] && [ "$(has 1G)" = no ]; then echo yes
         else echo no; fi)" "-m says the memory, whatever the size"

run -r 1280x720 > "$work/out"
check "$(has 512M)" "a small screen keeps 512 MB"

status=0
run -r banana > "$work/out" || status=$?
check "$(if [ "$status" = 2 ] && [ ! -s "$work/out" ]; then echo yes
         else echo no; fi)" "-r with no size is refused"

# **Diego's own command**, 28 September: a released 1920x1080 image beside
# a copy of the script, started with `sh`, asked for 3840x2160. It said "no
# image built for 3840x2160" and then "command not found" from running
# itself as "$0". Now that image, at that size.
mkdir "$work/downloads"
: > "$work/downloads/kosmos-0.10.186-abcdef0-1920x1080-full.elf"
cp "$here/run-kosmos.sh" "$work/downloads/run-kosmos-2.sh"
(cd "$work/downloads" && PATH="$work/bin:$PATH" DISK_MB=1 \
    sh run-kosmos-2.sh -r 3840x2160 -fit -b wm -camera pattern \
    > "$work/out" 2> "$work/err") || true
check "$(if grep -qxF '[name=opt/kosmos/fb,string=3840x2160]' "$work/out" &&
            tail -1 "$work/out" | grep -q 'kosmos-0.10.186-abcdef0-1920x1080-full.elf' &&
            ! grep -q 'not found' "$work/err"
         then echo yes; else echo no; fi)" \
      "Diego's command runs the 1920x1080 image at 3840x2160"

if [ "$fails" -ne 0 ]; then
    echo "FAIL: $fails of $((checks + fails)) checks on run-kosmos.sh"
    exit 1
fi

echo "PASS: $checks checks on run-kosmos.sh (the command line it gives QEMU:" \
     "the flags nobody guesses, a sound card, -b as one argument, -camera pattern," \
     "-r as the screen's size with memory to match, and Diego's own command)"
