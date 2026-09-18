#ifndef TM_DETECTOR_H
#define TM_DETECTOR_H

/**
 * Person detector for a ceiling-mounted MLX90640 with the 110 x 75 deg lens.
 *
 * Why it is not the old pipeline: at 3-5 m a pixel covers 16-34 cm of floor
 * at seated-head height, so a person is a blob of about 2-12 pixels, often
 * only part of a pixel wide. The old 8-bit pipeline opened the mask with a
 * 3x3 erosion (deleting anything under 3x3) and demanded 20-200 pixels. This
 * one works in degrees C, keeps single-pixel blobs, and splits blobs at local
 * heat peaks rather than by eroding them apart.
 *
 * Stages, per frame:
 *   1. d = T - background                       (float, C)
 *   2. foreground = d > max(min_contrast, k * sigma_pixel)
 *   3. 8-connected components of the foreground
 *   4. inside each component, local maxima of a 3x3-smoothed d are person
 *      candidates; maxima closer than split_sep merge; each pixel goes to its
 *      nearest surviving maximum
 *   5. keep blobs with min_area <= area <= max_area and peak >= min_peak
 *   6. background and per-pixel noise follow the scene where there is no
 *      foreground (dilated by one pixel), so a seated person is never absorbed
 *
 * Plain C with structs and static storage: no heap, no recursion, and the same
 * file compiles on the host for test/host/detector_test.py.
 */

#include <stdint.h>
#include <stdbool.h>
#include "tm_protocol.h"

typedef struct {
    float min_contrast_c;   // foreground threshold floor
    float min_peak_c;       // a blob's peak contrast must reach this
    float noise_k;          // foreground also needs d > noise_k * sigma
    uint16_t min_area;
    uint16_t max_area;
    uint16_t bg_tau_frames; // background EMA time constant
    uint16_t bg_learn_frames;
    float split_sep_px;     // peaks closer than this are one person
} TmDetectorParams;

typedef struct {
    float x;         // centroid, pixels (contrast-weighted)
    float y;
    uint16_t area;
    float contrast;  // peak d, C
    float peak;      // peak absolute temperature, C
    float heat;      // sum of positive d over the blob, C*px
} TmDetection;

typedef struct {
    TmDetectorParams params;

    float background[TM_GRID_SIZE];
    float variance[TM_GRID_SIZE];   // EMA of (T - background)^2 where not foreground
    float diff[TM_GRID_SIZE];       // last frame's d, kept for the RAW/debug path
    uint8_t foreground[TM_GRID_SIZE];
    uint8_t label[TM_GRID_SIZE];    // 0 = none, 1..n = detection index + 1

    uint16_t frames_learned;
    bool background_ready;
    bool global_shift;              // last frame: most of the scene moved at once
    bool truncated;                 // last frame: more blobs than TM_MAX_DETECTIONS

    TmDetection detections[TM_MAX_DETECTIONS];
    uint8_t count;
} TmDetector;

void tm_detector_default_params(TmDetectorParams* p);
void tm_detector_init(TmDetector* det, const TmDetectorParams* params);
void tm_detector_reset_background(TmDetector* det);

/** Process one frame of temperatures (C, row-major). Returns detections found. */
uint8_t tm_detector_step(TmDetector* det, const float* frame);

float tm_detector_background_mean(const TmDetector* det);

#endif // TM_DETECTOR_H
