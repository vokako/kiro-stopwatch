// Minimal IMA ADPCM encoder. Block format matches swlink/adpcm.py:
// int16 predictor LE, uint8 index, uint8 reserved, low-nibble-first codes.
#pragma once
#include <cstdint>
#include <cstddef>

namespace adpcm {
static constexpr int step[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static constexpr int index_delta[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};

inline size_t encode(const int16_t* in, size_t samples, uint8_t* out) {
  int predictor = 0, index = 0;
  out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
  for (size_t i = 0; i < samples; ++i) {
    int diff = (int)in[i] - predictor, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int s = step[index], delta = s >> 3;
    if (diff >= s) { code |= 4; diff -= s; delta += s; }
    s >>= 1; if (diff >= s) { code |= 2; diff -= s; delta += s; }
    s >>= 1; if (diff >= s) { code |= 1; delta += s; }
    predictor += (code & 8) ? -delta : delta;
    if (predictor > 32767) predictor = 32767; else if (predictor < -32768) predictor = -32768;
    index += index_delta[code]; if (index < 0) index = 0; else if (index > 88) index = 88;
    if (i & 1) out[4 + i / 2] |= (uint8_t)(code << 4); else out[4 + i / 2] = (uint8_t)code;
  }
  return 4 + (samples + 1) / 2;
}
} // namespace adpcm
