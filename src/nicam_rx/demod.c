/* DQPSK demodulation, carrier/timing recovery, slicing, bit buffers, and sync. */

static double wrap_pi(double x) {
    while (x <= -M_PI) {
        x += 2.0 * M_PI;
    }
    while (x > M_PI) {
        x -= 2.0 * M_PI;
    }
    return x;
}
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
#if NICAM_ENABLE_LEGACY_DEMOD
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
#endif
#if NICAM_ENABLE_LEGACY_DEMOD
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
#endif
#if NICAM_ENABLE_LEGACY_DEMOD
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
#endif
#if NICAM_ENABLE_LEGACY_DEMOD
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
#endif

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

static double qpsk_slicer_confidence(complexd x) {
    double distances[4] = {
        (x.re - 1.0) * (x.re - 1.0) + x.im * x.im,
        x.re * x.re + (x.im - 1.0) * (x.im - 1.0),
        (x.re + 1.0) * (x.re + 1.0) + x.im * x.im,
        x.re * x.re + (x.im + 1.0) * (x.im + 1.0),
    };
    double best = distances[0];
    double second = distances[1];
    if (second < best) {
        double tmp = best;
        best = second;
        second = tmp;
    }
    for (int i = 2; i < 4; i++) {
        if (distances[i] < best) {
            second = best;
            best = distances[i];
        } else if (distances[i] < second) {
            second = distances[i];
        }
    }
    double denom = best + second + 1.0e-12;
    double confidence = (second - best) / denom;
    if (confidence < 0.0) {
        return 0.0;
    }
    if (confidence > 1.0) {
        return 1.0;
    }
    return confidence;
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
    st->slicer_confidence_sum = 0.0;
    st->slicer_confidence_count = 0;
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
            st->slicer_confidence_sum += qpsk_slicer_confidence(sym);
            st->slicer_confidence_count++;
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

static long long wall_time_ms(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

static void json_write_string(FILE *fp, const char *value) {
    fputc('"', fp);
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', fp);
            fputc((int)*p, fp);
        } else if (*p >= 32 && *p <= 126) {
            fputc((int)*p, fp);
        } else {
            fprintf(fp, "\\u%04x", (unsigned int)*p);
        }
    }
    fputc('"', fp);
}

static void write_status_json(
    const Config *cfg,
    const AdaptiveHypothesisState *st,
    const char *rx_status,
    int signal_present,
    int locked,
    int frames,
    int bad_frames,
    int pending_frames,
    int align_drop_bits,
    int sync_drop_bits,
    size_t bitbuffer_bits,
    size_t hyp,
    size_t faw_hits,
    int faw_error_sum,
    int resets,
    const StationIdState *station_id
) {
    if (cfg->stats_json_path == NULL || cfg->stats_json_path[0] == '\0') {
        return;
    }

    size_t tmp_path_len = strlen(cfg->stats_json_path) + 32;
    char *tmp_path = (char *)malloc(tmp_path_len);
    if (tmp_path == NULL) {
        return;
    }
    int n = snprintf(tmp_path, tmp_path_len, "%s.tmp.%ld", cfg->stats_json_path, (long)getpid());
    if (n < 0 || (size_t)n >= tmp_path_len) {
        free(tmp_path);
        return;
    }

    FILE *fp = fopen(tmp_path, "w");
    if (fp == NULL) {
        free(tmp_path);
        return;
    }

    double slicer_confidence =
        st->slicer_confidence_count > 0
            ? st->slicer_confidence_sum / (double)st->slicer_confidence_count
            : 0.0;
    double bad_frame_rate = frames > 0 ? (double)bad_frames / (double)frames : 0.0;
    double carrier_hz = -st->carrier_freq * (double)cfg->sample_rate / (2.0 * M_PI);

    fprintf(fp, "{\n");
    fprintf(fp, "  \"timestamp_ms\": %lld,\n", wall_time_ms());
    fprintf(fp, "  \"rx_status\": ");
    json_write_string(fp, rx_status);
    fprintf(fp, ",\n");
    fprintf(fp, "  \"signal_present\": %s,\n", signal_present ? "true" : "false");
    fprintf(fp, "  \"locked\": %s,\n", locked ? "true" : "false");
    fprintf(fp, "  \"frames\": %d,\n", frames);
    fprintf(fp, "  \"bad_frames\": %d,\n", bad_frames);
    if (signal_present) {
        fprintf(fp, "  \"bad_frame_rate\": %.9f,\n", bad_frame_rate);
    } else {
        fprintf(fp, "  \"bad_frame_rate\": null,\n");
    }
    fprintf(fp, "  \"pending_frames\": %d,\n", pending_frames);
    fprintf(fp, "  \"align_drop_bits\": %d,\n", align_drop_bits);
    fprintf(fp, "  \"sync_drop_bits\": %d,\n", sync_drop_bits);
    fprintf(fp, "  \"bitbuffer_bits\": %zu,\n", bitbuffer_bits);
    fprintf(fp, "  \"hypothesis\": %zu,\n", hyp);
    fprintf(fp, "  \"faw_hits\": %zu,\n", faw_hits);
    fprintf(fp, "  \"faw_error_sum\": %d,\n", faw_error_sum);
    fprintf(fp, "  \"resets\": %d,\n", resets);
    fprintf(fp, "  \"slicer_conf\": %.6f,\n", slicer_confidence);
    fprintf(fp, "  \"slicer_symbols\": %zu,\n", st->slicer_confidence_count);
    fprintf(fp, "  \"carrier_hz\": %.3f,\n", carrier_hz);
    fprintf(fp, "  \"omega\": %.9f,\n", st->omega);
    fprintf(fp, "  \"station_id\": ");
    json_write_string(fp, station_id->displayed);
    fprintf(fp, ",\n");
    fprintf(fp, "  \"station_cycles\": %u\n", station_id->cycles);
    fprintf(fp, "}\n");

    if (fclose(fp) != 0) {
        unlink(tmp_path);
        free(tmp_path);
        return;
    }
    if (rename(tmp_path, cfg->stats_json_path) != 0) {
        unlink(tmp_path);
    }
    free(tmp_path);
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
    int adaptive_last_json_frame = 0;
    int adaptive_no_faw_chunks = 0;
    int adaptive_resets = 0;
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
            if (best_stream_hits == 0) {
                adaptive_no_faw_chunks++;
                if (adaptive_no_faw_chunks >= 20) {
                    for (size_t hyp = 0; hyp < hyp_count; hyp++) {
                        adaptive_init_hypothesis(&hps[hyp], cfg);
                    }
                    pending_conceal_frames = 0;
                    have_last_pcm = 0;
                    have_last_output = 0;
                    adaptive_no_faw_chunks = 0;
                    adaptive_resets++;
                    continue;
                }
            } else {
                adaptive_no_faw_chunks = 0;
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
                int stats_due = cfg->stats_every > 0 &&
                    adaptive_frames - adaptive_last_stat_frame >= cfg->stats_every;
                int json_due = cfg->stats_json_every > 0 &&
                    adaptive_frames - adaptive_last_json_frame >= cfg->stats_json_every;
                if (stats_due || json_due) {
                    AdaptiveHypothesisState *best_stats = &hps[best_stream_hyp];
                    double slicer_confidence =
                        best_stats->slicer_confidence_count > 0
                            ? best_stats->slicer_confidence_sum / (double)best_stats->slicer_confidence_count
                            : 0.0;
                    int locked = best_stream_hits > 0 && best_stream_errors == 0;
                    int signal_present =
                        best_stream_hits > 0 &&
                        best_stream_errors <= (int)(best_stream_hits * 2);
                    const char *rx_status = locked ? "locked" : (signal_present ? "unlocked" : "idle");
                    if (json_due) {
                        write_status_json(
                            cfg,
                            best_stats,
                            rx_status,
                            signal_present,
                            locked,
                            adaptive_frames,
                            adaptive_bad_frames,
                            pending_conceal_frames,
                            adaptive_align_drops,
                            adaptive_sync_bit_drops,
                            bb->len,
                            best_stream_hyp,
                            best_stream_hits,
                            best_stream_errors,
                            adaptive_resets,
                            &station_id
                        );
                        adaptive_last_json_frame = adaptive_frames;
                    }
                    if (stats_due) {
                        fprintf(stderr,
                                "nicam_stats frames=%d bad=%d pending=%d align_drop_bits=%d sync_drop_bits=%d bb_bits=%zu hyp=%zu faw_hits=%zu faw_errsum=%d resets=%d slicer_conf=%.3f slicer_symbols=%zu station_id=%s station_cycles=%u\n",
                                adaptive_frames,
                                adaptive_bad_frames,
                                pending_conceal_frames,
                                adaptive_align_drops,
                                adaptive_sync_bit_drops,
                                bb->len,
                                best_stream_hyp,
                                best_stream_hits,
                                best_stream_errors,
                                adaptive_resets,
                                slicer_confidence,
                                best_stats->slicer_confidence_count,
                                station_id.displayed,
                                station_id.cycles);
                        adaptive_last_stat_frame = adaptive_frames;
                    }
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
    double slicer_confidence =
        best->slicer_confidence_count > 0
            ? best->slicer_confidence_sum / (double)best->slicer_confidence_count
            : 0.0;
    fprintf(stderr,
            "adaptive_quality: selected_hyp=%zu raw_faw_best_offset=%zd raw_faw_best_frames=%zu raw_faw_hits=%zu raw_faw_error_sum=%d\n",
            best_hyp, best_offset, best_frames, best_hits, best_error_sum);
    fprintf(stderr,
            "adaptive_quality: conj=%d reverse=%d invert=%d rot=%d swap=%d carrier_sign=%d timing_sign=%d initial_mu=%.4f initial_freq_hz=%.1f symbols=%zu bits=%zu q_hist=%zu/%zu/%zu/%zu slicer_conf=%.3f slicer_symbols=%zu omega=%.6f carrier_hz=%.1f\n",
            best->conjugate, best->reverse, best->invert, best->rotation, best->swap_bits,
            best->carrier_error_sign, best->timing_error_sign,
            best->initial_mu, best->initial_freq_hz, best->symbols, best->bits.len,
            best->q_hist[0], best->q_hist[1], best->q_hist[2], best->q_hist[3],
            slicer_confidence, best->slicer_confidence_count,
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
#if NICAM_ENABLE_LEGACY_DEMOD
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
#endif
