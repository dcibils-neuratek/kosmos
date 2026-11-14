#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Media Kit on Kosmos (`docs/media.md` M1): FFmpeg 9.0.2 built whole,
with its assembly, and dav1d 1.5.4 under it.

`mediatest.elf` - the kit with `user/kits/mediatest/mediatest.c` - given
films and songs from a /Home of its own: every container and codec the
kit reads, made on this Mac by Homebrew's FFmpeg from its own test
patterns (`SAMPLES`; nothing of anybody's is in the repository), and a
truncated film and a file of noise, which must be refused. What it says, a
line a file and a stream, is held to two references it did not make:

- the same program compiled on this Mac against the same FFmpeg
  (`tools/w5_build.py host`; an x86-64 guest against an Intel build under
  Rosetta, as `x86-formats` is), every line;
- and each picture stream's fingerprint against what Homebrew's own
  `ffmpeg -f framemd5` says of the same file - FFmpeg's tool, built by
  somebody else, deciding the same thing.

Usage: run_mediakit.py IMAGE [APPS]

`KOSMOS_MEDIA_BREAK=<name>` is the control: the references are made from
that file with one byte changed in its middle, and the guest must
disagree with both.
"""

import hashlib
import fcntl
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    ROOT, "build", "user-x86_64" if X86 else "user", "apps")
WORK = scratch.directory("mediakit")
FOLDER = os.path.join(WORK, "films")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
FFMPEG = shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg"
REF = "host-x86_64" if X86 else "host"
ARCH_FLAGS = ["-arch", "x86_64", "-mmacosx-version-min=15.0"] if X86 else []
HOST = os.path.join(ROOT, "build", "w5", REF, "install")
KIT = os.path.join(ROOT, "user", "kits", "media")
TEST = os.path.join(ROOT, "user", "kits", "mediatest")
# Made once and kept: the recipes are the key, so a changed one is made again.
SAMPLES_DIR = os.path.join(ROOT, "build", "media", "samples")

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Media Kit's test (`tools/run_mediakit.py`).
-- kosmos: image mediatest.elf
local kit = use("mediatest.elf")
for _, line in ipairs(kit.run("/Home/apps/media/films")) do
  print(line)
end
"""

PICTURE = ["-f", "lavfi", "-i", "testsrc2=size=320x240:rate=24:duration=2"]
SOUND = ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=2"]
FRAGMENTED = ["-movflags", "frag_keyframe+empty_moov+default_base_moof"]

# (file, the decoder Homebrew's FFmpeg is told to check its pictures with,
#  the recipe). YouTube's own shapes first: AV1 and VP9 with Opus in WebM,
#  H.264 with AAC in MP4, and MP4 in fragments as MSE is handed it.
SAMPLES = [
    ("av1-opus.webm", "libdav1d", PICTURE + SOUND + [
        "-c:v", "libsvtav1", "-preset", "10", "-g", "24", "-c:a", "libopus"]),
    ("vp9-opus.webm", "vp9", PICTURE + SOUND + [
        "-c:v", "libvpx-vp9", "-deadline", "realtime", "-g", "24", "-c:a", "libopus"]),
    ("vp8-vorbis.webm", "vp8", PICTURE + SOUND + [
        "-c:v", "libvpx", "-deadline", "realtime", "-g", "24",
        "-c:a", "vorbis", "-strict", "-2", "-ac", "2"]),
    ("h264-aac.mp4", "h264", PICTURE + SOUND + [
        "-c:v", "libx264", "-preset", "veryfast", "-g", "24", "-c:a", "aac"]),
    ("h264-aac-fragmented.mp4", "h264", PICTURE + SOUND + [
        "-c:v", "libx264", "-preset", "veryfast", "-g", "24", "-c:a", "aac"] + FRAGMENTED),
    ("av1-10bit-fragmented.mp4", "libdav1d", PICTURE + [
        "-c:v", "libsvtav1", "-preset", "10", "-g", "24",
        "-pix_fmt", "yuv420p10le"] + FRAGMENTED),
    ("vp9-444.webm", "vp9", PICTURE + [
        "-c:v", "libvpx-vp9", "-deadline", "realtime", "-g", "24", "-pix_fmt", "yuv444p"]),
    ("opus.ogg", None, SOUND + ["-c:a", "libopus"]),
    ("sine.flac", None, SOUND + ["-c:a", "flac"]),
    ("sine.mp3", None, SOUND + ["-c:a", "libmp3lame", "-b:a", "128k"]),
    ("sine.aac", None, SOUND + ["-c:a", "aac", "-f", "adts"]),
    ("sine.wav", None, SOUND + ["-c:a", "pcm_s16le"]),
]


def samples():
    """Every film made, or kept from last time; then the two that must be
    refused, made from the first. (name, path, decoder)."""
    os.makedirs(SAMPLES_DIR, exist_ok=True)
    with open(os.path.join(SAMPLES_DIR, ".lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        version = subprocess.run([FFMPEG, "-version"], capture_output=True,
                                 text=True, check=True).stdout.splitlines()[0]
        found = []
        for name, decoder, recipe in SAMPLES:
            path = os.path.join(SAMPLES_DIR, name)
            key = path + ".recipe"
            said = repr((version, recipe))
            if not (os.path.exists(path) and os.path.exists(key)
                    and open(key).read() == said):
                # SVT-AV1 talks on stderr whatever FFmpeg's log level is;
                # what it said is shown only if the film was not made.
                made = subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y"]
                                      + recipe + [path], capture_output=True, text=True)
                if made.returncode != 0:
                    sys.exit("run_mediakit: %s was not made:\n%s" % (name, made.stderr))
                with open(key, "w") as f:
                    f.write(said)
            found.append((name, path, decoder))
        whole = open(os.path.join(SAMPLES_DIR, "h264-aac.mp4"), "rb").read()
        for name, data in (("truncated.mp4", whole[:3000]),
                           # Not random: the same noise every run.
                           ("noise.bin", hashlib.sha256(b"noise").digest() * 128)):
            path = os.path.join(SAMPLES_DIR, name)
            with open(path, "wb") as f:
                f.write(data)
            found.append((name, path, None))
    return found


def gather(found, broken):
    shutil.rmtree(FOLDER, ignore_errors=True)
    os.makedirs(FOLDER)
    for name, path, _ in found:
        into = os.path.join(FOLDER, name)
        shutil.copyfile(path, into)
        if name == broken:
            b = bytearray(open(into, "rb").read())
            b[len(b) // 2] ^= 0xff
            open(into, "wb").write(bytes(b))
    with open(os.path.join(FOLDER, "manifest.txt"), "w") as f:
        f.write("".join(n + "\n" for n, _, _ in found))


def mac_libraries():
    os.makedirs(os.path.join(ROOT, "build", "w5"), exist_ok=True)
    with open(os.path.join(ROOT, "build", "w5", ".host-lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        subprocess.run([sys.executable, os.path.join(HERE, "w5_build.py"), REF,
                        "dav1d", "ffmpeg"], check=True, capture_output=True)


def reference():
    binary = os.path.join(WORK, "mediatest-mac")
    lib = os.path.join(HOST, "lib")
    subprocess.run(["/usr/bin/clang"] + ARCH_FLAGS + [
                    "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-DMEDIATEST_HOST", "-I" + os.path.join(HOST, "include"), "-I" + KIT,
                    os.path.join(KIT, "media.c"), os.path.join(TEST, "mediatest.c")]
                   + [os.path.join(lib, n) for n in (
                       "libavformat.a", "libavcodec.a", "libswresample.a", "libavutil.a",
                       "libdav1d.a")] + ["-lpthread", "-o", binary],
                   check=True, capture_output=True)
    return subprocess.run(([ "arch", "-x86_64"] if X86 else []) + [binary, FOLDER],
                          capture_output=True, text=True, check=True).stdout.splitlines()


def framemd5(found):
    """What Homebrew's FFmpeg says of each file's first picture stream:
    the MD5 of the concatenated frame MD5s, as `mediatest.c` gives it."""
    said = {}
    for name, _, decoder in found:
        if not decoder:
            continue
        out = subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error",
                              "-c:v", decoder, "-i", os.path.join(FOLDER, name),
                              "-map", "0:v:0", "-fps_mode", "passthrough",
                              "-f", "framemd5", "-"], capture_output=True, text=True)
        sums = [ln.split(",")[-1].strip() for ln in out.stdout.splitlines()
                if ln and not ln.startswith("#")]
        said[name] = (len(sums), hashlib.md5("".join(sums).encode()).hexdigest())
    return said


def main():
    built = os.path.join(APPS, "mediatest.elf")
    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` (and ARCH=x86_64) builds it" % built)
        return 1
    if not os.path.exists(FFMPEG):
        print("FAIL: no ffmpeg on this Mac to make the films with (brew install ffmpeg)")
        return 1

    found = samples()
    mac_libraries()
    broken = os.environ.get("KOSMOS_MEDIA_BREAK")
    gather(found, broken)
    want = reference()
    tool = framemd5(found)
    if broken:
        gather(found, None)             # the guest gets the true files

    image = os.path.join(WORK, "mediatest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    with open(os.path.join(WORK, "mediatest.lua"), "w") as f:
        f.write(PROGRAM)

    disk = [(os.path.join(WORK, n), "/Home/apps/media/" + n)
            for n in ("mediatest.lua", "mediatest.elf")]
    disk += [(os.path.join(FOLDER, n), "/Home/apps/media/films/" + n)
             for n in [n for n, _, _ in found] + ["manifest.txt"]]
    megabytes = sum(os.path.getsize(src) for src, _ in disk) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)] + ["%s:%s" % f for f in disk],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 600)
    said = []
    done = False
    mark = 0
    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/media/mediatest.lua")
        while time.monotonic() < started + 600 and not done:
            time.sleep(0.2)
            guest._read_available()
            lines = guest.seen[mark:].replace("\r", "").split("\n")[1:]
            said = [ln for ln in lines if ln.split(" ")[0].rstrip(":")
                    in ("versions", "file", "stream", "seek", "mediatest")]
            done = any(ln.startswith("mediatest: ") for ln in said) \
                or "ended, code" in guest.seen[mark:]
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_MEDIA_SERIAL"):
        with open(os.environ["KOSMOS_MEDIA_SERIAL"], "w") as f:
            f.write(guest.seen)

    failed = False
    differ = [(w, s) for w, s in zip(want, said) if w != s]
    if len(want) != len(said) or differ:
        print("FAIL: the Media Kit on Kosmos is not what it is on this Mac: "
              "%d lines here, %d there, %d differ" % (len(want), len(said), len(differ)))
        for w, s in differ[:12]:
            print("  Mac:    %s\n  Kosmos: %s" % (w, s))
        if not any(ln.startswith("mediatest: ") for ln in said):
            print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        failed = True

    # Each file's first picture stream, against FFmpeg's own tool.
    pictures = 0
    for name, (frames, digest) in sorted(tool.items()):
        line = next((ln for ln in said if ln.startswith("stream %s #" % name)
                     and " video " in ln), None)
        mine = line.rsplit(", ", 1)[-1] if line else None
        count = line.split(": ", 1)[1].split(" frames")[0] if line else None
        if mine != digest or count != str(frames):
            print("FAIL: %s's pictures are not what `ffmpeg -f framemd5` says: "
                  "%s frames %s there, %d frames %s from ffmpeg" % (name, count, mine,
                                                                   frames, digest))
            failed = True
        else:
            pictures += frames
    if failed:
        return 1

    refused = [ln for ln in said if ln.startswith("file ") and " stream" not in ln]
    if len(refused) < 2:
        print("FAIL: the truncated film and the noise were not both refused: %s" % refused)
        return 1
    streams = sum(1 for ln in said if ln.startswith("stream "))
    print("PASS: the Media Kit read %d files on Kosmos - %d streams decoded, every line "
          "as on this Mac, and %d pictures in %d films as `ffmpeg -f framemd5` says them; "
          "%d refused, as they should be; in %.1f s (%s)"
          % (len(found), streams, pictures, len(tool), len(refused), took,
             want[0].split(": ", 1)[1] if want else "?"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
