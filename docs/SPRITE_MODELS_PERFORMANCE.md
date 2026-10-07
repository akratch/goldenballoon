# World sprite model performance

Optional plant and item meshes cost more to draw than the original billboards.
F2 toggles loaded replacements for comparison; collision, pickups and simulation
remain authoritative. WebGPU draws supported models; OpenGL retains billboards.

## Shared resources

The loader shares decoded atlases within the selected pack, and immutable atlas
identities let WebGPU reuse textures across meshes. Reloading content gives new
atlases new identities. Mesh and atlas resources are retired with their cache
generation; matching filenames in different packs do not merge their artwork.

Upload counters describe cumulative work, not current process memory or driver
allocation. Compare them alongside the content-memory diagnostics and cache
lifetime checks. A reduction in upload bytes does not establish higher FPS.

## Content costs

- Author genuinely reduced far meshes. Reusing near geometry at distance saves
  no geometry work.
- Budget triangles for the maximum number of simultaneous visible instances,
  including split-screen. A detailed palm can be much more expensive than its
  original billboard.
- Each visible model requires a draw and material state. Interpolated presents
  may replay it, and two-sided alpha cutouts add overdraw.
- Single-level atlases can alias and consume excess bandwidth at distance.
  Mip generation needs separate checks of alpha-cutout coverage.

Measure fixed camera routes at the same drawable size, presentation policy and
pack selection. Record GPU frame times and simulation hashes with models on and
off. Unpaced hidden-run elapsed time is not a display-FPS measurement.

## Regression checks

```bash
ctest --test-dir build-rel --output-on-failure \
  -R 'sprite_model|presentation_snapshot|presentation_packet'
python3 tests/check_sprite_models_gpu.py --build build-rel --rom /path/to/local.z64
```

The tests generate their own content. They cover bounded mesh parsing, pack
isolation, incomplete-family fallback, shared resource ownership, material
behavior, replay and GPU constructor failures. Startup reports `[MODELS]`
atlas/decode counts; shutdown's `[WGPU-MODELS]` reports upload and reuse work.
Private artwork and captures are not distributed with the project.
