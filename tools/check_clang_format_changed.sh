#!/usr/bin/env bash

set -euo pipefail

usage() {
	cat <<'EOF'
Usage:
  tools/check_clang_format_changed.sh --staged [--fix]
  tools/check_clang_format_changed.sh --base <rev> --head <rev> [--fix] [--no-merge-base]

Checks changed Source/ and test/ C/C++ files with clang-format 18.
By default, --base/--head mode compares from merge-base(base, head) to head,
which matches PR intent even when the branch is behind its base branch.

Options:
  --base <rev>       Base revision for branch checks.
  --head <rev>       Head revision for branch checks.
  --staged           Check staged files for pre-commit usage.
  --fix              Rewrite files in place. In --staged mode, re-stage them.
  --no-merge-base    Compare base..head directly.
  -h, --help         Show this help.

Set CLANG_FORMAT=/path/to/clang-format-18 to force a local formatter.
If no local clang-format 18 is found, Docker is used with
ghcr.io/jidicula/clang-format:18.
EOF
}

base=
head=
staged=0
fix=0
use_merge_base=1

while (($# > 0)); do
	case "$1" in
		--base)
			base="${2:-}"
			shift 2
			;;
		--head)
			head="${2:-}"
			shift 2
			;;
		--staged)
			staged=1
			shift
			;;
		--fix)
			fix=1
			shift
			;;
		--no-merge-base)
			use_merge_base=0
			shift
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "Unknown argument: $1" >&2
			usage >&2
			exit 2
			;;
	esac
done

if ((staged)); then
	if [[ -n "$base" || -n "$head" ]]; then
		echo "--staged cannot be combined with --base/--head." >&2
		exit 2
	fi
else
	if [[ -z "$base" || -z "$head" ]]; then
		echo "Provide --staged or both --base and --head." >&2
		usage >&2
		exit 2
	fi
fi

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

is_cxx_file() {
	case "$1" in
		*.c|*.C|*.cc|*.cpp|*.cxx|*.c++|*.h|*.H|*.hh|*.hpp|*.hxx|*.h++|*.ino|*.pde|*.proto|*.cu)
			return 0
			;;
		*)
			return 1
			;;
	esac
}

collect_files() {
	local compare_base

	if ((staged)); then
		git diff --cached --name-only --diff-filter=ACMRT -- Source test
		return
	fi

	compare_base="$base"
	if ((use_merge_base)); then
		compare_base="$(git merge-base "$base" "$head")"
	fi
	git diff --name-only --diff-filter=ACMRT "$compare_base" "$head" -- Source test
}

files=()
while IFS= read -r file; do
	files+=("$file")
done < <(
	collect_files | while IFS= read -r file; do
		if is_cxx_file "$file"; then
			printf '%s\n' "$file"
		fi
	done
)

if ((${#files[@]} == 0)); then
	echo "No changed C/C++ files to format-check."
	exit 0
fi

if ((staged && fix)); then
	unstaged=()
	for file in "${files[@]}"; do
		if ! git diff --quiet -- "$file"; then
			unstaged+=("$file")
		fi
	done
	if ((${#unstaged[@]} > 0)); then
		echo "Cannot auto-format staged files that also have unstaged edits:" >&2
		printf '  %s\n' "${unstaged[@]}" >&2
		echo "Stage or stash those edits, then commit again." >&2
		exit 1
	fi
fi

local_clang_format=
if [[ -n "${CLANG_FORMAT:-}" ]]; then
	local_clang_format="$CLANG_FORMAT"
elif [[ -x /opt/homebrew/opt/llvm@18/bin/clang-format ]]; then
	local_clang_format=/opt/homebrew/opt/llvm@18/bin/clang-format
elif command -v clang-format-18 >/dev/null 2>&1; then
	local_clang_format="$(command -v clang-format-18)"
elif command -v clang-format >/dev/null 2>&1; then
	local_clang_format="$(command -v clang-format)"
fi

use_docker=1
if [[ -n "$local_clang_format" ]]; then
	version="$("$local_clang_format" --version 2>/dev/null || true)"
	if [[ "$version" == *"version 18."* || "$version" == *"version 18 "* ]]; then
		use_docker=0
	elif [[ -n "${CLANG_FORMAT:-}" ]]; then
		echo "Expected clang-format 18 for DiabloNext formatting checks." >&2
		echo "Found: ${version:-$local_clang_format}" >&2
		exit 1
	fi
fi

run_clang_format() {
	if ((use_docker)); then
		if ! command -v docker >/dev/null 2>&1; then
			echo "clang-format 18 was not found and Docker is unavailable." >&2
			echo "Install LLVM 18, set CLANG_FORMAT, or install Docker." >&2
			exit 1
		fi
		docker run --rm \
			--user "$(id -u):$(id -g)" \
			--volume "${repo_root}:${repo_root}" \
			--workdir "$repo_root" \
			ghcr.io/jidicula/clang-format:18 \
			"$@"
	else
		"$local_clang_format" "$@"
	fi
}

printf 'Checking clang-format for changed files:\n'
printf '  %s\n' "${files[@]}"

if ((fix)); then
	run_clang_format -i --style=file --fallback-style=llvm "${files[@]}"
	if ((staged)); then
		git add -- "${files[@]}"
	fi
fi

run_clang_format --dry-run --Werror --style=file --fallback-style=llvm "${files[@]}"
