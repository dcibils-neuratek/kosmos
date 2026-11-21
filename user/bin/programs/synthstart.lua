-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Synth Kit started over and over, a note asked for at once each time.
--
-- An instrument for one fault: a note posted while the kit's thread had
-- not yet taken the song was dropped, because the thread drops what it is
-- handed with no song to play it in - and Flight's engine was heard silent
-- in about one gate in three (`testing.md` 18.983). It opens the sound,
-- starts the thread, hands it a song and a held note in Flight's order,
-- waits for the sound to pass its attack, and says whether it was heard.
-- Once a process: the kit keeps its song past a close, so only a
-- program's first start can meet a thread with none, and
-- `tools/run_x86.py`'s `synth_start` runs this many times over.
local audio = use("/Kosmos/Libraries/audio.lua")
local synth = use("/Kosmos/Kits/synth")

local song = { bpm = 120, fx = { dRet = 0, rRet = 0 },
  tracks = { { type = "synth", vol = 0.8, params = {
    w1 = 1, w2 = 1, oct2 = 0, mix = 0.5, sub = 0, noise = 0, mono = 1,
    ftype = 0, cut = 4000, res = 0.2, env = 0, aA = 0.001, aD = 0.1, aS = 1, aR = 0.05,
    lfoR = 1, lfoC = 0, lfoP = 0, lvl = 1, uni = 1, det = 0, glide = 0 } } } }

local st = {}
local out, why = audio.open("synthstart", 8)
if not out then print("synthstart: no sound: " .. tostring(why)) return end
synth.start(out.ring, out.rate)
synth.song(song)
synth.note_on(1, 45, 0.8)
local waited = 0
repeat
  sys.sleep(2)
  waited = waited + 1
  synth.state(st)
until (st.rendered or 0) >= 8192 or waited > 200
local peak = math.max(st.peak_l or 0, st.peak_r or 0)
synth.note_off(1, 45)
synth.close()
out:close()
print(("synthstart: %s, rendered %d, peak %.3f")
      :format(peak > 0.01 and "heard" or "silent", st.rendered or 0, peak))
