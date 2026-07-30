/* render.h -- the mixer: interpolation, gain, saturating accumulate.
 * SPEC.adoc Part 6. */
#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>

/* Renders `frames` stereo frames (interleaved int16 L,R) into `out`,
 * starting from silence. Advances every active voice's phase and envelope
 * by exactly `frames` samples at the fixed 22050 Hz render rate. */
void render_frames(int16_t *out, uint32_t frames);

#endif /* RENDER_H */
