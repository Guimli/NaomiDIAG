#!/usr/bin/env python3
"""Generate spoken diagnostic clips as signed 16-bit LE 22050 Hz mono PCM
embedded in a C header for the AICA to play (PCMS=0).

Usage: gen_audio.py <fr|en> [out.h]

Engine: Piper neural TTS (tools/.venv), one voice model per language.
Some entries use phonetic spelling so the TTS pronounces acronyms right
(e.g. FR 'rome bioss' -> 'ROM BIOS', 'enne ve ram' -> 'NVRAM')."""
import subprocess, tempfile, os, sys

HERE   = os.path.dirname(os.path.abspath(__file__))
PIPER  = os.path.join(HERE, ".venv", "bin", "piper")
VOICES = os.path.join(HERE, "voices")

MODEL = {
    "fr": os.path.join(VOICES, "fr_FR-siwis-medium.onnx"),
    "en": os.path.join(VOICES, "en_US-lessac-medium.onnx"),
}

# clip key -> spoken text per language. Keys and order MUST match between
# languages (they drive the shared CLIP_* enum consumed by the C code).
CLIPS = {
    "tests_done": {"fr": "Fin des tests.",                    "en": "Tests complete."},
    "audio_ok":   {"fr": "Sortie audio fonctionnelle.",       "en": "Audio output working."},
    "cpu_cache":  {"fr": "Mémoire cache du processeur.",      "en": "C P U cache memory."},
    "cpu_ram":    {"fr": "Mémoire principale.",               "en": "Main memory."},
    "data_bus":   {"fr": "Bus de données.",                   "en": "Data bus."},
    "addr_bus":   {"fr": "Bus d'adresses.",                   "en": "Address bus."},
    "sound_ram":  {"fr": "Mémoire son.",                      "en": "Sound memory."},
    "ok":         {"fr": "Test réussi.",                      "en": "Test passed."},
    "fail":       {"fr": "Test échoué.",                      "en": "Test failed."},
    "defect":     {"fr": "défectueuse.",                      "en": "defective."},
    "num_1":      {"fr": "numéro un,",                        "en": "number one,"},
    "num_2":      {"fr": "numéro deux,",                      "en": "number two,"},
    "num_3":      {"fr": "numéro trois,",                     "en": "number three,"},
    "num_4":      {"fr": "numéro quatre,",                    "en": "number four,"},
    "num_5":      {"fr": "numéro cinq,",                      "en": "number five,"},
    "num_6":      {"fr": "numéro six,",                       "en": "number six,"},
    "num_7":      {"fr": "numéro sept,",                      "en": "number seven,"},
    "num_8":      {"fr": "numéro huit,",                      "en": "number eight,"},
    # silkscreen IC designators (mapping from the original BIOS RAM TEST)
    "ic_9":       {"fr": "I C neuf,",                         "en": "I C nine,"},
    "ic_10":      {"fr": "I C dix,",                          "en": "I C ten,"},
    "ic_11":      {"fr": "I C onze,",                         "en": "I C eleven,"},
    "ic_12":      {"fr": "I C douze,",                        "en": "I C twelve,"},
    "ic_16":      {"fr": "I C seize,",                        "en": "I C sixteen,"},
    "ic_18":      {"fr": "I C dix-huit,",                     "en": "I C eighteen,"},
    "ic_20":      {"fr": "I C vingt,",                        "en": "I C twenty,"},
    "ic_22":      {"fr": "I C vingt-deux,",                   "en": "I C twenty two,"},
    "ic_29":      {"fr": "I C vingt-neuf,",                   "en": "I C twenty nine,"},
    "ic_35":      {"fr": "I C trente-cinq,",                  "en": "I C thirty five,"},
    "ic_27":      {"fr": "I C vingt-sept,",                   "en": "I C twenty seven,"},
    "bios":       {"fr": "rome bioss.",                       "en": "Rom bios."},
    "vram":       {"fr": "Mémoire vidéo.",                    "en": "Video memory."},
    "sram":       {"fr": "enne vé ram.",                      "en": "N V ram."},
    "rtc":        {"fr": "Horloge temps réel.",              "en": "Real time clock."},
    "dimm":       {"fr": "Carte dime.",                       "en": "Dimm board."},
    "absent":     {"fr": "absente.",                          "en": "not present."},
    "present":    {"fr": "présente.",                         "en": "present."},
    "jvs":        {"fr": "Contrôleur d'entrées sorties, émi.","en": "I O controller, M I E."},
    "naomi1":     {"fr": "Carte Naomi un.",                   "en": "Naomi one board."},
    "naomi2":     {"fr": "Carte Naomi deux.",                 "en": "Naomi two board."},
    "board_unk":  {"fr": "Carte non identifiée.",            "en": "Unknown board."},
    "vram_b":     {"fr": "Seconde mémoire vidéo.",           "en": "Second video memory."},
    "elan":       {"fr": "Mémoire du processeur géométrique.","en": "Geometry processor memory."},
    "eeprom":     {"fr": "Mémoire des réglages.",            "en": "Settings memory."},
    "x76":        {"fr": "Puce de sécurité cartouche.",      "en": "Cartridge security chip."},
    "cart":       {"fr": "Cartouche de jeu.",                 "en": "Game cartridge."},
}

RATE = 22050
MAX_SAMPLES = 0xFFF0            # AICA LEA is 16-bit

def usage():
    print("usage: gen_audio.py <fr|en> [out.h]")
    sys.exit(1)

if len(sys.argv) < 2 or sys.argv[1] not in MODEL:
    usage()
lang = sys.argv[1]
out_path = sys.argv[2] if len(sys.argv) > 2 else f"src/audio_clips_{lang}.h"
model = MODEL[lang]

def tts(text, wav):
    if os.path.exists(PIPER) and os.path.exists(model):
        subprocess.run([PIPER, "-m", model, "-f", wav],
                       input=text.encode(), check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        ev = "fr" if lang == "fr" else "en"
        print("WARNING: Piper missing, falling back to espeak-ng")
        subprocess.run(["espeak-ng", "-v", ev, "-s", "150", "-w", wav, text],
                       check=True)

out = ["/* Auto-generated by tools/gen_audio.py -- do not edit.",
       f" * Signed 16-bit LE mono PCM @ {RATE} Hz for AICA (PCMS=0).",
       f" * Language: {lang}. Voice: Piper neural TTS. */",
       "#ifndef AUDIO_CLIPS_H", "#define AUDIO_CLIPS_H", ""]

names, total = [], 0
for name, texts in CLIPS.items():
    text = texts[lang]
    with tempfile.TemporaryDirectory() as td:
        wav = os.path.join(td, "c.wav")
        raw = os.path.join(td, "c.raw")
        tts(text, wav)
        subprocess.run(["sox", wav, "-r", str(RATE), "-c", "1",
                        "-e", "signed-integer", "-b", "16", "-t", "raw", raw,
                        "norm", "-2", "silence", "1", "0.05", "0.3%",
                        "reverse", "silence", "1", "0.05", "0.3%", "reverse"],
                       check=True)
        data = open(raw, "rb").read()
    if len(data) > MAX_SAMPLES * 2:
        print(f"WARNING: {name} truncated to 2.9s (AICA LEA is 16-bit)")
        data = data[:MAX_SAMPLES * 2]
    if len(data) % 4:
        data += b"\0" * (4 - len(data) % 4)
    total += len(data)
    arr = ", ".join(str(b - 256 if b > 127 else b) for b in data)
    out.append("static const signed char __attribute__((aligned(4))) "
               f"clip_{name}[] = {{ {arr} }};")
    names.append(name)
    print(f"{name}: {len(data)} bytes ({len(data)/2/RATE:.2f} s)")

print(f"[{lang}] total embedded audio: {total} bytes")
out += ["", "typedef struct { const signed char *pcm; unsigned len; } audio_clip;",
        "enum {"]
out += [f"    CLIP_{n.upper()}," for n in names]
out += ["    CLIP_COUNT", "};", "",
        "static const audio_clip audio_clips[CLIP_COUNT] = {"]
out += [f"    {{ clip_{n}, sizeof clip_{n} }}," for n in names]
out += ["};", "", "#endif"]

with open(out_path, "w") as f:
    f.write("\n".join(out) + "\n")
