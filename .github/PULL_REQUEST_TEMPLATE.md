<!-- Thanks for contributing to mdkr64 (Golden Balloon)! -->

## What does this PR do?

<!-- A short description of the change and why. -->

## Checklist

- [ ] I did **not** commit any ROM or ROM-derived data (textures, audio, models,
      fonts, level/scene data, image/animation tables, logos — in `.bin` *or* as
      inline data arrays). See [DISCLAIMER.md](../DISCLAIMER.md) and
      [CONTRIBUTING.md](../CONTRIBUTING.md).
- [ ] I re-read my diff (`git diff --stat`) for accidental data blobs.
- [ ] I did not add private plans, handoffs, agent transcripts, personal paths,
      or a personal author/committer email; `python3 tools/check_public_surface.py
      --staged` passes.
- [ ] I ran the relevant validation on a dedicated test desktop
      (`MDKR_DEDICATED_TEST_DESKTOP=1 python3 tools/run_checks.py`, or the
      narrower `--only <name>` selection — see the workstation-safe
      `tools/run_checks.py --list`). Compiled, native-app, and browser evidence
      used its additional explicit class opt-in.
- [ ] Release/archive changes also pass every gate in
      [docs/RELEASE_CHECKLIST.md](../docs/RELEASE_CHECKLIST.md), including
      `tools/check_clean_room.sh` and `tools/check_no_rom.sh`.
- [ ] Code follows the existing style (`.clang-format` / `.editorconfig`).
- [ ] Game-code behavior changes (if any) are justified; port-only fixes live in
      `platform/`.
