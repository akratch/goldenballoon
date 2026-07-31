#!/usr/bin/env python3
"""Check GitHub-visible text for stale commit references.

After a public-history rewrite, issue/PR bodies, review summaries, milestones,
and comments can still mention old commit SHAs. If GitHub can resolve those
SHAs, public readers can follow them into pre-public history. This guard scans
public GitHub text and fails on commit-like tokens that resolve to commits
outside the current local history.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from typing import Any


COMMIT_LIKE_RE = re.compile(r"(?<![0-9A-Fa-f])([0-9A-Fa-f]{7,12}|[0-9A-Fa-f]{40})(?![0-9A-Fa-f])")


@dataclass(frozen=True)
class TextItem:
    kind: str
    url: str
    label: str
    text: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Scan GitHub-visible text for commit refs outside current history."
    )
    parser.add_argument("--repo", required=True, help="GitHub repository, OWNER/REPO")
    parser.add_argument(
        "--max-candidates",
        type=int,
        default=250,
        help="maximum distinct non-current commit-like tokens to resolve via GitHub",
    )
    return parser.parse_args()


def run_text(args: list[str]) -> str:
    result = subprocess.run(
        args,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return result.stdout


def run_json_lines(args: list[str]) -> list[dict[str, Any]]:
    output = run_text(args)
    return [json.loads(line) for line in output.splitlines() if line.strip()]


def reachable_shas() -> set[str]:
    return set(run_text(["git", "rev-list", "HEAD"]).splitlines())


def plausible_commit_token(token: str) -> bool:
    if token.isdigit() or not any(char.isdigit() for char in token):
        return False
    lowered = token.lower()
    return not lowered.startswith(
        (
            "000",
            "800",
            "801",
            "802",
            "803",
            "804",
            "805",
            "806",
            "807",
            "808",
            "809",
            "80a",
            "80b",
            "80c",
            "80d",
            "80e",
            "80f",
            "ed8",
        )
    )


def token_is_reachable(token: str, reachable: set[str]) -> bool:
    lowered = token.lower()
    return any(sha.startswith(lowered) or lowered.startswith(sha) for sha in reachable)


def rest_items(endpoint: str, kind: str) -> list[TextItem]:
    try:
        rows = run_json_lines(["gh", "api", "--paginate", endpoint, "--jq", ".[] | @json"])
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"could not fetch {kind} text: {exc.stderr.strip() or exc}"
        ) from exc

    items: list[TextItem] = []
    for row in rows:
        title = row.get("title") or ""
        body = row.get("body") or ""
        label = title or row.get("user", {}).get("login") or str(row.get("id", "unknown"))
        items.append(
            TextItem(
                kind=kind,
                url=row.get("html_url") or row.get("url") or "",
                label=label,
                text=f"{title}\n{body}",
            )
        )
    return items


def repository_items(repo: str) -> list[TextItem]:
    try:
        row = json.loads(run_text(["gh", "api", f"repos/{repo}", "--jq", "@json"]))
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"could not fetch repository metadata: {exc.stderr.strip() or exc}"
        ) from exc

    full_name = row.get("full_name") or repo
    return [
        TextItem(
            kind="repository",
            url=row.get("html_url") or f"https://github.com/{repo}",
            label=full_name,
            text="\n".join(
                (
                    full_name,
                    row.get("description") or "",
                    row.get("homepage") or "",
                )
            ),
        )
    ]


def label_items(repo: str) -> list[TextItem]:
    try:
        rows = run_json_lines(
            ["gh", "api", "--paginate", f"repos/{repo}/labels?per_page=100", "--jq", ".[] | @json"]
        )
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"could not fetch label text: {exc.stderr.strip() or exc}"
        ) from exc

    items: list[TextItem] = []
    for row in rows:
        name = row.get("name") or ""
        items.append(
            TextItem(
                kind="label",
                url=row.get("url") or f"https://github.com/{repo}/labels",
                label=name,
                text=f"{name}\n{row.get('description') or ''}",
            )
        )
    return items


def release_items(repo: str) -> list[TextItem]:
    try:
        rows = run_json_lines(
            ["gh", "api", "--paginate", f"repos/{repo}/releases?per_page=100", "--jq", ".[] | @json"]
        )
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"could not fetch release text: {exc.stderr.strip() or exc}"
        ) from exc

    items: list[TextItem] = []
    for row in rows:
        tag_name = row.get("tag_name") or ""
        name = row.get("name") or ""
        body = row.get("body") or ""
        asset_names = "\n".join(asset.get("name") or "" for asset in row.get("assets") or [])
        label = name or tag_name or str(row.get("id", "unknown"))
        items.append(
            TextItem(
                kind="release",
                url=row.get("html_url") or f"https://github.com/{repo}/releases",
                label=label,
                text=f"{tag_name}\n{name}\n{body}\n{asset_names}",
            )
        )
    return items


def milestone_items(repo: str) -> list[TextItem]:
    try:
        rows = run_json_lines(
            [
                "gh",
                "api",
                "--paginate",
                f"repos/{repo}/milestones?state=all&per_page=100",
                "--jq",
                ".[] | @json",
            ]
        )
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"could not fetch milestone text: {exc.stderr.strip() or exc}"
        ) from exc

    items: list[TextItem] = []
    for row in rows:
        title = row.get("title") or ""
        description = row.get("description") or ""
        items.append(
            TextItem(
                kind="milestone",
                url=(
                    row.get("html_url")
                    or row.get("url")
                    or f"https://github.com/{repo}/milestones"
                ),
                label=title or str(row.get("id", "unknown")),
                text=f"{title}\n{description}",
            )
        )
    return items


def commit_comment_items(repo: str) -> list[TextItem]:
    try:
        rows = run_json_lines(
            [
                "gh",
                "api",
                "--paginate",
                f"repos/{repo}/comments?per_page=100",
                "--jq",
                ".[] | @json",
            ]
        )
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"could not fetch commit comments: {exc.stderr.strip() or exc}"
        ) from exc

    items: list[TextItem] = []
    for row in rows:
        author = row.get("user", {}).get("login") or str(row.get("id", "unknown"))
        items.append(
            TextItem(
                kind="commit-comment",
                url=row.get("html_url") or row.get("url") or "",
                label=author,
                text=row.get("body") or "",
            )
        )
    return items


def pull_review_items(repo: str) -> list[TextItem]:
    try:
        pull_rows = run_json_lines(
            [
                "gh",
                "api",
                "--paginate",
                f"repos/{repo}/pulls?state=all&per_page=100",
                "--jq",
                ".[] | @json",
            ]
        )
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            "could not fetch pull requests for review summaries: "
            f"{exc.stderr.strip() or exc}"
        ) from exc

    items: list[TextItem] = []
    for pull in pull_rows:
        number = pull.get("number")
        if number is None:
            continue
        try:
            review_rows = run_json_lines(
                [
                    "gh",
                    "api",
                    "--paginate",
                    f"repos/{repo}/pulls/{number}/reviews?per_page=100",
                    "--jq",
                    ".[] | @json",
                ]
        )
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(
                f"could not fetch review summaries for PR #{number}: "
                f"{exc.stderr.strip() or exc}"
            ) from exc

        for review in review_rows:
            author = review.get("user", {}).get("login") or str(
                review.get("id", "unknown")
            )
            body = review.get("body") or ""
            items.append(
                TextItem(
                    kind="pr-review",
                    url=(
                        review.get("html_url")
                        or review.get("pull_request_url")
                        or pull.get("html_url")
                        or ""
                    ),
                    label=author,
                    text=body,
                )
            )
    return items


def discussion_items(repo: str) -> tuple[list[TextItem], list[str]]:
    owner, name = repo.split("/", 1)
    query = """
      query($owner: String!, $name: String!, $endCursor: String) {
        repository(owner: $owner, name: $name) {
          discussions(first: 100, after: $endCursor, orderBy: {field: UPDATED_AT, direction: DESC}) {
            pageInfo { hasNextPage endCursor }
            nodes {
              title
              body
              url
              comments(first: 100) {
                totalCount
                nodes {
                  body
                  url
                  author { login }
                }
              }
            }
          }
        }
      }
    """
    try:
        rows = run_json_lines(
            [
                "gh",
                "api",
                "graphql",
                "--paginate",
                "-F",
                f"owner={owner}",
                "-F",
                f"name={name}",
                "-f",
                f"query={query}",
                "--jq",
                ".data.repository.discussions.nodes[]? | @json",
            ]
        )
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(exc.stderr.strip() or "could not fetch discussions") from exc

    items: list[TextItem] = []
    incomplete: list[str] = []
    for discussion in rows:
        title = discussion.get("title") or ""
        items.append(
            TextItem(
                kind="discussion",
                url=discussion.get("url") or "",
                label=title,
                text=f"{title}\n{discussion.get('body') or ''}",
            )
        )
        comments = discussion.get("comments") or {}
        nodes = comments.get("nodes") or []
        total_count = int(comments.get("totalCount") or 0)
        if total_count > len(nodes):
            incomplete.append(
                f"{discussion.get('url') or title}: loaded {len(nodes)} of {total_count} comments"
            )
        for comment in nodes:
            author = (comment.get("author") or {}).get("login") or "unknown"
            items.append(
                TextItem(
                    kind="discussion-comment",
                    url=comment.get("url") or discussion.get("url") or "",
                    label=author,
                    text=comment.get("body") or "",
                )
            )
    return items, incomplete


def fetch_items(repo: str) -> tuple[list[TextItem], list[str]]:
    items: list[TextItem] = []
    items.extend(repository_items(repo))
    items.extend(label_items(repo))
    items.extend(milestone_items(repo))
    items.extend(release_items(repo))
    items.extend(rest_items(f"repos/{repo}/issues?state=all&per_page=100", "issue-or-pr"))
    items.extend(rest_items(f"repos/{repo}/issues/comments?per_page=100", "issue-comment"))
    items.extend(commit_comment_items(repo))
    items.extend(rest_items(f"repos/{repo}/pulls/comments?per_page=100", "pr-review-comment"))
    items.extend(pull_review_items(repo))
    discussions, incomplete = discussion_items(repo)
    items.extend(discussions)
    return items, incomplete


def resolve_commit(repo: str, token: str) -> str | None:
    result = subprocess.run(
        ["gh", "api", f"repos/{repo}/commits/{token}", "--jq", ".sha"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    if result.returncode != 0:
        return None
    sha = result.stdout.strip().lower()
    return sha or None


def main() -> int:
    args = parse_args()
    try:
        reachable = reachable_shas()
        items, incomplete = fetch_items(args.repo)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"FAIL: could not scan GitHub-visible text: {exc}", file=sys.stderr)
        return 1

    token_sources: dict[str, list[TextItem]] = {}
    for item in items:
        for match in COMMIT_LIKE_RE.finditer(item.text):
            token = match.group(1).lower()
            if plausible_commit_token(token) and not token_is_reachable(token, reachable):
                token_sources.setdefault(token, []).append(item)

    if len(token_sources) > args.max_candidates:
        print(
            f"FAIL: {len(token_sources)} distinct non-current commit-like tokens found; "
            f"limit is {args.max_candidates}. Clean the GitHub text surface or raise the limit deliberately.",
            file=sys.stderr,
        )
        for token in sorted(token_sources)[: args.max_candidates]:
            first = token_sources[token][0]
            print(f"  - {token}: {first.kind} {first.url}", file=sys.stderr)
        return 1

    stale: list[tuple[str, str, list[TextItem]]] = []
    for token, sources in sorted(token_sources.items()):
        resolved = resolve_commit(args.repo, token)
        if resolved and resolved not in reachable:
            stale.append((token, resolved, sources))

    if incomplete:
        print("FAIL: discussion comment scan was incomplete.", file=sys.stderr)
        for item in incomplete:
            print(f"  - {item}", file=sys.stderr)
        return 1

    if stale:
        print("FAIL: GitHub-visible text references commits outside current history.", file=sys.stderr)
        for token, resolved, sources in stale:
            first = sources[0]
            print(
                f"  - {token} -> {resolved[:12]} in {first.kind}: {first.url}",
                file=sys.stderr,
            )
        return 1

    print(
        f"PASS: scanned {len(items)} GitHub text item(s); "
        "no resolvable stale commit references found"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
