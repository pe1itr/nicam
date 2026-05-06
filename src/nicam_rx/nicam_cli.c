#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
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

#ifndef NICAM_ENABLE_LEGACY_DEMOD
#define NICAM_ENABLE_LEGACY_DEMOD 0
#endif

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
    int stats_json_every;
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
    const char *stats_json_path;
} Config;

enum {
    IQ_FORMAT_U8 = 0,
    IQ_FORMAT_S16 = 1,
};

typedef struct {
    uint8_t bits[BITBUF_CAP];
    size_t len;
} BitBuffer;

#if NICAM_ENABLE_LEGACY_DEMOD
typedef struct {
    int have_prev;
    complexd prev;
} DemodState;
#endif

typedef struct {
    int phase;
    int timing_idx;
    int reverse;
    int invert;
    int rotation;
    int swap_bits;
    BitBuffer bits;
#if NICAM_ENABLE_LEGACY_DEMOD
    DemodState demod;
#endif
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
    double slicer_confidence_sum;
    size_t slicer_confidence_count;
} AdaptiveHypothesisState;

typedef struct {
    size_t frames_checked;
    size_t hits;
    int best_error_sum;
    ssize_t best_offset;
} PatternScan;

#if NICAM_ENABLE_LEGACY_DEMOD
typedef struct {
    size_t samples;
    size_t quarter_counts[4];
    double mean_magnitude;
    double mean_delta;
} SymbolStats;
#endif

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
    int last_pos;
    unsigned seen_mask;
    unsigned updates;
    unsigned cycles;
} StationIdState;

static uint8_t scramble[BODY_BITS];

static void usage(const char *argv0) {
    fprintf(stderr,
            "Gebruik: %s [--sample-rate 1456000] [--input-sample-rate HZ] [--iq-format u8|s16] [--frontend-lowpass-hz HZ] [--carrier-search-hz HZ] [--carrier-search-step-hz HZ] [--timing-search-steps N] [--matched-filter] [--adaptive-demod|--adaptive-fixed] [--descramble-phase N|--legacy-descramble] [--ram-read-start N] [--ram-read-stride N] [--lock-confirm-frames N] [--lock-drop-frames N] [--stats-every N] [--stats-json FILE] [--stats-json-every N] [--bitstream-quality] [--verbose]\n"
            "stdin: interleaved IQ, default rtl_sdr uint8; stdout: stereo s16le 32 kHz\n"
            "default: adaptive-fixed decoder; legacy non-adaptive demod requires -DNICAM_ENABLE_LEGACY_DEMOD=1\n",
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
    cfg->stats_json_every = 0;
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
    cfg->stats_json_path = NULL;

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
        } else if (strcmp(argv[i], "--stats-json-every") == 0 && i + 1 < argc) {
            cfg->stats_json_every = atoi(argv[++i]);
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
        } else if (strcmp(argv[i], "--stats-json") == 0 && i + 1 < argc) {
            cfg->stats_json_path = argv[++i];
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
    if (cfg->stats_json_every < 0) {
        cfg->stats_json_every = 0;
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

#include "input.c"
#include "dsp.c"
#include "nicam.c"
#include "output.c"
#include "demod.c"

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
#if !NICAM_ENABLE_LEGACY_DEMOD
    if (!cfg.adaptive_demod) {
        cfg.adaptive_demod = 1;
        cfg.adaptive_fixed = 1;
        cfg.matched_filter = 1;
    }
#endif
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

#if !NICAM_ENABLE_LEGACY_DEMOD
    fprintf(stderr, "legacy non-adaptive demod is disabled in this build; use --adaptive-fixed\n");
    free_frontend_filter(&frontend);
    free_matched_filter(&matched);
    return 2;
#else
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
#endif
}
