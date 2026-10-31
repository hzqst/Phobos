#include "BlitFrameSource.h"

// The single definition of the state BlitFrameSource.h declares.
namespace BlitFrameSource
{
	FrameCache::Cache Frames(Budget);
	std::unordered_multimap<std::uint32_t, Entry> ByContent;
	std::unordered_map<Address, Entry, AddressHash> ByAddress;
	std::unordered_map<std::uint64_t, unsigned> IdentityUses;
	unsigned Epoch = 0, Serial = 0;
	std::uintptr_t NextIdentity = 0;
}
