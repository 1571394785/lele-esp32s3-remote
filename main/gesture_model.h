/* Auto-generated from gesture_model.npz -- do not edit by hand */
#pragma once

#include <math.h>
#include <stdint.h>

#define GESTURE_NUM_CLASSES   4
#define GESTURE_NUM_FEATURES  36
#define GESTURE_WINDOW_SIZE   200
#define GESTURE_SAMPLE_RATE   100

static const float gesture_norm_mean[GESTURE_NUM_FEATURES] = {
    -376.938538f, 693.338379f, -1657.361328f, 2020.310913f, 3677.672363f, -449.096649f, 160.457184f, 466.030731f, -1417.579834f, 1581.210083f, 2998.789795f, 185.689072f, 1638.500977f, 1367.698853f, -2823.218506f, 5790.218262f, 8613.436523f, 1861.567261f, 217.350189f, 1449.503174f, -4333.613281f, 5186.748047f, 9520.361328f, 68.563026f, 38.009098f, 5708.753906f, -11880.487305f, 19503.369141f, 31383.857422f, -32.138657f, 108.374664f, 1855.273804f, -5953.882324f, 4466.966309f, 10420.848633f, 51.470589f
};

static const float gesture_norm_std[GESTURE_NUM_FEATURES] = {
    443.406433f, 279.639282f, 367.098236f, 1366.015503f, 1445.009399f, 589.480408f, 220.817871f, 268.509399f, 662.569275f, 861.439331f, 1375.537109f, 226.478897f, 263.411774f, 414.457184f, 1950.668945f, 1926.792847f, 3416.244385f, 347.171814f, 718.590637f, 647.627075f, 2278.930176f, 2912.329346f, 3481.908691f, 316.597870f, 167.613144f, 1200.939209f, 3793.811035f, 6828.225098f, 7800.613281f, 104.494987f, 346.949554f, 1370.297974f, 2966.219238f, 3154.510254f, 5507.767090f, 82.969482f
};

static const float gesture_centroids[GESTURE_NUM_CLASSES][GESTURE_NUM_FEATURES] = {
    /* 1 */ {0.199925f, -0.114149f, -0.311350f, 0.266090f, 0.330641f, 0.368099f, -0.058118f, -0.342596f, 0.066353f, -0.124932f, -0.110200f, -0.004464f, 0.130929f, 0.666613f, -0.640617f, 0.374951f, 0.577267f, 0.382961f, -0.314608f, -0.314817f, -0.254080f, -0.119175f, 0.066617f, -0.204057f, -0.120739f, 0.523467f, 0.195483f, 0.688482f, 0.507586f, -0.189545f, -0.376531f, -0.392964f, 0.073076f, -0.438544f, -0.290526f, -0.103331f},
    /* 2 */ {-0.791426f, 1.296176f, 0.119234f, 0.074475f, 0.040113f, -1.423393f, -0.811483f, -0.756297f, 0.561640f, -1.000736f, -0.897249f, -0.809560f, -0.354636f, -1.299590f, 1.183878f, -0.028186f, -0.691890f, -1.266792f, -0.264487f, -0.500258f, 0.007290f, -0.485092f, -0.410511f, -0.130993f, 0.119164f, -0.634682f, -1.188003f, -1.007217f, -0.303880f, 0.110075f, -0.173008f, -0.710515f, 1.057719f, -0.447199f, -0.825765f, -0.082918f},
    /* 3 */ {-0.050403f, -0.897949f, 0.770261f, -0.936198f, -1.080701f, 0.026925f, 1.213860f, 1.865292f, -0.738368f, 1.469157f, 1.275727f, 1.072113f, -0.296891f, -0.676802f, 0.749778f, -1.060061f, -1.026006f, -0.118939f, 1.950622f, 1.561483f, 1.052035f, 1.230219f, 0.340413f, 1.552875f, 0.591075f, -0.865654f, 0.396419f, -1.068055f, -1.127714f, 0.561880f, 1.765881f, 1.833887f, -1.259201f, 1.745432f, 1.677822f, 0.699407f},
    /* 4 */ {0.956214f, -0.556013f, 1.175057f, -0.584774f, -0.851325f, 0.976235f, -0.545481f, 1.085546f, -0.608118f, 0.438266f, 0.567386f, -0.826519f, 1.045528f, -1.633953f, 1.431031f, -1.387522f, -1.599691f, 0.573009f, -2.556839f, 0.689420f, -0.663310f, -1.308145f, -0.660015f, -3.319868f, -1.407286f, -1.734261f, 0.984363f, -1.684826f, -1.953546f, 0.044391f, -1.006171f, 1.910002f, -0.855084f, 1.735859f, 1.454701f, -1.155191f},
};

static const int gesture_labels[GESTURE_NUM_CLASSES] = {1, 2, 3, 4};

static inline void gesture_extract_features(
    const float w[GESTURE_WINDOW_SIZE][6], int n,
    float feat[GESTURE_NUM_FEATURES])
{
    if (n > GESTURE_WINDOW_SIZE) n = GESTURE_WINDOW_SIZE;
    if (n < 10) return;
    int f = 0;
    for (int ch = 0; ch < 6; ch++) {
        float sum = 0;
        for (int i = 0; i < n; i++) sum += w[i][ch];
        float mean = sum / n;
        feat[f++] = mean;

        float ss = 0;
        for (int i = 0; i < n; i++) {
            float d = w[i][ch] - mean;
            ss += d * d;
        }
        feat[f++] = sqrtf(ss / n);

        float mn = w[0][ch], mx = w[0][ch];
        for (int i = 1; i < n; i++) {
            if (w[i][ch] < mn) mn = w[i][ch];
            if (w[i][ch] > mx) mx = w[i][ch];
        }
        feat[f++] = mn;
        feat[f++] = mx;
        feat[f++] = mx - mn;

        float tmp[GESTURE_WINDOW_SIZE];
        for (int i = 0; i < n; i++) tmp[i] = w[i][ch];
        for (int i = 1; i < n; i++) {
            float key = tmp[i];
            int j = i - 1;
            while (j >= 0 && tmp[j] > key) {
                tmp[j + 1] = tmp[j];
                j--;
            }
            tmp[j + 1] = key;
        }
        feat[f++] = (n % 2 == 0) ? (tmp[n / 2 - 1] + tmp[n / 2]) * 0.5f : tmp[n / 2];
    }
}

static inline void gesture_normalize(float feat[GESTURE_NUM_FEATURES])
{
    for (int i = 0; i < GESTURE_NUM_FEATURES; i++) {
        feat[i] = (feat[i] - gesture_norm_mean[i]) / gesture_norm_std[i];
    }
}

static inline int gesture_classify(
    const float feat[GESTURE_NUM_FEATURES], float *out_dist)
{
    int best_cls = 0;
    float best_d = 1e30f;
    for (int c = 0; c < GESTURE_NUM_CLASSES; c++) {
        float d = 0;
        for (int f = 0; f < GESTURE_NUM_FEATURES; f++) {
            float t = feat[f] - gesture_centroids[c][f];
            d += t * t;
        }
        d = sqrtf(d);
        if (d < best_d) {
            best_d = d;
            best_cls = gesture_labels[c];
        }
    }
    if (out_dist) *out_dist = best_d;
    return best_cls;
}

static inline int gesture_predict(
    const float w[GESTURE_WINDOW_SIZE][6], int n, float *out_dist)
{
    float feat[GESTURE_NUM_FEATURES] = {0};
    gesture_extract_features(w, n, feat);
    gesture_normalize(feat);
    return gesture_classify(feat, out_dist);
}
