/* mod_music.h — a content pack's `music/<sequence_id>.wav` in place of the
 * sequenced original.
 *
 * The replacement is PRESENTATION-ONLY, and that word decides the whole design.
 * When a pack supplies a track, the sequence player is *not* skipped: it starts,
 * it advances, it posts its events, its channel and tempo state evolve exactly
 * as it would with no pack installed, and anything in the game that asks
 * "what is playing, at what tempo, how far in" gets the same answer it always
 * did. What changes is one thing: the player's output gain goes to zero and
 * these samples are added to the same bus instead.
 *
 * Muting and skipping are not interchangeable. Skipping the player would drop
 * the AL_MIDI_META_TEMPO stream that drives music_animation_fraction(), the
 * sequence-id reads in objects.c that gate cutscene fades and channel sets, and
 * every event the queue would have posted. The [SIMHASH] v3 stream is
 * byte-identical with a music pack installed precisely because none of that is
 * touched -- tests/check_mod_music_override.py asserts it, and that assertion
 * is the contract this module is written against.
 *
 * Decoding happens ONCE, at track start, straight to the mixer's own rate and
 * channel count. The mixer's rate is fixed at init and never changes, so
 * resampling per callback would be work repeated forever for an answer that
 * cannot differ. The resampler is linear, which is the right trade here: the
 * common case is a pack authored at the output rate already (no resampling at
 * all), and the uncommon case is a stereo track a player converted themselves,
 * where the artefacts of linear interpolation sit far below what the 22 kHz
 * output bus can represent anyway.
 *
 * Single-threaded, by the same standing decision as mod_texture_store.h
 * (docs/ARCHITECTURE_DECISIONS.md §1). mdkr_mod_music_mix() is called from the
 * audio service tick on the main loop, not from a device callback: the SDL
 * audio thread pulls from the ring, which is downstream of everything here.
 */
#ifndef MDKR64_MOD_MUSIC_H
#define MDKR64_MOD_MUSIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decoded bytes one track may occupy, 64 MiB. At the port's 22050 Hz stereo
 * output that is about 12 minutes of music, which is longer than every sequence
 * in the game put together; a pack asking for more is refused with a reason in
 * the log rather than allowed to decide how much memory the process uses. The
 * same number bounds the encoded file, so a hostile header cannot get an
 * allocation approved by claiming a modest decoded size. */
#define MDKR_MOD_MUSIC_BYTES_MAX ((size_t)64u * 1024u * 1024u)

struct MdkrModRegistry;

/* Binds the store to an already-scanned registry and to the output format every
 * decoded track will be produced in. `registry` is borrowed, not copied: it must
 * outlive the store. Passing NULL is legal and leaves the store permanently
 * inactive, which is what a build with no content packs gets.
 *
 * `sample_rate` and `channels` are the mixer's, captured here because they are
 * fixed for the process; calling this twice with different values is a
 * programming error and is treated as a fresh binding (every cached track is
 * dropped, since it was decoded for the old format). */
void mdkr_mod_music_init(const struct MdkrModRegistry *registry,
                         int sample_rate, int channels);
/* Frees the decoded track and unbinds the registry. Safe to call twice, and
 * safe to call when init never ran. */
void mdkr_mod_music_shutdown(void);

/* Whether pack music may stand in for the sequence at all -- the music half of
 * the player's "Custom content" setting (Content.PacksEnabled), pushed here by
 * mdkr_video_config_publish() the same way the texture half is pushed to
 * mdkr_mod_texture_set_enabled(). Enabled by default, so a build that never
 * publishes anything behaves exactly as it did before this switch existed.
 *
 * It is read at mdkr_mod_music_begin() and NOWHERE ELSE, which makes it a
 * decision taken once per track rather than a gate on the mix. That is not an
 * economy, it is the only reversible place to put it: the sequence player is
 * muted by having its volume redirected here and set to zero, and nothing
 * short of the game's next volume change gives it back. Silencing a
 * replacement mid-track would therefore leave the original silent too, and the
 * player would hear a hole rather than the game's own music. Switching custom
 * content off is honoured from the next piece of music onwards; what is
 * already playing plays out. */
void mdkr_mod_music_set_enabled(int enabled);
int  mdkr_mod_music_enabled(void);

/* Asks for `music/<sequence_id>.wav`. Returns 1 when an enabled pack supplies a
 * usable track, which is the caller's signal to mute the sequence player; 0
 * when it does not, which changes nothing at all. Playback starts at the
 * beginning of the track.
 *
 * A track that will not decode is reported once, with the pack's own file named
 * and the reason stated, and is thereafter treated as absent -- a sequence that
 * restarts every lap must not put a line in the log every lap. */
int  mdkr_mod_music_begin(int sequence_id);

/* Ends the replacement. The decoded samples are kept, because the overwhelming
 * case for stopping is a sequence that is about to be started again. */
void mdkr_mod_music_stop(void);

/* 1 while a decoded track is standing in for the sequence player. This is the
 * only thing the mute decision may be conditioned on, so that "muted" and
 * "replaced" can never disagree. */
int  mdkr_mod_music_active(void);

/* The gain the replacement plays at, as Q15 (32768 == unity), clamped into
 * range. This is how the game's own music volume reaches the track: the
 * sequence player's volume is intercepted at alCSPSetVol(), converted, and
 * handed here before being zeroed, so a Music slider at 0 silences the
 * replacement exactly as it silences the sequence, and every authored fade the
 * sequence would have taken applies to the replacement too. */
void mdkr_mod_music_set_gain_q15(int gain_q15);

/* ADDS `frames` sample-frames of the track into `out`, at the gain above, with
 * saturation. Returns 1 when it contributed, 0 when it did not.
 *
 * Adding, not writing: `out` is the whole synthesised bus, and the sound
 * effects in it are not what was replaced. The sequence player's own
 * contribution to that buffer is already zero by the time this runs, which is
 * what makes an add the right operation rather than a mix decision.
 *
 * The track loops. A sequence that would have repeated for the length of a race
 * is replaced by something that does the same, rather than by something that
 * plays once and leaves silence behind it. */
int  mdkr_mod_music_mix(int16_t *out, int frames);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MOD_MUSIC_H */
