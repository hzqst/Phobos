#pragma once

struct SDL_GPUDevice;
struct SDL_Window;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUSampler;

// Draws the mirrored game screen into the window, scaled with its aspect ratio kept and the
// rest of the window black.
class PresentPass
{
public:
	static bool Create(SDL_GPUDevice* pDevice, SDL_Window* pWindow);
	static void Release(SDL_GPUDevice* pDevice);

	// pSource may be null, then the target is only cleared
	static void Draw(SDL_GPUCommandBuffer* pCommandBuffer, SDL_GPUTexture* pTarget, unsigned int targetWidth,
		unsigned int targetHeight, SDL_GPUTexture* pSource, int sourceWidth, int sourceHeight);

private:
	static SDL_GPUGraphicsPipeline* Pipeline;
	static SDL_GPUSampler* Sampler;
};
