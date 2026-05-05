/* Input byte stream and IQ sample conversion. */

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
