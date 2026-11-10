#ifndef ISO_SETUP_H
#define ISO_SETUP_H
/* Make sure <game_dir> holds the unpacked disc. If not, find the player's
 * disc image (iso_arg, a .iso beside the exe, or a file dialog) and unpack it.
 * Passing iso_arg forces a (re)install from that image.
 * Returns 1 when the game data is ready, 0 if the player cancelled or it failed. */
int iso_setup_ensure_game(const char *exe_dir, const char *game_dir, const char *iso_arg);
#endif
