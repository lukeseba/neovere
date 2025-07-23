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

audio_counter = 0

if gpu_enabled:
    import cupy as np
else:
    import numpy as np

import numpy as rnp
