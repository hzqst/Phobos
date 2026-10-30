#pragma once

#include "ABufferSnapshot.h"
#include "TerrainPass.h"

#include <Shaders/ABufferFrame.ps.dxil.h>
#include <Shaders/ABufferFrame.ps.spirv.h>

// The ABuffer the terrain shader reads: an R8 texture of ViewBounds' size, cleared to 127, then the
// snapshot's shroud and fog frames in their own render pass before the present's. Every overwrite is
// drawn before every darken; the trace's split count checks that this equals the native order of both
// passes per cell. Words above 255, which only abnormal input could write, are not representable.
// Only GPURender.cpp includes this.
namespace ABufferPass
{
	constexpr auto Passes = ABufferSnapshot::Passes;
	struct ModeUniforms { Uint32 Mode[4]; };

	inline SDL_GPUGraphicsPipeline* Pipelines[Passes] {};
	inline SDL_GPUTexture* Texture = nullptr;
	inline int Width = 0, Height = 0;
	inline TerrainPass::InstanceBuffer Frames;
	inline std::vector<TerrainBatch::Run> Runs[Passes];

	inline bool Create(SDL_GPUDevice* device)
	{
		const auto pixel = GPUShader::Make(g_ABufferFrame_ps_DXIL, g_ABufferFrame_ps_SPIRV);
		SDL_GPUColorTargetDescription color {};
		color.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
		Pipelines[0] = TerrainPass::CreatePipeline(device, pixel, 1, color);
		// 47F250: old + v - 127, which is old minus the shader's (127 - v), clamped at 0 by UNORM.
		color.blend_state.enable_blend = true;
		color.blend_state.src_color_blendfactor = color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		color.blend_state.src_alpha_blendfactor = color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		color.blend_state.color_blend_op = color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
		Pipelines[1] = TerrainPass::CreatePipeline(device, pixel, 1, color);
		return Pipelines[0] && Pipelines[1];
	}

	inline void Release(SDL_GPUDevice* device)
	{
		for (auto*& pipeline : Pipelines)
		{
			if (pipeline) SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
			pipeline = nullptr;
		}
		if (Texture) SDL_ReleaseGPUTexture(device, Texture);
		Texture = nullptr;
		Width = Height = 0;
		Frames.Release(device);
		Frames.Instances.clear();
		for (auto& runs : Runs) runs.clear();
	}

	inline FrameAtlas::Request Request(FrameCache::FrameID frame)
	{
		return {frame, {}, FrameAtlas::SourceKind::Mask, &ABufferSnapshot::Frames};
	}

	inline void AppendRequests(const ABufferSnapshot::Snapshot& snapshot, std::vector<FrameAtlas::Request>& requests)
	{
		if (!snapshot.Ready()) return;
		for (const auto& write : snapshot.Writes)
			for (const auto& id : write.ID)
				if (id) requests.push_back(Request(id));
	}

	// After FrameAtlas::Begin accepted the requests. width x height is ViewBounds' size; without a
	// ready snapshot of the same view the texture stays cleared to 127. A null snapshot draws nothing.
	inline bool Prepare(SDL_GPUDevice* device, const FrameAtlas& atlas, const ABufferSnapshot::Snapshot* snapshot, int width, int height)
	{
		auto& instances = Frames.Instances;
		instances.clear();
		for (auto& runs : Runs) runs.clear();
		if (!snapshot)
		{
			Frames.Pending = false;
			return true;
		}
		if (width <= 0 || height <= 0) return false;
		if (!Texture || width != Width || height != Height)
		{
			if (Texture) SDL_ReleaseGPUTexture(device, Texture);
			SDL_GPUTextureCreateInfo texture {};
			texture.type = SDL_GPU_TEXTURETYPE_2D;
			texture.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
			texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
			texture.width = Uint32(width);
			texture.height = Uint32(height);
			texture.layer_count_or_depth = 1;
			texture.num_levels = 1;
			Texture = SDL_CreateGPUTexture(device, &texture);
			Width = Texture ? width : 0;
			Height = Texture ? height : 0;
			if (!Texture) return false;
		}
		const auto& view = snapshot->View;
		const bool drawn = snapshot->Ready() && view.Width == width && view.Height == height;
		for (int pass = 0; pass < Passes; ++pass)
		{
			std::vector<const void*> pages;
			const auto first = std::uint32_t(instances.size());
			for (const auto& write : snapshot->Writes)
			{
				const auto* record = drawn ? ABufferSnapshot::Frames.Resolve(write.ID[pass]) : nullptr;
				if (!record) continue;
				const auto atlasView = atlas.Resolve(Request(write.ID[pass]));
				const auto& g = record->Geometry;
				// The writers address the buffer by x and the row below ViewBounds.Y; ViewBounds.X is 0.
				const TerrainBatch::Rect clip {write.Clip.X, write.Clip.Y - view.Y, write.Clip.Width, write.Clip.Height};
				TerrainBatch::Instance instance;
				if (!atlasView.Texture || !TerrainBatch::Place(write.X + g.Left, write.Y + g.Top - view.Y, g.Width, g.Height, 0, clip,
					atlasView.X, atlasView.Y, 0, 0, 0, false, instance)) continue;
				instances.push_back(instance);
				pages.push_back(atlasView.Texture);
			}
			Runs[pass] = TerrainBatch::Split(pages);
			for (auto& run : Runs[pass]) run.First += first;
		}
		return Frames.Prepare(device);
	}

	inline bool HasUploads() { return Frames.Pending; }

	inline bool Upload(SDL_GPUDevice* device, SDL_GPUCopyPass* pass) { return Frames.Upload(device, pass); }

	// Its own render pass, after the uploads and before the present's render pass samples the texture.
	inline bool Render(SDL_GPUCommandBuffer* commands)
	{
		if (!Texture) return true;
		SDL_GPUColorTargetInfo target {};
		target.texture = Texture;
		target.clear_color = {float(TerrainImage::DefaultAlpha) / 255.0f, 0.0f, 0.0f, 1.0f};
		target.load_op = SDL_GPU_LOADOP_CLEAR;
		target.store_op = SDL_GPU_STOREOP_STORE;
		target.cycle = true;
		auto* pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);
		if (!pass) return false;
		if (Frames.Buffer && !Frames.Instances.empty())
		{
			SDL_BindGPUVertexStorageBuffers(pass, 0, &Frames.Buffer, 1);
			for (int mode = 0; mode < Passes; ++mode)
			{
				if (Runs[mode].empty()) continue;
				SDL_BindGPUGraphicsPipeline(pass, Pipelines[mode]);
				const ModeUniforms uniforms {{Uint32(mode), 0, 0, 0}};
				SDL_PushGPUFragmentUniformData(commands, 0, &uniforms, sizeof(uniforms));
				for (const auto& run : Runs[mode])
				{
					const SDL_GPUTextureSamplerBinding binding {static_cast<SDL_GPUTexture*>(const_cast<void*>(run.Page)), TerrainPass::Sampler};
					SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
					const TerrainPass::ViewUniforms view {{Uint32(Width), Uint32(Height), run.First, 0}};
					SDL_PushGPUVertexUniformData(commands, 0, &view, sizeof(view));
					SDL_DrawGPUPrimitives(pass, run.Count * TerrainPass::VerticesPerTile, 1, 0, 0);
				}
			}
		}
		SDL_EndGPURenderPass(pass);
		return true;
	}
}
