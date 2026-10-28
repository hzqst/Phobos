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
	DSurface* const pPrimary = DSurface::Primary;

	if (!pPrimary)
		return Skip("there is no primary surface");

	if (pPrimary->GetBytesPerPixel() != BytesPerPixel)
		return Skip("the primary surface is not 16 bit");

	const int width = DSurface::WindowBounds.Width;
	const int height = DSurface::WindowBounds.Height;

	// With native DirectDraw in windowed mode the primary surface is the whole desktop and
	// the game draws into it at the screen position of its client area.
	POINT origin { 0, 0 };

	if (pPrimary->GetWidth() != width || pPrimary->GetHeight() != height)
		ClientToScreen(Game::hWnd, &origin);

	if (width <= 0 || height <= 0 || origin.x < 0 || origin.y < 0
		|| origin.x + width > pPrimary->GetWidth() || origin.y + height > pPrimary->GetHeight())
	{
		return Skip("the game area is not inside the primary surface");
	}

	if (!EnsureResources(pDevice, width, height))
		return CaptureResult::Failed;

	const HANDLE hMouseMutex = MouseMutex;

	if (hMouseMutex)
	{
		const DWORD wait = WaitForSingleObject(hMouseMutex, MouseMutexTimeoutMS);

		if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
			return Skip("the mouse mutex is busy");
	}

	CaptureResult result = CaptureResult::Skipped;

	if (const auto pSource = static_cast<const BYTE*>(pPrimary->Lock(origin.x, origin.y)))
	{
		const int pitch = pPrimary->GetPitch();

		if (const auto pDestination = static_cast<BYTE*>(SDL_MapGPUTransferBuffer(pDevice, TransferBuffer, true)))
		{
			const size_t rowSize = static_cast<size_t>(width) * BytesPerPixel;

			for (int y = 0; y < height; ++y)
				std::memcpy(pDestination + y * rowSize, pSource + y * pitch, rowSize);

			SDL_UnmapGPUTransferBuffer(pDevice, TransferBuffer);
			PendingUpload = true;
			result = CaptureResult::Captured;
		}
		else
		{
			result = CaptureResult::Failed;
		}

		pPrimary->Unlock();
	}

	if (hMouseMutex)
		ReleaseMutex(hMouseMutex);

	return result == CaptureResult::Skipped ? Skip("the primary surface could not be locked") : result;
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
