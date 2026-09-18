#include "tm_detector.h"

#include <math.h>
#include <string.h>

// Below this the MLX90640's own noise floor makes a smaller sigma a
// measurement artefact, and a threshold built on it would fire on noise.
#define TM_MIN_SIGMA_C 0.08f
// Fraction of the frame that may be foreground before it reads as the whole
// scene moving (air-con, sun, sensor warm-up) rather than people.
#define TM_GLOBAL_SHIFT_FRACTION 0.40f
#define TM_MAX_PEAKS 32

static float g_smooth[TM_GRID_SIZE];
static uint8_t g_component[TM_GRID_SIZE];
static uint16_t g_queue[TM_GRID_SIZE];
static uint16_t g_members[TM_GRID_SIZE];

void tm_detector_default_params(TmDetectorParams* p) {
    // Tuned for 110 deg at 3-5 m against the host scenarios; see
    // test/host/detector_test.py. A person there peaks 1.5-6 C above an office
    // floor; the sensor's noise is 0.1-0.25 C per pixel at 1 fps.
    p->min_contrast_c = 0.6f;
    p->min_peak_c = 1.2f;
    p->noise_k = 4.0f;
    p->min_area = 1;
    p->max_area = 60;
    p->bg_tau_frames = 90;
    p->bg_learn_frames = 20;
    p->split_sep_px = 1.9f;
}

void tm_detector_init(TmDetector* det, const TmDetectorParams* params) {
    memset(det, 0, sizeof(*det));
    det->params = *params;
}

void tm_detector_reset_background(TmDetector* det) {
    memset(det->background, 0, sizeof(det->background));
    memset(det->variance, 0, sizeof(det->variance));
    memset(det->foreground, 0, sizeof(det->foreground));
    memset(det->label, 0, sizeof(det->label));
    det->frames_learned = 0;
    det->background_ready = false;
    det->count = 0;
}

float tm_detector_background_mean(const TmDetector* det) {
    float sum = 0.0f;
    for (int i = 0; i < TM_GRID_SIZE; ++i) sum += det->background[i];
    return sum / TM_GRID_SIZE;
}

// Running mean and variance over the learning frames (Welford). `variance`
// holds the sum of squares until learning finishes.
static void learn(TmDetector* det, const float* t) {
    const float n = (float) (det->frames_learned + 1);
    for (int i = 0; i < TM_GRID_SIZE; ++i) {
        const float delta = t[i] - det->background[i];
        det->background[i] += delta / n;
        det->variance[i] += delta * (t[i] - det->background[i]);
    }
    det->frames_learned++;
    if (det->frames_learned >= det->params.bg_learn_frames) {
        for (int i = 0; i < TM_GRID_SIZE; ++i) {
            float v = det->variance[i] / (float) det->frames_learned;
            if (v < TM_MIN_SIGMA_C * TM_MIN_SIGMA_C) v = TM_MIN_SIGMA_C * TM_MIN_SIGMA_C;
            det->variance[i] = v;
        }
        det->background_ready = true;
    }
}

static bool near_foreground(const TmDetector* det, int x, int y) {
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int nx = x + dx, ny = y + dy;
            if (nx >= 0 && nx < TM_GRID_W && ny >= 0 && ny < TM_GRID_H &&
                det->foreground[ny * TM_GRID_W + nx]) {
                return true;
            }
        }
    }
    return false;
}

static void track_background(TmDetector* det, const float* t, bool everywhere, float alpha) {
    for (int y = 0; y < TM_GRID_H; ++y) {
        for (int x = 0; x < TM_GRID_W; ++x) {
            const int i = y * TM_GRID_W + x;
            float a = alpha;
            if (!everywhere && near_foreground(det, x, y)) {
                // A foreground pixel that is not part of any accepted person --
                // a radiator switched on, a region too big to be one person --
                // is absorbed ten times slower. People (label != 0) never are:
                // a student who sits still for two hours is still there.
                if (det->label[i] != 0 || !det->foreground[i]) continue;
                a = alpha * 0.1f;
            }
            const float delta = t[i] - det->background[i];
            det->background[i] += a * delta;
            if (!det->foreground[i]) {
                float v = det->variance[i] + a * (delta * delta - det->variance[i]);
                if (v < TM_MIN_SIGMA_C * TM_MIN_SIGMA_C) v = TM_MIN_SIGMA_C * TM_MIN_SIGMA_C;
                det->variance[i] = v;
            }
        }
    }
}

static void smooth_positive(const float* d, float* out) {
    static const float k[3] = {1.0f, 2.0f, 1.0f};
    for (int y = 0; y < TM_GRID_H; ++y) {
        for (int x = 0; x < TM_GRID_W; ++x) {
            float sum = 0.0f, wsum = 0.0f;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || nx >= TM_GRID_W || ny < 0 || ny >= TM_GRID_H) continue;
                    const float w = k[dx + 1] * k[dy + 1];
                    const float v = d[ny * TM_GRID_W + nx];
                    sum += w * (v > 0.0f ? v : 0.0f);
                    wsum += w;
                }
            }
            out[y * TM_GRID_W + x] = sum / wsum;
        }
    }
}

// Split one connected component into people at its heat peaks.
static void split_component(TmDetector* det, const float* t, int member_count) {
    const TmDetectorParams* p = &det->params;

    // Local maxima of the smoothed contrast. Ties broken by raster order so a
    // flat-topped blob yields one peak, not several.
    uint16_t peaks[TM_MAX_PEAKS];
    int peak_count = 0;
    for (int m = 0; m < member_count; ++m) {
        const int i = g_members[m];
        const int x = i % TM_GRID_W, y = i / TM_GRID_W;
        const float v = g_smooth[i];
        bool is_max = true;
        for (int dy = -1; dy <= 1 && is_max; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || nx >= TM_GRID_W || ny < 0 || ny >= TM_GRID_H) continue;
                const int j = ny * TM_GRID_W + nx;
                const float w = g_smooth[j];
                if (w > v || (w == v && j < i)) { is_max = false; break; }
            }
        }
        if (is_max && peak_count < TM_MAX_PEAKS) peaks[peak_count++] = (uint16_t) i;
    }
    if (peak_count == 0) return;

    // Strongest first, then drop any peak too close to a stronger one.
    for (int a = 1; a < peak_count; ++a) {
        const uint16_t key = peaks[a];
        int b = a - 1;
        while (b >= 0 && g_smooth[peaks[b]] < g_smooth[key]) { peaks[b + 1] = peaks[b]; --b; }
        peaks[b + 1] = key;
    }
    const float sep2 = p->split_sep_px * p->split_sep_px;
    int kept = 0;
    for (int a = 0; a < peak_count; ++a) {
        const int ax = peaks[a] % TM_GRID_W, ay = peaks[a] / TM_GRID_W;
        bool close = false;
        for (int b = 0; b < kept && !close; ++b) {
            const int bx = peaks[b] % TM_GRID_W, by = peaks[b] / TM_GRID_W;
            const float dx = (float) (ax - bx), dy = (float) (ay - by);
            close = dx * dx + dy * dy < sep2;
        }
        if (!close) peaks[kept++] = peaks[a];
    }

    // Accumulate each pixel into its nearest kept peak.
    float sum_w[TM_MAX_PEAKS] = {0}, sum_x[TM_MAX_PEAKS] = {0}, sum_y[TM_MAX_PEAKS] = {0};
    float peak_d[TM_MAX_PEAKS] = {0}, peak_t[TM_MAX_PEAKS] = {0};
    uint16_t area[TM_MAX_PEAKS] = {0};
    for (int m = 0; m < member_count; ++m) {
        const int i = g_members[m];
        const int x = i % TM_GRID_W, y = i / TM_GRID_W;
        int best = 0;
        float best_d2 = 1e9f;
        for (int k = 0; k < kept; ++k) {
            const float dx = (float) (x - peaks[k] % TM_GRID_W), dy = (float) (y - peaks[k] / TM_GRID_W);
            const float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; best = k; }
        }
        const float w = det->diff[i] > 0.0f ? det->diff[i] : 0.0f;
        sum_w[best] += w;
        sum_x[best] += w * ((float) x + 0.5f);
        sum_y[best] += w * ((float) y + 0.5f);
        area[best]++;
        if (det->diff[i] > peak_d[best]) peak_d[best] = det->diff[i];
        if (t[i] > peak_t[best]) peak_t[best] = t[i];
        g_component[i] = (uint8_t) (best + 1);   // reuse: owner within this component
    }

    for (int k = 0; k < kept; ++k) {
        if (area[k] < p->min_area || area[k] > p->max_area || peak_d[k] < p->min_peak_c || sum_w[k] <= 0.0f) {
            continue;
        }
        if (det->count >= TM_MAX_DETECTIONS) { det->truncated = true; return; }
        TmDetection* out = &det->detections[det->count];
        out->x = sum_x[k] / sum_w[k];
        out->y = sum_y[k] / sum_w[k];
        out->area = area[k];
        out->contrast = peak_d[k];
        out->peak = peak_t[k];
        out->heat = sum_w[k];
        det->count++;
        for (int m = 0; m < member_count; ++m) {
            const int i = g_members[m];
            if (g_component[i] == k + 1) det->label[i] = det->count;
        }
    }
}

uint8_t tm_detector_step(TmDetector* det, const float* t) {
    const TmDetectorParams* p = &det->params;
    det->count = 0;
    det->truncated = false;
    det->global_shift = false;
    memset(det->label, 0, sizeof(det->label));

    if (!det->background_ready) {
        learn(det, t);
        memset(det->diff, 0, sizeof(det->diff));
        memset(det->foreground, 0, sizeof(det->foreground));
        return 0;
    }

    int foreground_cells = 0;
    for (int i = 0; i < TM_GRID_SIZE; ++i) {
        det->diff[i] = t[i] - det->background[i];
        float threshold = p->noise_k * sqrtf(det->variance[i]);
        if (threshold < p->min_contrast_c) threshold = p->min_contrast_c;
        det->foreground[i] = det->diff[i] > threshold ? 1 : 0;
        foreground_cells += det->foreground[i];
    }

    const float alpha = 1.0f / (float) (p->bg_tau_frames > 0 ? p->bg_tau_frames : 1);

    if (foreground_cells > (int) (TM_GLOBAL_SHIFT_FRACTION * TM_GRID_SIZE)) {
        // Not a crowd under one sensor: the scene moved. Counting it would
        // report a room full of people; masking it would lock the background
        // out forever. Report nothing, flag it, and follow the scene quickly.
        det->global_shift = true;
        track_background(det, t, true, alpha * 8.0f > 1.0f ? 1.0f : alpha * 8.0f);
        return 0;
    }

    smooth_positive(det->diff, g_smooth);

    memset(g_component, 0, sizeof(g_component));
    uint8_t visited[TM_GRID_SIZE];
    memset(visited, 0, sizeof(visited));
    for (int start = 0; start < TM_GRID_SIZE; ++start) {
        if (!det->foreground[start] || visited[start]) continue;
        int head = 0, tail = 0, members = 0;
        g_queue[tail++] = (uint16_t) start;
        visited[start] = 1;
        while (head < tail) {
            const int i = g_queue[head++];
            g_members[members++] = (uint16_t) i;
            const int x = i % TM_GRID_W, y = i / TM_GRID_W;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || nx >= TM_GRID_W || ny < 0 || ny >= TM_GRID_H) continue;
                    const int j = ny * TM_GRID_W + nx;
                    if (det->foreground[j] && !visited[j]) {
                        visited[j] = 1;
                        g_queue[tail++] = (uint16_t) j;
                    }
                }
            }
        }
        split_component(det, t, members);
    }

    track_background(det, t, false, alpha);
    return det->count;
}
