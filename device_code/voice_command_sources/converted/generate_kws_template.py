#!/usr/bin/env python3
"""
Generate kws_templates.h from folders of WAV files.

Directory layout (example):
  wavs/
    set/
      sample1.wav
      sample2.wav
    cancel/
      a.wav
    add/
      1.wav
    minus/
      1.wav
    stop/
      x.wav
    timer/
      t1.wav
    minute/
      m1.wav
    minutes/
      m2.wav
    hour/
      h1.wav
    hours/
      h2.wav
    one/
      1.wav
    two/
      1.wav
    ...
    ninety/
      1.wav
    baking/
      a.wav
    cooking/
      b.wav
    break/
      c.wav
    homework/
      d.wav
    exercise/
      e.wav
    workout/
      f.wav

Each folder name MUST match the token strings in the firmware:
  ["set","cancel","add","minus","stop","timer",
   "minute","minutes","hour","hours",
   "one","two","three","four","five","six","seven","eight","nine",
   "ten","eleven","twelve","thirteen","fourteen","fifteen","sixteen","seventeen","eighteen","nineteen",
   "twenty","thirty","forty","fifty","sixty","seventy","eighty","ninety",
   "baking","cooking","break","homework","exercise","workout"]

Usage:
  python generate_kws_templates.py --wavs wavs --out kws_templates.h

Requires:
  pip install numpy scipy soundfile
"""

import argparse
import os
import glob
import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

# --- Keep these in sync with firmware ---
KWS_SR = 16000
FRAME_MS = 25
HOP_MS = 10
FRAME_SAMPLES = int(KWS_SR * FRAME_MS / 1000)   # 400
HOP_SAMPLES   = int(KWS_SR * HOP_MS / 1000)     # 160
KWS_NBINS = 24
KWS_MAX_FRAMES = 120
FMIN = 300.0
FMAX = 4000.0

# VAD params (match device roughly)
VAD_RMS_ON  = 900.0
VAD_ZC_RATIO = 0.02
VAD_MIN_FRAMES = 15
VAD_POSTROLL_FR = 8

TOKEN_LIST = [
  "set","cancel","add","minus","stop","timer",
  "minute","minutes","hour","hours",
  "one","two","three","four","five","six","seven","eight","nine",
  "ten","eleven","twelve","thirteen","fourteen","fifteen","sixteen","seventeen","eighteen","nineteen",
  "twenty","thirty","forty","fifty","sixty","seventy","eighty","ninety",
  "baking","cooking","break","homework","exercise","workout"
]
TOKEN_TO_ID = {name: i for i, name in enumerate(TOKEN_LIST)}

def hann(N):
  n = np.arange(N, dtype=np.float32)
  return 0.5 - 0.5*np.cos(2*np.pi*n/(N-1))

def compute_goertzel_coeffs():
  # log-spaced center freqs, then convert to cos(2*pi*k/N) weight
  freqs = FMIN * (FMAX/FMIN) ** (np.linspace(0.0, 1.0, KWS_NBINS))
  k = (FRAME_SAMPLES * freqs) / KWS_SR
  w = 2.0 * np.cos(2.0*np.pi*k/FRAME_SAMPLES)
  return w.astype(np.float32), freqs

GOERTZEL_W, _ = compute_goertzel_coeffs()
HANN = hann(FRAME_SAMPLES)

def mono16k(wav):
  x, sr = sf.read(wav, always_2d=False)
  # make mono
  if x.ndim == 2:
    x = x.mean(axis=1)
  x = x.astype(np.float32)

  # normalize 16-bit range if float
  # (not strictly needed; we keep amplitude scale similar to device)
  # resample to 16 kHz
  if sr != KWS_SR:
    # rational resampler
    # Find integers p/q approx KWS_SR/sr:
    # Use resample_poly with simple factors:
    from math import gcd
    g = gcd(int(KWS_SR), int(sr))
    up = int(KWS_SR // g)
    down = int(sr // g)
    x = resample_poly(x, up, down).astype(np.float32)

  return x

def frame_sig(x):
  """Return frames [num_frames, FRAME_SAMPLES] with hop HOP_SAMPLES."""
  if len(x) < FRAME_SAMPLES:
    return np.zeros((0, FRAME_SAMPLES), dtype=np.float32)
  n = 1 + (len(x) - FRAME_SAMPLES) // HOP_SAMPLES
  idx = np.arange(FRAME_SAMPLES)[None, :] + np.arange(n)[:, None]*HOP_SAMPLES
  return x[idx].copy()

def vad_mask(frames):
  if len(frames) == 0:
    return np.zeros((0,), dtype=bool)
  rms = np.sqrt((frames.astype(np.float32)**2).mean(axis=1))
  zc  = ((frames[:, 1:]*frames[:, :-1]) < 0).sum(axis=1)

  # ADAPTIVE thresholds (robust for quiet/loud files)
  rms_on = max(200.0, 0.6 * np.median(rms))        # scale to file loudness
  zc_ok  = zc > int(FRAME_SAMPLES * 0.004)         # ~0.4% of samples crossing

  mask = (rms > rms_on) & zc_ok
  return mask

def goertzel_frame(frame):
  """Compute 24-bin Goertzel log-power for one frame."""
  x = frame * HANN
  out = np.zeros((KWS_NBINS,), dtype=np.float32)
  for b in range(KWS_NBINS):
    w = GOERTZEL_W[b]
    s_prev = 0.0
    s_prev2 = 0.0
    for n in range(FRAME_SAMPLES):
      s = x[n] + w*s_prev - s_prev2
      s_prev2 = s_prev
      s_prev = s
    power = s_prev2*s_prev2 + s_prev*s_prev - w*s_prev*s_prev2
    out[b] = np.log(1e-3 + power)
  return out

def extract_feats_from_wav(wav_path):
  x = mono16k(wav_path)

  # frame and VAD
  frames = frame_sig(x)
  if frames.shape[0] == 0:
    return None

  vad = vad_mask(frames)
  if vad.sum() < 6:   # was 15
    # fall back: take center 0.6 s so single short words still pass
    center = frames.shape[0] // 2
    half = 30  # ~0.3 s on each side at 10 ms hop
    use_frames = frames[max(0, center-half):min(frames.shape[0], center+half)]
  else:
    last_idx = np.where(vad)[0][-1]
    start_idx = np.where(vad)[0][0]
    end_idx = min(frames.shape[0], last_idx + VAD_POSTROLL_FR + 1)
    use_frames = frames[start_idx:end_idx]


  # extend with small postroll like the device
  last_idx = np.where(vad)[0][-1] if vad.any() else -1
  end_idx = min(frames.shape[0], last_idx + VAD_POSTROLL_FR + 1)
  start_idx = np.where(vad)[0][0] if vad.any() else 0
  use_frames = frames[start_idx:end_idx]

  # compute features per frame
  feats = np.stack([goertzel_frame(f) for f in use_frames], axis=0)  # [T, 24]

  # CMVN per utterance
  mu = feats.mean(axis=0, keepdims=True)
  std = feats.std(axis=0, ddof=1, keepdims=True)
  std[std < 1e-6] = 1.0
  feats = (feats - mu) / std

  # clamp max frames
  if feats.shape[0] > KWS_MAX_FRAMES:
    feats = feats[:KWS_MAX_FRAMES, :]

  return feats.astype(np.float32)

def write_header(out_path, bank):
  """
  bank: dict token_id -> list of np.array [T,24]
  Emit C header with PROGMEM arrays and a small metadata table.
  """
  with open(out_path, "w") as f:
    f.write("// Auto-generated by generate_kws_templates.py\n")
    f.write("// Do not edit by hand.\n\n")
    f.write("#pragma once\n#include <Arduino.h>\n\n")
    f.write("// Must match firmware constants:\n")
    f.write("#define KWS_NBINS 24\n\n")

    # Emit per-template arrays
    tpl_names = []
    for tid in sorted(bank.keys()):
      tpl_list = bank[tid]
      for j, tpl in enumerate(tpl_list):
        name = f"kws_tpl_{tid}_{j}"
        tpl_names.append((tid, j, name, tpl.shape[0]))
        flat = tpl.reshape(-1)
        f.write(f"// token {tid} ({TOKEN_LIST[tid]}), template {j}, T={tpl.shape[0]}\n")
        f.write(f"static const float {name}[] PROGMEM = {{\n")
        # write as rows of 24
        idx = 0
        for t in range(tpl.shape[0]):
          row = ", ".join(f"{float(v):.6f}" for v in tpl[t])
          f.write(f"  {row},\n")
          idx += KWS_NBINS
        f.write("};\n\n")

    # For each token, write a small table of template pointers and lengths
    f.write("// Per-token template tables\n")
    for tid in range(len(TOKEN_LIST)):
      tpls = [x for x in tpl_names if x[0] == tid]
      f.write(f"static const uint16_t kws_token_{tid}_T[] PROGMEM = {{")
      f.write(", ".join(str(T) for (_,_,_,T) in tpls) if tpls else "")
      f.write("};\n")
      f.write(f"static const float* const kws_token_{tid}_ptrs[] PROGMEM = {{")
      f.write(", ".join(name for (_,_,name,_) in tpls) if tpls else "")
      f.write("};\n\n")

    # Summary table so firmware can iterate tokens
    f.write("// Summary table\n")
    f.write(f"static const uint16_t kws_token_counts[] PROGMEM = {{\n  ")
    counts = []
    for tid in range(len(TOKEN_LIST)):
      c = sum(1 for x in tpl_names if x[0] == tid)
      counts.append(c)
    f.write(", ".join(str(c) for c in counts))
    f.write("\n};\n\n")

    # function to copy from PROGMEM into runtime bank (malloc)
    f.write("// Loader to copy PROGMEM templates into runtime KWS bank\n")
    f.write("extern \"C\" {\n")
    f.write("  typedef struct { uint16_t T; float* feats; } KwsTemplate;\n")
    f.write("  typedef struct { uint8_t n; KwsTemplate tpl[3]; } KwsBank; // KWS_MAX_TEMPLATES=3 in firmware\n")
    f.write("  extern KwsBank g_bank[]; // defined in main.ino\n")
    f.write("}\n\n")

    f.write("static void kws_load_from_progmem() {\n")
    f.write("  for (int tid=0; tid<"+str(len(TOKEN_LIST))+"; ++tid) {\n")
    f.write("    uint16_t count = pgm_read_word(&kws_token_counts[tid]);\n")
    f.write("    KwsBank &bk = g_bank[tid];\n")
    f.write("    bk.n = 0;\n")
    f.write("    if (!count) continue;\n")
    f.write("    // read T table pointer\n")
    f.write("    const uint16_t* Ttbl;\n")
    f.write("    const float* const* Ptbl;\n")
    # We have to select the token-specific arrays by switch (C++ can’t index symbol names)
    f.write("    switch(tid){\n")
    for tid in range(len(TOKEN_LIST)):
      f.write(f"      case {tid}: Ttbl = kws_token_{tid}_T; Ptbl = kws_token_{tid}_ptrs; break;\n")
    f.write("      default: Ttbl=nullptr; Ptbl=nullptr; break;\n")
    f.write("    }\n")
    f.write("    if (!Ttbl || !Ptbl) continue;\n")
    f.write("    uint16_t use = count > 3 ? 3 : count; // cap to firmware slots\n")
    f.write("    for (uint16_t j=0; j<use; ++j){\n")
    f.write("      uint16_t T = pgm_read_word(&Ttbl[j]);\n")
    f.write("      const float* p = (const float*)pgm_read_ptr(&Ptbl[j]);\n")
    f.write("      size_t bytes = (size_t)T * KWS_NBINS * sizeof(float);\n")
    f.write("      float* buf = (float*) malloc(bytes);\n")
    f.write("      if(!buf) continue;\n")
    f.write("      memcpy_P(buf, p, bytes);\n")
    f.write("      bk.tpl[j].T = T;\n")
    f.write("      bk.tpl[j].feats = buf;\n")
    f.write("      bk.n++;\n")
    f.write("    }\n")
    f.write("  }\n")
    f.write("}\n")

  print(f"[OK] Wrote {out_path}")

def main():
  ap = argparse.ArgumentParser()
  ap.add_argument("--wavs", required=True, help="Root folder containing per-token subfolders")
  ap.add_argument("--out", default="kws_templates.h")
  args = ap.parse_args()

  bank = {}  # tid -> [feats...]
  missing = []

  for token in TOKEN_LIST:
    token_dir = os.path.join(args.wavs, token)
    wavs = sorted(glob.glob(os.path.join(token_dir, "*.wav")))
    if not wavs:
      missing.append(token)
      continue
    for w in wavs:
      feats = extract_feats_from_wav(w)
      if feats is None:
        print(f"[WARN] Skipped (VAD too short?) {w}")
        continue
      tid = TOKEN_TO_ID[token]
      bank.setdefault(tid, []).append(feats)

  if missing:
    print("[INFO] No WAVs for:", ", ".join(missing))

  if not bank:
    raise SystemExit("No valid templates found—nothing to write.")

  write_header(args.out, bank)

if __name__ == "__main__":
  main()
