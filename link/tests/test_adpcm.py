from swlink.adpcm import decode_block


def test_ima_adpcm_zero_codes_stay_silent():
    # predictor=0, index=0, all codes 0 -> each decoded sample is 0
    assert decode_block(b"\0\0\0\0\0\0", samples=4) == b"\0\0" * 4


def test_ima_adpcm_block_can_be_trimmed_to_exact_sample_count():
    # A complete byte contains two codes but callers can ask for one sample.
    assert len(decode_block(b"\0\0\0\0\x77", samples=1)) == 2
