/* PCM output helpers, J.17 deemphasis, and concealment interpolation. */

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
