# ADR-005 — Workspace path and repo isolation

**Status:** accepted (2026-09-07)

## Decision

Repo root: `/mnt/storage/projetos/XCloud360/` (renamed from `/mnt/storage/projetos/#X360/`).

Reasons for rename:
- `#` is shell comment leader — breaks unquoted paths in scripts/Makefiles/CMakeLists.
- URLs with `#` require `%23` encoding — friction for future git remote.

Isolated `.git` inside `XCloud360/`. The parent directory `/mnt/storage/projetos/` is itself a git repo (Sergio's personal client work); the local `.git` shadows the parent for anything inside this tree.

## Consequences

- No accidental commits into Sergio's personal repo.
- Remote destination (GitHub/GitLab/self-hosted) to be decided before first push. Not blocking local work.

## Related

- Spec §38
