#!/bin/sh
#
# Runs Kosmos under QEMU on macOS.
#
#   ./run-kosmos.sh                 a window, with the shell on this terminal
#   ./run-kosmos.sh -r 1920x1080    at that size, if a build of it is here
#   ./run-kosmos.sh -b wm           straight to the desktop
#   ./run-kosmos.sh -b "wm blocks"  with something on it
#   ./run-kosmos.sh -fit            scale the window down to fit this screen
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
#   -drive ... -device virtio-blk-device
#       A disk, made here if it is not here already: 64 MB of zeros, which
#       is what `make` does for a build tree. Without one there is no
#       filesystem at all - `/home` is an empty ramfs that forgets
#       everything at power off - so Doom cannot find a WAD, nothing can be
#       saved, and Tracker has nowhere to look. An operating system with no
#       disk is a demo.
#
#   -netdev user -device virtio-net-device
#       QEMU's own NAT, which needs no privileges and puts no packet on a
#       real network. The guest is 10.0.2.15 and **this computer is
#       10.0.2.2**, which is the whole of what `ping`, `fetch` and the
#       browser need. Without these two lines they all work and find
#       nothing, which looks like a broken network stack.
#
# The disk is `kosmos.img` beside the image, kept between runs, and made when
# it is missing. It arrives as 64 MB of zeros, and the system formats a blank
# disk the first time something asks it for a file - a disk of all zeros has
# nothing to lose, and making somebody type `mkfs` before the machine will
# keep a file is a ceremony over an empty box. A disk with something on it
# that is not a filesystem this understands is left alone and said so.
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
fit="no"
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
        -fit)    fit="yes" ;;
        -r)      want_size="yes" ;;
        -b)      want_boot="yes" ;;
        -*)      echo "unknown option: $arg" >&2; exit 2 ;;
        *)       image="$arg" ;;
    esac
done

# `-r list`: what sizes are available here.
if [ "$size" = "list" ]; then
    echo "images beside this script:"
    for f in "$here"/kosmos-*.elf "$here"/builds/kosmos-*.elf \
             "$here"/build/kosmos-*.elf; do
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
        for dir in "$here" "$here/builds" "$here/build" "build"; do
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
        #
        # The largest image here, which is what this script has claimed to
        # do since it was written and did not.
        #
        # It looked only for `kosmos.elf` and `build/kosmos.elf` - the names
        # a *build tree* has. A released image is called
        # `kosmos-0.8.33-f82c43e-1280x800-web.elf`, and the version and the
        # commit in that name change every time, which is the whole reason
        # nothing here may hardcode one. So none of the files anybody
        # actually downloads were ever found, and `./run-kosmos.sh` on a
        # machine holding three of them said there was no image.
        #
        # **Newest version first, then largest of that version.** Not
        # largest outright, which was the first attempt and picked a
        # months-old 1920x1080 image over the current one because it had
        # more pixels in it. Nobody wants the biggest old thing.
        #
        # Not by date either: a copied file's date is when it was copied,
        # and these are made to be copied. Not lexicographically, because
        # `0.10.0` sorts before `0.9.0` and this project is at 0.8 - a trap
        # with a date on it rather than a hypothetical one.
        #
        # `-ge` rather than `-gt` so that of two identical rankings the
        # later name wins, which puts a `-web` image ahead of the plain one
        # it sorts after. That is the right way round: it can do everything
        # the plain one can.
        #
        best_rank=-1
        best_px=0

        # `build` as well as `builds`, which is not a typo either way:
        # `builds/` is where releases are kept in the repository and
        # `build/` is where a build tree puts things - and somebody who
        # downloads one image and drops it next to this script may put it in
        # either, or in neither. All three are cheap to look in.
        for f in "$here"/kosmos-*.elf \
                 "$here"/builds/kosmos-*.elf \
                 "$here"/build/kosmos-*.elf; do
            [ -f "$f" ] || continue

            name=$(basename "$f")

            dims=$(echo "$name" \
                   | sed -n 's/.*-\([0-9][0-9]*\)x\([0-9][0-9]*\).*/\1 \2/p')
            [ -n "$dims" ] || continue

            ver=$(echo "$name" \
                  | sed -n 's/^kosmos-\([0-9][0-9]*\)\.\([0-9][0-9]*\)\.\([0-9][0-9]*\)-.*/\1 \2 \3/p')
            [ -n "$ver" ] || ver="0 0 0"

            # Expansion rather than `set --`, which would overwrite this
            # script's own arguments from inside a loop that is reading a
            # filename. They are not needed by this point, which is not a
            # reason to destroy them.
            v_rest=${ver#* }
            rank=$(( ${ver%% *} * 1000000
                     + ${v_rest%% *} * 1000
                     + ${v_rest##* } ))
            px=$(( ${dims% *} * ${dims#* } ))

            if [ "$rank" -gt "$best_rank" ] \
               || { [ "$rank" -eq "$best_rank" ] && [ "$px" -ge "$best_px" ]; }
            then
                best_rank="$rank"
                best_px="$px"
                image="$f"
            fi
        done

        # A build tree, if there are no released images beside the script.
        if [ -z "$image" ]; then
            for candidate in "$here/build/kosmos.elf" "build/kosmos.elf" \
                             "$here/kosmos.elf"; do
                [ -f "$candidate" ] && image="$candidate" && break
            done
        fi
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
             "$here"/build/kosmos-*.elf build/kosmos.elf; do
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

#
# The disk, beside the image, made once and kept.
#
# `make` has always done this for a build tree - `dd` 64 MB of zeros - and
# a released image had no disk at all, so everything that needs a
# filesystem failed on a machine that had only downloaded a binary. Doom
# saying it cannot find `/home/doom1.wad` is the polite version; the rest
# is just files that do not persist.
#
# Never overwritten. A run that silently reformatted the disk would be a
# run that eats whatever was on it, which is the one thing this must not do.
#
disk="$(dirname "$image")/kosmos.img"

if [ ! -f "$disk" ]; then
    echo "making $disk (${DISK_MB:-64} MB, empty)" >&2

    if ! dd if=/dev/zero of="$disk" bs=1m count="${DISK_MB:-64}" 2>/dev/null
    then
        echo "could not make a disk at $disk - carrying on without one." >&2
        disk=""
    else
        echo "  it is blank, and the system formats a blank disk itself." >&2
    fi
fi

#
# The command line, built as arguments rather than as a string.
#
# It used to be a string - `bootargs="-fw_cfg name=...,string=$boot"` -
# expanded unquoted, and a shell splits that on spaces. So `-b "wm blocks"`,
# which is this script's own documented example, reached QEMU as
# `string=wm` followed by a stray `blocks`: the desktop started and the
# thing you asked for did not, with nothing said. Every single-word `-b`
# worked, which is why it survived.
#
# `set --` is how a POSIX shell holds a list. There are no arrays here and a
# string is not a substitute for one.
#
set -- -M virt,gic-version=3 -cpu cortex-a72 -m 512M

# QEMU's own NAT: no privileges, no packet on a real network, and this
# computer is 10.0.2.2 from inside. `ping`, `fetch` and the browser all need
# it, and all fail quietly and confusingly without it.
set -- "$@" -netdev user,id=net0 -device virtio-net-device,netdev=net0

if [ -n "$disk" ]; then
    set -- "$@" -drive "file=$disk,format=raw,if=none,id=disk" \
                -device virtio-blk-device,drive=disk
fi

if [ "$serial_only" = "yes" ]; then
    set -- "$@" -nographic
else
    # -display default rather than cocoa, so this works over ssh with X or
    # on a machine whose QEMU was built without the cocoa backend.
    #
    # The display size is *compiled in* - the framebuffer is a static array,
    # so a different size is a different build and `-r` is how you pick one.
    # `-fit` is the escape hatch for when the only image you have is bigger
    # than the screen you have: QEMU scales the window instead. It is lossy
    # on a interface drawn with one-pixel bevels, which is why it is a flag
    # and not the default.
    #
    # macOS only, because `zoom-to-fit` is an option of the cocoa backend
    # and passing it to another one is an error rather than an ignored
    # request.
    #
    display="default"

    if [ "$fit" = "yes" ]; then
        if [ "$(uname -s)" = "Darwin" ]; then
            display="cocoa,zoom-to-fit=on"
        else
            echo "-fit needs the cocoa display, which is macOS only." >&2
            echo "Pick a build that fits instead: $0 -r list" >&2
        fi
    fi

    set -- "$@" -global virtio-mmio.force-legacy=false \
                -device ramfb \
                -device virtio-keyboard-device \
                -device virtio-tablet-device \
                -display "$display" -serial mon:stdio
fi

# What to run once it is up, through fw_cfg - which is how a machine is told
# what to do without being rebuilt.
if [ -n "$boot" ]; then
    set -- "$@" -fw_cfg "name=opt/kosmos/boot,string=$boot"
fi

exec qemu-system-aarch64 "$@" -kernel "$image"
