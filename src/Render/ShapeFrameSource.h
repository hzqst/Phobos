#pragma once

#include "CursorSnapshot.h"
#include "FrameCache.h"
#include "ShapeImage.h"
#include <algorithm>
#include <unordered_map>

// Engine adapter: used only at the native post-draw capture and lifecycle hooks.
// Cache records contain copied indices; numeric aliases below are never dereferenced.
namespace ShapeFrameSource
{
	constexpr size_t MaxEncodedBytes = 16 * 1024 * 1024;
	constexpr size_t MaxAliases = 512;
	extern FrameCache::Cache Frames;
	extern std::unordered_map<std::uintptr_t, std::uintptr_t> Aliases;

	inline bool Invalidate(std::uintptr_t address)
	{
		const auto alias = Aliases.find(address);
		const auto data = alias == Aliases.end() ? address : alias->second;
		const auto resource = Frames.LookupResource(data);
		if (!resource) return false;
		for (auto it = Aliases.begin(); it != Aliases.end();)
			if (it->second == data) it = Aliases.erase(it);
			else ++it;
		return Frames.Invalidate(resource);
	}

	inline void Clear()
	{
		Aliases.clear();
		Frames.Clear();
	}

	inline FrameCache::Status Decode(const SHPStruct* source, const FrameCache::Descriptor& d,
		std::vector<std::uint8_t>& indices)
	{
		using CursorSnapshot::Readable;
		const auto base = reinterpret_cast<std::uintptr_t>(source);
		if (d.Offset > UINTPTR_MAX - base) return FrameCache::Status::InvalidData;
		const auto* encoded = reinterpret_cast<const std::uint8_t*>(base + d.Offset);
		const auto bound = std::min(d.EncodedBound, size_t(UINTPTR_MAX - reinterpret_cast<std::uintptr_t>(encoded)));
		size_t bytes = size_t(d.Width) * d.Height;
		if (d.Flags & 2)
		{
			bytes = 0;
			for (int y = 0; y < d.Height; ++y)
			{
				if (bytes > bound || bound - bytes < 2 || !Readable(encoded + bytes, 2))
					return FrameCache::Status::InvalidData;
				const size_t length = encoded[bytes] | (size_t(encoded[bytes + 1]) << 8);
				if (length < 2 || length > bound - bytes || !Readable(encoded + bytes, length))
					return FrameCache::Status::InvalidData;
				bytes += length;
			}
		}
		if (bytes > bound || !Readable(encoded, bytes)) return FrameCache::Status::InvalidData;
		return ShapeImage::Decode({encoded, bytes}, d.Width, d.Height, (d.Flags & 2) != 0, indices)
			? FrameCache::Status::Ready : FrameCache::Status::InvalidData;
	}

	inline FrameCache::Result Acquire(const SHPStruct* original, int index, std::uintptr_t& dataIdentity)
	{
		using CursorSnapshot::Readable;
		using FrameCache::Status;
		dataIdentity = 0;
		if (!Readable(original, sizeof(*original))) return {Status::NotReady};
		const auto identity = reinterpret_cast<std::uintptr_t>(original);
		const SHPStruct* source = original;
		if (source->IsReference())
		{
			const auto* reference = source->AsReference();
			if (!Readable(reference, sizeof(*reference)) || !reference->Loaded)
			{
				Invalidate(identity);
				return {Status::NotReady};
			}
			source = reference->Data;
		}
		if (!Readable(source, sizeof(*source)) || source->IsReference() || index < 0 || index >= source->Frames)
			return {Status::InvalidData};
		const auto base = reinterpret_cast<std::uintptr_t>(source);
		if (const auto alias = Aliases.find(identity); alias != Aliases.end() && alias->second != base)
			Invalidate(identity);
		auto resource = Frames.LookupResource(base);
		const auto* info = Frames.Describe(resource);
		if (info && (info->Width != source->Width || info->Height != source->Height || info->Frames.size() != size_t(source->Frames)))
		{
			Invalidate(base);
			resource = {};
		}
		if (resource)
		{
			const auto headerOffset = sizeof(SHPStruct) + size_t(index) * sizeof(SHPFrame);
			if (headerOffset > UINTPTR_MAX - base || !Readable(reinterpret_cast<const void*>(base + headerOffset), sizeof(SHPFrame)))
				return {Status::InvalidData};
			const auto h = source->AsFile()->GetFrameHeader(index);
			const auto& d = Frames.Describe(resource)->Frames[index];
			const auto headers = sizeof(SHPStruct) + size_t(source->Frames) * sizeof(SHPFrame);
			const bool validOffset = h.Offset >= 0 && size_t(h.Offset) >= headers;
			if (validOffset && (d.Left != h.Left || d.Top != h.Top || d.Width != h.Width || d.Height != h.Height
				|| d.Flags != h.Flags || d.Offset != size_t(h.Offset)))
			{
				Invalidate(base);
				resource = {};
			}
			else if (!validOffset && d.Width != 0)
			{
				Invalidate(base);
				resource = {};
			}
		}
		if (!Aliases.contains(identity) && Aliases.size() >= MaxAliases) return {Status::BudgetExceeded};
		if (!resource)
		{
			const size_t headers = sizeof(SHPStruct) + size_t(source->Frames) * sizeof(SHPFrame);
			if (!Readable(source, headers)) return {Status::InvalidData};
			FrameCache::ResourceInfo description {source->Width, source->Height, {}};
			description.Frames.reserve(source->Frames);
			std::vector<size_t> offsets;
			offsets.reserve(source->Frames);
			for (int i = 0; i < source->Frames; ++i)
			{
				const auto h = source->AsFile()->GetFrameHeader(i);
				const bool validOffset = h.Offset >= 0 && size_t(h.Offset) >= headers;
				description.Frames.push_back({h.Left, h.Top, validOffset ? h.Width : 0, h.Height,
					h.Flags, validOffset ? size_t(h.Offset) : 0, MaxEncodedBytes});
				if (validOffset) offsets.push_back(size_t(h.Offset));
			}
			std::sort(offsets.begin(), offsets.end());
			for (auto& d : description.Frames)
			{
				const auto next = std::upper_bound(offsets.begin(), offsets.end(), d.Offset);
				if (next != offsets.end()) d.EncodedBound = std::min(d.EncodedBound, *next - d.Offset);
			}
			resource = Frames.Register(base, description);
			if (!resource) return {Status::BudgetExceeded};
		}
		Aliases[identity] = base;
		dataIdentity = base;
		return Frames.GetOrDecode(resource, unsigned(index), [source](const auto& d, auto& indices) { return Decode(source, d, indices); });
	}
}
