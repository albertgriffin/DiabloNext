/**
 * @file automap_render.cpp
 *
 * Line drawing routines for the automap.
 */
#include "engine/render/automap_render.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "automap.h"
#include "engine/render/primitive_render.hpp"
#include "utils/palette_blending.hpp"

namespace devilution {
namespace {

enum class DirectionX : int8_t {
	EAST = 1,
	WEST = -1,
};

enum class DirectionY : int8_t {
	SOUTH = 1,
	NORTH = -1,
};

constexpr uint8_t TransparentAutomapOverlayAlpha = 128;

bool AutomapOverlayCaptureActive = false;
uint64_t AutomapOverlayCaptureVersion = 0;
std::vector<RenderAutomapOverlayRect> AutomapOverlayRects;
bool AutomapOverlaySubCaptureActive = false;
std::vector<RenderAutomapOverlayRect> AutomapOverlaySubCaptureRects;

[[nodiscard]] uint8_t AutomapOverlayAlpha()
{
	return GetAutomapType() == AutomapType::Transparent ? TransparentAutomapOverlayAlpha : 255;
}

[[nodiscard]] bool ClipMapPixel(const Surface &out, const Point position)
{
	if (!out.InBounds(position))
		return false;
	if (GetAutomapType() == AutomapType::Minimap && !MinimapRect.contains(position))
		return false;
	return true;
}

[[nodiscard]] std::vector<RenderAutomapOverlayRect> &CurrentAutomapOverlayRects()
{
	return AutomapOverlaySubCaptureActive ? AutomapOverlaySubCaptureRects : AutomapOverlayRects;
}

void AppendMapOverlayRectWithAlpha(Rectangle rect, const uint8_t color, const uint8_t alpha)
{
	if (rect.size.width <= 0 || rect.size.height <= 0)
		return;

	RenderAutomapOverlayRect overlayRect {
		rect,
		color,
		alpha,
	};
	std::vector<RenderAutomapOverlayRect> &rects = CurrentAutomapOverlayRects();
	if (!rects.empty()) {
		RenderAutomapOverlayRect &previous = rects.back();
		if (previous.colorIndex == overlayRect.colorIndex && previous.alpha == overlayRect.alpha) {
			if (previous.rect.position.y == overlayRect.rect.position.y
			    && previous.rect.size.height == overlayRect.rect.size.height
			    && previous.rect.position.x + previous.rect.size.width == overlayRect.rect.position.x) {
				previous.rect.size.width += overlayRect.rect.size.width;
				return;
			}
			if (previous.rect.position.x == overlayRect.rect.position.x
			    && previous.rect.size.width == overlayRect.rect.size.width
			    && previous.rect.position.y + previous.rect.size.height == overlayRect.rect.position.y) {
				previous.rect.size.height += overlayRect.rect.size.height;
				return;
			}
		}
	}
	rects.push_back(overlayRect);
}

void AppendMapOverlayRect(Rectangle rect, const uint8_t color)
{
	AppendMapOverlayRectWithAlpha(rect, color, AutomapOverlayAlpha());
}

[[nodiscard]] bool ClipMapHorizontalSpan(const Surface &out, Point &from, int &width)
{
	if (from.y < 0 || from.y >= out.h() || from.x >= out.w() || width <= 0 || from.x + width <= 0)
		return false;

	if (from.x < 0) {
		width += from.x;
		from.x = 0;
	}
	if (from.x + width > out.w())
		width = out.w() - from.x;

	if (GetAutomapType() == AutomapType::Minimap) {
		if (from.y < MinimapRect.position.y || from.y >= MinimapRect.position.y + MinimapRect.size.height)
			return false;
		const int x0 = std::max(from.x, MinimapRect.position.x);
		const int x1 = std::min(from.x + width, MinimapRect.position.x + MinimapRect.size.width);
		if (x0 >= x1)
			return false;
		width = x1 - x0;
		from.x = x0;
	}

	return width > 0;
}

[[nodiscard]] bool ClipMapVerticalSpan(const Surface &out, Point &from, int &height)
{
	if (from.x < 0 || from.x >= out.w() || from.y >= out.h() || height <= 0 || from.y + height <= 0)
		return false;

	if (from.y < 0) {
		height += from.y;
		from.y = 0;
	}
	if (from.y + height > out.h())
		height = out.h() - from.y;

	if (GetAutomapType() == AutomapType::Minimap) {
		if (from.x < MinimapRect.position.x || from.x >= MinimapRect.position.x + MinimapRect.size.width)
			return false;
		const int y0 = std::max(from.y, MinimapRect.position.y);
		const int y1 = std::min(from.y + height, MinimapRect.position.y + MinimapRect.size.height);
		if (y0 >= y1)
			return false;
		height = y1 - y0;
		from.y = y0;
	}

	return height > 0;
}

void DrawMapHorizontalSpanNoLayerMark(const Surface &out, Point from, int width, const uint8_t color)
{
	if (!ClipMapHorizontalSpan(out, from, width))
		return;

	if (AutomapOverlayCaptureActive) {
		AppendMapOverlayRect({ from, { width, 1 } }, color);
		return;
	}

	uint8_t *dst = out.at(from.x, from.y);
	if (GetAutomapType() == AutomapType::Transparent) {
		const uint8_t *lookupTable = paletteTransparencyLookup[color];
		for (int i = 0; i < width; i++) {
			dst[i] = lookupTable[dst[i]];
		}
	} else {
		std::memset(dst, color, static_cast<size_t>(width));
	}
	MarkRenderLayerSpan(out, from, width);
}

void DrawMapVerticalSpanNoLayerMark(const Surface &out, Point from, int height, const uint8_t color)
{
	if (!ClipMapVerticalSpan(out, from, height))
		return;

	if (AutomapOverlayCaptureActive) {
		AppendMapOverlayRect({ from, { 1, height } }, color);
		return;
	}

	uint8_t *dst = out.at(from.x, from.y);
	const int pitch = out.pitch();
	if (GetAutomapType() == AutomapType::Transparent) {
		const uint8_t *lookupTable = paletteTransparencyLookup[color];
		for (int i = 0; i < height; i++) {
			*dst = lookupTable[*dst];
			dst += pitch;
		}
	} else {
		for (int i = 0; i < height; i++) {
			*dst = color;
			dst += pitch;
		}
	}
	MarkRenderLayerRect(out, { from, { 1, height } });
}

[[nodiscard]] bool DrawMapPixelNoLayerMark(const Surface &out, const Point position, const uint8_t color)
{
	if (!ClipMapPixel(out, position))
		return false;

	if (AutomapOverlayCaptureActive) {
		AppendMapOverlayRect({ position, { 1, 1 } }, color);
		return true;
	}

	uint8_t *dst = out.at(position.x, position.y);
	if (GetAutomapType() == AutomapType::Transparent) {
		*dst = paletteTransparencyLookup[color][*dst];
	} else {
		*dst = color;
	}
	return true;
}

void DrawMapHorizontalPixelPair(const Surface &out, const Point from, const int deltaX, const uint8_t color)
{
	const Point second { from.x + deltaX, from.y };
	const bool drewFirst = DrawMapPixelNoLayerMark(out, from, color);
	const bool drewSecond = DrawMapPixelNoLayerMark(out, second, color);
	if (AutomapOverlayCaptureActive)
		return;
	if (drewFirst && drewSecond) {
		MarkRenderLayerSpan(out, { std::min(from.x, second.x), from.y }, 2);
	} else if (drewFirst) {
		MarkRenderLayerPixel(out, from);
	} else if (drewSecond) {
		MarkRenderLayerPixel(out, second);
	}
}

void DrawMapSinglePixel(const Surface &out, const Point position, const uint8_t color)
{
	if (DrawMapPixelNoLayerMark(out, position, color) && !AutomapOverlayCaptureActive)
		MarkRenderLayerPixel(out, position);
}

template <DirectionY DirY>
void DrawMapSteepSegment(const Surface &out, Point from, const uint8_t colorIndex)
{
	const int deltaY = static_cast<int>(DirY);
	bool drewAny = false;
	drewAny |= DrawMapPixelNoLayerMark(out, { from.x, from.y + 1 }, 0);
	drewAny |= DrawMapPixelNoLayerMark(out, from, colorIndex);
	from.y += deltaY;
	drewAny |= DrawMapPixelNoLayerMark(out, { from.x, from.y + 1 }, 0);
	drewAny |= DrawMapPixelNoLayerMark(out, from, colorIndex);
	if (!drewAny)
		return;
	if (AutomapOverlayCaptureActive)
		return;

	const int y0 = std::min({ from.y, from.y + 1, from.y - deltaY, from.y - deltaY + 1 });
	const int y1 = std::max({ from.y, from.y + 1, from.y - deltaY, from.y - deltaY + 1 });
	MarkRenderLayerRect(out, { { from.x, y0 }, { 1, y1 - y0 + 1 } });
}

template <DirectionX DirX, DirectionY DirY>
void DrawMapLine(const Surface &out, Point from, int height, std::uint8_t colorIndex)
{
	const int deltaX = static_cast<int>(DirX);
	const int deltaY = static_cast<int>(DirY);
	while (height-- > 0) {
		DrawMapHorizontalPixelPair(out, { from.x, from.y + 1 }, deltaX, 0);
		DrawMapHorizontalPixelPair(out, from, deltaX, colorIndex);
		from.x += 2 * deltaX;
		from.y += deltaY;
	}
	DrawMapSinglePixel(out, { from.x, from.y + 1 }, 0);
	DrawMapSinglePixel(out, from, colorIndex);
}

template <DirectionX DirX, DirectionY DirY>
void DrawMapLineSteep(const Surface &out, Point from, int width, std::uint8_t colorIndex)
{
	const int deltaX = static_cast<int>(DirX);
	const int deltaY = static_cast<int>(DirY);
	while (width-- > 0) {
		DrawMapSteepSegment<DirY>(out, from, colorIndex);
		from.y += 2 * deltaY;
		from.x += deltaX;
	}
	DrawMapSinglePixel(out, { from.x, from.y + 1 }, 0);
	DrawMapSinglePixel(out, from, colorIndex);
}

} // namespace

void BeginRenderAutomapOverlayCapture()
{
	AutomapOverlayRects.clear();
	AutomapOverlaySubCaptureRects.clear();
	AutomapOverlayCaptureActive = true;
	AutomapOverlaySubCaptureActive = false;
	AutomapOverlayCaptureVersion++;
	if (AutomapOverlayCaptureVersion == 0)
		AutomapOverlayCaptureVersion = 1;
}

RenderAutomapOverlayView EndRenderAutomapOverlayCapture()
{
	AutomapOverlayCaptureActive = false;
	AutomapOverlaySubCaptureActive = false;
	return {
		AutomapOverlayRects.data(),
		AutomapOverlayRects.size(),
		AutomapOverlayCaptureVersion,
		!AutomapOverlayRects.empty(),
	};
}

void ClearRenderAutomapOverlayCapture()
{
	AutomapOverlayCaptureActive = false;
	AutomapOverlaySubCaptureActive = false;
	AutomapOverlayRects.clear();
	AutomapOverlaySubCaptureRects.clear();
}

bool RenderAutomapOverlayCaptureActive()
{
	return AutomapOverlayCaptureActive;
}

void BeginRenderAutomapOverlaySubCapture()
{
	AutomapOverlaySubCaptureRects.clear();
	AutomapOverlaySubCaptureActive = AutomapOverlayCaptureActive;
}

RenderAutomapOverlayView EndRenderAutomapOverlaySubCapture()
{
	AutomapOverlaySubCaptureActive = false;
	return {
		AutomapOverlaySubCaptureRects.data(),
		AutomapOverlaySubCaptureRects.size(),
		AutomapOverlayCaptureVersion,
		!AutomapOverlaySubCaptureRects.empty(),
	};
}

std::size_t RenderAutomapOverlayCaptureRectCount()
{
	return AutomapOverlayRects.size();
}

const RenderAutomapOverlayRect *RenderAutomapOverlayCaptureRects()
{
	return AutomapOverlayRects.data();
}

void ResizeRenderAutomapOverlayCapture(const std::size_t size)
{
	if (size < AutomapOverlayRects.size())
		AutomapOverlayRects.resize(size);
}

void AppendRenderAutomapOverlayRect(const Surface &out, RenderAutomapOverlayRect rect)
{
	if (!AutomapOverlayCaptureActive)
		return;

	int x0 = std::clamp(rect.rect.position.x, 0, out.w());
	int y0 = std::clamp(rect.rect.position.y, 0, out.h());
	int x1 = std::clamp(rect.rect.position.x + rect.rect.size.width, 0, out.w());
	int y1 = std::clamp(rect.rect.position.y + rect.rect.size.height, 0, out.h());

	if (GetAutomapType() == AutomapType::Minimap) {
		x0 = std::max(x0, MinimapRect.position.x);
		y0 = std::max(y0, MinimapRect.position.y);
		x1 = std::min(x1, MinimapRect.position.x + MinimapRect.size.width);
		y1 = std::min(y1, MinimapRect.position.y + MinimapRect.size.height);
	}

	if (x0 >= x1 || y0 >= y1)
		return;

	rect.rect = { { x0, y0 }, { x1 - x0, y1 - y0 } };
	AppendMapOverlayRectWithAlpha(rect.rect, rect.colorIndex, rect.alpha);
}

void DrawMapLineNS(const Surface &out, Point from, int height, std::uint8_t colorIndex)
{
	DrawMapVerticalSpanNoLayerMark(out, from, height, colorIndex);
}

void DrawMapLineWE(const Surface &out, Point from, int width, std::uint8_t colorIndex)
{
	DrawMapHorizontalSpanNoLayerMark(out, from, width, colorIndex);
}

void DrawMapLineNE(const Surface &out, Point from, int height, std::uint8_t colorIndex)
{
	DrawMapLine<DirectionX::EAST, DirectionY::NORTH>(out, from, height, colorIndex);
}

void DrawMapLineSE(const Surface &out, Point from, int height, std::uint8_t colorIndex)
{
	DrawMapLine<DirectionX::EAST, DirectionY::SOUTH>(out, from, height, colorIndex);
}

void DrawMapLineNW(const Surface &out, Point from, int height, std::uint8_t colorIndex)
{
	DrawMapLine<DirectionX::WEST, DirectionY::NORTH>(out, from, height, colorIndex);
}

void DrawMapLineSW(const Surface &out, Point from, int height, std::uint8_t colorIndex)
{
	DrawMapLine<DirectionX::WEST, DirectionY::SOUTH>(out, from, height, colorIndex);
}

void DrawMapLineSteepNE(const Surface &out, Point from, int width, std::uint8_t colorIndex)
{
	DrawMapLineSteep<DirectionX::EAST, DirectionY::NORTH>(out, from, width, colorIndex);
}

void DrawMapLineSteepSE(const Surface &out, Point from, int width, std::uint8_t colorIndex)
{
	DrawMapLineSteep<DirectionX::EAST, DirectionY::SOUTH>(out, from, width, colorIndex);
}

void DrawMapLineSteepNW(const Surface &out, Point from, int width, std::uint8_t colorIndex)
{
	DrawMapLineSteep<DirectionX::WEST, DirectionY::NORTH>(out, from, width, colorIndex);
}

void DrawMapLineSteepSW(const Surface &out, Point from, int width, std::uint8_t colorIndex)
{
	DrawMapLineSteep<DirectionX::WEST, DirectionY::SOUTH>(out, from, width, colorIndex);
}

/**
 * @brief Draws a line from first point to second point, unrestricted to the standard automap angles. Doesn't include shadow.
 */
void DrawMapFreeLine(const Surface &out, Point from, Point to, uint8_t colorIndex)
{
	const int dx = std::abs(to.x - from.x);
	const int dy = std::abs(to.y - from.y);
	const int sx = from.x < to.x ? 1 : -1;
	const int sy = from.y < to.y ? 1 : -1;
	int err = dx - dy;

	while (true) {
		SetMapPixel(out, from, colorIndex);

		if (from.x == to.x && from.y == to.y) {
			break;
		}

		const int e2 = 2 * err;
		if (e2 > -dy) {
			err -= dy;
			from.x += sx;
		}
		if (e2 < dx) {
			err += dx;
			from.y += sy;
		}
	}
}

void SetMapPixel(const Surface &out, Point position, uint8_t color)
{
	if (!ClipMapPixel(out, position))
		return;

	if (AutomapOverlayCaptureActive) {
		AppendMapOverlayRect({ position, { 1, 1 } }, color);
		return;
	}

	if (GetAutomapType() == AutomapType::Transparent) {
		SetHalfTransparentPixel(out, position, color);
	} else {
		out.SetPixel(position, color);
	}
}

} // namespace devilution
