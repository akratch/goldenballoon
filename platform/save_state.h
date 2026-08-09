/* save_state.h — practice-oriented state snapshots.
 *
 * DELIBERATELY NOT the progress save. Progress lives in
 * platform/save_container.c and its EEPROM image; a save state is a separate
 * file in a separate directory with a separate format, and no code path lets
 * one become the other. check_save_failsafe must keep passing untouched.
 *
 * That separation is enforced rather than assumed. This module includes
 * nothing from save_container.h or save_codec.h, its magic is its own, its
 * writer refuses to stamp a header whose tag is not this format's, and
 * nothing here knows the EEPROM image size or its checksum domains. A state
 * file dropped into the save directory is refused by both readers, because
 * neither one recognises the other's first four bytes.
 *
 * This task is the container only: the file format, and every way it is
 * allowed to be refused. Capture and restore land on top of it later, so the
 * job here is to make a hostile or damaged file a *refusal* rather than a
 * read primitive. The two traps this closes:
 *
 *   1. A declared length trusted without checking the file that backs it.
 *      `payload_bytes` is attacker-controlled the moment a player downloads
 *      someone else's state. The reader cross-checks it against both the
 *      caller's buffer capacity and the actual file size before a single
 *      payload byte is read, and allocates nothing of its own — the caller
 *      supplies the buffer, so the reader can never grow one to fit a lie.
 *
 *   2. A struct written straight to disk. Every multi-byte field is
 *      serialised explicitly little-endian into a fixed 72-byte record, so
 *      padding, field order, and host byte order cannot silently redefine the
 *      format. MDKR_SAVE_STATE_HEADER_BYTES is the contract;
 *      sizeof(MdkrSaveStateHeader) is an implementation detail of whichever
 *      compiler is in front of you.
 *
 * Every refusal writes a one-line human-readable reason, because a state a
 * player cannot load needs to say why in the UI, and
 * mdkr_save_state_validate() answers that question from a header alone,
 * without the payload and without the file.
 */
#ifndef MDKR64_SAVE_STATE_H
#define MDKR64_SAVE_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_SAVE_STATE_MAGIC   0x4D444B53u /* 'MDKS' */
#define MDKR_SAVE_STATE_VERSION 1u

#define MDKR_SAVE_STATE_ROM_REVISION_MAX 16u
#define MDKR_SAVE_STATE_APP_VERSION_MAX  32u

/*
 * Bytes the header occupies on disk, little-endian, in declaration order:
 *
 *   0  magic           u32
 *   4  format_version  u32
 *   8  rom_revision    16 bytes, NUL-terminated and NUL-padded
 *  24  app_version     32 bytes, NUL-terminated and NUL-padded
 *  56  tick            u64
 *  64  payload_bytes   u32
 *  68  payload_crc32   u32
 *  72  payload         payload_bytes bytes, and the file ends there
 *
 * This is the format contract. Do not derive it from sizeof or offsetof.
 */
#define MDKR_SAVE_STATE_HEADER_BYTES 72u

/*
 * Sanity ceiling on a declared payload. Well above any plausible arena
 * snapshot and far below the point where a length becomes an allocation
 * attack on whatever the caller sizes its buffer from.
 */
#define MDKR_SAVE_STATE_MAX_PAYLOAD (64u * 1024u * 1024u)

/* Comfortably fits every reason this module produces, including both a
 * 16-byte revision and a 32-byte version in one line. */
#define MDKR_SAVE_STATE_ERR_MAX 256u

typedef struct MdkrSaveStateHeader {
    uint32_t magic;
    uint32_t format_version;
    char     rom_revision[MDKR_SAVE_STATE_ROM_REVISION_MAX]; /* e.g. "us.v80" */
    char     app_version[MDKR_SAVE_STATE_APP_VERSION_MAX];   /* at capture */
    uint64_t tick;              /* authoritative tick at capture */
    uint32_t payload_bytes;
    uint32_t payload_crc32;
} MdkrSaveStateHeader;

/*
 * Non-zero results, so a caller that wants to branch can, without anyone
 * being required to. The one-line reason in `err` is the primary output.
 */
typedef enum MdkrSaveStateResult {
    MDKR_SAVE_STATE_OK = 0,
    MDKR_SAVE_STATE_ERR_ARGUMENT = -1,
    MDKR_SAVE_STATE_ERR_FORMAT = -2,    /* not this format at all */
    MDKR_SAVE_STATE_ERR_VERSION = -3,   /* this format, another revision of it */
    MDKR_SAVE_STATE_ERR_ROM = -4,
    MDKR_SAVE_STATE_ERR_APP = -5,
    MDKR_SAVE_STATE_ERR_TRUNCATED = -6, /* file is shorter than it claims */
    MDKR_SAVE_STATE_ERR_SIZE = -7,      /* declared length is impossible */
    MDKR_SAVE_STATE_ERR_CAPACITY = -8,  /* payload does not fit the caller */
    MDKR_SAVE_STATE_ERR_CHECKSUM = -9,
    MDKR_SAVE_STATE_ERR_IO = -10
} MdkrSaveStateResult;

/*
 * CRC-32/ISO-HDLC (the zlib/PNG one) over `size` bytes. Exposed so a caller
 * can fill in payload_crc32 and so tests can pin the polynomial: a state file
 * is meant to move between hosts, so this value is part of the format.
 * Returns 0 for an empty payload, which is that algorithm's own answer.
 */
uint32_t mdkr_save_state_crc32(const void *data, size_t size);

/*
 * Writes `header->payload_bytes` bytes of `payload` to `path`.
 *
 * `magic` and `format_version` must already hold this module's constants; a
 * header claiming to be anything else is refused rather than corrected, so a
 * caller that has confused this format with another finds out here.
 * `payload_crc32` is ignored on input and computed from the payload, so a
 * state whose checksum disagrees with its own bytes cannot be produced.
 * `rom_revision` and `app_version` must be NUL-terminated; the bytes after
 * the terminator are written as zeroes, so the file is byte-deterministic for
 * a given header and payload.
 *
 * 0 on success; non-zero with a one-line reason in `err`. A failed write
 * removes the file rather than leaving a half-written one behind.
 */
int mdkr_save_state_write(const char *path, const MdkrSaveStateHeader *header,
                          const void *payload, char *err, size_t err_size);

/*
 * Reads `path` into `*out_header` and the caller's `payload` buffer. No
 * allocation: `payload_cap` is the whole budget, and a state needing more is
 * refused, never truncated to fit.
 *
 * Does NOT compare ROM revision or app version — the file does not know what
 * session is loading it. Call mdkr_save_state_validate() with the returned
 * header for that, which is also how the UI can explain a refusal before any
 * payload is read.
 *
 * On failure `*out_header` is left untouched, so a rejected state cannot be
 * mistaken for a loaded one. Bytes beyond `payload_bytes` are never written.
 * A refusal decided from the header alone — wrong format, wrong version, bad
 * length, insufficient capacity — leaves the payload buffer untouched as
 * well; a checksum failure has already read the payload in, and zeroes it, so
 * a caller that ignores the return value gets blanks rather than plausible
 * corruption.
 *
 * 0 on success; non-zero with a one-line reason in `err`.
 */
int mdkr_save_state_read(const char *path, MdkrSaveStateHeader *out_header,
                         void *payload, size_t payload_cap,
                         char *err, size_t err_size);

/*
 * Decides whether `header` describes a state this session may load, without
 * the file and without the payload. Checks the format tag, the format
 * version, that the fixed-width text fields are terminated, and that the
 * declared length is possible; then compares `rom_revision` and
 * `app_version` exactly. Pass NULL for either string to skip that comparison
 * — that is how a state browser inspects a file it is not about to load.
 *
 * 0 on success; non-zero with a one-line reason in `err` naming both sides of
 * whichever comparison failed, because "wrong version" alone is not something
 * a player can act on.
 */
int mdkr_save_state_validate(const MdkrSaveStateHeader *header,
                             const char *rom_revision, const char *app_version,
                             char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_SAVE_STATE_H */
