#ifndef SFX_H
#define SFX_H

/* UI sound effects: a click as the cursor moves, a chime when something is
 * chosen. Short clips held in RAM and mixed by one background thread that
 * keeps an ALSA stream open, rather than a process spawned per press —
 * navigation can fire ten times a second and fork+exec+device-open on this
 * Cortex-A9 would arrive late enough to feel disconnected from the keypress
 * that caused it. */

enum { SFX_NAV, SFX_CONFIRM, SFX_COUNT };

/* Loads the clips and starts the mixer. Never fatal: with no clips, no
 * libasound on the device, or no sound card the app runs silent and says so
 * once on the debug log. */
void sfx_init(const char *dir);

void sfx_play(int id);

/* Menus want the device held open (a click has to be instant); playback wants
 * it released, so mplayer is not left contending with us for the card — on
 * the MiSTer that contention is total rather than partial, see sfx.c's
 * /dev/MrAudio comment. Call every frame — only transitions do any work. */
void sfx_set_enabled(int on);

void sfx_shutdown(void);

#endif
