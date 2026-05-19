/**
 * @file frame_compositor.hpp
 *
 * Final frame composition helpers for the indexed renderer.
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#else
#include <SDL.h>
#endif

#include "engine/rectangle.hpp"
#include "engine/render/render_layer_diagnostics.hpp"
#include "engine/render/render_perf.hpp"
#include "engine/size.hpp"
#ifdef BUILD_TESTING
#include "utils/attributes.h"
#endif

namespace devilution {

struct RgbColor {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a = 255;
};

struct PaletteSnapshot {
	std::array<RgbColor, 256> colors;
	uint64_t version = 0;
};

struct IndexBufferView {
	const uint8_t *pixels = nullptr;
	int width = 0;
	int height = 0;
	int pitch = 0;
};

struct DirtyRectList {
	std::vector<Rectangle> rects;
	bool fullFrame = false;
};

struct CompositionFrame {
	Size logicalSize;
	IndexBufferView indexBuffer;
	PaletteSnapshot palette;
	DirtyRectList dirtyRects;
	bool diagnosticTransform = false;
	RenderLayerDiagnosticMode renderLayerDiagnosticMode = RenderLayerDiagnosticMode::Off;
	RenderLayerMapView renderLayerMap;
};

enum class FrameCompositorBackendResult : uint8_t {
	None,
	UpdatedOutputSurface,
	Presented,
};

class IFrameCompositorBackend {
public:
	virtual ~IFrameCompositorBackend() = default;

	[[nodiscard]] virtual std::string_view Name() const = 0;
	[[nodiscard]] virtual bool IsAvailable() const = 0;
	[[nodiscard]] virtual FrameCompositorBackendResult Compose(const CompositionFrame &frame, SDL_Surface &outputSurface, const std::vector<Rectangle> &rects, RenderPerfCompositionStats &stats) = 0;
	virtual void Present() { }
};

[[nodiscard]] IndexBufferView MakeIndexBufferView(const SDL_Surface &surface);
[[nodiscard]] PaletteSnapshot MakePaletteSnapshot(const std::array<SDL_Color, 256> &palette, uint64_t version);
[[nodiscard]] std::unique_ptr<IFrameCompositorBackend> CreateCpuFrameCompositorBackend();

class IFrameCompositor {
public:
	virtual ~IFrameCompositor() = default;

	virtual void BeginFrame(Size logicalSize) = 0;
	virtual void SubmitIndexBuffer(IndexBufferView indexBuffer) = 0;
	virtual void SubmitPalette(const PaletteSnapshot &palette) = 0;
	virtual void SubmitDirtyRects(const DirtyRectList &dirtyRects) = 0;
	virtual void Compose(const CompositionFrame &frame) = 0;
	virtual void Compose() = 0;
	virtual void Present() = 0;
};

class CpuPaletteCompositor final : public IFrameCompositor {
public:
	CpuPaletteCompositor();
	explicit CpuPaletteCompositor(std::unique_ptr<IFrameCompositorBackend> backend);

	void BeginFrame(Size logicalSize) override;
	void SubmitIndexBuffer(IndexBufferView indexBuffer) override;
	void SubmitPalette(const PaletteSnapshot &palette) override;
	void SubmitDirtyRects(const DirtyRectList &dirtyRects) override;
	void Compose(const CompositionFrame &frame) override;
	void Compose() override;
	void Present() override;

	void SetOutputSurface(SDL_Surface *outputSurface);
	void AddDirtyRect(Rectangle rect);
	void SetFullFrameDirty();
	void ResetDirtyRects();
	void SetDiagnosticTransformEnabled(bool enabled);
	void SetBackend(std::unique_ptr<IFrameCompositorBackend> backend);
	[[nodiscard]] const DirtyRectList &GetDirtyRects() const;
	[[nodiscard]] const RenderPerfCompositionStats &GetLastCompositionStats() const;
	[[nodiscard]] FrameCompositorBackendResult GetLastBackendResult() const;

private:
	[[nodiscard]] FrameCompositorBackendResult ComposeRects(const CompositionFrame &frame, const std::vector<Rectangle> &rects);

	std::unique_ptr<IFrameCompositorBackend> backend_;
	Size logicalSize_ {};
	IndexBufferView indexBuffer_ {};
	PaletteSnapshot palette_ {};
	DirtyRectList dirtyRects_ {};
	SDL_Surface *outputSurface_ = nullptr;
	bool diagnosticTransformEnabled_ = false;
	RenderLayerDiagnosticMode renderLayerDiagnosticMode_ = RenderLayerDiagnosticMode::Off;
	RenderLayerMapView renderLayerMap_ {};
	bool hasComposedFrame_ = false;
	uint64_t lastComposedPaletteVersion_ = 0;
	bool lastComposedDiagnosticTransformEnabled_ = false;
	RenderLayerDiagnosticMode lastRenderLayerDiagnosticMode_ = RenderLayerDiagnosticMode::Off;
	bool outputSurfaceChangedSinceComposition_ = false;
	bool indexBufferChangedSinceComposition_ = false;
	bool logicalSizeChangedSinceComposition_ = false;
	FrameCompositorBackendResult lastBackendResult_ = FrameCompositorBackendResult::None;
	RenderPerfCompositionStats lastCompositionStats_ {};
};

[[nodiscard]] bool FrameCompositionEnabled();
void SubmitFrameCompositionDirtyRect(Rectangle rect);
void SubmitFrameCompositionFullFrame();
void ResetFrameCompositionDirtyRects();
[[nodiscard]] bool ComposeFrameToOutput(SDL_Surface *outputSurface);
void PresentFrameComposition();
void ShutdownFrameComposition();

#ifdef BUILD_TESTING
void DVL_API_FOR_TEST SetFrameCompositorThreadCountOverrideForTesting(int threadCount);
#endif

} // namespace devilution
