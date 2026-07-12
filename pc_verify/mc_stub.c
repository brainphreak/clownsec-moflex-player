/* Host-only link stubs for the ARM-assembly motion-comp routines in decoder/mc_asm.s (which only
 * assemble on devkitARM). They are referenced in mobiclip.c behind a runtime `mobi_opt & 0x100`
 * check that the PC verify build never enables, so they are never actually called -- these exist
 * solely to satisfy the linker on the host. Not part of the on-device build. */
#include <stdint.h>
void mc_havg_a(uint8_t *d, const uint8_t *s, int w, int h, int ds, int ss) { (void)d;(void)s;(void)w;(void)h;(void)ds;(void)ss; }
void mc_vavg_a(uint8_t *d, const uint8_t *s, int w, int h, int ds, int ss) { (void)d;(void)s;(void)w;(void)h;(void)ds;(void)ss; }
void mc_diag_a(uint8_t *d, const uint8_t *s, int w, int h, int ds, int ss) { (void)d;(void)s;(void)w;(void)h;(void)ds;(void)ss; }
