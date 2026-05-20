#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#else
#include <SDL.h>
#endif

#include "engine/render/accelerated_compositor_lifecycle.hpp"
#include "engine/render/accelerated_palette_compositor.hpp"
#include "engine/render/frame_compositor.hpp"
#include "engine/render/render_layer.hpp"
#include "options.h"
#include "utils/sdl_wrap.h"

namespace devilution {
namespace {

uint32_t ReadPixel(const SDL_Surface &surface, const int x, const int y)
{
#ifdef USE_SDL3
	const int bytesPerPixel = SDL_BYTESPERPIXEL(surface.format);
#else
	const int bytesPerPixel = surface.format->BytesPerPixel;
#endif
	const uint8_t *src = static_cast<const uint8_t *>(surface.pixels) + y * surface.pitch + x * bytesPerPixel;
	uint32_t pixel = 0;
	std::memcpy(&pixel, src, bytesPerPixel);
	return pixel;
}

SDL_Color ReadColor(const SDL_Surface &surface, const int x, const int y)
{
	SDL_Color color {};
	const uint32_t pixel = ReadPixel(surface, x, y);
#ifdef USE_SDL3
	SDL_GetRGBA(pixel, SDL_GetPixelFormatDetails(surface.format), SDL_GetSurfacePalette(const_cast<SDL_Surface *>(&surface)), &color.r, &color.g, &color.b, &color.a);
#else
	SDL_GetRGBA(pixel, surface.format, &color.r, &color.g, &color.b, &color.a);
#endif
	return color;
}

class FrameCompositorThreadCountOverrideGuard {
public:
	explicit FrameCompositorThreadCountOverrideGuard(const int threadCount)
	{
		SetFrameCompositorThreadCountOverrideForTesting(threadCount);
	}

	~FrameCompositorThreadCountOverrideGuard()
	{
		SetFrameCompositorThreadCountOverrideForTesting(0);
	}
};

class RenderLightShadowDiagnosticModeGuard {
public:
	explicit RenderLightShadowDiagnosticModeGuard(const RenderLightShadowDiagnosticMode mode)
	    : previousMode_(*GetOptions().Experimental.renderLightShadowDiagnosticMode)
	{
		GetOptions().Experimental.renderLightShadowDiagnosticMode.SetValue(mode);
	}

	~RenderLightShadowDiagnosticModeGuard()
	{
		GetOptions().Experimental.renderLightShadowDiagnosticMode.SetValue(previousMode_);
	}

private:
	RenderLightShadowDiagnosticMode previousMode_;
};

class RecordingFrameCompositorBackend final : public IFrameCompositorBackend {
public:
	std::string_view Name() const override
	{
		return "recording";
	}

	bool IsAvailable() const override
	{
		return available;
	}

	FrameCompositorBackendResult Compose(const CompositionFrame &frame, SDL_Surface &outputSurface, const std::vector<Rectangle> &rects, RenderPerfCompositionStats &stats) override
	{
		composeCallCount++;
		observedFrame = frame;
		observedOutputSurface = &outputSurface;
		observedRects = rects;
		stats.selectedThreadCount = selectedThreadCount;
		return composeResult;
	}

	void Present() override
	{
		presentCallCount++;
	}

	bool available = true;
	FrameCompositorBackendResult composeResult = FrameCompositorBackendResult::UpdatedOutputSurface;
	int selectedThreadCount = 7;
	int composeCallCount = 0;
	int presentCallCount = 0;
	CompositionFrame observedFrame {};
	SDL_Surface *observedOutputSurface = nullptr;
	std::vector<Rectangle> observedRects;
};

class RecordingAcceleratedPalettePresenter final : public IAcceleratedPalettePresenter {
public:
	std::string_view Name() const override
	{
		return "recording-accelerated";
	}

	bool IsAvailable() const override
	{
		return available;
	}

	bool PrepareIndexedFrame(const AcceleratedPaletteFrame &frame, RenderPerfCompositionStats &stats) override
	{
		indexedFrameCallCount++;
		observedIndexedFrame = frame.composition;
		observedIndexedLighting = frame.lighting;
		observedUploadDirtyRects.assign(frame.uploadDirtyRects.begin(), frame.uploadDirtyRects.end());
		if (!indexedFrameResult) {
			stats.failedUploadCount++;
			stats.uploadFallbackReason = CompositionUploadFallbackReason::SubmitFailed;
		}
		return indexedFrameResult;
	}

	bool PrepareOutputSurfaceFrame(const AcceleratedPaletteFrame &frame, SDL_Surface &outputSurface, RenderPerfCompositionStats &stats) override
	{
		outputSurfaceFrameCallCount++;
		observedOutputFrame = frame.composition;
		observedOutputSurface = &outputSurface;
		observedOutputLighting = frame.lighting;
		if (!outputSurfaceFrameResult) {
			stats.failedUploadCount++;
			stats.uploadFallbackReason = CompositionUploadFallbackReason::SubmitFailed;
		}
		return outputSurfaceFrameResult;
	}

	void Present() override
	{
		presentCallCount++;
	}

	bool available = true;
	bool indexedFrameResult = true;
	bool outputSurfaceFrameResult = true;
	int indexedFrameCallCount = 0;
	int outputSurfaceFrameCallCount = 0;
	int presentCallCount = 0;
	CompositionFrame observedIndexedFrame {};
	CompositionFrame observedOutputFrame {};
	const CompositionLightingInputs *observedIndexedLighting = nullptr;
	const CompositionLightingInputs *observedOutputLighting = nullptr;
	std::vector<Rectangle> observedUploadDirtyRects;
	SDL_Surface *observedOutputSurface = nullptr;
};

const CompositionSurfaceRoleCoverage &Coverage(const CompositionSurfaceMetadata &metadata, const CompositionSurfaceRole role)
{
	return metadata.roles[static_cast<size_t>(role)];
}

void ExpectWorldTint(const SDL_Surface &surface, const int x, const int y, const SDL_Color baseColor)
{
	const SDL_Color color = ReadColor(surface, x, y);
	EXPECT_EQ(color.r, baseColor.r / 2);
	EXPECT_EQ(color.g, (static_cast<uint16_t>(baseColor.g) + 255) / 2);
	EXPECT_EQ(color.b, baseColor.b / 2);
}

DirtyRectList FullFrameDirtyRectsForTest()
{
	DirtyRectList dirtyRects;
	dirtyRects.fullFrame = true;
	return dirtyRects;
}

TEST(FrameCompositor, AcceleratedCompositorApiNamesAreStable)
{
	EXPECT_EQ(AcceleratedCompositorApiName(AcceleratedCompositorApi::None), "none");
	EXPECT_EQ(AcceleratedCompositorApiName(AcceleratedCompositorApi::OpenGl), "OpenGL");
	EXPECT_EQ(AcceleratedCompositorApiName(AcceleratedCompositorApi::SdlGpu), "SDL_GPU");
}

TEST(FrameCompositor, NeutralCompositionLightingInputsPrepareNoOpBuffers)
{
	NeutralCompositionLightingInputs lightingInputs;
	const CompositionLightingInputs *prepared = lightingInputs.Prepare({ 2, 2 });
	ASSERT_NE(prepared, nullptr);
	EXPECT_EQ(prepared->diagnosticMode, RenderLightShadowDiagnosticMode::Off);

	EXPECT_TRUE(prepared->light.IsValid());
	EXPECT_EQ(prepared->light.size.width, 2);
	EXPECT_EQ(prepared->light.size.height, 2);
	EXPECT_EQ(prepared->light.pitch, 8);
	EXPECT_EQ(prepared->light.format, CompositionLightingBufferFormat::Rgba8);
	for (size_t i = 0; i < 16; i++) {
		EXPECT_EQ(prepared->light.pixels[i], 255);
	}

	EXPECT_TRUE(prepared->shadow.IsValid());
	EXPECT_EQ(prepared->shadow.size.width, 2);
	EXPECT_EQ(prepared->shadow.size.height, 2);
	EXPECT_EQ(prepared->shadow.pitch, 2);
	EXPECT_EQ(prepared->shadow.format, CompositionLightingBufferFormat::Alpha8);
	for (size_t i = 0; i < 4; i++) {
		EXPECT_EQ(prepared->shadow.pixels[i], 0);
	}

	const uint8_t *lightPixels = prepared->light.pixels;
	const uint8_t *shadowPixels = prepared->shadow.pixels;
	EXPECT_EQ(lightingInputs.Prepare({ 2, 2 }), prepared);
	EXPECT_EQ(prepared->light.pixels, lightPixels);
	EXPECT_EQ(prepared->shadow.pixels, shadowPixels);

	const CompositionLightingInputs *resized = lightingInputs.Prepare({ 3, 1 });
	ASSERT_NE(resized, nullptr);
	EXPECT_EQ(resized->light.size.width, 3);
	EXPECT_EQ(resized->light.size.height, 1);
	EXPECT_EQ(resized->light.pitch, 12);
	EXPECT_EQ(resized->shadow.size.width, 3);
	EXPECT_EQ(resized->shadow.size.height, 1);
	EXPECT_EQ(resized->shadow.pitch, 3);

	EXPECT_EQ(lightingInputs.Prepare({ 0, 1 }), nullptr);
	EXPECT_EQ(lightingInputs.Get(), nullptr);
}

TEST(FrameCompositor, DevelopmentCompositionLightingInputsOffReturnsNull)
{
	DevelopmentCompositionLightingInputs lightingInputs;
	EXPECT_EQ(lightingInputs.Prepare({ 16, 16 }, RenderLightShadowDiagnosticMode::Off), nullptr);
	EXPECT_EQ(lightingInputs.Get(), nullptr);
}

TEST(FrameCompositor, DevelopmentCompositionLightingInputsProduceDeterministicFullFrameDiagnostics)
{
	DevelopmentCompositionLightingInputs firstInputs;
	DevelopmentCompositionLightingInputs secondInputs;
	const CompositionLightingInputs *first = firstInputs.Prepare({ 96, 64 }, RenderLightShadowDiagnosticMode::FinalLitOutput);
	const CompositionLightingInputs *second = secondInputs.Prepare({ 96, 64 }, RenderLightShadowDiagnosticMode::FinalLitOutput);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);

	EXPECT_EQ(first->diagnosticMode, RenderLightShadowDiagnosticMode::FinalLitOutput);
	EXPECT_TRUE(first->light.dirtyRects.fullFrame);
	EXPECT_TRUE(first->shadow.dirtyRects.fullFrame);
	EXPECT_EQ(first->light.version, first->shadow.version);
	EXPECT_EQ(first->light.size, Size(96, 64));
	EXPECT_EQ(first->shadow.size, Size(96, 64));
	EXPECT_EQ(std::memcmp(first->light.pixels, second->light.pixels, static_cast<size_t>(first->light.pitch) * first->light.size.height), 0);
	EXPECT_EQ(std::memcmp(first->shadow.pixels, second->shadow.pixels, static_cast<size_t>(first->shadow.pitch) * first->shadow.size.height), 0);

	bool hasNonNeutralLight = false;
	for (int i = 0; i < first->light.pitch * first->light.size.height; i += 4) {
		if (first->light.pixels[i] != 255 || first->light.pixels[i + 1] != 255 || first->light.pixels[i + 2] != 255) {
			hasNonNeutralLight = true;
			break;
		}
	}
	bool hasShadow = false;
	for (int i = 0; i < first->shadow.pitch * first->shadow.size.height; i++) {
		if (first->shadow.pixels[i] != 0) {
			hasShadow = true;
			break;
		}
	}
	EXPECT_TRUE(hasNonNeutralLight);
	EXPECT_TRUE(hasShadow);

	const uint64_t firstVersion = first->light.version;
	const CompositionLightingInputs *advanced = firstInputs.Prepare({ 96, 64 }, RenderLightShadowDiagnosticMode::ShadowAlpha);
	ASSERT_NE(advanced, nullptr);
	EXPECT_EQ(advanced->diagnosticMode, RenderLightShadowDiagnosticMode::ShadowAlpha);
	EXPECT_GT(advanced->light.version, firstVersion);
	EXPECT_EQ(advanced->shadow.version, advanced->light.version);
}

TEST(FrameCompositor, DevelopmentCompositionLightingInputsNeutralizeNonWorldLayers)
{
	std::array<uint8_t, 5> layerMap {
		static_cast<uint8_t>(RenderLayer::World),
		static_cast<uint8_t>(RenderLayer::WorldOverlay),
		static_cast<uint8_t>(RenderLayer::Interface),
		static_cast<uint8_t>(RenderLayer::Cursor),
		static_cast<uint8_t>(RenderLayer::Debug),
	};
	DevelopmentCompositionLightingInputs lightingInputs;

	const CompositionLightingInputs *prepared = lightingInputs.Prepare({ 5, 1 }, RenderLightShadowDiagnosticMode::FinalLitOutput, { layerMap.data(), 5, 1, 5 });

	ASSERT_NE(prepared, nullptr);
	ASSERT_TRUE(prepared->light.IsValid());
	ASSERT_TRUE(prepared->shadow.IsValid());
	const auto isNeutralLight = [&](const int x) {
		const uint8_t *pixel = prepared->light.pixels + x * 4;
		return pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 255 && pixel[3] == 255;
	};
	EXPECT_FALSE(isNeutralLight(0));
	EXPECT_FALSE(isNeutralLight(1));
	EXPECT_TRUE(isNeutralLight(2));
	EXPECT_TRUE(isNeutralLight(3));
	EXPECT_TRUE(isNeutralLight(4));
	EXPECT_EQ(prepared->shadow.pixels[2], 0);
	EXPECT_EQ(prepared->shadow.pixels[3], 0);
	EXPECT_EQ(prepared->shadow.pixels[4], 0);
}

TEST(FrameCompositor, PlansUnchangedAttachmentUploadAsSkip)
{
	std::array<uint8_t, 16> pixels {};
	CompositionAttachment attachment {
		CompositionAttachmentRole::IndexedAlbedo,
		CompositionAttachmentFormat::Index8,
		{ 4, 4 },
		4,
		7,
		{},
		pixels.data(),
	};

	const CompositionAttachmentUploadPlan plan = PlanCompositionAttachmentUpload(attachment, true, 7);

	EXPECT_EQ(plan.action, CompositionAttachmentUploadAction::Skip);
	EXPECT_TRUE(plan.rects.empty());
	EXPECT_EQ(plan.byteCount, 0);
}

TEST(FrameCompositor, PlansChangedAttachmentDirtyRegionsAsPartialUpload)
{
	std::array<uint8_t, 16> pixels {};
	CompositionAttachment attachment {
		CompositionAttachmentRole::IndexedAlbedo,
		CompositionAttachmentFormat::Index8,
		{ 4, 4 },
		4,
		7,
		{ { { { 1, 1 }, { 2, 2 } }, { { 3, 0 }, { 4, 1 } } }, false },
		pixels.data(),
	};

	const CompositionAttachmentUploadPlan plan = PlanCompositionAttachmentUpload(attachment, true, 7);

	EXPECT_EQ(plan.action, CompositionAttachmentUploadAction::Partial);
	ASSERT_EQ(plan.rects.size(), 2);
	EXPECT_EQ(plan.rects[0].position.x, 1);
	EXPECT_EQ(plan.rects[0].position.y, 1);
	EXPECT_EQ(plan.rects[0].size.width, 2);
	EXPECT_EQ(plan.rects[0].size.height, 2);
	EXPECT_EQ(plan.rects[1].position.x, 3);
	EXPECT_EQ(plan.rects[1].position.y, 0);
	EXPECT_EQ(plan.rects[1].size.width, 1);
	EXPECT_EQ(plan.rects[1].size.height, 1);
	EXPECT_EQ(plan.byteCount, 5);
}

TEST(FrameCompositor, PlansBackendResetAsFullUploadEvenWhenVersionMatches)
{
	std::array<uint8_t, 16> pixels {};
	CompositionAttachment attachment {
		CompositionAttachmentRole::LightAccumulation,
		CompositionAttachmentFormat::Rgba8,
		{ 2, 2 },
		8,
		11,
		{},
		pixels.data(),
	};

	const CompositionAttachmentUploadPlan plan = PlanCompositionAttachmentUpload(attachment, false, 11);

	EXPECT_EQ(plan.action, CompositionAttachmentUploadAction::Full);
	ASSERT_EQ(plan.rects.size(), 1);
	EXPECT_EQ(plan.rects[0].position.x, 0);
	EXPECT_EQ(plan.rects[0].position.y, 0);
	EXPECT_EQ(plan.rects[0].size.width, 2);
	EXPECT_EQ(plan.rects[0].size.height, 2);
	EXPECT_EQ(plan.byteCount, 16);
}

TEST(FrameCompositor, MakesIndexBufferViewFromEightBitSurface)
{
	SDLSurfaceUniquePtr surface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 2, 8, SDL_PIXELFORMAT_INDEX8);

	IndexBufferView view = MakeIndexBufferView(*surface);

	EXPECT_EQ(view.pixels, static_cast<uint8_t *>(surface->pixels));
	EXPECT_EQ(view.width, 3);
	EXPECT_EQ(view.height, 2);
	EXPECT_EQ(view.pitch, surface->pitch);
}

TEST(FrameCompositor, CpuPaletteCompositorExpandsPaletteIndices)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 2, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 2;
	pixels[indexSurface->pitch] = 3;
	pixels[indexSurface->pitch + 1] = 4;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 255, 0, 0, 255 };
	palette[2] = { 0, 255, 0, 255 };
	palette[3] = { 0, 0, 255, 255 };
	palette[4] = { 255, 255, 255, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 2, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ 2, 2 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 7));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();

	EXPECT_EQ(ReadColor(*outputSurface, 0, 0).r, 255);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 0).g, 255);
	EXPECT_EQ(ReadColor(*outputSurface, 0, 1).b, 255);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 1).r, 255);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 1).g, 255);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 1).b, 255);
}

TEST(FrameCompositor, CpuPaletteCompositorRespectsDirtyRects)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 2;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };
	palette[2] = { 200, 210, 220, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ 2, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 8));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.AddDirtyRect({ { 1, 0 }, { 1, 1 } });
	compositor.Compose();

	EXPECT_EQ(ReadColor(*outputSurface, 0, 0).r, 0);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 0).r, 200);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 0).g, 210);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 0).b, 220);

	const RenderPerfCompositionStats &stats = compositor.GetLastCompositionStats();
	EXPECT_FALSE(stats.fullFrameComposed);
	EXPECT_EQ(stats.fullFrameReason, CompositionFullFrameReason::None);
	EXPECT_EQ(stats.submittedDirtyRectCount, 1);
	EXPECT_EQ(stats.normalizedDirtyRectCount, 1);
	EXPECT_EQ(stats.composedRectCount, 1);
	EXPECT_EQ(stats.submittedDirtyArea, 1);
	EXPECT_EQ(stats.normalizedDirtyArea, 1);
	EXPECT_EQ(stats.composedPixelArea, 1);
}

TEST(FrameCompositor, CpuPaletteCompositorDelegatesNormalizedRectsToBackend)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 4, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 1;
	pixels[2] = 1;
	pixels[3] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 4, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	auto backend = std::make_unique<RecordingFrameCompositorBackend>();
	RecordingFrameCompositorBackend *backendPtr = backend.get();
	CpuPaletteCompositor compositor(std::move(backend));
	compositor.BeginFrame({ 4, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.AddDirtyRect({ { 0, 0 }, { 2, 1 } });
	compositor.AddDirtyRect({ { 1, 0 }, { 2, 1 } });
	compositor.Compose();

	EXPECT_EQ(backendPtr->composeCallCount, 1);
	EXPECT_EQ(backendPtr->observedOutputSurface, outputSurface.get());
	EXPECT_EQ(backendPtr->observedFrame.logicalSize.width, 4);
	EXPECT_EQ(backendPtr->observedFrame.indexBuffer.pixels, pixels);
	EXPECT_EQ(backendPtr->observedFrame.palette.version, 42);
	ASSERT_EQ(backendPtr->observedRects.size(), 1);
	EXPECT_EQ(backendPtr->observedRects[0].position.x, 0);
	EXPECT_EQ(backendPtr->observedRects[0].position.y, 0);
	EXPECT_EQ(backendPtr->observedRects[0].size.width, 3);
	EXPECT_EQ(backendPtr->observedRects[0].size.height, 1);

	const RenderPerfCompositionStats &stats = compositor.GetLastCompositionStats();
	EXPECT_EQ(stats.composedRectCount, 1);
	EXPECT_EQ(stats.composedPixelArea, 3);
	EXPECT_EQ(stats.selectedThreadCount, backendPtr->selectedThreadCount);
}

TEST(FrameCompositor, CpuPaletteCompositorPropagatesCompositionSurfaceMetadataToBackend)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 5, 4, 8, SDL_PIXELFORMAT_INDEX8);

	std::array<SDL_Color, 256> palette {};
	palette[0] = { 10, 20, 30, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 5, 4, 32, SDL_PIXELFORMAT_RGBA8888);

	auto backend = std::make_unique<RecordingFrameCompositorBackend>();
	RecordingFrameCompositorBackend *backendPtr = backend.get();
	CpuPaletteCompositor compositor(std::move(backend));
	compositor.BeginFrame({ 5, 4 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.AddDirtyRect({ { 0, 0 }, { 2, 2 } }, CompositionSurfaceRole::World);
	compositor.AddDirtyRect({ { 3, 1 }, { 1, 2 } }, CompositionSurfaceRole::Interface);
	compositor.AddDirtyRect({ { 1, 1 }, { 1, 1 } }, CompositionSurfaceRole::Interface);
	compositor.SetFullFrameDirty(CompositionSurfaceRole::Cursor);
	compositor.Compose();

	const CompositionSurfaceMetadata &metadata = backendPtr->observedFrame.compositionSurfaceMetadata;
	const CompositionSurfaceRoleCoverage &world = Coverage(metadata, CompositionSurfaceRole::World);
	EXPECT_EQ(world.dirtyRectCount, 1);
	EXPECT_EQ(world.dirtyPixelArea, 4);
	EXPECT_EQ(world.dirtyBounds.position.x, 0);
	EXPECT_EQ(world.dirtyBounds.position.y, 0);
	EXPECT_EQ(world.dirtyBounds.size.width, 2);
	EXPECT_EQ(world.dirtyBounds.size.height, 2);
	EXPECT_FALSE(world.fullFrameDirty);

	const CompositionSurfaceRoleCoverage &interface = Coverage(metadata, CompositionSurfaceRole::Interface);
	EXPECT_EQ(interface.dirtyRectCount, 2);
	EXPECT_EQ(interface.dirtyPixelArea, 3);
	EXPECT_EQ(interface.dirtyBounds.position.x, 1);
	EXPECT_EQ(interface.dirtyBounds.position.y, 1);
	EXPECT_EQ(interface.dirtyBounds.size.width, 3);
	EXPECT_EQ(interface.dirtyBounds.size.height, 2);
	EXPECT_FALSE(interface.fullFrameDirty);

	const CompositionSurfaceRoleCoverage &cursor = Coverage(metadata, CompositionSurfaceRole::Cursor);
	EXPECT_TRUE(cursor.fullFrameDirty);
	EXPECT_EQ(cursor.dirtyRectCount, 1);
	EXPECT_EQ(cursor.dirtyPixelArea, 20);
	EXPECT_EQ(cursor.dirtyBounds.position.x, 0);
	EXPECT_EQ(cursor.dirtyBounds.position.y, 0);
	EXPECT_EQ(cursor.dirtyBounds.size.width, 5);
	EXPECT_EQ(cursor.dirtyBounds.size.height, 4);

	compositor.Present();
	EXPECT_EQ(Coverage(compositor.GetCompositionSurfaceMetadata(), CompositionSurfaceRole::World).dirtyRectCount, 0);
	EXPECT_EQ(Coverage(compositor.GetCompositionSurfaceMetadata(), CompositionSurfaceRole::Interface).dirtyRectCount, 0);
	EXPECT_FALSE(Coverage(compositor.GetCompositionSurfaceMetadata(), CompositionSurfaceRole::Cursor).fullFrameDirty);
}

TEST(FrameCompositor, CpuPaletteCompositorAddsSharedIndexSurfaceRoleAttachments)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 5, 4, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);

	std::array<SDL_Color, 256> palette {};
	palette[0] = { 10, 20, 30, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 5, 4, 32, SDL_PIXELFORMAT_RGBA8888);

	auto backend = std::make_unique<RecordingFrameCompositorBackend>();
	RecordingFrameCompositorBackend *backendPtr = backend.get();
	CpuPaletteCompositor compositor(std::move(backend));
	compositor.BeginFrame({ 5, 4 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.AddDirtyRect({ { 0, 0 }, { 2, 2 } }, CompositionSurfaceRole::World);
	compositor.AddDirtyRect({ { 2, 0 }, { 2, 1 } }, CompositionSurfaceRole::WorldOverlay);
	compositor.AddDirtyRect({ { 3, 1 }, { 1, 2 } }, CompositionSurfaceRole::Interface);
	compositor.SetFullFrameDirty(CompositionSurfaceRole::Cursor);
	compositor.AddDirtyRect({ { 4, 0 }, { 1, 1 } }, CompositionSurfaceRole::DiagnosticOverlay);
	compositor.Compose();

	const CompositionFrame &observedFrame = backendPtr->observedFrame;
	const CompositionAttachment *world = FindCompositionAttachment(observedFrame.attachments, CompositionAttachmentRole::WorldIndex);
	const CompositionAttachment *worldOverlay = FindCompositionAttachment(observedFrame.attachments, CompositionAttachmentRole::WorldOverlayIndex);
	const CompositionAttachment *interface = FindCompositionAttachment(observedFrame.attachments, CompositionAttachmentRole::InterfaceIndex);
	const CompositionAttachment *cursor = FindCompositionAttachment(observedFrame.attachments, CompositionAttachmentRole::CursorIndex);
	const CompositionAttachment *diagnostic = FindCompositionAttachment(observedFrame.attachments, CompositionAttachmentRole::Diagnostic);
	ASSERT_NE(world, nullptr);
	ASSERT_NE(worldOverlay, nullptr);
	ASSERT_NE(interface, nullptr);
	ASSERT_NE(cursor, nullptr);
	ASSERT_NE(diagnostic, nullptr);
	EXPECT_EQ(world->cpuPixels, pixels);
	EXPECT_EQ(worldOverlay->cpuPixels, pixels);
	EXPECT_EQ(interface->cpuPixels, pixels);
	EXPECT_EQ(cursor->cpuPixels, pixels);
	EXPECT_EQ(diagnostic->cpuPixels, pixels);
	ASSERT_EQ(world->dirtyRects.rects.size(), 1);
	EXPECT_EQ(world->dirtyRects.rects[0].size.width, 2);
	ASSERT_EQ(worldOverlay->dirtyRects.rects.size(), 1);
	EXPECT_EQ(worldOverlay->dirtyRects.rects[0].position.x, 2);
	ASSERT_EQ(interface->dirtyRects.rects.size(), 1);
	EXPECT_EQ(interface->dirtyRects.rects[0].position.y, 1);
	EXPECT_TRUE(cursor->dirtyRects.fullFrame);
	ASSERT_EQ(diagnostic->dirtyRects.rects.size(), 1);

	const RenderPerfCompositionStats &stats = compositor.GetLastCompositionStats();
	EXPECT_EQ(stats.worldRoleDirtyRectCount, 1);
	EXPECT_EQ(stats.worldOverlayRoleDirtyRectCount, 1);
	EXPECT_EQ(stats.interfaceRoleDirtyRectCount, 1);
	EXPECT_EQ(stats.cursorRoleDirtyRectCount, 1);
	EXPECT_EQ(stats.diagnosticOverlayRoleDirtyRectCount, 1);
	EXPECT_EQ(stats.cursorRoleDirtyPixelArea, 20);
}

TEST(FrameCompositor, CpuPaletteCompositorAddsWorldMaskAttachments)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	std::array<uint8_t, 2> materialMap {
		static_cast<uint8_t>(RenderWorldMaterial::Floor),
		static_cast<uint8_t>(RenderWorldMaterial::Actor),
	};
	std::array<uint8_t, 2> receiverMap { 255, 0 };
	std::array<uint8_t, 2> occluderMap { 0, 255 };
	std::array<uint8_t, 2> emissiveMap { 0, 255 };
	DirtyRectList dirtyRects;
	dirtyRects.rects.push_back({ { 1, 0 }, { 1, 1 } });

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 32, SDL_PIXELFORMAT_RGBA8888);
	auto backend = std::make_unique<RecordingFrameCompositorBackend>();
	RecordingFrameCompositorBackend *backendPtr = backend.get();
	CpuPaletteCompositor compositor(std::move(backend));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 2, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 42),
	    dirtyRects,
	    false,
	    RenderLayerDiagnosticMode::Off,
	    {},
	    {},
	    {},
	    { materialMap.data(), receiverMap.data(), occluderMap.data(), emissiveMap.data(), 2, 1, 2, 77 },
	    RenderWorldMaskDiagnosticMode::Off,
	});

	ASSERT_EQ(backendPtr->composeCallCount, 1);
	const CompositionFrame &observedFrame = backendPtr->observedFrame;
	const CompositionAttachment *material = FindCompositionAttachment(observedFrame.attachments, CompositionAttachmentRole::WorldMaterial);
	const CompositionAttachment *receiver = FindCompositionAttachment(observedFrame.attachments, CompositionAttachmentRole::WorldReceiver);
	const CompositionAttachment *occluder = FindCompositionAttachment(observedFrame.attachments, CompositionAttachmentRole::WorldOccluder);
	ASSERT_NE(material, nullptr);
	ASSERT_NE(receiver, nullptr);
	ASSERT_NE(occluder, nullptr);
	EXPECT_EQ(material->format, CompositionAttachmentFormat::Index8);
	EXPECT_EQ(receiver->format, CompositionAttachmentFormat::Alpha8);
	EXPECT_EQ(occluder->format, CompositionAttachmentFormat::Alpha8);
	EXPECT_EQ(material->logicalSize, Size(2, 1));
	EXPECT_EQ(material->pitch, 2);
	EXPECT_EQ(material->version, 77);
	EXPECT_EQ(receiver->version, 77);
	EXPECT_EQ(occluder->version, 77);
	EXPECT_EQ(material->cpuPixels, materialMap.data());
	EXPECT_EQ(receiver->cpuPixels, receiverMap.data());
	EXPECT_EQ(occluder->cpuPixels, occluderMap.data());
	ASSERT_EQ(material->dirtyRects.rects.size(), 1);
	EXPECT_EQ(material->dirtyRects.rects[0].position.x, 1);
	EXPECT_EQ(receiver->dirtyRects.rects.size(), 1);
	EXPECT_EQ(occluder->dirtyRects.rects.size(), 1);
	EXPECT_EQ(backendPtr->observedFrame.worldMaskMap.emissivePixels, emissiveMap.data());
}

TEST(FrameCompositor, CpuPaletteCompositorReportsDirectPresentationFromBackend)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	auto backend = std::make_unique<RecordingFrameCompositorBackend>();
	RecordingFrameCompositorBackend *backendPtr = backend.get();
	backendPtr->composeResult = FrameCompositorBackendResult::PreparedDirectPresentation;
	CpuPaletteCompositor compositor(std::move(backend));
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();

	EXPECT_EQ(compositor.GetLastBackendResult(), FrameCompositorBackendResult::PreparedDirectPresentation);
	EXPECT_EQ(backendPtr->composeCallCount, 1);

	compositor.Present();
	EXPECT_EQ(backendPtr->presentCallCount, 1);
	compositor.Present();
	EXPECT_EQ(backendPtr->presentCallCount, 1);
}

TEST(FrameCompositor, AcceleratedPaletteBackendPresentsIndexedFrame)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	auto presenter = std::make_unique<RecordingAcceleratedPalettePresenter>();
	RecordingAcceleratedPalettePresenter *presenterPtr = presenter.get();
	std::unique_ptr<IFrameCompositorBackend> backend = CreateAcceleratedPaletteCompositorBackend(std::move(presenter));
	ASSERT_NE(backend, nullptr);

	CpuPaletteCompositor compositor(std::move(backend));
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();

	EXPECT_EQ(compositor.GetLastBackendResult(), FrameCompositorBackendResult::PreparedDirectPresentation);
	EXPECT_EQ(presenterPtr->indexedFrameCallCount, 1);
	EXPECT_EQ(presenterPtr->outputSurfaceFrameCallCount, 0);
	EXPECT_EQ(presenterPtr->observedIndexedFrame.indexBuffer.pixels, pixels);
	EXPECT_EQ(presenterPtr->observedIndexedFrame.palette.version, 42);
	const CompositionAttachment *indexAttachment = FindCompositionAttachment(presenterPtr->observedIndexedFrame.attachments, CompositionAttachmentRole::IndexedAlbedo);
	ASSERT_NE(indexAttachment, nullptr);
	EXPECT_EQ(indexAttachment->format, CompositionAttachmentFormat::Index8);
	EXPECT_EQ(indexAttachment->logicalSize.width, 1);
	EXPECT_EQ(indexAttachment->logicalSize.height, 1);
	EXPECT_TRUE(indexAttachment->dirtyRects.fullFrame);
	const CompositionAttachment *paletteAttachment = FindCompositionAttachment(presenterPtr->observedIndexedFrame.attachments, CompositionAttachmentRole::Palette);
	ASSERT_NE(paletteAttachment, nullptr);
	EXPECT_EQ(paletteAttachment->format, CompositionAttachmentFormat::PaletteRgba8);
	EXPECT_EQ(paletteAttachment->version, 42);
	ASSERT_NE(presenterPtr->observedIndexedLighting, nullptr);
	EXPECT_TRUE(presenterPtr->observedIndexedLighting->light.IsValid());
	EXPECT_EQ(presenterPtr->observedIndexedLighting->light.size.width, 1);
	EXPECT_EQ(presenterPtr->observedIndexedLighting->light.size.height, 1);
	EXPECT_EQ(presenterPtr->observedIndexedLighting->light.pitch, 4);
	EXPECT_EQ(presenterPtr->observedIndexedLighting->light.format, CompositionLightingBufferFormat::Rgba8);
	EXPECT_EQ(presenterPtr->observedIndexedLighting->light.version, 1);
	EXPECT_EQ(presenterPtr->observedIndexedLighting->light.pixels[0], 255);
	EXPECT_TRUE(presenterPtr->observedIndexedLighting->shadow.IsValid());
	EXPECT_EQ(presenterPtr->observedIndexedLighting->shadow.size.width, 1);
	EXPECT_EQ(presenterPtr->observedIndexedLighting->shadow.size.height, 1);
	EXPECT_EQ(presenterPtr->observedIndexedLighting->shadow.pitch, 1);
	EXPECT_EQ(presenterPtr->observedIndexedLighting->shadow.format, CompositionLightingBufferFormat::Alpha8);
	EXPECT_EQ(presenterPtr->observedIndexedLighting->shadow.version, 1);
	EXPECT_EQ(presenterPtr->observedIndexedLighting->shadow.pixels[0], 0);
	EXPECT_EQ(compositor.GetLastCompositionStats().selectedThreadCount, 1);

	compositor.Present();
	EXPECT_EQ(presenterPtr->presentCallCount, 1);
	compositor.Present();
	EXPECT_EQ(presenterPtr->presentCallCount, 1);
}

TEST(FrameCompositor, AcceleratedPaletteBackendRetainsNoDirtyDirectPresentationExplicitly)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	auto presenter = std::make_unique<RecordingAcceleratedPalettePresenter>();
	RecordingAcceleratedPalettePresenter *presenterPtr = presenter.get();
	std::unique_ptr<IFrameCompositorBackend> backend = CreateAcceleratedPaletteCompositorBackend(std::move(presenter));
	ASSERT_NE(backend, nullptr);

	CpuPaletteCompositor compositor(std::move(backend));
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();
	EXPECT_EQ(compositor.GetLastBackendResult(), FrameCompositorBackendResult::PreparedDirectPresentation);
	compositor.Present();
	EXPECT_EQ(presenterPtr->presentCallCount, 1);

	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.Compose();

	EXPECT_EQ(compositor.GetLastBackendResult(), FrameCompositorBackendResult::RetainedDirectPresentation);
	EXPECT_EQ(presenterPtr->indexedFrameCallCount, 1);
	EXPECT_EQ(presenterPtr->outputSurfaceFrameCallCount, 0);
	EXPECT_EQ(compositor.GetLastCompositionStats().composedRectCount, 0);
	EXPECT_EQ(compositor.GetLastCompositionStats().backendRetainedDirectPresentationCount, 1);

	compositor.Present();
	EXPECT_EQ(presenterPtr->presentCallCount, 2);
	compositor.Present();
	EXPECT_EQ(presenterPtr->presentCallCount, 2);
}

TEST(FrameCompositor, AcceleratedPaletteBackendForwardsCompositionSurfaceMetadata)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 2, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 2, 32, SDL_PIXELFORMAT_RGBA8888);

	auto presenter = std::make_unique<RecordingAcceleratedPalettePresenter>();
	RecordingAcceleratedPalettePresenter *presenterPtr = presenter.get();
	std::unique_ptr<IFrameCompositorBackend> backend = CreateAcceleratedPaletteCompositorBackend(std::move(presenter));
	ASSERT_NE(backend, nullptr);

	CpuPaletteCompositor compositor(std::move(backend));
	compositor.BeginFrame({ 2, 2 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.AddDirtyRect({ { 1, 0 }, { 1, 2 } }, CompositionSurfaceRole::DiagnosticOverlay);
	compositor.Compose();

	EXPECT_EQ(compositor.GetLastBackendResult(), FrameCompositorBackendResult::PreparedDirectPresentation);
	EXPECT_EQ(presenterPtr->indexedFrameCallCount, 1);
	ASSERT_EQ(presenterPtr->observedUploadDirtyRects.size(), 1);
	EXPECT_EQ(presenterPtr->observedUploadDirtyRects[0].position.x, 1);
	EXPECT_EQ(presenterPtr->observedUploadDirtyRects[0].position.y, 0);
	EXPECT_EQ(presenterPtr->observedUploadDirtyRects[0].size.width, 1);
	EXPECT_EQ(presenterPtr->observedUploadDirtyRects[0].size.height, 2);
	const CompositionSurfaceRoleCoverage &diagnosticOverlay = Coverage(presenterPtr->observedIndexedFrame.compositionSurfaceMetadata, CompositionSurfaceRole::DiagnosticOverlay);
	EXPECT_EQ(diagnosticOverlay.dirtyRectCount, 1);
	EXPECT_EQ(diagnosticOverlay.dirtyPixelArea, 2);
	EXPECT_EQ(diagnosticOverlay.dirtyBounds.position.x, 1);
	EXPECT_EQ(diagnosticOverlay.dirtyBounds.position.y, 0);
	EXPECT_EQ(diagnosticOverlay.dirtyBounds.size.width, 1);
	EXPECT_EQ(diagnosticOverlay.dirtyBounds.size.height, 2);
}

TEST(FrameCompositor, AcceleratedPaletteBackendUsesIndexedPathWithLayerMapWhenDiagnosticsAreOff)
{
	RenderLightShadowDiagnosticModeGuard diagnosticMode(RenderLightShadowDiagnosticMode::Off);
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	std::array<uint8_t, 1> layerMap { static_cast<uint8_t>(RenderLayer::Interface) };
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	auto presenter = std::make_unique<RecordingAcceleratedPalettePresenter>();
	RecordingAcceleratedPalettePresenter *presenterPtr = presenter.get();
	std::unique_ptr<IFrameCompositorBackend> backend = CreateAcceleratedPaletteCompositorBackend(std::move(presenter));
	ASSERT_NE(backend, nullptr);

	CpuPaletteCompositor compositor(std::move(backend));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 1, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 42),
	    { {}, true },
	    false,
	    RenderLayerDiagnosticMode::Off,
	    { layerMap.data(), 1, 1, 1 },
	    {},
	    {},
	    {},
	    RenderWorldMaskDiagnosticMode::Off,
	});

	EXPECT_EQ(compositor.GetLastBackendResult(), FrameCompositorBackendResult::PreparedDirectPresentation);
	EXPECT_EQ(presenterPtr->indexedFrameCallCount, 1);
	EXPECT_EQ(presenterPtr->outputSurfaceFrameCallCount, 0);
	EXPECT_EQ(presenterPtr->observedIndexedFrame.renderLayerMap.pixels, layerMap.data());
}

TEST(FrameCompositor, AcceleratedPaletteBackendUsesCpuPixelsForDiagnostics)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	auto presenter = std::make_unique<RecordingAcceleratedPalettePresenter>();
	RecordingAcceleratedPalettePresenter *presenterPtr = presenter.get();
	std::unique_ptr<IFrameCompositorBackend> backend = CreateAcceleratedPaletteCompositorBackend(std::move(presenter));
	ASSERT_NE(backend, nullptr);

	CpuPaletteCompositor compositor(std::move(backend));
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.SetDiagnosticTransformEnabled(true);
	compositor.SetFullFrameDirty();
	compositor.Compose();

	EXPECT_EQ(compositor.GetLastBackendResult(), FrameCompositorBackendResult::PreparedDirectPresentation);
	EXPECT_EQ(presenterPtr->indexedFrameCallCount, 0);
	EXPECT_EQ(presenterPtr->outputSurfaceFrameCallCount, 1);
	EXPECT_EQ(presenterPtr->observedOutputSurface, outputSurface.get());
	EXPECT_EQ(presenterPtr->observedOutputFrame.indexBuffer.pixels, pixels);
	EXPECT_TRUE(presenterPtr->observedOutputFrame.diagnosticTransform);
	EXPECT_EQ(presenterPtr->observedOutputLighting, nullptr);

	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_NE(color.r, palette[1].r);
	EXPECT_NE(color.b, palette[1].b);
}

TEST(FrameCompositor, AcceleratedPaletteBackendUsesCpuPixelsForWorldMaskDiagnostics)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	std::array<uint8_t, 1> materialMap { static_cast<uint8_t>(RenderWorldMaterial::Actor) };
	std::array<uint8_t, 1> receiverMap { 255 };
	std::array<uint8_t, 1> occluderMap { 255 };
	std::array<uint8_t, 1> emissiveMap { 255 };
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	auto presenter = std::make_unique<RecordingAcceleratedPalettePresenter>();
	RecordingAcceleratedPalettePresenter *presenterPtr = presenter.get();
	std::unique_ptr<IFrameCompositorBackend> backend = CreateAcceleratedPaletteCompositorBackend(std::move(presenter));
	ASSERT_NE(backend, nullptr);

	CpuPaletteCompositor compositor(std::move(backend));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 1, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 42),
	    FullFrameDirtyRectsForTest(),
	    false,
	    RenderLayerDiagnosticMode::Off,
	    {},
	    {},
	    {},
	    { materialMap.data(), receiverMap.data(), occluderMap.data(), emissiveMap.data(), 1, 1, 1, 7 },
	    RenderWorldMaskDiagnosticMode::Material,
	});

	EXPECT_EQ(compositor.GetLastBackendResult(), FrameCompositorBackendResult::PreparedDirectPresentation);
	EXPECT_EQ(presenterPtr->indexedFrameCallCount, 0);
	EXPECT_EQ(presenterPtr->outputSurfaceFrameCallCount, 1);
	EXPECT_EQ(presenterPtr->observedOutputSurface, outputSurface.get());
	EXPECT_EQ(presenterPtr->observedOutputFrame.renderWorldMaskDiagnosticMode, RenderWorldMaskDiagnosticMode::Material);
	EXPECT_EQ(presenterPtr->observedOutputFrame.worldMaskMap.materialPixels, materialMap.data());
	EXPECT_EQ(presenterPtr->observedOutputLighting, nullptr);

	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, 255);
	EXPECT_EQ(color.g, 64);
	EXPECT_EQ(color.b, 220);
}

TEST(FrameCompositor, AcceleratedPaletteBackendForwardsLightingInputs)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	std::array<uint8_t, 4> lightPixels { 255, 255, 255, 255 };
	CompositionLightingInputs lightingInputs {};
	lightingInputs.light = {
		lightPixels.data(),
		{ 1, 1 },
		4,
		CompositionLightingBufferFormat::Rgba8,
		9,
		{},
	};
	EXPECT_TRUE(lightingInputs.light.IsValid());

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	auto presenter = std::make_unique<RecordingAcceleratedPalettePresenter>();
	RecordingAcceleratedPalettePresenter *presenterPtr = presenter.get();
	std::unique_ptr<IFrameCompositorBackend> backend = CreateAcceleratedPaletteCompositorBackend(std::move(presenter), &lightingInputs);
	ASSERT_NE(backend, nullptr);

	CpuPaletteCompositor compositor(std::move(backend));
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();

	EXPECT_EQ(compositor.GetLastBackendResult(), FrameCompositorBackendResult::PreparedDirectPresentation);
	EXPECT_EQ(presenterPtr->indexedFrameCallCount, 1);
	EXPECT_EQ(presenterPtr->observedIndexedFrame.indexBuffer.pixels, pixels);
	EXPECT_EQ(presenterPtr->observedIndexedLighting, &lightingInputs);
}

TEST(FrameCompositor, AcceleratedPaletteBackendUsesDevelopmentLightingDiagnosticOption)
{
	RenderLightShadowDiagnosticModeGuard diagnosticMode(RenderLightShadowDiagnosticMode::LightRgb);
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 2, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 1;
	pixels[indexSurface->pitch] = 1;
	pixels[indexSurface->pitch + 1] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 2, 32, SDL_PIXELFORMAT_RGBA8888);

	auto presenter = std::make_unique<RecordingAcceleratedPalettePresenter>();
	RecordingAcceleratedPalettePresenter *presenterPtr = presenter.get();
	std::unique_ptr<IFrameCompositorBackend> backend = CreateAcceleratedPaletteCompositorBackend(std::move(presenter));
	ASSERT_NE(backend, nullptr);

	CpuPaletteCompositor compositor(std::move(backend));
	compositor.BeginFrame({ 2, 2 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();

	ASSERT_NE(presenterPtr->observedIndexedLighting, nullptr);
	EXPECT_EQ(presenterPtr->observedIndexedLighting->diagnosticMode, RenderLightShadowDiagnosticMode::LightRgb);
	EXPECT_TRUE(presenterPtr->observedIndexedLighting->light.dirtyRects.fullFrame);
	EXPECT_TRUE(presenterPtr->observedIndexedLighting->shadow.dirtyRects.fullFrame);
	EXPECT_EQ(presenterPtr->observedIndexedLighting->light.size, Size(2, 2));
	EXPECT_EQ(presenterPtr->observedIndexedLighting->shadow.size, Size(2, 2));
}

TEST(FrameCompositor, AcceleratedPaletteBackendLightingDiagnosticUsesLayerMapIsolation)
{
	RenderLightShadowDiagnosticModeGuard diagnosticMode(RenderLightShadowDiagnosticMode::FinalLitOutput);
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 4, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 1;
	pixels[2] = 1;
	pixels[3] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	std::array<uint8_t, 4> layerMap {
		static_cast<uint8_t>(RenderLayer::World),
		static_cast<uint8_t>(RenderLayer::Interface),
		static_cast<uint8_t>(RenderLayer::Cursor),
		static_cast<uint8_t>(RenderLayer::Debug),
	};
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 4, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	auto presenter = std::make_unique<RecordingAcceleratedPalettePresenter>();
	RecordingAcceleratedPalettePresenter *presenterPtr = presenter.get();
	std::unique_ptr<IFrameCompositorBackend> backend = CreateAcceleratedPaletteCompositorBackend(std::move(presenter));
	ASSERT_NE(backend, nullptr);

	CpuPaletteCompositor compositor(std::move(backend));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 4, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 42),
	    { {}, true },
	    false,
	    RenderLayerDiagnosticMode::Off,
	    { layerMap.data(), 4, 1, 4 },
	    {},
	    {},
	    {},
	    RenderWorldMaskDiagnosticMode::Off,
	});

	ASSERT_NE(presenterPtr->observedIndexedLighting, nullptr);
	const uint8_t *light = presenterPtr->observedIndexedLighting->light.pixels;
	const uint8_t *shadow = presenterPtr->observedIndexedLighting->shadow.pixels;
	for (int x = 1; x < 4; x++) {
		const uint8_t *pixel = light + x * 4;
		EXPECT_EQ(pixel[0], 255);
		EXPECT_EQ(pixel[1], 255);
		EXPECT_EQ(pixel[2], 255);
		EXPECT_EQ(pixel[3], 255);
		EXPECT_EQ(shadow[x], 0);
	}
}

TEST(FrameCompositor, AcceleratedPaletteBackendUploadsCpuPixelsWhenIndexedUploadFails)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	auto presenter = std::make_unique<RecordingAcceleratedPalettePresenter>();
	RecordingAcceleratedPalettePresenter *presenterPtr = presenter.get();
	presenterPtr->indexedFrameResult = false;
	std::unique_ptr<IFrameCompositorBackend> backend = CreateAcceleratedPaletteCompositorBackend(std::move(presenter));
	ASSERT_NE(backend, nullptr);

	CpuPaletteCompositor compositor(std::move(backend));
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();

	EXPECT_EQ(compositor.GetLastBackendResult(), FrameCompositorBackendResult::PreparedDirectPresentation);
	EXPECT_EQ(presenterPtr->indexedFrameCallCount, 1);
	EXPECT_EQ(presenterPtr->outputSurfaceFrameCallCount, 1);
	EXPECT_EQ(compositor.GetLastCompositionStats().failedUploadCount, 1);
	EXPECT_EQ(compositor.GetLastCompositionStats().uploadFallbackReason, CompositionUploadFallbackReason::SubmitFailed);
	EXPECT_EQ(presenterPtr->observedOutputSurface, outputSurface.get());
	EXPECT_EQ(presenterPtr->observedOutputFrame.indexBuffer.pixels, pixels);
	EXPECT_EQ(presenterPtr->observedOutputLighting, nullptr);
	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, palette[1].r);
	EXPECT_EQ(color.g, palette[1].g);
	EXPECT_EQ(color.b, palette[1].b);
}

TEST(FrameCompositor, AcceleratedPaletteBackendLeavesCpuSurfaceResultWhenAllUploadsFail)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	auto presenter = std::make_unique<RecordingAcceleratedPalettePresenter>();
	RecordingAcceleratedPalettePresenter *presenterPtr = presenter.get();
	presenterPtr->indexedFrameResult = false;
	presenterPtr->outputSurfaceFrameResult = false;
	std::unique_ptr<IFrameCompositorBackend> backend = CreateAcceleratedPaletteCompositorBackend(std::move(presenter));
	ASSERT_NE(backend, nullptr);

	CpuPaletteCompositor compositor(std::move(backend));
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();

	EXPECT_EQ(compositor.GetLastBackendResult(), FrameCompositorBackendResult::UpdatedOutputSurface);
	EXPECT_EQ(presenterPtr->indexedFrameCallCount, 1);
	EXPECT_EQ(presenterPtr->outputSurfaceFrameCallCount, 1);
	EXPECT_EQ(compositor.GetLastCompositionStats().failedUploadCount, 2);
	EXPECT_EQ(compositor.GetLastCompositionStats().uploadFallbackReason, CompositionUploadFallbackReason::SubmitFailed);
	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, palette[1].r);
	EXPECT_EQ(color.g, palette[1].g);
	EXPECT_EQ(color.b, palette[1].b);

	compositor.Present();
	EXPECT_EQ(presenterPtr->presentCallCount, 0);
}

TEST(FrameCompositor, AcceleratedPaletteBackendFallsBackToCpuWhenPresenterBecomesUnavailable)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	auto presenter = std::make_unique<RecordingAcceleratedPalettePresenter>();
	RecordingAcceleratedPalettePresenter *presenterPtr = presenter.get();
	std::unique_ptr<IFrameCompositorBackend> backend = CreateAcceleratedPaletteCompositorBackend(std::move(presenter));
	ASSERT_NE(backend, nullptr);
	presenterPtr->available = false;

	CpuPaletteCompositor compositor(std::move(backend));
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();

	EXPECT_EQ(compositor.GetLastBackendResult(), FrameCompositorBackendResult::UpdatedOutputSurface);
	EXPECT_EQ(presenterPtr->indexedFrameCallCount, 0);
	EXPECT_EQ(presenterPtr->outputSurfaceFrameCallCount, 0);
	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, palette[1].r);
	EXPECT_EQ(color.g, palette[1].g);
	EXPECT_EQ(color.b, palette[1].b);
}

TEST(FrameCompositor, AcceleratedPaletteBackendRefreshesCpuSurfaceWhenRetainedPresenterResets)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	auto presenter = std::make_unique<RecordingAcceleratedPalettePresenter>();
	RecordingAcceleratedPalettePresenter *presenterPtr = presenter.get();
	std::unique_ptr<IFrameCompositorBackend> backend = CreateAcceleratedPaletteCompositorBackend(std::move(presenter));
	ASSERT_NE(backend, nullptr);

	CpuPaletteCompositor compositor(std::move(backend));
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();
	EXPECT_EQ(compositor.GetLastBackendResult(), FrameCompositorBackendResult::PreparedDirectPresentation);
	compositor.Present();

	presenterPtr->available = false;
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 42));
	compositor.Compose();

	EXPECT_EQ(compositor.GetLastBackendResult(), FrameCompositorBackendResult::UpdatedOutputSurface);
	EXPECT_EQ(compositor.GetLastCompositionStats().fullFrameReason, CompositionFullFrameReason::DirectPresentationUnavailable);
	EXPECT_EQ(presenterPtr->indexedFrameCallCount, 1);
	EXPECT_EQ(presenterPtr->outputSurfaceFrameCallCount, 0);
	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, palette[1].r);
	EXPECT_EQ(color.g, palette[1].g);
	EXPECT_EQ(color.b, palette[1].b);
}

TEST(FrameCompositor, CpuPaletteCompositorRecomposesFullFrameWhenPaletteVersionChanges)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 255, 0, 0, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 1));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose();
	compositor.Present();
	EXPECT_EQ(ReadColor(*outputSurface, 0, 0).r, 255);

	palette[1] = { 0, 255, 0, 255 };
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 2));
	compositor.Compose();

	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, 0);
	EXPECT_EQ(color.g, 255);

	const RenderPerfCompositionStats &stats = compositor.GetLastCompositionStats();
	EXPECT_TRUE(stats.fullFrameComposed);
	EXPECT_EQ(stats.fullFrameReason, CompositionFullFrameReason::PaletteChanged);
	EXPECT_EQ(stats.composedRectCount, 1);
	EXPECT_EQ(stats.composedPixelArea, 1);
}

TEST(FrameCompositor, CpuPaletteCompositorInvalidatesMappedPaletteWhenOutputFormatChanges)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 80, 200, 255 };

	SDLSurfaceUniquePtr outputRgba = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);
	SDLSurfaceUniquePtr outputBgra = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_BGRA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 1));
	compositor.SetOutputSurface(outputRgba.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();
	compositor.Present();

	SDL_Color color = ReadColor(*outputRgba, 0, 0);
	EXPECT_EQ(color.r, 10);
	EXPECT_EQ(color.g, 80);
	EXPECT_EQ(color.b, 200);

	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 1));
	compositor.SetOutputSurface(outputBgra.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();

	color = ReadColor(*outputBgra, 0, 0);
	EXPECT_EQ(color.r, 10);
	EXPECT_EQ(color.g, 80);
	EXPECT_EQ(color.b, 200);
}

TEST(FrameCompositor, CpuPaletteCompositorTreatsEmptyDirtyRectsAsInitialFullFrameOnly)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 255, 0, 0, 255 };
	palette[2] = { 0, 255, 0, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ 2, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 1));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose();
	compositor.Present();
	EXPECT_EQ(ReadColor(*outputSurface, 0, 0).r, 255);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 0).r, 255);

	pixels[0] = 2;
	compositor.BeginFrame({ 2, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 1));
	compositor.Compose();

	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, 255);
	EXPECT_EQ(color.g, 0);
}

TEST(FrameCompositor, CpuPaletteCompositorClipsDirtyRectsToOutputBounds)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 1;
	pixels[2] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 90, 10, 20, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ 3, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 1));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.AddDirtyRect({ { -1, 0 }, { 2, 1 } });
	compositor.Compose();

	EXPECT_EQ(ReadColor(*outputSurface, 0, 0).r, 90);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 0).r, 0);
	EXPECT_EQ(ReadColor(*outputSurface, 2, 0).r, 0);
}

TEST(FrameCompositor, CpuPaletteCompositorEscalatesManyDirtyRectsToFullFrame)
{
	constexpr int Width = 70;
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, Width, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	for (int x = 0; x < Width; x++) {
		pixels[x] = 1;
	}

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 40, 50, 60, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, Width, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ Width, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 1));
	compositor.SetOutputSurface(outputSurface.get());
	for (int x = 0; x < 65; x++) {
		compositor.AddDirtyRect({ { x, 0 }, { 1, 1 } });
	}
	compositor.Compose();

	EXPECT_EQ(ReadColor(*outputSurface, 0, 0).r, 40);
	EXPECT_EQ(ReadColor(*outputSurface, Width - 1, 0).r, 40);

	const RenderPerfCompositionStats &stats = compositor.GetLastCompositionStats();
	EXPECT_TRUE(stats.fullFrameComposed);
	EXPECT_EQ(stats.fullFrameReason, CompositionFullFrameReason::TooManyDirtyRects);
	EXPECT_EQ(stats.submittedDirtyRectCount, 65);
	EXPECT_EQ(stats.normalizedDirtyRectCount, 0);
	EXPECT_EQ(stats.submittedDirtyArea, 65);
	EXPECT_EQ(stats.normalizedDirtyArea, Width);
	EXPECT_EQ(stats.composedRectCount, 1);
	EXPECT_EQ(stats.composedPixelArea, Width);
}

TEST(FrameCompositor, CpuPaletteCompositorAppliesDiagnosticTransformAfterPaletteExpansion)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 100, 150, 200, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 9));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.SetDiagnosticTransformEnabled(true);
	compositor.SetFullFrameDirty();
	compositor.Compose();

	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_NE(color.r, 100);
	EXPECT_NE(color.g, 150);
	EXPECT_NE(color.b, 200);
}

TEST(FrameCompositor, RenderLayerDiagnosticOffKeepsPaletteExactOutput)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 20, 40, 60, 255 };

	std::array<uint8_t, 1> layerMap { static_cast<uint8_t>(RenderLayer::Cursor) };
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 1, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 1),
	    { {}, true },
	    false,
	    RenderLayerDiagnosticMode::Off,
	    { layerMap.data(), 1, 1, 1 },
	    {},
	    {},
	    {},
	    RenderWorldMaskDiagnosticMode::Off,
	});

	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, 20);
	EXPECT_EQ(color.g, 40);
	EXPECT_EQ(color.b, 60);
}

TEST(FrameCompositor, RenderWorldMaskDiagnosticOffKeepsPaletteExactOutput)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 20, 40, 60, 255 };

	std::array<uint8_t, 1> materialMap { static_cast<uint8_t>(RenderWorldMaterial::Actor) };
	std::array<uint8_t, 1> receiverMap { 255 };
	std::array<uint8_t, 1> occluderMap { 255 };
	std::array<uint8_t, 1> emissiveMap { 255 };
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 1, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 1),
	    FullFrameDirtyRectsForTest(),
	    false,
	    RenderLayerDiagnosticMode::Off,
	    {},
	    {},
	    {},
	    { materialMap.data(), receiverMap.data(), occluderMap.data(), emissiveMap.data(), 1, 1, 1, 9 },
	    RenderWorldMaskDiagnosticMode::Off,
	});

	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, 20);
	EXPECT_EQ(color.g, 40);
	EXPECT_EQ(color.b, 60);
}

TEST(FrameCompositor, RenderWorldMaskDiagnosticMaterialIsWorldOnly)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 20, 40, 60, 255 };

	std::array<uint8_t, 2> layerMap {
		static_cast<uint8_t>(RenderLayer::World),
		static_cast<uint8_t>(RenderLayer::Interface),
	};
	std::array<uint8_t, 2> materialMap {
		static_cast<uint8_t>(RenderWorldMaterial::Floor),
		static_cast<uint8_t>(RenderWorldMaterial::Actor),
	};
	std::array<uint8_t, 2> receiverMap { 0, 0 };
	std::array<uint8_t, 2> occluderMap { 0, 0 };
	std::array<uint8_t, 2> emissiveMap { 0, 0 };
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 2, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 1),
	    FullFrameDirtyRectsForTest(),
	    false,
	    RenderLayerDiagnosticMode::Off,
	    { layerMap.data(), 2, 1, 2 },
	    {},
	    {},
	    { materialMap.data(), receiverMap.data(), occluderMap.data(), emissiveMap.data(), 2, 1, 2, 9 },
	    RenderWorldMaskDiagnosticMode::Material,
	});

	const SDL_Color world = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(world.r, 48);
	EXPECT_EQ(world.g, 180);
	EXPECT_EQ(world.b, 80);

	const SDL_Color interface = ReadColor(*outputSurface, 1, 0);
	EXPECT_EQ(interface.r, 20);
	EXPECT_EQ(interface.g, 40);
	EXPECT_EQ(interface.b, 60);
}

TEST(FrameCompositor, RenderWorldMaskDiagnosticReceiverUsesMaskValues)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 20, 40, 60, 255 };

	std::array<uint8_t, 2> materialMap {
		static_cast<uint8_t>(RenderWorldMaterial::Floor),
		static_cast<uint8_t>(RenderWorldMaterial::Floor),
	};
	std::array<uint8_t, 2> receiverMap { 255, 0 };
	std::array<uint8_t, 2> occluderMap { 0, 0 };
	std::array<uint8_t, 2> emissiveMap { 0, 0 };
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 2, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 1),
	    FullFrameDirtyRectsForTest(),
	    false,
	    RenderLayerDiagnosticMode::Off,
	    {},
	    {},
	    {},
	    { materialMap.data(), receiverMap.data(), occluderMap.data(), emissiveMap.data(), 2, 1, 2, 9 },
	    RenderWorldMaskDiagnosticMode::Receiver,
	});

	const SDL_Color receiver = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(receiver.r, 64);
	EXPECT_EQ(receiver.g, 255);
	EXPECT_EQ(receiver.b, 96);

	const SDL_Color nonReceiver = ReadColor(*outputSurface, 1, 0);
	EXPECT_EQ(nonReceiver.r, 32);
	EXPECT_EQ(nonReceiver.g, 32);
	EXPECT_EQ(nonReceiver.b, 32);
}

TEST(FrameCompositor, RenderLayerDiagnosticTintBlendsLayerColor)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 100, 100, 100, 255 };

	std::array<uint8_t, 1> layerMap { static_cast<uint8_t>(RenderLayer::Interface) };
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 1, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 1),
	    { {}, true },
	    false,
	    RenderLayerDiagnosticMode::Tint,
	    { layerMap.data(), 1, 1, 1 },
	    {},
	    {},
	    {},
	    RenderWorldMaskDiagnosticMode::Off,
	});

	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, 50);
	EXPECT_EQ(color.g, 98);
	EXPECT_EQ(color.b, 177);
}

TEST(FrameCompositor, RenderLayerDiagnosticOutlineMarksLayerBoundaries)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 100, 100, 100, 255 };

	std::array<uint8_t, 2> layerMap {
		static_cast<uint8_t>(RenderLayer::World),
		static_cast<uint8_t>(RenderLayer::Interface),
	};
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 2, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 1),
	    { {}, true },
	    false,
	    RenderLayerDiagnosticMode::Outline,
	    { layerMap.data(), 2, 1, 2 },
	    {},
	    {},
	    {},
	    RenderWorldMaskDiagnosticMode::Off,
	});

	const SDL_Color worldBoundary = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(worldBoundary.r, 0);
	EXPECT_EQ(worldBoundary.g, 255);
	EXPECT_EQ(worldBoundary.b, 0);

	const SDL_Color interfaceBoundary = ReadColor(*outputSurface, 1, 0);
	EXPECT_EQ(interfaceBoundary.r, 0);
	EXPECT_EQ(interfaceBoundary.g, 96);
	EXPECT_EQ(interfaceBoundary.b, 255);
}

TEST(FrameCompositor, RenderLayerDiagnosticTintAndOutlineCombinesEffects)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 1;
	pixels[2] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 100, 100, 100, 255 };

	std::array<uint8_t, 3> layerMap {
		static_cast<uint8_t>(RenderLayer::World),
		static_cast<uint8_t>(RenderLayer::Interface),
		static_cast<uint8_t>(RenderLayer::Interface),
	};
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 3, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 1),
	    { {}, true },
	    false,
	    RenderLayerDiagnosticMode::TintAndOutline,
	    { layerMap.data(), 3, 1, 3 },
	    {},
	    {},
	    {},
	    RenderWorldMaskDiagnosticMode::Off,
	});

	const SDL_Color worldBoundary = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(worldBoundary.r, 0);
	EXPECT_EQ(worldBoundary.g, 255);
	EXPECT_EQ(worldBoundary.b, 0);

	const SDL_Color interfaceBoundary = ReadColor(*outputSurface, 1, 0);
	EXPECT_EQ(interfaceBoundary.r, 0);
	EXPECT_EQ(interfaceBoundary.g, 96);
	EXPECT_EQ(interfaceBoundary.b, 255);

	const SDL_Color interfaceTint = ReadColor(*outputSurface, 2, 0);
	EXPECT_EQ(interfaceTint.r, 50);
	EXPECT_EQ(interfaceTint.g, 98);
	EXPECT_EQ(interfaceTint.b, 177);
}

TEST(FrameCompositor, RenderLayerDiagnosticRecomposesFullFrameWithoutDirtyRects)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 100, 100, 100, 255 };

	std::array<uint8_t, 1> layerMap { static_cast<uint8_t>(RenderLayer::World) };
	CompositionFrame frame {
		{ 1, 1 },
		MakeIndexBufferView(*indexSurface),
		MakePaletteSnapshot(palette, 1),
		{},
		false,
		RenderLayerDiagnosticMode::Tint,
		{ layerMap.data(), 1, 1, 1 },
		{},
		{},
		{},
		RenderWorldMaskDiagnosticMode::Off,
	};
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose(frame);

	SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, 50);
	EXPECT_EQ(color.g, 177);
	EXPECT_EQ(color.b, 50);

	layerMap[0] = static_cast<uint8_t>(RenderLayer::Interface);
	compositor.Compose(frame);

	color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, 50);
	EXPECT_EQ(color.g, 98);
	EXPECT_EQ(color.b, 177);
}

TEST(FrameCompositor, CpuPaletteCompositorComposesLargeDiagnosticFrameAcrossRowBands)
{
	constexpr int Width = 512;
	constexpr int Height = 256;
	const FrameCompositorThreadCountOverrideGuard threadCountOverride { 4 };

	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, Width, Height, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	for (int y = 0; y < Height; y++) {
		for (int x = 0; x < Width; x++) {
			pixels[y * indexSurface->pitch + x] = static_cast<uint8_t>((x + y) % 3 + 1);
		}
	}

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 40, 60, 80, 255 };
	palette[2] = { 80, 100, 120, 255 };
	palette[3] = { 120, 140, 160, 255 };

	std::vector<uint8_t> layerMap(Width * Height, static_cast<uint8_t>(RenderLayer::World));
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, Width, Height, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { Width, Height },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 1),
	    { {}, true },
	    false,
	    RenderLayerDiagnosticMode::TintAndOutline,
	    { layerMap.data(), Width, Height, Width },
	    {},
	    {},
	    {},
	    RenderWorldMaskDiagnosticMode::Off,
	});

	ExpectWorldTint(*outputSurface, 0, 0, palette[1]);
	ExpectWorldTint(*outputSurface, Width - 1, 63, palette[2]);
	ExpectWorldTint(*outputSurface, 17, 64, palette[1]);
	ExpectWorldTint(*outputSurface, Width - 2, 127, palette[2]);
	ExpectWorldTint(*outputSurface, 31, 128, palette[1]);
	ExpectWorldTint(*outputSurface, Width - 3, 191, palette[2]);
	ExpectWorldTint(*outputSurface, 43, 192, palette[2]);
	ExpectWorldTint(*outputSurface, Width - 3, Height - 1, palette[3]);

	const RenderPerfCompositionStats &stats = compositor.GetLastCompositionStats();
	EXPECT_EQ(stats.selectedThreadCount, 4);
}

} // namespace
} // namespace devilution
