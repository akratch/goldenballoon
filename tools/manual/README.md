# Operator-run tools

These scripts are run by hand, by whoever owns the GitHub repository. They need
an authenticated `gh` session and they read or change repository settings, so no
CI job or check invokes them.

- `configure_github_launch_settings.sh` — apply or preview the recommended
  public-repository settings.
- `check_github_launch_ready.sh` — verify GitHub-side public repository hygiene.
  `tools/ci/check_release_ready.sh` asserts this file is present and executable,
  but never runs it.
