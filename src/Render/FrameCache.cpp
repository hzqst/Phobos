#include "FrameCache.h"
#include "ShapeImage.h"
#include <limits>
#include <utility>

namespace FrameCache
{
	namespace
	{
		IDSequence ResourceIDs;
		IDSequence FrameIDs;
	}

	std::uint64_t IDSequence::Next()
	{
		const auto result = NextValue;
		if (NextValue) NextValue = NextValue == std::numeric_limits<std::uint64_t>::max() ? 0 : NextValue + 1;
		return result;
	}

	ResourceID Cache::LookupResource(std::uintptr_t identity) const
	{
		const auto found = Identities.find(identity);
		return found == Identities.end() ? ResourceID {} : found->second;
	}

	ResourceID Cache::Register(std::uintptr_t identity, const ResourceInfo& info)
	{
		if (!identity || info.Frames.empty()) return {};
		if (const auto id = LookupResource(identity)) return id;
		if (Resources.size() >= Budget.Resources || info.Frames.size() > Budget.FrameSlots - Stats.FrameSlots)
		{
			++Stats.BudgetRejected;
			return {};
		}
		const ResourceID id {ResourceIDs.Next()};
		if (!id) return {};
		Resources.emplace(id.Value, Resource {identity, info, {}, {}});
		Identities.emplace(identity, id);
		Stats.FrameSlots += info.Frames.size();
		Stats.Resources = Resources.size();
		return id;
	}

	const ResourceInfo* Cache::Describe(ResourceID id) const
	{
		const auto found = Resources.find(id.Value);
		return found == Resources.end() ? nullptr : &found->second.Info;
	}

	Result Cache::GetOrDecode(ResourceID id, unsigned index,
		const std::function<Status(const Descriptor&, std::vector<std::uint8_t>&)>& decode)
	{
		auto found = Resources.find(id.Value);
		if (found == Resources.end()) return {Status::NotReady};
		auto& resource = found->second;
		if (index >= resource.Info.Frames.size()) return {Status::InvalidData};
		if (auto frame = resource.Frames.find(index); frame != resource.Frames.end())
		{
			++Stats.Hits;
			return {Status::Ready, &frame->second};
		}
		if (resource.InvalidFrames.contains(index)) return {Status::InvalidData};
		const auto& d = resource.Info.Frames[index];
		if (d.Width <= 0 || d.Height <= 0 || d.Width > ShapeImage::MaxDimension || d.Height > ShapeImage::MaxDimension)
		{
			resource.InvalidFrames.insert(index);
			++Stats.Invalid;
			return {Status::InvalidData};
		}
		const auto bytes = std::size_t(d.Width) * d.Height;
		if (bytes > Budget.IndexBytes - Stats.IndexBytes)
		{
			++Stats.BudgetRejected;
			return {Status::BudgetExceeded};
		}
		++Stats.Misses;
		++Stats.DecodeAttempts;
		std::vector<std::uint8_t> indices;
		auto state = decode(d, indices);
		if (state == Status::Ready && indices.size() != bytes) state = Status::InvalidData;
		if (state != Status::Ready)
		{
			if (state == Status::InvalidData)
			{
				resource.InvalidFrames.insert(index);
				++Stats.Invalid;
			}
			return {state};
		}
		const FrameID frameID {FrameIDs.Next()};
		if (!frameID) return {Status::IDExhausted};
		auto [frame, inserted] = resource.Frames.emplace(index, FrameRecord {frameID, id, index, d, std::move(indices)});
		Frames.emplace(frameID.Value, &frame->second);
		Stats.IndexBytes += bytes;
		Stats.Frames = Frames.size();
		++Stats.Decoded;
		return {Status::Ready, &frame->second};
	}

	const FrameRecord* Cache::Resolve(FrameID id) const
	{
		const auto found = Frames.find(id.Value);
		return found == Frames.end() ? nullptr : found->second;
	}

	bool Cache::Invalidate(ResourceID id)
	{
		const auto found = Resources.find(id.Value);
		if (found == Resources.end()) return false;
		for (const auto& [index, frame] : found->second.Frames)
		{
			Stats.IndexBytes -= frame.Indices.size();
			Frames.erase(frame.ID.Value);
		}
		Stats.FrameSlots -= found->second.Info.Frames.size();
		Identities.erase(found->second.Identity);
		Resources.erase(found);
		Stats.Resources = Resources.size();
		Stats.Frames = Frames.size();
		++Stats.Invalidations;
		return true;
	}

	void Cache::Clear()
	{
		while (!Resources.empty()) Invalidate(ResourceID {Resources.begin()->first});
	}
}
