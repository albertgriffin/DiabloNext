/**
 * @file frame_compositor.hpp
 *
 * Final frame composition helpers for the indexed renderer.
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
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
	uint64_t version = 0;
};

struct DirtyRectList {
	std::vector<Rectangle> rects;
	bool fullFrame = false;
};

enum class CompositionAttachmentRole : uint8_t {
	IndexedAlbedo,
	Palette,
	WorldIndex,
	WorldOverlayIndex,
	WorldMaterial,
	WorldDepth,
	WorldHeight,
	WorldNormal,
	WorldOccluder,
	WorldReceiver,
	LightAccumulation,
	ShadowMask,
	ParticleAccumulation,
	InterfaceIndex,
	CursorIndex,
	Diagnostic,
};

enum class CompositionAttachmentFormat : uint8_t {
	Unknown,
	Index8,
	PaletteRgba8,
	Rgba8,
	Alpha8,
};

struct CompositionAttachment {
	CompositionAttachmentRole role = CompositionAttachmentRole::IndexedAlbedo;
	CompositionAttachmentFormat format = CompositionAttachmentFormat::Unknown;
	Size logicalSize;
	int pitch = 0;
	uint64_t version = 0;
	DirtyRectList dirtyRects;
	const uint8_t *cpuPixels = nullptr;
};

enum class CompositionAttachmentUploadAction : uint8_t {
	Skip,
	Full,
	Partial,
};

struct CompositionAttachmentUploadPlan {
	CompositionAttachmentUploadAction action = CompositionAttachmentUploadAction::Skip;
	std::vector<Rectangle> rects;
	uint64_t byteCount = 0;
};

enum class CompositionSurfaceRole : uint8_t {
	World,
	WorldOverlay,
	Interface,
	Cursor,
	DiagnosticOverlay,
	Count,
};

inline constexpr size_t CompositionSurfaceRoleCount = static_cast<size_t>(CompositionSurfaceRole::Count);

struct CompositionSurfaceRoleCoverage {
	uint32_t dirtyRectCount = 0;
	uint64_t dirtyPixelArea = 0;
	Rectangle dirtyBounds;
	bool fullFrameDirty = false;
};

struct CompositionSurfaceMetadata {
	std::array<CompositionSurfaceRoleCoverage, CompositionSurfaceRoleCount> roles {};
};

struct CompositionFrame {
	Size logicalSize;
	IndexBufferView indexBuffer;
	PaletteSnapshot palette;
	DirtyRectList dirtyRects;
	bool diagnosticTransform = false;
	RenderLayerDiagnosticMode renderLayerDiagnosticMode = RenderLayerDiagnosticMode::Off;
	RenderLayerMapView renderLayerMap;
	CompositionSurfaceMetadata compositionSurfaceMetadata;
	std::vector<CompositionAttachment> attachments;
	RenderWorldMaskMapView worldMaskMap;
	RenderWorldMaskDiagnosticMode renderWorldMaskDiagnosticMode = RenderWorldMaskDiagnosticMode::Off;
	RenderWorldProxyMapView worldProxyMap;
	RenderWorldProxyDiagnosticMode renderWorldProxyDiagnosticMode = RenderWorldProxyDiagnosticMode::Off;
};

enum class FrameCompositorBackendResult : uint8_t {
	/** No CPU surface update and no direct presentation frame was produced for this composition pass. */
	NoFrameProduced,
	/** The CPU output surface now contains the composed frame; the caller must present it through the normal SDL path. */
	UpdatedOutputSurface,
	/** A backend-specific direct presentation frame was prepared; Present() must be called once to display it. */
	PreparedDirectPresentation,
	/** No new pixels were uploaded, but a previously prepared direct presentation frame is still valid for this frame. */
	RetainedDirectPresentation,
};

class IFrameCompositorBackend {
public:
	virtual ~IFrameCompositorBackend() = default;

	[[nodiscard]] virtual std::string_view Name() const = 0;
	[[nodiscard]] virtual bool IsAvailable() const = 0;
	[[nodiscard]] virtual bool CanRetainDirectPresentation() const { return false; }
	[[nodiscard]] virtual FrameCompositorBackendResult Compose(const CompositionFrame &frame, SDL_Surface &outputSurface, const std::vector<Rectangle> &rects, RenderPerfCompositionStats &stats) = 0;
	virtual void Present() { }
};

[[nodiscard]] IndexBufferView MakeIndexBufferView(const SDL_Surface &surface);
[[nodiscard]] PaletteSnapshot MakePaletteSnapshot(const std::array<SDL_Color, 256> &palette, uint64_t version);
[[nodiscard]] CompositionAttachment MakeIndexedAlbedoAttachment(IndexBufferView indexBuffer, Size logicalSize, const DirtyRectList &dirtyRects);
[[nodiscard]] CompositionAttachment MakePaletteAttachment(const PaletteSnapshot &palette);
[[nodiscard]] const CompositionAttachment *FindCompositionAttachment(std::span<const CompositionAttachment> attachments, CompositionAttachmentRole role);
[[nodiscard]] CompositionAttachmentUploadPlan PlanCompositionAttachmentUpload(const CompositionAttachment &attachment, bool alreadyUploaded, uint64_t uploadedVersion);
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
	void AddDirtyRect(Rectangle rect, CompositionSurfaceRole role);
	void SetFullFrameDirty();
	void SetFullFrameDirty(CompositionSurfaceRole role);
	void ResetDirtyRects();
	void SetDiagnosticTransformEnabled(bool enabled);
	void SetBackend(std::unique_ptr<IFrameCompositorBackend> backend);
	[[nodiscard]] const DirtyRectList &GetDirtyRects() const;
	[[nodiscard]] const CompositionSurfaceMetadata &GetCompositionSurfaceMetadata() const;
	[[nodiscard]] const RenderPerfCompositionStats &GetLastCompositionStats() const;
	[[nodiscard]] FrameCompositorBackendResult GetLastBackendResult() const;

private:
	[[nodiscard]] FrameCompositorBackendResult ComposeRects(const CompositionFrame &frame, const std::vector<Rectangle> &rects);

	std::unique_ptr<IFrameCompositorBackend> backend_;
	Size logicalSize_ {};
	IndexBufferView indexBuffer_ {};
	PaletteSnapshot palette_ {};
	DirtyRectList dirtyRects_ {};
	CompositionSurfaceMetadata compositionSurfaceMetadata_ {};
	SDL_Surface *outputSurface_ = nullptr;
	bool diagnosticTransformEnabled_ = false;
	RenderLayerDiagnosticMode renderLayerDiagnosticMode_ = RenderLayerDiagnosticMode::Off;
	RenderLayerMapView renderLayerMap_ {};
	RenderWorldMaskMapView worldMaskMap_ {};
	RenderWorldMaskDiagnosticMode renderWorldMaskDiagnosticMode_ = RenderWorldMaskDiagnosticMode::Off;
	RenderWorldProxyMapView worldProxyMap_ {};
	RenderWorldProxyDiagnosticMode renderWorldProxyDiagnosticMode_ = RenderWorldProxyDiagnosticMode::Off;
	bool hasComposedFrame_ = false;
	uint64_t lastComposedPaletteVersion_ = 0;
	bool lastComposedDiagnosticTransformEnabled_ = false;
	RenderLayerDiagnosticMode lastRenderLayerDiagnosticMode_ = RenderLayerDiagnosticMode::Off;
	RenderWorldMaskDiagnosticMode lastRenderWorldMaskDiagnosticMode_ = RenderWorldMaskDiagnosticMode::Off;
	RenderWorldProxyDiagnosticMode lastRenderWorldProxyDiagnosticMode_ = RenderWorldProxyDiagnosticMode::Off;
	bool outputSurfaceChangedSinceComposition_ = false;
	bool indexBufferChangedSinceComposition_ = false;
	bool logicalSizeChangedSinceComposition_ = false;
	bool directPresentationPending_ = false;
	bool lastComposedFrameUsedDirectPresentation_ = false;
	FrameCompositorBackendResult lastBackendResult_ = FrameCompositorBackendResult::NoFrameProduced;
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
