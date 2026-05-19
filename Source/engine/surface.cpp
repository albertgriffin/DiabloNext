#include "engine/surface.hpp"

#include <cstdint>
#include <cstring>

#include "engine/render/render_layer.hpp"

namespace devilution {

namespace {

template <bool SkipColorIndexZero>
void SurfaceBlit(const Surface &src, SDL_Rect srcRect, const Surface &dst, Point dstPosition)
{
	// We do not use `SDL_BlitSurface` here because the palettes may be different objects
	// and SDL would attempt to map them.

	dst.Clip(&srcRect, &dstPosition);
	if (srcRect.w <= 0 || srcRect.h <= 0)
		return;

	const std::uint8_t *srcBuf = src.at(srcRect.x, srcRect.y);
	const auto srcPitch = src.pitch();
	std::uint8_t *dstBuf = &dst[dstPosition];
	const auto dstPitch = dst.pitch();

	for (int h = srcRect.h; h != 0; --h) {
		if constexpr (SkipColorIndexZero) {
			int spanStart = -1;
			for (int x = 0; x < srcRect.w; ++x) {
				if (srcBuf[x] != 0) {
					dstBuf[x] = srcBuf[x];
					if (spanStart == -1)
						spanStart = x;
				} else if (spanStart != -1) {
					MarkRenderLayerSpan(dstBuf + spanStart, x - spanStart);
					spanStart = -1;
				}
			}
			if (spanStart != -1)
				MarkRenderLayerSpan(dstBuf + spanStart, srcRect.w - spanStart);
		} else {
			std::memcpy(dstBuf, srcBuf, srcRect.w);
			MarkRenderLayerSpan(dstBuf, srcRect.w);
		}
		srcBuf += srcPitch;
		dstBuf += dstPitch;
	}
}

} // namespace

void Surface::BlitFrom(const Surface &src, SDL_Rect srcRect, Point targetPosition) const
{
	SurfaceBlit</*SkipColorIndexZero=*/false>(src, srcRect, *this, targetPosition);
}

void Surface::BlitFromSkipColorIndexZero(const Surface &src, SDL_Rect srcRect, Point targetPosition) const
{
	SurfaceBlit</*SkipColorIndexZero=*/true>(src, srcRect, *this, targetPosition);
}

} // namespace devilution
