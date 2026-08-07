#!/usr/bin/env python3
"""RAW16 instruments are decoded as big-endian PCM, not host-endian noise.

This is a focused timbre/endian gate. ``check_audio_output.py`` deliberately
cannot detect a wrong instrument waveform: the pre-fix binary passes its RMS,
tempo, stereo, clipping, and reverb checks while the bass sounds garbled and
buzzy. This check covers that missing boundary in three independent ways:

1. SOURCE CLASSIFICATION. ``alRaw16Pull`` must use the dedicated RAW16 loader at
   exactly its three load sites. ``_decodeChunk`` must keep its one ordinary
   ``aLoadBuffer`` because ADPCM is a byte stream and swapping it would corrupt
   every compressed sample.

2. ROM-DERIVED ORACLE. An independent bounds-checked parser walks the US/PAL
   v80 audio banks. It requires the measured bank topology (233 unique music
   waves: 208 ADPCM + 25 RAW16; 444 unique SFX waves: 443 ADPCM + 1 RAW16), then
   interprets each RAW16 sample both as correct big-endian s16 and as the old
   little-endian behavior. The largest music sample's normalized roughness is
   0.0518 correctly versus 1.4158 in the legacy interpretation (27.34x); the
   extra SFX RAW16 sample is covered too. No ROM bytes or derived PCM are kept.

3. SAME-BINARY RUNTIME A/B. The production ``fixed`` mode and exact
   ``MDKR_RAW16=legacy`` pre-fix control run the same 4,300-frame route. They
   must issue the same load count/bytes at the same output block, produce an
   exact common PCM prefix, and diverge strongly only after RAW16 is reached.
   The legacy arm is the broken-direction control: if the fix disappears, the
   two captures become identical and this check fails.

Always headless and muted. Captures are ROM-derived temporary files deleted on
exit unless ``--keep-audio`` is explicitly supplied; they must never be
committed.
"""

from __future__ import annotations

import argparse
import array
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import wave
from dataclasses import dataclass
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SCRIPT = ROOT / "tests/input_scripts/race_drive_time_trial.txt"
DEFAULT_FRAMES = 4300
CHANNELS = 2
RATE = 22050
WIDTH = 2
PUMP_FRAMES = 736

LUTS = {
    # CRC1/CRC2: (build name, master LUT, asset base). PAL v80's complete
    # asset payload is byte-identical to US v80's and therefore has the same
    # audio-bank oracle at its relocated asset base.
    (0xE402430D, 0xD2FCFC9D): ("us.v80", 0x000ED0E0, 0x000ED1B0),
    (0x596E145B, 0xF7D9879F): ("pal.v80", 0x000ED170, 0x000ED240),
}
ASSET_AUDIO_TABLE = 38
ASSET_AUDIO = 39

EXPECTED_AUDIO_OFFSETS = (
    0x0000BCD0, 0x000B9C28, 0x000C8748, 0x0038A7B8,
    0x0038E44C, 0x003D6060, 0x003D6128, 0x003D7A28,
    0x003D8310, 0x003D83BC, 0x003D9E10, 0x003DA0F4,
    0x003DA0F4, -1, 0, 0,
)

RAW_TRACE_RE = re.compile(
    r"\[AUDIO\] raw16 mode=(fixed|legacy) loads=(\d+) bytes=(\d+) "
    r"first_block_sample=(none|\d+)"
)
BAD_RE = re.compile(
    r"\[FATAL\]|\[CRASH\]|\[FX BUG\]|AddressSanitizer|"
    r"UndefinedBehaviorSanitizer|runtime error:"
)


class CheckError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckError(message)


def normalize_rom(data: bytes) -> bytes:
    require(len(data) == 12 * 1024 * 1024,
            f"ROM is {len(data)} bytes, expected 12 MiB")
    magic = data[:4]
    if magic == b"\x80\x37\x12\x40":
        return data
    if magic == b"\x37\x80\x40\x12":
        out = bytearray(len(data))
        out[0::2], out[1::2] = data[1::2], data[0::2]
        return bytes(out)
    if magic == b"\x40\x12\x37\x80":
        out = bytearray(len(data))
        out[0::4], out[1::4], out[2::4], out[3::4] = (
            data[3::4], data[2::4], data[1::4], data[0::4]
        )
        return bytes(out)
    raise CheckError(f"unrecognized N64 ROM byte order ({magic.hex()})")


def be_u16(data: bytes, offset: int) -> int:
    require(0 <= offset <= len(data) - 2,
            f"BE u16 outside buffer: offset={offset:#x} size={len(data):#x}")
    return struct.unpack_from(">H", data, offset)[0]


def be_s16(data: bytes, offset: int) -> int:
    value = be_u16(data, offset)
    return value - 0x10000 if value & 0x8000 else value


def be_u32(data: bytes, offset: int) -> int:
    require(0 <= offset <= len(data) - 4,
            f"BE u32 outside buffer: offset={offset:#x} size={len(data):#x}")
    return struct.unpack_from(">I", data, offset)[0]


def be_s32(data: bytes, offset: int) -> int:
    value = be_u32(data, offset)
    return value - 0x100000000 if value & 0x80000000 else value


def rom_section(rom: bytes, lut_start: int, asset_base: int, index: int) -> bytes:
    section_count = be_u32(rom, lut_start)
    require(section_count >= ASSET_AUDIO + 1,
            f"master LUT has only {section_count} sections")
    start = be_u32(rom, lut_start + 4 * (index + 1))
    end = be_u32(rom, lut_start + 4 * (index + 2))
    require(start <= end, f"asset section {index} has descending bounds")
    require(asset_base + end <= len(rom),
            f"asset section {index} leaves the ROM")
    return rom[asset_base + start:asset_base + end]


@dataclass(frozen=True)
class Wave:
    control_offset: int
    sample_offset: int
    length: int
    wave_type: int


@dataclass
class BankCensus:
    bank_count: int
    instrument_offsets: set[int]
    sound_offsets: set[int]
    sound_references: int
    waves: dict[int, Wave]


class BankParser:
    """Independent parser for the serialized N64 ALBankFile layout."""

    def __init__(
        self,
        control: bytes,
        audio: bytes,
        table_base: int,
        sample_end: int,
        label: str,
    ):
        self.control = control
        self.audio = audio
        self.table_base = table_base
        self.sample_end = sample_end
        self.label = label
        self.instruments: set[int] = set()
        self.sounds: set[int] = set()
        self.active_instruments: set[int] = set()
        self.sound_references = 0
        self.waves: dict[int, Wave] = {}

    def has(self, offset: int, size: int, what: str) -> None:
        require(offset >= 0 and size >= 0 and offset <= len(self.control) - size,
                f"{self.label} {what} [{offset:#x}, {offset + size:#x}) "
                f"leaves {len(self.control):#x}-byte control bank")

    def u32(self, offset: int, what: str) -> int:
        self.has(offset, 4, what)
        return be_u32(self.control, offset)

    def s16(self, offset: int, what: str) -> int:
        self.has(offset, 2, what)
        return be_s16(self.control, offset)

    def pointer(self, offset: int, what: str) -> int:
        value = self.u32(offset, what)
        require(value == 0 or value % 4 == 0,
                f"{self.label} {what} is not 4-byte aligned: {value:#x}")
        return value

    def visit_wave(self, offset: int) -> None:
        if offset in self.waves:
            return
        self.has(offset, 12, "wavetable")
        sample_offset = self.u32(offset, "wavetable sample offset")
        length = be_s32(self.control, offset + 4)
        wave_type = self.control[offset + 8]
        require(length > 0, f"{self.label} wavetable {offset:#x} length={length}")
        require(wave_type in (0, 1),
                f"{self.label} wavetable {offset:#x} type={wave_type}")
        if wave_type == 0:
            self.has(offset, 20, "ADPCM wavetable")
        else:
            self.has(offset, 16, "RAW16 wavetable")
            require(length % 2 == 0,
                    f"{self.label} RAW16 wavetable {offset:#x} has odd length {length}")
        absolute_start = self.table_base + sample_offset
        absolute_end = absolute_start + length
        require(self.table_base <= absolute_start <= absolute_end <= self.sample_end,
                f"{self.label} wavetable {offset:#x} sample "
                f"[{absolute_start:#x}, {absolute_end:#x}) leaves "
                f"[{self.table_base:#x}, {self.sample_end:#x})")
        require(absolute_end <= len(self.audio),
                f"{self.label} wavetable {offset:#x} leaves ASSET_AUDIO")
        self.waves[offset] = Wave(
            offset, absolute_start, length, wave_type
        )

    def visit_sound(self, offset: int) -> None:
        if offset in self.sounds:
            return
        self.has(offset, 15, "sound")
        self.sounds.add(offset)
        wave_offset = self.pointer(offset + 8, "sound wavetable pointer")
        require(wave_offset != 0,
                f"{self.label} sound {offset:#x} has no wavetable")
        self.visit_wave(wave_offset)

    def visit_instrument(self, offset: int) -> None:
        if offset == 0 or offset in self.instruments:
            return
        require(offset not in self.active_instruments,
                f"{self.label} instrument cycle at {offset:#x}")
        self.has(offset, 16, "instrument")
        self.active_instruments.add(offset)
        sound_count = self.s16(offset + 14, "instrument sound count")
        require(0 <= sound_count <= 4096,
                f"{self.label} instrument {offset:#x} soundCount={sound_count}")
        self.has(offset + 16, sound_count * 4, "instrument sound array")
        self.instruments.add(offset)
        for index in range(sound_count):
            sound_offset = self.pointer(
                offset + 16 + index * 4,
                f"instrument sound pointer {index}",
            )
            if sound_offset != 0:
                self.sound_references += 1
                self.visit_sound(sound_offset)
        self.active_instruments.remove(offset)

    def parse(self) -> BankCensus:
        self.has(0, 4, "bank header")
        revision = be_u16(self.control, 0)
        bank_count = self.s16(2, "bank count")
        require(revision == 0x4231,
                f"{self.label} bank revision={revision:#x}, expected 0x4231")
        require(0 < bank_count <= 64,
                f"{self.label} bankCount={bank_count}")
        self.has(4, bank_count * 4, "bank array")
        for bank_index in range(bank_count):
            bank_offset = self.pointer(4 + bank_index * 4,
                                       f"bank pointer {bank_index}")
            require(bank_offset != 0,
                    f"{self.label} bank {bank_index} is null")
            self.has(bank_offset, 12, "bank")
            instrument_count = self.s16(bank_offset, "instrument count")
            require(0 <= instrument_count <= 4096,
                    f"{self.label} bank {bank_index} "
                    f"instrumentCount={instrument_count}")
            self.has(bank_offset + 12, instrument_count * 4,
                     "bank instrument array")
            percussion = self.pointer(bank_offset + 8,
                                      "percussion instrument pointer")
            self.visit_instrument(percussion)
            for instrument_index in range(instrument_count):
                instrument = self.pointer(
                    bank_offset + 12 + instrument_index * 4,
                    f"instrument pointer {instrument_index}",
                )
                self.visit_instrument(instrument)
        return BankCensus(
            bank_count,
            self.instruments,
            self.sounds,
            self.sound_references,
            self.waves,
        )


def normalized_roughness(samples: tuple[int, ...]) -> float:
    require(len(samples) >= 2, "RAW16 metric needs at least two samples")
    mean_square = sum(value * value for value in samples) / len(samples)
    require(mean_square > 0, "RAW16 metric encountered a silent sample")
    diff_square = sum(
        (samples[index] - samples[index - 1]) ** 2
        for index in range(1, len(samples))
    ) / (len(samples) - 1)
    return math.sqrt(diff_square / mean_square)


def zero_crossing_fraction(samples: tuple[int, ...]) -> float:
    return sum(
        (samples[index] < 0) != (samples[index - 1] < 0)
        for index in range(1, len(samples))
    ) / (len(samples) - 1)


def wave_metrics(audio: bytes, wave: Wave) -> tuple[float, float, float, float]:
    raw = audio[wave.sample_offset:wave.sample_offset + wave.length]
    count = len(raw) // 2
    correct = struct.unpack(f">{count}h", raw)
    legacy = struct.unpack(f"<{count}h", raw)
    return (
        normalized_roughness(correct),
        normalized_roughness(legacy),
        zero_crossing_fraction(correct),
        zero_crossing_fraction(legacy),
    )


def check_rom_oracle(rom_path: Path) -> dict[str, float | int | str]:
    rom = normalize_rom(rom_path.read_bytes())
    crc = (be_u32(rom, 0x10), be_u32(rom, 0x14))
    require(crc in LUTS,
            "RAW16 oracle supports only asset-identical US/PAL v80 ROMs; "
            f"got CRC1/CRC2 {crc[0]:08X}/{crc[1]:08X}")
    build, lut_start, asset_base = LUTS[crc]
    audio_table = rom_section(
        rom, lut_start, asset_base, ASSET_AUDIO_TABLE
    )
    audio = rom_section(rom, lut_start, asset_base, ASSET_AUDIO)
    require(len(audio_table) == 64,
            f"ASSET_AUDIO_TABLE is {len(audio_table)} bytes, expected 64")
    offsets = tuple(
        be_s32(audio_table, index)
        for index in range(0, len(audio_table), 4)
    )
    require(offsets == EXPECTED_AUDIO_OFFSETS,
            "ASSET_AUDIO_TABLE differs from the v80 topology this oracle "
            f"was derived for: {offsets!r}")

    sequence = BankParser(
        audio[:offsets[0]], audio, offsets[0], offsets[1], "music"
    ).parse()
    sound = BankParser(
        audio[offsets[1]:offsets[2]], audio, offsets[2], offsets[3], "SFX"
    ).parse()

    sequence_raw = [wave for wave in sequence.waves.values()
                    if wave.wave_type == 1]
    sound_raw = [wave for wave in sound.waves.values()
                 if wave.wave_type == 1]
    require(sequence.bank_count == 1, "music bank count changed")
    require(len(sequence.instrument_offsets) == 128,
            f"music instrument count={len(sequence.instrument_offsets)}, expected 128")
    require(sequence.sound_references == 297,
            f"music sound references={sequence.sound_references}, expected 297")
    require(len(sequence.waves) == 233,
            f"music unique waves={len(sequence.waves)}, expected 233")
    require(len(sequence_raw) == 25,
            f"music RAW16 waves={len(sequence_raw)}, expected 25")
    require(sum(w.wave_type == 0 for w in sequence.waves.values()) == 208,
            "music ADPCM wave count changed")
    require(sound.bank_count == 1, "SFX bank count changed")
    require(len(sound.instrument_offsets) == 1,
            f"SFX instrument count={len(sound.instrument_offsets)}, expected 1")
    require(sound.sound_references == 784,
            f"SFX sound references={sound.sound_references}, expected 784")
    require(len(sound.waves) == 444,
            f"SFX unique waves={len(sound.waves)}, expected 444")
    require(len(sound_raw) == 1,
            f"SFX RAW16 waves={len(sound_raw)}, expected 1")
    require(sum(w.wave_type == 0 for w in sound.waves.values()) == 443,
            "SFX ADPCM wave count changed")

    music_metrics = [(wave, wave_metrics(audio, wave))
                     for wave in sequence_raw]
    largest, largest_metrics = max(music_metrics,
                                   key=lambda item: item[0].length)
    correct_rough, legacy_rough, correct_zc, legacy_zc = largest_metrics
    ratio = legacy_rough / correct_rough
    require(largest.length == 18432,
            f"largest music RAW16 length={largest.length}, expected 18432")
    require(correct_rough < 0.10,
            f"correct largest-wave roughness {correct_rough:.6f} is implausibly high")
    require(ratio > 20.0,
            f"largest-wave endian discrimination only {ratio:.3f}x (need >20x)")
    require(legacy_zc > correct_zc * 20.0,
            f"largest-wave zero crossings do not discriminate endian modes "
            f"({correct_zc:.5f} vs {legacy_zc:.5f})")

    ratios = [metrics[1] / metrics[0] for _, metrics in music_metrics]
    require(all(value > 1.0 for value in ratios),
            "at least one music RAW16 wave is not smoother as big-endian PCM")
    require(sum(value > 1.10 for value in ratios) >= 24,
            "fewer than 24/25 music RAW16 waves have >1.10x endian discrimination")
    sfx_metrics = wave_metrics(audio, sound_raw[0])
    sfx_ratio = sfx_metrics[1] / sfx_metrics[0]
    require(sfx_ratio > 1.5,
            f"SFX RAW16 endian discrimination only {sfx_ratio:.3f}x")

    return {
        "build": build,
        "music_waves": len(sequence.waves),
        "music_raw": len(sequence_raw),
        "sfx_waves": len(sound.waves),
        "sfx_raw": len(sound_raw),
        "bass_correct_rough": correct_rough,
        "bass_legacy_rough": legacy_rough,
        "bass_ratio": ratio,
        "sfx_ratio": sfx_ratio,
    }


def function_body(source: str, name: str) -> str:
    for match in re.finditer(rf"\b{re.escape(name)}\s*\(", source):
        brace = source.find("{", match.end())
        semicolon = source.find(";", match.end())
        if brace < 0 or (semicolon >= 0 and semicolon < brace):
            continue
        depth = 0
        for index in range(brace, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[brace + 1:index]
    raise CheckError(f"could not extract C function {name}()")


def check_source_boundary() -> None:
    load_path = ROOT / "platform/audio_filters.c"
    mixer_path = ROOT / "platform/mixer.c"
    load = load_path.read_text(encoding="utf-8")
    mixer = mixer_path.read_text(encoding="utf-8")
    raw_body = function_body(load, "alRaw16Pull")
    adpcm_body = function_body(load, "_decodeChunk")
    raw_helper_calls = len(re.findall(r"\baLoadRaw16Buffer\s*\(", raw_body))
    raw_plain_calls = len(re.findall(r"\baLoadBuffer\s*\(", raw_body))
    adpcm_helper_calls = len(re.findall(r"\baLoadRaw16Buffer\s*\(", adpcm_body))
    adpcm_plain_calls = len(re.findall(r"\baLoadBuffer\s*\(", adpcm_body))
    require(raw_helper_calls == 3,
            f"alRaw16Pull has {raw_helper_calls} RAW16 loads, expected exactly 3")
    require(raw_plain_calls == 0,
            f"alRaw16Pull still has {raw_plain_calls} plain aLoadBuffer calls")
    require(adpcm_helper_calls == 0,
            "_decodeChunk incorrectly uses the word-swapping RAW16 loader")
    require(adpcm_plain_calls == 1,
            f"_decodeChunk has {adpcm_plain_calls} plain ADPCM loads, expected 1")
    require('getenv("MDKR_RAW16")' in mixer,
            "same-binary MDKR_RAW16 control is missing")
    require("GE007_DISABLE_RAW16_BYTESWAP" not in mixer,
            "stale GoldenEye RAW16 environment control remains")
    require("mdkr_copy_be16_to_host" in mixer,
            "mixer RAW16 path is not using the portable conversion primitive")


@dataclass(frozen=True)
class RuntimeTrace:
    mode: str
    loads: int
    byte_count: int
    first_block_sample: int


def parse_runtime_trace(output: str, expected_mode: str) -> RuntimeTrace:
    matches = RAW_TRACE_RE.findall(output)
    require(matches, f"{expected_mode}: no [AUDIO] raw16 summary")
    require(len(set(matches)) == 1,
            f"{expected_mode}: inconsistent duplicate RAW16 summaries: {matches}")
    mode, loads, byte_count, first = matches[-1]
    require(mode == expected_mode,
            f"{expected_mode}: runtime reported mode={mode}")
    require(first != "none",
            f"{expected_mode}: route never reached a RAW16 output block")
    return RuntimeTrace(mode, int(loads), int(byte_count), int(first))


def run_capture(
    binary: Path,
    rom: Path,
    script: Path,
    frames: int,
    mode: str,
    directory: Path,
    timeout: int,
) -> tuple[Path, RuntimeTrace, str]:
    directory.mkdir(parents=True, exist_ok=True)
    wav_path = directory / f"{mode}.wav"
    env = {
        key: value for key, value in os.environ.items()
        if not key.startswith("MDKR_")
    }
    env.update({
        "MDKR_AUDIO": "0",
        "MDKR_AUDIO_RMS": "1",
        "MDKR_AUDIO_DUMP": str(wav_path),
        "MDKR_AUDIO_REVERB": "0",
        "MDKR_RAW16": mode,
    })
    command = [
        str(binary),
        "--headless-frames", str(frames),
        "--input-script", str(script),
        "--rom", str(rom),
    ]
    proc = subprocess.run(
        command,
        cwd=directory,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
    )
    output = proc.stdout.decode("utf-8", "replace")
    require(proc.returncode == 0,
            f"{mode}: engine exited {proc.returncode}\n{output[-4000:]}")
    bad = BAD_RE.search(output)
    if bad is not None:
        raise CheckError(
            f"{mode}: runtime diagnostic {bad.group(0)!r}\n{output[-4000:]}"
        )
    require(wav_path.is_file() and wav_path.stat().st_size > 44,
            f"{mode}: no non-empty WAV was produced")
    return wav_path, parse_runtime_trace(output, mode), output


def load_wav(path: Path) -> array.array:
    with wave.open(str(path), "rb") as wav:
        got = (wav.getframerate(), wav.getnchannels(), wav.getsampwidth())
        require(got == (RATE, CHANNELS, WIDTH),
                f"{path.name}: WAV format={got}, expected {(RATE, CHANNELS, WIDTH)}")
        samples = array.array("h")
        samples.frombytes(wav.readframes(wav.getnframes()))
    if sys.byteorder == "big":
        samples.byteswap()
    return samples


def block_roughness(samples: array.array) -> float:
    require(len(samples) >= 4, "runtime block too short")
    mean_square = sum(value * value for value in samples) / len(samples)
    if mean_square == 0:
        return 0.0
    # Interleaved stereo: lag two compares adjacent frames of the same channel.
    diff_square = sum(
        (samples[index] - samples[index - CHANNELS]) ** 2
        for index in range(CHANNELS, len(samples))
    ) / (len(samples) - CHANNELS)
    return math.sqrt(diff_square / mean_square)


def compare_runtime(
    fixed_path: Path,
    fixed_trace: RuntimeTrace,
    legacy_path: Path,
    legacy_trace: RuntimeTrace,
) -> dict[str, float | int]:
    require(fixed_trace.loads > 0 and fixed_trace.byte_count > 0,
            "fixed arm reported no RAW16 work")
    require(
        (fixed_trace.loads, fixed_trace.byte_count,
         fixed_trace.first_block_sample) ==
        (legacy_trace.loads, legacy_trace.byte_count,
         legacy_trace.first_block_sample),
        "fixed/legacy reachability differs: "
        f"{fixed_trace} vs {legacy_trace}",
    )
    fixed = load_wav(fixed_path)
    legacy = load_wav(legacy_path)
    require(len(fixed) == len(legacy) and len(fixed) > 0,
            f"capture lengths differ: {len(fixed)} vs {len(legacy)} samples")
    boundary = fixed_trace.first_block_sample * CHANNELS
    require(0 < boundary < len(fixed),
            f"first RAW16 boundary {boundary} is outside {len(fixed)} samples")
    require(fixed[:boundary] == legacy[:boundary],
            "PCM differs before the first block whose synthesis loaded RAW16")

    first_difference = next(
        (index for index, pair in enumerate(zip(fixed, legacy))
         if pair[0] != pair[1]),
        None,
    )
    require(first_difference is not None,
            "fixed output equals MDKR_RAW16=legacy; the endian fix is absent")
    require(first_difference >= boundary,
            "fixed/legacy PCM diverged before RAW16 was reached")
    require(first_difference <= boundary + 8 * PUMP_FRAMES * CHANNELS,
            "RAW16 was loaded but did not affect output within eight pump blocks")

    post_fixed = fixed[boundary:]
    post_legacy = legacy[boundary:]
    differing = sum(
        left != right for left, right in zip(post_fixed, post_legacy)
    )
    difference_square = sum(
        (left - right) ** 2
        for left, right in zip(post_fixed, post_legacy)
    )
    difference_fraction = differing / len(post_fixed)
    difference_rms = math.sqrt(difference_square / len(post_fixed))
    require(difference_fraction > 0.10,
            f"only {difference_fraction:.2%} of post-RAW16 PCM differs (need >10%)")
    require(difference_rms > 500.0,
            f"post-RAW16 difference RMS {difference_rms:.1f} is too small")

    block_samples = PUMP_FRAMES * CHANNELS
    max_difference_rms = 0.0
    max_roughness_ratio = 0.0
    for start in range(boundary, len(fixed) - block_samples + 1,
                       block_samples):
        fixed_block = fixed[start:start + block_samples]
        legacy_block = legacy[start:start + block_samples]
        block_difference_rms = math.sqrt(
            sum((left - right) ** 2
                for left, right in zip(fixed_block, legacy_block))
            / block_samples
        )
        max_difference_rms = max(max_difference_rms, block_difference_rms)
        fixed_roughness = block_roughness(fixed_block)
        legacy_roughness = block_roughness(legacy_block)
        if fixed_roughness > 0:
            max_roughness_ratio = max(
                max_roughness_ratio,
                legacy_roughness / fixed_roughness,
            )
    require(max_difference_rms > 2500.0,
            f"strongest affected block difference RMS={max_difference_rms:.1f}, "
            "need >2500")
    require(max_roughness_ratio > 4.0,
            f"legacy's strongest block roughness ratio={max_roughness_ratio:.2f}x, "
            "need >4x")
    return {
        "loads": fixed_trace.loads,
        "bytes": fixed_trace.byte_count,
        "first_block": fixed_trace.first_block_sample,
        "first_difference": first_difference // CHANNELS,
        "difference_fraction": difference_fraction,
        "difference_rms": difference_rms,
        "max_block_difference_rms": max_difference_rms,
        "max_block_roughness_ratio": max_roughness_ratio,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR,
                        help="native build directory or mdkr64 executable")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--script", default=str(DEFAULT_SCRIPT))
    parser.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument(
        "--keep-audio",
        help="keep ROM-derived fixed/legacy WAVs under this directory",
    )
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    script = Path(args.script).expanduser()
    if not script.is_absolute():
        script = (ROOT / script).resolve()
    require(binary.is_file(), f"build executable not found: {binary}")
    require(rom.is_file(), f"ROM not found: {rom}")
    require(script.is_file(), f"input script not found: {script}")
    require(args.frames >= DEFAULT_FRAMES,
            f"--frames must be at least {DEFAULT_FRAMES} to reach the RAW16 fixture")

    print("1. source boundary: three RAW16 loads; one untouched ADPCM load")
    check_source_boundary()
    print("   ok  alRaw16Pull=3 converted, _decodeChunk=1 byte-stream")

    print("2. independent ROM-bank census and endian oracle")
    oracle = check_rom_oracle(rom)
    print(
        "   ok  %(build)s music=%(music_waves)d waves/%(music_raw)d RAW16; "
        "SFX=%(sfx_waves)d/%(sfx_raw)d; bass roughness "
        "%(bass_correct_rough).5f -> %(bass_legacy_rough).5f "
        "(%(bass_ratio).2fx); SFX %(sfx_ratio).2fx" % oracle
    )

    owned_temp = None
    if args.keep_audio:
        work = Path(args.keep_audio).expanduser().resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        owned_temp = tempfile.TemporaryDirectory(prefix="mdkr_raw16_")
        work = Path(owned_temp.name)

    print("3. same-binary fixed/legacy runtime A/B")
    try:
        fixed_path, fixed_trace, _ = run_capture(
            binary, rom, script, args.frames, "fixed",
            work / "fixed-run", args.timeout,
        )
        legacy_path, legacy_trace, _ = run_capture(
            binary, rom, script, args.frames, "legacy",
            work / "legacy-run", args.timeout,
        )
        runtime = compare_runtime(
            fixed_path, fixed_trace, legacy_path, legacy_trace
        )
        print(
            f"   ok  loads={runtime['loads']} bytes={runtime['bytes']} "
            f"first-block={runtime['first_block']} "
            f"first-diff={runtime['first_difference']} "
            f"post-diff={runtime['difference_fraction']:.2%} "
            f"diff-rms={runtime['difference_rms']:.1f} "
            f"max-block={runtime['max_block_difference_rms']:.1f} "
            f"legacy-roughness={runtime['max_block_roughness_ratio']:.2f}x"
        )
        if args.keep_audio:
            print(f"   kept ROM-derived captures under {work} — do not commit them")
    finally:
        if owned_temp is not None:
            owned_temp.cleanup()

    print("check_raw16_audio: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CheckError, OSError, subprocess.TimeoutExpired, wave.Error) as error:
        print(f"check_raw16_audio: FAIL — {error}", file=sys.stderr)
        raise SystemExit(1)
