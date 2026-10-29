#include <Automation/AutomationFrameControl.h>
#include <cassert>
#include <iostream>
using namespace Automation;
using namespace std::chrono_literals;

struct FakeFrames final : FrameHost
{
	bool ready = true, paused = false;
	uint64_t scene = 1, frames = 100, barriers = 0;
	size_t pending = 0;
	bool Available() const override { return ready; }
	uint64_t Scene() const override { return scene; }
	uint64_t CompletedFrames() const override { return frames; }
	uint64_t PauseBarriers() const override { return barriers; }
	bool Paused() const override { return paused; }
	void SetPaused(bool value) override { paused = value; }
	size_t Pending() const override { return pending; }
	void SetPending(size_t value) override { pending = value; }
};

int __cdecl main()
{
	AutomationDispatcher dispatcher("test");
	AutomationFrameControl control;
	FakeFrames host;
	auto pause = dispatcher.Submit("game_pause", Json::object(), 10s);
	assert(control.Tick(*pause, host, 0).is_null());
	assert(host.paused);
	assert(control.Tick(*pause, host, 1).is_null()); // A flag is not an acknowledgement.
	++host.barriers;
	assert(control.Tick(*pause, host, 2)["completed"] == true);
	auto step = dispatcher.Submit("game_step", {{"frames", 5}, {"timeout_ms", 100}}, 10s);
	assert(control.Tick(*step, host, 3).is_null());
	auto overlap = dispatcher.Submit("game_step", {{"frames", 1}}, 10s);
	assert(control.Tick(*overlap, host, 4)["automation_error"] == "frame_control_busy");
	assert(5 == host.pending);
	host.frames += 5; host.pending = 0;
	assert(control.Tick(*step, host, 5).is_null()); // Wait for the stopped loop as well.
	++host.barriers;
	auto result = control.Tick(*step, host, 6);
	assert(result["completed"] == true && result["actual_frames"] == 5);
	auto timeout = dispatcher.Submit("game_step", {{"frames", 5}, {"timeout_ms", 100}}, 10s);
	assert(control.Tick(*timeout, host, 10).is_null());
	host.frames += 2; host.pending -= 2;
	result = control.Tick(*timeout, host, 110);
	assert(result["automation_error"] == "frame_control_timeout" && result["actual_frames"] == 2);
	assert(host.paused && !host.pending);
	auto pending = dispatcher.Submit("game_step", {{"frames", 5}}, 10s);
	assert(control.Tick(*pending, host, 120).is_null());
	auto resume = dispatcher.Submit("game_resume", Json::object(), 10s);
	assert(control.Tick(*resume, host, 121).is_null());
	assert(!host.paused && !host.pending);
	assert(control.Tick(*pending, host, 122)["automation_error"] == "frame_control_superseded");
	assert(control.Tick(*resume, host, 123).is_null());
	++host.frames;
	assert(control.Tick(*resume, host, 124)["completed"] == true);
	auto pause2 = dispatcher.Submit("game_pause", Json::object(), 10s);
	assert(control.Tick(*pause2, host, 130).is_null());
	control.Cancel(*pause2, host);
	assert(!host.paused);
	host.ready = false;
	auto unavailable = dispatcher.Submit("game_pause", Json::object(), 10s);
	assert(control.Tick(*unavailable, host, 140).contains("automation_error"));
	host.ready = true; host.paused = true;
	auto scene = dispatcher.Submit("game_step", {{"frames", 5}}, 10s);
	assert(control.Tick(*scene, host, 150).is_null());
	++host.scene;
	assert(control.Tick(*scene, host, 151)["automation_error"] == "scene_changed");
	assert(!host.pending);
	control.Stop(host);
	std::cout << "Frame control tests passed\n";
}
