#pragma once

struct SDL_GPUDevice;
struct SDL_GPUCopyPass;
struct SDL_GPUTexture;
struct SDL_GPUTransferBuffer;

// Copies what the player sees - the game area of the primary surface, cursor included -
// into a 16 bit integer texture, keeping the game's own pixel format.
class SurfaceMirror
{
public:
	enum class CaptureResult
	{
		Captured,
		Skipped,  // nothing readable right now, try again on the next frame
		Failed,   // SDL error
	};

	static CaptureResult Capture(SDL_GPUDevice* pDevice);
	static bool HasPendingUpload();
	static void Upload(SDL_GPUCopyPass* pCopyPass);
	static void Release(SDL_GPUDevice* pDevice);

	static SDL_GPUTexture* GetTexture();
	static int GetWidth();
	static int GetHeight();

private:
	static bool EnsureResources(SDL_GPUDevice* pDevice, int width, int height);
	static CaptureResult Skip(const char* pReason);

	static SDL_GPUTexture* Texture;
	static SDL_GPUTransferBuffer* TransferBuffer;
	static int Width;
	static int Height;
	static bool PendingUpload;
};
