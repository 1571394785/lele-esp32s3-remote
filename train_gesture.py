#!/usr/bin/env python3
"""
Gesture Recognition Training
Load CSV data from data/1..data/9, extract features,
train nearest-centroid classifier, export C header for ESP32.

Usage:
    python train_gesture.py [--data-dir data] [--output gesture_model.h]
"""

import argparse
import csv
import sys
from pathlib import Path
import numpy as np


def load_csv(path):
    """Load one CSV, return (N, 6) float32 array."""
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            try:
                rows.append([float(row[c]) for c in
                             ("acc_x", "acc_y", "acc_z",
                              "gyr_x", "gyr_y", "gyr_z")])
            except (ValueError, KeyError):
                continue
    return np.array(rows, dtype=np.float32) if rows else None


def extract_features(sig):
    """Per channel: mean, std, min, max, range, median -> 36 features."""
    if sig.shape[0] < 10:
        return None
    feats = []
    for ch in range(6):
        x = sig[:, ch]
        feats.extend([np.mean(x), np.std(x), np.min(x), np.max(x),
                      np.ptp(x), np.median(x)])
    return np.array(feats, dtype=np.float32)


def load_dataset(data_dir):
    Xs, ys = [], []
    dp = Path(data_dir)
    for cid in range(1, 10):
        cd = dp / str(cid)
        if not cd.is_dir():
            print(f"  [skip] {cd}")
            continue
        files = sorted(cd.glob("*.csv"))
        print(f"  class {cid}: {len(files)} files")
        for f in files:
            sig = load_csv(f)
            if sig is None or sig.shape[0] < 20:
                continue
            feat = extract_features(sig)
            if feat is not None:
                Xs.append(feat)
                ys.append(cid)
    if not Xs:
        print("ERROR: No data. Record gestures first!")
        sys.exit(1)
    return np.array(Xs), np.array(ys, dtype=np.int32)


def compute_centroids(X, y):
    return {int(c): X[y == c].mean(axis=0) for c in np.unique(y)}


def evaluate(centroids, X, y):
    ok = 0
    for xi, yi in zip(X, y):
        best = min(centroids.keys(), key=lambda c: np.linalg.norm(xi - centroids[c]))
        if best == yi:
            ok += 1
    return ok / len(y)


def export_c(centroids, feat_mean, feat_std, nf, path):
    nc = len(centroids)
    cls = sorted(centroids.keys())

    cent_lines = []
    for c in cls:
        v = ", ".join(f"{x:.6f}f" for x in centroids[c])
        cent_lines.append(f"    /* {c} */ {{{v}}},")
    cent_block = "\n".join(cent_lines)

    mean_str = ", ".join(f"{x:.6f}f" for x in feat_mean)
    std_str  = ", ".join(f"{x:.6f}f" for x in feat_std)
    label_str = ", ".join(str(c) for c in cls)

    content = f"""\
/* Auto-generated gesture model -- do not edit */
#pragma once
#include <stdint.h>
#include <math.h>

#define GESTURE_NUM_CLASSES   {nc}
#define GESTURE_NUM_FEATURES  {nf}
#define GESTURE_WINDOW_SIZE   200
#define GESTURE_SAMPLE_RATE   100

static const float gesture_norm_mean[GESTURE_NUM_FEATURES] = {{
    {mean_str}
}};
static const float gesture_norm_std[GESTURE_NUM_FEATURES] = {{
    {std_str}
}};
static const float gesture_centroids[GESTURE_NUM_CLASSES][GESTURE_NUM_FEATURES] = {{
{cent_block}
}};
static const int gesture_labels[GESTURE_NUM_CLASSES] = {{{label_str}}};

/* --- feature extraction (insertion-sort median, no malloc) --- */
static inline void gesture_extract_features(
    const float w[GESTURE_WINDOW_SIZE][6], int n,
    float feat[GESTURE_NUM_FEATURES])
{{
    if (n > GESTURE_WINDOW_SIZE) n = GESTURE_WINDOW_SIZE;
    if (n < 10) return;
    int f = 0;
    for (int ch = 0; ch < 6; ch++) {{
        float sum = 0;
        for (int i = 0; i < n; i++) sum += w[i][ch];
        float mean = sum / n;
        feat[f++] = mean;

        float ss = 0;
        for (int i = 0; i < n; i++) {{ float d = w[i][ch]-mean; ss += d*d; }}
        feat[f++] = sqrtf(ss / n);

        float mn = w[0][ch], mx = w[0][ch];
        for (int i = 1; i < n; i++) {{
            if (w[i][ch] < mn) mn = w[i][ch];
            if (w[i][ch] > mx) mx = w[i][ch];
        }}
        feat[f++] = mn;
        feat[f++] = mx;
        feat[f++] = mx - mn;

        float tmp[GESTURE_WINDOW_SIZE];
        for (int i = 0; i < n; i++) tmp[i] = w[i][ch];
        for (int i = 1; i < n; i++) {{
            float key = tmp[i]; int j = i-1;
            while (j >= 0 && tmp[j] > key) {{ tmp[j+1] = tmp[j]; j--; }}
            tmp[j+1] = key;
        }}
        feat[f++] = (n%2==0) ? (tmp[n/2-1]+tmp[n/2])*0.5f : tmp[n/2];
    }}
}}

static inline void gesture_normalize(float feat[GESTURE_NUM_FEATURES])
{{
    for (int i = 0; i < GESTURE_NUM_FEATURES; i++)
        feat[i] = (feat[i] - gesture_norm_mean[i]) / gesture_norm_std[i];
}}

static inline int gesture_classify(
    const float feat[GESTURE_NUM_FEATURES], float *out_dist)
{{
    int best_cls = 0; float best_d = 1e30f;
    for (int c = 0; c < GESTURE_NUM_CLASSES; c++) {{
        float d = 0;
        for (int f = 0; f < GESTURE_NUM_FEATURES; f++) {{
            float t = feat[f] - gesture_centroids[c][f];
            d += t * t;
        }}
        d = sqrtf(d);
        if (d < best_d) {{ best_d = d; best_cls = gesture_labels[c]; }}
    }}
    if (out_dist) *out_dist = best_d;
    return best_cls;
}}

static inline int gesture_predict(
    const float w[GESTURE_WINDOW_SIZE][6], int n, float *out_dist)
{{
    float feat[GESTURE_NUM_FEATURES];
    gesture_extract_features(w, n, feat);
    gesture_normalize(feat);
    return gesture_classify(feat, out_dist);
}}
"""
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"C header -> {path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", default="data")
    ap.add_argument("--output", default="gesture_model.h")
    args = ap.parse_args()

    print("=== Gesture Training ===\n")
    X, y = load_dataset(args.data_dir)
    print(f"\n{len(X)} samples, {X.shape[1]} features, classes {sorted(np.unique(y))}")

    mu = X.mean(axis=0)
    sd = X.std(axis=0)
    sd[sd < 1e-6] = 1.0
    Xn = (X - mu) / sd

    cent = compute_centroids(Xn, y)
    acc = evaluate(cent, Xn, y)
    print(f"Accuracy: {acc*100:.1f}% ({int(acc*len(y))}/{len(y)})")
    for c in sorted(cent):
        print(f"  class {c}: {int(np.sum(y==c))} samples")

    export_c(cent, mu, sd, X.shape[1], args.output)

    # also save .npz for Python-side inference
    npz_path = args.output.replace(".h", ".npz")
    cent_arr = np.array([cent[c] for c in sorted(cent)])
    labels_arr = np.array(sorted(cent.keys()), dtype=np.int32)
    np.savez(npz_path, norm_mean=mu, norm_std=sd,
             centroids=cent_arr, labels=labels_arr)
    print(f"NPZ model -> {npz_path}")
    print("Done!")


if __name__ == "__main__":
    main()
