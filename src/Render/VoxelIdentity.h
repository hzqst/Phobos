#pragma once

#include "FrameCache.h"
#include <array>
#include <unordered_map>

// P1/P2 only: candidate identities and a bounded shadow oracle. No fast acquisition path.
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
		std::unordered_map<Key, FrameCache::FrameID, KeyHash> Entries;
		unsigned Epoch = 0;
		bool Disabled = false;
	public:
		size_t Size() const { return Entries.size(); }
		Result Observe(const Key& key, FrameCache::FrameID id, const FrameCache::Cache& frames)
		{
			if (Epoch != key.Epoch) { Entries.clear(); Epoch = key.Epoch; Disabled = false; }
			if (Disabled) return Result::Disabled;
			const auto* current = frames.Resolve(id);
			if (!current) return Result::Missing;
			auto found = Entries.find(key);
			const auto* previous = found == Entries.end() ? nullptr : frames.Resolve(found->second);
			if (!previous)
			{
				if (Entries.size() >= MaxEntries) Entries.clear();
				Entries[key] = id;
				return Result::Seeded;
			}
			if (previous->Geometry.Width != current->Geometry.Width || previous->Geometry.Height != current->Geometry.Height
				|| previous->Indices != current->Indices)
			{
				Disabled = true; Entries.clear(); // A mismatch poisons this epoch, including evicted keys.
				return Result::Mismatch;
			}
			return Result::Equal;
		}
	};
}
