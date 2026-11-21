#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Radio library, heard (`docs/flight.md` F9).

A station served from this Mac - a tone made into an MP3 here by ffmpeg,
sent again and again on one connection with no length, as an Icecast
station sends - reached by the guest at 10.0.2.2 through a redirect, as a
station's address very often is. `radio <address> 10` is run over Telnet
on the desktop the machine boots, and its sound goes to an HD Audio card
whose output QEMU writes to a WAV here. Checked:

- the redirect was followed, and the station answered 200;
- the station's bytes came - more than the region holds, so the region
  was emptied and filled again - and the player decoded and played them,
  with no frame refused;
- **the sound card heard the tone**: its loudest sample, and seconds of it,
  counted in the WAV, which no number the guest prints can flatter;
- a station that stops is asked again: the server closes the first
  connection after a while, and the program says it connected again.

`--garbage` is the control: the same station sending bytes that are not
MP3 - no frame's sync in them - which the player passes over, so no frame
is heard and the suite must fail.

Usage: run_radio.py IMAGE [--garbage]
"""

import http.server
import os
import random
import re
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402

GARBAGE = "--garbage" in sys.argv
sys.argv = [a for a in sys.argv if a != "--garbage"]
IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
WORK = scratch.directory("radio")
FFMPEG = "/opt/homebrew/bin/ffmpeg" if os.path.exists("/opt/homebrew/bin/ffmpeg") else "ffmpeg"
WAV_HEADER = 44
SECONDS = 10
FIRST_CONNECTION_BYTES = 96 * 1024      # then the server hangs up, once


def station_bytes():
    """Four seconds of a 660 Hz tone at 128 kbit/s, as MP3 - or, for the
    control, as many bytes of something that is not."""
    path = os.path.join(WORK, "tone.mp3")
    subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                    "sine=frequency=660:sample_rate=44100:duration=4", "-ac", "2",
                    "-b:a", "128k", "-codec:a", "libmp3lame", path], check=True)
    data = open(path, "rb").read()
    if GARBAGE:
        rng = random.Random(7)
        data = bytes((rng.randrange(256) & 0xFE) for _ in range(len(data)))   # no 0xFF: no sync
    return data


def serve(data):
    asked = {"tune": 0, "station": 0}

    class Station(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path == "/tune":
                asked["tune"] += 1
                self.send_response(302)
                self.send_header("Location", "/station.mp3")
                self.end_headers()
                return
            if self.path != "/station.mp3":
                self.send_error(404)
                return
            asked["station"] += 1
            first = asked["station"] == 1
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("icy-name", "Kosmos test station")
            self.end_headers()
            sent, began = 0, time.monotonic()
            try:
                # At about the station's own rate, 16 KB a second, ahead by
                # a few seconds as a real server's burst is.
                while time.monotonic() - began < 120:
                    for i in range(0, len(data), 4096):
                        due = began + (sent - 64 * 1024) / 16000.0
                        if due > time.monotonic():
                            time.sleep(due - time.monotonic())
                        self.wfile.write(data[i:i + 4096])
                        sent += 4096
                        if first and sent >= FIRST_CONNECTION_BYTES:
                            return                       # hung up: asked again
            except (BrokenPipeError, ConnectionResetError):
                pass

    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Station)
    httpd.daemon_threads = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, asked


def heard(path):
    """(seconds not silent, loudest sample) in what QEMU wrote."""
    if not os.path.exists(path):
        return 0.0, 0
    data = open(path, "rb").read()[WAV_HEADER:]
    frames = len(data) // 4
    if frames == 0:
        return 0.0, 0
    samples = struct.unpack("<%dh" % (frames * 2), data[:frames * 4])
    loud = sum(1 for i in range(frames) if abs(samples[2 * i]) > 500)
    return loud / 44100.0, max(abs(s) for s in samples)


def main():
    import run_screenshot as R
    import run_servers as SV

    data = station_bytes()
    httpd, asked = serve(data)
    port = httpd.server_address[1]
    wav = os.path.join(WORK, "heard.wav")
    if os.path.exists(wav):
        os.remove(wav)
    R.X86_ARGS = R.X86_ARGS + ["-smp", "2", "-audiodev", "wav,id=snd0,path=%s" % wav,
                               "-device", "ich9-intel-hda", "-device", "hda-output,audiodev=snd0"]
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = SV.boot(IMAGE, telnet, web)
    fails = []
    try:
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = SV.connect(telnet)
        said = session.run("radio http://10.0.2.2:%d/tune %d" % (port, SECONDS)).decode(
            errors="replace")
    except R.Failure as e:
        print("FAIL: %s" % e)
        return 1
    finally:
        guest.close()
        httpd.shutdown()

    for line in said.splitlines():
        if line.startswith("radio:"):
            print(line.rstrip())
    seconds, loudest = heard(wav)
    print("radio: the sound card heard %.1f s of tone, its loudest sample %d; the station was "
          "asked %d times by its redirect and %d times itself" % (seconds, loudest, asked["tune"],
                                                                   asked["station"]))
    lines = re.findall(r"^radio: (\w+) status=(\d+) got=(\d+) frames=(\d+) dry=(\d+) "
                       r"refused=(\d+) connects=(\d+)", said, re.M)
    done = re.search(r"radio: done after \d+ s, (\d+) bytes, (\d+) frames heard", said)

    if not lines or not done:
        fails.append("the program did not report: %r" % said[-400:])
    else:
        last = lines[-1]
        if asked["tune"] < 1 or "200" not in [l[1] for l in lines]:
            fails.append("the redirect was not followed to a 200")
        if int(done.group(1)) <= 64 * 1024:
            fails.append("no more than one region of the station came: %s bytes" % done.group(1))
        if int(done.group(2)) < 44100 * 3:
            fails.append("under three seconds of frames were heard: %s" % done.group(2))
        if int(last[5]) != 0:
            fails.append("%s frames were refused" % last[5])
        if asked["station"] < 2 or int(last[6]) < 3:
            fails.append("the station that hung up was not asked again: asked %d, connects %s"
                         % (asked["station"], last[6]))
    if loudest < 1000 or seconds < 3:
        fails.append("the sound card heard no station: %.1f s, loudest %d" % (seconds, loudest))

    if fails:
        for f in fails:
            print("FAIL: " + f)
        return 1
    print("PASS: the Radio library played a station - a redirect followed, %s bytes, %.1f s "
              "heard at %d, asked again after it hung up" % (done.group(1), seconds, loudest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
