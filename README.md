# Image Dedupe (C)

Scans a folder of images and groups exact duplicates and near-duplicates
(resized copies, or copies with a small edit like an added line).

## Build

Keep all three files in the same folder, then:

```
gcc -O2 -o image_dedupe image_dedupe.c -lm
```

No system image libraries needed - stb_image.h / stb_image_resize2.h are
public-domain single-header libraries that handle decoding (JPG, PNG, BMP,
GIF, TGA) and resizing internally.

## Run

```
./image_dedupe /path/to/your/folder            # default threshold: 15
./image_dedupe /path/to/your/folder 10          # stricter matching
```

## How it works

1. MD5 of raw file bytes -> catches byte-identical duplicates instantly.
2. Perceptual hash (DCT-based pHash): each image is resized to 32x32
   grayscale, a 2D DCT is applied, and a 64-bit fingerprint is built from
   the low-frequency coefficients (the first/DC coefficient is dropped).
   This fingerprint is stable under resizing and small local edits.
3. Hamming distance between fingerprints determines similarity; images
   within `threshold` bits of each other are grouped together
   (transitively, via union-find), so if A~B and B~C, all three land in
   one group even if A and C alone wouldn't match.

Tested against a synthetic set (original, exact copy, resized copy, copy
with a small added line, copy with a large added line, and an unrelated
image): all "same image" variants landed within Hamming distance 12 of
each other, while the unrelated image sat at distance 30+. That gap is
why 15 is a safe default threshold, but adjust it for your own images if
you see false positives/negatives.
# image-deduper
