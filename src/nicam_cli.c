#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BIT_RATE 728000
#define SYMBOL_RATE 364000
#define FRAME_BITS 728
#define BODY_BITS 720
#define PAYLOAD_BITS 704
#define PCM_SAMPLES 64
#define BITBUF_CAP (FRAME_BITS * 96)
#define LEGACY_SCRAMBLE_PHASE 9

static const uint8_t FAW[8] = {0, 1, 0, 0, 1, 1, 1, 0};

typedef struct {
    double re;
    double im;
} complexd;

typedef struct {
    int sample_rate;
    int input_sample_rate;
    int sps;
    int iq_format;
    int verbose;
    int bitstream_quality;
    int stats_every;
    int frontend_lowpass_hz;
    int carrier_search_hz;
    int carrier_search_step_hz;
    int timing_search_steps;
    int chunk_bytes;
    int matched_filter;
    double matched_rolloff;
    int matched_span_symbols;
    int adaptive_demod;
    int adaptive_fixed;
    double adaptive_costas_alpha;
    double adaptive_gain_mu;
    double adaptive_omega_relative_limit;
    int ram_read_start;
    int ram_read_stride;
    int max_parity_errors;
    int max_pcm_step;
    int conceal_mode;
    int lock_max_parity_errors;
    int lock_confirm_frames;
    int lock_drop_frames;
    int max_bad_frames;
    int conceal_bad_frames;
    int scramble_phase;
    int allow_unsupported_modes;
} Config;

enum {
    IQ_FORMAT_U8 = 0,
    IQ_FORMAT_S16 = 1,
};

typedef struct {
    uint8_t bits[BITBUF_CAP];
    size_t len;
} BitBuffer;

typedef struct {
    int have_prev;
    complexd prev;
} DemodState;

typedef struct {
    int phase;
    int timing_idx;
    int reverse;
    int invert;
    int rotation;
    int swap_bits;
    BitBuffer bits;
    DemodState demod;
} HypothesisState;

typedef struct {
    int conjugate;
    int reverse;
    int invert;
    int rotation;
    int swap_bits;
    int carrier_error_sign;
    int timing_error_sign;
    double initial_mu;
    double initial_freq_hz;
    double mu;
    double omega;
    double omega_mid;
    double omega_min;
    double omega_max;
    double gain_mu;
    double gain_omega;
    double carrier_phase;
    double carrier_freq;
    double carrier_alpha;
    double carrier_beta;
    double agc_power;
    int have_prev_decision;
    int prev_decision;
    int have_mm;
    complexd mm_sym[3];
    complexd mm_dec[3];
    BitBuffer bits;
    size_t q_hist[4];
    size_t symbols;
} AdaptiveHypothesisState;

typedef struct {
    size_t frames_checked;
    size_t hits;
    int best_error_sum;
    ssize_t best_offset;
} PatternScan;

typedef struct {
    size_t samples;
    size_t quarter_counts[4];
    double mean_magnitude;
    double mean_delta;
} SymbolStats;

typedef struct {
    double *taps;
    complexd *tail;
    size_t taps_len;
} MatchedFilter;

typedef struct {
    double *taps;
    complexd *tail;
    size_t taps_len;
} FrontendFilter;

typedef struct {
    int input_rate;
    int output_rate;
    double next_pos;
    size_t input_consumed;
    int have_last;
    complexd last;
} Resampler;

typedef struct {
    double b0;
    double b1;
    double a1;
    double x1[2];
    double y1[2];
} J17Filter;

typedef struct {
    unsigned counts[8][95];
    char displayed[9];
} StationIdState;

static uint8_t scramble[BODY_BITS];

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

static void usage(const char *argv0) {
    fprintf(stderr,
            "Gebruik: %s [--sample-rate 1456000] [--input-sample-rate HZ] [--iq-format u8|s16] [--frontend-lowpass-hz HZ] [--carrier-search-hz HZ] [--carrier-search-step-hz HZ] [--timing-search-steps N] [--matched-filter] [--adaptive-demod|--adaptive-fixed] [--descramble-phase N|--legacy-descramble] [--ram-read-start N] [--ram-read-stride N] [--lock-confirm-frames N] [--lock-drop-frames N] [--bitstream-quality] [--verbose]\n"
            "stdin: interleaved IQ, default rtl_sdr uint8; stdout: stereo s16le 32 kHz\n",
            argv0);
}

static int parse_args(int argc, char **argv, Config *cfg) {
    cfg->sample_rate = SYMBOL_RATE * 4;
    cfg->input_sample_rate = SYMBOL_RATE * 4;
    cfg->sps = 4;
    cfg->iq_format = IQ_FORMAT_U8;
    cfg->verbose = 0;
    cfg->bitstream_quality = 0;
    cfg->stats_every = 0;
    cfg->frontend_lowpass_hz = 0;
    cfg->carrier_search_hz = 0;
    cfg->carrier_search_step_hz = 0;
    cfg->timing_search_steps = 8;
    cfg->chunk_bytes = 32768;
    cfg->matched_filter = 0;
    cfg->matched_rolloff = 0.4;
    cfg->matched_span_symbols = 6;
    cfg->adaptive_demod = 0;
    cfg->adaptive_fixed = 0;
    cfg->adaptive_costas_alpha = 0.15;
    cfg->adaptive_gain_mu = 0.0;
    cfg->adaptive_omega_relative_limit = 0.005;
    cfg->ram_read_start = -1;
    cfg->ram_read_stride = 555;
    cfg->max_parity_errors = 4;
    cfg->max_pcm_step = 0;
    cfg->conceal_mode = 1;
    cfg->lock_max_parity_errors = 8;
    cfg->lock_confirm_frames = 4;
    cfg->lock_drop_frames = 16;
    cfg->max_bad_frames = 8;
    cfg->conceal_bad_frames = 1;
    cfg->scramble_phase = 0;
    cfg->allow_unsupported_modes = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sample-rate") == 0 && i + 1 < argc) {
            cfg->sample_rate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--input-sample-rate") == 0 && i + 1 < argc) {
            cfg->input_sample_rate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--iq-format") == 0 && i + 1 < argc) {
            const char *fmt = argv[++i];
            if (strcmp(fmt, "u8") == 0) {
                cfg->iq_format = IQ_FORMAT_U8;
            } else if (strcmp(fmt, "s16") == 0 || strcmp(fmt, "s16le") == 0 || strcmp(fmt, "cs16") == 0) {
                cfg->iq_format = IQ_FORMAT_S16;
            } else {
                fprintf(stderr, "onbekend IQ-formaat: %s\n", fmt);
                usage(argv[0]);
                return -1;
            }
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg->verbose = 1;
        } else if (strcmp(argv[i], "--bitstream-quality") == 0) {
            cfg->bitstream_quality = 1;
        } else if (strcmp(argv[i], "--stats-every") == 0 && i + 1 < argc) {
            cfg->stats_every = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--frontend-lowpass-hz") == 0 && i + 1 < argc) {
            cfg->frontend_lowpass_hz = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--carrier-search-hz") == 0 && i + 1 < argc) {
            cfg->carrier_search_hz = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--carrier-search-step-hz") == 0 && i + 1 < argc) {
            cfg->carrier_search_step_hz = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timing-search-steps") == 0 && i + 1 < argc) {
            cfg->timing_search_steps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--chunk-bytes") == 0 && i + 1 < argc) {
            cfg->chunk_bytes = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--matched-filter") == 0) {
            cfg->matched_filter = 1;
        } else if (strcmp(argv[i], "--adaptive-demod") == 0) {
            cfg->adaptive_demod = 1;
            cfg->matched_filter = 1;
        } else if (strcmp(argv[i], "--adaptive-fixed") == 0) {
            cfg->adaptive_demod = 1;
            cfg->adaptive_fixed = 1;
            cfg->matched_filter = 1;
        } else if (strcmp(argv[i], "--matched-rolloff") == 0 && i + 1 < argc) {
            cfg->matched_rolloff = atof(argv[++i]);
        } else if (strcmp(argv[i], "--matched-span-symbols") == 0 && i + 1 < argc) {
            cfg->matched_span_symbols = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--adaptive-costas-alpha") == 0 && i + 1 < argc) {
            cfg->adaptive_costas_alpha = atof(argv[++i]);
        } else if (strcmp(argv[i], "--adaptive-gain-mu") == 0 && i + 1 < argc) {
            cfg->adaptive_gain_mu = atof(argv[++i]);
        } else if (strcmp(argv[i], "--adaptive-omega-relative-limit") == 0 && i + 1 < argc) {
            cfg->adaptive_omega_relative_limit = atof(argv[++i]);
        } else if (strcmp(argv[i], "--ram-read-start") == 0 && i + 1 < argc) {
            cfg->ram_read_start = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ram-read-stride") == 0 && i + 1 < argc) {
            cfg->ram_read_stride = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-parity-errors") == 0 && i + 1 < argc) {
            cfg->max_parity_errors = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-pcm-step") == 0 && i + 1 < argc) {
            cfg->max_pcm_step = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--conceal-mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "bridge") == 0) {
                cfg->conceal_mode = 1;
            } else if (strcmp(mode, "mute") == 0) {
                cfg->conceal_mode = 2;
            } else if (strcmp(mode, "drop") == 0) {
                cfg->conceal_mode = 0;
            } else {
                fprintf(stderr, "onbekende conceal-mode: %s\n", mode);
                return -1;
            }
        } else if (strcmp(argv[i], "--j17-deemphasis") == 0) {
            /* J.17 de-emphasis is mandatory for NICAM audio; keep the flag accepted for old scripts. */
        } else if (strcmp(argv[i], "--lock-max-parity-errors") == 0 && i + 1 < argc) {
            cfg->lock_max_parity_errors = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--lock-confirm-frames") == 0 && i + 1 < argc) {
            cfg->lock_confirm_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--lock-drop-frames") == 0 && i + 1 < argc) {
            cfg->lock_drop_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-bad-frames") == 0 && i + 1 < argc) {
            cfg->max_bad_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-conceal") == 0) {
            cfg->conceal_bad_frames = 0;
        } else if (strcmp(argv[i], "--descramble-phase") == 0 && i + 1 < argc) {
            cfg->scramble_phase = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--legacy-descramble") == 0) {
            cfg->scramble_phase = -LEGACY_SCRAMBLE_PHASE;
        } else if (strcmp(argv[i], "--allow-unsupported-modes") == 0) {
            cfg->allow_unsupported_modes = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 1;
        } else {
            usage(argv[0]);
            return -1;
        }
    }

    if (cfg->sample_rate <= 0 || cfg->sample_rate % SYMBOL_RATE != 0) {
        fprintf(stderr, "sample-rate moet een veelvoud van %d zijn\n", SYMBOL_RATE);
        return -1;
    }
    if (cfg->input_sample_rate <= 0) {
        fprintf(stderr, "input-sample-rate moet positief zijn\n");
        return -1;
    }
    cfg->sps = cfg->sample_rate / SYMBOL_RATE;
    if (cfg->matched_rolloff < 0.0 || cfg->matched_rolloff > 1.0) {
        fprintf(stderr, "matched-rolloff moet tussen 0 en 1 liggen\n");
        return -1;
    }
    if (cfg->matched_span_symbols < 2) {
        cfg->matched_span_symbols = 2;
    }
    if (cfg->matched_span_symbols % 2 != 0) {
        cfg->matched_span_symbols++;
    }
    if (cfg->adaptive_costas_alpha <= 0.0) {
        cfg->adaptive_costas_alpha = 0.15;
    }
    if (cfg->adaptive_omega_relative_limit <= 0.0) {
        cfg->adaptive_omega_relative_limit = 0.005;
    }
    if (cfg->timing_search_steps < 1) {
        cfg->timing_search_steps = 1;
    }
    if (cfg->stats_every < 0) {
        cfg->stats_every = 0;
    }
    if (cfg->chunk_bytes < 4096) {
        cfg->chunk_bytes = 4096;
    }
    if (cfg->lock_confirm_frames < 1) {
        cfg->lock_confirm_frames = 1;
    }
    if (cfg->lock_drop_frames < cfg->lock_confirm_frames) {
        cfg->lock_drop_frames = cfg->lock_confirm_frames;
    }
    if (cfg->ram_read_stride == 0) {
        cfg->ram_read_stride = 1;
    }
    if (cfg->carrier_search_hz < 0) {
        cfg->carrier_search_hz = -cfg->carrier_search_hz;
    }
    if (cfg->carrier_search_step_hz < 0) {
        cfg->carrier_search_step_hz = -cfg->carrier_search_step_hz;
    }
    cfg->scramble_phase %= BODY_BITS;
    if (cfg->scramble_phase < 0) {
        cfg->scramble_phase += BODY_BITS;
    }
    return 0;
}

static int read_exactish(uint8_t *buf, size_t bytes) {
    size_t got = 0;
    while (got < bytes) {
        ssize_t n = read(STDIN_FILENO, buf + got, bytes - got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            return -1;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    return (int)got;
}

static double wrap_pi(double x) {
    while (x <= -M_PI) {
        x += 2.0 * M_PI;
    }
    while (x > M_PI) {
        x -= 2.0 * M_PI;
    }
    return x;
}

static void bitbuf_append(BitBuffer *bb, const uint8_t *bits, size_t n);
static void bitbuf_drop(BitBuffer *bb, size_t n);
static int faw_errors_at(const uint8_t *bits, size_t offset);
static int clamp_int(int value, int lo, int hi);
static int descramble_body_phase(const uint8_t *frame, uint8_t body[BODY_BITS], int scramble_phase);
static int ctrl_mode_supported_for_pcm(const uint8_t ctrl16[16]);
static int decode_payload_pcm(
    const uint8_t body[BODY_BITS],
    int16_t pcm[PCM_SAMPLES],
    int ram_read_start,
    int ram_read_stride
);
static void station_id_init(StationIdState *state);
static void station_id_update(StationIdState *state, const uint8_t ctrl16[16]);
static int write_all(const void *data, size_t bytes);
static PatternScan scan_pattern(const uint8_t *bits, size_t len, const uint8_t pattern[8]);
static void bitstream_quality_report(const HypothesisState *hyp, const Config *cfg);

static ssize_t find_next_faw_offset(const BitBuffer *bb, size_t max_search) {
    if (bb->len < FRAME_BITS) {
        return -1;
    }
    size_t limit = bb->len - FRAME_BITS;
    if (limit > max_search) {
        limit = max_search;
    }
    for (size_t off = 1; off <= limit; off++) {
        if (faw_errors_at(bb->bits, off) == 0) {
            return (ssize_t)off;
        }
    }
    return -1;
}

static void init_j17_filter(J17Filter *filter, double sample_rate) {
    memset(filter, 0, sizeof(*filter));
    double k = 2.0 * sample_rate;
    double pole = 3000.0;
    double zero = 3000.0 * sqrt(75.0);
    double gain = pow(75.0, -0.25);
    filter->b0 = gain * (k + zero) / (k + pole);
    filter->b1 = gain * (zero - k) / (k + pole);
    filter->a1 = (pole - k) / (k + pole);
}

static void apply_j17_filter(J17Filter *filter, int16_t pcm[PCM_SAMPLES]) {
    for (int sample = 0; sample < PCM_SAMPLES / 2; sample++) {
        for (int ch = 0; ch < 2; ch++) {
            double x = (double)pcm[2 * sample + ch];
            double y = filter->b0 * x + filter->b1 * filter->x1[ch] - filter->a1 * filter->y1[ch];
            filter->x1[ch] = x;
            filter->y1[ch] = y;
            pcm[2 * sample + ch] = (int16_t)clamp_int((int)lrint(y), -32768, 32767);
        }
    }
}

static void make_interpolated_pcm(
    int16_t pcm[PCM_SAMPLES],
    const int16_t last_output[2],
    const int16_t next_input[2],
    int frame_index,
    int frame_count
) {
    int total_samples = frame_count * (PCM_SAMPLES / 2);
    int base_sample = frame_index * (PCM_SAMPLES / 2);
    for (int sample = 0; sample < PCM_SAMPLES / 2; sample++) {
        int pos = base_sample + sample + 1;
        int keep = total_samples + 1 - pos;
        int take = pos;
        for (int ch = 0; ch < 2; ch++) {
            int mixed = (last_output[ch] * keep + next_input[ch] * take) / (total_samples + 1);
            pcm[2 * sample + ch] = (int16_t)clamp_int(mixed, -32768, 32767);
        }
    }
}

static void remember_pcm_tail(const int16_t pcm[PCM_SAMPLES], int16_t last_output[2]) {
    last_output[0] = pcm[PCM_SAMPLES - 2];
    last_output[1] = pcm[PCM_SAMPLES - 1];
}

static int pcm_max_step(const int16_t pcm[PCM_SAMPLES], const int16_t last_output[2], int have_last_output) {
    int max_step = 0;
    for (int ch = 0; ch < 2; ch++) {
        int prev = have_last_output ? last_output[ch] : pcm[ch];
        for (int sample = 0; sample < PCM_SAMPLES / 2; sample++) {
            int cur = pcm[2 * sample + ch];
            int step = abs(cur - prev);
            if (step > max_step) {
                max_step = step;
            }
            prev = cur;
        }
    }
    return max_step;
}

static void bits_from_quarter(int q, uint8_t *out, size_t *idx) {
    switch (q & 3) {
        case 0:
            out[(*idx)++] = 0;
            out[(*idx)++] = 0;
            break;
        case 3:
            out[(*idx)++] = 0;
            out[(*idx)++] = 1;
            break;
        case 2:
            out[(*idx)++] = 1;
            out[(*idx)++] = 1;
            break;
        default:
            out[(*idx)++] = 1;
            out[(*idx)++] = 0;
            break;
    }
}

static void bits_from_quarter_xor(int q, int invert, int swap_bits, uint8_t *out, size_t *idx) {
    uint8_t tmp[2];
    size_t local = 0;
    bits_from_quarter(q, tmp, &local);
    uint8_t a = (uint8_t)(tmp[0] ^ (invert ? 1 : 0));
    uint8_t b = (uint8_t)(tmp[1] ^ (invert ? 1 : 0));
    if (swap_bits) {
        out[(*idx)++] = b;
        out[(*idx)++] = a;
    } else {
        out[(*idx)++] = a;
        out[(*idx)++] = b;
    }
}

static void bits_from_quarter_rotated(int q, int rotation, int invert, int swap_bits, uint8_t *out, size_t *idx) {
    q = (q + rotation) & 3;
    bits_from_quarter_xor(q, invert, swap_bits, out, idx);
}

static size_t raw_to_samples(const uint8_t *raw, size_t raw_bytes, const Config *cfg, complexd **out) {
    size_t iq_samples = cfg->iq_format == IQ_FORMAT_S16 ? raw_bytes / 4 : raw_bytes / 2;
    complexd *samples = (complexd *)malloc(iq_samples * sizeof(complexd));
    if (samples == NULL) {
        *out = NULL;
        return 0;
    }

    if (cfg->iq_format == IQ_FORMAT_S16) {
        const int16_t *raw_samples = (const int16_t *)raw;
        for (size_t i = 0; i < iq_samples; i++) {
            samples[i].re = (double)raw_samples[2 * i] / 32768.0;
            samples[i].im = (double)raw_samples[2 * i + 1] / 32768.0;
        }
    } else {
        for (size_t i = 0; i < iq_samples; i++) {
            samples[i].re = ((double)raw[2 * i] - 127.5) / 127.5;
            samples[i].im = ((double)raw[2 * i + 1] - 127.5) / 127.5;
        }
    }

    *out = samples;
    return iq_samples;
}

static complexd interpolate_sample(const complexd *samples, size_t sample_count, double pos) {
    complexd out = {0.0, 0.0};
    if (pos < 0.0) {
        return out;
    }
    size_t i0 = (size_t)floor(pos);
    if (i0 >= sample_count) {
        return out;
    }
    size_t i1 = i0 + 1;
    double frac = pos - (double)i0;
    if (i1 >= sample_count) {
        return samples[i0];
    }
    out.re = samples[i0].re * (1.0 - frac) + samples[i1].re * frac;
    out.im = samples[i0].im * (1.0 - frac) + samples[i1].im * frac;
    return out;
}

static int init_resampler(Resampler *rs, int input_rate, int output_rate) {
    memset(rs, 0, sizeof(*rs));
    if (input_rate <= 0 || output_rate <= 0) {
        return -1;
    }
    rs->input_rate = input_rate;
    rs->output_rate = output_rate;
    rs->next_pos = 0.0;
    rs->input_consumed = 0;
    rs->have_last = 0;
    rs->last.re = 0.0;
    rs->last.im = 0.0;
    return 0;
}

static void resample_linear_stream(Resampler *rs, const complexd *in, size_t in_samples, complexd *out, size_t out_cap, size_t *out_samples) {
    *out_samples = 0;
    if (in_samples == 0 || out_cap == 0) {
        rs->input_consumed += in_samples;
        if (in_samples > 0) {
            rs->last = in[in_samples - 1];
            rs->have_last = 1;
        }
        return;
    }

    double step = (double)rs->input_rate / (double)rs->output_rate;
    size_t abs_start = rs->input_consumed;
    double chunk_end = (double)abs_start + (double)in_samples;

    while (rs->next_pos + 1.0 < chunk_end && *out_samples < out_cap) {
        double pos = rs->next_pos;
        size_t i0 = (size_t)floor(pos);
        size_t i1 = i0 + 1;
        complexd s0;
        complexd s1;
        if (i0 < abs_start) {
            s0 = rs->have_last ? rs->last : in[0];
        } else {
            size_t j0 = i0 - abs_start;
            s0 = in[j0];
        }
        if (i1 < abs_start) {
            s1 = rs->have_last ? rs->last : in[0];
        } else if (i1 >= abs_start + in_samples) {
            break;
        } else {
            size_t j1 = i1 - abs_start;
            s1 = in[j1];
        }
        double frac = pos - (double)i0;
        out[*out_samples].re = s0.re * (1.0 - frac) + s1.re * frac;
        out[*out_samples].im = s0.im * (1.0 - frac) + s1.im * frac;
        (*out_samples)++;
        rs->next_pos += step;
    }

    rs->input_consumed += in_samples;
    rs->last = in[in_samples - 1];
    rs->have_last = 1;
}

static size_t timing_symbols_from_samples(
    const complexd *samples,
    size_t sample_count,
    const Config *cfg,
    int phase,
    int timing_idx,
    complexd *sym
) {
    if (cfg->timing_search_steps < 1) {
        return 0;
    }
    double frac = (double)timing_idx / (double)cfg->timing_search_steps;
    double start = (double)phase + frac;
    size_t usable = 0;
    for (double pos = start; pos + (double)(cfg->sps - 1) < (double)sample_count; pos += (double)cfg->sps) {
        sym[usable++] = interpolate_sample(samples, sample_count, pos);
    }
    return usable;
}

static int init_matched_filter(MatchedFilter *mf, const Config *cfg) {
    memset(mf, 0, sizeof(*mf));
    if (!cfg->matched_filter) {
        return 0;
    }

    int span = cfg->matched_span_symbols;
    int half = span * cfg->sps / 2;
    size_t taps_len = (size_t)(2 * half + 1);
    double *taps = (double *)malloc(taps_len * sizeof(double));
    complexd *tail = (complexd *)calloc(taps_len > 0 ? taps_len - 1 : 0, sizeof(complexd));
    if (taps == NULL || (taps_len > 1 && tail == NULL)) {
        free(taps);
        free(tail);
        return -1;
    }

    double beta = cfg->matched_rolloff;
    double sum = 0.0;
    for (int n = -half; n <= half; n++) {
        double t = (double)n / (double)cfg->sps;
        double value;
        if (beta == 0.0) {
            value = fabs(t) < 1.0e-12 ? 1.0 : sin(M_PI * t) / (M_PI * t);
        } else if (fabs(t) < 1.0e-12) {
            value = 1.0 + beta * (4.0 / M_PI - 1.0);
        } else if (fabs(fabs(4.0 * beta * t) - 1.0) < 1.0e-12) {
            double angle = M_PI / (4.0 * beta);
            value = beta / sqrt(2.0) *
                ((1.0 + 2.0 / M_PI) * sin(angle) +
                 (1.0 - 2.0 / M_PI) * cos(angle));
        } else {
            double numerator =
                sin(M_PI * t * (1.0 - beta)) +
                4.0 * beta * t * cos(M_PI * t * (1.0 + beta));
            double denominator = M_PI * t * (1.0 - (4.0 * beta * t) * (4.0 * beta * t));
            value = numerator / denominator;
        }
        taps[(size_t)(n + half)] = value;
        sum += value;
    }
    if (fabs(sum) < 1.0e-12) {
        free(taps);
        free(tail);
        return -1;
    }
    for (size_t i = 0; i < taps_len; i++) {
        taps[i] /= sum;
    }

    mf->taps = taps;
    mf->tail = tail;
    mf->taps_len = taps_len;
    return 0;
}

static void free_matched_filter(MatchedFilter *mf) {
    free(mf->taps);
    free(mf->tail);
    mf->taps = NULL;
    mf->tail = NULL;
    mf->taps_len = 0;
}

static int init_frontend_filter(FrontendFilter *ff, const Config *cfg) {
    memset(ff, 0, sizeof(*ff));
    if (cfg->frontend_lowpass_hz <= 0) {
        return 0;
    }

    int cutoff = cfg->frontend_lowpass_hz;
    if (cutoff >= cfg->sample_rate / 2) {
        cutoff = cfg->sample_rate / 2 - 1;
    }
    int span = 8 * cfg->sps;
    if (span < 16) {
        span = 16;
    }
    if (span % 2 != 0) {
        span++;
    }
    int half = span / 2;
    size_t taps_len = (size_t)(2 * half + 1);
    double *taps = (double *)malloc(taps_len * sizeof(double));
    complexd *tail = (complexd *)calloc(taps_len > 0 ? taps_len - 1 : 0, sizeof(complexd));
    if (taps == NULL || (taps_len > 1 && tail == NULL)) {
        free(taps);
        free(tail);
        return -1;
    }

    double fc = (double)cutoff / (double)cfg->sample_rate;
    double sum = 0.0;
    for (int n = -half; n <= half; n++) {
        double x = (double)n;
        double value;
        if (fabs(x) < 1.0e-12) {
            value = 2.0 * fc;
        } else {
            value = sin(2.0 * M_PI * fc * x) / (M_PI * x);
        }
        double window = 0.54 + 0.46 * cos(M_PI * x / (double)half);
        value *= window;
        taps[(size_t)(n + half)] = value;
        sum += value;
    }
    if (fabs(sum) < 1.0e-12) {
        free(taps);
        free(tail);
        return -1;
    }
    for (size_t i = 0; i < taps_len; i++) {
        taps[i] /= sum;
    }

    ff->taps = taps;
    ff->tail = tail;
    ff->taps_len = taps_len;
    return 0;
}

static void free_frontend_filter(FrontendFilter *ff) {
    free(ff->taps);
    free(ff->tail);
    ff->taps = NULL;
    ff->tail = NULL;
    ff->taps_len = 0;
}

static void filter_frontend_stream(FrontendFilter *ff, const complexd *in, size_t samples, complexd *out) {
    if (ff->taps_len == 0) {
        memcpy(out, in, samples * sizeof(complexd));
        return;
    }
    size_t tail_len = ff->taps_len - 1;
    size_t padded_len = tail_len + samples;
    complexd *padded = (complexd *)malloc(padded_len * sizeof(complexd));
    if (padded == NULL) {
        memset(out, 0, samples * sizeof(complexd));
        return;
    }
    if (tail_len > 0) {
        memcpy(padded, ff->tail, tail_len * sizeof(complexd));
    }
    memcpy(padded + tail_len, in, samples * sizeof(complexd));

    for (size_t n = 0; n < samples; n++) {
        size_t y_index = tail_len + n;
        double re = 0.0;
        double im = 0.0;
        for (size_t k = 0; k < ff->taps_len; k++) {
            if (y_index < k) {
                break;
            }
            size_t x_index = y_index - k;
            if (x_index >= padded_len) {
                continue;
            }
            double tap = ff->taps[k];
            re += padded[x_index].re * tap;
            im += padded[x_index].im * tap;
        }
        out[n].re = re;
        out[n].im = im;
    }
    if (tail_len > 0) {
        memcpy(ff->tail, padded + samples, tail_len * sizeof(complexd));
    }
    free(padded);
}

static void rotate_stream(const complexd *in, size_t samples, double phase_step, complexd *out) {
    double phase = 0.0;
    double c = cos(phase_step);
    double s = sin(phase_step);
    double rot_re = 1.0;
    double rot_im = 0.0;
    for (size_t i = 0; i < samples; i++) {
        out[i].re = in[i].re * rot_re - in[i].im * rot_im;
        out[i].im = in[i].re * rot_im + in[i].im * rot_re;
        double next_re = rot_re * c - rot_im * s;
        double next_im = rot_re * s + rot_im * c;
        rot_re = next_re;
        rot_im = next_im;
        phase += phase_step;
        (void)phase;
    }
}

static void filter_iq_stream(MatchedFilter *mf, const complexd *in, size_t samples, complexd *out) {
    size_t tail_len = mf->taps_len - 1;
    size_t padded_len = tail_len + samples;
    complexd *padded = (complexd *)malloc(padded_len * sizeof(complexd));
    if (padded == NULL) {
        memset(out, 0, samples * sizeof(complexd));
        return;
    }
    if (tail_len > 0) {
        memcpy(padded, mf->tail, tail_len * sizeof(complexd));
    }
    memcpy(padded + tail_len, in, samples * sizeof(complexd));

    for (size_t n = 0; n < samples; n++) {
        size_t y_index = tail_len + n;
        double re = 0.0;
        double im = 0.0;
        for (size_t k = 0; k < mf->taps_len; k++) {
            if (y_index < k) {
                break;
            }
            size_t x_index = y_index - k;
            if (x_index >= padded_len) {
                continue;
            }
            double tap = mf->taps[k];
            re += padded[x_index].re * tap;
            im += padded[x_index].im * tap;
        }
        out[n].re = re;
        out[n].im = im;
    }

    if (tail_len > 0) {
        memcpy(mf->tail, padded + samples, tail_len * sizeof(complexd));
    }
    free(padded);
}

static size_t demod_symbols_variant(
    const complexd *sym,
    size_t symbols,
    DemodState *state,
    int reverse,
    int invert,
    int rotation,
    int swap_bits,
    SymbolStats *stats,
    uint8_t *out_bits
) {
    if (symbols == 0) {
        return 0;
    }

    double *phase_delta = (double *)malloc((symbols + 1) * sizeof(double));
    if (phase_delta == NULL) {
        return 0;
    }

    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
    }

    size_t deltas = 0;
    complexd prev;
    size_t start = 0;
    if (state->have_prev) {
        prev = state->prev;
    } else {
        prev = sym[0];
        start = 1;
    }

    double sum_re = 0.0;
    double sum_im = 0.0;
    double sum_mag = 0.0;
    double sum_delta = 0.0;
    const double quarter = M_PI / 2.0;
    for (size_t i = start; i < symbols; i++) {
        double prod_re = sym[i].re * prev.re + sym[i].im * prev.im;
        double prod_im = sym[i].im * prev.re - sym[i].re * prev.im;
        double delta = atan2(prod_im, prod_re);
        if (reverse) {
            delta = -delta;
        }
        phase_delta[deltas++] = delta;
        if (stats != NULL) {
            double mag = sqrt(sym[i].re * sym[i].re + sym[i].im * sym[i].im);
            sum_mag += mag;
            sum_delta += delta;
            int q = (int)llround(delta / quarter) & 3;
            stats->quarter_counts[q]++;
            stats->samples++;
        }
        double nearest = round(delta / quarter) * quarter;
        double residual = wrap_pi(delta - nearest);
        sum_re += cos(residual);
        sum_im += sin(residual);
        prev = sym[i];
    }
    state->prev = sym[symbols - 1];
    state->have_prev = 1;

    double carrier_step = 0.0;
    if ((sum_re * sum_re + sum_im * sum_im) > 1.0e-12) {
        carrier_step = atan2(sum_im, sum_re);
    }

    size_t out_idx = 0;
    for (size_t i = 0; i < deltas; i++) {
        int q = (int)llround((phase_delta[i] - carrier_step) / quarter);
        bits_from_quarter_rotated(q, rotation, invert, swap_bits, out_bits, &out_idx);
    }

    if (stats != NULL && stats->samples > 0) {
        stats->mean_magnitude = sum_mag / (double)stats->samples;
        stats->mean_delta = sum_delta / (double)stats->samples;
    }

    free(phase_delta);
    return out_idx;
}

static complexd complex_conj_if(complexd x, int conjugate) {
    if (conjugate) {
        x.im = -x.im;
    }
    return x;
}

static complexd complex_rotate(complexd x, double phase) {
    double c = cos(phase);
    double s = sin(phase);
    complexd out;
    out.re = x.re * c - x.im * s;
    out.im = x.re * s + x.im * c;
    return out;
}

static complexd complex_lerp(complexd a, complexd b, double frac) {
    complexd out;
    out.re = a.re * (1.0 - frac) + b.re * frac;
    out.im = a.im * (1.0 - frac) + b.im * frac;
    return out;
}

static int nearest_qpsk(complexd x, complexd *decision) {
    if (fabs(x.re) >= fabs(x.im)) {
        if (x.re >= 0.0) {
            decision->re = 1.0;
            decision->im = 0.0;
            return 0;
        }
        decision->re = -1.0;
        decision->im = 0.0;
        return 2;
    }
    if (x.im >= 0.0) {
        decision->re = 0.0;
        decision->im = 1.0;
        return 1;
    }
    decision->re = 0.0;
    decision->im = -1.0;
    return 3;
}

static void adaptive_init_hypothesis(AdaptiveHypothesisState *st, const Config *cfg) {
    double sps = (double)cfg->sample_rate / (double)SYMBOL_RATE;
    double gain_mu = cfg->adaptive_gain_mu;
    if (gain_mu <= 0.0) {
        if (cfg->sps == 2) {
            gain_mu = 0.050;
        } else if (cfg->sps == 3) {
            gain_mu = 0.075;
        } else if (cfg->sps == 4) {
            gain_mu = 0.11;
        } else if (cfg->sps == 5) {
            gain_mu = 0.125;
        } else {
            gain_mu = 0.15;
        }
    }
    st->mu = st->initial_mu;
    st->omega = sps;
    st->omega_mid = sps;
    st->omega_min = sps * (1.0 - cfg->adaptive_omega_relative_limit);
    st->omega_max = sps * (1.0 + cfg->adaptive_omega_relative_limit);
    st->gain_mu = gain_mu;
    st->gain_omega = 0.25 * gain_mu * gain_mu;
    st->carrier_phase = 0.0;
    st->carrier_freq = -2.0 * M_PI * st->initial_freq_hz / (double)cfg->sample_rate;
    st->carrier_alpha = cfg->adaptive_costas_alpha;
    st->carrier_beta = 0.25 * cfg->adaptive_costas_alpha * cfg->adaptive_costas_alpha;
    st->agc_power = 1.0e-6;
    st->have_prev_decision = 0;
    st->prev_decision = 0;
    st->have_mm = 0;
    memset(st->mm_sym, 0, sizeof(st->mm_sym));
    memset(st->mm_dec, 0, sizeof(st->mm_dec));
    st->bits.len = 0;
    memset(st->q_hist, 0, sizeof(st->q_hist));
    st->symbols = 0;
}

static void adaptive_mm_shift(AdaptiveHypothesisState *st, complexd sym, complexd dec) {
    st->mm_sym[2] = st->mm_sym[1];
    st->mm_sym[1] = st->mm_sym[0];
    st->mm_sym[0] = sym;
    st->mm_dec[2] = st->mm_dec[1];
    st->mm_dec[1] = st->mm_dec[0];
    st->mm_dec[0] = dec;
    if (st->have_mm < 3) {
        st->have_mm++;
    }
}

static size_t adaptive_process_samples(
    AdaptiveHypothesisState *st,
    const Config *cfg,
    const complexd *samples,
    size_t sample_count,
    uint8_t *out_bits
) {
    (void)cfg;
    size_t out_idx = 0;
    size_t pin = 0;
    const double agc_alpha = 0.002;
    while (pin + 1 < sample_count) {
        if (st->mu < 1.0) {
            complexd s0 = complex_conj_if(samples[pin], st->conjugate);
            complexd s1 = complex_conj_if(samples[pin + 1], st->conjugate);
            complexd r0 = complex_rotate(s0, st->carrier_phase);
            complexd r1 = complex_rotate(s1, st->carrier_phase + st->carrier_freq);
            complexd raw_sym = complex_lerp(r0, r1, st->mu);
            double power = raw_sym.re * raw_sym.re + raw_sym.im * raw_sym.im;
            st->agc_power = st->agc_power * (1.0 - agc_alpha) + power * agc_alpha;
            double gain = st->agc_power > 1.0e-12 ? 1.0 / sqrt(st->agc_power) : 1.0;
            complexd sym = {raw_sym.re * gain, raw_sym.im * gain};

            complexd dec;
            int idx = nearest_qpsk(sym, &dec);
            if (st->have_prev_decision) {
                int delta = (idx - st->prev_decision) & 3;
                if (st->reverse) {
                    delta = (-delta) & 3;
                }
                st->q_hist[(size_t)delta]++;
                bits_from_quarter_rotated(delta, st->rotation, st->invert, st->swap_bits, out_bits, &out_idx);
            }
            st->prev_decision = idx;
            st->have_prev_decision = 1;
            st->symbols++;

            double phase_error = (double)st->carrier_error_sign *
                atan2(sym.im * dec.re - sym.re * dec.im, sym.re * dec.re + sym.im * dec.im);
            st->carrier_freq += st->carrier_beta * phase_error;
            st->carrier_phase += st->carrier_alpha * phase_error;

            adaptive_mm_shift(st, sym, dec);
            if (st->have_mm >= 3) {
                complexd p0 = st->mm_sym[0];
                complexd p1 = st->mm_sym[1];
                complexd p2 = st->mm_sym[2];
                complexd c0 = st->mm_dec[0];
                complexd c1 = st->mm_dec[1];
                complexd c2 = st->mm_dec[2];
                double e_re = ((p0.re - p2.re) * c1.re + (p0.im - p2.im) * c1.im) -
                              ((c0.re - c2.re) * p1.re + (c0.im - c2.im) * p1.im);
                double err = (double)st->timing_error_sign * e_re;
                if (err > 1.0) {
                    err = 1.0;
                } else if (err < -1.0) {
                    err = -1.0;
                }
                st->omega += st->gain_omega * err;
                if (st->omega < st->omega_min) {
                    st->omega = st->omega_min;
                } else if (st->omega > st->omega_max) {
                    st->omega = st->omega_max;
                }
                st->mu += st->omega + st->gain_mu * err;
            } else {
                st->mu += st->omega;
            }
        }
        pin++;
        st->mu -= 1.0;
        st->carrier_phase += st->carrier_freq;
        if (st->carrier_phase > 1.0e6 || st->carrier_phase < -1.0e6) {
            st->carrier_phase = wrap_pi(st->carrier_phase);
        }
    }
    return out_idx;
}

static int adaptive_setup_hypotheses(const Config *cfg, AdaptiveHypothesisState **out_hps, size_t *out_count) {
    if (cfg->adaptive_fixed) {
        AdaptiveHypothesisState *hps = (AdaptiveHypothesisState *)calloc(1, sizeof(*hps));
        if (hps == NULL) {
            return -1;
        }
        hps[0].conjugate = 0;
        hps[0].reverse = 0;
        hps[0].invert = 0;
        hps[0].rotation = 0;
        hps[0].swap_bits = 0;
        hps[0].carrier_error_sign = -1;
        hps[0].timing_error_sign = 1;
        hps[0].initial_mu = 0.0;
        hps[0].initial_freq_hz = 0.0;
        adaptive_init_hypothesis(&hps[0], cfg);
        *out_hps = hps;
        *out_count = 1;
        return 0;
    }

    int carrier_steps = 1;
    int carrier_range = cfg->carrier_search_hz;
    int carrier_step = cfg->carrier_search_step_hz;
    if (carrier_range > 0 && carrier_step > 0) {
        carrier_steps = (carrier_range / carrier_step) * 2 + 1;
    }
    int timing_steps = cfg->timing_search_steps > 0 ? cfg->timing_search_steps : cfg->sps;
    if (timing_steps < cfg->sps) {
        timing_steps = cfg->sps;
    }
    size_t count = (size_t)carrier_steps * (size_t)timing_steps * 256U;
    AdaptiveHypothesisState *hps = (AdaptiveHypothesisState *)calloc(count, sizeof(*hps));
    if (hps == NULL) {
        return -1;
    }
    size_t idx = 0;
    for (int carrier_idx = 0; carrier_idx < carrier_steps; carrier_idx++) {
        int carrier_hz = 0;
        if (carrier_steps > 1) {
            int rel = carrier_idx - (carrier_steps / 2);
            carrier_hz = rel * carrier_step;
        }
        for (int timing = 0; timing < timing_steps; timing++) {
            double mu = (double)timing / (double)timing_steps;
            for (int carrier_sign_idx = 0; carrier_sign_idx < 2; carrier_sign_idx++) {
                int carrier_error_sign = carrier_sign_idx == 0 ? 1 : -1;
                for (int timing_sign_idx = 0; timing_sign_idx < 2; timing_sign_idx++) {
                    int timing_error_sign = timing_sign_idx == 0 ? 1 : -1;
                    for (int conjugate = 0; conjugate < 2; conjugate++) {
                        for (int reverse = 0; reverse < 2; reverse++) {
                            for (int invert = 0; invert < 2; invert++) {
                                for (int rotation = 0; rotation < 4; rotation++) {
                                    for (int swap_bits = 0; swap_bits < 2; swap_bits++) {
                                        hps[idx].conjugate = conjugate;
                                        hps[idx].reverse = reverse;
                                        hps[idx].invert = invert;
                                        hps[idx].rotation = rotation;
                                        hps[idx].swap_bits = swap_bits;
                                        hps[idx].carrier_error_sign = carrier_error_sign;
                                        hps[idx].timing_error_sign = timing_error_sign;
                                        hps[idx].initial_mu = mu;
                                        hps[idx].initial_freq_hz = (double)carrier_hz;
                                        adaptive_init_hypothesis(&hps[idx], cfg);
                                        idx++;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    *out_hps = hps;
    *out_count = idx;
    return 0;
}

static void station_id_init(StationIdState *state) {
    memset(state, 0, sizeof(*state));
    memcpy(state->displayed, "        ", 8);
    state->displayed[8] = '\0';
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
    fprintf(stderr, "station_id=%s\n", state->displayed);
}

static int run_adaptive_quality(const Config *cfg, FrontendFilter *frontend, MatchedFilter *matched) {
    AdaptiveHypothesisState *hps = NULL;
    size_t hyp_count = 0;
    if (adaptive_setup_hypotheses(cfg, &hps, &hyp_count) != 0) {
        fprintf(stderr, "adaptive demod hypotheses initialisatie mislukt\n");
        return 1;
    }

    const size_t bytes_per_iq = cfg->iq_format == IQ_FORMAT_S16 ? 4 : 2;
    const size_t raw_bytes = (size_t)cfg->chunk_bytes;
    uint8_t *raw = (uint8_t *)malloc(raw_bytes);
    uint8_t *chunk_bits = (uint8_t *)malloc((raw_bytes / bytes_per_iq + 1) * 2);
    if (raw == NULL || chunk_bits == NULL) {
        free(raw);
        free(chunk_bits);
        free(hps);
        return 1;
    }

    Resampler resampler;
    if (init_resampler(&resampler, cfg->input_sample_rate, cfg->sample_rate) != 0) {
        free(raw);
        free(chunk_bits);
        free(hps);
        return 1;
    }

    int16_t last_pcm[PCM_SAMPLES] = {0};
    int16_t last_output[2] = {0, 0};
    int have_last_pcm = 0;
    int have_last_output = 0;
    int pending_conceal_frames = 0;
    int adaptive_frames = 0;
    int adaptive_bad_frames = 0;
    int adaptive_align_drops = 0;
    int adaptive_sync_bit_drops = 0;
    int adaptive_last_stat_frame = 0;
    StationIdState station_id;
    J17Filter j17;
    station_id_init(&station_id);
    init_j17_filter(&j17, 32000.0);

    while (1) {
        int got = read_exactish(raw, raw_bytes);
        if (got <= 0) {
            break;
        }
        complexd *samples = NULL;
        size_t sample_count = raw_to_samples(raw, (size_t)got, cfg, &samples);
        if (sample_count == 0 || samples == NULL) {
            free(samples);
            continue;
        }
        size_t resampled_cap = (size_t)ceil((double)sample_count * (double)cfg->sample_rate / (double)cfg->input_sample_rate) + 8;
        complexd *resampled = (complexd *)malloc(resampled_cap * sizeof(complexd));
        size_t resampled_count = 0;
        if (resampled == NULL) {
            free(samples);
            continue;
        }
        resample_linear_stream(&resampler, samples, sample_count, resampled, resampled_cap, &resampled_count);
        if (resampled_count == 0) {
            free(samples);
            free(resampled);
            continue;
        }
        complexd *frontend_alloc = NULL;
        complexd *frontend_out = resampled;
        if (frontend->taps_len > 0) {
            frontend_alloc = (complexd *)malloc(resampled_count * sizeof(complexd));
            if (frontend_alloc == NULL) {
                free(samples);
                free(resampled);
                continue;
            }
            filter_frontend_stream(frontend, resampled, resampled_count, frontend_alloc);
            frontend_out = frontend_alloc;
        }
        complexd *filtered = frontend_out;
        complexd *filtered_alloc = NULL;
        if (cfg->matched_filter) {
            filtered_alloc = (complexd *)malloc(resampled_count * sizeof(complexd));
            if (filtered_alloc == NULL) {
                free(samples);
                free(resampled);
                free(frontend_alloc);
                continue;
            }
            filter_iq_stream(matched, frontend_out, resampled_count, filtered_alloc);
            filtered = filtered_alloc;
        }

        for (size_t hyp = 0; hyp < hyp_count; hyp++) {
            size_t nbits = adaptive_process_samples(&hps[hyp], cfg, filtered, resampled_count, chunk_bits);
            bitbuf_append(&hps[hyp].bits, chunk_bits, nbits);
        }

        if (!cfg->bitstream_quality) {
            size_t best_stream_hyp = 0;
            int best_stream_errors = INT_MAX;
            size_t best_stream_hits = 0;
            ssize_t best_stream_offset = -1;
            for (size_t hyp = 0; hyp < hyp_count; hyp++) {
                PatternScan scan = scan_pattern(hps[hyp].bits.bits, hps[hyp].bits.len, FAW);
                if (scan.best_error_sum < best_stream_errors ||
                    (scan.best_error_sum == best_stream_errors && scan.hits > best_stream_hits)) {
                    best_stream_errors = scan.best_error_sum;
                    best_stream_hits = scan.hits;
                    best_stream_offset = scan.best_offset;
                    best_stream_hyp = hyp;
                }
            }
            BitBuffer *bb = &hps[best_stream_hyp].bits;
            if (best_stream_offset > 0) {
                adaptive_align_drops += (int)best_stream_offset;
                bitbuf_drop(bb, (size_t)best_stream_offset);
            }
            while (bb->len >= FRAME_BITS) {
                if (faw_errors_at(bb->bits, 0) != 0) {
                    ssize_t next_faw = find_next_faw_offset(bb, FRAME_BITS * 3);
                    if (next_faw > 0) {
                        adaptive_sync_bit_drops += (int)next_faw;
                        if (cfg->conceal_bad_frames && have_last_pcm && cfg->conceal_mode == 1) {
                            int missing_frames = (int)((next_faw + FRAME_BITS / 2) / FRAME_BITS);
                            if (missing_frames < 1) {
                                missing_frames = 1;
                            }
                            pending_conceal_frames += missing_frames;
                            adaptive_bad_frames += missing_frames;
                        }
                        bitbuf_drop(bb, (size_t)next_faw);
                    } else {
                        adaptive_sync_bit_drops++;
                        bitbuf_drop(bb, 1);
                    }
                    continue;
                }
                uint8_t body[BODY_BITS];
                int16_t pcm[PCM_SAMPLES];
                if (descramble_body_phase(bb->bits, body, cfg->scramble_phase) != 0) {
                    bitbuf_drop(bb, 1);
                    continue;
                }
                int left = 0;
                int right = 0;
                int parity_errors = decode_payload_pcm(body, pcm, cfg->ram_read_start, cfg->ram_read_stride);
                int mode_supported = ctrl_mode_supported_for_pcm(body);
                int pcm_step = pcm_max_step(pcm, last_output, have_last_output);
                int pcm_continuity_ok = cfg->max_pcm_step <= 0 || !have_last_output || pcm_step <= cfg->max_pcm_step;
                if (mode_supported && parity_errors <= cfg->max_parity_errors && pcm_continuity_ok) {
                    station_id_update(&station_id, body);
                    if (pending_conceal_frames > 0 && have_last_output) {
                        int16_t bridge_pcm[PCM_SAMPLES];
                        int16_t bridge_start[2] = {last_output[0], last_output[1]};
                        int16_t next_input[2] = {pcm[0], pcm[1]};
                        for (int frame = 0; frame < pending_conceal_frames; frame++) {
                            make_interpolated_pcm(
                                bridge_pcm,
                                bridge_start,
                                next_input,
                                frame,
                                pending_conceal_frames
                            );
                            apply_j17_filter(&j17, bridge_pcm);
                            if (write_all(bridge_pcm, sizeof(bridge_pcm)) < 0) {
                                free(samples);
                                free(resampled);
                                free(frontend_alloc);
                                free(filtered_alloc);
                                free(raw);
                                free(chunk_bits);
                                free(hps);
                                return 0;
                            }
                            remember_pcm_tail(bridge_pcm, last_output);
                            have_last_output = 1;
                        }
                    }
                    pending_conceal_frames = 0;
                    memcpy(last_pcm, pcm, sizeof(last_pcm));
                    have_last_pcm = 1;
                    apply_j17_filter(&j17, pcm);
                    if (write_all(pcm, sizeof(pcm)) < 0) {
                        free(samples);
                        free(resampled);
                        free(frontend_alloc);
                        free(filtered_alloc);
                        free(raw);
                        free(chunk_bits);
                        free(hps);
                        return 0;
                    }
                    remember_pcm_tail(pcm, last_output);
                    have_last_output = 1;
                } else if (cfg->conceal_bad_frames && have_last_pcm && cfg->conceal_mode == 1) {
                    adaptive_bad_frames++;
                    if (have_last_output) {
                        pending_conceal_frames++;
                    }
                } else if (cfg->conceal_bad_frames && have_last_pcm && cfg->conceal_mode == 2) {
                    adaptive_bad_frames++;
                    int16_t mute_pcm[PCM_SAMPLES] = {0};
                    apply_j17_filter(&j17, mute_pcm);
                    if (write_all(mute_pcm, sizeof(mute_pcm)) < 0) {
                        free(samples);
                        free(resampled);
                        free(frontend_alloc);
                        free(filtered_alloc);
                        free(raw);
                        free(chunk_bits);
                        free(hps);
                        return 0;
                    }
                    remember_pcm_tail(mute_pcm, last_output);
                    have_last_output = 1;
                } else {
                    adaptive_bad_frames++;
                }
                (void)left;
                (void)right;
                adaptive_frames++;
                if (cfg->stats_every > 0 &&
                    adaptive_frames - adaptive_last_stat_frame >= cfg->stats_every) {
                    fprintf(stderr,
                            "nicam_stats frames=%d bad=%d pending=%d align_drop_bits=%d sync_drop_bits=%d bb_bits=%zu hyp=%zu faw_hits=%zu faw_errsum=%d\n",
                            adaptive_frames,
                            adaptive_bad_frames,
                            pending_conceal_frames,
                            adaptive_align_drops,
                            adaptive_sync_bit_drops,
                            bb->len,
                            best_stream_hyp,
                            best_stream_hits,
                            best_stream_errors);
                    adaptive_last_stat_frame = adaptive_frames;
                }
                bitbuf_drop(bb, FRAME_BITS);
            }
            for (size_t hyp = 0; hyp < hyp_count; hyp++) {
                if (hyp == best_stream_hyp) {
                    continue;
                }
                if (hps[hyp].bits.len > FRAME_BITS * 24) {
                    bitbuf_drop(&hps[hyp].bits, hps[hyp].bits.len - FRAME_BITS * 24);
                }
            }
        }

        free(samples);
        free(resampled);
        free(frontend_alloc);
        free(filtered_alloc);
    }

    if (!cfg->bitstream_quality) {
        if (pending_conceal_frames > 0 && have_last_output) {
            int16_t bridge_pcm[PCM_SAMPLES];
            for (int frame = 0; frame < pending_conceal_frames; frame++) {
                make_interpolated_pcm(
                    bridge_pcm,
                    last_output,
                    last_output,
                    frame,
                    pending_conceal_frames
                );
                apply_j17_filter(&j17, bridge_pcm);
                if (write_all(bridge_pcm, sizeof(bridge_pcm)) < 0) {
                    free(raw);
                    free(chunk_bits);
                    free(hps);
                    return 0;
                }
            }
        }
        if (cfg->verbose) {
            fprintf(stderr, "adaptive_decoded_frames=%d adaptive_bad_frames=%d\n", adaptive_frames, adaptive_bad_frames);
        }
        free(raw);
        free(chunk_bits);
        free(hps);
        return 0;
    }

    int best_error_sum = INT_MAX;
    ssize_t best_offset = -1;
    size_t best_hits = 0;
    size_t best_frames = 0;
    size_t best_hyp = 0;
    for (size_t hyp = 0; hyp < hyp_count; hyp++) {
        PatternScan scan = scan_pattern(hps[hyp].bits.bits, hps[hyp].bits.len, FAW);
        if (scan.best_error_sum < best_error_sum ||
            (scan.best_error_sum == best_error_sum && scan.hits > best_hits)) {
            best_error_sum = scan.best_error_sum;
            best_offset = scan.best_offset;
            best_hits = scan.hits;
            best_frames = scan.frames_checked;
            best_hyp = hyp;
        }
    }
    AdaptiveHypothesisState *best = &hps[best_hyp];
    fprintf(stderr,
            "adaptive_quality: selected_hyp=%zu raw_faw_best_offset=%zd raw_faw_best_frames=%zu raw_faw_hits=%zu raw_faw_error_sum=%d\n",
            best_hyp, best_offset, best_frames, best_hits, best_error_sum);
    fprintf(stderr,
            "adaptive_quality: conj=%d reverse=%d invert=%d rot=%d swap=%d carrier_sign=%d timing_sign=%d initial_mu=%.4f initial_freq_hz=%.1f symbols=%zu bits=%zu q_hist=%zu/%zu/%zu/%zu omega=%.6f carrier_hz=%.1f\n",
            best->conjugate, best->reverse, best->invert, best->rotation, best->swap_bits,
            best->carrier_error_sign, best->timing_error_sign,
            best->initial_mu, best->initial_freq_hz, best->symbols, best->bits.len,
            best->q_hist[0], best->q_hist[1], best->q_hist[2], best->q_hist[3],
            best->omega, -best->carrier_freq * (double)cfg->sample_rate / (2.0 * M_PI));
    HypothesisState report_hyp;
    memset(&report_hyp, 0, sizeof(report_hyp));
    report_hyp.invert = best->invert;
    report_hyp.rotation = best->rotation;
    report_hyp.swap_bits = best->swap_bits;
    report_hyp.bits = best->bits;
    bitstream_quality_report(&report_hyp, cfg);

    free(raw);
    free(chunk_bits);
    free(hps);
    return 0;
}

static void bitbuf_append(BitBuffer *bb, const uint8_t *bits, size_t n) {
    if (n >= BITBUF_CAP) {
        bits += n - BITBUF_CAP;
        n = BITBUF_CAP;
        bb->len = 0;
    }
    if (bb->len + n > BITBUF_CAP) {
        size_t drop = bb->len + n - BITBUF_CAP;
        memmove(bb->bits, bb->bits + drop, bb->len - drop);
        bb->len -= drop;
    }
    memcpy(bb->bits + bb->len, bits, n);
    bb->len += n;
}

static void bitbuf_drop(BitBuffer *bb, size_t n) {
    if (n >= bb->len) {
        bb->len = 0;
        return;
    }
    memmove(bb->bits, bb->bits + n, bb->len - n);
    bb->len -= n;
}

static int faw_errors_at(const uint8_t *bits, size_t offset) {
    int errors = 0;
    for (int i = 0; i < 8; i++) {
        errors += (bits[offset + (size_t)i] ^ FAW[i]) != 0;
    }
    return errors;
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

static int hamming_distance_bits(const uint8_t *a, const uint8_t *b, size_t count) {
    int dist = 0;
    for (size_t i = 0; i < count; i++) {
        dist += ((a[i] ^ b[i]) & 1U) != 0;
    }
    return dist;
}

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

static PatternScan scan_pattern(const uint8_t *bits, size_t len, const uint8_t pattern[8]) {
    PatternScan scan;
    scan.frames_checked = 0;
    scan.hits = 0;
    scan.best_error_sum = INT_MAX;
    scan.best_offset = -1;
    if (len < FRAME_BITS) {
        return scan;
    }
    size_t max_offset = FRAME_BITS;
    if (max_offset > len - FRAME_BITS) {
        max_offset = len - FRAME_BITS;
    }
    const size_t max_frames_eval = 32;
    for (size_t off = 0; off <= max_offset; off++) {
        size_t frames = (len - off) / FRAME_BITS;
        if (frames > max_frames_eval) {
            frames = max_frames_eval;
        }
        if (frames == 0) {
            continue;
        }
        int error_sum = 0;
        size_t hits = 0;
        for (size_t frame = 0; frame < frames; frame++) {
            size_t pos = off + frame * FRAME_BITS;
            int errors = 0;
            for (int i = 0; i < 8; i++) {
                errors += (bits[pos + (size_t)i] ^ pattern[i]) != 0;
            }
            if (errors == 0) {
                hits++;
            }
            error_sum += errors;
        }
        if (error_sum < scan.best_error_sum || (error_sum == scan.best_error_sum && hits > scan.hits)) {
            scan.best_error_sum = error_sum;
            scan.best_offset = (ssize_t)off;
            scan.hits = hits;
            scan.frames_checked = frames;
        }
    }
    return scan;
}

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

static int frame_score(const uint8_t *frame, const Config *cfg) {
    return frame_quality_score(frame, cfg, NULL, NULL, NULL, NULL, NULL);
}

static ssize_t find_lock_with_score(const BitBuffer *bb, const Config *cfg, int *score_out) {
    if (bb->len < FRAME_BITS * 3) {
        if (score_out != NULL) {
            *score_out = -1000000;
        }
        return -1;
    }
    size_t max_offset = bb->len - FRAME_BITS * 3;
    int best_score = -1000000;
    ssize_t best_offset = -1;
    for (size_t off = 0; off <= max_offset; off++) {
        if (faw_errors_at(bb->bits, off) != 0 ||
            faw_errors_at(bb->bits, off + FRAME_BITS) != 0 ||
            faw_errors_at(bb->bits, off + FRAME_BITS * 2) != 0) {
            continue;
        }
        int score = 0;
        int good = 0;
        for (int i = 0; i < 8; i++) {
            size_t pos = off + (size_t)i * FRAME_BITS;
            if (pos + FRAME_BITS > bb->len) {
                break;
            }
            int s = frame_score(bb->bits + pos, cfg);
            if (s < -100000) {
                break;
            }
            score += s;
            good++;
        }
        if (good >= 4) {
            int c0_frames = 0;
            int c0_errors = c0_pattern_errors(bb->bits, bb->len, off, cfg, &c0_frames);
            score += c0_frames * 40;
            score -= c0_errors * 120;
        }
        if (good >= 3 && score > best_score) {
            best_score = score;
            best_offset = (ssize_t)off;
        }
    }
    if (score_out != NULL) {
        *score_out = best_score;
    }
    return best_offset;
}

static int write_all(const void *data, size_t bytes) {
    const uint8_t *p = (const uint8_t *)data;
    while (bytes > 0) {
        ssize_t n = write(STDOUT_FILENO, p, bytes);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        bytes -= (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv) {
    Config cfg;
    int parsed = parse_args(argc, argv, &cfg);
    if (parsed > 0) {
        return 0;
    }
    if (parsed < 0) {
        return 2;
    }
    init_scramble();
    FrontendFilter frontend;
    if (init_frontend_filter(&frontend, &cfg) != 0) {
        fprintf(stderr, "frontend filter initialisatie mislukt\n");
        return 1;
    }
    Resampler resampler;
    if (init_resampler(&resampler, cfg.input_sample_rate, cfg.sample_rate) != 0) {
        fprintf(stderr, "resampler initialisatie mislukt\n");
        free_frontend_filter(&frontend);
        return 1;
    }
    MatchedFilter matched;
    if (init_matched_filter(&matched, &cfg) != 0) {
        fprintf(stderr, "matched filter initialisatie mislukt\n");
        free_frontend_filter(&frontend);
        return 1;
    }
    if (cfg.adaptive_demod) {
        int rc = run_adaptive_quality(&cfg, &frontend, &matched);
        free_frontend_filter(&frontend);
        free_matched_filter(&matched);
        return rc;
    }

    const size_t bytes_per_iq = cfg.iq_format == IQ_FORMAT_S16 ? 4 : 2;
    const size_t raw_bytes = 1 << 20;
    uint8_t *raw = (uint8_t *)malloc(raw_bytes);
    uint8_t *chunk_bits = (uint8_t *)malloc((raw_bytes / bytes_per_iq + 1) * 2);
    if (raw == NULL || chunk_bits == NULL) {
        fprintf(stderr, "out of memory\n");
        free(raw);
        free(chunk_bits);
        free_frontend_filter(&frontend);
        free_matched_filter(&matched);
        return 1;
    }

    size_t hyp_count = (size_t)cfg.sps * (size_t)cfg.timing_search_steps * 32;
    HypothesisState *hps = (HypothesisState *)calloc(hyp_count, sizeof(HypothesisState));
    if (hps == NULL) {
        fprintf(stderr, "out of memory\n");
        free(raw);
        free(chunk_bits);
        free_frontend_filter(&frontend);
        free_matched_filter(&matched);
        return 1;
    }
    for (int phase = 0; phase < cfg.sps; phase++) {
        for (int timing_idx = 0; timing_idx < cfg.timing_search_steps; timing_idx++) {
            for (int reverse = 0; reverse < 2; reverse++) {
                for (int invert = 0; invert < 2; invert++) {
                    for (int rotation = 0; rotation < 4; rotation++) {
                        for (int swap_bits = 0; swap_bits < 2; swap_bits++) {
                            size_t idx = ((((size_t)phase * (size_t)cfg.timing_search_steps + (size_t)timing_idx) * 32) +
                                          (size_t)reverse * 16 + (size_t)invert * 8 + (size_t)rotation * 2) +
                                         (size_t)swap_bits;
                            hps[idx].phase = phase;
                            hps[idx].timing_idx = timing_idx;
                            hps[idx].reverse = reverse;
                            hps[idx].invert = invert;
                            hps[idx].rotation = rotation;
                            hps[idx].swap_bits = swap_bits;
                        }
                    }
                }
            }
        }
    }
    int locked = 0;
    int lock_confirmed = 0;
    int locked_hyp = -1;
    int best_quality_hyp = -1;
    int lock_good_streak = 0;
    int lock_bad_streak = 0;
    int lock_sync_miss = 0;
    uint8_t last_ctrl16[16] = {0};
    int have_last_ctrl16 = 0;
    const int sync_resync_window = 4;
    int total_frames = 0;
    int last_good_frame = -1;
    int last_good_pre_errors = -1;
    int last_good_post_errors = -1;
    int last_good_lock_quality = -1;
    uint8_t last_good_ctrl16[16] = {0};
    StationIdState station_id;
    int16_t last_pcm[PCM_SAMPLES] = {0};
    station_id_init(&station_id);

    while (1) {
        int got = read_exactish(raw, raw_bytes);
        if (got < 0) {
            break;
        }
        if (got == 0) {
            break;
        }

        complexd *samples = NULL;
        size_t sample_count = raw_to_samples(raw, (size_t)got, &cfg, &samples);
        if (sample_count == 0 || samples == NULL) {
            free(samples);
            continue;
        }
        size_t resampled_cap = (size_t)ceil((double)sample_count * (double)cfg.sample_rate / (double)cfg.input_sample_rate) + 8;
        if (resampled_cap < 8) {
            resampled_cap = 8;
        }
        complexd *resampled = (complexd *)malloc(resampled_cap * sizeof(complexd));
        size_t resampled_count = 0;
        if (resampled == NULL) {
            free(samples);
            continue;
        }
        resample_linear_stream(&resampler, samples, sample_count, resampled, resampled_cap, &resampled_count);
        if (resampled_count == 0) {
            free(samples);
            free(resampled);
            continue;
        }
        complexd *frontend_alloc = NULL;
        complexd *frontend_out = resampled;
        if (frontend.taps_len > 0) {
            frontend_alloc = (complexd *)malloc(resampled_count * sizeof(complexd));
            if (frontend_alloc == NULL) {
                free(samples);
                free(resampled);
                continue;
            }
            filter_frontend_stream(&frontend, resampled, resampled_count, frontend_alloc);
            frontend_out = frontend_alloc;
        }
        complexd *filtered = frontend_out;
        complexd *filtered_alloc = NULL;
        if (cfg.matched_filter) {
            filtered_alloc = (complexd *)malloc(resampled_count * sizeof(complexd));
            if (filtered_alloc == NULL) {
                free(samples);
                free(frontend_alloc);
                free(resampled);
                continue;
            }
            filter_iq_stream(&matched, frontend_out, resampled_count, filtered_alloc);
            filtered = filtered_alloc;
        }

        int best_hyp = -1;
        int best_score = -1000000;
        ssize_t best_offset = -1;
        int carrier_steps = 0;
        int carrier_range = cfg.carrier_search_hz;
        int carrier_step = cfg.carrier_search_step_hz;
        if (carrier_range > 0 && carrier_step > 0) {
            carrier_steps = (carrier_range / carrier_step) * 2 + 1;
        }
        if (carrier_steps <= 0) {
            carrier_steps = 1;
            carrier_step = 0;
            carrier_range = 0;
        }

        for (int carrier_idx = 0; carrier_idx < carrier_steps; carrier_idx++) {
            int carrier_hz = 0;
            complexd *carrier_in = filtered;
            complexd *carrier_alloc = NULL;
            if (carrier_steps > 1) {
                int rel = carrier_idx - (carrier_steps / 2);
                carrier_hz = rel * carrier_step;
                if (carrier_hz == 0) {
                    carrier_in = filtered;
                } else {
                    double phase_step = -2.0 * M_PI * (double)carrier_hz / (double)cfg.sample_rate;
                    carrier_alloc = (complexd *)malloc(resampled_count * sizeof(complexd));
                    if (carrier_alloc == NULL) {
                        continue;
                    }
                    rotate_stream(filtered, resampled_count, phase_step, carrier_alloc);
                    carrier_in = carrier_alloc;
                }
            }

            for (int phase = 0; phase < cfg.sps; phase++) {
                for (int timing_idx = 0; timing_idx < cfg.timing_search_steps; timing_idx++) {
                    complexd *phase_sym = (complexd *)malloc(resampled_count * sizeof(complexd));
                    if (phase_sym == NULL) {
                        continue;
                    }
                    size_t symbols = timing_symbols_from_samples(carrier_in, resampled_count, &cfg, phase, timing_idx, phase_sym);
                    if (symbols > 0) {
                        for (int reverse = 0; reverse < 2; reverse++) {
                            for (int invert = 0; invert < 2; invert++) {
                                for (int rotation = 0; rotation < 4; rotation++) {
                                    for (int swap_bits = 0; swap_bits < 2; swap_bits++) {
                                        size_t hyp = ((((size_t)phase * (size_t)cfg.timing_search_steps + (size_t)timing_idx) * 32) +
                                                      (size_t)reverse * 16 + (size_t)invert * 8 + (size_t)rotation * 2) +
                                                     (size_t)swap_bits;
                                        SymbolStats stats;
                                        size_t hyp_bits_len = demod_symbols_variant(
                                            phase_sym, symbols, &hps[hyp].demod, reverse, invert, rotation, swap_bits, &stats, chunk_bits);
                                        bitbuf_append(&hps[hyp].bits, chunk_bits, hyp_bits_len);
                                        int hyp_score = -1000000;
                                        ssize_t hyp_offset = find_lock_with_score(&hps[hyp].bits, &cfg, &hyp_score);
                                        if (cfg.verbose) {
                                            double timing = (double)timing_idx / (double)cfg.timing_search_steps;
                                            fprintf(stderr,
                                                    "carrier=%d phase=%d timing=%.3f reverse=%d invert=%d rot=%d swap=%d symbols=%zu bits=%zu score=%d offset=%zd len=%zu q=%zu/%zu/%zu/%zu mag=%.4f d=%.4f\n",
                                                    carrier_hz, phase, timing, reverse, invert, rotation, swap_bits, symbols, hyp_bits_len, hyp_score, hyp_offset, hps[hyp].bits.len,
                                                    stats.quarter_counts[0], stats.quarter_counts[1], stats.quarter_counts[2], stats.quarter_counts[3],
                                                    stats.mean_magnitude, stats.mean_delta);
                                        }
                                        if (hyp_offset >= 0 && hyp_score > best_score) {
                                            best_score = hyp_score;
                                            best_offset = hyp_offset;
                                            best_hyp = (int)hyp;
                                        }
                                    }
                                }
                            }
                        }
                    } else if (cfg.verbose) {
                        double timing = (double)timing_idx / (double)cfg.timing_search_steps;
                        fprintf(stderr, "carrier=%d phase=%d timing=%.3f symbols=0\n", carrier_hz, phase, timing);
                    }
                    free(phase_sym);
                }
            }
            free(carrier_alloc);
        }
        free(samples);
        free(resampled);
        free(frontend_alloc);
        free(filtered_alloc);

        if (!locked) {
            if (best_hyp >= 0) {
                locked = 1;
                lock_confirmed = 0;
                locked_hyp = best_hyp;
                lock_good_streak = 0;
                lock_bad_streak = 0;
                lock_sync_miss = 0;
                have_last_ctrl16 = 0;
                bitbuf_drop(&hps[locked_hyp].bits, (size_t)best_offset);
                if (cfg.verbose) {
                    fprintf(stderr, "locked: phase=%d reverse=%d invert=%d offset=%zd score=%d\n",
                            hps[locked_hyp].phase, hps[locked_hyp].reverse, hps[locked_hyp].invert, best_offset, best_score);
                }
            } else {
                for (size_t hyp = 0; hyp < hyp_count; hyp++) {
                    if (hps[hyp].bits.len > FRAME_BITS * 24) {
                        bitbuf_drop(&hps[hyp].bits, hps[hyp].bits.len - FRAME_BITS * 24);
                    }
                }
                continue;
            }
        }

        while (1) {
            BitBuffer *bb = &hps[locked_hyp].bits;
            if (bb->len < FRAME_BITS) {
                break;
            }
            if (faw_errors_at(bb->bits, 0) != 0) {
                int resync_shift = -1;
                for (int shift = 1; shift <= sync_resync_window; shift++) {
                    if (bb->len > (size_t)shift && faw_errors_at(bb->bits, (size_t)shift) == 0) {
                        resync_shift = shift;
                        break;
                    }
                }
                if (resync_shift >= 0) {
                    bitbuf_drop(bb, (size_t)resync_shift);
                    lock_sync_miss = 0;
                    continue;
                }
                lock_sync_miss++;
                bitbuf_drop(bb, 1);
                if (cfg.verbose && lock_sync_miss == 1) {
                    fprintf(stderr, "sync miss: count=%d\n", lock_sync_miss);
                }
                continue;
            }
            lock_sync_miss = 0;

            uint8_t body[BODY_BITS];
            uint8_t ctrl16[16] = {0};
            int16_t pcm[PCM_SAMPLES];
            if (descramble_body_phase(bb->bits, body, cfg.scramble_phase) != 0) {
                bitbuf_drop(bb, 1);
                lock_bad_streak++;
                if (lock_bad_streak >= cfg.lock_drop_frames) {
                    if (cfg.verbose) {
                        char cbits[8];
                        char abits[16];
                        describe_ctrl16(last_good_ctrl16, cbits, sizeof(cbits), abits, sizeof(abits));
                        fprintf(stderr,
                                "lost lock: descramble last_good_frame=%d bad_sync_count=%d bad_score_count=%d current_bit_offset=%d expected_next_faw_offset=%d ram_read_start=%d ram_read_stride=%d C=%s AD=%s parity_errors=%d pre_errors=%d lockq=%d\n",
                                last_good_frame,
                                lock_sync_miss,
                                lock_bad_streak,
                                0,
                                0,
                                cfg.ram_read_start,
                                cfg.ram_read_stride,
                                cbits,
                                abits,
                                last_good_post_errors,
                                last_good_pre_errors,
                                last_good_lock_quality);
                    }
                    locked = 0;
                    lock_confirmed = 0;
                    locked_hyp = -1;
                    lock_good_streak = 0;
                    lock_bad_streak = 0;
                    have_last_ctrl16 = 0;
                    break;
                }
                continue;
            }
            int pre_errors = 0;
            int parity_errors = 0;
            int left = 0;
            int right = 0;
            int frame_quality = frame_quality_score(
                bb->bits,
                &cfg,
                &pre_errors,
                &parity_errors,
                ctrl16,
                &left,
                &right);
            (void)left;
            (void)right;
            int mode_supported = ctrl_mode_supported_for_pcm(ctrl16);
            decode_payload_pcm(body, pcm, cfg.ram_read_start, cfg.ram_read_stride);
            int c_dist = have_last_ctrl16 ? hamming_distance_bits(last_ctrl16 + 1, ctrl16 + 1, 4) : 0;
            int lock_quality = frame_quality + (c_dist <= 1 ? 100 : (c_dist <= 2 ? 50 : 0));
            bitbuf_drop(bb, FRAME_BITS);

            if (!lock_confirmed) {
                if (lock_quality >= 400) {
                    lock_good_streak++;
                } else {
                    lock_good_streak = 0;
                }
                if (cfg.verbose) {
                    char cbits[8];
                    char abits[16];
                    describe_bits(ctrl16, 5, cbits, sizeof(cbits));
                    describe_bits(ctrl16 + 5, 11, abits, sizeof(abits));
                    fprintf(stderr,
                            "candidate frame: pre=%d post=%d score=%d lockq=%d cdist=%d good=%d C=%s AD=%s\n",
                            pre_errors,
                            parity_errors,
                            frame_quality,
                            lock_quality,
                            c_dist,
                            lock_good_streak,
                            cbits,
                            abits);
                }
                if (lock_good_streak >= cfg.lock_confirm_frames) {
                    lock_confirmed = 1;
                    last_good_frame = total_frames;
                    last_good_pre_errors = pre_errors;
                    last_good_post_errors = parity_errors;
                    last_good_lock_quality = lock_quality;
                    memcpy(last_good_ctrl16, ctrl16, sizeof(last_good_ctrl16));
                    if (cfg.verbose) {
                        char cbits[8];
                        char abits[16];
                        describe_ctrl16(ctrl16, cbits, sizeof(cbits), abits, sizeof(abits));
                        fprintf(stderr,
                                "lock confirmed: phase=%d reverse=%d invert=%d offset=%zd score=%d pre=%d post=%d C=%s AD=%s\n",
                                hps[locked_hyp].phase,
                                hps[locked_hyp].reverse,
                                hps[locked_hyp].invert,
                                best_offset,
                                frame_quality,
                                pre_errors,
                                parity_errors,
                                cbits,
                                abits);
                    }
                }
            }

            int unsupported_mode = !mode_supported && !cfg.allow_unsupported_modes;
            int parity_ok = parity_errors <= cfg.max_parity_errors;
            if (unsupported_mode) {
                parity_ok = 0;
                memset(pcm, 0, sizeof(pcm));
            }
            if (parity_ok) {
                station_id_update(&station_id, ctrl16);
                lock_bad_streak = 0;
                lock_sync_miss = 0;
                memcpy(last_pcm, pcm, sizeof(pcm));
                last_good_frame = total_frames;
                last_good_pre_errors = pre_errors;
                last_good_post_errors = parity_errors;
                last_good_lock_quality = lock_quality;
                memcpy(last_good_ctrl16, ctrl16, sizeof(last_good_ctrl16));
            } else {
                if (lock_confirmed) {
                    lock_bad_streak++;
                }
                if (cfg.verbose) {
                    fprintf(stderr, "bad frame: pre_errors=%d parity_errors=%d lockq=%d cdist=%d bad_streak=%d confirmed=%d mode=%d unsupported=%d\n",
                            pre_errors, parity_errors, lock_quality, c_dist, lock_bad_streak, lock_confirmed, ctrl_mode(ctrl16), unsupported_mode);
                }
                if (cfg.conceal_bad_frames && !unsupported_mode) {
                    memcpy(pcm, last_pcm, sizeof(pcm));
                }
                if (lock_confirmed && lock_bad_streak >= cfg.lock_drop_frames) {
                    if (cfg.verbose) {
                        char cbits[8];
                        char abits[16];
                        describe_ctrl16(last_good_ctrl16, cbits, sizeof(cbits), abits, sizeof(abits));
                        fprintf(stderr,
                                "lost lock: quality last_good_frame=%d bad_sync_count=%d bad_score_count=%d current_bit_offset=%d expected_next_faw_offset=%d ram_read_start=%d ram_read_stride=%d C=%s AD=%s parity_errors=%d pre_errors=%d lockq=%d\n",
                                last_good_frame,
                                lock_sync_miss,
                                lock_bad_streak,
                                0,
                                0,
                                cfg.ram_read_start,
                                cfg.ram_read_stride,
                                cbits,
                                abits,
                                last_good_post_errors,
                                last_good_pre_errors,
                                last_good_lock_quality);
                    }
                    locked = 0;
                    lock_confirmed = 0;
                    locked_hyp = -1;
                    lock_good_streak = 0;
                    lock_bad_streak = 0;
                    lock_sync_miss = 0;
                    have_last_ctrl16 = 0;
                    break;
                }
            }
            memcpy(last_ctrl16, ctrl16, sizeof(last_ctrl16));
            have_last_ctrl16 = 1;

            if (!cfg.bitstream_quality && write_all(pcm, sizeof(pcm)) < 0) {
                free(raw);
                free(chunk_bits);
                free(hps);
                free_frontend_filter(&frontend);
                free_matched_filter(&matched);
                return 0;
            }
            total_frames++;
        }

        for (size_t hyp = 0; hyp < hyp_count; hyp++) {
            if ((int)hyp == locked_hyp) {
                continue;
            }
            if (hps[hyp].bits.len > FRAME_BITS * 24) {
                bitbuf_drop(&hps[hyp].bits, hps[hyp].bits.len - FRAME_BITS * 24);
            }
        }
    }

    if (cfg.bitstream_quality) {
        uint8_t raw_faw[8];
        memcpy(raw_faw, FAW, sizeof(raw_faw));
        int best_error_sum = INT_MAX;
        ssize_t best_offset = -1;
        size_t best_hits = 0;
        size_t best_frames = 0;
        size_t best_len = 0;
        for (size_t hyp = 0; hyp < hyp_count; hyp++) {
            PatternScan scan = scan_pattern(hps[hyp].bits.bits, hps[hyp].bits.len, raw_faw);
            if (scan.best_error_sum < best_error_sum ||
                (scan.best_error_sum == best_error_sum && scan.hits > best_hits) ||
                (scan.best_error_sum == best_error_sum && scan.hits == best_hits && hps[hyp].bits.len > best_len)) {
                best_error_sum = scan.best_error_sum;
                best_offset = scan.best_offset;
                best_hits = scan.hits;
                best_frames = scan.frames_checked;
                best_quality_hyp = (int)hyp;
                best_len = hps[hyp].bits.len;
            }
        }
        if (best_quality_hyp < 0) {
            best_quality_hyp = locked_hyp >= 0 ? locked_hyp : 0;
        }
        if (best_quality_hyp >= 0) {
            fprintf(stderr,
                    "quality: selected_hyp=%d raw_faw_best_offset=%zd raw_faw_best_frames=%zu raw_faw_hits=%zu raw_faw_error_sum=%d\n",
                    best_quality_hyp, best_offset, best_frames, best_hits, best_error_sum);
            bitstream_quality_report(&hps[best_quality_hyp], &cfg);
        }
    }

    if (cfg.verbose) {
        fprintf(stderr, "decoded_frames=%d\n", total_frames);
    }
    free(raw);
    free(chunk_bits);
    free(hps);
    free_frontend_filter(&frontend);
    free_matched_filter(&matched);
    return 0;
}
