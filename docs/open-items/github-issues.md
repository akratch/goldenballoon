# Public issue acceptance

Open issue inventory checked on 2026-10-07. Implementation, final-build testing,
and public availability are separate states. A passing test for one symptom
cannot resolve a broader feature request. Follow the [release checklist](../RELEASE_CHECKLIST.md)
for final candidate and packaged-platform testing.

| Issue | Current implementation | Remaining acceptance |
|---|---|---|
| [#58: bonus portraits and NDS content](https://github.com/akratch/goldenballoon/issues/58) | Portrait support shipped in 1.7.0. | Additional characters and tracks requested in the same issue are separate future work. |
| [#65: custom tracks](https://github.com/akratch/goldenballoon/issues/65) | Dormant loader and admission groundwork; no shipping importer or authored-course workflow. | The [custom-track integration blockers](../DKR_R_CUSTOM_TRACK_PARITY_REVIEW.md#19-integration-blockers) must be resolved before a future release. |
| [#66: texture-pack stutter and Enhanced 60 audio](https://github.com/akratch/goldenballoon/issues/66) | Texture upload and Enhanced 60 audio fixes are in the 1.8 candidate. | Final-build texture-pack timing and music coverage; reproduce on the reporting configuration. Do not promise elimination of every source of stutter. |
| [#67: picture-frame finish banners](https://github.com/akratch/goldenballoon/issues/67) | The candidate corrects finish-banner clipping with Widescreen HUD. | Packaged Windows/NVIDIA testing against the reported Windows 11 and 4K-display case. macOS tests cannot establish that hardware result. |
| [#68: feature-request label](https://github.com/akratch/goldenballoon/issues/68) | Separate feature-request issue template added. | Public label creation and relabeling remain repository-maintenance actions. |

The launcher-skip, opponent-skill, split-screen and Future Fun Land changes from
1.7.0 remain covered by their regression checks. Earlier candidate test results
do not substitute for testing the exact 1.8 source and packaged builds.
