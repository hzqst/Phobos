#include <Render/FrameAtlas.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <array>
#include <vector>
#include <Shaders/Fullscreen.vs.dxil.h>
#include <Shaders/IndexedSprite.ps.dxil.h>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s (%s)\n", __LINE__, #c, SDL_GetError()); return false; } } while (false)

static bool Submit(FrameAtlas& atlas, SDL_GPUDevice* device)
{
	auto* commands = SDL_AcquireGPUCommandBuffer(device);
	CHECK(commands);
	if (atlas.HasUploads())
	{
		auto* pass = SDL_BeginGPUCopyPass(commands); CHECK(pass);
		atlas.EncodeUploads(pass); SDL_EndGPUCopyPass(pass);
	}
	CHECK(atlas.Submit(commands)); // Also exercises upload-only/null-swapchain submission.
	return true;
}

static bool Read(SDL_GPUDevice* device, FrameAtlas::View view, int width, int height, unsigned bpp, unsigned expected)
{
	const unsigned pitch = 256;
	SDL_GPUTransferBufferCreateInfo info {}; info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; info.size = pitch * height;
	auto* buffer = SDL_CreateGPUTransferBuffer(device, &info); CHECK(buffer);
	auto* commands = SDL_AcquireGPUCommandBuffer(device); CHECK(commands);
	auto* pass = SDL_BeginGPUCopyPass(commands); CHECK(pass);
	SDL_GPUTextureRegion region {}; region.texture = view.Texture; region.x = view.X; region.y = view.Y; region.w = width; region.h = height; region.d = 1;
	SDL_GPUTextureTransferInfo destination {}; destination.transfer_buffer = buffer; destination.pixels_per_row = pitch / bpp; destination.rows_per_layer = height;
	SDL_DownloadFromGPUTexture(pass, &region, &destination); SDL_EndGPUCopyPass(pass);
	auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands); CHECK(fence);
	CHECK(SDL_WaitForGPUFences(device, true, &fence, 1));
	const auto* pixels = static_cast<const unsigned char*>(SDL_MapGPUTransferBuffer(device, buffer, false)); CHECK(pixels);
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
			CHECK(expected == (bpp == 1 ? pixels[y * pitch + x] : reinterpret_cast<const unsigned*>(pixels + y * pitch)[x]));
	SDL_UnmapGPUTransferBuffer(device, buffer); SDL_ReleaseGPUFence(device, fence); SDL_ReleaseGPUTransferBuffer(device, buffer);
	return true;
}

static bool Draw(SDL_GPUDevice* device, FrameAtlas::View view, unsigned expected)
{
	SDL_GPUShaderCreateInfo shader {}; shader.entrypoint = "main"; shader.format = SDL_GPU_SHADERFORMAT_DXIL;
	shader.stage = SDL_GPU_SHADERSTAGE_VERTEX; shader.code = g_Fullscreen_vs_DXIL; shader.code_size = sizeof(g_Fullscreen_vs_DXIL);
	auto* vertex = SDL_CreateGPUShader(device, &shader); CHECK(vertex);
	shader.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; shader.code = g_IndexedSprite_ps_DXIL; shader.code_size = sizeof(g_IndexedSprite_ps_DXIL);
	shader.num_samplers = 2; shader.num_uniform_buffers = 1;
	auto* fragment = SDL_CreateGPUShader(device, &shader); CHECK(fragment);
	SDL_GPUColorTargetDescription target {}; target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	target.blend_state.enable_blend = true;
	target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
	target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
	target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
	SDL_GPUGraphicsPipelineCreateInfo pipelineInfo {}; pipelineInfo.vertex_shader = vertex; pipelineInfo.fragment_shader = fragment;
	pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	pipelineInfo.target_info.num_color_targets = 1; pipelineInfo.target_info.color_target_descriptions = &target;
	auto* pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo); CHECK(pipeline);
	SDL_GPUTextureCreateInfo textureInfo {}; textureInfo.type = SDL_GPU_TEXTURETYPE_2D; textureInfo.format = target.format;
	textureInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; textureInfo.width = textureInfo.height = 2;
	textureInfo.layer_count_or_depth = textureInfo.num_levels = 1;
	auto* texture = SDL_CreateGPUTexture(device, &textureInfo); CHECK(texture);
	SDL_GPUSamplerCreateInfo samplerInfo {};
	auto* sampler = SDL_CreateGPUSampler(device, &samplerInfo); CHECK(sampler);
	auto* commands = SDL_AcquireGPUCommandBuffer(device); CHECK(commands);
	const unsigned uniforms[12] = {11,5,0,0,5,6,5,0,unsigned(view.X),unsigned(view.Y),2,2};
	SDL_PushGPUFragmentUniformData(commands,0,uniforms,sizeof(uniforms));
	SDL_GPUColorTargetInfo output {}; output.texture = texture; output.clear_color = {0,1,0,1};
	output.load_op = SDL_GPU_LOADOP_CLEAR; output.store_op = SDL_GPU_STOREOP_STORE;
	auto* pass = SDL_BeginGPURenderPass(commands,&output,1,nullptr); CHECK(pass);
	SDL_BindGPUGraphicsPipeline(pass,pipeline);
	const SDL_GPUTextureSamplerBinding bindings[] = {{view.Texture,sampler},{view.Palette,sampler}};
	SDL_BindGPUFragmentSamplers(pass,0,bindings,2); SDL_DrawGPUPrimitives(pass,3,1,0,0); SDL_EndGPURenderPass(pass);
	CHECK(SDL_SubmitGPUCommandBuffer(commands));
	CHECK(Read(device,{texture,nullptr,0,0},2,2,4,expected));
	SDL_ReleaseGPUSampler(device,sampler); SDL_ReleaseGPUTexture(device,texture);
	SDL_ReleaseGPUGraphicsPipeline(device,pipeline); SDL_ReleaseGPUShader(device,vertex); SDL_ReleaseGPUShader(device,fragment);
	return true;
}

static bool Run(SDL_GPUDevice* device, bool r32)
{
	FrameAtlas atlas; FrameAtlas::Options options;
	options.PageSize = 4; options.MaxPages = 3; options.ForceR32 = r32;
	CHECK(atlas.Create(device, options));
	FrameCache::Cache cache;
	std::uintptr_t identity = 1;
	auto make = [&](int side, unsigned char value) {
		const auto resource = cache.Register(identity++, {side,side,{{0,0,side,side,0,0,size_t(side*side)}}});
		return cache.GetOrDecode(resource,0,[&](const auto&, auto& pixels) { pixels.assign(size_t(side*side), value); return FrameCache::Status::Ready; }).Frame->ID;
	};
	PaletteCache::Cache paletteCache;
	PaletteCache::Data palette; palette.Shift = {11,5,0}; palette.Bits = {5,6,5}; palette.Colors[7] = 0xF800;
	auto colors = paletteCache.Acquire(palette);
	FrameAtlas::Request a {make(2,7),colors}, b {make(2,9),colors}, c {make(4,11),colors}, d {make(4,13),colors}, e {make(4,15),colors};
	const std::array first {a,a};
	CHECK(FrameAtlas::BeginResult::Ready == atlas.Begin(cache,first));
	const auto av = atlas.Resolve(a); CHECK(av.Texture && av.Palette);
	CHECK(Submit(atlas,device));
	CHECK(1u == atlas.GetStats().FrameUploads && 1u == atlas.GetStats().PaletteUploads);
	const std::array second {a,b,c,d};
	CHECK(FrameAtlas::BeginResult::Ready == atlas.Begin(cache,second));
	const auto bv = atlas.Resolve(b), cv = atlas.Resolve(c), dv = atlas.Resolve(d);
	CHECK(av.Texture == bv.Texture && av.Texture != cv.Texture && cv.Texture != dv.Texture);
	CHECK(Submit(atlas,device));
	CHECK(Read(device,av,2,2,atlas.GetStats().BytesPerPixel,7)); // Appending B did not destroy A.
	CHECK(Read(device,bv,2,2,atlas.GetStats().BytesPerPixel,9));
	CHECK(Read(device,cv,4,4,atlas.GetStats().BytesPerPixel,11));
	CHECK(Read(device,dv,4,4,atlas.GetStats().BytesPerPixel,13));
	CHECK(3u == atlas.GetStats().Pages && 4u == atlas.GetStats().FrameUploads);
	CHECK(Read(device,{av.Palette,nullptr,7,0},1,1,4,0xF800));
	CHECK(Draw(device,av,0xFF0000FF)); // RGB565 red, actual indexed shader.
	CHECK(Draw(device,bv,0xFF000000)); // Nonzero index with black palette remains opaque; offset region.
	const auto uploaded = atlas.GetStats().IndexBytes;
	for (int i = 0; i < 300; ++i)
	{
		SDL_WaitForGPUIdle(device); atlas.Poll(); // Deterministic test only; production never waits here.
		CHECK(FrameAtlas::BeginResult::Ready == atlas.Begin(cache,second));
		CHECK(!atlas.HasUploads()); CHECK(Submit(atlas,device));
	}
	CHECK(uploaded == atlas.GetStats().IndexBytes);
	const std::array allPinned {a,c,d,e};
	SDL_WaitForGPUIdle(device); atlas.Poll();
	CHECK(FrameAtlas::BeginResult::Ready == atlas.Begin(cache,allPinned));
	CHECK(!atlas.Resolve(e).Texture && atlas.Resolve(a).Texture);
	CHECK(Submit(atlas,device));
	SDL_WaitForGPUIdle(device); atlas.Poll();
	const std::array onlyE {e};
	CHECK(FrameAtlas::BeginResult::Ready == atlas.Begin(cache,onlyE));
	const auto reserved = atlas.Resolve(e); CHECK(reserved.Texture && atlas.HasUploads());
	const auto beforeCancel = atlas.GetStats().FrameUploads;
	atlas.CancelBatch(); // No command encoded/submitted, reservation must be reused.
	CHECK(beforeCancel == atlas.GetStats().FrameUploads);
	CHECK(FrameAtlas::BeginResult::Ready == atlas.Begin(cache,onlyE));
	CHECK(reserved.Texture == atlas.Resolve(e).Texture && reserved.X == atlas.Resolve(e).X);
	CHECK(Submit(atlas,device)); CHECK(!atlas.Resolve(e).Texture); // Resolve is batch-local.
	CHECK(Read(device,reserved,4,4,atlas.GetStats().BytesPerPixel,15));
	CHECK(beforeCancel + 1 == atlas.GetStats().FrameUploads);
	const auto beforeColor = atlas.GetStats().IndexBytes;
	palette.Colors[15] = 0x001F;
	const std::array recolored {FrameAtlas::Request {e.Frame,paletteCache.Acquire(palette)}};
	SDL_WaitForGPUIdle(device); atlas.Poll();
	CHECK(FrameAtlas::BeginResult::Ready == atlas.Begin(cache,recolored));
	const auto colorView = atlas.Resolve(recolored[0]); CHECK(Submit(atlas,device));
	CHECK(beforeColor == atlas.GetStats().IndexBytes && 2u == atlas.GetStats().PaletteUploads);
	CHECK(Read(device,{colorView.Palette,nullptr,15,0},1,1,4,0x001F));
	CHECK(Draw(device,colorView,0xFFFF0000));
	SDL_WaitForGPUIdle(device); atlas.Release();
	CHECK(atlas.Create(device,options)); // Device-resource rebuild preserves CPU FrameID.
	CHECK(FrameAtlas::BeginResult::Ready == atlas.Begin(cache,onlyE));
	const auto rebuilt = atlas.Resolve(e); CHECK(rebuilt.Texture && Submit(atlas,device));
	CHECK(Read(device,rebuilt,4,4,atlas.GetStats().BytesPerPixel,15));
	cache.Clear(); SDL_WaitForGPUIdle(device); atlas.Poll();
	CHECK(FrameAtlas::BeginResult::Ready == atlas.Begin(cache,{}));
	CHECK(0u == atlas.GetStats().Pages && !atlas.Resolve(e).Texture);
	atlas.CancelBatch(); SDL_WaitForGPUIdle(device); atlas.Release();
	CHECK(atlas.Create(device,options));
	palette.Colors[0] = 0xF800; // Transparency follows the index, not the palette color.
	const std::array transparent {FrameAtlas::Request {make(2,0),paletteCache.Acquire(palette)}};
	CHECK(FrameAtlas::BeginResult::Ready == atlas.Begin(cache,transparent));
	const auto transparentView = atlas.Resolve(transparent[0]); CHECK(Submit(atlas,device));
	CHECK(Draw(device,transparentView,0xFF00FF00));
	SDL_WaitForGPUIdle(device); atlas.Release();
	return true;
}

int main()
{
	if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
	auto* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL, false, "direct3d12");
	if (!device) { std::fprintf(stderr,"%s\n",SDL_GetError()); return 1; }
	std::printf("driver=%s R8_UINT=%d R32_UINT=%d\n", SDL_GetGPUDeviceDriver(device),
		SDL_GPUTextureSupportsFormat(device,SDL_GPU_TEXTUREFORMAT_R8_UINT,SDL_GPU_TEXTURETYPE_2D,SDL_GPU_TEXTUREUSAGE_SAMPLER),
		SDL_GPUTextureSupportsFormat(device,SDL_GPU_TEXTUREFORMAT_R32_UINT,SDL_GPU_TEXTURETYPE_2D,SDL_GPU_TEXTUREUSAGE_SAMPLER));
	std::printf("Load: R8=%d R32=%d\n",
		SDL_GPUTextureSupportsFormat(device,SDL_GPU_TEXTUREFORMAT_R8_UINT,SDL_GPU_TEXTURETYPE_2D,SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ),
		SDL_GPUTextureSupportsFormat(device,SDL_GPU_TEXTUREFORMAT_R32_UINT,SDL_GPU_TEXTURETYPE_2D,SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ));
	const bool ok = Run(device,false) && Run(device,true);
	SDL_WaitForGPUIdle(device); SDL_DestroyGPUDevice(device); SDL_Quit();
	if (!ok) return 1;
	std::puts("FrameAtlas GPU: R8/R32, same-page append, three pages, 300 no-upload submissions, pinning and cancellation passed.");
}
