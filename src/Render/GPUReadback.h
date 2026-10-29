#pragma once
#include "CaptureImage.h"
#include <memory>
#include <SDL3/SDL_gpu.h>

// One bounded, on-demand offscreen capture. All methods run on the render thread.
class GPUReadback
{
public:
	GPUReadback() = default;
	GPUReadback(const GPUReadback&) = delete;
	GPUReadback& operator=(const GPUReadback&) = delete;
	bool Begin(SDL_GPUDevice* device, std::shared_ptr<RenderCapture::Image> image, SDL_GPUTextureFormat format, int width, int height);
	SDL_GPUTexture* Target() const { return Texture; }
	bool Busy() const { return bool(Image); }
	// Call after the normal renderer/Atlas submitted the offscreen draw.
	void SubmitDownload(SDL_GPUDevice* device);
	void Poll(SDL_GPUDevice* device);
	void Reset(SDL_GPUDevice* device, const char* error);
private:
	std::shared_ptr<RenderCapture::Image> Image;
	SDL_GPUTexture* Texture = nullptr;
	SDL_GPUTransferBuffer* Transfer = nullptr;
	SDL_GPUFence* Fence = nullptr;
	unsigned Pitch = 0;
	bool BGRA = false;
};
