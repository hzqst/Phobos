#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Main-thread CPU cache. Addresses are identity tokens, never dereferenced here.
namespace FrameCache
{
	template<class Tag> struct ID
	{
		std::uint64_t Value = 0;
		explicit operator bool() const { return Value != 0; }
		bool operator==(const ID&) const = default;
	};
	using ResourceID = ID<struct ResourceTag>;
	using FrameID = ID<struct FrameTag>;
	class IDSequence
	{
	public:
		explicit IDSequence(std::uint64_t first = 1) : NextValue(first) { }
		std::uint64_t Next();
	private:
		std::uint64_t NextValue;
	};
	struct Descriptor
	{
		int Left = 0, Top = 0, Width = 0, Height = 0;
		unsigned Flags = 0;
		std::size_t Offset = 0, EncodedBound = 0;
	};
	struct ResourceInfo
	{
		int Width = 0, Height = 0;
		std::vector<Descriptor> Frames;
	};
	struct FrameRecord
	{
		FrameID ID;
		ResourceID Resource;
		unsigned Index = 0;
		Descriptor Geometry;
		std::vector<std::uint8_t> Indices;
	};
	enum class Status { Ready, NotReady, InvalidData, BudgetExceeded, IDExhausted };
	struct Result { Status State; const FrameRecord* Frame = nullptr; };
	struct Limits
	{
		std::size_t IndexBytes = 64 * 1024 * 1024;
		std::size_t FrameSlots = 65536;
		std::size_t Resources = 256;
	};
	struct Statistics
	{
		std::uint64_t Hits = 0, Misses = 0, DecodeAttempts = 0, Decoded = 0;
		std::uint64_t Invalid = 0, BudgetRejected = 0, Invalidations = 0;
		std::size_t IndexBytes = 0, FrameSlots = 0, Resources = 0, Frames = 0;
	};
	class Cache
	{
	public:
		explicit Cache(Limits limits = {}) : Budget(limits) { }
		Cache(const Cache&) = delete;
		Cache& operator=(const Cache&) = delete;
		ResourceID LookupResource(std::uintptr_t dataIdentity) const;
		ResourceID Register(std::uintptr_t dataIdentity, const ResourceInfo& info);
		const ResourceInfo* Describe(ResourceID resource) const;
		// decode is synchronous and must not mutate this cache or call engine callbacks.
		Result GetOrDecode(ResourceID resource, unsigned frame,
			const std::function<Status(const Descriptor&, std::vector<std::uint8_t>&)>& decode);
		const FrameRecord* Resolve(FrameID frame) const;
		bool Invalidate(ResourceID resource);
		void Clear(); // Never resets process-wide ID sequences or cumulative counters.
		const Statistics& GetStats() const { return Stats; }
	private:
		struct Resource
		{
			std::uintptr_t Identity;
			ResourceInfo Info;
			std::unordered_map<unsigned, FrameRecord> Frames;
			std::unordered_set<unsigned> InvalidFrames;
		};
		Limits Budget;
		Statistics Stats;
		std::unordered_map<std::uintptr_t, ResourceID> Identities;
		std::unordered_map<std::uint64_t, Resource> Resources;
		std::unordered_map<std::uint64_t, const FrameRecord*> Frames;
	};
}
