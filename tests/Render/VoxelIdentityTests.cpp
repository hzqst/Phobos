#include "Render/VoxelIdentity.h"
#include <cassert>
#include <cstdio>

int main()
{
	using namespace VoxelIdentity;
	Tracker tracker;
	Component body;
	body.Payload = 123; body.Blitter = 456; body.Parameters[0] = 32;
	Key key, other;
	tracker.BeginFrame(1);
	tracker.Begin(100, 200, 1);
	assert(!tracker.Candidate(200, {0, 0, 32, 32}, 1, key));
	tracker.Add(200, body, 1);
	assert(tracker.Candidate(200, {0, 0, 32, 32}, 1, key));
	assert(tracker.Candidate(200, {0, 16, 32, 16}, 1, other));
	assert(!(key == other)); // Split draws retain the context and distinguish source regions.
	tracker.End(100);
	tracker.Begin(101, 200, 1);
	tracker.Add(200, body, 1);
	assert(tracker.Candidate(200, {0, 0, 32, 32}, 1, other) && key == other);
	tracker.Unknown(201);
	assert(tracker.Candidate(200, {0, 0, 32, 32}, 1, other));
	tracker.Unknown(200);
	assert(!tracker.Candidate(200, {0, 0, 32, 32}, 1, other));
	tracker.End(101);
	tracker.Begin(102, 200, 1); tracker.Add(200, body, 1);
	tracker.Begin(103, 200, 1); tracker.Add(200, body, 1);
	assert(!tracker.Candidate(200, {0, 0, 32, 32}, 1, other));
	tracker.End(103);
	assert(!tracker.Candidate(200, {0, 0, 32, 32}, 1, other));
	tracker.End(102);
	tracker.Begin(104, 200, 1); tracker.Add(200, body, 1);
	assert(!tracker.Candidate(200, {0, 0, 32, 32}, 2, other));
	tracker.End(104);
	tracker.Begin(105, 200, 1);
	for (size_t i = 0; i <= MaxParts; ++i) tracker.Add(200, body, 1);
	assert(!tracker.Candidate(200, {0, 0, 32, 32}, 1, other));
	tracker.End(105);
	tracker.Begin(108, 200, 1); tracker.Add(200, body, 1);
	auto turret = body; turret.Payload = 789;
	tracker.Add(200, turret, 1);
	assert(tracker.Candidate(200, {0, 0, 32, 32}, 1, other));
	const auto bodyThenTurret = other;
	tracker.End(108);
	tracker.Begin(109, 200, 1); tracker.Add(200, turret, 1); tracker.Add(200, body, 1);
	assert(tracker.Candidate(200, {0, 0, 32, 32}, 1, other));
	assert(!(bodyThenTurret == other));
	tracker.End(109);
	for (size_t i = 0; i <= MaxDepth; ++i) tracker.Begin(200 + i, 200, 1);
	assert(!tracker.Candidate(200, {0, 0, 32, 32}, 1, other));
	tracker.BeginFrame(1);
	tracker.Begin(106, 200, 1); tracker.Add(200, body, 1); tracker.End(999);
	tracker.Begin(107, 200, 1); tracker.Add(200, body, 1);
	assert(!tracker.Candidate(200, {0, 0, 32, 32}, 1, other));

	FrameCache::Cache frames;
	auto make = [&](std::uintptr_t identity, std::uint8_t value)
	{
		FrameCache::ResourceInfo info {2, 2, {{0, 0, 2, 2}}};
		auto resource = frames.Register(identity, info);
		auto result = frames.GetOrDecode(resource, 0, [=](const auto&, auto& pixels)
			{ pixels.assign(4, value); return FrameCache::Status::Ready; });
		assert(result.Frame);
		return result.Frame->ID;
	};
	auto first = make(1, 7), equal = make(2, 7), different = make(3, 8);
	Validator validator;
	assert(!validator.Lookup(key, frames));
	assert(Result::Seeded == validator.Observe(key, first, frames));
	assert(!validator.Lookup(key, frames)); // A baseline alone cannot authorize reuse.
	assert(Result::Equal == validator.Observe(key, equal, frames));
	assert(first == validator.Lookup(key, frames));
	auto translated = key; translated.Translation[7] = 8;
	assert(!validator.Lookup(translated, frames));
	assert(Result::Mismatch == validator.Observe(key, different, frames));
	assert(!validator.Lookup(key, frames));
	assert(Result::Disabled == validator.Observe(bodyThenTurret, first, frames));
	key.Epoch = 2;
	assert(Result::Seeded == validator.Observe(key, first, frames));
	frames.Clear();
	assert(!validator.Lookup(key, frames));
	assert(Result::Missing == validator.Observe(key, first, frames));
	first = make(4, 7);
	assert(Result::Seeded == validator.Observe(key, first, frames));
	for (size_t i = 0; i <= MaxEntries; ++i)
	{
		key.Region[0] = int(i);
		validator.Observe(key, first, frames);
	}
	assert(validator.Size() <= MaxEntries);

	ScratchGuard guard;
	assert(guard.NeedsSeed(10, 20));
	guard.Begin(10, 20, true);
	assert(!guard.CanReuse());
	guard.Write({20, 30, 40, 50});
	assert(guard.CanReuse());
	guard.End(true);
	assert(!guard.NeedsSeed(10, 20));
	assert(guard.NeedsSeed(10, 21)); // Same surface, replaced buffer.
	guard.Begin(10, 20, true); guard.Write({20, 30, 40, 50}); guard.End(false);
	assert(guard.NeedsSeed(10, 20));
	guard.Begin(10, 20, false); guard.Write({20, 30, 40, 50});
	assert(!guard.CanReuse());
	guard.End(true);
	guard.Begin(10, 20, true); guard.Write({20, 30, 40, 50});
	guard.Unknown(10);
	assert(!guard.CanReuse()); guard.End(true);
	guard.Begin(10, 20, true); guard.Write({20, 30, 40, 50});
	guard.Begin(10, 20, true); guard.Write({20, 30, 40, 50});
	assert(!guard.CanReuse()); guard.End(true);
	assert(!guard.CanReuse()); guard.End(true);
	guard.Begin(10, 20, true); guard.Write({250, 0, 20, 20});
	assert(!guard.CanReuse()); guard.End(true);
	guard.Begin(10, 20, true); guard.Write({0, 0, 20, 20}); guard.Write({0, 0, 20, 20});
	assert(!guard.CanReuse()); guard.End(true); // First fast path accepts one component only.
	guard.BeginFrame(); assert(guard.NeedsSeed(10, 20));
	std::puts("VoxelIdentityTests passed");
}
