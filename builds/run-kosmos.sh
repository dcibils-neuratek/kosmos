#!/bin/sh
#
# Runs Kosmos under QEMU on macOS.
#
#   ./run-kosmos.sh                 a window, with the shell on this terminal
#   ./run-kosmos.sh -r 1920x1080    at that size, if a build of it is here
#   ./run-kosmos.sh -b wm           straight to the desktop
#   ./run-kosmos.sh -b "wm blocks"  with something on it
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
#   -netdev user -device virtio-net-device
#       QEMU's own NAT, which needs no privileges and puts no packet on a
#       real network. The guest is 10.0.2.15 and **this computer is
#       10.0.2.2**, which is the whole of what `ping`, `fetch` and the
#       browser need. Without these two lines they all work and find
#       nothing, which looks like a broken network stack.
#
# **To browse something**, serve a directory here and ask for it there:
#
#   python3 -m http.server 8000          (on this computer)
#   ./run-kosmos.sh -b "wm browser:10.0.2.2:8000/"
#
# There is no DNS, so an address is four numbers. That is a missing resolver
# rather than a missing browser, and `ping` says the same thing.
#
set -eu

here=$(dirname "$0")
image=""
serial_only="no"
size=""
want_size="no"

# What to start once it is up. Empty means the shell.
#
#   ./run-kosmos.sh -b wm            straight to the desktop
#   ./run-kosmos.sh -b "wm blocks"   with something on it
boot=""
want_boot="no"

for arg in "$@"; do
    if [ "$want_size" = "yes" ]; then
        size="$arg"
        want_size="no"
        continue
    fi

    if [ "$want_boot" = "yes" ]; then
        boot="$arg"
        want_boot="no"
        continue
    fi

    case "$arg" in
        -serial) serial_only="yes" ;;
        -r)      want_size="yes" ;;
        -b)      want_boot="yes" ;;
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
        #
        # The trailing `*` is not decoration: an image carrying the browser
        # is named `...-1280x800-web.elf`, and a pattern ending in the size
        # matched every ordinary build and none of those. `-r 1280x800`
        # silently found the old small image and said nothing about the one
        # that was actually being looked for.
        for dir in "$here" "$here/builds" "build"; do
            for f in "$dir"/kosmos-*-"$size".elf "$dir"/kosmos-*-"$size"-*.elf; do
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
    echo >&2

    # What is actually here, because "no image at <path>" on its own leaves
    # you guessing whether the file is missing, the name is wrong, or you
    # are in the wrong directory. Three different problems, one message.
    found="no"
    for f in "$here"/kosmos-*.elf "$here"/builds/kosmos-*.elf \
             build/kosmos.elf; do
        if [ -f "$f" ]; then
            [ "$found" = "no" ] && echo "Images I can see:" >&2
            found="yes"
            echo "  $f" >&2
        fi
    done

    if [ "$found" = "no" ]; then
        echo "There are none beside this script or in ./builds." >&2
        echo "Build one with \`make\`, or pass a path:" >&2
        echo "  ./run-kosmos.sh kosmos.elf" >&2
    fi

    exit 1
fi

# Passed through fw_cfg, which is how a machine is told what to do without
# being rebuilt. Built as an array so an empty option adds no arguments at
# all rather than an empty one.
bootargs=""
if [ -n "$boot" ]; then
    bootargs="-fw_cfg name=opt/kosmos/boot,string=$boot"
fi

if [ "$serial_only" = "yes" ]; then
    # shellcheck disable=SC2086
    exec qemu-system-aarch64 \
        -M virt,gic-version=3 -cpu cortex-a72 -m 512M \
        -netdev user,id=net0 \
        -device virtio-net-device,netdev=net0 \
        -nographic $bootargs \
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
    -netdev user,id=net0 \
    -device virtio-net-device,netdev=net0 \
    -display default -serial mon:stdio $bootargs \
    -kernel "$image"
