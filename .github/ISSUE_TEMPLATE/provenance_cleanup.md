---
name: Provenance cleanup
about: Track SDK, asset-safety, or licensing cleanup work
title: "Provenance: "
labels: provenance
---

### Scope
What file, dependency, SDK surface, or asset-safety path should be cleaned up?

### Current status
Link the relevant source/docs and summarize what is still in-tree or still
needed by the build.

### Desired end state
Describe the clean-room replacement, removal, or documentation update.

### Validation
List the commands or checks that should pass after the cleanup.

```sh
tools/check_clean_room.sh
MDKR64_HIDDEN=1 MDKR_AUDIO=0 \
  ctest --test-dir build --output-on-failure -j1 -LE 'gpu|app_process|browser'
```
