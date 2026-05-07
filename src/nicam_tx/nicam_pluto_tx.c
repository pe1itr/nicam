#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <iio.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BIT_RATE 728000
#define SYMBOL_RATE 364000
#define SAMPLE_RATE_DEFAULT (SYMBOL_RATE * 4)
#define FRAME_BITS 728
#define BODY_BITS 720
#define PAYLOAD_BITS 704
#define PCM_FRAMES 32
#define PCM_VALUES 64
#define IQ_SAMPLES_PER_FRAME 1456

static volatile sig_atomic_t stop_requested = 0;

static const uint8_t FAW[8] = {0, 1, 0, 0, 1, 1, 1, 0};
static uint8_t scramble[BODY_BITS];

typedef enum {
    SOURCE_TONE,
    SOURCE_SILENCE,
    SOURCE_PCM,
} SourceMode;

typedef struct {
    const char *uri;
    const char *connect_mode;
    const char *ip;
    const char *usb_uri;
    const char *pcm_path;
    const char *iq_out_path;
    char resolved_uri[256];
    long long lo_hz;
    long long baseband_sample_rate;
    long long tx_sample_rate;
    int tx_sample_rate_explicit;
    long long rf_bandwidth;
    double tx_gain;
    double tx_amplitude;
    double baseband_amplitude;
    double tone_hz;
    double tone_level;
    double pulse_rolloff;
    int pulse_span_symbols;
    size_t buffer_frames;
    unsigned int kernel_buffers;
    int status_every;
    int realtime;
    int pulse_shape;
    int pcm_timeout_ms;
    int seconds;
    SourceMode source;
    char station_id[9];
} Config;

typedef struct {
    double b0;
    double b1;
    double a1;
    double x1[2];
    double y1[2];
} J17Filter;

typedef struct {
    int enabled;
    int sps;
    int taps_len;
    double *taps;
    double *hist_i;
    double *hist_q;
} RrcShaper;

typedef struct {
    long long input_rate;
    long long output_rate;
    long long interp;
    long long decim;
    long long input_consumed;
    long long output_generated;
    long long input_index;
    long long phase;
    int32_t *frac_q15;
    int16_t *tail_i;
    int16_t *tail_q;
    size_t tail_len;
} SincResampler;

static double sinc1(double x);

static void handle_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction pipe_sa;
    memset(&pipe_sa, 0, sizeof(pipe_sa));
    pipe_sa.sa_handler = SIG_IGN;
    sigemptyset(&pipe_sa.sa_mask);
    sigaction(SIGPIPE, &pipe_sa, NULL);
}

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void sleep_until(double target) {
    while (!stop_requested) {
        double now = monotonic_seconds();
        double remaining = target - now;
        if (remaining <= 0.0) {
            return;
        }
        struct timespec req;
        req.tv_sec = (time_t)remaining;
        req.tv_nsec = (long)((remaining - (double)req.tv_sec) * 1000000000.0);
        if (req.tv_nsec < 0) {
            req.tv_nsec = 0;
        }
        nanosleep(&req, NULL);
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Gebruik: %s --lo HZ [--connect-mode network|usb|auto] [--ip A.B.C.D] [--usb-uri URI] "
            "[--source tone|silence|pcm] [--pcm-in FILE|-] "
            "[--baseband-sample-rate HZ] [--tx-sample-rate HZ] [--sample-rate HZ] "
            "[--tx-gain DB] [--rf-bandwidth HZ] "
            "[--tx-amplitude A] [--nicam-rf-level 0..1023] [--tone-hz HZ] "
            "[--pulse-rolloff R] [--pulse-span-symbols N] [--no-pulse-shape] "
            "[--buffer-frames N] [--kernel-buffers N] [--station-id TEXT] "
            "[--status-every N] [--pcm-timeout-ms N] [--seconds N] "
            "[--iq-out FILE] [--no-realtime]\n",
            prog);
}

static int16_t clamp_i16(long value) {
    if (value > 32767) {
        return 32767;
    }
    if (value < -32768) {
        return -32768;
    }
    return (int16_t)value;
}

static int16_t scale_to_i16(double sample) {
    if (sample > 1.0) {
        sample = 1.0;
    } else if (sample < -1.0) {
        sample = -1.0;
    }
    return clamp_i16(lrint(sample * 32767.0));
}

static uint8_t scale_to_u8(double sample) {
    if (sample > 1.0) {
        sample = 1.0;
    } else if (sample < -1.0) {
        sample = -1.0;
    }
    long out = lrint(sample * 127.5 + 127.5);
    if (out < 0) {
        out = 0;
    } else if (out > 255) {
        out = 255;
    }
    return (uint8_t)out;
}

static int write_ll_attr(struct iio_channel *chn, const char *attr, long long value) {
    int ret = iio_channel_attr_write_longlong(chn, attr, value);
    if (ret < 0) {
        fprintf(stderr, "Kan %s niet zetten op %lld: %s\n", attr, value, strerror(-ret));
    }
    return ret;
}

static long long gcd_ll(long long a, long long b) {
    if (a < 0) {
        a = -a;
    }
    if (b < 0) {
        b = -b;
    }
    while (b != 0) {
        long long t = a % b;
        a = b;
        b = t;
    }
    return a == 0 ? 1 : a;
}

static int init_resampler(SincResampler *rs, long long input_rate, long long output_rate) {
    memset(rs, 0, sizeof(*rs));
    if (input_rate <= 0 || output_rate <= 0) {
        return -1;
    }
    long long g = gcd_ll(input_rate, output_rate);
    rs->input_rate = input_rate;
    rs->output_rate = output_rate;
    rs->interp = output_rate / g;
    rs->decim = input_rate / g;
    rs->tail_len = 2;
    rs->frac_q15 = calloc((size_t)rs->interp, sizeof(int32_t));
    rs->tail_i = calloc(rs->tail_len, sizeof(int16_t));
    rs->tail_q = calloc(rs->tail_len, sizeof(int16_t));
    if (rs->frac_q15 == NULL || rs->tail_i == NULL || rs->tail_q == NULL) {
        free(rs->frac_q15);
        free(rs->tail_i);
        free(rs->tail_q);
        memset(rs, 0, sizeof(*rs));
        return -1;
    }
    for (long long phase = 0; phase < rs->interp; phase++) {
        rs->frac_q15[phase] = (int32_t)((phase * 32768 + rs->interp / 2) / rs->interp);
    }
    return 0;
}

static void free_resampler(SincResampler *rs) {
    free(rs->frac_q15);
    free(rs->tail_i);
    free(rs->tail_q);
    memset(rs, 0, sizeof(*rs));
}

static void resampler_store_tail(SincResampler *rs, const int16_t *in_i, const int16_t *in_q, size_t in_samples) {
    if (rs->tail_len == 0) {
        return;
    }
    if (in_samples >= rs->tail_len) {
        memcpy(rs->tail_i, in_i + in_samples - rs->tail_len, rs->tail_len * sizeof(int16_t));
        memcpy(rs->tail_q, in_q + in_samples - rs->tail_len, rs->tail_len * sizeof(int16_t));
        return;
    }
    size_t keep = rs->tail_len - in_samples;
    memmove(rs->tail_i, rs->tail_i + in_samples, keep * sizeof(int16_t));
    memmove(rs->tail_q, rs->tail_q + in_samples, keep * sizeof(int16_t));
    memcpy(rs->tail_i + keep, in_i, in_samples * sizeof(int16_t));
    memcpy(rs->tail_q + keep, in_q, in_samples * sizeof(int16_t));
}

static size_t resample_iq_block(
    SincResampler *rs,
    const int16_t *in_i,
    const int16_t *in_q,
    size_t in_samples,
    int16_t *out_i,
    int16_t *out_q,
    size_t out_samples
) {
    if (rs->input_rate == rs->output_rate) {
        size_t n = in_samples < out_samples ? in_samples : out_samples;
        memcpy(out_i, in_i, n * sizeof(int16_t));
        memcpy(out_q, in_q, n * sizeof(int16_t));
        rs->input_consumed += (long long)in_samples;
        rs->output_generated += (long long)n;
        resampler_store_tail(rs, in_i, in_q, in_samples);
        return n;
    }

    long long center = rs->input_index;
    long long phase = rs->phase;
    long long chunk_start = rs->input_consumed;
    long long decim = rs->decim;
    long long interp = rs->interp;
    const int32_t *frac_q15 = rs->frac_q15;
    for (size_t n = 0; n < out_samples; n++) {
        size_t j = (size_t)(center - chunk_start);
        size_t j1 = j + 1 < in_samples ? j + 1 : j;
        int32_t frac = frac_q15[phase];
        int32_t i0 = in_i[j];
        int32_t q0 = in_q[j];
        out_i[n] = (int16_t)(i0 + ((((int32_t)in_i[j1] - i0) * frac + 16384) >> 15));
        out_q[n] = (int16_t)(q0 + ((((int32_t)in_q[j1] - q0) * frac + 16384) >> 15));
        phase += decim;
        if (phase >= interp) {
            phase -= interp;
            center++;
        }
    }
    rs->input_index = center;
    rs->phase = phase;
    rs->input_consumed += (long long)in_samples;
    rs->output_generated += (long long)out_samples;
    resampler_store_tail(rs, in_i, in_q, in_samples);
    return out_samples;
}

static int write_double_attr(struct iio_channel *chn, const char *attr, double value) {
    char text[64];
    snprintf(text, sizeof(text), "%.6f", value);
    int ret = iio_channel_attr_write(chn, attr, text);
    if (ret < 0) {
        fprintf(stderr, "Kan %s niet zetten op %s: %s\n", attr, text, strerror(-ret));
    }
    return ret;
}

static int attr_list_contains_ll(const char *text, long long value) {
    long long range_min = 0;
    long long range_step = 0;
    long long range_max = 0;
    if (sscanf(text, " [ %lld %lld %lld ]", &range_min, &range_step, &range_max) == 3 &&
        range_step > 0 && range_min <= range_max) {
        return value >= range_min && value <= range_max && ((value - range_min) % range_step) == 0;
    }

    const char *p = text;
    while (*p != '\0') {
        while (*p != '\0' && !isdigit((unsigned char)*p) && *p != '-') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        errno = 0;
        char *end = NULL;
        long long first = strtoll(p, &end, 10);
        if (end == p || errno != 0) {
            p++;
            continue;
        }
        p = end;
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '-') {
            p++;
            errno = 0;
            long long last = strtoll(p, &end, 10);
            if (end != p && errno == 0) {
                if (value >= first && value <= last) {
                    return 1;
                }
                p = end;
                continue;
            }
        }
        if (value == first) {
            return 1;
        }
    }
    return 0;
}

static int attr_list_choose_rate(const char *text, long long minimum, long long *chosen) {
    long long range_min = 0;
    long long range_step = 0;
    long long range_max = 0;
    if (sscanf(text, " [ %lld %lld %lld ]", &range_min, &range_step, &range_max) == 3 &&
        range_step > 0 && range_min <= range_max) {
        const long long preferred[] = {3840000, 30720000};
        for (size_t i = 0; i < sizeof(preferred) / sizeof(preferred[0]); i++) {
            long long rate = preferred[i];
            if (rate >= minimum && rate >= range_min && rate <= range_max &&
                ((rate - range_min) % range_step) == 0) {
                *chosen = rate;
                return 0;
            }
        }

        long long candidate = minimum > range_min ? minimum : range_min;
        long long rem = (candidate - range_min) % range_step;
        if (rem != 0) {
            candidate += range_step - rem;
        }
        if (candidate <= range_max) {
            *chosen = candidate;
            return 0;
        }
        return -1;
    }

    const char *p = text;
    long long best = 0;
    while (*p != '\0') {
        while (*p != '\0' && !isdigit((unsigned char)*p) && *p != '-') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        errno = 0;
        char *end = NULL;
        long long first = strtoll(p, &end, 10);
        if (end == p || errno != 0) {
            p++;
            continue;
        }
        p = end;
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        long long candidate = first;
        if (*p == '-') {
            p++;
            errno = 0;
            long long last = strtoll(p, &end, 10);
            if (end != p && errno == 0) {
                if (minimum >= first && minimum <= last) {
                    candidate = minimum;
                } else if (first >= minimum) {
                    candidate = first;
                } else {
                    candidate = 0;
                }
                p = end;
            }
        } else if (candidate < minimum) {
            candidate = 0;
        }
        if (candidate > 0 && (best == 0 || candidate < best)) {
            best = candidate;
        }
    }
    if (best == 0) {
        return -1;
    }
    *chosen = best;
    return 0;
}

static int read_sampling_frequency_available(struct iio_channel *primary, struct iio_channel *fallback, char *buf, size_t buf_len) {
    ssize_t ret = iio_channel_attr_read(primary, "sampling_frequency_available", buf, buf_len);
    if (ret >= 0) {
        if ((size_t)ret >= buf_len) {
            buf[buf_len - 1] = '\0';
        }
        return 0;
    }
    if (fallback != NULL) {
        ret = iio_channel_attr_read(fallback, "sampling_frequency_available", buf, buf_len);
        if (ret >= 0) {
            if ((size_t)ret >= buf_len) {
                buf[buf_len - 1] = '\0';
            }
            return 0;
        }
    }
    return -1;
}

static int context_model_matches(struct iio_context *ctx, const char *needle) {
    const char *model = iio_context_get_attr_value(ctx, "hw_model");
    if (model != NULL && strstr(model, needle) != NULL) {
        return 1;
    }
    model = iio_context_get_attr_value(ctx, "model");
    return model != NULL && strstr(model, needle) != NULL;
}

static int context_prefers_native_nicam_rate(struct iio_context *ctx) {
    return context_model_matches(ctx, "Z7010") || context_model_matches(ctx, "AD9364");
}

static int resolve_tx_sample_rate(
    struct iio_context *ctx,
    struct iio_channel *tx_phy,
    struct iio_channel *tx_i,
    long long baseband_rate,
    long long requested_rate,
    int requested_explicit,
    long long *resolved_rate
) {
    char available[1024];
    memset(available, 0, sizeof(available));
    if (read_sampling_frequency_available(tx_phy, tx_i, available, sizeof(available)) < 0) {
        *resolved_rate = requested_explicit ? requested_rate : baseband_rate;
        fprintf(stderr,
                "Waarschuwing: sampling_frequency_available niet leesbaar; probeer Pluto/IIO TX sample-rate %lld Hz\n",
                *resolved_rate);
        return 0;
    }

    if (requested_explicit) {
        if (!attr_list_contains_ll(available, requested_rate)) {
            if (context_prefers_native_nicam_rate(ctx) && requested_rate == baseband_rate) {
                fprintf(stderr,
                        "nicam_pluto_tx: gevraagde TX sample-rate %lld Hz staat niet in sampling_frequency_available, maar dit Pluto-model gebruikt de native NICAM-rate toch: %s\n",
                        requested_rate, available);
                *resolved_rate = requested_rate;
                return 0;
            }
            fprintf(stderr,
                    "Gevraagde Pluto/IIO TX sample-rate %lld Hz wordt niet ondersteund. sampling_frequency_available: %s\n",
                    requested_rate, available);
            return -1;
        }
        *resolved_rate = requested_rate;
        return 0;
    }

    if (attr_list_contains_ll(available, baseband_rate)) {
        *resolved_rate = baseband_rate;
        fprintf(stderr,
                "nicam_pluto_tx: auto TX sample-rate kiest baseband-rate %lld Hz; apparaat ondersteunt: %s\n",
                *resolved_rate, available);
        return 0;
    }
    if (context_prefers_native_nicam_rate(ctx)) {
        *resolved_rate = baseband_rate;
        fprintf(stderr,
                "nicam_pluto_tx: auto TX sample-rate kiest native NICAM-rate %lld Hz voor dit Pluto-model, ondanks sampling_frequency_available: %s\n",
                *resolved_rate, available);
        return 0;
    }

    if (attr_list_choose_rate(available, baseband_rate, resolved_rate) < 0) {
        fprintf(stderr,
                "Geen geschikte Pluto/IIO TX sample-rate gevonden voor NICAM baseband %lld Hz. sampling_frequency_available: %s\n",
                baseband_rate, available);
        return -1;
    }
    fprintf(stderr,
            "nicam_pluto_tx: auto TX sample-rate kiest %lld Hz omdat baseband-rate %lld Hz niet door dit apparaat wordt ondersteund. sampling_frequency_available: %s\n",
            *resolved_rate, baseband_rate, available);
    return 0;
}

static int resolve_iio_uri(Config *cfg) {
    if (cfg->uri != NULL) {
        snprintf(cfg->resolved_uri, sizeof(cfg->resolved_uri), "%s", cfg->uri);
        return 0;
    }
    if (strcmp(cfg->connect_mode, "network") == 0) {
        if (cfg->ip == NULL || cfg->ip[0] == '\0') {
            fprintf(stderr, "NICAM TX connect mode network vereist een IP-adres (--ip / NICAM_TX_IP)\n");
            return -1;
        }
        snprintf(cfg->resolved_uri, sizeof(cfg->resolved_uri), "ip:%s", cfg->ip);
        return 0;
    }
    if (strcmp(cfg->connect_mode, "usb") == 0) {
        const char *usb_uri = (cfg->usb_uri != NULL && cfg->usb_uri[0] != '\0') ? cfg->usb_uri : "usb:";
        snprintf(cfg->resolved_uri, sizeof(cfg->resolved_uri), "%s", usb_uri);
        return 0;
    }
    if (strcmp(cfg->connect_mode, "auto") == 0) {
        snprintf(cfg->resolved_uri, sizeof(cfg->resolved_uri), "auto");
        return 0;
    }
    fprintf(stderr, "Onbekende NICAM TX connect mode %s; gebruik network, usb of auto\n", cfg->connect_mode);
    return -1;
}

static struct iio_context *open_iio_context(const Config *cfg) {
    if (strcmp(cfg->connect_mode, "auto") == 0 && cfg->uri == NULL) {
        return iio_create_default_context();
    }
    return iio_create_context_from_uri(cfg->resolved_uri);
}

static void log_iio_device_model(struct iio_context *ctx, struct iio_device *phy) {
    const char *model = iio_context_get_attr_value(ctx, "hw_model");
    if (model == NULL) {
        model = iio_context_get_attr_value(ctx, "model");
    }
    if (model != NULL && model[0] != '\0') {
        fprintf(stderr, "nicam_pluto_tx: device model=%s\n", model);
        return;
    }

    char phy_model[128];
    memset(phy_model, 0, sizeof(phy_model));
    if (phy != NULL && iio_device_attr_read(phy, "model", phy_model, sizeof(phy_model)) >= 0 && phy_model[0] != '\0') {
        fprintf(stderr, "nicam_pluto_tx: device model=%s\n", phy_model);
        return;
    }

    const char *ctx_name = iio_context_get_name(ctx);
    const char *phy_name = phy != NULL ? iio_device_get_name(phy) : NULL;
    fprintf(stderr, "nicam_pluto_tx: device model=%s%s%s\n",
            ctx_name != NULL ? ctx_name : "unknown",
            phy_name != NULL ? "/" : "",
            phy_name != NULL ? phy_name : "");
}

static void init_scramble(void) {
    uint16_t reg = 0x1ff;
    for (int i = 0; i < BODY_BITS + 9; i++) {
        uint8_t out = (reg >> 8) & 1;
        uint8_t feedback = ((reg >> 8) ^ (reg >> 4)) & 1;
        if (i >= 9) {
            scramble[i - 9] = out;
        }
        reg = (uint16_t)(((reg << 1) & 0x1fe) | feedback);
    }
}

static void j17_init(J17Filter *f) {
    double k = 2.0 * 32000.0;
    double zero = 3000.0;
    double pole = 3000.0 * sqrt(75.0);
    double gain = pow(75.0, 0.25);
    f->b0 = gain * (k + zero) / (k + pole);
    f->b1 = gain * (zero - k) / (k + pole);
    f->a1 = (pole - k) / (k + pole);
    f->x1[0] = f->x1[1] = 0.0;
    f->y1[0] = f->y1[1] = 0.0;
}

static void j17_process(J17Filter *f, int16_t pcm[PCM_VALUES]) {
    for (int frame = 0; frame < PCM_FRAMES; frame++) {
        for (int ch = 0; ch < 2; ch++) {
            int idx = frame * 2 + ch;
            double x = (double)pcm[idx];
            double y = f->b0 * x + f->b1 * f->x1[ch] - f->a1 * f->y1[ch];
            f->x1[ch] = x;
            f->y1[ch] = y;
            pcm[idx] = clamp_i16(lrint(y));
        }
    }
}

static double sinc1(double x) {
    if (fabs(x) < 1e-12) {
        return 1.0;
    }
    return sin(M_PI * x) / (M_PI * x);
}

static int rrc_init(RrcShaper *shaper, int enabled, int sps, double rolloff, int span_symbols) {
    memset(shaper, 0, sizeof(*shaper));
    shaper->enabled = enabled;
    shaper->sps = sps;
    if (!enabled) {
        return 0;
    }
    if (sps <= 0 || rolloff < 0.0 || rolloff > 1.0) {
        return -1;
    }

    int span = span_symbols < 2 ? 2 : span_symbols;
    if (span & 1) {
        span++;
    }
    int half = span * sps / 2;
    shaper->taps_len = half * 2 + 1;
    shaper->taps = calloc((size_t)shaper->taps_len, sizeof(double));
    shaper->hist_i = calloc((size_t)shaper->taps_len, sizeof(double));
    shaper->hist_q = calloc((size_t)shaper->taps_len, sizeof(double));
    if (shaper->taps == NULL || shaper->hist_i == NULL || shaper->hist_q == NULL) {
        return -1;
    }

    double sum = 0.0;
    for (int idx = 0; idx < shaper->taps_len; idx++) {
        double t = (double)(idx - half) / (double)sps;
        double tap;
        if (rolloff == 0.0) {
            tap = sinc1(t);
        } else if (fabs(t) < 1e-12) {
            tap = 1.0 + rolloff * (4.0 / M_PI - 1.0);
        } else if (fabs(fabs(4.0 * rolloff * t) - 1.0) < 1e-12) {
            double angle = M_PI / (4.0 * rolloff);
            tap = rolloff / sqrt(2.0) *
                  ((1.0 + 2.0 / M_PI) * sin(angle) +
                   (1.0 - 2.0 / M_PI) * cos(angle));
        } else {
            double numerator =
                sin(M_PI * t * (1.0 - rolloff)) +
                4.0 * rolloff * t * cos(M_PI * t * (1.0 + rolloff));
            double denominator = M_PI * t * (1.0 - pow(4.0 * rolloff * t, 2.0));
            tap = numerator / denominator;
        }
        shaper->taps[idx] = tap;
        sum += tap;
    }
    if (sum != 0.0) {
        for (int idx = 0; idx < shaper->taps_len; idx++) {
            shaper->taps[idx] /= sum;
        }
    }
    return 0;
}

static void rrc_free(RrcShaper *shaper) {
    free(shaper->taps);
    free(shaper->hist_i);
    free(shaper->hist_q);
    memset(shaper, 0, sizeof(*shaper));
}

static void rrc_push(RrcShaper *shaper, double in_i, double in_q, double *out_i, double *out_q) {
    memmove(shaper->hist_i + 1, shaper->hist_i, (size_t)(shaper->taps_len - 1) * sizeof(double));
    memmove(shaper->hist_q + 1, shaper->hist_q, (size_t)(shaper->taps_len - 1) * sizeof(double));
    shaper->hist_i[0] = in_i;
    shaper->hist_q[0] = in_q;

    double acc_i = 0.0;
    double acc_q = 0.0;
    for (int i = 0; i < shaper->taps_len; i++) {
        acc_i += shaper->taps[i] * shaper->hist_i[i];
        acc_q += shaper->taps[i] * shaper->hist_q[i];
    }
    *out_i = acc_i;
    *out_q = acc_q;
}

static size_t read_pcm_bytes(FILE *fp, uint8_t *buf, size_t bytes, int timeout_ms) {
    size_t have = 0;
    int fd = fileno(fp);
    while (have < bytes && !stop_requested) {
        if (fd < 0) {
            size_t got = fread(buf + have, 1, bytes - have, fp);
            if (got > 0) {
                have += got;
                continue;
            }
            break;
        }
        if (fd >= 0 && timeout_ms >= 0) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN | POLLHUP | POLLERR;
            pfd.revents = 0;
            int ready = poll(&pfd, 1, timeout_ms);
            if (ready == 0) {
                break;
            }
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if ((pfd.revents & POLLIN) == 0 && (pfd.revents & (POLLHUP | POLLERR)) != 0) {
                break;
            }
        }

        ssize_t got = read(fd, buf + have, bytes - have);
        if (got > 0) {
            have += (size_t)got;
            continue;
        }
        if (got == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    return have;
}

static void make_tone_pcm(int16_t pcm[PCM_VALUES], double tone_hz, double tone_level, long long frame_index) {
    long long start = frame_index * PCM_FRAMES;
    for (int n = 0; n < PCM_FRAMES; n++) {
        float sample_index = (float)(start + n);
        float phase = (float)(2.0 * M_PI * tone_hz / 32000.0) * sample_index;
        int16_t sample = clamp_i16(lrintf((float)tone_level * sinf(phase) * 32767.0f));
        pcm[2 * n] = sample;
        pcm[2 * n + 1] = sample;
    }
}

static void read_pcm_frame(const Config *cfg, FILE *fp, int16_t pcm[PCM_VALUES]) {
    uint8_t raw[PCM_VALUES * 2];
    size_t got = read_pcm_bytes(fp, raw, sizeof(raw), cfg->pcm_timeout_ms);
    if (got < sizeof(raw)) {
        memset(raw + got, 0, sizeof(raw) - got);
        if (fp != stdin && feof(fp)) {
            clearerr(fp);
            rewind(fp);
        }
    }
    for (int i = 0; i < PCM_VALUES; i++) {
        pcm[i] = (int16_t)((uint16_t)raw[2 * i] | ((uint16_t)raw[2 * i + 1] << 8));
    }
}

static int range_word_and_shift(const int16_t samples14[PCM_FRAMES], int *shift) {
    int peak = 0;
    for (int i = 0; i < PCM_FRAMES; i++) {
        int value = samples14[i];
        int mag = value < 0 ? -value : value;
        if (mag > peak) {
            peak = mag;
        }
    }
    if (peak >= 4096) {
        *shift = 4;
        return 7;
    }
    if (peak >= 2048) {
        *shift = 3;
        return 6;
    }
    if (peak >= 1024) {
        *shift = 2;
        return 5;
    }
    if (peak >= 512) {
        *shift = 1;
        return 3;
    }
    if (peak >= 256) {
        *shift = 0;
        return 4;
    }
    if (peak >= 128) {
        *shift = 0;
        return 2;
    }
    *shift = 0;
    return 0;
}

static void encode_channel(const int16_t *samples, int stride, uint8_t words[PCM_FRAMES][11], int *range_word) {
    int16_t samples14[PCM_FRAMES];
    for (int i = 0; i < PCM_FRAMES; i++) {
        int value = samples[i * stride] >> 2;
        if (value > 8191) {
            value = 8191;
        } else if (value < -8192) {
            value = -8192;
        }
        samples14[i] = (int16_t)value;
    }

    int shift = 0;
    *range_word = range_word_and_shift(samples14, &shift);
    for (int i = 0; i < PCM_FRAMES; i++) {
        int compressed = samples14[i] >> shift;
        uint16_t word = (uint16_t)(compressed & 0x3ff);
        int parity = 0;
        for (int bit = 0; bit < 10; bit++) {
            words[i][bit] = (word >> bit) & 1;
            if (bit >= 4) {
                parity ^= words[i][bit];
            }
        }
        words[i][10] = (uint8_t)parity;
    }
}

static void apply_signalling(uint8_t words[PCM_VALUES][11], int left_range, int right_range) {
    static const int masks[6] = {4, 4, 2, 2, 1, 1};
    int ranges[2] = {left_range, right_range};
    for (int start = 0; start < 6; start++) {
        if (ranges[start & 1] & masks[start]) {
            for (int word = start; word < 54; word += 6) {
                words[word][10] ^= 1;
            }
        }
    }
}

static void interleave_payload(const uint8_t raw[PAYLOAD_BITS], uint8_t out[PAYLOAD_BITS]) {
    for (int raw_index = 0; raw_index < PAYLOAD_BITS; raw_index++) {
        int row = raw_index % 44;
        int col = raw_index / 44;
        out[row * 16 + col] = raw[raw_index] & 1;
    }
}

static void encode_payload(int16_t pcm[PCM_VALUES], uint8_t payload[PAYLOAD_BITS]) {
    uint8_t left[PCM_FRAMES][11];
    uint8_t right[PCM_FRAMES][11];
    uint8_t words[PCM_VALUES][11];
    uint8_t raw[PAYLOAD_BITS];
    int left_range = 0;
    int right_range = 0;

    encode_channel(&pcm[0], 2, left, &left_range);
    encode_channel(&pcm[1], 2, right, &right_range);
    for (int i = 0; i < PCM_FRAMES; i++) {
        memcpy(words[2 * i], left[i], 11);
        memcpy(words[2 * i + 1], right[i], 11);
    }
    apply_signalling(words, left_range, right_range);

    for (int word = 0; word < PCM_VALUES; word++) {
        for (int bit = 0; bit < 11; bit++) {
            raw[word * 11 + bit] = words[word][bit];
        }
    }
    interleave_payload(raw, payload);
}

static void station_id_bits(const char station_id[9], long long frame_index, uint8_t ad[11]) {
    memset(ad, 0, 11);
    if (station_id[0] == '\0') {
        return;
    }
    int pos = (int)((frame_index / 120) % 8);
    unsigned char ch = station_id[pos] ? (unsigned char)station_id[pos] : (unsigned char)' ';
    for (int bit = 0; bit < 3; bit++) {
        ad[bit] = (pos >> bit) & 1;
    }
    for (int bit = 0; bit < 8; bit++) {
        ad[3 + bit] = (ch >> bit) & 1;
    }
}

static void build_frame_bits(const uint8_t payload[PAYLOAD_BITS], long long frame_index, const char station_id[9], uint8_t bits[FRAME_BITS]) {
    uint8_t body[BODY_BITS];
    uint8_t ad[11];
    station_id_bits(station_id, frame_index, ad);

    body[0] = ((frame_index % 16) < 8) ? 1 : 0;
    body[1] = 0;
    body[2] = 0;
    body[3] = 0;
    body[4] = 0;
    memcpy(body + 5, ad, 11);
    memcpy(body + 16, payload, PAYLOAD_BITS);

    memcpy(bits, FAW, 8);
    for (int i = 0; i < BODY_BITS; i++) {
        bits[8 + i] = (body[i] ^ scramble[i]) & 1;
    }
}

static void modulate_frame(
    const uint8_t bits[FRAME_BITS],
    int *phase_quarter,
    RrcShaper *shaper,
    double amp,
    int16_t *iq_i,
    int16_t *iq_q
) {
    size_t pos = 0;
    for (int b = 0; b < FRAME_BITS; b += 2) {
        int code = bits[b] * 2 + bits[b + 1];
        int step = 0;
        if (code == 1) {
            step = -1;
        } else if (code == 2) {
            step = 1;
        } else if (code == 3) {
            step = 2;
        }
        *phase_quarter = (*phase_quarter + step) & 3;
        double re = 0.0;
        double im = 0.0;
        switch (*phase_quarter) {
            case 0:
                re = amp;
                break;
            case 1:
                im = amp;
                break;
            case 2:
                re = -amp;
                break;
            default:
                im = -amp;
                break;
        }
        if (shaper->enabled) {
            for (int s = 0; s < shaper->sps; s++) {
                double out_i = 0.0;
                double out_q = 0.0;
                rrc_push(
                    shaper,
                    s == 0 ? re * (double)shaper->sps : 0.0,
                    s == 0 ? im * (double)shaper->sps : 0.0,
                    &out_i,
                    &out_q
                );
                iq_i[pos] = scale_to_i16(out_i);
                iq_q[pos] = scale_to_i16(out_q);
                pos++;
            }
        } else {
            int16_t si = scale_to_i16(re);
            int16_t sq = scale_to_i16(im);
            for (int s = 0; s < 4; s++) {
                iq_i[pos] = si;
                iq_q[pos] = sq;
                pos++;
            }
        }
    }
}

static void generate_frame(
    Config *cfg,
    FILE *pcm_fp,
    J17Filter *j17,
    RrcShaper *shaper,
    long long frame_index,
    int *phase_quarter,
    int16_t *iq_i,
    int16_t *iq_q
) {
    int16_t pcm[PCM_VALUES];
    uint8_t payload[PAYLOAD_BITS];
    uint8_t bits[FRAME_BITS];

    if (cfg->source == SOURCE_TONE) {
        make_tone_pcm(pcm, cfg->tone_hz, cfg->tone_level, frame_index);
    } else if (cfg->source == SOURCE_PCM) {
        read_pcm_frame(cfg, pcm_fp, pcm);
    } else {
        memset(pcm, 0, sizeof(pcm));
    }

    j17_process(j17, pcm);
    encode_payload(pcm, payload);
    build_frame_bits(payload, frame_index, cfg->station_id, bits);
    modulate_frame(bits, phase_quarter, shaper, cfg->baseband_amplitude * cfg->tx_amplitude, iq_i, iq_q);
}

static int parse_args(int argc, char **argv, Config *cfg) {
    cfg->uri = NULL;
    cfg->connect_mode = "network";
    cfg->ip = "192.168.2.1";
    cfg->usb_uri = "usb:";
    cfg->pcm_path = "-";
    cfg->iq_out_path = NULL;
    cfg->lo_hz = 0;
    cfg->baseband_sample_rate = SAMPLE_RATE_DEFAULT;
    cfg->tx_sample_rate = 0;
    cfg->tx_sample_rate_explicit = 0;
    cfg->rf_bandwidth = 1750000;
    cfg->tx_gain = 0.0;
    cfg->tx_amplitude = 3.0;
    cfg->baseband_amplitude = 200.0 / 1023.0;
    cfg->tone_hz = 1000.0;
    cfg->tone_level = 0.35;
    cfg->pulse_rolloff = 0.4;
    cfg->pulse_span_symbols = 6;
    cfg->buffer_frames = 50;
    cfg->kernel_buffers = 4;
    cfg->status_every = 50;
    cfg->realtime = 1;
    cfg->pulse_shape = 1;
    cfg->pcm_timeout_ms = 120;
    cfg->seconds = 0;
    cfg->source = SOURCE_TONE;
    memset(cfg->station_id, 0, sizeof(cfg->station_id));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--uri") == 0 && i + 1 < argc) {
            cfg->uri = argv[++i];
        } else if (strcmp(argv[i], "--connect-mode") == 0 && i + 1 < argc) {
            cfg->connect_mode = argv[++i];
        } else if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            cfg->ip = argv[++i];
        } else if (strcmp(argv[i], "--usb-uri") == 0 && i + 1 < argc) {
            cfg->usb_uri = argv[++i];
        } else if (strcmp(argv[i], "--lo") == 0 && i + 1 < argc) {
            cfg->lo_hz = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--sample-rate") == 0 && i + 1 < argc) {
            cfg->baseband_sample_rate = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--baseband-sample-rate") == 0 && i + 1 < argc) {
            cfg->baseband_sample_rate = atoll(argv[++i]);
        } else if ((strcmp(argv[i], "--tx-sample-rate") == 0 ||
                    strcmp(argv[i], "--device-sample-rate") == 0) && i + 1 < argc) {
            cfg->tx_sample_rate = atoll(argv[++i]);
            cfg->tx_sample_rate_explicit = 1;
        } else if (strcmp(argv[i], "--tx-gain") == 0 && i + 1 < argc) {
            cfg->tx_gain = atof(argv[++i]);
        } else if (strcmp(argv[i], "--rf-bandwidth") == 0 && i + 1 < argc) {
            cfg->rf_bandwidth = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--tx-amplitude") == 0 && i + 1 < argc) {
            cfg->tx_amplitude = atof(argv[++i]);
        } else if (strcmp(argv[i], "--nicam-rf-level") == 0 && i + 1 < argc) {
            cfg->baseband_amplitude = atof(argv[++i]) / 1023.0;
        } else if (strcmp(argv[i], "--amplitude") == 0 && i + 1 < argc) {
            cfg->baseband_amplitude = atof(argv[++i]);
        } else if (strcmp(argv[i], "--tone-hz") == 0 && i + 1 < argc) {
            cfg->tone_hz = atof(argv[++i]);
        } else if (strcmp(argv[i], "--tone-level") == 0 && i + 1 < argc) {
            cfg->tone_level = atof(argv[++i]);
        } else if (strcmp(argv[i], "--pulse-rolloff") == 0 && i + 1 < argc) {
            cfg->pulse_rolloff = atof(argv[++i]);
        } else if (strcmp(argv[i], "--pulse-span-symbols") == 0 && i + 1 < argc) {
            cfg->pulse_span_symbols = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--buffer-frames") == 0 && i + 1 < argc) {
            cfg->buffer_frames = (size_t)atoll(argv[++i]);
        } else if (strcmp(argv[i], "--buffer-samples") == 0 && i + 1 < argc) {
            size_t samples = (size_t)atoll(argv[++i]);
            cfg->buffer_frames = samples / IQ_SAMPLES_PER_FRAME;
            if (cfg->buffer_frames == 0) {
                cfg->buffer_frames = 1;
            }
        } else if (strcmp(argv[i], "--kernel-buffers") == 0 && i + 1 < argc) {
            cfg->kernel_buffers = (unsigned int)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--status-every") == 0 && i + 1 < argc) {
            cfg->status_every = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pcm-timeout-ms") == 0 && i + 1 < argc) {
            cfg->pcm_timeout_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            cfg->seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--station-id") == 0 && i + 1 < argc) {
            snprintf(cfg->station_id, sizeof(cfg->station_id), "%-8.8s", argv[++i]);
        } else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
            const char *source = argv[++i];
            if (strcmp(source, "tone") == 0) {
                cfg->source = SOURCE_TONE;
            } else if (strcmp(source, "silence") == 0) {
                cfg->source = SOURCE_SILENCE;
            } else if (strcmp(source, "pcm") == 0) {
                cfg->source = SOURCE_PCM;
            } else {
                usage(argv[0]);
                return -1;
            }
        } else if (strcmp(argv[i], "--pcm-in") == 0 && i + 1 < argc) {
            cfg->pcm_path = argv[++i];
            cfg->source = SOURCE_PCM;
        } else if (strcmp(argv[i], "--iq-out") == 0 && i + 1 < argc) {
            cfg->iq_out_path = argv[++i];
        } else if (strcmp(argv[i], "--no-realtime") == 0) {
            cfg->realtime = 0;
        } else if (strcmp(argv[i], "--no-pulse-shape") == 0) {
            cfg->pulse_shape = 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            usage(argv[0]);
            return -1;
        }
    }

    if (cfg->baseband_sample_rate != SAMPLE_RATE_DEFAULT) {
        fprintf(stderr, "Alleen NICAM baseband sample-rate %d wordt ondersteund; wijzig de TX device sample-rate voor Pluto/IIO\n", SAMPLE_RATE_DEFAULT);
        return -1;
    }
    if (cfg->tx_sample_rate_explicit && cfg->tx_sample_rate <= 0) {
        fprintf(stderr, "TX device sample-rate moet groter zijn dan nul\n");
        return -1;
    }
    if (!cfg->tx_sample_rate_explicit) {
        cfg->tx_sample_rate = cfg->baseband_sample_rate;
    }
    if (cfg->lo_hz <= 0 && cfg->iq_out_path == NULL) {
        usage(argv[0]);
        return -1;
    }
    if (resolve_iio_uri(cfg) < 0) {
        return -1;
    }
    if (cfg->buffer_frames == 0) {
        cfg->buffer_frames = 1;
    }
    return 0;
}

static int run_iq_file(Config *cfg, FILE *pcm_fp) {
    FILE *out = fopen(cfg->iq_out_path, "wb");
    if (out == NULL) {
        perror(cfg->iq_out_path);
        return 1;
    }

    J17Filter j17;
    j17_init(&j17);
    RrcShaper shaper;
    if (rrc_init(&shaper, cfg->pulse_shape, (int)(cfg->baseband_sample_rate / SYMBOL_RATE),
                 cfg->pulse_rolloff, cfg->pulse_span_symbols) < 0) {
        fprintf(stderr, "Kan RRC pulse shaper niet initialiseren\n");
        fclose(out);
        return 1;
    }
    int phase_quarter = 0;
    int16_t iq_i[IQ_SAMPLES_PER_FRAME];
    int16_t iq_q[IQ_SAMPLES_PER_FRAME];
    long long max_frames = cfg->seconds > 0 ? (long long)cfg->seconds * 1000 : 10000;
    for (long long frame = 0; frame < max_frames && !stop_requested; frame++) {
        generate_frame(cfg, pcm_fp, &j17, &shaper, frame, &phase_quarter, iq_i, iq_q);
        for (int n = 0; n < IQ_SAMPLES_PER_FRAME; n++) {
            double tx_scale = cfg->tx_amplitude == 0.0 ? 1.0 : cfg->tx_amplitude;
            uint8_t pair[2] = {
                scale_to_u8((double)iq_i[n] / 32767.0 / tx_scale),
                scale_to_u8((double)iq_q[n] / 32767.0 / tx_scale),
            };
            if (fwrite(pair, 1, 2, out) != 2) {
                perror("fwrite");
                fclose(out);
                rrc_free(&shaper);
                return 1;
            }
        }
    }
    rrc_free(&shaper);
    fclose(out);
    return 0;
}

int main(int argc, char **argv) {
    Config cfg;
    if (parse_args(argc, argv, &cfg) < 0) {
        return 2;
    }

    install_signal_handlers();
    init_scramble();

    FILE *pcm_fp = stdin;
    if (cfg.source == SOURCE_PCM && strcmp(cfg.pcm_path, "-") != 0) {
        pcm_fp = fopen(cfg.pcm_path, "rb");
        if (pcm_fp == NULL) {
            perror(cfg.pcm_path);
            return 1;
        }
    }

    if (cfg.iq_out_path != NULL) {
        int ret = run_iq_file(&cfg, pcm_fp);
        if (pcm_fp != stdin) {
            fclose(pcm_fp);
        }
        return ret;
    }

    fprintf(stderr, "nicam_pluto_tx: connect mode=%s resolved IIO URI=%s\n", cfg.connect_mode, cfg.resolved_uri);
    struct iio_context *ctx = open_iio_context(&cfg);
    if (ctx == NULL) {
        fprintf(stderr, "Kan IIO context niet openen voor %s\n", cfg.resolved_uri);
        if (pcm_fp != stdin) {
            fclose(pcm_fp);
        }
        return 1;
    }

    struct iio_device *phy = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_device *tx = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    if (phy == NULL || tx == NULL) {
        fprintf(stderr, "Kan Pluto devices ad9361-phy/cf-ad9361-dds-core-lpc niet vinden\n");
        iio_context_destroy(ctx);
        if (pcm_fp != stdin) {
            fclose(pcm_fp);
        }
        return 1;
    }
    log_iio_device_model(ctx, phy);

    struct iio_channel *tx_lo = iio_device_find_channel(phy, "altvoltage1", true);
    struct iio_channel *tx_phy = iio_device_find_channel(phy, "voltage0", true);
    struct iio_channel *tx_i = iio_device_find_channel(tx, "voltage0", true);
    struct iio_channel *tx_q = iio_device_find_channel(tx, "voltage1", true);
    if (tx_lo == NULL || tx_phy == NULL || tx_i == NULL || tx_q == NULL) {
        fprintf(stderr, "Kan Pluto TX-kanalen niet vinden\n");
        iio_context_destroy(ctx);
        if (pcm_fp != stdin) {
            fclose(pcm_fp);
        }
        return 1;
    }

    if (resolve_tx_sample_rate(ctx, tx_phy, tx_i, cfg.baseband_sample_rate, cfg.tx_sample_rate,
                               cfg.tx_sample_rate_explicit, &cfg.tx_sample_rate) < 0) {
        iio_context_destroy(ctx);
        if (pcm_fp != stdin) {
            fclose(pcm_fp);
        }
        return 1;
    }
    long long rate_gcd = gcd_ll(cfg.baseband_sample_rate, cfg.tx_sample_rate);
    fprintf(stderr,
            "nicam_pluto_tx: NICAM baseband sample rate=%lld Hz, Pluto/IIO TX sample rate=%lld Hz, resampler=%lld/%lld, RF bandwidth=%lld Hz\n",
            cfg.baseband_sample_rate, cfg.tx_sample_rate,
            cfg.tx_sample_rate / rate_gcd, cfg.baseband_sample_rate / rate_gcd, cfg.rf_bandwidth);

    if (write_ll_attr(tx_lo, "frequency", cfg.lo_hz) < 0 ||
        write_ll_attr(tx_phy, "sampling_frequency", cfg.tx_sample_rate) < 0 ||
        write_ll_attr(tx_phy, "rf_bandwidth", cfg.rf_bandwidth) < 0 ||
        write_double_attr(tx_phy, "hardwaregain", cfg.tx_gain) < 0) {
        iio_context_destroy(ctx);
        if (pcm_fp != stdin) {
            fclose(pcm_fp);
        }
        return 1;
    }

    iio_channel_enable(tx_i);
    iio_channel_enable(tx_q);
    if (cfg.kernel_buffers > 0) {
        int ret = iio_device_set_kernel_buffers_count(tx, cfg.kernel_buffers);
        if (ret < 0) {
            fprintf(stderr, "Waarschuwing: kernel buffer count niet gezet: %s\n", strerror(-ret));
        }
    }

    size_t baseband_buffer_samples = cfg.buffer_frames * IQ_SAMPLES_PER_FRAME;
    size_t device_buffer_samples = (size_t)(((long long)baseband_buffer_samples * cfg.tx_sample_rate + cfg.baseband_sample_rate - 1) / cfg.baseband_sample_rate);
    struct iio_buffer *buf = iio_device_create_buffer(tx, device_buffer_samples, false);
    if (buf == NULL) {
        fprintf(stderr, "Kan TX-buffer niet maken\n");
        iio_context_destroy(ctx);
        if (pcm_fp != stdin) {
            fclose(pcm_fp);
        }
        return 1;
    }

    int16_t *iq_i = malloc(baseband_buffer_samples * sizeof(int16_t));
    int16_t *iq_q = malloc(baseband_buffer_samples * sizeof(int16_t));
    int16_t *tx_i_buf = malloc(device_buffer_samples * sizeof(int16_t));
    int16_t *tx_q_buf = malloc(device_buffer_samples * sizeof(int16_t));
    if (iq_i == NULL || iq_q == NULL || tx_i_buf == NULL || tx_q_buf == NULL) {
        perror("malloc");
        free(iq_i);
        free(iq_q);
        free(tx_i_buf);
        free(tx_q_buf);
        iio_buffer_destroy(buf);
        iio_context_destroy(ctx);
        if (pcm_fp != stdin) {
            fclose(pcm_fp);
        }
        return 1;
    }

    J17Filter j17;
    j17_init(&j17);
    RrcShaper shaper;
    if (rrc_init(&shaper, cfg.pulse_shape, (int)(cfg.baseband_sample_rate / SYMBOL_RATE),
                 cfg.pulse_rolloff, cfg.pulse_span_symbols) < 0) {
        fprintf(stderr, "Kan RRC pulse shaper niet initialiseren\n");
        free(iq_i);
        free(iq_q);
        free(tx_i_buf);
        free(tx_q_buf);
        iio_buffer_destroy(buf);
        iio_context_destroy(ctx);
        if (pcm_fp != stdin) {
            fclose(pcm_fp);
        }
        return 1;
    }
    int phase_quarter = 0;
    SincResampler resampler;
    if (init_resampler(&resampler, cfg.baseband_sample_rate, cfg.tx_sample_rate) < 0) {
        fprintf(stderr, "Kan TX-resampler niet initialiseren\n");
        free(iq_i);
        free(iq_q);
        free(tx_i_buf);
        free(tx_q_buf);
        rrc_free(&shaper);
        iio_buffer_destroy(buf);
        iio_context_destroy(ctx);
        if (pcm_fp != stdin) {
            fclose(pcm_fp);
        }
        return 1;
    }
    long long buffers_sent = 0;
    long long frames_sent = 0;
    long long samples_sent = 0;
    long long max_frames = cfg.seconds > 0 ? (long long)cfg.seconds * 1000 : 0;
    double start_time = monotonic_seconds();
    double next_push_time = start_time;
    double buffer_seconds = (double)device_buffer_samples / (double)cfg.tx_sample_rate;

    while (!stop_requested) {
        size_t frames_this = cfg.buffer_frames;
        if (max_frames > 0 && frames_sent + (long long)frames_this > max_frames) {
            frames_this = (size_t)(max_frames - frames_sent);
            if (frames_this == 0) {
                break;
            }
        }

        for (size_t f = 0; f < frames_this; f++) {
            generate_frame(&cfg, pcm_fp, &j17, &shaper, frames_sent + (long long)f, &phase_quarter,
                           iq_i + f * IQ_SAMPLES_PER_FRAME, iq_q + f * IQ_SAMPLES_PER_FRAME);
        }
        for (size_t f = frames_this; f < cfg.buffer_frames; f++) {
            memset(iq_i + f * IQ_SAMPLES_PER_FRAME, 0, IQ_SAMPLES_PER_FRAME * sizeof(int16_t));
            memset(iq_q + f * IQ_SAMPLES_PER_FRAME, 0, IQ_SAMPLES_PER_FRAME * sizeof(int16_t));
        }

        size_t baseband_samples_this = frames_this * IQ_SAMPLES_PER_FRAME;
        size_t device_samples_this = (size_t)(((long long)baseband_samples_this * cfg.tx_sample_rate) / cfg.baseband_sample_rate);
        if (device_samples_this > device_buffer_samples) {
            device_samples_this = device_buffer_samples;
        }
        size_t resampled = resample_iq_block(&resampler, iq_i, iq_q, baseband_samples_this,
                                             tx_i_buf, tx_q_buf, device_samples_this);
        if (resampled < device_buffer_samples) {
            memset(tx_i_buf + resampled, 0, (device_buffer_samples - resampled) * sizeof(int16_t));
            memset(tx_q_buf + resampled, 0, (device_buffer_samples - resampled) * sizeof(int16_t));
        }

        char *pi = iio_buffer_first(buf, tx_i);
        char *pq = iio_buffer_first(buf, tx_q);
        ptrdiff_t step = iio_buffer_step(buf);
        char *end = iio_buffer_end(buf);
        for (size_t n = 0; n < device_buffer_samples && pi < end && pq < end; n++) {
            *(int16_t *)pi = tx_i_buf[n];
            *(int16_t *)pq = tx_q_buf[n];
            pi += step;
            pq += step;
        }

        if (cfg.realtime && buffers_sent > 0) {
            sleep_until(next_push_time);
        }
        ssize_t pushed = iio_buffer_push(buf);
        if (pushed < 0) {
            fprintf(stderr, "iio_buffer_push faalde: %s\n", strerror((int)-pushed));
            break;
        }

        buffers_sent++;
        frames_sent += (long long)frames_this;
        samples_sent += (long long)resampled;
        if (cfg.realtime) {
            next_push_time += buffer_seconds;
            double now = monotonic_seconds();
            if (next_push_time < now - buffer_seconds) {
                next_push_time = now;
            }
        }
        if (cfg.status_every > 0 && buffers_sent % cfg.status_every == 0) {
            double elapsed = monotonic_seconds() - start_time;
            double nominal = (double)samples_sent / (double)cfg.tx_sample_rate;
            fprintf(stderr,
                    "nicam_pluto_tx: buffers=%lld frames=%lld samples=%lld "
                    "elapsed=%.2f nominal=%.2f realtime=%d source=%d\n",
                    buffers_sent, frames_sent, samples_sent, elapsed, nominal,
                    cfg.realtime, (int)cfg.source);
        }
        if (max_frames > 0 && frames_sent >= max_frames) {
            break;
        }
    }

    free(iq_i);
    free(iq_q);
    free(tx_i_buf);
    free(tx_q_buf);
    free_resampler(&resampler);
    rrc_free(&shaper);
    iio_buffer_destroy(buf);
    iio_context_destroy(ctx);
    if (pcm_fp != stdin) {
        fclose(pcm_fp);
    }
    return 0;
}
