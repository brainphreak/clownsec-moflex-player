/* Minimal MP4 (H.264/AVC) playback via the New-3DS MVD hardware decoder.
 * VIDEO-ONLY test path for now (no audio/seek/panel) -- proves MVD decodes on hardware.
 * New-3DS only; on Old 3DS it shows a "needs New 3DS" notice and returns. */
#ifndef MP4_PLAY_H
#define MP4_PLAY_H

#include "moflex_playback.h"   /* MoflexResult */

MoflexResult mp4_play(const char *path);

#endif
