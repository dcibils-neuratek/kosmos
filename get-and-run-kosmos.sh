#!/bin/sh
#
# Downloads the newest Kosmos image and runs it.
#
# **Nothing to download first**, which is the point of it:
#
#   sh -c "$(curl -fsSL https://raw.githubusercontent.com/\
#            dcibils-neuratek/kosmos/main/get-and-run-kosmos.sh)" -- -b "wm"
#
# `sh -c "$(...)"` rather than `curl | sh`, and the difference is not style.
# A pipe *is* this script's stdin, and the last thing it does is hand over to
# QEMU with `-serial mon:stdio` - so the guest's console would be reading
# from an exhausted pipe and see end-of-file the moment it started. With
# `-c` the terminal is still the terminal.
#
# Or keep a copy, which is quicker to type afterwards:
#
#   ./get-and-run-kosmos.sh                  the newest one, and boot it
#   ./get-and-run-kosmos.sh -b "wm browser"  ...starting the browser
#   ./get-and-run-kosmos.sh --plain          the small image, no browser
#   ./get-and-run-kosmos.sh --list           what is published
#   ./get-and-run-kosmos.sh --dir ~/kosmos   where to keep what it fetches
#   ./get-and-run-kosmos.sh --update         fetch a newer copy of this file
#
# Anything it does not recognise is passed to `run-kosmos.sh`, so `-serial`
# and `-b` work here exactly as they do there.
#
# **This is the only file you need.** It fetches the image and the runner
# beside it, so there is nothing to clone and nothing to build. What it
# cannot fetch is QEMU: `brew install qemu` on macOS.
#
# **Nothing here knows a file name.** A release is called something like
# `kosmos-0.8.33-f82c43e-1280x800-web.elf` and the version and the commit in
# that change every time, so the list is asked for and the newest is worked
# out - the same rule `run-kosmos.sh` uses on files already downloaded:
# newest version first, then the largest build of that version.
#
# Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.

set -eu

REPO="dcibils-neuratek/kosmos"
BRANCH="main"
API="https://api.github.com/repos/$REPO/contents/builds"
RAW_ROOT="https://raw.githubusercontent.com/$REPO/$BRANCH"
RAW="$RAW_ROOT/builds"

dir="$HOME/.kosmos"
want="richest"
mode="run"
passthrough=""
want_dir="no"

for arg in "$@"; do
    if [ "$want_dir" = "yes" ]; then
        dir="$arg"
        want_dir="no"
        continue
    fi

    case "$arg" in
        --plain) want="plain" ;;
        --web|--full) want="richest" ;;
        --list)  mode="list" ;;
        --update) mode="update" ;;
        --dir)   want_dir="yes" ;;
        --help|-h)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            # Everything else belongs to run-kosmos.sh. Quoted so that
            # `-b "wm browser"` survives as one argument.
            passthrough="$passthrough '$(printf '%s' "$arg" | sed "s/'/'\\\\''/g")'"
            ;;
    esac
done

if ! command -v curl >/dev/null 2>&1; then
    echo "curl is not on PATH, and this needs it to fetch anything." >&2
    exit 1
fi

#
# What is published.
#
# The GitHub contents API rather than a hardcoded list, because the point of
# this script is that nobody has to know what the files are called. It needs
# no token: the repository is public.
#
#
# Whether this file is the published one.
#
# It fetches the image and `run-kosmos.sh` every time and never looked at
# *itself*, so a bug in here was permanent for anybody holding an old copy -
# which is exactly what happened: this looked for a name the release had
# stopped using, was fixed, and went on failing for the one person running
# it, because the fix was in a file they already had.
#
# It says rather than replacing. A script that silently overwrites the file
# it is being executed from is a surprise, and on some shells it is a
# corrupted read as well.
#
#
# There may be no file to compare: run through `sh -c "$(curl ...)"` there
# is no copy of this anywhere, which is the arrangement that cannot go
# stale in the first place. `$0` is then the shell, so the marker is what
# tells a real copy of this script from a file that happens to be there.
#
me="$0"
have_copy="no"

if [ -r "$me" ] && grep -q "^# Kosmos\. Copyright" "$me" 2>/dev/null; then
    have_copy="yes"
fi

if [ "$have_copy" = "no" ]; then
    if [ "$mode" = "update" ]; then
        echo "there is no local copy to update: this came off the network." >&2
        exit 0
    fi

    published=""
else
    published=$(curl -fsSL "$RAW_ROOT/get-and-run-kosmos.sh" 2>/dev/null || true)
fi

if [ -n "$published" ]; then
    mine_sum=$(cksum < "$me" | cut -d" " -f1,2)
    theirs_sum=$(printf '%s\n' "$published" | cksum | cut -d" " -f1,2)

    if [ "$mine_sum" != "$theirs_sum" ]; then
        if [ "$mode" = "update" ]; then
            printf '%s\n' "$published" > "$me.part"
            chmod +x "$me.part"
            mv "$me.part" "$me"
            echo "updated $me"
            exit 0
        fi

        # "differs", not "is older": a checksum knows they are not the
        # same and nothing about which came first. Claiming newer would be
        # wrong for anybody who has edited their copy, which is a thing a
        # shell script invites.
        echo "note: this differs from the published $(basename "$me")." >&2
        echo "      $me --update   takes the published one" >&2
        echo >&2
    elif [ "$mode" = "update" ]; then
        echo "$(basename "$me") is already the published one."
        exit 0
    fi
elif [ "$mode" = "update" ]; then
    echo "could not fetch a copy to compare against." >&2
    exit 1
fi

listing=$(curl -fsSL "$API" 2>/dev/null) || {
    echo "could not reach $API" >&2
    echo "Either there is no network here, or the repository moved." >&2
    exit 1
}

# `name size` a line, for the image files only. GitHub prints `name` before
# `size` in every entry, which is what lets one awk pass pair them.
images=$(printf '%s\n' "$listing" | awk '
    /"name":/ { n = $0; sub(/.*"name": *"/, "", n); sub(/".*/, "", n) }
    /"size":/ {
        if (n ~ /^kosmos-.*\.elf$/) {
            s = $0; gsub(/[^0-9]/, "", s); print n, s
        }
        n = ""
    }')

if [ -z "$images" ]; then
    echo "the repository published no images." >&2
    exit 1
fi

if [ "$mode" = "list" ]; then
    echo "published in $REPO:"
    printf '%s\n' "$images" | while read -r name size; do
        case "$name" in
            *-full.elf) has="browser, Doom" ;;
            *-web.elf)  has="browser" ;;
            *-doom.elf) has="Doom" ;;
            *)          has="the desktop" ;;
        esac

        printf '  %-44s %4s MB   %s\n' "$name" "$((size / 1048576))" "$has"
    done
    exit 0
fi

#
# The newest, and the largest of that version.
#
# Not the largest outright, which would pick a months-old 1920x1080 over the
# current one for having more pixels in it. Not by date, because a published
# file's date is when it was published rather than what it contains. And not
# lexicographically, because `0.10.0` sorts before `0.9.0`.
#
best=""
best_size=0
best_rank=-1
best_carries=-1
best_px=0

printf '%s\n' "$images" > "${TMPDIR:-/tmp}/kosmos-images.$$"

while read -r name size; do
    #
    # How much is in it, from the suffix the release put there.
    #
    # This used to test `kind = want` with `want` fixed at "web", and the
    # release stopped saying `-web` the day it started carrying Doom as
    # well. So `--list` showed an image and the downloader said none was
    # published - a matcher that had to be taught every name the build might
    # invent, and was not.
    #
    # Ranked rather than matched, so a suffix nobody here has heard of is
    # simply an image like any other. And a preference is a preference: if
    # the wanted kind is absent, the best of what there *is* gets used
    # rather than nothing. Refusing to run beside a working image is not a
    # useful thing for a downloader to do.
    #
    case "$name" in
        *-full.elf) carries=3 ;;
        *-web.elf)  carries=2 ;;
        *-doom.elf) carries=1 ;;
        *)          carries=0 ;;
    esac

    # `--plain` wants the smallest, everything else the most complete.
    [ "$want" = "plain" ] && carries=$(( 3 - carries ))

    dims=$(printf '%s' "$name" \
           | sed -n 's/.*-\([0-9][0-9]*\)x\([0-9][0-9]*\).*/\1 \2/p')
    [ -n "$dims" ] || continue

    ver=$(printf '%s' "$name" \
          | sed -n 's/^kosmos-\([0-9][0-9]*\)\.\([0-9][0-9]*\)\.\([0-9][0-9]*\)-.*/\1 \2 \3/p')
    [ -n "$ver" ] || ver="0 0 0"

    v_rest=${ver#* }
    rank=$(( ${ver%% *} * 1000000 + ${v_rest%% *} * 1000 + ${v_rest##* } ))
    px=$(( ${dims% *} * ${dims#* } ))

    # Newest, then most complete, then largest.
    if [ "$rank" -gt "$best_rank" ] \
       || { [ "$rank" -eq "$best_rank" ] && [ "$carries" -gt "$best_carries" ]; } \
       || { [ "$rank" -eq "$best_rank" ] && [ "$carries" -eq "$best_carries" ] \
            && [ "$px" -ge "$best_px" ]; }
    then
        best_rank="$rank"
        best_carries="$carries"
        best_px="$px"
        best="$name"
        best_size="$size"
    fi
done < "${TMPDIR:-/tmp}/kosmos-images.$$"

rm -f "${TMPDIR:-/tmp}/kosmos-images.$$"

if [ -z "$best" ]; then
    echo "nothing published here looks like an image. Try --list." >&2
    exit 1
fi

#
# Said, when what was picked is not what was asked for.
#
# Version outranks completeness on purpose: a `--plain` that fetched a
# months-old image to honour a size preference would be answering the wrong
# question, and nobody wants a stale operating system. But doing the
# sensible thing quietly is how a script gets blamed for something else, so
# it says which preference it could not meet and what it did instead.
#
case "$best" in
    *-full.elf|*-web.elf|*-doom.elf) got="richest" ;;
    *)                               got="plain"   ;;
esac

if [ "$want" = "plain" ] && [ "$got" != "plain" ]; then
    echo "no lean image at this version, so this is the full one." >&2
elif [ "$want" = "richest" ] && [ "$got" = "plain" ]; then
    echo "the newest image here carries no browser." >&2
fi

mkdir -p "$dir"

#
# Fetched only if it is not already here and the right length.
#
# The length is the check rather than a checksum because it is the one the
# listing already carries, and what it is really guarding against is a
# half-finished download or an error page saved under the name of an image.
# The magic number below catches the rest of that.
#
if [ -f "$dir/$best" ] && [ "$(wc -c < "$dir/$best" | tr -d ' ')" = "$best_size" ]
then
    echo "have $best"
else
    echo "fetching $best ($((best_size / 1048576)) MB)"
    curl -fL --progress-bar -o "$dir/$best.part" "$RAW/$best"
    mv "$dir/$best.part" "$dir/$best"
fi

# An ELF starts with 0x7f E L F. An error page saved under this name does
# not, and would otherwise fail much later and much less clearly.
magic=$(dd if="$dir/$best" bs=4 count=1 2>/dev/null | od -An -c | tr -d ' \n')

if [ "$magic" != "177ELF" ]; then
    echo "$dir/$best is not an ELF - the download went wrong." >&2
    rm -f "$dir/$best"
    exit 1
fi

# And the runner, which is small and changes with the images.
curl -fsSL -o "$dir/run-kosmos.sh" "$RAW/run-kosmos.sh" || {
    echo "could not fetch run-kosmos.sh" >&2
    exit 1
}

chmod +x "$dir/run-kosmos.sh"

echo "running $dir/$best"
echo

# `eval` so that a quoted `-b "wm browser"` arrives as one argument. The
# quoting was done when the arguments were collected, above.
eval exec "$dir/run-kosmos.sh" "$passthrough" '"$dir/$best"'
