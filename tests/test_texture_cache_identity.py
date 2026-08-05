#!/usr/bin/env python3
"""Keep every texture-upload input in the native frontend cache identity.

The cache reuses backend texture objects across draws. Two tiles may point at
the same source address and logical dimensions while differing in source row
pitch, loaded span, mipmap policy, or cutout reduction. Those inputs produce
different uploaded bytes and therefore must never hit the same cache entry.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
GFX_PC = ROOT / "platform" / "fast3d" / "gfx_pc_dkr.c"
KEY_HEADER = ROOT / "platform" / "fast3d" / "gfx_texture_cache_key.h"

# Upload inputs that must never leave the identity. The complete field set is
# derived from the struct itself, so a newly declared member fails the equality
# and bind assertions until it is acknowledged there too.
REQUIRED_FIELDS = (
    "addr",
    "source_line_bytes",
    "source_size_bytes",
    "palette_hash",
    "width",
    "height",
    "fmt",
    "siz",
    "palette",
    "line_swapped",
    "font_remastered",
    "mipmaps",
    "cutout",
)


def struct_field_names(source: str, name: str) -> tuple[str, ...]:
    """Return the declared field identifiers of a plain C struct, in order."""

    match = re.search(
        rf"struct\s+{re.escape(name)}\s*\{{(?P<body>.*?)\}}\s*;", source, re.S
    )
    if match is None:
        raise AssertionError(f"missing struct {name}")
    body = re.sub(r"/\*.*?\*/", " ", match.group("body"), flags=re.S)
    body = re.sub(r"//[^\n]*", " ", body)
    if "{" in body:
        raise AssertionError(f"struct {name} is no longer a flat declaration")
    fields: list[str] = []
    for declaration in body.split(";"):
        if not declaration.strip():
            continue
        declarators = declaration.split(",")
        head = re.findall(r"[A-Za-z_]\w*", declarators[0])
        if len(head) < 2:
            raise AssertionError(f"unparsed declaration in {name}: {declaration}")
        fields.append(head[-1])
        for extra in declarators[1:]:
            names = re.findall(r"[A-Za-z_]\w*", extra)
            if len(names) != 1:
                raise AssertionError(f"unparsed declarator in {name}: {extra}")
            fields.append(names[0])
    return tuple(fields)


def function_body(source: str, name: str) -> str:
    """Return a C function body while ignoring braces in comments/literals."""

    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source, re.S)
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = source.find("{", match.start())
    depth = 0
    index = start
    state = "code"
    while index < len(source):
        char = source[index]
        pair = source[index:index + 2]
        if state == "code":
            if pair == "/*":
                state = "block-comment"
                index += 2
                continue
            if pair == "//":
                state = "line-comment"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[start:index + 1]
        elif state == "block-comment" and pair == "*/":
            state = "code"
            index += 2
            continue
        elif state == "line-comment" and char == "\n":
            state = "code"
        elif state in ("string", "character"):
            if char == "\\":
                index += 2
                continue
            if (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        index += 1
    raise AssertionError(f"unterminated function {name}")


class TextureCacheIdentityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = GFX_PC.read_text(encoding="utf-8")
        cls.key_header = KEY_HEADER.read_text(encoding="utf-8")
        cls.key_fields = struct_field_names(cls.key_header, "DkrTexCacheKey")

    def test_key_declares_every_upload_input(self) -> None:
        for field in REQUIRED_FIELDS:
            self.assertIn(field, self.key_fields, field)

    def test_key_equality_compares_every_field(self) -> None:
        body = function_body(self.key_header, "dkr_texcache_key_equal")
        for field in self.key_fields:
            self.assertRegex(
                body,
                rf"left->{field}\s*==\s*right->{field}",
                field,
            )

    def test_bind_builds_and_uses_the_complete_key(self) -> None:
        body = function_body(self.source, "dkr_bind_tile")
        for field in self.key_fields:
            self.assertRegex(body, rf"\.{field}\s*=", field)
        self.assertRegex(
            body,
            r"\.source_size_bytes\s*=\s*source_size_bytes",
        )
        self.assertRegex(
            body,
            r"\.source_line_bytes\s*=\s*source_line_bytes",
        )
        self.assertRegex(
            body,
            r"\.mipmaps\s*=\s*g_pcMipmaps[\s\S]*?upload_texture_mipped",
        )
        self.assertRegex(body, r"\.cutout\s*=\s*cutout")
        self.assertIn(
            "dkr_texcache_key_equal(&tex_cache[i].key, &key)", body
        )
        self.assertRegex(body, r"\.key\s*=\s*key")

    def test_decoder_and_key_share_pitch_resolution(self) -> None:
        upload = function_body(self.source, "dkr_upload_tile_texture")
        bind = function_body(self.source, "dkr_bind_tile")
        call = "dkr_tile_source_line_bytes(td, source_size_bytes)"
        self.assertIn(call, upload)
        self.assertIn(call, bind)

    def test_invalidation_uses_the_keyed_source_span(self) -> None:
        body = function_body(self.source, "gfx_dkr_texcache_invalidate_range")
        self.assertIn("tex_cache[i].key.addr", body)
        self.assertIn("tex_cache[i].key.source_size_bytes", body)

    def test_slot_reuse_forgets_bound_sampler_state(self) -> None:
        body = function_body(self.source, "dkr_bind_tile")
        forget = body.find("dkr_forget_texture_binding(tid)")
        select = body.find("gfx_rapi->select_texture(unit, tid)")
        upload = body.find("dkr_upload_tile_texture(td, cutout")
        self.assertGreaterEqual(forget, 0)
        self.assertGreater(select, forget)
        self.assertGreater(upload, select)


if __name__ == "__main__":
    unittest.main()
