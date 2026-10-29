#include <Render/GPUReadback.h>
#include <SDL3/SDL.h>
#include <cassert>
#include <cstdio>

int main(int argc, char** argv)
{
	const char* driver = argc > 1 ? argv[1] : "direct3d12";
	assert(SDL_Init(SDL_INIT_VIDEO));
	auto* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV, true, driver);
	if (!device) { std::fprintf(stderr, "Device unavailable: %s\n", SDL_GetError()); return 2; }
	GPUReadback readback;
	for (int i = 0; i < 16; ++i)
	{
		auto image = std::make_shared<RenderCapture::Image>();
		const auto format = i % 2 ? SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		assert(readback.Begin(device, image, format, 65 + i, 17)); // Non-aligned row lengths.
		auto* commands = SDL_AcquireGPUCommandBuffer(device); assert(commands);
		SDL_GPUColorTargetInfo target {};
		target.texture = readback.Target(); target.clear_color = {.25f,.5f,.75f,1.f};
		target.load_op = SDL_GPU_LOADOP_CLEAR; target.store_op = SDL_GPU_STOREOP_STORE;
		auto* pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr); assert(pass);
		SDL_EndGPURenderPass(pass);
		assert(SDL_SubmitGPUCommandBuffer(commands));
		readback.SubmitDownload(device);
		assert(!image->Ready); // Download alone is not completion.
		const auto deadline = SDL_GetTicks() + 5000;
		while (!image->Ready && image->Error.empty() && SDL_GetTicks() < deadline) { readback.Poll(device); SDL_Delay(1); }
		assert(image->Ready && !readback.Busy());
		for (size_t p = 0; p < image->Pixels.size(); p += 4)
		{
			assert(image->Pixels[p] >= 63 && image->Pixels[p] <= 64);
			assert(image->Pixels[p+1] >= 127 && image->Pixels[p+1] <= 128);
			assert(image->Pixels[p+2] >= 191 && image->Pixels[p+2] <= 192);
			assert(image->Pixels[p+3] == 255);
		}
	}
	auto invalid = std::make_shared<RenderCapture::Image>();
	for (int i = 0; i < 8; ++i)
	{
		auto pending = std::make_shared<RenderCapture::Image>();
		assert(readback.Begin(device, pending, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 64, 64));
		readback.SubmitDownload(device);
		pending->Cancelled = true; readback.Poll(device);
		assert(!readback.Busy() && !pending->Ready);
	}
	assert(!readback.Begin(device, invalid, SDL_GPU_TEXTUREFORMAT_R16_UINT, 16, 16));
	assert(invalid->Error == "unsupported_gpu_capture_format");
	auto cancelled = std::make_shared<RenderCapture::Image>();
	assert(readback.Begin(device, cancelled, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 16, 16));
	cancelled->Cancelled = true; readback.Poll(device);
	assert(!readback.Busy() && !cancelled->Ready && cancelled->Error == "capture_cancelled");
	auto destroyed = std::make_shared<RenderCapture::Image>();
	assert(readback.Begin(device, destroyed, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 16, 16));
	readback.Reset(device, "gpu_device_destroyed");
	assert(!destroyed->Ready && destroyed->Error == "gpu_device_destroyed");
	SDL_DestroyGPUDevice(device); SDL_Quit();
	std::printf("%s: readback RGBA/BGRA pixels, aligned pitch, 16 resource cycles, cancel and destruction PASS\n", driver);
}
