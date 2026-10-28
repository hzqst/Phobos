#pragma once
#include "FrameCache.h"
#include <array>
#include <memory>
#include <vector>

namespace PaletteCache
{
	using PaletteID = FrameCache::ID<struct PaletteTag>;
	struct Data
	{
		std::array<std::uint16_t, 256> Colors {};
		std::array<std::uint32_t, 3> Shift {}, Bits {};
		bool operator==(const Data&) const = default;
	};
	struct Record { PaletteID ID; Data Values; };
	using Handle = std::shared_ptr<const Record>;
	inline FrameCache::IDSequence IDs;
	class Cache
	{
	public:
		explicit Cache(size_t limit = 256) : Limit(limit) { }
		Handle Acquire(const Data& data)
		{
			for (size_t i = 0; i < 3; ++i)
				if (!data.Bits[i] || data.Bits[i] > 8 || data.Shift[i] > 15 || data.Shift[i] + data.Bits[i] > 16) return {};
			for (auto& entry : Entries)
				if (entry.Color->Values == data) { entry.LastUse = ++Clock; return entry.Color; }
			if (Entries.size() >= Limit)
			{
				auto victim = Entries.end();
				for (auto it = Entries.begin(); it != Entries.end(); ++it)
					if (it->Color.use_count() == 1 && (victim == Entries.end() || it->LastUse < victim->LastUse)) victim = it;
				if (victim == Entries.end()) return {};
				Entries.erase(victim);
			}
			const PaletteID id {IDs.Next()};
			if (!id) return {};
			auto record = std::make_shared<const Record>(Record {id, data});
			Entries.push_back({record, ++Clock});
			return record;
		}
	private:
		struct Entry { Handle Color; std::uint64_t LastUse; };
		size_t Limit;
		std::uint64_t Clock = 0;
		std::vector<Entry> Entries;
	};
}
