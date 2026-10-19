#!/usr/bin/env python3
"""
IMU 3D Attitude Visualizer  (pygame + OpenGL)
Read CSV data from ESP32 serial and render a real-time 3D orientation.
GPU-accelerated rendering via pygame / PyOpenGL.

CSV per line:
  timestamp_ms,acc_x,acc_y,acc_z,gyr_x,gyr_y,gyr_z,label
"""

import argparse
import csv
import math
import os
import sys
import threading
import time
from collections import deque
from typing import Optional

import numpy as np


# ── Gesture classifier (loads .npz from train_gesture.py) ─────────────────

class GestureClassifier:
    """Lightweight nearest-centroid classifier for real-time gesture recognition."""

    def __init__(self, model_path: str):
        self.ready = False
        if not os.path.isfile(model_path):
            print(f"[gesture] Model not found: {model_path}  (run train_gesture.py first)")
            return
        data = np.load(model_path)
        self.norm_mean = data["norm_mean"]   # (36,)
        self.norm_std = data["norm_std"]     # (36,)
        self.centroids = data["centroids"]   # (C, 36)
        self.labels = data["labels"]         # (C,) int
        self.num_classes = len(self.labels)
        self.ready = True
        print(f"[gesture] Loaded model: {self.num_classes} classes, {self.centroids.shape[1]} features")

    @staticmethod
    def _extract_features(sig: np.ndarray):
        """sig: (N, 6) float array. Returns (36,) feature vector."""
        n = sig.shape[0]
        if n < 10:
            return None
        feats = []
        for ch in range(6):
            x = sig[:, ch]
            feats.extend([x.mean(), x.std(), x.min(), x.max(), float(np.ptp(x)), np.median(x)])
        return np.array(feats, dtype=np.float32)

    def retrain(self, data_dir: str, model_path: str):
        """Retrain on all CSVs in data_dir, save .npz, reload into self."""
        import csv as _csv
        from pathlib import Path

        def _load_csv(path):
            rows = []
            with open(path, "r", encoding="utf-8") as f:
                for row in _csv.DictReader(f):
                    try:
                        rows.append([float(row[c]) for c in
                                     ("acc_x","acc_y","acc_z","gyr_x","gyr_y","gyr_z")])
                    except (ValueError, KeyError):
                        continue
            return np.array(rows, dtype=np.float32) if rows else None

        Xs, ys = [], []
        dp = Path(data_dir)
        for cid in range(1, 10):
            cd = dp / str(cid)
            if not cd.is_dir():
                continue
            for f in sorted(cd.glob("*.csv")):
                sig = _load_csv(f)
                if sig is None or sig.shape[0] < 20:
                    continue
                feat = self._extract_features(sig)
                if feat is not None:
                    Xs.append(feat)
                    ys.append(cid)
        if not Xs:
            return "No data to train"
        X = np.array(Xs)
        y = np.array(ys, dtype=np.int32)
        mu = X.mean(axis=0)
        sd = X.std(axis=0); sd[sd < 1e-6] = 1.0
        Xn = (X - mu) / sd
        classes = np.unique(y)
        cent = {int(c): Xn[y == c].mean(axis=0) for c in classes}
        # save
        cent_arr = np.array([cent[c] for c in sorted(cent)])
        labels_arr = np.array(sorted(cent.keys()), dtype=np.int32)
        np.savez(model_path, norm_mean=mu, norm_std=sd,
                 centroids=cent_arr, labels=labels_arr)
        # reload
        self.norm_mean = mu
        self.norm_std = sd
        self.centroids = cent_arr
        self.labels = labels_arr
        self.num_classes = len(labels_arr)
        self.ready = True
        return f"Trained {len(y)} samples, {self.num_classes} classes"

    def predict(self, window: np.ndarray):
        """
        window: (N, 6) raw IMU data.
        Returns: list of (class_id, probability) sorted by probability desc,
                 or empty list if model not ready.
        """
        if not self.ready:
            return []
        feat = self._extract_features(window)
        if feat is None:
            return []
        # normalize
        feat = (feat - self.norm_mean) / self.norm_std
        # distances to each centroid
        dists = np.linalg.norm(self.centroids - feat, axis=1)
        # convert distances to pseudo-probabilities (softmax with temperature)
        # lower distance = higher probability
        temp = 2.0  # temperature: lower = sharper
        neg_dists = -dists / temp
        neg_dists -= neg_dists.max()  # numerical stability
        exp_d = np.exp(neg_dists)
        probs = exp_d / exp_d.sum()
        # sort by probability descending
        order = np.argsort(-probs)
        return [(int(self.labels[i]), float(probs[i])) for i in order]


import serial
import serial.tools.list_ports

import pygame
from pygame.locals import *
from OpenGL.GL import *
from OpenGL.GLU import *


# ── Serial helpers ──────────────────────────────────────────────────────────

def list_serial_ports():
    return [(p.device, p.description) for p in serial.tools.list_ports.comports()]


def read_serial_line(ser: serial.Serial, writer: Optional[csv.writer] = None):
    """Read one CSV line from serial, return dict or None."""
    try:
        raw = ser.readline()
        if not raw:
            return None
        line = raw.decode("utf-8", errors="ignore").strip()
        if not line:
            return None
        if line.startswith("timestamp") or line.startswith("acc"):
            return None
        parts = line.split(",")
        if len(parts) < 8:
            return None
        data = {
            "ts":    int(parts[0]),
            "ax":    float(parts[1]),
            "ay":    float(parts[2]),
            "az":    float(parts[3]),
            "gx":    float(parts[4]),
            "gy":    float(parts[5]),
            "gz":    float(parts[6]),
            "label": parts[7].strip(),
        }
        if writer:
            writer.writerow([data[k] for k in data])
        return data
    except (ValueError, IndexError, UnicodeDecodeError):
        return None


# ── Serial reader thread ────────────────────────────────────────────────────

class SerialReader:
    """Reads serial data in a background thread, keeping the latest sample
    and a ring buffer for recording."""

    def __init__(self, ser, writer=None, buf_seconds: int = 30):
        self.ser = ser
        self.writer = writer
        self.data = None
        self.lock = threading.Lock()
        self.running = True
        # ring buffer: ~100 samples/sec * buf_seconds
        self._buf = deque(maxlen=buf_seconds * 150)
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def _loop(self):
        while self.running:
            d = read_serial_line(self.ser, self.writer)
            if d is not None:
                with self.lock:
                    self.data = d
                    self._buf.append(d)
            else:
                time.sleep(0.001)

    def latest(self):
        with self.lock:
            return self.data

    def take_buffer(self, n: int):
        """Take the last n samples from the ring buffer."""
        with self.lock:
            buf = list(self._buf)
        return buf[-n:] if n < len(buf) else buf

    def clear_buffer(self):
        with self.lock:
            self._buf.clear()

    def stop(self):
        self.running = False


# ── Complementary filter ────────────────────────────────────────────────────

class AttitudeFilter:
    def __init__(self, alpha: float = 0.98, gyro_scale: float = 0.02628):
        self.alpha = alpha
        self.gyro_scale = gyro_scale  # LSB -> dps conversion factor
        self.roll = 0.0
        self.pitch = 0.0
        self.yaw = 0.0
        self._last_ts: Optional[float] = None

    def update(self, ax, ay, az, gx, gy, gz, ts_ms):
        dt = 0.01
        if self._last_ts is not None:
            dt = max(0.001, min(0.1, (ts_ms - self._last_ts) / 1000.0))
        self._last_ts = ts_ms

        acc_roll = math.atan2(ay, az)
        acc_pitch = math.atan2(-ax, math.sqrt(ay * ay + az * az))

        # raw LSB -> dps -> rad/s
        gx_r = math.radians(gx * self.gyro_scale)
        gy_r = math.radians(gy * self.gyro_scale)
        gz_r = math.radians(gz * self.gyro_scale)

        self.roll  = self.alpha * (self.roll  + gx_r * dt) + (1 - self.alpha) * acc_roll
        self.pitch = self.alpha * (self.pitch + gy_r * dt) + (1 - self.alpha) * acc_pitch
        self.yaw  += gz_r * dt
        return self.roll, self.pitch, self.yaw


# ── OpenGL drawing helpers ──────────────────────────────────────────────────

_CUBE_VERTS = np.array([
    [-1, -1, -1], [ 1, -1, -1], [ 1,  1, -1], [-1,  1, -1],
    [-1, -1,  1], [ 1, -1,  1], [ 1,  1,  1], [-1,  1,  1],
], dtype=float) * 0.45

# 6 faces, each 4 vertex indices  (outward normal order)
_CUBE_FACES = [
    (1, 2, 6, 5),   # +X  red
    (0, 4, 7, 3),   # -X  green
    (2, 3, 7, 6),   # +Y  blue
    (0, 1, 5, 4),   # -Y  yellow
    (4, 5, 6, 7),   # +Z  magenta
    (0, 3, 2, 1),   # -Z  cyan
]
_FACE_COLORS = [
    (0.90, 0.20, 0.20, 0.55),
    (0.20, 0.80, 0.20, 0.55),
    (0.20, 0.20, 0.90, 0.55),
    (0.90, 0.90, 0.20, 0.55),
    (0.80, 0.40, 0.80, 0.55),
    (0.40, 0.80, 0.80, 0.55),
]

_EDGE_COLOR = (1.0, 1.0, 1.0, 0.9)
_AXIS_LEN = 0.80


def _draw_cube(r, p, y):
    """Draw the colored cube rotated by (roll, pitch, yaw)."""
    glPushMatrix()
    # OpenGL rotation order: Y * X * Z  (matches our ZYX Euler)
    glRotatef(math.degrees(y), 0, 0, 1)
    glRotatef(math.degrees(p), 0, 1, 0)
    glRotatef(math.degrees(r), 1, 0, 0)

    # faces
    glBegin(GL_QUADS)
    for fi, color in zip(_CUBE_FACES, _FACE_COLORS):
        glColor4f(*color)
        for vi in fi:
            glVertex3f(*_CUBE_VERTS[vi])
    glEnd()

    # edges
    glColor4f(*_EDGE_COLOR)
    glLineWidth(2.0)
    edges = [(0,1),(1,2),(2,3),(3,0),(4,5),(5,6),(6,7),(7,4),
             (0,4),(1,5),(2,6),(3,7)]
    glBegin(GL_LINES)
    for i, j in edges:
        glVertex3f(*_CUBE_VERTS[i])
        glVertex3f(*_CUBE_VERTS[j])
    glEnd()

    glPopMatrix()


def _draw_axes(r, p, y):
    """Draw XYZ axis arrows rotated by the current attitude."""
    glPushMatrix()
    glRotatef(math.degrees(y), 0, 0, 1)
    glRotatef(math.degrees(p), 0, 1, 0)
    glRotatef(math.degrees(r), 1, 0, 0)

    glLineWidth(3.0)
    glBegin(GL_LINES)
    # X - red
    glColor3f(1, 0, 0); glVertex3f(0, 0, 0); glVertex3f(_AXIS_LEN, 0, 0)
    # Y - green
    glColor3f(0, 1, 0); glVertex3f(0, 0, 0); glVertex3f(0, _AXIS_LEN, 0)
    # Z - blue
    glColor3f(0, 0, 1); glVertex3f(0, 0, 0); glVertex3f(0, 0, _AXIS_LEN)
    glEnd()

    # tips
    glPointSize(8)
    glBegin(GL_POINTS)
    glColor3f(1, 0, 0); glVertex3f(_AXIS_LEN, 0, 0)
    glColor3f(0, 1, 0); glVertex3f(0, _AXIS_LEN, 0)
    glColor3f(0, 0, 1); glVertex3f(0, 0, _AXIS_LEN)
    glEnd()

    glPopMatrix()


def _draw_grid():
    """Draw a faint reference grid on the XZ plane."""
    glColor4f(0.3, 0.3, 0.3, 0.4)
    glLineWidth(1.0)
    glBegin(GL_LINES)
    for i in range(-5, 6):
        f = i * 0.2
        glVertex3f(f, -1.0, -1.0); glVertex3f(f, -1.0, 1.0)
        glVertex3f(-1.0, -1.0, f); glVertex3f(1.0, -1.0, f)
    glEnd()


# ── HUD text (bitmap) ──────────────────────────────────────────────────────

def _draw_text(surface, text, x, y, font, color=(0, 255, 0)):
    """Render text as a pygame surface, then blit it onto the OpenGL framebuffer."""
    img = font.render(text, True, color, (0, 0, 0))
    img_data = pygame.image.tostring(img, "RGBA", True)
    w, h = img.get_size()
    glWindowPos2d(x, y)
    glDrawPixels(w, h, GL_RGBA, GL_UNSIGNED_BYTE, img_data)


# ── CLI ────────────────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(description="ESP32 IMU 3D Attitude Visualizer")
    p.add_argument("-p", "--port", help="Serial port (e.g. COM3)",default="COM6")
    p.add_argument("-b", "--baud", type=int, default=115200, help="Baud rate")
    p.add_argument("-o", "--output", help="CSV output file (optional)")
    p.add_argument("-l", "--label", default="default", help="Label tag for file")
    p.add_argument("--alpha", type=float, default=0.90,
                   help="Filter alpha (0=accel only, 1=gyro only, default 0.90)")
    p.add_argument("--gyro-scale", type=float, default=0.02628,
                   help="Gyro LSB->dps scale (BMI220 ±1000dps=0.02628)")
    p.add_argument("--model", default="gesture_model.npz",
                   help="Gesture model .npz file (default: gesture_model.npz)")
    p.add_argument("--window", type=int, default=200,
                   help="Classification window in samples (default: 200 = 2s)")
    p.add_argument("--list", action="store_true", help="List serial ports")
    return p.parse_args()


# ── Main ───────────────────────────────────────────────────────────────────

def main():
    args = parse_args()

    if args.list:
        for dev, desc in list_serial_ports():
            print(f"  {dev}  {desc}")
        return

    if not args.port:
        print("Error: --port required.  Use --list to see available ports.")
        return

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"Cannot open {args.port}: {e}")
        return

    csv_file = None
    writer = None
    if args.output:
        csv_file = open(args.output, "w", newline="", encoding="utf-8")
        writer = csv.writer(csv_file)
        writer.writerow(["timestamp_ms", "acc_x", "acc_y", "acc_z",
                         "gyr_x", "gyr_y", "gyr_z", "label"])

    reader = SerialReader(ser, writer)
    att = AttitudeFilter(alpha=args.alpha, gyro_scale=args.gyro_scale)
    classifier = GestureClassifier(args.model)

    # ── gesture sliding window ──
    gesture_window = deque(maxlen=args.window)  # each item: [ax,ay,az,gx,gy,gz]
    gesture_results = []  # list of (class_id, probability)
    gesture_msg_ts = 0.0
    gesture_confirmed_cls = 0    # last confirmed class (>90%)
    gesture_confirmed_prob = 0.0

    # ── recording state ──
    RECORD_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
    RECORD_DURATION_S = 2
    # numpad key -> label number: K_KP1..K_KP9 -> "1".."9"
    _NP_KEY_MAP = {
        K_KP1: "1", K_KP2: "2", K_KP3: "3",
        K_KP4: "4", K_KP5: "5", K_KP6: "6",
        K_KP7: "7", K_KP8: "8", K_KP9: "9",
        # also support top-row number keys as fallback
        K_1: "1", K_2: "2", K_3: "3",
        K_4: "4", K_5: "5", K_6: "6",
        K_7: "7", K_8: "8", K_9: "9",
    }
    recording_key = None     # which label is being recorded ("1".."9")
    record_start_ts = 0      # time.time() when recording started
    record_count = 0         # samples collected this session
    record_msg = ""          # status message to show in HUD
    record_msg_ts = 0.0      # when the message was set (for auto-clear)

    # ── pygame / OpenGL init ──
    pygame.init()
    WIDTH, HEIGHT = 900, 700
    screen = pygame.display.set_mode((WIDTH, HEIGHT), DOUBLEBUF | OPENGL)
    pygame.display.set_caption(f"IMU 3D  |  {args.port}")
    clock = pygame.time.Clock()

    # OpenGL state
    glEnable(GL_DEPTH_TEST)
    glEnable(GL_BLEND)
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
    glEnable(GL_LINE_SMOOTH)
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST)
    glClearColor(0.06, 0.06, 0.08, 1.0)

    # projection
    glMatrixMode(GL_PROJECTION)
    glLoadIdentity()
    gluPerspective(45, WIDTH / HEIGHT, 0.1, 100.0)
    glTranslatef(0, 0, -4.0)
    glMatrixMode(GL_MODELVIEW)

    font = pygame.font.SysFont("consolas", 16)

    label_text = args.label
    running = True

    while running:
        for event in pygame.event.get():
            if event.type == QUIT:
                running = False
            elif event.type == KEYDOWN:
                if event.key == K_ESCAPE:
                    running = False
                elif event.key in _NP_KEY_MAP and recording_key is None:
                    # start recording
                    label = _NP_KEY_MAP[event.key]
                    recording_key = label
                    record_start_ts = time.time()
                    record_count = 0
                    reader.clear_buffer()
                    record_msg = f"REC {label} ... 0.0s"
                    record_msg_ts = time.time()
                    # create folder
                    folder = os.path.join(RECORD_DIR, label)
                    os.makedirs(folder, exist_ok=True)
                elif event.key == K_t:
                    # retrain and reload model
                    record_msg = "Training..."
                    record_msg_ts = time.time()
                    result = classifier.retrain(RECORD_DIR, args.model)
                    record_msg = result
                    record_msg_ts = time.time()

        # read latest data
        data = reader.latest()

        # ── recording logic ──
        if recording_key is not None:
            elapsed = time.time() - record_start_ts
            if elapsed >= RECORD_DURATION_S:
                # finish recording: grab all buffered samples
                samples = reader.take_buffer(record_count + 200)  # extra margin
                folder = os.path.join(RECORD_DIR, recording_key)
                ts_tag = int(time.time())
                fpath = os.path.join(folder, f"{ts_tag}.csv")
                with open(fpath, "w", newline="", encoding="utf-8") as f:
                    w = csv.writer(f)
                    w.writerow(["timestamp_ms", "acc_x", "acc_y", "acc_z",
                                "gyr_x", "gyr_y", "gyr_z", "label"])
                    for s in samples:
                        w.writerow([s["ts"], s["ax"], s["ay"], s["az"],
                                    s["gx"], s["gy"], s["gz"], s["label"]])
                record_msg = f"SAVED {fpath}  ({len(samples)} samples)"
                record_msg_ts = time.time()
                recording_key = None
            else:
                record_count += 1
                record_msg = f"REC {recording_key} ... {elapsed:.1f}s / {RECORD_DURATION_S}s"
                record_msg_ts = time.time()

        if data is not None:
            r, p, y = att.update(data["ax"], data["ay"], data["az"],
                                  data["gx"], data["gy"], data["gz"], data["ts"])
            label_text = data["label"]
            cur_ts = data["ts"]
            # push to gesture window (deque auto-slides, always latest 200)
            gesture_window.append([data["ax"], data["ay"], data["az"],
                                   data["gx"], data["gy"], data["gz"]])
            # infer every frame when window is full
            if len(gesture_window) >= args.window and classifier.ready:
                win = np.array(gesture_window, dtype=np.float32)
                gesture_results = classifier.predict(win)
                gesture_msg_ts = time.time()
                if gesture_results and gesture_results[0][1] > 0.7:
                    gesture_confirmed_cls = gesture_results[0][0]
                    gesture_confirmed_prob = gesture_results[0][1]
                else:
                    gesture_confirmed_cls = 0
                    gesture_confirmed_prob = 0.0
        else:
            r, p, y = att.roll, att.pitch, att.yaw
            cur_ts = 0

        # ── render ──
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
        glLoadIdentity()
        glTranslatef(0, 0, -4.0)

        # slow camera orbit with mouse
        mx, my = pygame.mouse.get_pos()
        cam_yaw   = (mx - WIDTH / 2) * 0.3
        cam_pitch = (my - HEIGHT / 2) * 0.3
        glRotatef(cam_pitch, 1, 0, 0)
        glRotatef(cam_yaw,   0, 1, 0)

        _draw_grid()
        _draw_cube(r, p, y)
        _draw_axes(r, p, y)

        # ── HUD overlay (text in window coords) ──
        glDisable(GL_DEPTH_TEST)

        rd = math.degrees(r)
        pd = math.degrees(p)
        yd = math.degrees(y)

        # ── Left HUD: status ──
        hud_lines = [
            f"Port: {args.port} @ {args.baud}",
            f"Label: {label_text}   ts: {cur_ts} ms",
            f"Roll:  {rd:+8.2f} deg",
            f"Pitch: {pd:+8.2f} deg",
            f"Yaw:   {yd:+8.2f} deg",
            "",
            f"Window: {len(gesture_window)}/{args.window}",
            "Numpad 1-9 = record 2s",
            "T = retrain model",
            "ESC = quit",
        ]
        if record_msg and (time.time() - record_msg_ts < 5.0):
            hud_lines.append("")
            hud_lines.append(record_msg)

        yy = HEIGHT - 20
        for i, line in enumerate(hud_lines):
            is_rec_msg = (i == len(hud_lines) - 1 and record_msg
                          and (time.time() - record_msg_ts < 5.0))
            color = (255, 60, 60) if recording_key else (60, 255, 60) if is_rec_msg else (0, 255, 0)
            _draw_text(screen, line, 10, yy, font, color)
            yy -= 20

        # ── Right HUD: gesture probabilities ──
        if gesture_results and classifier.ready:
            rx = WIDTH - 240
            _draw_text(screen, "=== PREDICTION ===", rx, HEIGHT - 20, font, (255, 255, 0))
            ry = HEIGHT - 45
            for cls_id, prob in gesture_results[:min(9, len(gesture_results))]:
                bar_len = int(prob * 25)  # max 25 chars wide
                bar = "#" * bar_len + "-" * (25 - bar_len)
                pct = prob * 100
                # highlight: confirmed=green, top=yellow, else=gray
                if gesture_confirmed_cls == cls_id and gesture_confirmed_prob > 0.90:
                    color = (0, 255, 100)
                elif cls_id == gesture_results[0][0]:
                    color = (0, 200, 255)
                elif prob > 0.1:
                    color = (200, 200, 0)
                else:
                    color = (100, 100, 100)
                _draw_text(screen, f"{cls_id}: {bar} {pct:5.1f}%", rx, ry, font, color)
                ry -= 20
            # show confirmed result
            if gesture_confirmed_cls > 0:
                _draw_text(screen, f">> CONFIRMED: {gesture_confirmed_cls} "
                           f"({gesture_confirmed_prob*100:.1f}%)",
                           rx, ry - 5, font, (0, 255, 100))
            else:
                win_cls, win_prob = gesture_results[0]
                _draw_text(screen, f">> {win_cls} ({win_prob*100:.1f}%)",
                           rx, ry - 5, font, (255, 150, 0))

        glEnable(GL_DEPTH_TEST)

        pygame.display.flip()
        clock.tick(60)  # target 60 FPS

    # cleanup
    reader.stop()
    ser.close()
    if csv_file:
        csv_file.close()
        print(f"Saved to {args.output}")
    pygame.quit()
    print("Done.")


if __name__ == "__main__":
    main()