// Runs the node's detector on frames from stdin (768 float32 C per frame) and
// prints one JSON line per frame. Compiled from src/tm_detector.cpp unchanged.
//
// With --planes it also writes each frame's internal planes to stdout, so the
// algo debugger can show what the detector saw rather than a second
// implementation's guess at it. Per frame, after the JSON line:
//
//   "P" 'L' 'N' '1'          4-byte marker
//   background  float32[768]
//   diff        float32[768] (T - background)
//   foreground  uint8[768]   (0/1 mask)
//   label       uint8[768]   (0 = none, else detection index + 1)
//
// Nothing here changes detector behaviour: it only reads the struct the
// detector already keeps.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tm_detector.h"

static TmDetector det;

int main(int argc, char** argv) {
    TmDetectorParams p;
    tm_detector_default_params(&p);
    bool planes = false;
    // Optional overrides: name=value pairs, for tuning sweeps.
    for (int a = 1; a < argc; ++a) {
        if (!strcmp(argv[a], "--planes")) { planes = true; continue; }
        float v;
        char name[32];
        if (sscanf(argv[a], "%31[^=]=%f", name, &v) != 2) continue;
        if (!strcmp(name, "min_contrast")) p.min_contrast_c = v;
        else if (!strcmp(name, "min_peak")) p.min_peak_c = v;
        else if (!strcmp(name, "noise_k")) p.noise_k = v;
        else if (!strcmp(name, "min_area")) p.min_area = (uint16_t) v;
        else if (!strcmp(name, "max_area")) p.max_area = (uint16_t) v;
        else if (!strcmp(name, "bg_tau")) p.bg_tau_frames = (uint16_t) v;
        else if (!strcmp(name, "bg_frames")) p.bg_learn_frames = (uint16_t) v;
        else if (!strcmp(name, "split_sep")) p.split_sep_px = v;
    }
    tm_detector_init(&det, &p);
    float frame[TM_GRID_SIZE];
    int index = 0;
    while (fread(frame, sizeof(float), TM_GRID_SIZE, stdin) == TM_GRID_SIZE) {
        const uint8_t n = tm_detector_step(&det, frame);
        printf("{\"i\":%d,\"ready\":%d,\"shift\":%d,\"trunc\":%d,\"bg\":%.3f,\"n\":%u,\"d\":[", index++,
               det.background_ready ? 1 : 0, det.global_shift ? 1 : 0, det.truncated ? 1 : 0,
               tm_detector_background_mean(&det), (unsigned) n);
        for (uint8_t k = 0; k < n; ++k) {
            const TmDetection* d = &det.detections[k];
            printf("%s[%.3f,%.3f,%u,%.2f,%.2f,%.2f]", k ? "," : "", d->x, d->y, (unsigned) d->area,
                   d->contrast, d->peak, d->heat);
        }
        printf("]}\n");
        if (planes) {
            fwrite("PLN1", 1, 4, stdout);
            fwrite(det.background, sizeof(float), TM_GRID_SIZE, stdout);
            fwrite(det.diff, sizeof(float), TM_GRID_SIZE, stdout);
            fwrite(det.foreground, 1, TM_GRID_SIZE, stdout);
            fwrite(det.label, 1, TM_GRID_SIZE, stdout);
        }
        fflush(stdout);
    }
    return 0;
}
