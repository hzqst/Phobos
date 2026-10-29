#include "GPUReadback.h"
#include <algorithm>
#include <cstring>

bool GPUReadback::Begin(SDL_GPUDevice* device, std::shared_ptr<RenderCapture::Image> image,
	SDL_GPUTextureFormat format, int width, int height)
{
	if (Busy()) { image->Error = "gpu_capture_busy"; return false; }
	if (!RenderCapture::ValidSize(width, height)) { image->Error = "capture_size_limit"; return false; }
	if (format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM && format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM)
	{ image->Error = "unsupported_gpu_capture_format"; return false; }
	Image = std::move(image); Image->Width = width; Image->Height = height;
	BGRA = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
	Pitch = (unsigned(width) * 4 + 255) & ~255u;
	SDL_GPUTextureCreateInfo texture {};
	texture.type = SDL_GPU_TEXTURETYPE_2D; texture.format = format;
	texture.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	texture.width = width; texture.height = height; texture.num_levels = texture.layer_count_or_depth = 1;
	Texture = SDL_CreateGPUTexture(device, &texture);
	SDL_GPUTransferBufferCreateInfo transfer {};
	transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; transfer.size = Pitch * height;
	Transfer = SDL_CreateGPUTransferBuffer(device, &transfer);
	if (!Texture || !Transfer) { Reset(device, "gpu_capture_allocation_failed"); return false; }
	return true;
}

void GPUReadback::SubmitDownload(SDL_GPUDevice* device)
{
	if (!Image || Fence) return;
	auto* commands = SDL_AcquireGPUCommandBuffer(device);
	if (!commands) { Reset(device, "gpu_download_command_failed"); return; }
	auto* pass = SDL_BeginGPUCopyPass(commands);
	if (!pass) { SDL_CancelGPUCommandBuffer(commands); Reset(device, "gpu_download_pass_failed"); return; }
	SDL_GPUTextureRegion source {};
	source.texture = Texture; source.w = Image->Width; source.h = Image->Height; source.d = 1;
	SDL_GPUTextureTransferInfo destination {};
	destination.transfer_buffer = Transfer; destination.pixels_per_row = Pitch / 4; destination.rows_per_layer = Image->Height;
	SDL_DownloadFromGPUTexture(pass, &source, &destination);
	SDL_EndGPUCopyPass(pass);
	Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
	if (!Fence) Reset(device, "gpu_download_submit_failed");
}

void GPUReadback::Poll(SDL_GPUDevice* device)
{
	if (!Image) return;
	if (Image->Cancelled) { Reset(device, "capture_cancelled"); return; }
	if (!Fence || !SDL_QueryGPUFence(device, Fence)) return;
	const auto* pixels = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(device, Transfer, false));
	if (!pixels) { Reset(device, "gpu_download_map_failed"); return; }
	Image->Pixels.resize(size_t(Image->Width) * Image->Height * 4);
	for (int y = 0; y < Image->Height; ++y)
		std::memcpy(Image->Pixels.data() + size_t(y) * Image->Width * 4, pixels + size_t(y) * Pitch, size_t(Image->Width) * 4);
	SDL_UnmapGPUTransferBuffer(device, Transfer);
	if (BGRA) for (size_t i = 0; i < Image->Pixels.size(); i += 4) std::swap(Image->Pixels[i], Image->Pixels[i + 2]);
	Image->Ready = true;
	Reset(device, nullptr);
}

void GPUReadback::Reset(SDL_GPUDevice* device, const char* error)
{
	if (Image && error && !Image->Ready) Image->Error = error;
	// SDL defers destruction of resources referenced by submitted GPU commands.
	if (Fence) SDL_ReleaseGPUFence(device, Fence);
	if (Transfer) SDL_ReleaseGPUTransferBuffer(device, Transfer);
	if (Texture) SDL_ReleaseGPUTexture(device, Texture);
	Fence = nullptr; Transfer = nullptr; Texture = nullptr; Image.reset(); Pitch = 0;
}
