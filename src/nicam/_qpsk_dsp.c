#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <math.h>

static int phase_step_from_bits(unsigned char b0, unsigned char b1) {
    unsigned char code = (unsigned char)((b0 << 1) | b1);
    switch (code) {
        case 0: return 0;   /* 00 */
        case 1: return -1;  /* 01 */
        case 2: return 1;   /* 10 */
        default: return 2;  /* 11 */
    }
}

static unsigned char nearest_quarter(float re, float im) {
    float are = fabsf(re);
    float aim = fabsf(im);
    if (are >= aim) {
        return (re >= 0.0f) ? 0 : 2;
    }
    return (im >= 0.0f) ? 1 : 3;
}

static PyObject *py_bits_to_symbols(PyObject *self, PyObject *args) {
    Py_buffer bits_buf;
    int initial_phase;
    if (!PyArg_ParseTuple(args, "y*i", &bits_buf, &initial_phase)) {
        return NULL;
    }
    if (bits_buf.len % 2 != 0) {
        PyBuffer_Release(&bits_buf);
        PyErr_SetString(PyExc_ValueError, "bit count must be even");
        return NULL;
    }

    Py_ssize_t n_symbols = bits_buf.len / 2;
    PyObject *out = PyBytes_FromStringAndSize(NULL, n_symbols * 8);
    if (out == NULL) {
        PyBuffer_Release(&bits_buf);
        return NULL;
    }

    const unsigned char *bits = (const unsigned char *)bits_buf.buf;
    float *iq = (float *)PyBytes_AS_STRING(out);
    int phase = ((initial_phase % 4) + 4) % 4;

    for (Py_ssize_t i = 0; i < n_symbols; i++) {
        int step = phase_step_from_bits(bits[2 * i] & 1, bits[2 * i + 1] & 1);
        phase = (phase + step) & 3;
        switch (phase) {
            case 0:
                iq[2 * i] = 1.0f;
                iq[2 * i + 1] = 0.0f;
                break;
            case 1:
                iq[2 * i] = 0.0f;
                iq[2 * i + 1] = 1.0f;
                break;
            case 2:
                iq[2 * i] = -1.0f;
                iq[2 * i + 1] = 0.0f;
                break;
            default:
                iq[2 * i] = 0.0f;
                iq[2 * i + 1] = -1.0f;
                break;
        }
    }

    PyBuffer_Release(&bits_buf);
    return Py_BuildValue("Ni", out, phase);
}

static PyObject *py_symbols_to_bits(PyObject *self, PyObject *args) {
    Py_buffer sym_buf;
    PyObject *prev_obj = Py_None;
    if (!PyArg_ParseTuple(args, "y*|O", &sym_buf, &prev_obj)) {
        return NULL;
    }
    if (sym_buf.len % 8 != 0) {
        PyBuffer_Release(&sym_buf);
        PyErr_SetString(PyExc_ValueError, "symbols buffer must be complex64");
        return NULL;
    }

    Py_ssize_t n = sym_buf.len / 8;
    const float *iq = (const float *)sym_buf.buf;
    int has_prev = 0;
    unsigned char prev_q = 0;
    if (prev_obj != Py_None) {
        double re = PyComplex_RealAsDouble(prev_obj);
        double im = PyComplex_ImagAsDouble(prev_obj);
        if (PyErr_Occurred()) {
            PyBuffer_Release(&sym_buf);
            return NULL;
        }
        prev_q = nearest_quarter((float)re, (float)im);
        has_prev = 1;
    }

    if (!has_prev && n < 2) {
        PyObject *empty = PyBytes_FromStringAndSize("", 0);
        PyBuffer_Release(&sym_buf);
        if (empty == NULL) {
            return NULL;
        }
        return Py_BuildValue("NO", empty, Py_None);
    }

    Py_ssize_t out_pairs = has_prev ? n : (n - 1);
    PyObject *out = PyBytes_FromStringAndSize(NULL, out_pairs * 2);
    if (out == NULL) {
        PyBuffer_Release(&sym_buf);
        return NULL;
    }
    unsigned char *bits = (unsigned char *)PyBytes_AS_STRING(out);

    Py_ssize_t i = 0;
    unsigned char curr_q;
    if (!has_prev) {
        prev_q = nearest_quarter(iq[0], iq[1]);
        i = 1;
    }
    Py_ssize_t out_idx = 0;
    for (; i < n; i++) {
        curr_q = nearest_quarter(iq[2 * i], iq[2 * i + 1]);
        unsigned char delta = (unsigned char)((curr_q - prev_q) & 3);
        switch (delta) {
            case 0:
                bits[out_idx++] = 0;
                bits[out_idx++] = 0;
                break;
            case 3:
                bits[out_idx++] = 0;
                bits[out_idx++] = 1;
                break;
            case 2:
                bits[out_idx++] = 1;
                bits[out_idx++] = 1;
                break;
            default:
                bits[out_idx++] = 1;
                bits[out_idx++] = 0;
                break;
        }
        prev_q = curr_q;
    }

    PyObject *last_symbol = PyComplex_FromDoubles((double)iq[2 * (n - 1)], (double)iq[2 * (n - 1) + 1]);
    PyBuffer_Release(&sym_buf);
    if (last_symbol == NULL) {
        Py_DECREF(out);
        return NULL;
    }
    return Py_BuildValue("NN", out, last_symbol);
}

static PyMethodDef methods[] = {
    {"bits_to_symbols", py_bits_to_symbols, METH_VARARGS, "Map DQPSK bits to complex64 symbols"},
    {"symbols_to_bits", py_symbols_to_bits, METH_VARARGS, "Demap DQPSK symbols to bits"},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "_qpsk_dsp",
    "QPSK DSP helpers in C",
    -1,
    methods,
};

PyMODINIT_FUNC PyInit__qpsk_dsp(void) {
    return PyModule_Create(&module);
}
