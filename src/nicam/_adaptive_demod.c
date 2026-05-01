#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int initialized;
    double sample_rate;
    double symbol_rate;
    double omega;
    double min_freqw;
    double max_freqw;
    double mu;
    double phase;
    double freqw;
    double agc_gain;
    double est_power;
    double hist_pr[3];
    double hist_pi[3];
    double hist_cr[3];
    double hist_ci[3];
    int have_prev;
    int prev_idx;
} DemodState;

static void state_capsule_destructor(PyObject *capsule) {
    DemodState *state = (DemodState *)PyCapsule_GetPointer(capsule, "nicam.adaptive_demod.state");
    free(state);
}

static PyObject *state_to_capsule(DemodState *state) {
    return PyCapsule_New(state, "nicam.adaptive_demod.state", state_capsule_destructor);
}

static DemodState *state_from_capsule(PyObject *obj) {
    return (DemodState *)PyCapsule_GetPointer(obj, "nicam.adaptive_demod.state");
}

static void rotate_sample(double ir, double iq, double phase, double *or_, double *oi) {
    double c = cos(phase);
    double s = sin(phase);
    *or_ = ir * c + iq * s;
    *oi = -ir * s + iq * c;
}

static int nearest_qpsk(double r, double i, double *cr, double *ci) {
    double ar = fabs(r);
    double ai = fabs(i);
    if (ar >= ai) {
        if (r >= 0) {
            *cr = 1.0;
            *ci = 0.0;
            return 0;
        }
        *cr = -1.0;
        *ci = 0.0;
        return 2;
    }
    if (i >= 0) {
        *cr = 0.0;
        *ci = 1.0;
        return 1;
    }
    *cr = 0.0;
    *ci = -1.0;
    return 3;
}

static void append_dibit(PyObject *out, int delta) {
    char bits[2];
    switch (delta & 3) {
    case 0:
        bits[0] = 0;
        bits[1] = 0;
        break;
    case 3:
        bits[0] = 0;
        bits[1] = 1;
        break;
    case 2:
        bits[0] = 1;
        bits[1] = 1;
        break;
    default:
        bits[0] = 1;
        bits[1] = 0;
        break;
    }
    PyByteArray_Resize(out, PyByteArray_Size(out) + 2);
    char *buf = PyByteArray_AS_STRING(out);
    Py_ssize_t n = PyByteArray_Size(out);
    buf[n - 2] = bits[0];
    buf[n - 1] = bits[1];
}

static PyObject *adaptive_create(PyObject *self, PyObject *args, PyObject *kwargs) {
    double sample_rate;
    double symbol_rate = 364000.0;
    double timing_phase = 0.0;
    double freq_offset = 0.0;
    static char *kwlist[] = {
        "sample_rate",
        "symbol_rate",
        "timing_phase",
        "freq_offset",
        NULL,
    };
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "d|ddd", kwlist, &sample_rate, &symbol_rate, &timing_phase, &freq_offset)) {
        return NULL;
    }

    DemodState *state = (DemodState *)calloc(1, sizeof(DemodState));
    if (!state) {
        return PyErr_NoMemory();
    }
    state->initialized = 1;
    state->sample_rate = sample_rate;
    state->symbol_rate = symbol_rate;
    state->omega = sample_rate / symbol_rate;
    state->mu = timing_phase;
    while (state->mu >= 1.0) {
        state->mu -= 1.0;
    }
    state->phase = 0.0;
    state->freqw = 2.0 * M_PI * freq_offset / sample_rate;
    double limit = 2.0 * M_PI * symbol_rate / 8.0 / sample_rate;
    state->min_freqw = state->freqw - limit;
    state->max_freqw = state->freqw + limit;
    state->est_power = 1.0;
    state->agc_gain = 1.0;
    state->have_prev = 0;
    state->prev_idx = 0;
    return state_to_capsule(state);
}

static PyObject *adaptive_process(PyObject *self, PyObject *args, PyObject *kwargs) {
    PyObject *capsule;
    Py_buffer raw;
    double pll_adjustment = 1.0;
    static char *kwlist[] = {"state", "raw", "pll_adjustment", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oy*|d", kwlist, &capsule, &raw, &pll_adjustment)) {
        return NULL;
    }

    DemodState *st = state_from_capsule(capsule);
    if (!st) {
        PyBuffer_Release(&raw);
        return NULL;
    }
    const uint8_t *data = (const uint8_t *)raw.buf;
    Py_ssize_t nsamples = raw.len / 2;

    if (nsamples < 2) {
        PyBuffer_Release(&raw);
        return Py_BuildValue("y#iddd", "", 0, 0, 0.0, 0.0, 0.0);
    }

    if (st->est_power == 1.0) {
        Py_ssize_t ninit = nsamples < 4096 ? nsamples : 4096;
        double p = 0.0;
        for (Py_ssize_t k = 0; k < ninit; ++k) {
            double r = ((double)data[2 * k] - 127.5) / 127.5;
            double i = ((double)data[2 * k + 1] - 127.5) / 127.5;
            p += r * r + i * i;
        }
        p /= (double)ninit;
        if (p > 0.0) {
            st->est_power = p;
            st->agc_gain = 1.0 / sqrt(p);
        }
    }

    double freq_alpha = 0.04 * pll_adjustment;
    double freq_beta = 0.0012 / st->omega * pll_adjustment;
    double gain_mu = 0.02;
    double max_mucorr = 0.1;
    double kest = 0.01;

    PyObject *bits = PyByteArray_FromStringAndSize(NULL, 0);
    if (!bits) {
        PyBuffer_Release(&raw);
        return NULL;
    }

    int decoded_symbols = 0;
    double err_power = 0.0;
    double sym_power = 0.0;
    Py_ssize_t pin = 0;

    while (pin < nsamples - 1) {
        if (st->mu < 1.0) {
            double r0 = ((double)data[2 * pin] - 127.5) / 127.5;
            double i0 = ((double)data[2 * pin + 1] - 127.5) / 127.5;
            double r1 = ((double)data[2 * (pin + 1)] - 127.5) / 127.5;
            double i1 = ((double)data[2 * (pin + 1) + 1] - 127.5) / 127.5;
            double rr0, ri0, rr1, ri1;
            rotate_sample(r0, i0, st->phase, &rr0, &ri0);
            rotate_sample(r1, i1, st->phase + st->freqw, &rr1, &ri1);

            double raw_r = rr0 * (1.0 - st->mu) + rr1 * st->mu;
            double raw_i = ri0 * (1.0 - st->mu) + ri1 * st->mu;
            double sr = raw_r * st->agc_gain;
            double si = raw_i * st->agc_gain;

            double cr, ci;
            int idx = nearest_qpsk(sr, si, &cr, &ci);

            if (st->have_prev) {
                append_dibit(bits, idx - st->prev_idx);
            }
            st->prev_idx = idx;
            st->have_prev = 1;

            double phase_error = atan2(si * cr - sr * ci, sr * cr + si * ci);
            st->phase += phase_error * freq_alpha;
            st->freqw += phase_error * freq_beta;
            if (st->freqw < st->min_freqw) {
                st->freqw = st->min_freqw;
            } else if (st->freqw > st->max_freqw) {
                st->freqw = st->max_freqw;
            }

            st->hist_pr[2] = st->hist_pr[1];
            st->hist_pi[2] = st->hist_pi[1];
            st->hist_cr[2] = st->hist_cr[1];
            st->hist_ci[2] = st->hist_ci[1];
            st->hist_pr[1] = st->hist_pr[0];
            st->hist_pi[1] = st->hist_pi[0];
            st->hist_cr[1] = st->hist_cr[0];
            st->hist_ci[1] = st->hist_ci[0];
            st->hist_pr[0] = sr;
            st->hist_pi[0] = si;
            st->hist_cr[0] = cr;
            st->hist_ci[0] = ci;

            double muerr =
                ((st->hist_pr[0] - st->hist_pr[2]) * st->hist_cr[1] +
                 (st->hist_pi[0] - st->hist_pi[2]) * st->hist_ci[1]) -
                ((st->hist_cr[0] - st->hist_cr[2]) * st->hist_pr[1] +
                 (st->hist_ci[0] - st->hist_ci[2]) * st->hist_pi[1]);
            double mucorr = muerr * gain_mu;
            if (mucorr < -max_mucorr) {
                mucorr = -max_mucorr;
            } else if (mucorr > max_mucorr) {
                mucorr = max_mucorr;
            }
            st->mu += st->omega + mucorr;

            double insp = raw_r * raw_r + raw_i * raw_i;
            st->est_power = insp * kest + st->est_power * (1.0 - kest);
            if (st->est_power > 0.0) {
                st->agc_gain = 1.0 / sqrt(st->est_power);
            }

            double er = sr - cr;
            double ei = si - ci;
            err_power += er * er + ei * ei;
            sym_power += cr * cr + ci * ci;
            ++decoded_symbols;
        }

        ++pin;
        st->mu -= 1.0;
        st->phase += st->freqw;
        if (st->phase > 1e6 || st->phase < -1e6) {
            st->phase = fmod(st->phase + M_PI, 2.0 * M_PI) - M_PI;
        }
    }

    double evm = decoded_symbols ? sqrt(err_power / (double)decoded_symbols) : 0.0;
    double snr = evm > 0.0 ? -20.0 * log10(evm) : 99.0;
    double freq_hz = st->freqw * st->sample_rate / (2.0 * M_PI);

    PyObject *bytes = PyBytes_FromStringAndSize(PyByteArray_AS_STRING(bits), PyByteArray_Size(bits));
    Py_DECREF(bits);
    PyBuffer_Release(&raw);
    if (!bytes) {
        return NULL;
    }
    PyObject *result = Py_BuildValue("Niddd", bytes, decoded_symbols, evm, snr, freq_hz);
    return result;
}

static PyMethodDef methods[] = {
    {"create", (PyCFunction)adaptive_create, METH_VARARGS | METH_KEYWORDS, "Create adaptive QPSK demod state."},
    {"process", (PyCFunction)adaptive_process, METH_VARARGS | METH_KEYWORDS, "Process interleaved uint8 IQ and return unpacked bits."},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "_adaptive_demod",
    NULL,
    -1,
    methods,
};

PyMODINIT_FUNC PyInit__adaptive_demod(void) {
    return PyModule_Create(&module);
}
