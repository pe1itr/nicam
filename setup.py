from __future__ import annotations

from setuptools import Extension, setup


setup(
    ext_modules=[
        Extension(
            "nicam._adaptive_demod",
            ["src/nicam/_adaptive_demod.c"],
            extra_compile_args=["-O3"],
        ),
        Extension(
            "nicam._qpsk_dsp",
            ["src/nicam/_qpsk_dsp.c"],
            extra_compile_args=["-O3"],
        ),
    ],
)
