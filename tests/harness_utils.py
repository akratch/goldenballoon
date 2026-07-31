"""Shared, side-effect-free helpers for the headless regression checks."""

from __future__ import annotations

import os
from pathlib import Path


def resolve_binary(build: str | os.PathLike[str]) -> str:
    """Resolve ``--build`` consistently from either a directory or executable.

    Every behavioural check accepts the same contract:

    - ``--build build-rel`` resolves to ``build-rel/mdkr64``;
    - ``--build build-rel/mdkr64`` remains that executable.

    Existence is intentionally checked by each caller so its existing failure
    message and any additional input validation remain intact.
    """

    path = Path(build).expanduser()
    if path.is_dir():
        path = path / "mdkr64"
    return str(path)
