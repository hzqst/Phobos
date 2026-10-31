#pragma once

#include "FrameCache.h"
#include <array>
#include <unordered_map>

// Candidate identities, a bounded oracle, and conservative fast-path admission.
// All addresses are opaque tokens; engine memory is never accessed by this header.
namespace VoxelIdentity
{
	constexpr size_t MaxParts = 16, MaxDepth = 8, MaxEntries = 512;
	struct Component
	{
		std::uintptr_t Payload = 0, Blitter = 0;
		// Bounds, destination, source limit, source part, then Z/gradient/light/tint.
		std::array<int, 20> Parameters {};
		bool operator==(const Component&) const = default;
	};
	struct Key
	{
		unsigned Epoch = 0;
		size_t Count = 0;
		std::array<Component, MaxParts> Parts {};
		// Actual source pixels acquired after final clipping, including a bridge split.
		std::array<int, 4> Region {};
		// P3's single RLEBlitTransXlat<byte> component uses this complete translation table.
		// Kept out of the hash deliberately: equality still distinguishes tables without pixel hashing.
		std::array<std::uint8_t, 256> Translation {};
		bool operator==(const Key&) const = default;
	};
	struct KeyHash
	{
		size_t operator()(const Key& key) const
		{
			size_t hash = key.Epoch;
			const auto mix = [&](size_t value) { hash ^= value + 0x9E3779B9u + (hash << 6) + (hash >> 2); };
			mix(key.Count);
			for (size_t i = 0; i < key.Count; ++i)
			{
				mix(key.Parts[i].Payload); mix(key.Parts[i].Blitter);
				for (auto value : key.Parts[i].Parameters) mix(size_t(value));
			}
			for (auto value : key.Region) mix(size_t(value));
			return hash;
		}
	};
	class ScratchGuard
	{
		unsigned Depth = 0;
		bool Clean = false, Eligible = false, Written = false;
	public:
		static constexpr int Extent = 256;
		std::uintptr_t Surface = 0, Buffer = 0;
		std::array<int, 4> Dirty {};
		void BeginFrame() { *this = {}; }
		bool NeedsSeed(std::uintptr_t surface, std::uintptr_t buffer) const
		{
			return !Depth && (!Clean || Surface != surface || Buffer != buffer);
		}
		void Begin(std::uintptr_t surface, std::uintptr_t buffer, bool observedClean)
		{
			if (Depth++ != 0) { Clean = Eligible = false; return; }
			Surface = surface; Buffer = buffer;
			Eligible = surface && buffer && observedClean;
			Written = false; Dirty = {}; Clean = false;
		}
		void Unknown(std::uintptr_t surface)
		{
			if (Surface == surface) Clean = Eligible = false;
		}
		void Write(std::array<int, 4> rect)
		{
			if (Depth != 1 || Written || rect[0] < 0 || rect[1] < 0 || rect[2] <= 0 || rect[3] <= 0
				|| rect[2] > Extent || rect[3] > Extent || rect[0] > Extent - rect[2] || rect[1] > Extent - rect[3])
				{ Eligible = Clean = false; return; }
			Written = true; Dirty = rect;
		}
		bool CanReuse() const { return Depth == 1 && Eligible && Written; }
		void End(bool cleared)
		{
			Clean = CanReuse() && cleared;
			Eligible = false;
			if (Depth) --Depth;
		}
	};
	class Tracker
	{
		struct Scope { std::uintptr_t Token = 0, Scratch = 0; Key Identity; bool Valid = true; };
		std::array<Scope, MaxDepth> Stack {};
		size_t Depth = 0;
		bool Fault = false;
	public:
		unsigned ScopeErrors = 0;
		void BeginFrame(unsigned /*epoch*/)
		{
			if (Depth || Fault) ++ScopeErrors;
			Depth = 0; Fault = false;
		}
		void Begin(std::uintptr_t token, std::uintptr_t scratch, unsigned epoch)
		{
			if (Fault) return;
			if (Depth == MaxDepth) { Fault = true; ++ScopeErrors; return; }
			// Nested draws can overwrite the shared scratch. Reject both scopes conservatively.
			for (size_t i = 0; i < Depth; ++i) Stack[i].Valid = false;
			auto& scope = Stack[Depth];
			scope = {}; scope.Token = token; scope.Scratch = scratch;
			scope.Identity.Epoch = epoch; scope.Valid = Depth == 0 && scratch != 0;
			++Depth;
		}
		void End(std::uintptr_t token)
		{
			if (Fault) return;
			if (!Depth || Stack[Depth - 1].Token != token) { Fault = true; ++ScopeErrors; return; }
			--Depth;
		}
		bool Targets(std::uintptr_t surface) const
		{
			return !Fault && Depth && Stack[Depth - 1].Scratch == surface;
		}
		void Unknown(std::uintptr_t surface)
		{
			if (Targets(surface)) Stack[Depth - 1].Valid = false;
		}
		void Add(std::uintptr_t surface, const Component& part, unsigned epoch)
		{
			if (!Targets(surface)) return;
			auto& scope = Stack[Depth - 1];
			if (scope.Identity.Epoch != epoch || !part.Payload || scope.Identity.Count == MaxParts)
				{ scope.Valid = false; return; }
			if (scope.Valid) scope.Identity.Parts[scope.Identity.Count++] = part;
		}
		bool Candidate(std::uintptr_t source, std::array<int, 4> region, unsigned epoch, Key& out) const
		{
			if (!Targets(source)) return false;
			const auto& scope = Stack[Depth - 1];
			if (!scope.Valid || !scope.Identity.Count || scope.Identity.Epoch != epoch || region[2] <= 0 || region[3] <= 0) return false;
			out = scope.Identity; out.Region = region;
			return true;
		}
	};
	enum class Result { Seeded, Equal, Mismatch, Missing, Disabled };
	class Validator
	{
		struct Entry { FrameCache::FrameID Frame; bool Verified = false; };
		std::unordered_map<Key, Entry, KeyHash> Entries;
		unsigned Epoch = 0;
		bool Disabled = false;
	public:
		size_t Size() const { return Entries.size(); }
		FrameCache::FrameID Lookup(const Key& key, const FrameCache::Cache& frames) const
		{
			if (Disabled || Epoch != key.Epoch) return {};
			const auto found = Entries.find(key);
			if (found == Entries.end() || !found->second.Verified || !frames.Resolve(found->second.Frame)) return {};
			return found->second.Frame;
		}
		Result Observe(const Key& key, FrameCache::FrameID id, const FrameCache::Cache& frames)
		{
			if (Epoch != key.Epoch) { Entries.clear(); Epoch = key.Epoch; Disabled = false; }
			if (Disabled) return Result::Disabled;
			const auto* current = frames.Resolve(id);
			if (!current) return Result::Missing;
			auto found = Entries.find(key);
			const auto* previous = found == Entries.end() ? nullptr : frames.Resolve(found->second.Frame);
			if (!previous)
			{
				if (Entries.size() >= MaxEntries) Entries.clear();
				Entries[key] = {id, false};
				return Result::Seeded;
			}
			if (previous->Geometry.Width != current->Geometry.Width || previous->Geometry.Height != current->Geometry.Height
				|| previous->Indices != current->Indices)
			{
				Disabled = true; Entries.clear(); // A mismatch poisons this epoch, including evicted keys.
				return Result::Mismatch;
			}
			found->second.Verified = true;
			return Result::Equal;
		}
	};
}
