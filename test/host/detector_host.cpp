// Runs the node's detector on frames from stdin (768 float32 C per frame) and
// prints one JSON line per frame. Compiled from src/tm_detector.cpp unchanged.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tm_detector.h"

static TmDetector det;

int main(int argc, char** argv) {
    TmDetectorParams p;
    tm_detector_default_params(&p);
    // Optional overrides: name=value pairs, for tuning sweeps.
    for (int a = 1; a < argc; ++a) {
        float v;
        char name[32];
        if (sscanf(argv[a], "%31[^=]=%f", name, &v) != 2) continue;
        if (!strcmp(name, "min_contrast")) p.min_contrast_c = v;
        else if (!strcmp(name, "min_peak")) p.min_peak_c = v;
        else if (!strcmp(name, "noise_k")) p.noise_k = v;
        else if (!strcmp(name, "min_area")) p.min_area = (uint16_t) v;
        else if (!strcmp(name, "max_area")) p.max_area = (uint16_t) v;
        else if (!strcmp(name, "bg_tau")) p.bg_tau_frames = (uint16_t) v;
        else if (!strcmp(name, "split_sep")) p.split_sep_px = v;
    }
    tm_detector_init(&det, &p);
    float frame[TM_GRID_SIZE];
    int index = 0;
    while (fread(frame, sizeof(float), TM_GRID_SIZE, stdin) == TM_GRID_SIZE) {
        const uint8_t n = tm_detector_step(&det, frame);
        printf("{\"i\":%d,\"ready\":%d,\"shift\":%d,\"n\":%u,\"d\":[", index++,
               det.background_ready ? 1 : 0, det.global_shift ? 1 : 0, (unsigned) n);
        for (uint8_t k = 0; k < n; ++k) {
            const TmDetection* d = &det.detections[k];
            printf("%s[%.3f,%.3f,%u,%.2f,%.2f,%.2f]", k ? "," : "", d->x, d->y, (unsigned) d->area,
                   d->contrast, d->peak, d->heat);
        }
        printf("]}\n");
    }
    return 0;
}
