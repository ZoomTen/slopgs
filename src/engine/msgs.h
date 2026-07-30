/* msgs.h -- public ABI for the freestanding wasm32 MIDI synth.
 * This is the fixed ABI from SPEC.adoc Part 1 S1.5.3. Do not change signatures. */
#ifndef MSGS_H
#define MSGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t msgs_abi_version(void);                 /* returns 1 */
uint32_t msgs_mem_size(void);                    /* bytes of linear memory required/used */
uint32_t msgs_alloc(uint32_t nbytes);            /* bump allocator; returns offset into linear memory */
int32_t  msgs_init(uint32_t dls_ptr, uint32_t dls_len);      /* host places gm.dls first; 0 = OK */
void     msgs_reset(void);                       /* channels+voices to defaults AND rewinds the loaded song */
int32_t  msgs_load_smf(uint32_t smf_ptr, uint32_t smf_len);  /* 0 = OK */
void     msgs_set_loop(int32_t loops);           /* -1 = infinite, 0 = play once */
uint32_t msgs_render(uint32_t out_ptr, uint32_t frames);     /* stereo interleaved int16 @22050 Hz */
int32_t  msgs_is_finished(void);
void     msgs_midi(uint32_t status, uint32_t d1, uint32_t d2); /* inject one short message immediately */

#ifdef __cplusplus
}
#endif

#endif /* MSGS_H */
