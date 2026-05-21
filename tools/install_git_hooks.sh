#!/usr/bin/env bash

set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
git -C "$repo_root" config core.hooksPath .githooks

echo "Configured Git hooks for $repo_root"
echo "Pre-commit will run tools/check_clang_format_changed.sh --staged --fix"
