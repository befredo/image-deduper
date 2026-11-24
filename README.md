# Image Dedupe (C)

Finds exact duplicates and near-duplicates (resized copies, or copies with a
small edit such as an added line) in a folder of images.

## Setup (once per machine)

You need a C compiler. Nothing else: the stb headers are included in the repo.

- **Linux:** `sudo apt install build-essential` (or your distro's equivalent)
- **macOS:** `xcode-select --install`
- **Windows:** `winget install BrechtSanders.WinLibs.POSIX.UCRT`, then reopen the terminal

## Build (same command everywhere)

    cc -O2 -o image_dedupe image_dedupe.c -lm

(On Linux/macOS `make` does the same thing.)

## Run

    ./image_dedupe /path/to/folder          # Linux / macOS
    .\image_dedupe.exe C:\path\to\folder    # Windows PowerShell

Optional second argument: Hamming threshold (default 15, lower = stricter).

## Method

MD5 of file bytes catches exact duplicates. A DCT-based perceptual hash
(32x32 grayscale -> 64-bit fingerprint) plus Hamming distance catches resized
and lightly edited copies. Supports JPG, PNG, BMP, GIF, TGA (no WEBP).
