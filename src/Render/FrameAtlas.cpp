#include "FrameAtlas.h"
#include "AtlasAllocator.h"
#include <SDL3/SDL_gpu.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

struct FrameAtlas::Impl
{
	struct Page
	{
		explicit Page(int side) : Allocator(side) { }
		AtlasAllocator Allocator;
		SDL_GPUTexture* Texture = nullptr;
		std::uint64_t LastUse = 0;
		bool Retiring = false, Pinned = false;
	};
	struct Location { Page* Owner; AtlasAllocator::Region Rect; bool Uploaded = false, Queued = false; };
	struct Color
	{
		PaletteCache::Data Data;
		SDL_GPUTexture* Texture = nullptr;
		std::uint64_t LastUse = 0;
		bool Uploaded = false, Queued = false, Retiring = false, Pinned = false;
	};
	struct Slot
	{
		SDL_GPUTransferBuffer* Transfer = nullptr;
		unsigned Capacity = 0;
		SDL_GPUFence* Fence = nullptr;
		std::uint64_t Serial = 0;
	};
	struct Upload
	{
		std::uint64_t ID;
		bool Palette;
		SDL_GPUTexture* Texture;
		AtlasAllocator::Region Rect;
		unsigned Offset, Pitch, BytesPerPixel;
		const FrameCache::FrameRecord* Source = nullptr; // Only consumed synchronously in Begin.
	};
	SDL_GPUDevice* Device = nullptr;
	SDL_GPUTextureFormat Format = SDL_GPU_TEXTUREFORMAT_INVALID;
	Options Config;
	Statistics Stats;
	std::vector<std::unique_ptr<Page>> Pages;
	std::unordered_map<std::uint64_t, Location> Locations;
	std::unordered_map<std::uint64_t, Color> Colors;
	std::array<Slot, 3> Slots;
	std::vector<Upload> Uploads;
	std::uint64_t Completed = 0, LastInvalidations = 0;
	int ActiveSlot = -1;
	unsigned UsedBytes = 0;
	bool Encoded = false, Failed = false;

	std::uint64_t PageBytes() const { return std::uint64_t(Config.PageSize) * Config.PageSize * Stats.BytesPerPixel; }
	SDL_GPUTexture* Texture(int width, int height, SDL_GPUTextureFormat format)
	{
		SDL_GPUTextureCreateInfo info {};
		info.type = SDL_GPU_TEXTURETYPE_2D; info.format = format; info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = width; info.height = height; info.layer_count_or_depth = info.num_levels = 1;
		auto* texture = SDL_CreateGPUTexture(Device, &info);
		if (!texture) Failed = true;
		return texture;
	}
	void Collect()
	{
		for (auto it = Pages.begin(); it != Pages.end();)
			if ((*it)->Retiring && (*it)->LastUse <= Completed)
			{
				SDL_ReleaseGPUTexture(Device, (*it)->Texture);
				it = Pages.erase(it);
			}
			else ++it;
		for (auto it = Colors.begin(); it != Colors.end();)
			if (it->second.Retiring && it->second.LastUse <= Completed)
			{
				SDL_ReleaseGPUTexture(Device, it->second.Texture);
				it = Colors.erase(it);
			}
			else ++it;
	}
	void Retire(Page* page)
	{
		page->Retiring = true;
		for (auto it = Locations.begin(); it != Locations.end();)
			if (it->second.Owner == page) it = Locations.erase(it); else ++it;
		++Stats.Evictions;
	}
	Location* Reserve(const FrameCache::FrameRecord& frame)
	{
		if (auto found = Locations.find(frame.ID.Value); found != Locations.end()) return &found->second;
		const int width = frame.Geometry.Width, height = frame.Geometry.Height;
		if (width <= 0 || height <= 0 || width > Config.PageSize || height > Config.PageSize) return nullptr;
		for (auto& page : Pages)
			if (!page->Retiring)
				if (const auto region = page->Allocator.Allocate(width, height))
					return &Locations.emplace(frame.ID.Value, Location {page.get(), *region}).first->second;
		const auto hasBudget = [&] { return Pages.size() < Config.MaxPages && (Pages.size() + 1) * PageBytes() <= Config.PageBudget; };
		if (!hasBudget())
		{
			Page* victim = nullptr;
			for (auto& page : Pages)
				if (!page->Retiring && !page->Pinned && (!victim || page->LastUse < victim->LastUse)) victim = page.get();
			if (victim) { Retire(victim); Collect(); }
		}
		if (!hasBudget()) return nullptr;
		auto page = std::make_unique<Page>(Config.PageSize);
		page->Texture = Texture(Config.PageSize, Config.PageSize, Format);
		if (!page->Texture) return nullptr;
		const auto region = page->Allocator.Allocate(width, height);
		auto* owner = page.get();
		Pages.push_back(std::move(page));
		return &Locations.emplace(frame.ID.Value, Location {owner, *region}).first->second;
	}
	Color* GetColor(const PaletteCache::Record& palette)
	{
		if (auto it = Colors.find(palette.ID.Value); it != Colors.end()) return it->second.Retiring ? nullptr : &it->second;
		if (Colors.size() >= Config.MaxPalettes)
		{
			Color* victim = nullptr;
			for (auto& [id, color] : Colors)
				if (!color.Pinned && !color.Retiring && (!victim || color.LastUse < victim->LastUse)) victim = &color;
			if (victim) { victim->Retiring = true; Collect(); }
		}
		if (Colors.size() >= Config.MaxPalettes) return nullptr;
		Color color; color.Data = palette.Values;
		color.Texture = Texture(256, 1, SDL_GPU_TEXTUREFORMAT_R32_UINT);
		if (!color.Texture) return nullptr;
		return &Colors.emplace(palette.ID.Value, std::move(color)).first->second;
	}
	bool Queue(std::uint64_t id, bool palette, SDL_GPUTexture* texture, AtlasAllocator::Region rect,
		unsigned bpp, const FrameCache::FrameRecord* source = nullptr)
	{
		const unsigned offset = AtlasAllocator::AlignedOffset(UsedBytes);
		const unsigned pitch = AtlasAllocator::RowPitch(rect.Width, bpp);
		const std::uint64_t end = std::uint64_t(offset) + std::uint64_t(pitch) * rect.Height;
		if (end > Config.StagingBudget) { ++Stats.BudgetMisses; return false; }
		Uploads.push_back({id, palette, texture, rect, offset, pitch, bpp, source});
		UsedBytes = unsigned(end);
		return true;
	}
};

FrameAtlas::FrameAtlas() = default;
FrameAtlas::~FrameAtlas() = default; // Explicit Release obeys renderer/device shutdown ordering.

bool FrameAtlas::Create(SDL_GPUDevice* device, Options options)
{
	if (State || !device || options.PageSize <= 0 || options.PageSize > 2048 || !options.MaxPages
		|| !options.MaxPalettes || !options.StagingBudget || options.StagingBudget > 16 * 1024 * 1024) return false;
	State = std::make_unique<Impl>();
	auto& s = *State; s.Device = device; s.Config = options;
	// SDL 3.4.18's D3D12 SAMPLER query checks SHADER_SAMPLE, which rejects
	// integer formats despite SHADER_LOAD support. Our shaders exclusively Load.
	// The graphics-storage query checks SHADER_LOAD on that backend; the actual
	// resource stays SAMPLER for SDL's combined texture/sampler binding contract.
	const auto queryUsage = std::strcmp(SDL_GetGPUDeviceDriver(device), "direct3d12") == 0
		? SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ : SDL_GPU_TEXTUREUSAGE_SAMPLER;
	s.Format = !options.ForceR32 && SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R8_UINT,
		SDL_GPU_TEXTURETYPE_2D, queryUsage) ? SDL_GPU_TEXTUREFORMAT_R8_UINT : SDL_GPU_TEXTUREFORMAT_R32_UINT;
	if (!SDL_GPUTextureSupportsFormat(device, s.Format, SDL_GPU_TEXTURETYPE_2D, queryUsage)
		|| !SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R32_UINT, SDL_GPU_TEXTURETYPE_2D, queryUsage))
	{
		State.reset(); return false;
	}
	s.Stats.BytesPerPixel = s.Format == SDL_GPU_TEXTUREFORMAT_R8_UINT ? 1 : 4;
	return true;
}

void FrameAtlas::Release()
{
	if (!State) return;
	auto& s = *State;
	for (auto& slot : s.Slots)
	{
		if (slot.Fence) SDL_ReleaseGPUFence(s.Device, slot.Fence);
		if (slot.Transfer) SDL_ReleaseGPUTransferBuffer(s.Device, slot.Transfer);
	}
	for (auto& page : s.Pages) SDL_ReleaseGPUTexture(s.Device, page->Texture);
	for (auto& [id, color] : s.Colors) SDL_ReleaseGPUTexture(s.Device, color.Texture);
	State.reset();
}

void FrameAtlas::Poll()
{
	if (!State) return;
	auto& s = *State;
	// Advance only a contiguous completed prefix, not the maximum signaled serial.
	for (;;)
	{
		Impl::Slot* oldest = nullptr;
		for (auto& slot : s.Slots)
			if (slot.Fence && (!oldest || slot.Serial < oldest->Serial)) oldest = &slot;
		if (!oldest || !SDL_QueryGPUFence(s.Device, oldest->Fence)) break;
		s.Completed = oldest->Serial;
		SDL_ReleaseGPUFence(s.Device, oldest->Fence); oldest->Fence = nullptr;
	}
	s.Collect();
}

FrameAtlas::BeginResult FrameAtlas::Begin(const FrameCache::Cache& cache, std::span<const Request> requests)
{
	if (!State || State->Failed || State->ActiveSlot >= 0) return BeginResult::Failed;
	Poll();
	auto& s = *State;
	for (size_t i = 0; i < s.Slots.size(); ++i)
		if (!s.Slots[i].Fence) { s.ActiveSlot = int(i); break; }
	if (s.ActiveSlot < 0) { ++s.Stats.Busy; return BeginResult::Busy; }
	s.UsedBytes = 0; s.Uploads.clear(); s.Encoded = false;
	for (auto& page : s.Pages) page->Pinned = false;
	for (auto& [id, color] : s.Colors) color.Pinned = false;
	if (s.LastInvalidations != cache.GetStats().Invalidations)
	{
		s.LastInvalidations = cache.GetStats().Invalidations;
		for (auto it = s.Locations.begin(); it != s.Locations.end();)
			if (!cache.Resolve(FrameCache::FrameID {it->first})) it = s.Locations.erase(it); else ++it;
		for (auto& page : s.Pages)
			if (!page->Retiring && std::none_of(s.Locations.begin(), s.Locations.end(),
				[&](const auto& pair) { return pair.second.Owner == page.get(); })) s.Retire(page.get());
		s.Collect();
	}
	// Pin all current requests before considering an LRU victim, including later draws.
	for (const auto& request : requests)
	{
		if (!request.Palette || !cache.Resolve(request.Frame)) continue;
		if (auto it = s.Locations.find(request.Frame.Value); it != s.Locations.end()) it->second.Owner->Pinned = true;
		if (auto it = s.Colors.find(request.Palette->ID.Value); it != s.Colors.end()) it->second.Pinned = true;
	}
	for (const auto& request : requests)
	{
		const auto* frame = cache.Resolve(request.Frame);
		if (!frame || !request.Palette) continue;
		auto* color = s.GetColor(*request.Palette);
		if (!color) { ++s.Stats.BudgetMisses; continue; }
		color->Pinned = true;
		if (!color->Uploaded && !color->Queued)
			color->Queued = s.Queue(request.Palette->ID.Value, true, color->Texture, {0,0,256,1}, 4);
		auto* location = s.Reserve(*frame);
		if (!location) { ++s.Stats.BudgetMisses; continue; }
		location->Owner->Pinned = true;
		if (!location->Uploaded && !location->Queued)
			location->Queued = s.Queue(frame->ID.Value, false, location->Owner->Texture, location->Rect, s.Stats.BytesPerPixel, frame);
	}
	if (s.Failed) { CancelBatch(); return BeginResult::Failed; }
	if (s.UsedBytes)
	{
		auto& slot = s.Slots[s.ActiveSlot];
		if (slot.Capacity < s.UsedBytes)
		{
			if (slot.Transfer) SDL_ReleaseGPUTransferBuffer(s.Device, slot.Transfer);
			SDL_GPUTransferBufferCreateInfo info {};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			info.size = std::min(s.Config.StagingBudget, std::max(s.UsedBytes, 64u * 1024));
			slot.Transfer = SDL_CreateGPUTransferBuffer(s.Device, &info);
			slot.Capacity = slot.Transfer ? info.size : 0;
			if (!slot.Transfer) { CancelBatch(); return BeginResult::Failed; }
		}
		auto* bytes = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(s.Device, slot.Transfer, false));
		if (!bytes) { CancelBatch(); return BeginResult::Failed; }
		std::memset(bytes, 0, s.UsedBytes);
		for (auto& upload : s.Uploads)
		{
			if (upload.Palette)
			{
				auto* row = reinterpret_cast<std::uint32_t*>(bytes + upload.Offset);
				const auto& palette = s.Colors.at(upload.ID).Data.Colors;
				for (size_t i = 0; i < palette.size(); ++i) row[i] = palette[i];
			}
			else
			{
				for (int y = 0; y < upload.Rect.Height; ++y)
				{
					auto* row = bytes + upload.Offset + y * upload.Pitch;
					const auto* source = upload.Source->Indices.data() + size_t(y) * upload.Rect.Width;
					if (upload.BytesPerPixel == 1) std::memcpy(row, source, upload.Rect.Width);
					else for (int x = 0; x < upload.Rect.Width; ++x) reinterpret_cast<std::uint32_t*>(row)[x] = source[x];
				}
			}
			upload.Source = nullptr;
		}
		SDL_UnmapGPUTransferBuffer(s.Device, slot.Transfer);
	}
	return BeginResult::Ready;
}

FrameAtlas::View FrameAtlas::Resolve(const Request& request) const
{
	if (!State || State->ActiveSlot < 0 || !request.Palette) return {};
	const auto& s = *State;
	const auto frame = s.Locations.find(request.Frame.Value);
	const auto color = s.Colors.find(request.Palette->ID.Value);
	if (frame == s.Locations.end() || color == s.Colors.end() || color->second.Retiring
		|| (!frame->second.Uploaded && !frame->second.Queued) || (!color->second.Uploaded && !color->second.Queued)) return {};
	return {frame->second.Owner->Texture, color->second.Texture, frame->second.Rect.X, frame->second.Rect.Y};
}

bool FrameAtlas::HasUploads() const { return State && !State->Uploads.empty(); }

void FrameAtlas::EncodeUploads(SDL_GPUCopyPass* pass)
{
	auto& s = *State;
	for (const auto& upload : s.Uploads)
	{
		SDL_GPUTextureTransferInfo source {};
		source.transfer_buffer = s.Slots[s.ActiveSlot].Transfer;
		source.offset = upload.Offset; source.pixels_per_row = upload.Pitch / upload.BytesPerPixel; source.rows_per_layer = upload.Rect.Height;
		SDL_GPUTextureRegion dest {};
		dest.texture = upload.Texture; dest.x = upload.Rect.X; dest.y = upload.Rect.Y;
		dest.w = upload.Rect.Width; dest.h = upload.Rect.Height; dest.d = 1;
		SDL_UploadToGPUTexture(pass, &source, &dest, false); // Append only; never cycle an atlas page.
	}
	s.Encoded = true;
}

bool FrameAtlas::Submit(SDL_GPUCommandBuffer* commands)
{
	auto& s = *State;
	// Even an upload-only/null-swapchain submission owns a slot until its fence completes.
	auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
	if (!fence) { s.Failed = true; CancelBatch(); return false; }
	auto& slot = s.Slots[s.ActiveSlot];
	slot.Fence = fence; slot.Serial = ++s.Stats.Submissions;
	for (auto& page : s.Pages) if (page->Pinned) page->LastUse = slot.Serial;
	for (auto& [id, color] : s.Colors) if (color.Pinned) color.LastUse = slot.Serial;
	if (s.Encoded)
	{
		for (const auto& upload : s.Uploads)
			if (upload.Palette)
			{
				s.Colors.at(upload.ID).Uploaded = true;
				++s.Stats.PaletteUploads; s.Stats.PaletteBytes += 256 * 4;
			}
			else
			{
				s.Locations.at(upload.ID).Uploaded = true;
				++s.Stats.FrameUploads; s.Stats.IndexBytes += std::uint64_t(upload.Rect.Width) * upload.Rect.Height * upload.BytesPerPixel;
			}
		s.Stats.TransferBytes += s.UsedBytes;
	}
	CancelBatch();
	return true;
}

void FrameAtlas::CancelBatch()
{
	if (!State) return;
	auto& s = *State;
	for (const auto& upload : s.Uploads)
		if (upload.Palette) s.Colors.at(upload.ID).Queued = false;
		else s.Locations.at(upload.ID).Queued = false;
	s.Uploads.clear(); s.ActiveSlot = -1; s.Encoded = false; s.UsedBytes = 0;
}

FrameAtlas::Statistics FrameAtlas::GetStats() const
{
	if (!State) return {};
	const auto& s = *State;
	auto stats = s.Stats;
	stats.Pages = unsigned(s.Pages.size()); stats.PageBytes = stats.Pages * s.PageBytes();
	stats.Palettes = unsigned(s.Colors.size());
	for (const auto& page : s.Pages) { stats.RetiringPages += page->Retiring; stats.OccupiedPixels += page->Allocator.UsedPixels(); }
	for (const auto& slot : s.Slots) stats.InFlight += slot.Fence != nullptr;
	return stats;
}
