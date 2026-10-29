#include <Render/GPUReadback.h>
#include <cassert>
#include <cstdio>

struct SDL_GPUDevice {};
struct SDL_GPUTexture {};
struct SDL_GPUTransferBuffer { std::vector<uint8_t> Bytes; };
struct SDL_GPUFence {};
struct SDL_GPUCommandBuffer {};
struct SDL_GPUCopyPass {};
static bool Signalled = false, FailTexture = false, FailMap = false, FailSubmit = false;
static unsigned Maps = 0, Textures = 0, Buffers = 0, Fences = 0;
static SDL_GPUCommandBuffer Commands;
static SDL_GPUCopyPass Pass;
SDL_GPUTexture* SDLCALL SDL_CreateGPUTexture(SDL_GPUDevice*, const SDL_GPUTextureCreateInfo*)
{ if (FailTexture) return nullptr; ++Textures; return new SDL_GPUTexture; }
void SDLCALL SDL_ReleaseGPUTexture(SDL_GPUDevice*, SDL_GPUTexture* texture) { --Textures; delete texture; }
SDL_GPUTransferBuffer* SDLCALL SDL_CreateGPUTransferBuffer(SDL_GPUDevice*, const SDL_GPUTransferBufferCreateInfo* info)
{ ++Buffers; return new SDL_GPUTransferBuffer{std::vector<uint8_t>(info->size, 127)}; }
void SDLCALL SDL_ReleaseGPUTransferBuffer(SDL_GPUDevice*, SDL_GPUTransferBuffer* buffer) { --Buffers; delete buffer; }
SDL_GPUCommandBuffer* SDLCALL SDL_AcquireGPUCommandBuffer(SDL_GPUDevice*) { return &Commands; }
bool SDLCALL SDL_CancelGPUCommandBuffer(SDL_GPUCommandBuffer*) { return true; }
SDL_GPUCopyPass* SDLCALL SDL_BeginGPUCopyPass(SDL_GPUCommandBuffer*) { return &Pass; }
void SDLCALL SDL_EndGPUCopyPass(SDL_GPUCopyPass*) {}
void SDLCALL SDL_DownloadFromGPUTexture(SDL_GPUCopyPass*, const SDL_GPUTextureRegion*, const SDL_GPUTextureTransferInfo* info)
{ assert(info->pixels_per_row * 4 % 256 == 0 && info->offset % 512 == 0); }
SDL_GPUFence* SDLCALL SDL_SubmitGPUCommandBufferAndAcquireFence(SDL_GPUCommandBuffer*)
{ if (FailSubmit) return nullptr; ++Fences; return new SDL_GPUFence; }
bool SDLCALL SDL_QueryGPUFence(SDL_GPUDevice*, SDL_GPUFence*) { return Signalled; }
void SDLCALL SDL_ReleaseGPUFence(SDL_GPUDevice*, SDL_GPUFence* fence) { --Fences; delete fence; }
void* SDLCALL SDL_MapGPUTransferBuffer(SDL_GPUDevice*, SDL_GPUTransferBuffer* buffer, bool cycle)
{ assert(Signalled && !cycle); ++Maps; return FailMap ? nullptr : buffer->Bytes.data(); }
void SDLCALL SDL_UnmapGPUTransferBuffer(SDL_GPUDevice*, SDL_GPUTransferBuffer*) {}

int main()
{
	SDL_GPUDevice device;
	GPUReadback readback;
	auto begin = [&] {
		auto image = std::make_shared<RenderCapture::Image>();
		readback.Begin(&device, image, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 65, 7);
		return image;
	};
	auto image = begin(); readback.SubmitDownload(&device);
	readback.Poll(&device);
	assert(!image->Ready && Maps == 0 && readback.Busy());
	auto busy = begin(); assert(busy->Error == "gpu_capture_busy");
	Signalled = true; readback.Poll(&device);
	assert(image->Ready && image->Pixels.size() == 65u * 7 * 4 && Maps == 1);
	assert(!Textures && !Buffers && !Fences);
	Signalled = false;
	auto cancelled = begin(); readback.SubmitDownload(&device); cancelled->Cancelled = true; readback.Poll(&device);
	assert(!cancelled->Ready && Maps == 1 && !Textures && !Buffers && !Fences);
	FailTexture = true; auto failed = begin(); FailTexture = false;
	assert(failed->Error == "gpu_capture_allocation_failed" && !Textures && !Buffers);
	FailSubmit = true; failed = begin(); readback.SubmitDownload(&device); FailSubmit = false;
	assert(failed->Error == "gpu_download_submit_failed" && !Textures && !Buffers && !Fences);
	FailMap = Signalled = true; failed = begin(); readback.SubmitDownload(&device); readback.Poll(&device);
	assert(!failed->Ready && failed->Error == "gpu_download_map_failed" && !Textures && !Buffers && !Fences);
	std::puts("GPUReadback lifetime: no map before fence, busy, cancellation, allocation/map/submit failure and resource release PASS");
}
