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
import re
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

# **The Lua door, in `media.elf`** (`docs/media.md` M6a): the same films
# opened through `/Kosmos/Kits/media` by a program that names the image,
# kept where `make apps` and a stick put it, /Home/System/Media. Each
# stream said as `mediatest` says it - kind, codec, size and depth, or rate
# and channels - and the packets of each file counted; a file refused, with
# the reason `mediatest` gave.
DOOR = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Media Kit's Lua door's test (`tools/run_mediakit.py`).
-- kosmos: image media
local media = use("/Kosmos/Kits/media")
print("door: FFmpeg " .. media.version())
local folder = "/Home/apps/media/films/"
-- M6b: every stream decoded through the door, its frames counted and their
-- times hashed as `mediatest` hashes them (FNV-1a over each pts's eight
-- bytes), a picture drawn onto a surface and a sound handed over as an
-- address - the counts and hashes held to mediatest's by the harness.
local function fnv(h, v)
  for k = 0, 7 do
    h = (h ~ ((v >> (8 * k)) & 0xff)) * 0x100000001b3
  end
  return h
end
local function drain(t)
  for _ = 1, 100000 do
    if t.refused then return end
    if t.kind == "video" then
      local at, why = t.dec:picture(t.surface)
      if at == false then return end
      if at == nil then t.refused = why; return end
      t.n = t.n + 1
      t.times = fnv(t.times, at)
      for _, xy in ipairs(t.probes) do
        if t.surface:get(xy[1], xy[2]) ~= xy[3] then t.painted = true end
      end
    else
      local at, len, pts, frames = t.dec:sound()
      if at == false then return end
      if at == nil then t.refused = len; return end
      t.n = t.n + 1
      t.times = fnv(t.times, pts)
      t.samples = t.samples + frames
      if at == 0 or len ~= frames * t.channels * 2 then t.wrong = true end
    end
  end
end
local function decode(name)
  local src = media.open(folder .. name)
  local tracks, order = {}, {}
  for i, st in ipairs(src:streams()) do
    if st.kind == "video" or st.kind == "audio" then
      local dec, why = src:decoder(i - 1, 2)
      if not dec then
        print(("door %s #%d: %s"):format(name, i - 1, why))
      else
        local t = { dec = dec, kind = st.kind, n = 0, samples = 0, i = i - 1,
                    times = 0xcbf29ce484222325, channels = st.channels }
        if st.kind == "video" then
          t.surface = gfx.surface{ w = st.width, h = st.height }
          t.probes = {}
          for _, f in ipairs({ { 1, 1 }, { 2, 2 }, { 1, 3 }, { 3, 1 }, { 3, 3 } }) do
            local x, y = st.width * f[1] // 4, st.height * f[2] // 4
            t.probes[#t.probes + 1] = { x, y, t.surface:get(x, y) }
          end
        end
        tracks[i - 1] = t
        order[#order + 1] = t
      end
    end
  end
  while true do
    local p = src:read()
    if not p then break end
    local t = tracks[p.stream]
    if t and not t.refused then
      local ok = t.dec:send(p)
      for _ = 1, 3 do
        if ok ~= false then break end
        drain(t)
        ok = t.dec:send(p)
      end
      drain(t)
    end
    p:free()
  end
  for _, t in ipairs(order) do
    if not t.refused then
      t.dec:send()
      drain(t)
    end
    if t.refused then
      print(("door %s #%d: %s"):format(name, t.i, t.refused))
    elseif t.kind == "video" then
      print(("door %s #%d: %d frames, times %016x, %s"):format(name, t.i, t.n, t.times,
            t.painted and "painted" or "not painted"))
    else
      print(("door %s #%d: %d samples, times %016x, %s"):format(name, t.i, t.samples,
            t.times, t.wrong and "its bytes wrong" or "its bytes right"))
    end
    t.dec:close()
    if t.surface then t.surface:free() end
  end
  src:close()
end
for name in fs.read(folder .. "manifest.txt"):gmatch("%S+") do
  local src, why = media.open(folder .. name)
  if not src then
    print("door " .. name .. ": " .. tostring(why))
  else
    for i, st in ipairs(src:streams()) do
      if st.kind == "video" then
        print(("door %s #%d video %s %dx%d %d bits"):format(name, i - 1, st.codec,
              st.width, st.height, st.bits))
      elseif st.kind == "audio" then
        print(("door %s #%d audio %s %d Hz %d ch"):format(name, i - 1, st.codec,
              st.rate, st.channels))
      end
    end
    local packets, keys, bytes = 0, 0, 0
    while true do
      local p, err = src:read()
      if not p then
        if err then print("door " .. name .. ": read stopped: " .. err) end
        break
      end
      packets = packets + 1
      if p.key then keys = keys + 1 end
      bytes = bytes + p.size
      p:free()
    end
    src:close()
    print(("door %s: %d packets, %d keys, %d bytes"):format(name, packets, keys, bytes))
    decode(name)
  end
end
print("door: done")
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

    door = os.path.join(APPS, "media.elf")
    if not os.path.exists(door):
        print("FAIL: no %s - `make apps` (and ARCH=x86_64) builds it" % door)
        return 1
    door_image = os.path.join(WORK, "media.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", door, door_image], check=True)
    with open(os.path.join(WORK, "door.lua"), "w") as f:
        f.write(DOOR)

    disk = [(os.path.join(WORK, n), "/Home/apps/media/" + n)
            for n in ("mediatest.lua", "mediatest.elf", "door.lua")]
    disk += [(door_image, "/Home/System/Media/media.elf")]
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

        # The Lua door, in `media.elf`, over the same files.
        mark_door = len(guest.seen)
        guest.type("run /Home/apps/media/door.lua")
        began = time.monotonic()
        while time.monotonic() < began + 300:
            time.sleep(0.2)
            guest._read_available()
            # Not "ended, code": `run`'s own process ends before the
            # program it starts has said anything.
            tail = guest.seen[mark_door:]
            if "door: done" in tail or "\nrun: " in tail:
                break
        door_said = [ln for ln in guest.seen[mark_door:].replace("\r", "").split("\n")
                     if ln.startswith("door")]
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

    # The door: every stream `mediatest` decoded, said the same way, and a
    # packet or more of every file it opened; the refused ones refused with
    # its reasons.
    def shape(line):
        rest = line.split(" ", 1)[1]
        if " video " in rest:
            return rest.split(" bits", 1)[0] + " bits"
        return rest.split(" ch", 1)[0] + " ch"

    want_streams = sorted(shape(ln) for ln in said
                          if ln.startswith("stream ") and (" video " in ln or " audio " in ln)
                          and ": " in ln and ("frames" in ln or "samples" in ln))
    door_streams = sorted(ln.split(" ", 1)[1] for ln in door_said
                          if " #" in ln and (" video " in ln or " audio " in ln))
    opened = [n for n, _, _ in found
              if any(ln.startswith("file %s: " % n) and " stream" in ln for ln in said)]
    counted = {ln.split(" ")[1].rstrip(":"): int(ln.split(": ")[1].split(" ")[0])
               for ln in door_said if " packets, " in ln}
    want_refused = sorted(ln.split(" ", 1)[1] for ln in refused)
    door_refused = sorted(ln.split(" ", 1)[1] for ln in door_said
                          if ln.startswith("door ") and ": " in ln and " #" not in ln
                          and " packets, " not in ln and not ln.startswith("door: "))
    problems = []
    if not any(ln.startswith("door: FFmpeg ") for ln in door_said):
        problems.append("the program in media.elf did not reach the kit: %r"
                        % door_said[-3:])
    missing = [w for w in want_streams if w not in door_streams]
    if missing or len(door_streams) != len(want_streams):
        problems.append("its streams are not mediatest's: %d here, %d there; missing %s"
                        % (len(door_streams), len(want_streams), missing[:4]))
    empty = [n for n in opened if counted.get(n, 0) <= 0]
    if empty:
        problems.append("no packets read from %s" % empty)
    if door_refused != want_refused:
        problems.append("its refusals are not mediatest's: %s against %s"
                        % (door_refused, want_refused))
    # M6b: each stream decoded through the door, its frames and their times
    # as mediatest's; 4:2:0 pictures drawn, and everything else refused.
    decoded = {}
    for ln in said:
        m = re.match(r"stream (\S+) #(\d+) (video|audio) .*?(?: bits (\w+))?: "
                     r"(\d+) (?:frames|samples), times ([0-9a-f]{16})", ln)
        if not m:
            continue
        key = "%s #%s" % (m.group(1), m.group(2))
        if m.group(3) == "video" and m.group(4) != "yuv420":
            decoded[key] = "REFUSED"
        elif m.group(3) == "video":
            decoded[key] = "%s frames, times %s, painted" % (m.group(5), m.group(6))
        else:
            decoded[key] = "%s samples, times %s, its bytes right" % (m.group(5), m.group(6))
    door_decoded = {}
    for ln in door_said:
        m = re.match(r"door (\S+ #\d+): (.*)$", ln)
        if m:
            door_decoded[m.group(1)] = m.group(2)
    wrong = []
    for key, w in sorted(decoded.items()):
        got = door_decoded.get(key)
        if w == "REFUSED":
            if got is None or "not 4:2:0" not in got:
                wrong.append("%s: %r, where a refusal was wanted" % (key, got))
        elif got != w:
            wrong.append("%s: %r, where mediatest says %r" % (key, got, w))
    if not decoded:
        wrong.append("mediatest decoded nothing to hold the door to")
    if wrong:
        problems.append("its decoding is not mediatest's: " + "; ".join(wrong[:4])
                        + (" and %d more" % (len(wrong) - 4) if len(wrong) > 4 else ""))
    if problems:
        print("FAIL: the Media Kit's Lua door in media.elf: " + "; ".join(problems))
        return 1
    streams = sum(1 for ln in said if ln.startswith("stream "))
    print("PASS: the Media Kit read %d files on Kosmos - %d streams decoded, every line "
          "as on this Mac, and %d pictures in %d films as `ffmpeg -f framemd5` says them; "
          "%d refused, as they should be; in %.1f s (%s); and through the Lua door in "
          "media.elf, %d streams said the same, %d packets read and %d streams decoded "
          "as mediatest decoded them (%d refused as not 4:2:0)"
          % (len(found), streams, pictures, len(tool), len(refused), took,
             want[0].split(": ", 1)[1] if want else "?", len(door_streams),
             sum(counted.values()), len(decoded),
             sum(1 for w in decoded.values() if w == "REFUSED")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
