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

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Gebruik: %s --lo HZ [--sample-rate HZ] [--tx-gain DB] "
            "[--rf-bandwidth HZ] [--amplitude A] [--buffer-samples N] "
            "[--kernel-buffers N] [--uri URI] [--iq-in FILE] "
            "[--status-every N] [--no-realtime]\n"
            "stdin/default input: rtl_sdr-style interleaved uint8 IQ\n",
            prog);
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

static int16_t u8_to_i16(uint8_t value, double amplitude) {
    double sample = ((double)value - 127.5) / 127.5;
    sample *= amplitude;
    if (sample > 1.0) {
        sample = 1.0;
    } else if (sample < -1.0) {
        sample = -1.0;
    }
    long out = lrint(sample * 32767.0);
    if (out > 32767) {
        out = 32767;
    } else if (out < -32768) {
        out = -32768;
    }
    return (int16_t)out;
}

int main(int argc, char **argv) {
    const char *uri = "ip:192.168.2.1";
    const char *iq_path = "-";
    long long lo_hz = 0;
    long long sample_rate = 1456000;
    long long rf_bandwidth = 1750000;
    double tx_gain = 0.0;
    double amplitude = 3.0;
    size_t buffer_samples = 65536;
    unsigned int kernel_buffers = 4;
    int status_every = 50;
    int realtime = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--uri") == 0 && i + 1 < argc) {
            uri = argv[++i];
        } else if (strcmp(argv[i], "--iq-in") == 0 && i + 1 < argc) {
            iq_path = argv[++i];
        } else if (strcmp(argv[i], "--lo") == 0 && i + 1 < argc) {
            lo_hz = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--sample-rate") == 0 && i + 1 < argc) {
            sample_rate = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--tx-gain") == 0 && i + 1 < argc) {
            tx_gain = atof(argv[++i]);
        } else if (strcmp(argv[i], "--rf-bandwidth") == 0 && i + 1 < argc) {
            rf_bandwidth = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--amplitude") == 0 && i + 1 < argc) {
            amplitude = atof(argv[++i]);
        } else if (strcmp(argv[i], "--buffer-samples") == 0 && i + 1 < argc) {
            buffer_samples = (size_t)atoll(argv[++i]);
        } else if (strcmp(argv[i], "--kernel-buffers") == 0 && i + 1 < argc) {
            kernel_buffers = (unsigned int)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--status-every") == 0 && i + 1 < argc) {
            status_every = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-realtime") == 0) {
            realtime = 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (lo_hz <= 0 || sample_rate <= 0 || buffer_samples == 0) {
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    FILE *iq_fp = stdin;
    if (strcmp(iq_path, "-") != 0) {
        iq_fp = fopen(iq_path, "rb");
        if (iq_fp == NULL) {
            perror(iq_path);
            return 1;
        }
    }

    struct iio_context *ctx = iio_create_context_from_uri(uri);
    if (ctx == NULL) {
        fprintf(stderr, "Kan IIO context niet openen voor %s\n", uri);
        if (iq_fp != stdin) {
            fclose(iq_fp);
        }
        return 1;
    }

    struct iio_device *phy = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_device *tx = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    if (phy == NULL || tx == NULL) {
        fprintf(stderr, "Kan Pluto devices ad9361-phy/cf-ad9361-dds-core-lpc niet vinden\n");
        iio_context_destroy(ctx);
        if (iq_fp != stdin) {
            fclose(iq_fp);
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
        if (iq_fp != stdin) {
            fclose(iq_fp);
        }
        return 1;
    }

    if (write_ll_attr(tx_lo, "frequency", lo_hz) < 0 ||
        write_ll_attr(tx_phy, "sampling_frequency", sample_rate) < 0 ||
        write_ll_attr(tx_phy, "rf_bandwidth", rf_bandwidth) < 0 ||
        write_double_attr(tx_phy, "hardwaregain", tx_gain) < 0) {
        iio_context_destroy(ctx);
        if (iq_fp != stdin) {
            fclose(iq_fp);
        }
        return 1;
    }

    iio_channel_enable(tx_i);
    iio_channel_enable(tx_q);
    if (kernel_buffers > 0) {
        int ret = iio_device_set_kernel_buffers_count(tx, kernel_buffers);
        if (ret < 0) {
            fprintf(stderr, "Waarschuwing: kernel buffer count niet gezet: %s\n", strerror(-ret));
        }
    }

    struct iio_buffer *buf = iio_device_create_buffer(tx, buffer_samples, false);
    if (buf == NULL) {
        fprintf(stderr, "Kan TX-buffer niet maken\n");
        iio_context_destroy(ctx);
        if (iq_fp != stdin) {
            fclose(iq_fp);
        }
        return 1;
    }

    uint8_t *raw = malloc(buffer_samples * 2);
    if (raw == NULL) {
        perror("malloc");
        iio_buffer_destroy(buf);
        iio_context_destroy(ctx);
        if (iq_fp != stdin) {
            fclose(iq_fp);
        }
        return 1;
    }

    long long buffers_sent = 0;
    long long samples_sent = 0;
    double start_time = monotonic_seconds();
    double next_push_time = start_time;
    double buffer_seconds = (double)buffer_samples / (double)sample_rate;
    while (!stop_requested) {
        size_t got = read_full(iq_fp, raw, buffer_samples * 2);
        if (got < 2) {
            break;
        }
        if (got % 2) {
            got--;
        }
        size_t samples = got / 2;
        char *pi = iio_buffer_first(buf, tx_i);
        char *pq = iio_buffer_first(buf, tx_q);
        ptrdiff_t step = iio_buffer_step(buf);
        char *end = iio_buffer_end(buf);
        for (size_t n = 0; n < samples && pi < end && pq < end; n++) {
            *(int16_t *)pi = u8_to_i16(raw[2 * n], amplitude);
            *(int16_t *)pq = u8_to_i16(raw[2 * n + 1], amplitude);
            pi += step;
            pq += step;
        }
        for (size_t n = samples; n < buffer_samples && pi < end && pq < end; n++) {
            *(int16_t *)pi = 0;
            *(int16_t *)pq = 0;
            pi += step;
            pq += step;
        }

        if (realtime && buffers_sent > 0) {
            sleep_until(next_push_time);
        }
        ssize_t pushed = iio_buffer_push(buf);
        if (pushed < 0) {
            fprintf(stderr, "iio_buffer_push faalde: %s\n", strerror((int)-pushed));
            break;
        }
        buffers_sent++;
        samples_sent += (long long)samples;
        if (realtime) {
            next_push_time += buffer_seconds;
            double now = monotonic_seconds();
            if (next_push_time < now - buffer_seconds) {
                next_push_time = now;
            }
        }
        if (status_every > 0 && buffers_sent % status_every == 0) {
            double elapsed = monotonic_seconds() - start_time;
            double nominal = (double)samples_sent / (double)sample_rate;
            fprintf(stderr,
                    "nicam_pluto_u8_tx: buffers=%lld samples=%lld amplitude=%.3f "
                    "elapsed=%.2f nominal=%.2f realtime=%d\n",
                    buffers_sent, samples_sent, amplitude, elapsed, nominal, realtime);
        }
        if (samples < buffer_samples) {
            break;
        }
    }

    free(raw);
    iio_buffer_destroy(buf);
    iio_context_destroy(ctx);
    if (iq_fp != stdin) {
        fclose(iq_fp);
    }
    return 0;
}
