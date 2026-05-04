#!/bin/bash
set -e

VENV_DIR="$HOME/neovere_venv"

echo "Creating virtual environment at: $VENV_DIR"
/usr/bin/python3 -m venv "$VENV_DIR"

echo "Installing dependencies..."
"$VENV_DIR/bin/pip" install --upgrade pip
"$VENV_DIR/bin/pip" install pillow opencv-python scipy librosa soundfile openai pyqt5 numpy psutil

echo ""
echo "Done. Neovere will automatically use this environment on next run."
