#!/bin/bash
#
# clean_git_appledouble.sh
#
# This project's working copy lives on a non-native filesystem volume, which
# causes macOS (Finder/Spotlight/backup indexing) to periodically spawn
# AppleDouble sidecar files ("._foo" next to "foo") -- including inside
# .git/ itself. Those "._pack-*.idx"/"._pack-*.pack" files are NOT real git
# data, but git's own consistency checks trip over them:
#   error: non-monotonic index .git/objects/pack/._pack-<hash>.idx
# git operations still succeed despite the noisy stderr (verified: the real
# pack-*.idx/.pack/.rev files are untouched, and `git fsck` shows no actual
# corruption -- only "dangling" entries, which are normal/harmless), but the
# warnings are confusing and this recurs every time git creates new pack
# files (gc, merge, push, ...).
#
# Usage: bash Scripts/clean_git_appledouble.sh [--dry-run]
#
# Safe to run anytime. Only deletes files matching "._*" inside .git/ --
# never touches real git objects, refs, or any tracked project file.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

DRY_RUN=0
if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN=1
fi

FOUND=$(find .git -name "._*" -type f || true)

if [[ -z "$FOUND" ]]; then
    echo "No AppleDouble junk found in .git/."
    exit 0
fi

COUNT=$(echo "$FOUND" | grep -c .)
echo "Found $COUNT AppleDouble junk file(s) in .git/:"
echo "$FOUND"

if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "(dry-run: not deleting)"
    exit 0
fi

find .git -name "._*" -type f -delete
echo "Deleted $COUNT file(s)."

echo "Verifying repository integrity..."
FSCK_ISSUES=$(git fsck --no-progress 2>&1 | grep -vi "dangling" || true)
if [[ -n "$FSCK_ISSUES" ]]; then
    echo "WARNING: git fsck reported issues after cleanup:"
    echo "$FSCK_ISSUES"
    exit 1
fi
echo "OK: git fsck clean (only normal dangling objects, if any)."
