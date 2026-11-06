/*
 * image_dedupe.c
 *
 * Scans a folder of images and groups together:
 *   - exact duplicates (identical bytes)
 *   - near-duplicates (same image resized, or with a small edit like an added line)
 *
 * Method:
 *   1. MD5 hash of file bytes -> instantly catches exact duplicates.
 *   2. Perceptual hash (pHash, DCT-based) -> catches resized / lightly edited
 *      versions of the same image, compared using Hamming distance.
 *
 * Dependencies (header-only, no system libs/linking needed):
 *   stb_image.h          - image decoding (JPG, PNG, BMP, GIF, TGA, ...)
 *   stb_image_resize2.h  - high quality image resizing
 *   Both must sit in the same folder as this .c file.
 *   Get them from: https://github.com/nothings/stb
 *
 * Build:
 *   gcc -O2 -o image_dedupe image_dedupe.c -lm
 *
 * Run:
 *   ./image_dedupe <folderPath> [hammingThreshold]
 *
 * Example:
 *   ./image_dedupe ./photos 15
 *
 * hammingThreshold (default 15): max number of differing bits (out of 64)
 * for two images to be considered "similar". Lower = stricter.
 *   0-5    -> nearly identical (resize / recompression only)
 *   6-15   -> similar with minor edits (e.g. a small added line, light crop)
 *   16-20  -> looser match, may start including different-but-similar images
 *   21+    -> generally different images
 */

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

#define HASH_SIZE 32   /* image is resized to HASH_SIZE x HASH_SIZE grayscale before DCT */
#define HASH_BITS 8    /* we keep an 8x8 low-frequency block -> 64 bit hash */
#define HASH_LEN (HASH_BITS * HASH_BITS - 1) /* 63 bits: 8x8 block minus the DC term */
#define MAX_FILES 4096
#define MAX_PATH_LEN 1024

/* ---------- Minimal MD5 implementation (RFC 1321) ---------- */

typedef struct {
    uint32_t state[4];
    uint64_t count;
    unsigned char buffer[64];
} MD5_CTX;

static uint32_t md5_leftrotate(uint32_t x, uint32_t c) {
    return (x << c) | (x >> (32 - c));
}

static void md5_transform(uint32_t state[4], const unsigned char block[64]) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const uint32_t S[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };

    uint32_t M[16];
    for (int i = 0; i < 16; i++) {
        M[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1] << 8) |
               ((uint32_t)block[i*4+2] << 16) | ((uint32_t)block[i*4+3] << 24);
    }

    uint32_t A = state[0], B = state[1], C = state[2], D = state[3];

    for (int i = 0; i < 64; i++) {
        uint32_t F;
        int g;
        if (i < 16) { F = (B & C) | (~B & D); g = i; }
        else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) % 16; }
        else if (i < 48) { F = B ^ C ^ D; g = (3*i + 5) % 16; }
        else { F = C ^ (B | ~D); g = (7*i) % 16; }

        F = F + A + K[i] + M[g];
        A = D; D = C; C = B;
        B = B + md5_leftrotate(F, S[i]);
    }

    state[0] += A; state[1] += B; state[2] += C; state[3] += D;
}

static void md5_file(const char *path, unsigned char digest_out[16]) {
    FILE *f = fopen(path, "rb");
    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t total_len = 0;
    unsigned char block[64];
    size_t n;

    if (!f) { memset(digest_out, 0, 16); return; }

    while ((n = fread(block, 1, 64, f)) == 64) {
        md5_transform(state, block);
        total_len += 64;
    }

    /* last partial block + padding */
    unsigned char pad[128];
    memset(pad, 0, sizeof(pad));
    memcpy(pad, block, n);
    pad[n] = 0x80;
    total_len += n;

    size_t pad_len = (n < 56) ? 64 : 128;
    uint64_t bit_len = total_len * 8;
    memcpy(pad + pad_len - 8, &bit_len, 8);

    md5_transform(state, pad);
    if (pad_len == 128) md5_transform(state, pad + 64);

    fclose(f);

    for (int i = 0; i < 4; i++) {
        digest_out[i*4]   = (unsigned char)(state[i] & 0xff);
        digest_out[i*4+1] = (unsigned char)((state[i] >> 8) & 0xff);
        digest_out[i*4+2] = (unsigned char)((state[i] >> 16) & 0xff);
        digest_out[i*4+3] = (unsigned char)((state[i] >> 24) & 0xff);
    }
}

static void md5_to_hex(const unsigned char digest[16], char out[33]) {
    static const char *hexchars = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i*2]   = hexchars[(digest[i] >> 4) & 0xf];
        out[i*2+1] = hexchars[digest[i] & 0xf];
    }
    out[32] = '\0';
}

/* ---------- DCT-based perceptual hash ---------- */

static void dct_1d(const double *in, double *out, int n) {
    for (int k = 0; k < n; k++) {
        double sum = 0.0;
        for (int i = 0; i < n; i++) {
            sum += in[i] * cos((M_PI / n) * (i + 0.5) * k);
        }
        out[k] = sum * (k == 0 ? sqrt(1.0 / n) : sqrt(2.0 / n));
    }
}

static void dct_2d(double matrix[HASH_SIZE][HASH_SIZE], double out[HASH_SIZE][HASH_SIZE]) {
    double rows_dct[HASH_SIZE][HASH_SIZE];
    double in_buf[HASH_SIZE], out_buf[HASH_SIZE];

    /* DCT on rows */
    for (int r = 0; r < HASH_SIZE; r++) {
        for (int c = 0; c < HASH_SIZE; c++) in_buf[c] = matrix[r][c];
        dct_1d(in_buf, out_buf, HASH_SIZE);
        for (int c = 0; c < HASH_SIZE; c++) rows_dct[r][c] = out_buf[c];
    }

    /* DCT on columns */
    for (int c = 0; c < HASH_SIZE; c++) {
        for (int r = 0; r < HASH_SIZE; r++) in_buf[r] = rows_dct[r][c];
        dct_1d(in_buf, out_buf, HASH_SIZE);
        for (int r = 0; r < HASH_SIZE; r++) out[r][c] = out_buf[r];
    }
}

static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

/* Returns 1 on success, 0 on failure (unreadable/unsupported image). */
static int perceptual_hash(const char *path, char hash_out[HASH_LEN + 1]) {
    int width, height, channels;
    unsigned char *data = stbi_load(path, &width, &height, &channels, 1 /* force grayscale */);
    if (!data) return 0;

    unsigned char *resized = (unsigned char *)malloc(HASH_SIZE * HASH_SIZE);
    stbir_resize_uint8_linear(data, width, height, 0,
                               resized, HASH_SIZE, HASH_SIZE, 0,
                               STBIR_1CHANNEL);
    stbi_image_free(data);

    double matrix[HASH_SIZE][HASH_SIZE];
    for (int y = 0; y < HASH_SIZE; y++) {
        for (int x = 0; x < HASH_SIZE; x++) {
            matrix[y][x] = (double)resized[y * HASH_SIZE + x];
        }
    }
    free(resized);

    double dct_out[HASH_SIZE][HASH_SIZE];
    dct_2d(matrix, dct_out);

    double low_freq[HASH_LEN];
    int idx = 0;
    for (int y = 0; y < HASH_BITS; y++) {
        for (int x = 0; x < HASH_BITS; x++) {
            if (x == 0 && y == 0) continue; /* skip DC term */
            low_freq[idx++] = dct_out[y][x];
        }
    }

    double sorted[HASH_LEN];
    memcpy(sorted, low_freq, sizeof(low_freq));
    qsort(sorted, HASH_LEN, sizeof(double), compare_doubles);
    double median = sorted[HASH_LEN / 2];

    for (int i = 0; i < HASH_LEN; i++) {
        hash_out[i] = (low_freq[i] > median) ? '1' : '0';
    }
    hash_out[HASH_LEN] = '\0';
    return 1;
}

static int hamming_distance(const char *a, const char *b) {
    int dist = 0;
    for (int i = 0; i < HASH_LEN; i++) {
        if (a[i] != b[i]) dist++;
    }
    return dist;
}

/* ---------- Union-Find to group images transitively ---------- */

static int uf_parent[MAX_FILES];

static void uf_init(int n) {
    for (int i = 0; i < n; i++) uf_parent[i] = i;
}

static int uf_find(int x) {
    if (uf_parent[x] != x) {
        uf_parent[x] = uf_find(uf_parent[x]);
    }
    return uf_parent[x];
}

static void uf_union(int a, int b) {
    int rootA = uf_find(a);
    int rootB = uf_find(b);
    if (rootA != rootB) uf_parent[rootA] = rootB;
}

/* ---------- File collection ---------- */

static int has_image_extension(const char *name) {
    const char *exts[] = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tga", NULL};
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (int i = 0; exts[i] != NULL; i++) {
        if (strcasecmp(dot, exts[i]) == 0) return 1;
    }
    return 0;
}

static int collect_image_files(const char *folder, char paths[MAX_FILES][MAX_PATH_LEN]) {
    DIR *dir = opendir(folder);
    if (!dir) {
        fprintf(stderr, "Folder not found: %s\n", folder);
        exit(1);
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < MAX_FILES) {
        if (!has_image_extension(entry->d_name)) continue;

        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s/%s", folder, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
            strncpy(paths[count], full_path, MAX_PATH_LEN - 1);
            paths[count][MAX_PATH_LEN - 1] = '\0';
            count++;
        }
    }
    closedir(dir);
    return count;
}

/* ---------- Main ---------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <folderPath> [hammingThreshold]\n", argv[0]);
        return 1;
    }

    const char *folder_path = argv[1];
    int threshold = (argc > 2) ? atoi(argv[2]) : 15;

    static char paths[MAX_FILES][MAX_PATH_LEN];
    int n = collect_image_files(folder_path, paths);

    if (n == 0) {
        printf("No image files found in folder.\n");
        return 0;
    }

    printf("Found %d image(s). Analyzing...\n\n", n);

    /* Step 1: exact duplicates via MD5 */
    static char md5_hex[MAX_FILES][33];
    for (int i = 0; i < n; i++) {
        unsigned char digest[16];
        md5_file(paths[i], digest);
        md5_to_hex(digest, md5_hex[i]);
    }

    /* Step 2: perceptual hash for near-duplicate detection */
    static char phash[MAX_FILES][HASH_LEN + 1];
    static int phash_ok[MAX_FILES];
    for (int i = 0; i < n; i++) {
        phash_ok[i] = perceptual_hash(paths[i], phash[i]);
        if (!phash_ok[i]) {
            fprintf(stderr, "Could not process %s (unreadable/unsupported format)\n", paths[i]);
        }
    }

    uf_init(n);
    for (int i = 0; i < n; i++) {
        if (!phash_ok[i]) continue;
        for (int j = i + 1; j < n; j++) {
            if (!phash_ok[j]) continue;
            int dist = hamming_distance(phash[i], phash[j]);
            if (dist <= threshold) uf_union(i, j);
        }
    }

    /* ---------- Report: exact duplicates ---------- */

    printf("=== Exact duplicates (identical files) ===\n");
    static int md5_reported[MAX_FILES];
    int exact_found = 0;
    for (int i = 0; i < n; i++) {
        if (md5_reported[i]) continue;
        int members[MAX_FILES], mcount = 0;
        members[mcount++] = i;
        for (int j = i + 1; j < n; j++) {
            if (!md5_reported[j] && strcmp(md5_hex[i], md5_hex[j]) == 0) {
                members[mcount++] = j;
            }
        }
        if (mcount > 1) {
            exact_found = 1;
            for (int k = 0; k < mcount; k++) {
                printf("  %s\n", paths[members[k]]);
                md5_reported[members[k]] = 1;
            }
            printf("\n");
        }
    }
    if (!exact_found) printf("  None found.\n\n");

    /* ---------- Report: near-duplicate groups ---------- */

    printf("=== Similar / near-duplicate groups (Hamming distance <= %d) ===\n", threshold);
    static int group_reported[MAX_FILES];
    int similar_found = 0;
    int unique_count = 0;
    int group_count = 0;

    for (int i = 0; i < n; i++) {
        int root = uf_find(i);
        if (group_reported[root]) continue;
        group_reported[root] = 1;

        int members[MAX_FILES], mcount = 0;
        for (int j = i; j < n; j++) {
            if (uf_find(j) == root) members[mcount++] = j;
        }

        if (mcount > 1) {
            similar_found = 1;
            group_count++;
            printf("  Group:\n");
            for (int k = 0; k < mcount; k++) {
                printf("    - %s\n", paths[members[k]]);
            }
            printf("\n");
        } else {
            unique_count++;
        }
    }
    if (!similar_found) printf("  None found.\n\n");

    printf("=== Summary ===\n");
    printf("Total images: %d\n", n);
    printf("Unique images (no match found): %d\n", unique_count);
    printf("Groups with duplicates/similars: %d\n", group_count);

    return 0;
}
