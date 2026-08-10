/* dr_wav_impl.c — the one translation unit that instantiates dr_wav.
 *
 * dr_wav.h is header-only: exactly one TU in the program may define
 * DR_WAV_IMPLEMENTATION, and this is it. Keeping that TU separate from the code
 * that uses the decoder is what lets CMake compile third-party source with
 * warnings off (set_source_files_properties(... COMPILE_OPTIONS -w)) without
 * weakening -Wall -Wextra -Werror for any first-party file. This is the same
 * arrangement lib/stb/stb_image_impl.c uses, for the same reason. Never fold it
 * back into platform/mod_music.c.
 *
 * The configuration is deliberately narrow, and platform/mod_music.c repeats it
 * before taking the declarations — the two must agree about which entry points
 * exist:
 *
 *   DR_WAV_NO_STDIO   The music store reads the WAV itself, out of a content
 *                     pack that may be a zip, so there is not necessarily a
 *                     path to open at all. Letting dr_wav open paths would put
 *                     a second, narrow-CRT file API in the tree beside the
 *                     UTF-8 boundary fs_utf8 already owns, and it would be an
 *                     API that cannot see inside an archive.
 *
 * The conversion API is deliberately NOT disabled: drwav_read_pcm_frames_s16()
 * is what lets a pack ship 8-bit, 24-bit, 32-bit or float WAV and still reach
 * the mixer as the s16 it wants, decoded once at load.
 */
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "dr_wav.h"
