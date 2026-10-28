#include <Render/FrameCache.h>
#include <Render/ShapeImage.h>
#include <cstdio>
#include <limits>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (false)

int main()
{
	using namespace FrameCache;
	Cache cache;
	const Descriptor a {0, 0, 2, 1, 0, 56, 2};
	const Descriptor b {1, -1, 2, 1, 2, 58, 6};
	const ResourceInfo info {2, 1, {a, b}};
	unsigned calls = 0;
	auto decode = [&](const Descriptor& d, std::vector<std::uint8_t>& pixels) {
		++calls;
		const std::uint8_t plain[] {1, 0};
		const std::uint8_t rle[] {5, 0, 1, 0, 1};
		return ShapeImage::Decode(d.Flags & 2 ? std::span<const std::uint8_t>(rle) : std::span<const std::uint8_t>(plain),
			d.Width, d.Height, (d.Flags & 2) != 0, pixels) ? Status::Ready : Status::InvalidData;
	};
	const auto resource = cache.Register(0x1000, info);
	CHECK(resource);
	CHECK(resource == cache.LookupResource(0x1000)); // aliases share live Data identity
	const auto first = cache.GetOrDecode(resource, 0, decode);
	CHECK(Status::Ready == first.State && first.Frame);
	const FrameID firstID = first.Frame->ID;
	const auto second = cache.GetOrDecode(resource, 1, decode);
	CHECK(Status::Ready == second.State && second.Frame->ID != firstID);
	CHECK(first.Frame->Indices == second.Frame->Indices);
	CHECK(firstID == cache.GetOrDecode(resource, 0, decode).Frame->ID);
	for (int i = 0; i < 300; ++i) CHECK(firstID == cache.GetOrDecode(resource, 0, decode).Frame->ID);
	CHECK(2u == calls && 2u == cache.GetStats().Decoded);
	CHECK(4u == cache.GetStats().IndexBytes);
	CHECK(Status::InvalidData == cache.GetOrDecode(resource, 2, decode).State);
	CHECK(2u == calls);
	CHECK(cache.Invalidate(resource));
	CHECK(!cache.Resolve(firstID) && !cache.LookupResource(0x1000));
	CHECK(0u == cache.GetStats().IndexBytes);
	const auto reused = cache.Register(0x1000, info);
	CHECK(reused && reused != resource);
	CHECK(firstID != cache.GetOrDecode(reused, 0, decode).Frame->ID);
	cache.Clear();
	CHECK(!cache.Resolve(firstID) && !cache.LookupResource(0x1000));
	CHECK(reused != cache.Register(0x1000, info));

	Cache small({2, 4, 2}); // bytes, descriptor slots, resource count
	const auto limited = small.Register(0x2000, info);
	CHECK(limited);
	CHECK(Status::Ready == small.GetOrDecode(limited, 0, decode).State);
	const auto beforeBudget = calls;
	CHECK(Status::BudgetExceeded == small.GetOrDecode(limited, 1, decode).State);
	CHECK(beforeBudget == calls && 2u == small.GetStats().IndexBytes);
	CHECK(!small.Register(0x3000, {2,1,{a,b,a}}));
	CHECK(small.Invalidate(limited));
	CHECK(small.Register(0x3000, info)); // descriptor and byte budgets returned

	Cache failures;
	const auto bad = failures.Register(0x4000, info);
	unsigned failedCalls = 0;
	auto invalid = [&](const Descriptor&, std::vector<std::uint8_t>& out) {
		++failedCalls; out = {9}; return Status::InvalidData;
	};
	CHECK(Status::InvalidData == failures.GetOrDecode(bad, 0, invalid).State);
	CHECK(Status::InvalidData == failures.GetOrDecode(bad, 0, invalid).State);
	CHECK(1u == failedCalls && 0u == failures.GetStats().IndexBytes);
	auto notReady = [&](const Descriptor&, std::vector<std::uint8_t>&) { return Status::NotReady; };
	CHECK(Status::NotReady == failures.GetOrDecode(bad, 1, notReady).State);
	CHECK(Status::Ready == failures.GetOrDecode(bad, 1, decode).State);
	CHECK(failures.Invalidate(bad));
	const auto recovered = failures.Register(0x4000, info);
	CHECK(Status::Ready == failures.GetOrDecode(recovered, 0, decode).State);
	CHECK(Status::NotReady == failures.GetOrDecode(bad, 0, decode).State);

	IDSequence sequence(std::numeric_limits<std::uint64_t>::max());
	CHECK(std::numeric_limits<std::uint64_t>::max() == sequence.Next());
	CHECK(0u == sequence.Next() && 0u == sequence.Next());
	std::puts("FrameCache: identities, A/B/A and 300 hits, address reuse, budgets, failure retry and ID exhaustion passed.");
}
