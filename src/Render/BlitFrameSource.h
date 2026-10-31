#pragma once

#include "FrameCache.h"
#include "ShapeImage.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

// The 8-bit images RenderLayers blits outside 4AED70 (gamemd-reverse #26 §5): a voxel's working surface
// or a unit's scratch through 4373B0, and an entry of a type's voxel cache through 437A10. A surface is
// redrawn between calls, so its pixels are copied at the call and kept by content; a cache entry stays
// the same until 5F99E0 resets the cache's arena, so it is kept by its address and size within an Epoch.
// Entries unused for MaxIdle captures are dropped every SweepInterval captures, or all those unused in
// the last capture once the cache is three quarters full. Engine-free: callers pass readable memory.
namespace BlitFrameSource
{
	constexpr FrameCache::Limits Budget {32 * 1024 * 1024, 16384, 16384};
	constexpr unsigned SweepInterval = 64, MaxIdle = 256;

	struct Entry
	{
		FrameCache::ResourceID Resource;
		FrameCache::FrameID Frame;
		unsigned LastUse = 0;
	};
	struct Address
	{
		std::uintptr_t Data = 0;
		int Width = 0, Height = 0;
		unsigned Epoch = 0;
		bool operator==(const Address&) const = default;
	};
	struct AddressHash
	{
		size_t operator()(const Address& a) const
		{
			return a.Data ^ (size_t(a.Width) << 20) ^ (size_t(a.Height) << 8) ^ (size_t(a.Epoch) * 0x9E3779B9u);
		}
	};

	extern FrameCache::Cache Frames;
	extern std::unordered_multimap<std::uint32_t, Entry> ByContent;
	extern std::unordered_map<Address, Entry, AddressHash> ByAddress;
	// Last use through a verified identity, bounded by the live frames and removed by Sweep/Clear.
	extern std::unordered_map<std::uint64_t, unsigned> IdentityUses;
	// 5F99E0's resets, the captures begun, and the identity of the next resource.
	extern unsigned Epoch, Serial;
	extern std::uintptr_t NextIdentity;

	// FNV-1a over the size and the pixels.
	inline std::uint32_t Hash(const std::vector<std::uint8_t>& pixels, int width, int height)
	{
		std::uint32_t hash = 2166136261u;
		const auto mix = [&hash](std::uint8_t byte) { hash = (hash ^ byte) * 16777619u; };
		for (int i = 0; i < 4; ++i) { mix(std::uint8_t(width >> (8 * i))); mix(std::uint8_t(height >> (8 * i))); }
		for (const auto byte : pixels) mix(byte);
		return hash;
	}

	// A resource of one frame holding the pixels; no entry where the budget is spent.
	inline Entry Register(int width, int height, std::vector<std::uint8_t>&& pixels)
	{
		if (!++NextIdentity) ++NextIdentity;
		FrameCache::ResourceInfo info {width, height, {{0, 0, width, height, 0, 0, pixels.size()}}};
		const auto resource = Frames.Register(NextIdentity, info);
		if (!resource) return {};
		const auto result = Frames.GetOrDecode(resource, 0, [&pixels](const auto&, auto& indices)
		{
			indices = std::move(pixels);
			return FrameCache::Status::Ready;
		});
		if (result.State != FrameCache::Status::Ready)
		{
			Frames.Invalidate(resource);
			return {};
		}
		return {resource, result.Frame->ID, Serial};
	}

	// The rows of a surface from its first pixel, by content.
	inline FrameCache::FrameID AcquirePixels(const std::uint8_t* first, std::ptrdiff_t pitch, int width, int height)
	{
		if (!first || width <= 0 || height <= 0 || width > ShapeImage::MaxDimension || height > ShapeImage::MaxDimension) return {};
		std::vector<std::uint8_t> pixels(size_t(width) * height);
		for (int y = 0; y < height; ++y)
			std::copy_n(first + y * pitch, width, pixels.data() + size_t(y) * width);
		const auto hash = Hash(pixels, width, height);
		for (auto [it, end] = ByContent.equal_range(hash); it != end; ++it)
		{
			const auto* frame = Frames.Resolve(it->second.Frame);
			if (frame && frame->Geometry.Width == width && frame->Geometry.Height == height && frame->Indices == pixels)
			{
				it->second.LastUse = Serial;
				return it->second.Frame;
			}
		}
		const auto entry = Register(width, height, std::move(pixels));
		if (!entry.Frame) return {};
		ByContent.emplace(hash, entry);
		return entry.Frame;
	}

	// An RLE frame (the SHP compression 661590 writes) of width x height at data, of at most bound bytes,
	// by address within the current Epoch.
	inline FrameCache::FrameID AcquireEncoded(const std::uint8_t* data, size_t bound, int width, int height)
	{
		if (!data || width <= 0 || height <= 0 || width > ShapeImage::MaxDimension || height > ShapeImage::MaxDimension) return {};
		const Address key {reinterpret_cast<std::uintptr_t>(data), width, height, Epoch};
		if (const auto found = ByAddress.find(key); found != ByAddress.end())
		{
			if (Frames.Resolve(found->second.Frame))
			{
				found->second.LastUse = Serial;
				return found->second.Frame;
			}
			ByAddress.erase(found);
		}
		size_t length = 0;
		for (int y = 0; y < height; ++y)
		{
			if (bound - length < 2) return {};
			const size_t line = data[length] | (size_t(data[length + 1]) << 8);
			if (line < 2 || line > bound - length) return {};
			length += line;
		}
		std::vector<std::uint8_t> pixels;
		if (!ShapeImage::Decode({data, length}, width, height, true, pixels)) return {};
		const auto entry = Register(width, height, std::move(pixels));
		if (!entry.Frame) return {};
		ByAddress.emplace(key, entry);
		return entry.Frame;
	}

	inline bool Touch(FrameCache::FrameID id)
	{
		if (!Frames.Resolve(id)) return false;
		IdentityUses[id.Value] = Serial;
		return true;
	}

	// Drops the entries unused for at least idle captures (at least 1).
	inline void Sweep(unsigned idle)
	{
		if (!idle) idle = 1;
		const auto stale = [idle](const Entry& entry)
		{
			const auto used = IdentityUses.find(entry.Frame.Value);
			return Serial - entry.LastUse >= idle && (used == IdentityUses.end() || Serial - used->second >= idle);
		};
		for (auto it = ByContent.begin(); it != ByContent.end();)
			if (stale(it->second)) { IdentityUses.erase(it->second.Frame.Value); Frames.Invalidate(it->second.Resource); it = ByContent.erase(it); } else ++it;
		for (auto it = ByAddress.begin(); it != ByAddress.end();)
			if (stale(it->second)) { IdentityUses.erase(it->second.Frame.Value); Frames.Invalidate(it->second.Resource); it = ByAddress.erase(it); } else ++it;
	}

	inline void BeginCapture()
	{
		++Serial;
		if (Frames.GetStats().IndexBytes > Budget.IndexBytes / 4 * 3) Sweep(1);
		else if (Serial % SweepInterval == 0) Sweep(MaxIdle);
	}

	// 5F99E0 resets the arena: an address may now hold another image.
	inline void Purged() { ++Epoch; }

	inline void Clear()
	{
		ByContent.clear();
		ByAddress.clear();
		IdentityUses.clear();
		Frames.Clear();
	}
}
