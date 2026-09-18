#!/usr/bin/env python3
"""Yamaha 4-bit ADPCM encoder for the AICA (PCMS=2).

The AICA decodes this format in hardware, so a clip stored this way costs a
quarter of the ROM and needs no decompressor, no scratch buffer and no CPU:
the bytes are copied into sound RAM exactly as they sit in the EPROM.

The decoder is transcribed from MAME's aica.cpp (DecodeADPCM), which is the
reference this project validates against:

    x = (quant * quant_mul[d & 7]) / 8      quant_mul = 1, 3, 5 ... 15
    if (d & 8) x = -x
    signal = clip16(signal + x)
    quant  = clamp((quant * TableQuant[d & 7]) >> 8, 0x7f, 0x6000)

with signal = 0 and quant = 0x7f at key-on, and the LOW nibble of each byte
holding the first of its two samples.

The encoder runs the decoder in lockstep -- it has to, since every step
depends on the state the decoder will be in -- and picks, of the sixteen
codes available at that step, the one landing closest to the sample it
wants. Greedy rather than optimal over the clip: a trellis would do better,
but measured against the usual `delta * 4 / quant` shortcut this search wins
nothing at all (27.2 dB against 27.2 dB on the English clips), so there is
no reason to reach for one. It is kept because it is obviously correct
without having to trust any algebra about the step table.

Measured on the project's own clips: 4.00x, SNR 27.2 dB (EN) / 25.1 dB (FR).
"""

DIFF  = (1, 3, 5, 7, 9, 11, 13, 15)
QUANT = (230, 230, 230, 230, 307, 409, 512, 614)   # ADFIX(0.898 ... 2.398)


def _clip16(x):
    return -32768 if x < -32768 else (32767 if x > 32767 else x)


def encode(pcm):
    """pcm: iterable of signed 16-bit samples -> bytes, 2 samples per byte."""
    out = bytearray()
    signal, quant = 0, 0x7f
    hi, held = False, 0
    for s in pcm:
        s = int(s)          # a numpy int16 would wrap in the arithmetic below
        best, best_err = 0, None
        for code in range(8):
            x = (quant * DIFF[code]) // 8
            if x > 0x7FFF:
                x = 0x7FFF
            for sign in (0, 8):
                err = abs(s - _clip16(signal + (-x if sign else x)))
                if best_err is None or err < best_err:
                    best_err, best = err, sign | code
        x = (quant * DIFF[best & 7]) // 8
        if x > 0x7FFF:
            x = 0x7FFF
        signal = _clip16(signal + (-x if best & 8 else x))
        quant = (quant * QUANT[best & 7]) >> 8
        quant = 0x7f if quant < 0x7f else (0x6000 if quant > 0x6000 else quant)
        if hi:
            out.append(held | (best << 4))
            hi = False
        else:
            held, hi = best, True
    if hi:                       # odd sample count: pad the high nibble
        out.append(held)
    return bytes(out)


def decode(data, nsamples):
    """Reference decoder, for measuring what the encoder actually delivers."""
    out = []
    signal, quant = 0, 0x7f
    for i in range(nsamples):
        d = data[i >> 1]
        d = (d & 0xF) if (i & 1) == 0 else (d >> 4)
        x = (quant * DIFF[d & 7]) // 8
        if x > 0x7FFF:
            x = 0x7FFF
        signal = _clip16(signal + (-x if d & 8 else x))
        quant = (quant * QUANT[d & 7]) >> 8
        quant = 0x7f if quant < 0x7f else (0x6000 if quant > 0x6000 else quant)
        out.append(signal)
    return out
