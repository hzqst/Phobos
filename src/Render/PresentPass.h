#pragma once

#include <span>
#include <array>
#include <cstdint>

struct SDL_GPUDevice;
struct SDL_Window;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUSampler;

// Clears to black and draws either the diagnostic mirror or an independent cursor.
// Both paths use the same logical-canvas scaling and integer color decoder.
class PresentPass
{
public:
	struct Sprite
	{
		SDL_GPUTexture* Texture;
		int X, Y, Width, Height, SourceX, SourceY;
		bool IndexedAlpha = true;
		SDL_GPUTexture* Palette = nullptr;
		std::array<std::uint32_t, 3> Shift {}, Bits {};
	};
	static bool DrawSprites(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target,
		unsigned int targetWidth, unsigned int targetHeight, int logicalWidth, int logicalHeight,
		std::span<const Sprite> sprites);
	static bool Create(SDL_GPUDevice* pDevice, SDL_Window* pWindow);
	static void Release(SDL_GPUDevice* pDevice);

	// pSource may be null, then the target is only cleared
	static bool Draw(SDL_GPUCommandBuffer* pCommandBuffer, SDL_GPUTexture* pTarget, unsigned int targetWidth,
		unsigned int targetHeight, SDL_GPUTexture* pSource, int sourceWidth, int sourceHeight);
	static bool DrawCursor(SDL_GPUCommandBuffer* pCommandBuffer, SDL_GPUTexture* pTarget, unsigned int targetWidth,
		unsigned int targetHeight, SDL_GPUTexture* pSource, int sourceWidth, int sourceHeight,
		int x, int y, int logicalWidth, int logicalHeight);

private:
	static bool DrawRegion(SDL_GPUCommandBuffer* pCommandBuffer, SDL_GPUTexture* pTarget, unsigned int targetWidth,
		unsigned int targetHeight, SDL_GPUTexture* pSource, int sourceWidth, int sourceHeight,
		int x, int y, int logicalWidth, int logicalHeight, bool cursor);
	static SDL_GPUGraphicsPipeline* Pipeline;
	static SDL_GPUGraphicsPipeline* IndexedPipeline;
	static SDL_GPUSampler* Sampler;
};
