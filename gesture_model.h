/* Auto-generated gesture model -- do not edit */
#pragma once
#include <stdint.h>
#include <math.h>

#define GESTURE_NUM_CLASSES   4
#define GESTURE_NUM_FEATURES  36
#define GESTURE_WINDOW_SIZE   200
#define GESTURE_SAMPLE_RATE   100

static const float gesture_norm_mean[GESTURE_NUM_FEATURES] = {
    -443.414978f, 635.855286f, -1665.735352f, 1752.794067f, 3418.529297f, -499.147064f, 211.046967f, 486.325226f, -1398.156860f, 1709.980347f, 3108.137207f, 231.779419f, 1627.715820f, 1349.561401f, -2714.715576f, 5639.745117f, 8354.460938f, 1852.936279f, 237.033707f, 1438.054810f, -4262.911621f, 4985.343262f, 9248.254883f, 77.490196f, 49.635159f, 5495.125488f, -11217.441406f, 18867.431641f, 30084.873047f, -25.171568f, 117.420387f, 1984.422241f, -6356.754883f, 4528.480469f, 10885.235352f, 55.671570f
};
static const float gesture_norm_std[GESTURE_NUM_FEATURES] = {
    411.948120f, 179.296646f, 354.202728f, 1099.284180f, 1217.135864f, 562.549438f, 189.368423f, 282.670258f, 680.298706f, 849.947144f, 1421.406738f, 206.806488f, 278.990662f, 424.300568f, 1984.730835f, 1990.035156f, 3504.045898f, 367.548584f, 770.036499f, 675.916199f, 2287.206055f, 2950.442383f, 3449.508301f, 337.532623f, 159.469406f, 1123.005493f, 2705.317383f, 6882.038574f, 7345.114746f, 102.873062f, 367.885498f, 1426.794312f, 2951.106201f, 3300.015869f, 5695.534180f, 87.040131f
};
static const float gesture_centroids[GESTURE_NUM_CLASSES][GESTURE_NUM_FEATURES] = {
    /* 1 */ {0.207667f, 0.039944f, -0.342822f, 0.462381f, 0.517376f, 0.331263f, -0.080837f, -0.375298f, 0.079945f, -0.153410f, -0.129996f, 0.006179f, 0.138643f, 0.690020f, -0.684978f, 0.411250f, 0.621539f, 0.382859f, -0.339540f, -0.360328f, -0.328043f, -0.204404f, 0.042679f, -0.205343f, -0.127052f, 0.686722f, 0.113510f, 0.735392f, 0.647221f, -0.218881f, -0.425163f, -0.416782f, 0.044418f, -0.480804f, -0.301595f, -0.082959f},
    /* 2 */ {-1.151323f, 1.255723f, -0.028980f, -0.463055f, -0.409785f, -1.588148f, -0.848358f, -0.879138f, 0.738833f, -1.091948f, -1.006556f, -0.875854f, -0.454536f, -1.382995f, 1.337155f, -0.058813f, -0.790781f, -1.402122f, -0.325678f, -0.646547f, 0.143033f, -0.487720f, -0.511997f, -0.206748f, 0.175960f, -1.207209f, -1.040119f, -1.129174f, -0.674893f, 0.198941f, -0.175708f, -0.880399f, 1.339116f, -0.601783f, -1.042530f, -0.159437f},
    /* 3 */ {0.107119f, -1.079879f, 0.821946f, -0.920002f, -1.070118f, 0.117185f, 1.148303f, 1.700052f, -0.747676f, 1.337518f, 1.157630f, 0.951230f, -0.241655f, -0.618354f, 0.682241f, -0.950760f, -0.926389f, -0.088862f, 1.794740f, 1.513067f, 1.017316f, 1.282590f, 0.422494f, 1.430113f, 0.548355f, -0.735499f, 0.310829f, -0.967298f, -1.020797f, 0.503014f, 1.640798f, 1.670755f, -1.129134f, 1.649831f, 1.540973f, 0.618432f},
    /* 4 */ {1.190606f, -0.546580f, 1.241479f, -0.483309f, -0.797799f, 1.111942f, -0.903223f, 0.959368f, -0.620820f, 0.292688f, 0.472147f, -1.128008f, 1.025803f, -1.553300f, 1.351803f, -1.267814f, -1.485700f, 0.564725f, -2.411579f, 0.677503f, -0.691822f, -1.222984f, -0.587332f, -3.140408f, -1.552057f, -1.664385f, 1.135335f, -1.579246f, -1.897843f, -0.022634f, -0.973500f, 1.743856f, -0.722948f, 1.640680f, 1.325208f, -1.149430f},
};
static const int gesture_labels[GESTURE_NUM_CLASSES] = {1, 2, 3, 4};

/* --- feature extraction (insertion-sort median, no malloc) --- */
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
        for (int i = 0; i < n; i++) { float d = w[i][ch]-mean; ss += d*d; }
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
            float key = tmp[i]; int j = i-1;
            while (j >= 0 && tmp[j] > key) { tmp[j+1] = tmp[j]; j--; }
            tmp[j+1] = key;
        }
        feat[f++] = (n%2==0) ? (tmp[n/2-1]+tmp[n/2])*0.5f : tmp[n/2];
    }
}

static inline void gesture_normalize(float feat[GESTURE_NUM_FEATURES])
{
    for (int i = 0; i < GESTURE_NUM_FEATURES; i++)
        feat[i] = (feat[i] - gesture_norm_mean[i]) / gesture_norm_std[i];
}

static inline int gesture_classify(
    const float feat[GESTURE_NUM_FEATURES], float *out_dist)
{
    int best_cls = 0; float best_d = 1e30f;
    for (int c = 0; c < GESTURE_NUM_CLASSES; c++) {
        float d = 0;
        for (int f = 0; f < GESTURE_NUM_FEATURES; f++) {
            float t = feat[f] - gesture_centroids[c][f];
            d += t * t;
        }
        d = sqrtf(d);
        if (d < best_d) { best_d = d; best_cls = gesture_labels[c]; }
    }
    if (out_dist) *out_dist = best_d;
    return best_cls;
}

static inline int gesture_predict(
    const float w[GESTURE_WINDOW_SIZE][6], int n, float *out_dist)
{
    float feat[GESTURE_NUM_FEATURES];
    gesture_extract_features(w, n, feat);
    gesture_normalize(feat);
    return gesture_classify(feat, out_dist);
}
