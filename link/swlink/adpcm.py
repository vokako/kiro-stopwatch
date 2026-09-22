"""IMA ADPCM block codec used by swlink BLE microphone streaming.

Wire block: little-endian predictor int16, index uint8, reserved uint8, then
one byte for every two mono samples (low nibble first). Each 1024-sample device
block is 516 bytes instead of 2048 bytes of s16le PCM.
"""
from __future__ import annotations

import struct

_STEP = (7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
         50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
         253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
         1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
         3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
         11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
         32767)
_INDEX = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)


def decode_block(data: bytes, samples: int | None = None) -> bytes:
    """Decode a self-contained IMA ADPCM block to s16le PCM."""
    if len(data) < 4:
        raise ValueError("ADPCM block shorter than 4-byte header")
    predictor, index, _ = struct.unpack_from("<hBB", data)
    index = max(0, min(88, index))
    n = samples if samples is not None else (len(data) - 4) * 2
    out: list[int] = []
    for byte in data[4:]:
        for code in (byte & 0x0F, byte >> 4):
            step = _STEP[index]
            diff = step >> 3
            if code & 1: diff += step >> 2
            if code & 2: diff += step >> 1
            if code & 4: diff += step
            predictor += -diff if code & 8 else diff
            predictor = max(-32768, min(32767, predictor))
            index = max(0, min(88, index + _INDEX[code]))
            out.append(predictor)
            if len(out) == n:
                return struct.pack("<%dh" % len(out), *out)
    return struct.pack("<%dh" % len(out), *out)
