# Image Dedupe

I download a lot of images that I think will be useful for my D&D campaign, and often find myself with the same image downloaded more than once, so I wanted to have a tool that helped me take out the duplicates of the same image.
The program finds exact and near duplicates (resized copies, or copies with a
small edit such as an added line) in a folder of images.

- [ ] Create a folder with the copies
- [ ] Give the option to look at the copies and delete them

## Setup

You need a C compiler.

- **Linux:** `sudo apt install build-essential`
- **macOS:** `xcode-select --install`
- **Windows:** `winget install BrechtSanders.WinLibs.POSIX.UCRT`

## Build

    gcc -O2 -o image_dedupe image_dedupe.c -lm

(On Linux/macOS `make` does the same thing.)

## Run

    ./image_dedupe /path/to/folder          # Linux / macOS
    .\image_dedupe.exe C:\path\to\folder    # Windows PowerShell

Optional second argument: Hamming threshold (default 15, lower = stricter).

## Method

MD5 of file bytes catches exact duplicates. A DCT-based perceptual hash
(32x32 grayscale -> 64-bit fingerprint) plus Hamming distance catches resized
and lightly edited copies. Supports JPG, PNG, BMP, GIF, TGA (no WEBP).
