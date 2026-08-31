#!/bin/sh
#
# Runs Kosmos under QEMU on macOS.
#
#   ./run-kosmos.sh                 a window, with the shell on this terminal
#   ./run-kosmos.sh -r 1920x1080    at that size, if a build of it is here
#   ./run-kosmos.sh -serial         no window, serial only
#   ./run-kosmos.sh path.elf        a particular image
#
# The display size is baked into the image: the framebuffer is a static
# array, so a different size is a different build. `-r` picks the one that
# was built for it. `./run-kosmos.sh -r list` says which are here.
#
# In the window, type `wm` for the desktop. Control-C gives the screen back
# to the shell; Control-A then X quits QEMU.
#
# The image is self-contained - the userland, the interpreter, every program
# and the font are inside it - so this script and one .elf are the whole of
# what has to travel. What is worth carrying is the command line, because
# two of these flags are not guessable and the system is quietly diminished
# without them:
#
#   -global virtio-mmio.force-legacy=false
#       QEMU's virtio-mmio devices default to the *legacy* interface, which
#       this system's driver correctly refuses. Without this line there is
#       no keyboard and no pointer, the boot log says so, and everything
#       still works over the serial line - which makes it look like a
#       Kosmos bug rather than a missing flag.
#
#   -device virtio-tablet-device
#       An absolute pointing device rather than a relative one. Absolute is
#       the right kind for a virtual machine: there is no acceleration curve
#       to agree on with the host, so the guest cursor cannot drift away
#       from the real one.
#
set -eu

here=$(dirname "$0")
image=""
serial_only="no"
size=""
want_size="no"

for arg in "$@"; do
    if [ "$want_size" = "yes" ]; then
        size="$arg"
        want_size="no"
        continue
    fi

    case "$arg" in
        -serial) serial_only="yes" ;;
        -r)      want_size="yes" ;;
        -*)      echo "unknown option: $arg" >&2; exit 2 ;;
        *)       image="$arg" ;;
    esac
done

# `-r list`: what sizes are available here.
if [ "$size" = "list" ]; then
    echo "images beside this script:"
    for f in "$here"/kosmos-*.elf "$here"/builds/kosmos-*.elf; do
        [ -f "$f" ] && echo "  $(basename "$f")"
    done
    exit 0
fi

if [ -z "$image" ]; then
    if [ -n "$size" ]; then
        # The newest build for that size, wherever the images are.
        for dir in "$here" "$here/builds" "build"; do
            for f in "$dir"/kosmos-*-"$size".elf; do
                [ -f "$f" ] && image="$f"
            done
        done

        if [ -z "$image" ]; then
            echo "no image built for $size." >&2
            echo "Available:" >&2
            "$0" -r list >&2
            echo "Build one with: make FB=$size" >&2
            exit 1
        fi
    else
        for candidate in "$here/build/kosmos.elf" "build/kosmos.elf" \
                         "$here/kosmos.elf"; do
            [ -f "$candidate" ] && image="$candidate" && break
        done
    fi
fi

if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    echo "qemu-system-aarch64 is not on PATH." >&2
    echo "On macOS: brew install qemu" >&2
    exit 1
fi

if [ -z "$image" ] || [ ! -f "$image" ]; then
    echo "no image at $image" >&2
    echo "Build one with \`make\`, or pass a path: ./run-kosmos.sh kosmos.elf" >&2
    exit 1
fi

if [ "$serial_only" = "yes" ]; then
    exec qemu-system-aarch64 \
        -M virt,gic-version=3 -cpu cortex-a72 -m 512M \
        -nographic \
        -kernel "$image"
fi

# -display default rather than cocoa, so this works over ssh with X or on a
# machine whose QEMU was built without the cocoa backend. QEMU picks.
exec qemu-system-aarch64 \
    -M virt,gic-version=3 -cpu cortex-a72 -m 512M \
    -global virtio-mmio.force-legacy=false \
    -device ramfb \
    -device virtio-keyboard-device \
    -device virtio-tablet-device \
    -display default -serial mon:stdio \
    -kernel "$image"
