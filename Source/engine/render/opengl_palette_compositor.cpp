/**
 * @file opengl_palette_compositor.cpp
 *
 * Optional OpenGL palette compositor spike.
 */
#include "engine/render/opengl_palette_compositor.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#else
#include <SDL.h>
#endif

#include "controls/control_mode.hpp"
#include "engine/render/accelerated_palette_compositor.hpp"
#include "engine/render/frame_compositor.hpp"
#include "headless_mode.hpp"
#include "options.h"
#include "utils/display.h"
#include "utils/log.hpp"

#if defined(DEVILUTIONX_OPENGL_PALETTE_COMPOSITOR) && defined(__APPLE__) && !defined(USE_SDL1)
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/gl3.h>
#define DEVILUTIONX_OPENGL_PALETTE_COMPOSITOR_ACTIVE 1
#else
#define DEVILUTIONX_OPENGL_PALETTE_COMPOSITOR_ACTIVE 0
#endif

namespace devilution {
namespace {

[[nodiscard]] bool OpenGlPaletteCompositorBuildAvailable()
{
	return DEVILUTIONX_OPENGL_PALETTE_COMPOSITOR_ACTIVE != 0;
}

[[nodiscard]] bool OpenGlPaletteCompositorAllowedByRuntime()
{
#ifdef USE_SDL1
	return false;
#else
	return !HeadlessMode
	    && *GetOptions().Experimental.renderFrameCompositor
	    && *GetOptions().Experimental.renderFrameCompositorBackend == RenderFrameCompositorBackend::OpenGlPalette
	    && ControlMode != ControlTypes::VirtualGamepad;
#endif
}

#if DEVILUTIONX_OPENGL_PALETTE_COMPOSITOR_ACTIVE

constexpr std::string_view OpenGlBackendName = "opengl-palette";

[[nodiscard]] bool SdlGlMakeCurrent(SDL_Window *window, SDL_GLContext context)
{
#ifdef USE_SDL3
	return SDL_GL_MakeCurrent(window, context);
#else
	return SDL_GL_MakeCurrent(window, context) == 0;
#endif
}

[[nodiscard]] bool SdlGlSetSwapInterval(const int interval)
{
#ifdef USE_SDL3
	return SDL_GL_SetSwapInterval(interval);
#else
	return SDL_GL_SetSwapInterval(interval) == 0;
#endif
}

void SdlGlDrawableSize(SDL_Window *window, int &width, int &height)
{
#ifdef USE_SDL3
	SDL_GetWindowSizeInPixels(window, &width, &height);
#else
	SDL_GL_GetDrawableSize(window, &width, &height);
#endif
}

[[nodiscard]] std::string GetShaderInfoLog(const GLuint shader)
{
	GLint length = 0;
	glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
	if (length <= 1)
		return {};
	std::string log(static_cast<size_t>(length), '\0');
	glGetShaderInfoLog(shader, length, nullptr, log.data());
	return log;
}

[[nodiscard]] std::string GetProgramInfoLog(const GLuint program)
{
	GLint length = 0;
	glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
	if (length <= 1)
		return {};
	std::string log(static_cast<size_t>(length), '\0');
	glGetProgramInfoLog(program, length, nullptr, log.data());
	return log;
}

[[nodiscard]] GLuint CompileShader(const GLenum type, const char *source)
{
	const GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);

	GLint status = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status == GL_TRUE)
		return shader;

	Log("OpenGL palette compositor shader compile failed: {}", GetShaderInfoLog(shader));
	glDeleteShader(shader);
	return 0;
}

[[nodiscard]] GLuint CreatePaletteProgram()
{
	constexpr char VertexShaderSource[] = R"(
#version 150
in vec2 aPosition;
in vec2 aTexCoord;
out vec2 vTexCoord;
void main()
{
	vTexCoord = aTexCoord;
	gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";
	constexpr char FragmentShaderSource[] = R"(
#version 150
uniform sampler2D uIndex;
uniform sampler2D uPalette;
uniform sampler2D uOutput;
uniform int uMode;
in vec2 vTexCoord;
out vec4 outColor;
void main()
{
	if (uMode == 0) {
		float indexValue = texture(uIndex, vTexCoord).r;
		int paletteIndex = int(indexValue * 255.0 + 0.5);
		outColor = texelFetch(uPalette, ivec2(paletteIndex, 0), 0);
	} else {
		outColor = texture(uOutput, vTexCoord);
	}
}
)";

	const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, VertexShaderSource);
	if (vertexShader == 0)
		return 0;

	const GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, FragmentShaderSource);
	if (fragmentShader == 0) {
		glDeleteShader(vertexShader);
		return 0;
	}

	const GLuint program = glCreateProgram();
	glAttachShader(program, vertexShader);
	glAttachShader(program, fragmentShader);
	glBindAttribLocation(program, 0, "aPosition");
	glBindAttribLocation(program, 1, "aTexCoord");
	glLinkProgram(program);
	glDeleteShader(vertexShader);
	glDeleteShader(fragmentShader);

	GLint status = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (status == GL_TRUE) {
		glUseProgram(program);
		glUniform1i(glGetUniformLocation(program, "uIndex"), 0);
		glUniform1i(glGetUniformLocation(program, "uPalette"), 1);
		glUniform1i(glGetUniformLocation(program, "uOutput"), 2);
		glUseProgram(0);
		return program;
	}

	Log("OpenGL palette compositor program link failed: {}", GetProgramInfoLog(program));
	glDeleteProgram(program);
	return 0;
}

[[nodiscard]] int SurfaceBytesPerPixel(const SDL_Surface &surface)
{
#ifdef USE_SDL3
	return SDL_BYTESPERPIXEL(surface.format);
#else
	return surface.format->BytesPerPixel;
#endif
}

void ReadSurfaceRgba(const SDL_Surface &surface, const uint32_t pixel, uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &a)
{
#ifdef USE_SDL3
	SDL_GetRGBA(pixel, SDL_GetPixelFormatDetails(surface.format), SDL_GetSurfacePalette(const_cast<SDL_Surface *>(&surface)), &r, &g, &b, &a);
#else
	SDL_GetRGBA(pixel, surface.format, &r, &g, &b, &a);
#endif
}

class OpenGlPaletteCompositorState {
public:
	bool Initialize(SDL_Window *window)
	{
		Destroy();
		if (window == nullptr)
			return false;

		window_ = window;
		context_ = SDL_GL_CreateContext(window_);
		if (context_ == nullptr) {
			Log("OpenGL palette compositor unavailable: {}", SDL_GetError());
			return false;
		}
		if (!MakeCurrent()) {
			Log("OpenGL palette compositor unavailable: {}", SDL_GetError());
			Destroy();
			return false;
		}
		UpdateSwapInterval();
		if (!CreateResources()) {
			Log("OpenGL palette compositor unavailable: resource setup failed");
			Destroy();
			return false;
		}

		Log("OpenGL palette compositor initialized");
		available_ = true;
		return true;
	}

	void Destroy()
	{
		if (context_ != nullptr && window_ != nullptr)
			(void)SdlGlMakeCurrent(window_, context_);
		DeleteResources();
		if (context_ != nullptr)
			SDL_GL_DeleteContext(context_);
		context_ = nullptr;
		window_ = nullptr;
		available_ = false;
		hasFrameTexture_ = false;
		indexTextureWidth_ = 0;
		indexTextureHeight_ = 0;
		outputTextureWidth_ = 0;
		outputTextureHeight_ = 0;
		lastSwapInterval_ = -2;
	}

	[[nodiscard]] bool IsAvailable() const
	{
		return available_;
	}

	[[nodiscard]] bool PrepareIndexedFrame(const CompositionFrame &frame)
	{
		if (!available_ || frame.indexBuffer.pixels == nullptr || frame.logicalSize.width <= 0 || frame.logicalSize.height <= 0)
			return false;
		if (!MakeCurrent())
			return false;

		UpdateSwapInterval();
		SetTextureFilter(indexTexture_);
		ResizeTexture(indexTexture_, indexTextureWidth_, indexTextureHeight_, frame.logicalSize.width, frame.logicalSize.height, GL_R8, GL_RED);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, indexTexture_);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.indexBuffer.pitch);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.logicalSize.width, frame.logicalSize.height, GL_RED, GL_UNSIGNED_BYTE, frame.indexBuffer.pixels);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

		if (!UploadPalette(frame.palette))
			return false;
		pendingLogicalSize_ = frame.logicalSize;
		pendingMode_ = PendingMode::IndexedPalette;
		hasFrameTexture_ = true;
		return true;
	}

	[[nodiscard]] bool PrepareOutputSurfaceFrame(const CompositionFrame &frame, SDL_Surface &outputSurface)
	{
		if (!available_ || outputSurface.pixels == nullptr || outputSurface.w <= 0 || outputSurface.h <= 0)
			return false;
		if (!MakeCurrent())
			return false;

		UpdateSwapInterval();
		if (!UploadOutputSurface(outputSurface))
			return false;
		pendingLogicalSize_ = frame.logicalSize.width > 0 && frame.logicalSize.height > 0 ? frame.logicalSize : Size { outputSurface.w, outputSurface.h };
		pendingMode_ = PendingMode::OutputSurface;
		hasFrameTexture_ = true;
		return true;
	}

	void Present()
	{
		if (!available_ || !hasFrameTexture_)
			return;
		if (!MakeCurrent())
			return;

		UpdateSwapInterval();
		const Size viewportSize = pendingLogicalSize_;
		int drawableWidth = 0;
		int drawableHeight = 0;
		SdlGlDrawableSize(window_, drawableWidth, drawableHeight);
		const Rectangle viewport = CalculateViewport({ drawableWidth, drawableHeight }, viewportSize);

		glDisable(GL_BLEND);
		glClearColor(0.F, 0.F, 0.F, 1.F);
		glClear(GL_COLOR_BUFFER_BIT);
		glViewport(viewport.position.x, viewport.position.y, viewport.size.width, viewport.size.height);
		glUseProgram(program_);
		glUniform1i(modeUniform_, pendingMode_ == PendingMode::IndexedPalette ? 0 : 1);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, indexTexture_);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, paletteTexture_);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, outputTexture_);
		glBindVertexArray(vao_);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindVertexArray(0);
		glUseProgram(0);
		SDL_GL_SwapWindow(window_);
	}

private:
	enum class PendingMode : uint8_t {
		IndexedPalette,
		OutputSurface,
	};

	[[nodiscard]] bool MakeCurrent()
	{
		return window_ != nullptr && context_ != nullptr && SdlGlMakeCurrent(window_, context_);
	}

	[[nodiscard]] bool CreateResources()
	{
		program_ = CreatePaletteProgram();
		if (program_ == 0)
			return false;
		modeUniform_ = glGetUniformLocation(program_, "uMode");

		constexpr std::array<float, 16> vertices {
			-1.F,
			-1.F,
			0.F,
			1.F,
			1.F,
			-1.F,
			1.F,
			1.F,
			-1.F,
			1.F,
			0.F,
			0.F,
			1.F,
			1.F,
			1.F,
			0.F,
		};

		glGenVertexArrays(1, &vao_);
		glBindVertexArray(vao_);
		glGenBuffers(1, &vbo_);
		glBindBuffer(GL_ARRAY_BUFFER, vbo_);
		glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)), vertices.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void *>(0));
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void *>(2 * sizeof(float)));
		glBindVertexArray(0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		CreateTexture(paletteTexture_);
		ResizeTexture(paletteTexture_, paletteTextureWidth_, paletteTextureHeight_, 256, 1, GL_RGBA8, GL_RGBA);
		CreateTexture(indexTexture_);
		ResizeTexture(indexTexture_, indexTextureWidth_, indexTextureHeight_, 1, 1, GL_R8, GL_RED);
		constexpr uint8_t zeroIndex = 0;
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, indexTexture_);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RED, GL_UNSIGNED_BYTE, &zeroIndex);
		CreateTexture(outputTexture_);
		ResizeTexture(outputTexture_, outputTextureWidth_, outputTextureHeight_, 1, 1, GL_RGBA8, GL_RGBA);
		constexpr std::array<uint8_t, 4> opaqueBlack { 0, 0, 0, 255 };
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, outputTexture_);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, opaqueBlack.data());
		return glGetError() == GL_NO_ERROR;
	}

	void DeleteResources()
	{
		DeleteTexture(indexTexture_);
		DeleteTexture(paletteTexture_);
		DeleteTexture(outputTexture_);
		if (vbo_ != 0)
			glDeleteBuffers(1, &vbo_);
		if (vao_ != 0)
			glDeleteVertexArrays(1, &vao_);
		if (program_ != 0)
			glDeleteProgram(program_);
		vbo_ = 0;
		vao_ = 0;
		program_ = 0;
		modeUniform_ = -1;
	}

	void CreateTexture(GLuint &texture)
	{
		if (texture == 0)
			glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		SetTextureFilter(texture);
	}

	void DeleteTexture(GLuint &texture)
	{
		if (texture != 0)
			glDeleteTextures(1, &texture);
		texture = 0;
	}

	void SetTextureFilter(const GLuint texture)
	{
		if (texture == 0)
			return;
		const GLint filter = *GetOptions().Graphics.scaleQuality == ScalingQuality::NearestPixel ? GL_NEAREST : GL_LINEAR;
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	}

	void ResizeTexture(GLuint &texture, int &currentWidth, int &currentHeight, const int width, const int height, const GLint internalFormat, const GLenum format)
	{
		CreateTexture(texture);
		if (currentWidth == width && currentHeight == height)
			return;

		glBindTexture(GL_TEXTURE_2D, texture);
		glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, nullptr);
		currentWidth = width;
		currentHeight = height;
	}

	[[nodiscard]] bool UploadPalette(const PaletteSnapshot &palette)
	{
		std::array<uint8_t, 256 * 4> paletteRgba {};
		for (size_t i = 0; i < palette.colors.size(); i++) {
			paletteRgba[i * 4 + 0] = palette.colors[i].r;
			paletteRgba[i * 4 + 1] = palette.colors[i].g;
			paletteRgba[i * 4 + 2] = palette.colors[i].b;
			paletteRgba[i * 4 + 3] = palette.colors[i].a;
		}

		SetTextureFilter(paletteTexture_);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, paletteTexture_);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, paletteRgba.data());
		return glGetError() == GL_NO_ERROR;
	}

	[[nodiscard]] bool UploadOutputSurface(SDL_Surface &surface)
	{
		const bool mustLock = SDL_MUSTLOCK(&surface);
		if (mustLock) {
#ifdef USE_SDL3
			if (!SDL_LockSurface(&surface))
				return false;
#else
			if (SDL_LockSurface(&surface) < 0)
				return false;
#endif
		}

		const int bytesPerPixel = SurfaceBytesPerPixel(surface);
		outputRgba_.resize(static_cast<size_t>(surface.w) * static_cast<size_t>(surface.h) * 4);
		for (int y = 0; y < surface.h; y++) {
			const uint8_t *src = static_cast<const uint8_t *>(surface.pixels) + static_cast<ptrdiff_t>(y) * surface.pitch;
			uint8_t *dst = outputRgba_.data() + static_cast<ptrdiff_t>(y) * surface.w * 4;
			for (int x = 0; x < surface.w; x++) {
				uint32_t pixel = 0;
				std::memcpy(&pixel, src + x * bytesPerPixel, bytesPerPixel);
				ReadSurfaceRgba(surface, pixel, dst[x * 4 + 0], dst[x * 4 + 1], dst[x * 4 + 2], dst[x * 4 + 3]);
			}
		}

		if (mustLock)
			SDL_UnlockSurface(&surface);

		SetTextureFilter(outputTexture_);
		ResizeTexture(outputTexture_, outputTextureWidth_, outputTextureHeight_, surface.w, surface.h, GL_RGBA8, GL_RGBA);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, outputTexture_);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, surface.w, surface.h, GL_RGBA, GL_UNSIGNED_BYTE, outputRgba_.data());
		return glGetError() == GL_NO_ERROR;
	}

	[[nodiscard]] Rectangle CalculateViewport(const Size drawableSize, const Size logicalSize) const
	{
		if (drawableSize.width <= 0 || drawableSize.height <= 0 || logicalSize.width <= 0 || logicalSize.height <= 0)
			return { { 0, 0 }, drawableSize };

		if (*GetOptions().Graphics.integerScaling) {
			const int scale = std::max(1, std::min(drawableSize.width / logicalSize.width, drawableSize.height / logicalSize.height));
			const Size scaledSize { logicalSize.width * scale, logicalSize.height * scale };
			return {
				{ (drawableSize.width - scaledSize.width) / 2, (drawableSize.height - scaledSize.height) / 2 },
				scaledSize,
			};
		}

		const double drawableAspect = static_cast<double>(drawableSize.width) / drawableSize.height;
		const double logicalAspect = static_cast<double>(logicalSize.width) / logicalSize.height;
		Size scaledSize {};
		if (drawableAspect > logicalAspect) {
			scaledSize.height = drawableSize.height;
			scaledSize.width = static_cast<int>(drawableSize.height * logicalAspect);
		} else {
			scaledSize.width = drawableSize.width;
			scaledSize.height = static_cast<int>(drawableSize.width / logicalAspect);
		}
		return {
			{ (drawableSize.width - scaledSize.width) / 2, (drawableSize.height - scaledSize.height) / 2 },
			scaledSize,
		};
	}

	void UpdateSwapInterval()
	{
		const int swapInterval = *GetOptions().Graphics.frameRateControl == FrameRateControl::VerticalSync ? 1 : 0;
		if (lastSwapInterval_ == swapInterval)
			return;
		if (!SdlGlSetSwapInterval(swapInterval)) {
			Log("OpenGL palette compositor could not set swap interval {}: {}", swapInterval, SDL_GetError());
		}
		lastSwapInterval_ = swapInterval;
	}

	SDL_Window *window_ = nullptr;
	SDL_GLContext context_ = nullptr;
	bool available_ = false;
	GLuint program_ = 0;
	GLint modeUniform_ = -1;
	GLuint vao_ = 0;
	GLuint vbo_ = 0;
	GLuint indexTexture_ = 0;
	GLuint paletteTexture_ = 0;
	GLuint outputTexture_ = 0;
	int indexTextureWidth_ = 0;
	int indexTextureHeight_ = 0;
	int paletteTextureWidth_ = 0;
	int paletteTextureHeight_ = 0;
	int outputTextureWidth_ = 0;
	int outputTextureHeight_ = 0;
	int lastSwapInterval_ = -2;
	bool hasFrameTexture_ = false;
	PendingMode pendingMode_ = PendingMode::IndexedPalette;
	Size pendingLogicalSize_ {};
	std::vector<uint8_t> outputRgba_;
};

OpenGlPaletteCompositorState &OpenGlState()
{
	static OpenGlPaletteCompositorState state;
	return state;
}

class OpenGlPalettePresenter final : public IAcceleratedPalettePresenter {
public:
	std::string_view Name() const override
	{
		return OpenGlBackendName;
	}

	bool IsAvailable() const override
	{
		return OpenGlState().IsAvailable();
	}

	bool PrepareIndexedFrame(const AcceleratedPaletteFrame &frame) override
	{
		return OpenGlState().PrepareIndexedFrame(frame.composition);
	}

	bool PrepareOutputSurfaceFrame(const AcceleratedPaletteFrame &frame, SDL_Surface &outputSurface) override
	{
		return OpenGlState().PrepareOutputSurfaceFrame(frame.composition, outputSurface);
	}

	void Present() override
	{
		OpenGlState().Present();
	}
};

#endif

#if !DEVILUTIONX_OPENGL_PALETTE_COMPOSITOR_ACTIVE
bool LoggedUnavailableBuild = false;
#endif

} // namespace

bool OpenGlPaletteCompositorRequested()
{
	return OpenGlPaletteCompositorAllowedByRuntime();
}

bool OpenGlPaletteCompositorWindowRequested()
{
	return OpenGlPaletteCompositorRequested() && OpenGlPaletteCompositorBuildAvailable();
}

AcceleratedCompositorWindowFlags ConfigureOpenGlPaletteCompositorWindow()
{
#if DEVILUTIONX_OPENGL_PALETTE_COMPOSITOR_ACTIVE
	if (!OpenGlPaletteCompositorWindowRequested())
		return 0;
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	return SDL_WINDOW_OPENGL;
#else
	return 0;
#endif
}

bool ReinitializeOpenGlPaletteCompositor(SDL_Window *window)
{
#if DEVILUTIONX_OPENGL_PALETTE_COMPOSITOR_ACTIVE
	if (!OpenGlPaletteCompositorRequested())
		return false;
	return OpenGlState().Initialize(window);
#else
	(void)window;
	return false;
#endif
}

bool OpenGlPaletteCompositorIsActive()
{
#if DEVILUTIONX_OPENGL_PALETTE_COMPOSITOR_ACTIVE
	return OpenGlState().IsAvailable();
#else
	return false;
#endif
}

void ShutdownOpenGlPaletteCompositor()
{
#if DEVILUTIONX_OPENGL_PALETTE_COMPOSITOR_ACTIVE
	OpenGlState().Destroy();
#endif
}

std::unique_ptr<IFrameCompositorBackend> CreateOpenGlPaletteCompositorBackend()
{
#if DEVILUTIONX_OPENGL_PALETTE_COMPOSITOR_ACTIVE
	if (!OpenGlState().IsAvailable())
		return nullptr;
	return CreateAcceleratedPaletteCompositorBackend(std::make_unique<OpenGlPalettePresenter>());
#else
	if (OpenGlPaletteCompositorAllowedByRuntime() && !LoggedUnavailableBuild) {
		Log("OpenGL palette compositor was selected but is not built; falling back to CPU palette compositor");
		LoggedUnavailableBuild = true;
	}
	return nullptr;
#endif
}

} // namespace devilution
