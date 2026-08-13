---
name: Platform validation report
about: Report a clean build/test/run result for a supported platform
title: "Validation: platform "
labels: validation, build
---

**Do not attach ROMs, extracted assets, save files, screenshots, video, or
audio captures. Text logs are fine.**

### Platform
- OS / version:
- GPU / driver, if an interactive run was tested:
- Commit SHA:

### Tool versions
Paste the relevant text output.

```sh
cmake --version
cc --version
python3 --version
pkg-config --modversion sdl2 || pkgconf --modversion sdl2
```

### Commands tested
Mark each as pass/fail and paste concise text logs for failures.

- [ ] `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- [ ] `cmake --build build -j`
- [ ] `python3 tools/run_checks.py`
- [ ] `./build/mdkr64 --rom /path/to/baserom.us.v80.z64`

Automation windows render hidden or ordered behind the desktop by design (set
`MDKR_TEST_VISIBLE_HEADLESS=1` to watch one), and the release gate is the
complete suite wherever it runs. The ROM-free lane on its own is
`python3 tools/run_checks.py --only rom_free_units`.

### Notes
Mention missing dependencies, documentation gaps, runtime issues, or anything
that should become a separate bug report.
