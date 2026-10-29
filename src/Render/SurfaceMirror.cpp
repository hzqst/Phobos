#include "SurfaceMirror.h"

#include <Surface.h>
#include <Unsorted.h>

#include <Helpers/CompileTime.h>
#include <Utilities/Debug.h>

#include <SDL3/SDL_gpu.h>

#include <Windows.h>

#include <cstring>

SDL_GPUTexture* SurfaceMirror::Texture = nullptr;
SDL_GPUTransferBuffer* SurfaceMirror::TransferBuffer = nullptr;
int SurfaceMirror::Width = 0;
int SurfaceMirror::Height = 0;
bool SurfaceMirror::PendingUpload = false;

namespace
{
	// WWMouseClass and its thread draw the cursor straight onto the primary surface while
	// holding this mutex.
	DEFINE_REFERENCE(HANDLE, MouseMutex, 0xB78168u)

	constexpr int BytesPerPixel = 2;
	constexpr DWORD MouseMutexTimeoutMS = 16;
	constexpr ULONGLONG SkipLogIntervalMS = 5000;

	ULONGLONG LastSkipLogTime = 0;
}

SurfaceMirror::CaptureResult SurfaceMirror::Capture(SDL_GPUDevice* pDevice)
{
	const int width = DSurface::WindowBounds.Width, height = DSurface::WindowBounds.Height;
	if (width <= 0 || height <= 0) return Skip("invalid primary dimensions");
	if (!EnsureResources(pDevice, width, height)) return CaptureResult::Failed;
	const char* error = nullptr;
	bool mapFailed = false;
	const bool copied = ReadPrimary([&](const unsigned char* source, int pitch, int w, int h)
	{
		if (w != width || h != height) return false;
		auto* destination = static_cast<BYTE*>(SDL_MapGPUTransferBuffer(pDevice, TransferBuffer, true));
		if (!destination) { mapFailed = true; return false; }
		for (int y = 0; y < h; ++y)
			std::memcpy(destination + size_t(y) * w * 2, source + ptrdiff_t(y) * pitch, size_t(w) * 2);
		SDL_UnmapGPUTransferBuffer(pDevice, TransferBuffer);
		return true;
	}, error);
	if (!copied) return mapFailed ? CaptureResult::Failed : Skip(error ? error : "primary copy failed");
	PendingUpload = true;
	return CaptureResult::Captured;
}

bool SurfaceMirror::ReadPrimary(const std::function<bool(const unsigned char*, int, int, int)>& copy, const char*& error)
{
	DSurface* const pPrimary = DSurface::Primary;

	if (!pPrimary)
		{ error = "primary_surface_unavailable"; return false; }

	if (pPrimary->GetBytesPerPixel() != BytesPerPixel)
		{ error = "primary_surface_not_16_bit"; return false; }

	const int width = DSurface::WindowBounds.Width;
	const int height = DSurface::WindowBounds.Height;

	// With native DirectDraw in windowed mode the primary surface is the whole desktop and
	// the game draws into it at the screen position of its client area.
	POINT origin { 0, 0 };

	if (pPrimary->GetWidth() != width || pPrimary->GetHeight() != height)
		if (!ClientToScreen(Game::hWnd, &origin)) { error = "client_origin_unavailable"; return false; }

	if (width <= 0 || height <= 0 || origin.x < 0 || origin.y < 0
		|| origin.x + width > pPrimary->GetWidth() || origin.y + height > pPrimary->GetHeight())
	{
		error = "game_area_outside_primary_surface"; return false;
	}

	const HANDLE hMouseMutex = MouseMutex;

	if (hMouseMutex)
	{
		const DWORD wait = WaitForSingleObject(hMouseMutex, MouseMutexTimeoutMS);

		if (wait == WAIT_ABANDONED) { ReleaseMutex(hMouseMutex); error = "mouse_mutex_abandoned"; return false; }
		if (wait != WAIT_OBJECT_0) { error = "mouse_mutex_busy"; return false; }
	}

	bool result = false;
	error = "primary_surface_lock_failed";

	if (const auto pSource = static_cast<const BYTE*>(pPrimary->Lock(origin.x, origin.y)))
	{
		const int pitch = pPrimary->GetPitch();

		error = "invalid_primary_pitch";
		if (int64_t(pitch) >= int64_t(pPrimary->GetWidth()) * 2 || -int64_t(pitch) >= int64_t(pPrimary->GetWidth()) * 2)
		{
			error = "primary_copy_failed";
			result = copy(pSource, pitch, width, height);
		}

		pPrimary->Unlock();
	}

	if (hMouseMutex)
		ReleaseMutex(hMouseMutex);

	return result;
}

bool SurfaceMirror::CapturePixels(RenderCapture::Image& image)
{
	image.Width = image.LogicalWidth = DSurface::WindowBounds.Width;
	image.Height = image.LogicalHeight = DSurface::WindowBounds.Height;
	if (!RenderCapture::ValidSize(image.Width, image.Height)) { image.Error = "capture_size_limit"; return false; }
	std::vector<uint16_t> pixels(size_t(image.Width) * image.Height);
	const char* error = nullptr;
	if (!ReadPrimary([&](const unsigned char* source, int pitch, int w, int h)
	{
		if (w != image.Width || h != image.Height) return false;
		for (int y = 0; y < h; ++y)
			std::memcpy(pixels.data() + size_t(y) * w, source + ptrdiff_t(y) * pitch, size_t(w) * 2);
		return true;
	}, error)) { image.Error = error; return false; }
	const auto& pack = Unsorted::ColorPackData;
	if (!RenderCapture::Decode16(pixels, {pack._R_SHL, pack._G_SHL, pack._B_SHL},
		{8 - pack._R_SHR, 8 - pack._G_SHR, 8 - pack._B_SHR}, image.Pixels))
	{ image.Error = "unsupported_color_packing"; return false; }
	image.Viewport = {0, 0, float(image.Width), float(image.Height)};
	image.Source = "ddraw"; image.Backend = "DirectDraw"; image.OutputMode = "original";
	image.Ready = true;
	return true;
}

bool SurfaceMirror::HasPendingUpload()
{
	return PendingUpload;
}

void SurfaceMirror::Upload(SDL_GPUCopyPass* pCopyPass)
{
	if (!PendingUpload)
		return;

	SDL_GPUTextureTransferInfo source {};
	source.transfer_buffer = TransferBuffer;
	source.pixels_per_row = static_cast<Uint32>(Width);
	source.rows_per_layer = static_cast<Uint32>(Height);

	SDL_GPUTextureRegion destination {};
	destination.texture = Texture;
	destination.w = static_cast<Uint32>(Width);
	destination.h = static_cast<Uint32>(Height);
	destination.d = 1;

	// Cycling hands out a fresh texture while the previous frame may still be sampling it
	SDL_UploadToGPUTexture(pCopyPass, &source, &destination, true);
	PendingUpload = false;
}

void SurfaceMirror::Release(SDL_GPUDevice* pDevice)
{
	if (Texture)
	{
		SDL_ReleaseGPUTexture(pDevice, Texture);
		Texture = nullptr;
	}

	if (TransferBuffer)
	{
		SDL_ReleaseGPUTransferBuffer(pDevice, TransferBuffer);
		TransferBuffer = nullptr;
	}

	Width = 0;
	Height = 0;
	PendingUpload = false;
}

SDL_GPUTexture* SurfaceMirror::GetTexture()
{
	return Texture;
}

int SurfaceMirror::GetWidth()
{
	return Width;
}

int SurfaceMirror::GetHeight()
{
	return Height;
}

bool SurfaceMirror::EnsureResources(SDL_GPUDevice* pDevice, int width, int height)
{
	if (Texture && width == Width && height == Height)
		return true;

	Release(pDevice);

	SDL_GPUTextureCreateInfo textureInfo {};
	textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
	textureInfo.format = SDL_GPU_TEXTUREFORMAT_R16_UINT;
	textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	textureInfo.width = static_cast<Uint32>(width);
	textureInfo.height = static_cast<Uint32>(height);
	textureInfo.layer_count_or_depth = 1;
	textureInfo.num_levels = 1;

	Texture = SDL_CreateGPUTexture(pDevice, &textureInfo);

	if (!Texture)
		return false;

	SDL_GPUTransferBufferCreateInfo bufferInfo {};
	bufferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	bufferInfo.size = static_cast<Uint32>(width) * static_cast<Uint32>(height) * BytesPerPixel;

	TransferBuffer = SDL_CreateGPUTransferBuffer(pDevice, &bufferInfo);

	if (!TransferBuffer)
		return false;

	Width = width;
	Height = height;

	Debug::Log("[GPURender] Mirroring the game screen at %dx%d.\n", width, height);
	return true;
}

// Skipping is expected now and then (lost surfaces, a busy mouse thread), so it is only
// logged once in a while.
SurfaceMirror::CaptureResult SurfaceMirror::Skip(const char* pReason)
{
	const ULONGLONG now = GetTickCount64();

	if (now - LastSkipLogTime >= SkipLogIntervalMS)
	{
		LastSkipLogTime = now;
		Debug::Log("[GPURender] Skipped a frame: %s.\n", pReason);
	}

	return CaptureResult::Skipped;
}
