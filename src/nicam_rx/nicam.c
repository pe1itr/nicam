/* NICAM frame descrambling, deinterleaving, parity, audio decode, and quality reports. */

static void init_scramble(void) {
    uint8_t reg[9];
    for (int i = 0; i < 9; i++) {
        reg[i] = 1;
    }
    uint8_t legacy[BODY_BITS + LEGACY_SCRAMBLE_PHASE];
    for (int i = 0; i < BODY_BITS + LEGACY_SCRAMBLE_PHASE; i++) {
        legacy[i] = reg[8];
        uint8_t feedback = reg[8] ^ reg[4];
        for (int j = 8; j > 0; j--) {
            reg[j] = reg[j - 1];
        }
        reg[0] = feedback;
    }
    for (int i = 0; i < BODY_BITS; i++) {
        scramble[i] = legacy[i + LEGACY_SCRAMBLE_PHASE];
    }
}
static void station_id_init(StationIdState *state) {
    memset(state, 0, sizeof(*state));
    memcpy(state->displayed, "        ", 8);
    state->displayed[8] = '\0';
    state->last_pos = -1;
}

static void station_id_update(StationIdState *state, const uint8_t ctrl16[16]) {
    int pos = (ctrl16[5] & 1U) | ((ctrl16[6] & 1U) << 1) | ((ctrl16[7] & 1U) << 2);
    int ch = 0;
    for (int bit = 0; bit < 8; bit++) {
        ch |= (ctrl16[8 + bit] & 1U) << bit;
    }
    if (pos < 0 || pos >= 8 || ch < 32 || ch > 126) {
        return;
    }
    int completed_cycle = state->last_pos == 7 && pos == 0 && state->seen_mask == 0xffU;
    if (pos != state->last_pos) {
        state->last_pos = pos;
        state->seen_mask |= 1U << pos;
        state->updates++;
        if (completed_cycle) {
            state->cycles++;
        }
    }

    unsigned *bucket = state->counts[pos];
    unsigned total = 0;
    for (int i = 0; i < 95; i++) {
        total += bucket[i];
    }
    if (total > 2000) {
        for (int i = 0; i < 95; i++) {
            bucket[i] = (bucket[i] + 1U) / 2U;
        }
    }
    bucket[ch - 32]++;

    unsigned best_count = 0;
    int best_ch = 32;
    for (int i = 0; i < 95; i++) {
        if (bucket[i] > best_count) {
            best_count = bucket[i];
            best_ch = i + 32;
        }
    }
    if (best_count < 3 || state->displayed[pos] == (char)best_ch) {
        return;
    }

    state->displayed[pos] = (char)best_ch;
}
static int descramble_payload_phase(const uint8_t *frame, uint8_t payload[PAYLOAD_BITS], int scramble_phase) {
    if (faw_errors_at(frame, 0) != 0) {
        return -1;
    }
    for (int i = 0; i < PAYLOAD_BITS; i++) {
        int idx = scramble_phase + i;
        if (idx >= BODY_BITS) {
            idx %= BODY_BITS;
        }
        payload[i] = frame[24 + i] ^ scramble[idx];
    }
    return 0;
}

static void descramble_serial_body_variant(const uint8_t *body, uint8_t *out, int variant);

static int descramble_body_phase(const uint8_t *frame, uint8_t body[BODY_BITS], int scramble_phase) {
    if (faw_errors_at(frame, 0) != 0) {
        return -1;
    }
    for (int i = 0; i < BODY_BITS; i++) {
        int idx = scramble_phase + i;
        if (idx >= BODY_BITS) {
            idx %= BODY_BITS;
        }
        body[i] = frame[8 + i] ^ scramble[idx];
    }
    return 0;
}

static int descramble_body(const uint8_t *frame, uint8_t body[BODY_BITS]) {
    return descramble_body_phase(frame, body, 0);
}

static int range_to_shift(int range_word) {
    switch (range_word & 7) {
        case 7:
            return 4;
        case 6:
            return 3;
        case 5:
            return 2;
        case 3:
            return 1;
        default:
            return 0;
    }
}

static int clamp_int(int value, int lo, int hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static int gcd_int(int a, int b) {
    if (a < 0) {
        a = -a;
    }
    if (b < 0) {
        b = -b;
    }
    while (b != 0) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static void deinterleave_words(const uint8_t *tx, uint8_t words[64][11]) {
    for (int word = 0; word < 64; word++) {
        for (int bit = 0; bit < 11; bit++) {
            int raw_index = word * 11 + bit;
            int row = raw_index % 44;
            int col = raw_index / 44;
            words[word][bit] = tx[row * 16 + col] & 1;
        }
    }
}

static void deinterleave_sound_words(const uint8_t *body, uint8_t words[64][11]) {
    deinterleave_words(body + 16, words);
}

static void read_words_ram_affine(const uint8_t *body, uint8_t words[64][11], int start, int stride) {
    const uint8_t *ram = body + 16;
    for (int word = 0; word < 64; word++) {
        for (int bit = 0; bit < 11; bit++) {
            int i = word * 11 + bit;
            int idx = start + i * stride;
            idx %= 704;
            if (idx < 0) {
                idx += 704;
            }
            words[word][bit] = ram[idx] & 1U;
        }
    }
}

static void deinterleave_words_variant(
    const uint8_t *tx,
    uint8_t words[64][11],
    int transpose,
    int reverse_payload,
    int reverse_words,
    int reverse_bits,
    int invert
) {
    for (int word = 0; word < 64; word++) {
        int src_word = reverse_words ? (63 - word) : word;
        for (int bit = 0; bit < 11; bit++) {
            int src_bit = reverse_bits ? (10 - bit) : bit;
            int raw_index = src_word * 11 + src_bit;
            if (reverse_payload) {
                raw_index = 703 - raw_index;
            }
            int row = raw_index % 44;
            int col = raw_index / 44;
            int index = transpose ? (col * 44 + row) : (row * 16 + col);
            words[word][bit] = (tx[index] ^ (invert ? 1U : 0U)) & 1U;
        }
    }
}

static void reverse_bits_per_16_words(const uint8_t *in, uint8_t *out) {
    for (int word = 0; word < 44; word++) {
        for (int bit = 0; bit < 16; bit++) {
            out[word * 16 + bit] = in[word * 16 + (15 - bit)];
        }
    }
}

static int parity_syndrome(const uint8_t word[11]) {
    int parity = word[10] & 1;
    for (int i = 4; i < 10; i++) {
        parity ^= word[i] & 1;
    }
    return parity & 1;
}

static int decode_ranges_and_errors(const uint8_t words[64][11], int *left, int *right);

static int phase_from_bits(uint8_t a, uint8_t b) {
    if (a == 0 && b == 0) {
        return 0;
    }
    if (a == 0 && b == 1) {
        return 3;
    }
    if (a == 1 && b == 1) {
        return 2;
    }
    return 1;
}

static void reverse_pattern_bits(const uint8_t in[8], uint8_t out[8]) {
    for (int i = 0; i < 8; i++) {
        out[i] = in[7 - i];
    }
}

static void invert_pattern_bits(const uint8_t in[8], uint8_t out[8]) {
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(in[i] ^ 1U);
    }
}

static void describe_pattern(const uint8_t pattern[8], char *buf, size_t buf_len) {
    size_t pos = 0;
    for (int i = 0; i < 8 && pos + 1 < buf_len; i++) {
        buf[pos++] = pattern[i] ? '1' : '0';
    }
    if (buf_len > 0) {
        buf[pos < buf_len ? pos : buf_len - 1] = '\0';
    }
}

static void describe_bits(const uint8_t *bits, size_t count, char *buf, size_t buf_len) {
    size_t pos = 0;
    for (size_t i = 0; i < count && pos + 1 < buf_len; i++) {
        buf[pos++] = bits[i] ? '1' : '0';
    }
    if (buf_len > 0) {
        buf[pos < buf_len ? pos : buf_len - 1] = '\0';
    }
}

#if NICAM_ENABLE_LEGACY_DEMOD
static int hamming_distance_bits(const uint8_t *a, const uint8_t *b, size_t count) {
    int dist = 0;
    for (size_t i = 0; i < count; i++) {
        dist += ((a[i] ^ b[i]) & 1U) != 0;
    }
    return dist;
}
#endif

static void describe_ctrl16(const uint8_t ctrl16[16], char *cbits, size_t cbits_len, char *abits, size_t abits_len) {
    describe_bits(ctrl16, 5, cbits, cbits_len);
    describe_bits(ctrl16 + 5, 11, abits, abits_len);
}

static int ctrl_mode(const uint8_t ctrl16[16]) {
    return ((ctrl16[1] & 1U) << 2) | ((ctrl16[2] & 1U) << 1) | (ctrl16[3] & 1U);
}

static int ctrl_mode_supported_for_pcm(const uint8_t ctrl16[16]) {
    return ctrl_mode(ctrl16) == 0;
}

static int c0_expected_for_phase(int phase, int frame_index) {
    int pos = (phase + frame_index) & 15;
    return pos < 8 ? 1 : 0;
}

#if NICAM_ENABLE_LEGACY_DEMOD
static int c0_pattern_errors(const uint8_t *bits, size_t len, size_t offset, const Config *cfg, int *frames_out) {
    uint8_t body[BODY_BITS];
    int c0[16];
    int frames = 0;
    for (; frames < 16; frames++) {
        size_t pos = offset + (size_t)frames * FRAME_BITS;
        if (pos + FRAME_BITS > len || faw_errors_at(bits, pos) != 0) {
            break;
        }
        if (descramble_body_phase(bits + pos, body, cfg->scramble_phase) != 0) {
            break;
        }
        c0[frames] = body[0] & 1U;
    }
    if (frames_out != NULL) {
        *frames_out = frames;
    }
    if (frames < 4) {
        return frames;
    }

    int best = frames;
    for (int phase = 0; phase < 16; phase++) {
        int errors = 0;
        for (int frame = 0; frame < frames; frame++) {
            errors += c0[frame] != c0_expected_for_phase(phase, frame);
        }
        if (errors < best) {
            best = errors;
        }
    }
    return best;
}
#endif
static int payload_parity_errors_at(const uint8_t *bits, size_t len, size_t offset, size_t *frames_checked) {
    *frames_checked = 0;
    int total = 0;
    const size_t max_frames_eval = 32;
    for (size_t pos = offset, frame = 0; pos + FRAME_BITS <= len && frame < max_frames_eval; pos += FRAME_BITS, frame++) {
        if (faw_errors_at(bits, pos) != 0) {
            break;
        }
        uint8_t body[BODY_BITS];
        if (descramble_body(bits + pos, body) != 0) {
            break;
        }
        uint8_t words[64][11];
        int left = 0;
        int right = 0;
        deinterleave_sound_words(body, words);
        total += decode_ranges_and_errors(words, &left, &right);
        (*frames_checked)++;
    }
    return total;
}

static void normalize_body_720(const uint8_t *body, uint8_t *out, int transpose, int row_reverse, int col_reverse, int body_reverse, int invert) {
    for (int row = 0; row < 45; row++) {
        for (int col = 0; col < 16; col++) {
            int sr = row_reverse ? (44 - row) : row;
            int sc = col_reverse ? (15 - col) : col;
            int src_idx = transpose ? (sc * 45 + sr) : (sr * 16 + sc);
            if (body_reverse) {
                src_idx = 719 - src_idx;
            }
            out[row * 16 + col] = (body[src_idx] ^ (invert ? 1U : 0U)) & 1U;
        }
    }
}

static void descramble_linear_phase(const uint8_t *in, size_t len, int phase, uint8_t *out) {
    for (size_t i = 0; i < len; i++) {
        out[i] = in[i] ^ scramble[(phase + (int)i) % BODY_BITS];
    }
}

enum {
    PRBS_VARIANT_LSB_RSHIFT = 0,
    PRBS_VARIANT_MSB_LSHIFT = 1,
    PRBS_VARIANT_LSB_LSHIFT = 2,
    PRBS_VARIANT_MSB_RSHIFT = 3,
};

static void generate_prbs_variant(int variant, uint8_t out[BODY_BITS]) {
    uint16_t reg = 0x1FF;
    for (int i = 0; i < BODY_BITS; i++) {
        switch (variant) {
            case PRBS_VARIANT_MSB_LSHIFT:
                out[i] = (uint8_t)((reg >> 8) & 1U);
                reg = (uint16_t)(((reg << 1) & 0x1FFU) | ((((reg >> 8) ^ (reg >> 3)) & 1U)));
                break;
            case PRBS_VARIANT_LSB_LSHIFT:
                out[i] = (uint8_t)((reg >> 0) & 1U);
                reg = (uint16_t)(((reg << 1) & 0x1FFU) | ((((reg >> 8) ^ (reg >> 3)) & 1U)));
                break;
            case PRBS_VARIANT_MSB_RSHIFT:
                out[i] = (uint8_t)((reg >> 8) & 1U);
                reg = (uint16_t)((reg >> 1) | (((((reg >> 0) ^ (reg >> 5)) & 1U)) << 8));
                break;
            case PRBS_VARIANT_LSB_RSHIFT:
            default:
                out[i] = (uint8_t)((reg >> 0) & 1U);
                reg = (uint16_t)((reg >> 1) | (((((reg >> 0) ^ (reg >> 5)) & 1U)) << 8));
                break;
        }
    }
}

static void descramble_serial_body_variant(const uint8_t *body, uint8_t *out, int variant) {
    uint8_t prbs[BODY_BITS];
    generate_prbs_variant(variant, prbs);
    for (int i = 0; i < BODY_BITS; i++) {
        out[i] = body[i] ^ prbs[i];
    }
}

static void word_parity_error_profile(const uint8_t words[64][11], int per_word[64], int *total) {
    int sum = 0;
    for (int word = 0; word < 64; word++) {
        int err = parity_syndrome(words[word]);
        per_word[word] = err;
        sum += err;
    }
    *total = sum;
}

static int decode_ranges_and_errors_profile(const uint8_t words[64][11], int *left, int *right, int group_err[8]) {
    static const int masks[6] = {4, 4, 2, 2, 1, 1};
    int ranges[2] = {0, 0};
    int errors = 0;
    if (group_err != NULL) {
        for (int i = 0; i < 8; i++) {
            group_err[i] = 0;
        }
    }
    for (int start = 0; start < 6; start++) {
        int syndromes[9];
        int count = 0;
        int group_errors = 0;
        for (int idx = 0, word = start; word < 54; word += 6, idx++) {
            syndromes[idx] = parity_syndrome(words[word]);
            count += syndromes[idx];
        }
        int bit = count >= 5;
        ranges[start & 1] |= bit * masks[start];
        for (int idx = 0; idx < 9; idx++) {
            group_errors += syndromes[idx] != bit;
        }
        if (group_err != NULL) {
            group_err[start] = group_errors;
        }
        errors += group_errors;
    }
    for (int start = 54, tail_group = 6; start <= 59; start += 5, tail_group++) {
        int syndromes[5];
        int count = 0;
        int group_errors = 0;
        for (int idx = 0; idx < 5; idx++) {
            syndromes[idx] = parity_syndrome(words[start + idx]);
            count += syndromes[idx];
        }
        int bit = count >= 3;
        for (int idx = 0; idx < 5; idx++) {
            group_errors += syndromes[idx] != bit;
        }
        if (group_err != NULL) {
            group_err[tail_group] = group_errors;
        }
        errors += group_errors;
    }
    *left = ranges[0];
    *right = ranges[1];
    return errors;
}

static int decode_payload_variant(
    const uint8_t payload[PAYLOAD_BITS],
    int transpose,
    int reverse_16,
    int reverse_payload,
    int reverse_words,
    int reverse_bits,
    int invert,
    int *left,
    int *right
);

static void body_descramble_quality_report(const BitBuffer *bb, size_t offset, const Config *cfg) {
    const size_t max_frames_eval = 32;
    uint8_t body[BODY_BITS];
    int c0[32];
    int mode_hist[8] = {0};
    size_t frames = 0;
    int best_c0_errors = 32;
    int best_c0_phase = 0;
    char raw16[32];
    char ctrl16[32];
    char cbits[8];
    char abits[16];

    memset(c0, 0, sizeof(c0));
    memset(raw16, 0, sizeof(raw16));
    memset(ctrl16, 0, sizeof(ctrl16));
    memset(cbits, 0, sizeof(cbits));
    memset(abits, 0, sizeof(abits));

    for (size_t frame = 0; frame < max_frames_eval; frame++) {
        size_t pos = offset + frame * FRAME_BITS;
        if (pos + FRAME_BITS > bb->len || faw_errors_at(bb->bits, pos) != 0) {
            break;
        }
        if (descramble_body_phase(bb->bits + pos, body, cfg->scramble_phase) != 0) {
            break;
        }
        if (frame == 0) {
            describe_bits(bb->bits + pos + 8, 16, raw16, sizeof(raw16));
            describe_bits(body, 16, ctrl16, sizeof(ctrl16));
            describe_ctrl16(body, cbits, sizeof(cbits), abits, sizeof(abits));
        }
        c0[frames] = body[0] & 1U;
        mode_hist[ctrl_mode(body)]++;
        frames++;
    }

    if (frames > 0) {
        for (int phase = 0; phase < 16; phase++) {
            int errors = 0;
            for (size_t frame = 0; frame < frames; frame++) {
                errors += c0[frame] != c0_expected_for_phase(phase, (int)frame);
            }
            if (errors < best_c0_errors) {
                best_c0_errors = errors;
                best_c0_phase = phase;
            }
        }
    }

    fprintf(stderr,
            "quality: body_descramble offset=%zu frames=%zu phase=%d raw16=%s ctrl16=%s C=%s AD=%s c0_phase=%d c0_errors=%d\n",
            offset,
            frames,
            cfg->scramble_phase,
            raw16,
            ctrl16,
            cbits,
            abits,
            best_c0_phase,
            frames > 0 ? best_c0_errors : -1);
    fprintf(stderr,
            "quality: body_descramble mode_hist=%d/%d/%d/%d/%d/%d/%d/%d\n",
            mode_hist[0], mode_hist[1], mode_hist[2], mode_hist[3],
            mode_hist[4], mode_hist[5], mode_hist[6], mode_hist[7]);
}

static void bitstream_quality_report(const HypothesisState *hyp, const Config *cfg) {
    if (hyp == NULL) {
        fprintf(stderr, "quality: geen hypothese beschikbaar\n");
        return;
    }
    const BitBuffer *bb = &hyp->bits;
    if (bb->len < FRAME_BITS) {
        fprintf(stderr, "quality: te weinig bits: %zu\n", bb->len);
        return;
    }

    size_t q_hist[4] = {0, 0, 0, 0};
    size_t delta_hist[4] = {0, 0, 0, 0};
    size_t symbols = bb->len / 2;
    int prev_q = -1;
    for (size_t i = 0; i + 1 < bb->len; i += 2) {
        int q = phase_from_bits(bb->bits[i], bb->bits[i + 1]);
        q_hist[(size_t)q]++;
        if (prev_q >= 0) {
            delta_hist[(size_t)((q - prev_q) & 3)]++;
        }
        prev_q = q;
    }

    uint8_t patterns[4][8];
    uint8_t inverted[8];
    uint8_t reversed[8];
    uint8_t reversed_inverted[8];
    memcpy(patterns[0], FAW, sizeof(FAW));
    invert_pattern_bits(FAW, inverted);
    reverse_pattern_bits(FAW, reversed);
    reverse_pattern_bits(inverted, reversed_inverted);
    memcpy(patterns[1], inverted, sizeof(inverted));
    memcpy(patterns[2], reversed, sizeof(reversed));
    memcpy(patterns[3], reversed_inverted, sizeof(reversed_inverted));
    const char *labels[4] = {"raw", "inverted", "reversed", "reversed+inverted"};

    fprintf(stderr,
            "quality: hyp phase=%d timing=%d reverse=%d invert=%d rot=%d swap=%d bits=%zu symbols=%zu\n",
            hyp->phase, hyp->timing_idx, hyp->reverse, hyp->invert, hyp->rotation, hyp->swap_bits, bb->len, symbols);
    fprintf(stderr,
            "quality: q_hist=%zu/%zu/%zu/%zu delta_hist=%zu/%zu/%zu/%zu\n",
            q_hist[0], q_hist[1], q_hist[2], q_hist[3],
            delta_hist[0], delta_hist[1], delta_hist[2], delta_hist[3]);

    PatternScan raw_scan = scan_pattern(bb->bits, bb->len, patterns[0]);
    for (int i = 0; i < 4; i++) {
        PatternScan scan = scan_pattern(bb->bits, bb->len, patterns[i]);
        char patbuf[16];
        describe_pattern(patterns[i], patbuf, sizeof(patbuf));
        fprintf(stderr,
                "quality: faw_%s pattern=%s best_offset=%zd frames=%zu hits=%zu error_sum=%d\n",
                labels[i], patbuf, scan.best_offset, scan.frames_checked, scan.hits, scan.best_error_sum);
    }

    size_t best_frames = 0;
    size_t payload_offset = raw_scan.best_offset >= 0 ? (size_t)raw_scan.best_offset : 0;
    body_descramble_quality_report(bb, payload_offset, cfg);
    int best_payload_errors = payload_parity_errors_at(bb->bits, bb->len, payload_offset, &best_frames);
    fprintf(stderr,
            "quality: payload-after-descramble offset=%zu frames=%zu parity_errors=%d\n",
            payload_offset, best_frames, best_payload_errors);

    {
        static const struct {
            const char *name;
            int transpose;
            int reverse_16;
            int reverse_payload;
            int reverse_words;
            int reverse_bits;
            int invert;
        } variants[] = {
            {"normal", 0, 0, 0, 0, 0, 0},
            {"transpose", 1, 0, 0, 0, 0, 0},
            {"invert", 0, 0, 0, 0, 0, 1},
            {"transpose+invert", 1, 0, 0, 0, 0, 1},
            {"bitrev", 0, 0, 0, 0, 1, 0},
            {"transpose+bitrev", 1, 0, 0, 0, 1, 0},
            {"bitrev+invert", 0, 0, 0, 0, 1, 1},
            {"transpose+bitrev+invert", 1, 0, 0, 0, 1, 1},
            {"wordrev", 0, 0, 0, 1, 0, 0},
            {"transpose+wordrev", 1, 0, 0, 1, 0, 0},
            {"wordrev+invert", 0, 0, 0, 1, 0, 1},
            {"transpose+wordrev+invert", 1, 0, 0, 1, 0, 1},
            {"payloadrev", 0, 0, 1, 0, 0, 0},
            {"transpose+payloadrev", 1, 0, 1, 0, 0, 0},
            {"payloadrev+invert", 0, 0, 1, 0, 0, 1},
            {"transpose+payloadrev+invert", 1, 0, 1, 0, 0, 1},
            {"word16bitrev", 0, 1, 0, 0, 0, 0},
            {"word16bitrev+transpose", 1, 1, 0, 0, 0, 0},
            {"word16bitrev+invert", 0, 1, 0, 0, 0, 1},
            {"word16bitrev+transpose+invert", 1, 1, 0, 0, 0, 1},
        };
        const size_t max_frames_eval = 8;
        size_t best_variant_idx = 0;
        int best_variant_errors = INT_MAX;
        size_t best_variant_frames = 0;
        for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
            int total_errors = 0;
            size_t frames_checked = 0;
            int left0 = 0;
            int right0 = 0;
            int have_lr = 0;
            for (size_t pos = payload_offset, frame = 0; pos + FRAME_BITS <= bb->len && frame < max_frames_eval; pos += FRAME_BITS, frame++) {
                if (faw_errors_at(bb->bits, pos) != 0) {
                    break;
                }
                uint8_t payload[PAYLOAD_BITS];
                int payload_scramble_phase = (cfg->scramble_phase + 16) % BODY_BITS;
                if (descramble_payload_phase(bb->bits + pos, payload, payload_scramble_phase) != 0) {
                    break;
                }
                int left = 0;
                int right = 0;
                int errors = decode_payload_variant(
                    payload,
                    variants[v].transpose,
                    variants[v].reverse_16,
                    variants[v].reverse_payload,
                    variants[v].reverse_words,
                    variants[v].reverse_bits,
                    variants[v].invert,
                    &left,
                    &right);
                total_errors += errors;
                if (!have_lr) {
                    left0 = left;
                    right0 = right;
                    have_lr = 1;
                }
                frames_checked++;
            }
            fprintf(stderr,
                    "quality: payload_variant=%s frames=%zu parity_errors=%d left=%d right=%d\n",
                    variants[v].name, frames_checked, total_errors, left0, right0);
            if (frames_checked > 0 && total_errors < best_variant_errors) {
                best_variant_errors = total_errors;
                best_variant_idx = v;
                best_variant_frames = frames_checked;
            }
        }

        if (best_variant_errors < INT_MAX) {
            int best_phase = 16;
            int best_phase_errors = INT_MAX;
            int best_phase_left = 0;
            int best_phase_right = 0;
            for (int scramble_phase = 0; scramble_phase < BODY_BITS; scramble_phase++) {
                int total_errors = 0;
                size_t frames_checked = 0;
                int left0 = 0;
                int right0 = 0;
                int have_lr = 0;
                for (size_t pos = payload_offset, frame = 0; pos + FRAME_BITS <= bb->len && frame < max_frames_eval; pos += FRAME_BITS, frame++) {
                    if (faw_errors_at(bb->bits, pos) != 0) {
                        break;
                    }
                    uint8_t payload[PAYLOAD_BITS];
                    if (descramble_payload_phase(bb->bits + pos, payload, scramble_phase) != 0) {
                        break;
                    }
                    int left = 0;
                    int right = 0;
                    int errors = decode_payload_variant(
                        payload,
                        variants[best_variant_idx].transpose,
                        variants[best_variant_idx].reverse_16,
                        variants[best_variant_idx].reverse_payload,
                        variants[best_variant_idx].reverse_words,
                        variants[best_variant_idx].reverse_bits,
                        variants[best_variant_idx].invert,
                        &left,
                        &right);
                    total_errors += errors;
                    if (!have_lr) {
                        left0 = left;
                        right0 = right;
                        have_lr = 1;
                    }
                    frames_checked++;
                }
                if (frames_checked > 0 && total_errors < best_phase_errors) {
                    best_phase_errors = total_errors;
                    best_phase = scramble_phase;
                    best_phase_left = left0;
                    best_phase_right = right0;
                }
            }
            fprintf(stderr,
                    "quality: descramble_phase best_variant=%s phase=%d frames=%zu parity_errors=%d left=%d right=%d\n",
                    variants[best_variant_idx].name, best_phase, best_variant_frames, best_phase_errors, best_phase_left, best_phase_right);
        }

        const size_t max_geom_frames = 4;
        int best_geom_total = INT_MAX;
        int best_geom_profile[64] = {0};
        int best_geom_col_reverse = 0;
        int best_geom_row_reverse = 0;
        int best_geom_body_reverse = 0;
        int best_geom_invert = 0;
        int best_geom_offset = 0;
        for (int row_reverse = 0; row_reverse < 2; row_reverse++) {
            for (int col_reverse = 0; col_reverse < 2; col_reverse++) {
                for (int body_reverse = 0; body_reverse < 2; body_reverse++) {
                    for (int invert = 0; invert < 2; invert++) {
                        for (int offset = 0; offset < 16; offset++) {
                            int total_errors = 0;
                            int profile[64] = {0};
                            size_t frames_checked = 0;
                            for (size_t pos = payload_offset, frame = 0; pos + FRAME_BITS <= bb->len && frame < max_geom_frames; pos += FRAME_BITS, frame++) {
                                if (faw_errors_at(bb->bits, pos) != 0) {
                                    break;
                                }
                                const uint8_t *body = bb->bits + pos + 8;
                                uint8_t norm720[720];
                                uint8_t descrambled[PAYLOAD_BITS];
                                uint8_t words[64][11];
                                normalize_body_720(body, norm720, 1, row_reverse, col_reverse, body_reverse, invert);
                                descramble_linear_phase(norm720 + offset, PAYLOAD_BITS, 0, descrambled);
                                deinterleave_words(descrambled, words);
                                int left = 0;
                                int right = 0;
                                int errors = decode_ranges_and_errors(words, &left, &right);
                                total_errors += errors;
                                int per_word[64];
                                int word_total = 0;
                                word_parity_error_profile(words, per_word, &word_total);
                                for (int w = 0; w < 64; w++) {
                                    profile[w] += per_word[w];
                                }
                                frames_checked++;
                            }
                            if (frames_checked > 0 && total_errors < best_geom_total) {
                                best_geom_total = total_errors;
                                memcpy(best_geom_profile, profile, sizeof(best_geom_profile));
                                best_geom_row_reverse = row_reverse;
                                best_geom_col_reverse = col_reverse;
                                best_geom_body_reverse = body_reverse;
                                best_geom_invert = invert;
                                best_geom_offset = offset;
                            }
                        }
                    }
                }
            }
        }
        fprintf(stderr,
                "quality: geom45x16 rowrev=%d colrev=%d bodyrev=%d invert=%d offset=%d parity_errors=%d\n",
                best_geom_row_reverse, best_geom_col_reverse, best_geom_body_reverse, best_geom_invert, best_geom_offset, best_geom_total);
        for (int w = 0; w < 64; w++) {
            if (best_geom_profile[w] != 0) {
                fprintf(stderr, "quality: word_err[%d]=%d\n", w, best_geom_profile[w]);
            }
        }

        {
            const size_t max_spec_frames = 8;
            static const char *prbs_names[4] = {"lsb_rshift", "msb_lshift", "lsb_lshift", "msb_rshift"};
            int prbs_best_pre[4] = {INT_MAX, INT_MAX, INT_MAX, INT_MAX};
            int prbs_best_post[4] = {INT_MAX, INT_MAX, INT_MAX, INT_MAX};
            int prbs_best_delta[4] = {INT_MAX, INT_MAX, INT_MAX, INT_MAX};
            int prbs_best_row[4] = {0};
            int prbs_best_col[4] = {0};
            int prbs_best_body[4] = {0};
            int prbs_best_invert[4] = {0};
            int prbs_best_offset[4] = {0};
            int prbs_best_group[4][8];
            int prbs_best_word[4][64];
            int prbs_best_frame_pre[4][8];
            int prbs_best_frame_post[4][8];
            int prbs_best_left[4][8];
            int prbs_best_right[4][8];
            memset(prbs_best_group, 0, sizeof(prbs_best_group));
            memset(prbs_best_word, 0, sizeof(prbs_best_word));
            memset(prbs_best_frame_pre, 0, sizeof(prbs_best_frame_pre));
            memset(prbs_best_frame_post, 0, sizeof(prbs_best_frame_post));
            memset(prbs_best_left, 0, sizeof(prbs_best_left));
            memset(prbs_best_right, 0, sizeof(prbs_best_right));

            for (int row_reverse = 0; row_reverse < 2; row_reverse++) {
                for (int col_reverse = 0; col_reverse < 2; col_reverse++) {
                    for (int body_reverse = 0; body_reverse < 2; body_reverse++) {
                        for (int invert = 0; invert < 2; invert++) {
                            for (int offset = 0; offset < 16; offset++) {
                                for (int prbs_variant = 0; prbs_variant < 4; prbs_variant++) {
                                    int total_pre = 0;
                                    int total_post = 0;
                                    int group_acc[8] = {0};
                                    int word_acc[64] = {0};
                                    int frame_pre[8] = {0};
                                    int frame_post[8] = {0};
                                    int frame_left[8] = {0};
                                    int frame_right[8] = {0};
                                    size_t frames_checked = 0;
                                    for (size_t pos = payload_offset, frame = 0; pos + FRAME_BITS <= bb->len && frame < max_spec_frames; pos += FRAME_BITS, frame++) {
                                        if (faw_errors_at(bb->bits, pos) != 0) {
                                            break;
                                        }
                                        const uint8_t *body = bb->bits + pos + 8;
                                        uint8_t body_descr[720];
                                        uint8_t norm_raw[720];
                                        uint8_t norm_descr[720];
                                        uint8_t payload_raw[PAYLOAD_BITS];
                                        uint8_t payload_descr[PAYLOAD_BITS];
                                        uint8_t words[64][11];
                                        int per_word[64];
                                        int groups[8];
                                        int left = 0;
                                        int right = 0;
                                        normalize_body_720(body, norm_raw, 1, row_reverse, col_reverse, body_reverse, invert);
                                        memcpy(payload_raw, norm_raw + offset, PAYLOAD_BITS);
                                        deinterleave_words(payload_raw, words);
                                        frame_pre[frame] = decode_ranges_and_errors_profile(words, &left, &right, NULL);
                                        total_pre += frame_pre[frame];

                                        descramble_serial_body_variant(body, body_descr, prbs_variant);
                                        normalize_body_720(body_descr, norm_descr, 1, row_reverse, col_reverse, body_reverse, invert);
                                        memcpy(payload_descr, norm_descr + offset, PAYLOAD_BITS);
                                        deinterleave_words(payload_descr, words);
                                        frame_post[frame] = decode_ranges_and_errors_profile(words, &left, &right, groups);
                                        total_post += frame_post[frame];
                                        frame_left[frame] = left;
                                        frame_right[frame] = right;
                                        word_parity_error_profile(words, per_word, &frame_post[frame]);
                                        for (int w = 0; w < 64; w++) {
                                            word_acc[w] += per_word[w];
                                        }
                                        for (int g = 0; g < 8; g++) {
                                            group_acc[g] += groups[g];
                                        }
                                        frames_checked++;
                                    }
                                    if (frames_checked > 0) {
                                        int delta = total_post - total_pre;
                                        if (total_post < prbs_best_post[prbs_variant] ||
                                            (total_post == prbs_best_post[prbs_variant] && delta < prbs_best_delta[prbs_variant])) {
                                            prbs_best_pre[prbs_variant] = total_pre;
                                            prbs_best_post[prbs_variant] = total_post;
                                            prbs_best_delta[prbs_variant] = delta;
                                            prbs_best_row[prbs_variant] = row_reverse;
                                            prbs_best_col[prbs_variant] = col_reverse;
                                            prbs_best_body[prbs_variant] = body_reverse;
                                            prbs_best_invert[prbs_variant] = invert;
                                            prbs_best_offset[prbs_variant] = offset;
                                            memcpy(prbs_best_group[prbs_variant], group_acc, sizeof(group_acc));
                                            memcpy(prbs_best_word[prbs_variant], word_acc, sizeof(word_acc));
                                            memcpy(prbs_best_frame_pre[prbs_variant], frame_pre, sizeof(frame_pre));
                                            memcpy(prbs_best_frame_post[prbs_variant], frame_post, sizeof(frame_post));
                                            memcpy(prbs_best_left[prbs_variant], frame_left, sizeof(frame_left));
                                            memcpy(prbs_best_right[prbs_variant], frame_right, sizeof(frame_right));
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

        for (int v = 0; v < 4; v++) {
            fprintf(stderr,
                    "quality: prbs_variant=%s pre=%d post=%d delta=%d rowrev=%d colrev=%d bodyrev=%d invert=%d offset=%d\n",
                    prbs_names[v], prbs_best_pre[v], prbs_best_post[v], prbs_best_delta[v],
                    prbs_best_row[v], prbs_best_col[v], prbs_best_body[v], prbs_best_invert[v], prbs_best_offset[v]);
                for (size_t frame = 0; frame < max_spec_frames; frame++) {
                    if (prbs_best_frame_pre[v][frame] == 0 && prbs_best_frame_post[v][frame] == 0) {
                        continue;
                    }
                    fprintf(stderr,
                            "quality: prbs_variant=%s frame=%zu pre=%d post=%d left=%d right=%d\n",
                            prbs_names[v], frame, prbs_best_frame_pre[v][frame], prbs_best_frame_post[v][frame], prbs_best_left[v][frame], prbs_best_right[v][frame]);
                }
                for (int g = 0; g < 8; g++) {
                    if (prbs_best_group[v][g] != 0) {
                        fprintf(stderr, "quality: prbs_variant=%s group_err[%d]=%d\n", prbs_names[v], g, prbs_best_group[v][g]);
                    }
                }
                for (int w = 0; w < 64; w++) {
                    if (prbs_best_word[v][w] != 0) {
                        fprintf(stderr, "quality: prbs_variant=%s word_err[%d]=%d\n", prbs_names[v], w, prbs_best_word[v][w]);
                    }
                }
            }
        }

        {
            const size_t stability_slice_frames = 8;
            const size_t stability_max_slices = 4;
            const size_t stability_candidates = 4 * 2 * 2 * 2 * 2 * 16;
            static const char *prbs_names[4] = {"lsb_rshift", "msb_lshift", "lsb_lshift", "msb_rshift"};
            int win_counts[4 * 16 * 16] = {0};
            size_t total_slices = 0;
            size_t stable_post_better = 0;
            size_t stable_near_zero_frames = 0;
            int best_global_post = INT_MAX;
            int best_global_delta = INT_MAX;
            int best_global_prbs = 0;
            int best_global_row = 0;
            int best_global_col = 0;
            int best_global_body = 0;
            int best_global_invert = 0;
            int best_global_offset = 0;

            for (size_t slice = 0; slice < stability_max_slices; slice++) {
                size_t slice_start_frame = slice * stability_slice_frames;
                size_t slice_end_frame = slice_start_frame + stability_slice_frames;
                int best_post = INT_MAX;
                int best_pre = INT_MAX;
                int best_delta = INT_MAX;
                int best_prbs = 0;
                int best_row = 0;
                int best_col = 0;
                int best_body = 0;
                int best_invert = 0;
                int best_offset = 0;
                int best_frame_pre[8] = {0};
                int best_frame_post[8] = {0};
                int best_frame_left[8] = {0};
                int best_frame_right[8] = {0};
                uint8_t best_frame_ctrl[8][16];
                int best_group[8] = {0};
                int best_word[64] = {0};
                size_t frames_checked = 0;
                memset(best_frame_ctrl, 0, sizeof(best_frame_ctrl));

                for (int row_reverse = 0; row_reverse < 2; row_reverse++) {
                    for (int col_reverse = 0; col_reverse < 2; col_reverse++) {
                        for (int body_reverse = 0; body_reverse < 2; body_reverse++) {
                            for (int invert = 0; invert < 2; invert++) {
                                for (int offset = 0; offset < 16; offset++) {
                                    for (int prbs_variant = 0; prbs_variant < 4; prbs_variant++) {
                                        int total_pre = 0;
                                        int total_post = 0;
                                        int frame_pre[8] = {0};
                                        int frame_post[8] = {0};
                        int frame_left[8] = {0};
                        int frame_right[8] = {0};
                        uint8_t frame_ctrl[8][16];
                        int group_acc[8] = {0};
                        int word_acc[64] = {0};
                        size_t local_frames = 0;
                        memset(frame_ctrl, 0, sizeof(frame_ctrl));

                                        for (size_t frame = slice_start_frame; frame < slice_end_frame; frame++) {
                                            size_t pos = payload_offset + frame * FRAME_BITS;
                                            if (pos + FRAME_BITS > bb->len) {
                                                break;
                                            }
                                            if (faw_errors_at(bb->bits, pos) != 0) {
                                                break;
                                            }
                                            const uint8_t *body = bb->bits + pos + 8;
                                            uint8_t norm_raw[720];
                                            uint8_t body_descr[720];
                                            uint8_t norm_descr[720];
                                            uint8_t payload_raw[PAYLOAD_BITS];
                                            uint8_t payload_descr[PAYLOAD_BITS];
                                            uint8_t words[64][11];
                                            int per_word[64];
                                            int groups[8];
                                            int left = 0;
                                            int right = 0;

                                            normalize_body_720(body, norm_raw, 1, row_reverse, col_reverse, body_reverse, invert);
                                            memcpy(payload_raw, norm_raw + offset, PAYLOAD_BITS);
                                            deinterleave_words(payload_raw, words);
                                            frame_pre[local_frames] = decode_ranges_and_errors_profile(words, &left, &right, NULL);
                                            total_pre += frame_pre[local_frames];

                                            descramble_serial_body_variant(body, body_descr, prbs_variant);
                                            normalize_body_720(body_descr, norm_descr, 1, row_reverse, col_reverse, body_reverse, invert);
                                            memcpy(payload_descr, norm_descr + offset, PAYLOAD_BITS);
                                            deinterleave_words(payload_descr, words);
                                            frame_post[local_frames] = decode_ranges_and_errors_profile(words, &left, &right, groups);
                                            total_post += frame_post[local_frames];
                                            frame_left[local_frames] = left;
                                            frame_right[local_frames] = right;
                                            word_parity_error_profile(words, per_word, &frame_post[local_frames]);
                                            for (int w = 0; w < 64; w++) {
                                                word_acc[w] += per_word[w];
                                            }
                                            for (int g = 0; g < 8; g++) {
                                                group_acc[g] += groups[g];
                                            }
                                            local_frames++;
                                        }

                                        if (local_frames == 0) {
                                            continue;
                                        }
                                        frames_checked += local_frames;
                                        int delta = total_post - total_pre;
                                        if (total_post < best_post || (total_post == best_post && delta < best_delta)) {
                                            best_post = total_post;
                                            best_pre = total_pre;
                                            best_delta = delta;
                                            best_prbs = prbs_variant;
                                            best_row = row_reverse;
                                            best_col = col_reverse;
                                            best_body = body_reverse;
                                            best_invert = invert;
                                            best_offset = offset;
                                            memcpy(best_frame_pre, frame_pre, sizeof(best_frame_pre));
                                            memcpy(best_frame_post, frame_post, sizeof(best_frame_post));
                                            memcpy(best_frame_left, frame_left, sizeof(best_frame_left));
                                            memcpy(best_frame_right, frame_right, sizeof(best_frame_right));
                                            memcpy(best_group, group_acc, sizeof(best_group));
                                            memcpy(best_word, word_acc, sizeof(best_word));
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                if (frames_checked == 0) {
                    continue;
                }

                total_slices++;
                if (best_post < best_pre) {
                    stable_post_better++;
                }
                for (size_t i = 0; i < stability_slice_frames; i++) {
                    if (best_frame_post[i] > 0 && best_frame_post[i] <= 2) {
                        stable_near_zero_frames++;
                    }
                }

                int winner_index = ((((best_prbs * 2 + best_row) * 2 + best_col) * 2 + best_body) * 2 + best_invert) * 16 + best_offset;
                if (winner_index >= 0 && winner_index < (int)stability_candidates) {
                    win_counts[winner_index]++;
                }
                if (best_post < best_global_post || (best_post == best_global_post && best_delta < best_global_delta)) {
                    best_global_post = best_post;
                    best_global_delta = best_delta;
                    best_global_prbs = best_prbs;
                    best_global_row = best_row;
                    best_global_col = best_col;
                    best_global_body = best_body;
                    best_global_invert = best_invert;
                    best_global_offset = best_offset;
                }

                fprintf(stderr,
                        "quality: stability slice=%zu frames=%zu winner=prbs:%s rowrev=%d colrev=%d bodyrev=%d invert=%d offset=%d pre=%d post=%d delta=%d\n",
                        slice,
                        frames_checked,
                        prbs_names[best_prbs],
                        best_row,
                        best_col,
                        best_body,
                        best_invert,
                        best_offset,
                        best_pre,
                        best_post,
                        best_delta);
                for (size_t i = 0; i < stability_slice_frames && i < 8; i++) {
                    if (best_frame_pre[i] == 0 && best_frame_post[i] == 0) {
                        continue;
                    }
                    fprintf(stderr,
                            "quality: stability slice=%zu frame=%zu pre=%d post=%d left=%d right=%d\n",
                            slice,
                            i,
                            best_frame_pre[i],
                            best_frame_post[i],
                            best_frame_left[i],
                            best_frame_right[i]);
                }
                for (int g = 0; g < 8; g++) {
                    if (best_group[g] != 0) {
                        fprintf(stderr, "quality: stability slice=%zu group_err[%d]=%d\n", slice, g, best_group[g]);
                    }
                }
                for (int w = 0; w < 64; w++) {
                    if (best_word[w] != 0) {
                        fprintf(stderr, "quality: stability slice=%zu word_err[%d]=%d\n", slice, w, best_word[w]);
                    }
                }
            }

            fprintf(stderr,
                    "quality: stability summary slices=%zu post_better=%zu near_zero_frames=%zu best=prbs:%s rowrev=%d colrev=%d bodyrev=%d invert=%d offset=%d post=%d delta=%d\n",
                    total_slices,
                    stable_post_better,
                    stable_near_zero_frames,
                    prbs_names[best_global_prbs],
                    best_global_row,
                    best_global_col,
                    best_global_body,
                    best_global_invert,
                    best_global_offset,
                    best_global_post,
                    best_global_delta);
            for (int idx = 0; idx < (int)stability_candidates; idx++) {
                if (win_counts[idx] == 0) {
                    continue;
                }
                int rem = idx;
                int offset = rem % 16;
                rem /= 16;
                int invert = rem % 2;
                rem /= 2;
                int body_reverse = rem % 2;
                rem /= 2;
                int col_reverse = rem % 2;
                rem /= 2;
                int row_reverse = rem % 2;
                rem /= 2;
                int prbs_variant = rem;
                fprintf(stderr,
                        "quality: stability winner_count=%d prbs=%s rowrev=%d colrev=%d bodyrev=%d invert=%d offset=%d\n",
                        win_counts[idx],
                        prbs_names[prbs_variant],
                        row_reverse,
                        col_reverse,
                        body_reverse,
                        invert,
                        offset);
            }
        }

        {
            const size_t slice_frames = 8;
            const size_t max_slices = 4;
            static const char *prbs_names[4] = {"lsb_rshift", "msb_lshift", "lsb_lshift", "msb_rshift"};
            int win_counts[4] = {0};
            size_t total_slices = 0;
            size_t post_better_slices = 0;
            size_t near_zero_frames = 0;
            int best_global_post = INT_MAX;
            int best_global_delta = INT_MAX;
            int best_global_variant = 0;
            uint8_t best_raw16[16] = {0};
            uint8_t best_ctrl16[16] = {0};

            for (size_t slice = 0; slice < max_slices; slice++) {
                size_t start_frame = slice * slice_frames;
                int best_pre = INT_MAX;
                int best_post = INT_MAX;
                int best_delta = INT_MAX;
                int best_variant = 0;
                int best_frame_pre[8] = {0};
                int best_frame_post[8] = {0};
                int best_frame_left[8] = {0};
                int best_frame_right[8] = {0};
                uint8_t best_frame_ctrl[8][16];
                int best_group[8] = {0};
                int best_word[64] = {0};
                size_t frames_checked = 0;
                memset(best_frame_ctrl, 0, sizeof(best_frame_ctrl));

                for (int prbs_variant = 0; prbs_variant < 4; prbs_variant++) {
                    int total_pre = 0;
                    int total_post = 0;
                    int frame_pre[8] = {0};
                    int frame_post[8] = {0};
                    int frame_left[8] = {0};
                    int frame_right[8] = {0};
                    uint8_t frame_ctrl[8][16];
                    int group_acc[8] = {0};
                    int word_acc[64] = {0};
                    size_t local_frames = 0;
                    memset(frame_ctrl, 0, sizeof(frame_ctrl));

                    for (size_t frame = 0; frame < slice_frames; frame++) {
                        size_t pos = payload_offset + (start_frame + frame) * FRAME_BITS;
                        if (pos + FRAME_BITS > bb->len) {
                            break;
                        }
                        if (faw_errors_at(bb->bits, pos) != 0) {
                            break;
                        }
                        const uint8_t *body = bb->bits + pos + 8;
                        uint8_t body_descr[720];
                        uint8_t words_raw[64][11];
                        uint8_t words_post[64][11];
                        int per_word[64];
                        int groups[8];
                        int left = 0;
                        int right = 0;

                        deinterleave_sound_words(body, words_raw);
                        frame_pre[local_frames] = decode_ranges_and_errors_profile(words_raw, &left, &right, NULL);
                        total_pre += frame_pre[local_frames];

                        descramble_serial_body_variant(body, body_descr, prbs_variant);
                        deinterleave_sound_words(body_descr, words_post);
                        frame_post[local_frames] = decode_ranges_and_errors_profile(words_post, &left, &right, groups);
                        total_post += frame_post[local_frames];
                        frame_left[local_frames] = left;
                        frame_right[local_frames] = right;
                        memcpy(frame_ctrl[local_frames], body_descr, 16);
                        word_parity_error_profile(words_post, per_word, &frame_post[local_frames]);
                        for (int w = 0; w < 64; w++) {
                            word_acc[w] += per_word[w];
                        }
                        for (int g = 0; g < 8; g++) {
                            group_acc[g] += groups[g];
                        }
                        local_frames++;
                    }

                    if (local_frames == 0) {
                        continue;
                    }
                    frames_checked += local_frames;
                    int delta = total_post - total_pre;
                    if (total_post < best_post || (total_post == best_post && delta < best_delta)) {
                        best_pre = total_pre;
                        best_post = total_post;
                        best_delta = delta;
                        best_variant = prbs_variant;
                        if (local_frames > 0) {
                            size_t pos0 = payload_offset + start_frame * FRAME_BITS;
                            const uint8_t *body0 = bb->bits + pos0 + 8;
                            uint8_t body_descr0[720];
                            descramble_serial_body_variant(body0, body_descr0, prbs_variant);
                            memcpy(best_raw16, body0, 16);
                            memcpy(best_ctrl16, body_descr0, 16);
                        }
                        memcpy(best_frame_pre, frame_pre, sizeof(best_frame_pre));
                        memcpy(best_frame_post, frame_post, sizeof(best_frame_post));
                        memcpy(best_frame_left, frame_left, sizeof(best_frame_left));
                        memcpy(best_frame_right, frame_right, sizeof(best_frame_right));
                        memcpy(best_frame_ctrl, frame_ctrl, sizeof(best_frame_ctrl));
                        memcpy(best_group, group_acc, sizeof(best_group));
                        memcpy(best_word, word_acc, sizeof(best_word));
                    }
                }

                if (frames_checked == 0) {
                    continue;
                }

                total_slices++;
                if (best_post < best_pre) {
                    post_better_slices++;
                }
                for (size_t i = 0; i < slice_frames && i < 8; i++) {
                    if (best_frame_post[i] > 0 && best_frame_post[i] <= 2) {
                        near_zero_frames++;
                    }
                }
                win_counts[best_variant]++;
                if (best_post < best_global_post || (best_post == best_global_post && best_delta < best_global_delta)) {
                    best_global_post = best_post;
                    best_global_delta = best_delta;
                    best_global_variant = best_variant;
                }

                fprintf(stderr,
                        "quality: spec_stability slice=%zu frames=%zu winner=prbs:%s pre=%d post=%d delta=%d\n",
                        slice,
                        frames_checked,
                        prbs_names[best_variant],
                        best_pre,
                        best_post,
                        best_delta);
                for (size_t i = 0; i < slice_frames && i < 8; i++) {
                    if (best_frame_pre[i] == 0 && best_frame_post[i] == 0) {
                        continue;
                    }
                    char cbits[8];
                    char abits[16];
                    describe_bits(best_frame_ctrl[i], 5, cbits, sizeof(cbits));
                    describe_bits(best_frame_ctrl[i] + 5, 11, abits, sizeof(abits));
                    fprintf(stderr,
                            "quality: spec_stability slice=%zu frame=%zu pre=%d post=%d left=%d right=%d C=%s AD=%s\n",
                            slice,
                            i,
                            best_frame_pre[i],
                            best_frame_post[i],
                            best_frame_left[i],
                            best_frame_right[i],
                            cbits,
                            abits);
                }
                for (int g = 0; g < 8; g++) {
                    if (best_group[g] != 0) {
                        fprintf(stderr, "quality: spec_stability slice=%zu group_err[%d]=%d\n", slice, g, best_group[g]);
                    }
                }
                for (int w = 0; w < 64; w++) {
                    if (best_word[w] != 0) {
                        fprintf(stderr, "quality: spec_stability slice=%zu word_err[%d]=%d\n", slice, w, best_word[w]);
                    }
                }
            }

            fprintf(stderr,
                    "quality: spec_stability summary slices=%zu post_better=%zu near_zero_frames=%zu best=prbs:%s post=%d delta=%d\n",
                    total_slices,
                    post_better_slices,
                    near_zero_frames,
                    prbs_names[best_global_variant],
                    best_global_post,
                    best_global_delta);
            {
                char raw16_buf[32];
                char ctrl16_buf[32];
                describe_bits(best_raw16, 16, raw16_buf, sizeof(raw16_buf));
                describe_bits(best_ctrl16, 16, ctrl16_buf, sizeof(ctrl16_buf));
                fprintf(stderr, "quality: spec_stability raw16=%s ctrl16=%s\n", raw16_buf, ctrl16_buf);
            }
            for (int v = 0; v < 4; v++) {
                if (win_counts[v] != 0) {
                    fprintf(stderr, "quality: spec_stability winner_count=%d prbs=%s\n", win_counts[v], prbs_names[v]);
                }
            }
        }

        {
            const size_t ram_scan_frames = 4;
            int best_pre = INT_MAX;
            int best_post = INT_MAX;
            int best_delta = INT_MAX;
            int best_start = 0;
            int best_stride = 1;
            int best_frame_pre[4] = {0};
            int best_frame_post[4] = {0};
            int best_frame_left[4] = {0};
            int best_frame_right[4] = {0};
            uint8_t best_ctrl16[16] = {0};
            size_t frames_checked = 0;
            size_t strides_checked = 0;

            for (int stride = 1; stride < 704; stride++) {
                if (gcd_int(stride, 704) != 1) {
                    continue;
                }
                strides_checked++;
                for (int start = 0; start < 704; start++) {
                    int total_pre = 0;
                    int total_post = 0;
                    int frame_pre[4] = {0};
                    int frame_post[4] = {0};
                    int frame_left[4] = {0};
                    int frame_right[4] = {0};
                    uint8_t candidate_ctrl16[16] = {0};
                    size_t local_frames = 0;

                    for (size_t frame = 0; frame < ram_scan_frames; frame++) {
                        size_t pos = payload_offset + frame * FRAME_BITS;
                        if (pos + FRAME_BITS > bb->len) {
                            break;
                        }
                        if (faw_errors_at(bb->bits, pos) != 0) {
                            break;
                        }
                        const uint8_t *body = bb->bits + pos + 8;
                        uint8_t body_descr[720];
                        uint8_t words_raw[64][11];
                        uint8_t words_post[64][11];
                        int left = 0;
                        int right = 0;

                        if (descramble_body_phase(bb->bits + pos, body_descr, 0) != 0) {
                            break;
                        }
                        read_words_ram_affine(body, words_raw, start, stride);
                        read_words_ram_affine(body_descr, words_post, start, stride);

                        frame_pre[local_frames] = decode_ranges_and_errors_profile(words_raw, &left, &right, NULL);
                        frame_post[local_frames] = decode_ranges_and_errors_profile(words_post, &left, &right, NULL);
                        total_pre += frame_pre[local_frames];
                        total_post += frame_post[local_frames];
                        frame_left[local_frames] = left;
                        frame_right[local_frames] = right;
                        if (local_frames == 0) {
                            memcpy(candidate_ctrl16, body_descr, 16);
                        }
                        local_frames++;
                    }

                    if (local_frames == 0) {
                        continue;
                    }
                    frames_checked += local_frames;
                    int delta = total_post - total_pre;
                    if (total_post < best_post || (total_post == best_post && delta < best_delta)) {
                        best_pre = total_pre;
                        best_post = total_post;
                        best_delta = delta;
                        best_start = start;
                        best_stride = stride;
                        memcpy(best_ctrl16, candidate_ctrl16, sizeof(best_ctrl16));
                        memcpy(best_frame_pre, frame_pre, sizeof(best_frame_pre));
                        memcpy(best_frame_post, frame_post, sizeof(best_frame_post));
                        memcpy(best_frame_left, frame_left, sizeof(best_frame_left));
                        memcpy(best_frame_right, frame_right, sizeof(best_frame_right));
                    }
                }
            }

            fprintf(stderr,
                    "quality: ram_scan_affine strides=%zu frames=%zu best_start=%d best_stride=%d pre=%d post=%d delta=%d\n",
                    strides_checked,
                    frames_checked,
                    best_start,
                    best_stride,
                    best_pre,
                    best_post,
                    best_delta);
            {
                char ctrl16_buf[32];
                char cbits[8];
                char abits[16];
                describe_bits(best_ctrl16, 16, ctrl16_buf, sizeof(ctrl16_buf));
                describe_bits(best_ctrl16, 5, cbits, sizeof(cbits));
                describe_bits(best_ctrl16 + 5, 11, abits, sizeof(abits));
                fprintf(stderr, "quality: ram_scan ctrl16=%s C=%s AD=%s\n", ctrl16_buf, cbits, abits);
            }
            for (size_t i = 0; i < ram_scan_frames && i < 4; i++) {
                if (best_frame_pre[i] == 0 && best_frame_post[i] == 0) {
                    continue;
                }
                fprintf(stderr,
                        "quality: ram_scan frame=%zu pre=%d post=%d left=%d right=%d\n",
                        i,
                        best_frame_pre[i],
                        best_frame_post[i],
                        best_frame_left[i],
                        best_frame_right[i]);
            }
        }

        return;

        {
            const size_t max_scan_frames = 8;
            int best_scan_total = INT_MAX;
            int best_scan_offset = -1;
            int best_scan_phase = -1;
            int best_scan_frame_pre[8] = {0};
            int best_scan_frame_post[8] = {0};
            int best_scan_left[8] = {0};
            int best_scan_right[8] = {0};
            int best_scan_profile[64] = {0};
            for (int offset = 0; offset < 16; offset++) {
                for (int phase = 0; phase < 728; phase++) {
                    int total_post = 0;
                    int total_pre = 0;
                    int frame_pre[8] = {0};
                    int frame_post[8] = {0};
                    int frame_left[8] = {0};
                    int frame_right[8] = {0};
                    int profile[64] = {0};
                    size_t frames_checked = 0;
                    for (size_t pos = payload_offset, frame = 0; pos + FRAME_BITS <= bb->len && frame < max_scan_frames; pos += FRAME_BITS, frame++) {
                        if (faw_errors_at(bb->bits, pos) != 0) {
                            break;
                        }
                        const uint8_t *body = bb->bits + pos + 8;
                        uint8_t norm720[720];
                        uint8_t payload_raw[PAYLOAD_BITS];
                        uint8_t payload_descr[PAYLOAD_BITS];
                        uint8_t words[64][11];
                        int per_word[64];
                        int left = 0;
                        int right = 0;
                        normalize_body_720(body, norm720, 1, 0, 0, 1, 0);
                        memcpy(payload_raw, norm720 + offset, PAYLOAD_BITS);
                        deinterleave_words(payload_raw, words);
                        frame_pre[frame] = decode_ranges_and_errors(words, &left, &right);
                        total_pre += frame_pre[frame];

                        descramble_linear_phase(payload_raw, PAYLOAD_BITS, phase, payload_descr);
                        deinterleave_words(payload_descr, words);
                        frame_post[frame] = decode_ranges_and_errors(words, &left, &right);
                        total_post += frame_post[frame];
                        frame_left[frame] = left;
                        frame_right[frame] = right;
                        word_parity_error_profile(words, per_word, &frame_post[frame]);
                        for (int w = 0; w < 64; w++) {
                            profile[w] += per_word[w];
                        }
                        frames_checked++;
                    }
                    if (frames_checked > 0 && total_post < best_scan_total) {
                        best_scan_total = total_post;
                        best_scan_offset = offset;
                        best_scan_phase = phase;
                        memcpy(best_scan_frame_pre, frame_pre, sizeof(best_scan_frame_pre));
                        memcpy(best_scan_frame_post, frame_post, sizeof(best_scan_frame_post));
                        memcpy(best_scan_left, frame_left, sizeof(best_scan_left));
                        memcpy(best_scan_right, frame_right, sizeof(best_scan_right));
                        memcpy(best_scan_profile, profile, sizeof(best_scan_profile));
                    }
                    (void)total_pre;
                }
            }
            fprintf(stderr,
                    "quality: scan45x16 offset=%d phase=%d parity_errors=%d\n",
                    best_scan_offset, best_scan_phase, best_scan_total);
            for (size_t frame = 0; frame < max_scan_frames; frame++) {
                if (best_scan_frame_pre[frame] == 0 && best_scan_frame_post[frame] == 0) {
                    continue;
                }
                fprintf(stderr,
                        "quality: scan45x16 frame=%zu pre=%d post=%d left=%d right=%d\n",
                        frame, best_scan_frame_pre[frame], best_scan_frame_post[frame], best_scan_left[frame], best_scan_right[frame]);
            }
            for (int w = 0; w < 64; w++) {
                if (best_scan_profile[w] != 0) {
                    fprintf(stderr, "quality: scan45x16 word_err[%d]=%d\n", w, best_scan_profile[w]);
                }
            }
        }
    }
}

static int decode_ranges_and_errors(const uint8_t words[64][11], int *left, int *right) {
    static const int masks[6] = {4, 4, 2, 2, 1, 1};
    int ranges[2] = {0, 0};
    int errors = 0;
    for (int start = 0; start < 6; start++) {
        int syndromes[9];
        int count = 0;
        for (int idx = 0, word = start; word < 54; word += 6, idx++) {
            syndromes[idx] = parity_syndrome(words[word]);
            count += syndromes[idx];
        }
        int bit = count >= 5;
        ranges[start & 1] |= bit * masks[start];
        for (int idx = 0; idx < 9; idx++) {
            errors += syndromes[idx] != bit;
        }
    }
    for (int start = 54; start <= 59; start += 5) {
        int syndromes[5];
        int count = 0;
        for (int idx = 0; idx < 5; idx++) {
            syndromes[idx] = parity_syndrome(words[start + idx]);
            count += syndromes[idx];
        }
        int bit = count >= 3;
        for (int idx = 0; idx < 5; idx++) {
            errors += syndromes[idx] != bit;
        }
    }
    *left = ranges[0];
    *right = ranges[1];
    return errors;
}

static int decode_signed10(const uint8_t word[11]) {
    int value = 0;
    for (int i = 0; i < 10; i++) {
        value |= (word[i] & 1) << i;
    }
    if (value & 0x200) {
        value -= 0x400;
    }
    return value;
}

static int16_t decode_sample(const uint8_t word[11], int range_word) {
    int sample14 = decode_signed10(word) << range_to_shift(range_word);
    sample14 = clamp_int(sample14, -8192, 8191);
    return (int16_t)clamp_int(sample14 << 2, -32768, 32767);
}

static int decode_payload_pcm(
    const uint8_t body[BODY_BITS],
    int16_t pcm[PCM_SAMPLES],
    int ram_read_start,
    int ram_read_stride
) {
    uint8_t words[64][11];
    int left = 0;
    int right = 0;
    if (ram_read_start >= 0) {
        read_words_ram_affine(body, words, ram_read_start, ram_read_stride);
    } else {
        deinterleave_sound_words(body, words);
    }
    int errors = decode_ranges_and_errors(words, &left, &right);
    for (int sample = 0; sample < 32; sample++) {
        pcm[2 * sample] = decode_sample(words[2 * sample], left);
        pcm[2 * sample + 1] = decode_sample(words[2 * sample + 1], right);
    }
    return errors;
}

static int decode_payload_variant(
    const uint8_t payload[PAYLOAD_BITS],
    int transpose,
    int reverse_16,
    int reverse_payload,
    int reverse_words,
    int reverse_bits,
    int invert,
    int *left,
    int *right
) {
    uint8_t payload_tmp[PAYLOAD_BITS];
    const uint8_t *source = payload;
    if (reverse_16) {
        reverse_bits_per_16_words(payload, payload_tmp);
        source = payload_tmp;
    }
    uint8_t words[64][11];
    deinterleave_words_variant(source, words, transpose, reverse_payload, reverse_words, reverse_bits, invert);
    return decode_ranges_and_errors(words, left, right);
}

#if NICAM_ENABLE_LEGACY_DEMOD
static int frame_quality_score(
    const uint8_t *frame,
    const Config *cfg,
    int *pre_errors_out,
    int *post_errors_out,
    uint8_t ctrl16_out[16],
    int *left_out,
    int *right_out
) {
    uint8_t body[BODY_BITS];
    if (descramble_body_phase(frame, body, cfg->scramble_phase) != 0) {
        return -1;
    }
    uint8_t words_raw[64][11];
    uint8_t words[64][11];
    int left = 0;
    int right = 0;
    if (cfg->ram_read_start >= 0) {
        read_words_ram_affine(frame + 8, words_raw, cfg->ram_read_start, cfg->ram_read_stride);
        read_words_ram_affine(body, words, cfg->ram_read_start, cfg->ram_read_stride);
    } else {
        deinterleave_sound_words(frame + 8, words_raw);
        deinterleave_sound_words(body, words);
    }
    int pre_errors = decode_ranges_and_errors(words_raw, &left, &right);
    int post_errors = decode_ranges_and_errors(words, &left, &right);
    if (pre_errors_out != NULL) {
        *pre_errors_out = pre_errors;
    }
    if (post_errors_out != NULL) {
        *post_errors_out = post_errors;
    }
    if (ctrl16_out != NULL) {
        memcpy(ctrl16_out, body, 16);
    }
    if (left_out != NULL) {
        *left_out = left;
    }
    if (right_out != NULL) {
        *right_out = right;
    }

    int score = 1000;
    score -= post_errors * 24;
    score -= pre_errors * 4;
    if (post_errors < pre_errors) {
        score += 128;
    } else {
        score -= 128;
    }
    if (post_errors <= cfg->lock_max_parity_errors) {
        score += 64;
    }
    if (post_errors == 0) {
        score += 64;
    }
    if (ctrl_mode_supported_for_pcm(body)) {
        score += 96;
    } else if (!cfg->allow_unsupported_modes) {
        score -= 384;
    }
    return score;
}
#endif
