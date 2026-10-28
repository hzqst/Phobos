#include <Render/FrameAtlas.h>
#include <SDL3/SDL_gpu.h>
#include <array>
#include <cassert>
#include <cstdio>

// A controlled completion timeline, independent of GPU speed. Link without SDL3.lib.
struct SDL_GPUDevice {};
struct SDL_GPUTexture {};
struct SDL_GPUTransferBuffer { std::vector<unsigned char> Bytes; };
struct SDL_GPUFence { bool Done = false; };
struct SDL_GPUCommandBuffer {};
struct SDL_GPUCopyPass {};
static std::vector<SDL_GPUFence*> Fences;
static unsigned ReleasedTextures = 0, Maps = 0;
static bool FailTexture = false, FailTransfer = false, FailMap = false, FailSubmit = false;
const char* SDLCALL SDL_GetGPUDeviceDriver(SDL_GPUDevice*) { return "test"; }
bool SDLCALL SDL_GPUTextureSupportsFormat(SDL_GPUDevice*, SDL_GPUTextureFormat, SDL_GPUTextureType, SDL_GPUTextureUsageFlags) { return true; }
SDL_GPUTexture* SDLCALL SDL_CreateGPUTexture(SDL_GPUDevice*, const SDL_GPUTextureCreateInfo*) { return FailTexture ? nullptr : new SDL_GPUTexture; }
void SDLCALL SDL_ReleaseGPUTexture(SDL_GPUDevice*, SDL_GPUTexture* texture) { ++ReleasedTextures; delete texture; }
SDL_GPUTransferBuffer* SDLCALL SDL_CreateGPUTransferBuffer(SDL_GPUDevice*, const SDL_GPUTransferBufferCreateInfo* info)
{ return FailTransfer ? nullptr : new SDL_GPUTransferBuffer {std::vector<unsigned char>(info->size)}; }
void SDLCALL SDL_ReleaseGPUTransferBuffer(SDL_GPUDevice*, SDL_GPUTransferBuffer* buffer) { delete buffer; }
void* SDLCALL SDL_MapGPUTransferBuffer(SDL_GPUDevice*, SDL_GPUTransferBuffer* buffer, bool cycle)
{ assert(!cycle); ++Maps; return FailMap ? nullptr : buffer->Bytes.data(); }
void SDLCALL SDL_UnmapGPUTransferBuffer(SDL_GPUDevice*, SDL_GPUTransferBuffer*) {}
void SDLCALL SDL_UploadToGPUTexture(SDL_GPUCopyPass*, const SDL_GPUTextureTransferInfo* source, const SDL_GPUTextureRegion*, bool cycle)
{ assert(!cycle && source->offset % 512 == 0); }
SDL_GPUFence* SDLCALL SDL_SubmitGPUCommandBufferAndAcquireFence(SDL_GPUCommandBuffer*)
{ if (FailSubmit) return nullptr; auto* fence = new SDL_GPUFence; Fences.push_back(fence); return fence; }
bool SDLCALL SDL_QueryGPUFence(SDL_GPUDevice*, SDL_GPUFence* fence) { return fence->Done; }
void SDLCALL SDL_ReleaseGPUFence(SDL_GPUDevice*, SDL_GPUFence* fence) { delete fence; }

int main()
{
	SDL_GPUDevice device; SDL_GPUCommandBuffer commands; SDL_GPUCopyPass copy;
	FrameCache::Cache cache; PaletteCache::Cache colors;
	PaletteCache::Data palette; palette.Shift = {11,5,0}; palette.Bits = {5,6,5};
	auto color = colors.Acquire(palette);
	auto make = [&](std::uintptr_t address) {
		auto resource = cache.Register(address, {4,4,{{0,0,4,4,0,0,16}}});
		return FrameAtlas::Request {cache.GetOrDecode(resource,0,[](const auto&, auto& pixels) {
			pixels.assign(16,7); return FrameCache::Status::Ready;
		}).Frame->ID,color};
	};
	const std::array a {make(1)}, b {make(2)};
	FrameAtlas::Options options; options.PageSize = 4; options.MaxPages = 1;
	FrameAtlas atlas; assert(atlas.Create(&device,options));
	for (int i = 0; i < 3; ++i)
	{
		assert(FrameAtlas::BeginResult::Ready == atlas.Begin(cache,a));
		if (atlas.HasUploads()) atlas.EncodeUploads(&copy);
		assert(atlas.Submit(&commands));
	}
	const auto maps = Maps;
	assert(FrameAtlas::BeginResult::Busy == atlas.Begin(cache,b));
	assert(maps == Maps); // Never map a transfer slot while its submission is pending.
	Fences[1]->Done = true; atlas.Poll();
	assert(3u == atlas.GetStats().InFlight); // Out-of-order completion cannot advance the prefix.
	Fences[0]->Done = true; atlas.Poll();
	assert(1u == atlas.GetStats().InFlight);
	assert(FrameAtlas::BeginResult::Ready == atlas.Begin(cache,b));
	assert(!atlas.Resolve(b[0]).Texture && 1u == atlas.GetStats().RetiringPages);
	assert(0u == ReleasedTextures); // A's third submission still uses the retiring page.
	atlas.CancelBatch(); Fences[2]->Done = true; atlas.Poll();
	assert(1u == ReleasedTextures && 0u == atlas.GetStats().Pages);
	assert(FrameAtlas::BeginResult::Ready == atlas.Begin(cache,b));
	assert(atlas.Resolve(b[0]).Texture); atlas.EncodeUploads(&copy);
	FailSubmit = true; assert(!atlas.Submit(&commands));
	assert(1u == atlas.GetStats().FrameUploads); // Failed submission must not publish B.
	assert(FrameAtlas::BeginResult::Failed == atlas.Begin(cache,b));
	atlas.Release(); FailSubmit = false;
	for (auto* failure : {&FailTexture,&FailTransfer,&FailMap})
	{
		assert(atlas.Create(&device,options)); *failure = true;
		assert(FrameAtlas::BeginResult::Failed == atlas.Begin(cache,a));
		assert(0u == atlas.GetStats().FrameUploads && !atlas.Resolve(a[0]).Texture);
		*failure = false; atlas.Release();
	}
	assert(atlas.Create(&device,options)); FailMap = true;
	assert(FrameAtlas::BeginResult::Failed == atlas.Begin(cache,a)); FailMap = false;
	assert(FrameAtlas::BeginResult::Ready == atlas.Begin(cache,a));
	assert(atlas.HasUploads() && atlas.Resolve(a[0]).Texture); atlas.CancelBatch(); atlas.Release();
	std::puts("FrameAtlas lifetime: busy slots, ordered fences, deferred retirement, failed creation/map/submit and retry passed.");
}
