from noise import pnoise2
import numpy as np
import subprocess
import os
import sys
import copy
from scipy.io import wavfile
from scipy.fft import fft, fftfreq
from PIL import Image, ImageDraw, ImageFont
from PyQt5.QtCore import QFile

try:
    import cv2
except ImportError:
    print("Error: OpenCV is not installed. Please install it using `pip install opencv-python`.")
    exit(1)

fourcc = cv2.VideoWriter_fourcc(*'mp4v')

_path = "[%$#path#$%]"
arial = "[%$#arial#$%]"