import subprocess
import os
import sys
import copy
import shutil
from scipy.io import wavfile
from scipy.fft import fft, fftfreq
from PIL import Image, ImageDraw, ImageFont
from PyQt5.QtCore import QFile
import librosa
import soundfile as sf
from openai import OpenAI
from pathlib import Path
import string
import random
from typing import List, Tuple
import re
import pickle



try:
    import cv2
except ImportError:
    print("Error: OpenCV is not installed. Please install it using `pip install opencv-python`.")
    exit(1)

fourcc = cv2.VideoWriter_fourcc(*'mp4v')

_paths = [%$#path#$%]
arial = "[%$#arial#$%]"

api_key = "" #[%$# #$%]
gpu_enabled = False #[%%# #$%]
dx = 1.0 #[%$#dx#$%]
dt = 1.0 #[%$#dt#$%]

audio_counter = 0

if gpu_enabled:
    import cupy as np
else:
    import numpy as np

import numpy as rnp


# ---------- Phase profiler ----------
import time as _time
from collections import defaultdict as _defaultdict
from contextlib import contextmanager as _contextmanager

_phase_totals = _defaultdict(float)
_phase_counts = _defaultdict(int)

@_contextmanager
def _profile(name):
    t = _time.perf_counter()
    try:
        yield
    finally:
        _phase_totals[name] += _time.perf_counter() - t
        _phase_counts[name] += 1

def _print_profile_summary():
    if not _phase_totals:
        return
    total = sum(_phase_totals.values())
    print("\n[profile] phase breakdown:")
    print(f"  {'phase':<24} {'total(s)':>10} {'calls':>8} {'avg(ms)':>10} {'%':>6}")
    for name in sorted(_phase_totals, key=lambda k: -_phase_totals[k]):
        t = _phase_totals[name]
        n = _phase_counts[name]
        avg_ms = (t / n) * 1000 if n else 0
        pct = (t / total) * 100 if total > 0 else 0
        print(f"  {name:<24} {t:>10.3f} {n:>8} {avg_ms:>10.3f} {pct:>5.1f}%")
    _phase_totals.clear()
    _phase_counts.clear()
