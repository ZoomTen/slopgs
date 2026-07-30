/* smf.h -- Standard MIDI File parser and sequencer.
 * SPEC.adoc S1.5.4: NOT part of the original swmidi.sys -- this exists only so
 * the module is runnable/testable standalone. Ordinary SMF 0/1 parsing and
 * a tick->sample scheduler feeding msgs_midi/synth_sysex; no reverse-
 * engineered behaviour to match. */
#ifndef SMF_H
#define SMF_H

#include <stdint.h>

int smf_load(const uint8_t *data, uint32_t len); /* 0 = OK; rewinds */
void smf_rewind(void);                           /* back to tick 0, clears the finished latch */
void smf_set_loop(int32_t loops);                /* -1 = infinite, 0 = once, N = loop N extra times */
uint32_t smf_render(int16_t *out, uint32_t frames);
int32_t smf_is_finished(void);

#endif /* SMF_H */
