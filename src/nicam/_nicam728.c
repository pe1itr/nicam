#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdint.h>

#define PAYLOAD_BITS 704
#define WORDS 64
#define WORD_BITS 11

static int range_to_shift(int range_word) {
    switch (range_word & 7) {
        case 7:
            return 4;
        case 6:
            return 3;
        case 5:
            return 2;
        case 3:
            return 1;
        default:
            return 0;
    }
}

static int clamp_int(int value, int lo, int hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static void deinterleave_words(const uint8_t *tx, uint8_t words[WORDS][WORD_BITS]) {
    for (int word = 0; word < WORDS; word++) {
        for (int bit = 0; bit < WORD_BITS; bit++) {
            int raw_index = word * WORD_BITS + bit;
            int row = raw_index % 44;
            int col = raw_index / 44;
            words[word][bit] = tx[row * 16 + col] & 1;
        }
    }
}

static int parity_syndrome(const uint8_t word[WORD_BITS]) {
    int parity = word[10] & 1;
    for (int i = 4; i < 10; i++) {
        parity ^= (word[i] & 1);
    }
    return parity & 1;
}

static void decode_ranges_and_errors(
    const uint8_t words[WORDS][WORD_BITS],
    int *left_range,
    int *right_range,
    int *errors
) {
    static const int masks[6] = {4, 4, 2, 2, 1, 1};
    int ranges[2] = {0, 0};
    int total_errors = 0;

    for (int start = 0; start < 6; start++) {
        int count = 0;
        int syndromes[9];
        for (int idx = 0, word = start; word < 54; word += 6, idx++) {
            syndromes[idx] = parity_syndrome(words[word]);
            count += syndromes[idx];
        }
        int bit = (count >= 5) ? 1 : 0;
        ranges[start & 1] |= bit * masks[start];
        for (int idx = 0; idx < 9; idx++) {
            if (syndromes[idx] != bit) {
                total_errors++;
            }
        }
    }

    for (int start = 54; start <= 59; start += 5) {
        int count = 0;
        int syndromes[5];
        for (int idx = 0; idx < 5; idx++) {
            syndromes[idx] = parity_syndrome(words[start + idx]);
            count += syndromes[idx];
        }
        int bit = (count >= 3) ? 1 : 0;
        for (int idx = 0; idx < 5; idx++) {
            if (syndromes[idx] != bit) {
                total_errors++;
            }
        }
    }

    *left_range = ranges[0];
    *right_range = ranges[1];
    *errors = total_errors;
}

static int decode_signed10(const uint8_t word[WORD_BITS]) {
    int value = 0;
    for (int i = 0; i < 10; i++) {
        value |= (word[i] & 1) << i;
    }
    if (value & 0x200) {
        value -= 0x400;
    }
    return value;
}

static int16_t decode_sample(const uint8_t word[WORD_BITS], int range_word) {
    int sample14 = decode_signed10(word) << range_to_shift(range_word);
    sample14 = clamp_int(sample14, -8192, 8191);
    return (int16_t)clamp_int(sample14 << 2, -32768, 32767);
}

static PyObject *py_payload_parity_errors(PyObject *self, PyObject *args) {
    Py_buffer payload_buf;
    if (!PyArg_ParseTuple(args, "y*", &payload_buf)) {
        return NULL;
    }
    if (payload_buf.len != PAYLOAD_BITS) {
        PyBuffer_Release(&payload_buf);
        PyErr_SetString(PyExc_ValueError, "payload must be 704 bytes/bits");
        return NULL;
    }

    uint8_t words[WORDS][WORD_BITS];
    deinterleave_words((const uint8_t *)payload_buf.buf, words);
    int left_range = 0;
    int right_range = 0;
    int errors = 0;
    decode_ranges_and_errors(words, &left_range, &right_range, &errors);
    PyBuffer_Release(&payload_buf);
    return PyLong_FromLong(errors);
}

static PyObject *py_decode_payload(PyObject *self, PyObject *args) {
    Py_buffer payload_buf;
    if (!PyArg_ParseTuple(args, "y*", &payload_buf)) {
        return NULL;
    }
    if (payload_buf.len != PAYLOAD_BITS) {
        PyBuffer_Release(&payload_buf);
        PyErr_SetString(PyExc_ValueError, "payload must be 704 bytes/bits");
        return NULL;
    }

    uint8_t words[WORDS][WORD_BITS];
    deinterleave_words((const uint8_t *)payload_buf.buf, words);
    int left_range = 0;
    int right_range = 0;
    int errors = 0;
    decode_ranges_and_errors(words, &left_range, &right_range, &errors);

    PyObject *pcm_obj = PyBytes_FromStringAndSize(NULL, WORDS * (Py_ssize_t)sizeof(int16_t));
    if (pcm_obj == NULL) {
        PyBuffer_Release(&payload_buf);
        return NULL;
    }
    int16_t *pcm = (int16_t *)PyBytes_AS_STRING(pcm_obj);
    for (int sample = 0; sample < 32; sample++) {
        pcm[2 * sample] = decode_sample(words[2 * sample], left_range);
        pcm[2 * sample + 1] = decode_sample(words[2 * sample + 1], right_range);
    }

    PyBuffer_Release(&payload_buf);
    return Py_BuildValue("Ni", pcm_obj, errors);
}

static PyMethodDef methods[] = {
    {"payload_parity_errors", py_payload_parity_errors, METH_VARARGS, "Return NICAM 728 payload parity/range errors"},
    {"decode_payload", py_decode_payload, METH_VARARGS, "Decode NICAM 728 payload to PCM16 bytes and error count"},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "_nicam728",
    "NICAM 728 payload helpers in C",
    -1,
    methods,
};

PyMODINIT_FUNC PyInit__nicam728(void) {
    return PyModule_Create(&module);
}
