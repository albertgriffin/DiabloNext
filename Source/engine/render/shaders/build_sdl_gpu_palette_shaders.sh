#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")"

if ! command -v shadercross >/dev/null 2>&1; then
	echo "shadercross was not found. Install SDL_shadercross to regenerate SDL_GPU palette shader assets." >&2
	exit 1
fi

make_header() {
	local input="$1"
	local symbol="$2"
	local output="$3"

	xxd -i -n "$symbol" "$input" \
		| sed \
			-e 's/^unsigned /static const unsigned /g' \
			-e 's/^unsigned int/static const unsigned int/g' \
		> "$output"
}

for stage in vert frag; do
	input="sdl_gpu_palette.${stage}.hlsl"

	shadercross "$input" -o "sdl_gpu_palette.${stage}.spv"
	shadercross "$input" -o "sdl_gpu_palette.${stage}.msl"
	shadercross "$input" -o "sdl_gpu_palette.${stage}.dxil"

	make_header "sdl_gpu_palette.${stage}.spv" "sdl_gpu_palette_${stage}_spv" "sdl_gpu_palette.${stage}.spv.h"
	make_header "sdl_gpu_palette.${stage}.msl" "sdl_gpu_palette_${stage}_msl" "sdl_gpu_palette.${stage}.msl.h"
	make_header "sdl_gpu_palette.${stage}.dxil" "sdl_gpu_palette_${stage}_dxil" "sdl_gpu_palette.${stage}.dxil.h"
done

rm -f sdl_gpu_palette.vert.spv sdl_gpu_palette.vert.msl sdl_gpu_palette.vert.dxil
rm -f sdl_gpu_palette.frag.spv sdl_gpu_palette.frag.msl sdl_gpu_palette.frag.dxil
