/* Sample-rate conversion and frontend/matched filtering. */

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
