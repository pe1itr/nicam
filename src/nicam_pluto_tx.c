#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <iio.h>
#include <math.h>
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
    const char *pcm_path;
    const char *iq_out_path;
    long long lo_hz;
    long long sample_rate;
    long long rf_bandwidth;
    double tx_gain;
    double tx_amplitude;
    double baseband_amplitude;
    double tone_hz;
    double tone_level;
    size_t buffer_frames;
    unsigned int kernel_buffers;
    int status_every;
    int realtime;
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

static void handle_signal(int sig) {
    (void)sig;
    stop_requested = 1;
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
            "Gebruik: %s --lo HZ [--source tone|silence|pcm] [--pcm-in FILE|-] "
            "[--sample-rate HZ] [--tx-gain DB] [--rf-bandwidth HZ] "
            "[--tx-amplitude A] [--nicam-rf-level 0..1023] [--tone-hz HZ] "
            "[--buffer-frames N] [--kernel-buffers N] [--station-id TEXT] "
            "[--status-every N] [--seconds N] [--iq-out FILE] [--no-realtime]\n",
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

static int write_double_attr(struct iio_channel *chn, const char *attr, double value) {
    char text[64];
    snprintf(text, sizeof(text), "%.6f", value);
    int ret = iio_channel_attr_write(chn, attr, text);
    if (ret < 0) {
        fprintf(stderr, "Kan %s niet zetten op %s: %s\n", attr, text, strerror(-ret));
    }
    return ret;
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

static size_t read_full(FILE *fp, uint8_t *buf, size_t bytes) {
    size_t have = 0;
    while (have < bytes && !stop_requested) {
        size_t got = fread(buf + have, 1, bytes - have, fp);
        if (got > 0) {
            have += got;
            continue;
        }
        if (ferror(fp)) {
            if (errno == EINTR) {
                clearerr(fp);
                continue;
            }
            break;
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

static void read_pcm_frame(FILE *fp, int16_t pcm[PCM_VALUES]) {
    uint8_t raw[PCM_VALUES * 2];
    size_t got = read_full(fp, raw, sizeof(raw));
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

static void modulate_frame(const uint8_t bits[FRAME_BITS], int *phase_quarter, double amp, int16_t *iq_i, int16_t *iq_q) {
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
        int16_t si = scale_to_i16(re);
        int16_t sq = scale_to_i16(im);
        for (int s = 0; s < 4; s++) {
            iq_i[pos] = si;
            iq_q[pos] = sq;
            pos++;
        }
    }
}

static void generate_frame(Config *cfg, FILE *pcm_fp, J17Filter *j17, long long frame_index, int *phase_quarter, int16_t *iq_i, int16_t *iq_q) {
    int16_t pcm[PCM_VALUES];
    uint8_t payload[PAYLOAD_BITS];
    uint8_t bits[FRAME_BITS];

    if (cfg->source == SOURCE_TONE) {
        make_tone_pcm(pcm, cfg->tone_hz, cfg->tone_level, frame_index);
    } else if (cfg->source == SOURCE_PCM) {
        read_pcm_frame(pcm_fp, pcm);
    } else {
        memset(pcm, 0, sizeof(pcm));
    }

    j17_process(j17, pcm);
    encode_payload(pcm, payload);
    build_frame_bits(payload, frame_index, cfg->station_id, bits);
    modulate_frame(bits, phase_quarter, cfg->baseband_amplitude * cfg->tx_amplitude, iq_i, iq_q);
}

static int parse_args(int argc, char **argv, Config *cfg) {
    cfg->uri = "ip:192.168.2.1";
    cfg->pcm_path = "-";
    cfg->iq_out_path = NULL;
    cfg->lo_hz = 0;
    cfg->sample_rate = SAMPLE_RATE_DEFAULT;
    cfg->rf_bandwidth = 1750000;
    cfg->tx_gain = 0.0;
    cfg->tx_amplitude = 3.0;
    cfg->baseband_amplitude = 200.0 / 1023.0;
    cfg->tone_hz = 1000.0;
    cfg->tone_level = 0.35;
    cfg->buffer_frames = 50;
    cfg->kernel_buffers = 4;
    cfg->status_every = 50;
    cfg->realtime = 1;
    cfg->seconds = 0;
    cfg->source = SOURCE_TONE;
    memset(cfg->station_id, 0, sizeof(cfg->station_id));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--uri") == 0 && i + 1 < argc) {
            cfg->uri = argv[++i];
        } else if (strcmp(argv[i], "--lo") == 0 && i + 1 < argc) {
            cfg->lo_hz = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--sample-rate") == 0 && i + 1 < argc) {
            cfg->sample_rate = atoll(argv[++i]);
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
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            usage(argv[0]);
            return -1;
        }
    }

    if (cfg->sample_rate != SAMPLE_RATE_DEFAULT) {
        fprintf(stderr, "Alleen sample-rate %d wordt nu native ondersteund\n", SAMPLE_RATE_DEFAULT);
        return -1;
    }
    if (cfg->lo_hz <= 0 && cfg->iq_out_path == NULL) {
        usage(argv[0]);
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
    int phase_quarter = 0;
    int16_t iq_i[IQ_SAMPLES_PER_FRAME];
    int16_t iq_q[IQ_SAMPLES_PER_FRAME];
    long long max_frames = cfg->seconds > 0 ? (long long)cfg->seconds * 1000 : 10000;
    for (long long frame = 0; frame < max_frames && !stop_requested; frame++) {
        generate_frame(cfg, pcm_fp, &j17, frame, &phase_quarter, iq_i, iq_q);
        for (int n = 0; n < IQ_SAMPLES_PER_FRAME; n++) {
            double tx_scale = cfg->tx_amplitude == 0.0 ? 1.0 : cfg->tx_amplitude;
            uint8_t pair[2] = {
                scale_to_u8((double)iq_i[n] / 32767.0 / tx_scale),
                scale_to_u8((double)iq_q[n] / 32767.0 / tx_scale),
            };
            if (fwrite(pair, 1, 2, out) != 2) {
                perror("fwrite");
                fclose(out);
                return 1;
            }
        }
    }
    fclose(out);
    return 0;
}

int main(int argc, char **argv) {
    Config cfg;
    if (parse_args(argc, argv, &cfg) < 0) {
        return 2;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);
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

    struct iio_context *ctx = iio_create_context_from_uri(cfg.uri);
    if (ctx == NULL) {
        fprintf(stderr, "Kan IIO context niet openen voor %s\n", cfg.uri);
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

    if (write_ll_attr(tx_lo, "frequency", cfg.lo_hz) < 0 ||
        write_ll_attr(tx_phy, "sampling_frequency", cfg.sample_rate) < 0 ||
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

    size_t buffer_samples = cfg.buffer_frames * IQ_SAMPLES_PER_FRAME;
    struct iio_buffer *buf = iio_device_create_buffer(tx, buffer_samples, false);
    if (buf == NULL) {
        fprintf(stderr, "Kan TX-buffer niet maken\n");
        iio_context_destroy(ctx);
        if (pcm_fp != stdin) {
            fclose(pcm_fp);
        }
        return 1;
    }

    int16_t *iq_i = malloc(buffer_samples * sizeof(int16_t));
    int16_t *iq_q = malloc(buffer_samples * sizeof(int16_t));
    if (iq_i == NULL || iq_q == NULL) {
        perror("malloc");
        free(iq_i);
        free(iq_q);
        iio_buffer_destroy(buf);
        iio_context_destroy(ctx);
        if (pcm_fp != stdin) {
            fclose(pcm_fp);
        }
        return 1;
    }

    J17Filter j17;
    j17_init(&j17);
    int phase_quarter = 0;
    long long buffers_sent = 0;
    long long frames_sent = 0;
    long long samples_sent = 0;
    long long max_frames = cfg.seconds > 0 ? (long long)cfg.seconds * 1000 : 0;
    double start_time = monotonic_seconds();
    double next_push_time = start_time;
    double buffer_seconds = (double)buffer_samples / (double)cfg.sample_rate;

    while (!stop_requested) {
        size_t frames_this = cfg.buffer_frames;
        if (max_frames > 0 && frames_sent + (long long)frames_this > max_frames) {
            frames_this = (size_t)(max_frames - frames_sent);
            if (frames_this == 0) {
                break;
            }
        }

        for (size_t f = 0; f < frames_this; f++) {
            generate_frame(&cfg, pcm_fp, &j17, frames_sent + (long long)f, &phase_quarter,
                           iq_i + f * IQ_SAMPLES_PER_FRAME, iq_q + f * IQ_SAMPLES_PER_FRAME);
        }
        for (size_t f = frames_this; f < cfg.buffer_frames; f++) {
            memset(iq_i + f * IQ_SAMPLES_PER_FRAME, 0, IQ_SAMPLES_PER_FRAME * sizeof(int16_t));
            memset(iq_q + f * IQ_SAMPLES_PER_FRAME, 0, IQ_SAMPLES_PER_FRAME * sizeof(int16_t));
        }

        char *pi = iio_buffer_first(buf, tx_i);
        char *pq = iio_buffer_first(buf, tx_q);
        ptrdiff_t step = iio_buffer_step(buf);
        char *end = iio_buffer_end(buf);
        for (size_t n = 0; n < buffer_samples && pi < end && pq < end; n++) {
            *(int16_t *)pi = iq_i[n];
            *(int16_t *)pq = iq_q[n];
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
        samples_sent += (long long)(frames_this * IQ_SAMPLES_PER_FRAME);
        if (cfg.realtime) {
            next_push_time += buffer_seconds;
            double now = monotonic_seconds();
            if (next_push_time < now - buffer_seconds) {
                next_push_time = now;
            }
        }
        if (cfg.status_every > 0 && buffers_sent % cfg.status_every == 0) {
            double elapsed = monotonic_seconds() - start_time;
            double nominal = (double)samples_sent / (double)cfg.sample_rate;
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
    iio_buffer_destroy(buf);
    iio_context_destroy(ctx);
    if (pcm_fp != stdin) {
        fclose(pcm_fp);
    }
    return 0;
}
