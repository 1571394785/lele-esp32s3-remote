#include "qrcode_v2.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define QR_DATA_CODEWORDS 16
#define QR_ECC_CODEWORDS 10
#define QR_TOTAL_CODEWORDS (QR_DATA_CODEWORDS + QR_ECC_CODEWORDS)

static uint8_t s_gf_exp[512];
static uint8_t s_gf_log[256];
static bool s_gf_ready;
static bool s_base[QR_V2_SIZE][QR_V2_SIZE];
static bool s_reserved[QR_V2_SIZE][QR_V2_SIZE];
static bool s_candidate[QR_V2_SIZE][QR_V2_SIZE];

static void gf_init(void)
{
    if (s_gf_ready) {
        return;
    }

    uint16_t x = 1;
    for (int i = 0; i < 255; i++) {
        s_gf_exp[i] = (uint8_t)x;
        s_gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) {
            x ^= 0x11D;
        }
    }
    for (int i = 255; i < 512; i++) {
        s_gf_exp[i] = s_gf_exp[i - 255];
    }
    s_gf_ready = true;
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) {
        return 0;
    }
    return s_gf_exp[s_gf_log[a] + s_gf_log[b]];
}

static void rs_generator(uint8_t gen[QR_ECC_CODEWORDS + 1])
{
    memset(gen, 0, QR_ECC_CODEWORDS + 1);
    gen[0] = 1;
    int degree = 0;
    for (int i = 0; i < QR_ECC_CODEWORDS; i++) {
        uint8_t next[QR_ECC_CODEWORDS + 1] = {0};
        for (int j = 0; j <= degree; j++) {
            next[j] ^= gf_mul(gen[j], s_gf_exp[i]);
            next[j + 1] ^= gen[j];
        }
        memcpy(gen, next, sizeof(next));
        degree++;
    }
}

static void rs_ecc(const uint8_t data[QR_DATA_CODEWORDS], uint8_t ecc[QR_ECC_CODEWORDS])
{
    uint8_t gen[QR_ECC_CODEWORDS + 1];
    rs_generator(gen);
    memset(ecc, 0, QR_ECC_CODEWORDS);

    for (int i = 0; i < QR_DATA_CODEWORDS; i++) {
        uint8_t factor = data[i] ^ ecc[0];
        memmove(&ecc[0], &ecc[1], QR_ECC_CODEWORDS - 1);
        ecc[QR_ECC_CODEWORDS - 1] = 0;
        for (int j = 0; j < QR_ECC_CODEWORDS; j++) {
            ecc[j] ^= gf_mul(gen[j + 1], factor);
        }
    }
}

static void set_module(bool m[QR_V2_SIZE][QR_V2_SIZE], bool reserved[QR_V2_SIZE][QR_V2_SIZE],
                       int x, int y, bool on)
{
    if (x >= 0 && x < QR_V2_SIZE && y >= 0 && y < QR_V2_SIZE) {
        m[y][x] = on;
        reserved[y][x] = true;
    }
}

static void finder(bool m[QR_V2_SIZE][QR_V2_SIZE], bool reserved[QR_V2_SIZE][QR_V2_SIZE],
                   int x, int y)
{
    for (int dy = -1; dy <= 7; dy++) {
        for (int dx = -1; dx <= 7; dx++) {
            int xx = x + dx;
            int yy = y + dy;
            if (xx < 0 || xx >= QR_V2_SIZE || yy < 0 || yy >= QR_V2_SIZE) {
                continue;
            }
            bool on = dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6 &&
                      (dx == 0 || dx == 6 || dy == 0 || dy == 6 ||
                       (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4));
            set_module(m, reserved, xx, yy, on);
        }
    }
}

static int alnum_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    switch (c) {
    case ' ': return 36;
    case '$': return 37;
    case '%': return 38;
    case '*': return 39;
    case '+': return 40;
    case '-': return 41;
    case '.': return 42;
    case '/': return 43;
    case ':': return 44;
    default: return -1;
    }
}

static uint16_t format_bits(int mask)
{
    uint16_t data = (uint16_t)mask; // Error correction M.
    uint16_t v = (uint16_t)(data << 10);
    for (int i = 14; i >= 10; i--) {
        if ((v >> i) & 1U) {
            v ^= (uint16_t)(0x537U << (i - 10));
        }
    }
    return (uint16_t)(((data << 10) | v) ^ 0x5412U);
}

static void place_format(bool m[QR_V2_SIZE][QR_V2_SIZE], int mask)
{
    uint16_t f = format_bits(mask);
    const uint8_t c1[15][2] = {
        {0, 8}, {1, 8}, {2, 8}, {3, 8}, {4, 8}, {5, 8}, {7, 8}, {8, 8},
        {8, 7}, {8, 5}, {8, 4}, {8, 3}, {8, 2}, {8, 1}, {8, 0},
    };
    const uint8_t c2[15][2] = {
        {8, 20}, {8, 19}, {8, 18}, {8, 17}, {8, 16}, {8, 15}, {8, 14},
        {13, 8}, {14, 8}, {15, 8}, {16, 8}, {17, 8}, {18, 8}, {19, 8}, {20, 8},
    };
    for (int i = 0; i < 15; i++) {
        m[c1[i][1]][c1[i][0]] = ((f >> i) & 1U) != 0;
        m[c2[i][1]][c2[i][0]] = ((f >> i) & 1U) != 0;
    }
}

static bool mask_on(int mask, int x, int y)
{
    switch (mask) {
    case 0: return ((x + y) & 1) == 0;
    case 1: return (y & 1) == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return ((y / 2) + (x / 3)) % 2 == 0;
    case 5: return ((x * y) % 2 + (x * y) % 3) == 0;
    case 6: return (((x * y) % 2 + (x * y) % 3) & 1) == 0;
    default: return (((x + y) % 2 + (x * y) % 3) & 1) == 0;
    }
}

static int run_penalty(const bool *line, int len)
{
    int penalty = 0;
    int run = 1;
    bool last = line[0];
    for (int i = 1; i < len; i++) {
        if (line[i] == last) {
            run++;
        } else {
            if (run >= 5) {
                penalty += 3 + run - 5;
            }
            run = 1;
            last = line[i];
        }
    }
    if (run >= 5) {
        penalty += 3 + run - 5;
    }
    return penalty;
}

static int penalty_score(bool m[QR_V2_SIZE][QR_V2_SIZE])
{
    int penalty = 0;
    int dark = 0;
    const bool pattern[11] = {true, false, true, true, true, false, true, false, false, false, false};

    for (int y = 0; y < QR_V2_SIZE; y++) {
        penalty += run_penalty(m[y], QR_V2_SIZE);
        for (int x = 0; x < QR_V2_SIZE; x++) {
            if (m[y][x]) dark++;
        }
    }
    for (int x = 0; x < QR_V2_SIZE; x++) {
        bool col[QR_V2_SIZE];
        for (int y = 0; y < QR_V2_SIZE; y++) {
            col[y] = m[y][x];
        }
        penalty += run_penalty(col, QR_V2_SIZE);
    }
    for (int y = 0; y < QR_V2_SIZE - 1; y++) {
        for (int x = 0; x < QR_V2_SIZE - 1; x++) {
            if (m[y][x] == m[y][x + 1] && m[y][x] == m[y + 1][x] &&
                m[y][x] == m[y + 1][x + 1]) {
                penalty += 3;
            }
        }
    }
    for (int y = 0; y < QR_V2_SIZE; y++) {
        for (int x = 0; x <= QR_V2_SIZE - 11; x++) {
            bool same = true;
            bool same_rev = true;
            for (int i = 0; i < 11; i++) {
                same &= m[y][x + i] == pattern[i];
                same_rev &= m[y][x + i] == pattern[10 - i];
            }
            if (same || same_rev) {
                penalty += 40;
            }
        }
    }
    for (int x = 0; x < QR_V2_SIZE; x++) {
        for (int y = 0; y <= QR_V2_SIZE - 11; y++) {
            bool same = true;
            bool same_rev = true;
            for (int i = 0; i < 11; i++) {
                same &= m[y + i][x] == pattern[i];
                same_rev &= m[y + i][x] == pattern[10 - i];
            }
            if (same || same_rev) {
                penalty += 40;
            }
        }
    }

    int percent = dark * 100 / (QR_V2_SIZE * QR_V2_SIZE);
    int balance = percent > 50 ? percent - 50 : 50 - percent;
    penalty += (balance / 5) * 10;
    return penalty;
}

esp_err_t qrcode_v2_make(const char *text, bool modules[QR_V2_SIZE][QR_V2_SIZE])
{
    if (text == NULL || modules == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(text);
    if (len > QR_V2_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < len; i++) {
        if (alnum_value(text[i]) < 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    gf_init();

    uint8_t data[QR_DATA_CODEWORDS] = {0};
    int bit_len = 0;
#define PUT_BITS(value, count) do { \
        uint32_t _v = (uint32_t)(value); \
        for (int _i = (count) - 1; _i >= 0; _i--) { \
            if ((_v >> _i) & 1U) { \
                data[bit_len / 8] |= (uint8_t)(0x80U >> (bit_len % 8)); \
            } \
            bit_len++; \
        } \
    } while (0)

    PUT_BITS(0x2, 4);
    PUT_BITS(len, 9);
    for (size_t i = 0; i < len; i += 2) {
        int first = alnum_value(text[i]);
        if (i + 1 < len) {
            int second = alnum_value(text[i + 1]);
            PUT_BITS(first * 45 + second, 11);
        } else {
            PUT_BITS(first, 6);
        }
    }
    int remaining = QR_DATA_CODEWORDS * 8 - bit_len;
    PUT_BITS(0, remaining < 4 ? remaining : 4);
    while ((bit_len & 7) != 0) {
        PUT_BITS(0, 1);
    }
#undef PUT_BITS

    static const uint8_t pads[2] = {0xEC, 0x11};
    for (int i = bit_len / 8, p = 0; i < QR_DATA_CODEWORDS; i++, p++) {
        data[i] = pads[p & 1];
    }

    uint8_t ecc[QR_ECC_CODEWORDS];
    rs_ecc(data, ecc);
    uint8_t codewords[QR_TOTAL_CODEWORDS];
    memcpy(codewords, data, QR_DATA_CODEWORDS);
    memcpy(codewords + QR_DATA_CODEWORDS, ecc, QR_ECC_CODEWORDS);

    memset(s_base, 0, sizeof(s_base));
    memset(s_reserved, 0, sizeof(s_reserved));
    finder(s_base, s_reserved, 0, 0);
    finder(s_base, s_reserved, QR_V2_SIZE - 7, 0);
    finder(s_base, s_reserved, 0, QR_V2_SIZE - 7);
    for (int i = 8; i < QR_V2_SIZE - 8; i++) {
        set_module(s_base, s_reserved, i, 6, (i & 1) == 0);
        set_module(s_base, s_reserved, 6, i, (i & 1) == 0);
    }
    set_module(s_base, s_reserved, 8, QR_V2_SIZE - 8, true);
    for (int i = 0; i < 9; i++) {
        if (i != 6) {
            s_reserved[8][i] = true;
            s_reserved[i][8] = true;
        }
    }
    for (int i = 0; i < 8; i++) {
        s_reserved[QR_V2_SIZE - 1 - i][8] = true;
        s_reserved[8][QR_V2_SIZE - 1 - i] = true;
    }

    int bit = 0;
    bool upward = true;
    for (int x = QR_V2_SIZE - 1; x > 0; x -= 2) {
        if (x == 6) {
            x--;
        }
        for (int yi = 0; yi < QR_V2_SIZE; yi++) {
            int y = upward ? (QR_V2_SIZE - 1 - yi) : yi;
            for (int dx = 0; dx < 2; dx++) {
                int xx = x - dx;
                if (!s_reserved[y][xx]) {
                    s_base[y][xx] = ((codewords[bit / 8] >> (7 - (bit % 8))) & 1U) != 0;
                    bit++;
                }
            }
        }
        upward = !upward;
    }

    int best_mask = 0;
    int best_penalty = 0x7FFFFFFF;
    for (int mask = 0; mask < 8; mask++) {
        memcpy(s_candidate, s_base, sizeof(s_candidate));
        for (int y = 0; y < QR_V2_SIZE; y++) {
            for (int x = 0; x < QR_V2_SIZE; x++) {
                if (!s_reserved[y][x] && mask_on(mask, x, y)) {
                    s_candidate[y][x] = !s_candidate[y][x];
                }
            }
        }
        place_format(s_candidate, mask);
        int score = penalty_score(s_candidate);
        if (score < best_penalty) {
            best_penalty = score;
            best_mask = mask;
            memcpy(modules, s_candidate, sizeof(s_candidate));
        }
    }

    (void)best_mask;
    return ESP_OK;
}
