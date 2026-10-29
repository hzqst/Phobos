#include <Automation/AutomationInput.h>
#include <Automation/EngineInputQueue.h>
#include <Windows.h>
#include <cassert>
#include <iostream>
#include <set>
#include <vector>

using namespace Automation;
struct FakeKeyboard
{
	int Head = 0, Tail = 0;
	uint16_t Buffer[256] {};
	bool Suppress = false;
	bool Put_Element(uint16_t word)
	{
		const int next = (Tail + 1) % 256;
		if (next == Head) return false;
		Buffer[Tail] = word; Tail = next; return true;
	}
	bool Put_Key_Message(uint16_t key, bool release) { return Suppress || Put_Element(key | (release ? 0x800 : 0)); }
};
struct FakeHost : InputHost
{
	bool Focus = true, RejectMove = false, QueueEmpty = true;
	int X = 10, Y = 20, WheelDelta = 0;
	std::set<int> Down;
	std::vector<std::pair<int, bool>> Events;
	bool Available() const override { return Focus; }
	bool Position(int& x, int& y) const override { x = X; y = Y; return true; }
	bool Move(int x, int y) override { if (RejectMove) return false; X = x; Y = y; return true; }
	bool Key(int key, bool down) override { Events.emplace_back(key, down); if (down) Down.insert(key); else Down.erase(key); return true; }
	bool Button(int button, bool down) override { return Key(button, down); }
	bool Wheel(int delta) override { WheelDelta += delta; return true; }
	bool IsDown(int key) const override { return Down.contains(key); }
	bool Drained() const override { return QueueEmpty; }
};

int __cdecl main()
{
	FakeKeyboard keyboard;
	keyboard.Head = 2; keyboard.Tail = 254;
	assert(EnqueueEngineInput(keyboard, 1, false, true, 718, 220));
	assert(1 == keyboard.Tail && 1 == keyboard.Buffer[254] && 718 == keyboard.Buffer[255] && 220 == keyboard.Buffer[0]);
	assert(!EnqueueEngineInput(keyboard, 1, true, true, 718, 220) && 1 == keyboard.Tail);
	keyboard.Head = 4;
	assert(!EnqueueEngineInput(keyboard, 1, true, true, 718, 220) && 1 == keyboard.Tail);
	keyboard.Head = keyboard.Tail;
	keyboard.Suppress = true;
	assert(!EnqueueEngineInput(keyboard, 1, false, true, 20, 30) && 1 == keyboard.Tail);
	keyboard.Suppress = false;
	assert(EnqueueEngineInput(keyboard, 1, true, true, 20, 30) && 0x801 == keyboard.Buffer[1]);
	keyboard.Head = -1;
	assert(!EnqueueEngineInput(keyboard, 13, false, false, 0, 0));
	FakeHost host;
	AutomationInput input;
	uint64_t id = 0, now = 0;
	const auto command = [&](std::string name, Json args)
	{
		auto c = std::make_shared<AutomationDispatcher::Command>();
		c->ID = ++id; c->Operation = std::move(name); c->Arguments = std::move(args); return c;
	};
	const auto tick = [&](const auto& c) { now += 32; return input.Tick(*c, host, 800, 600, now); };
	auto tap = command("game_key", {{"key", "ENTER"}, {"action", "tap"}, {"hold_ms", 64}});
	assert(tick(tap).is_null());
	assert(host.Down.contains(VK_RETURN));
	assert(tick(tap).is_null() && host.Down.contains(VK_RETURN));
	assert(tick(tap).is_null() && !host.Down.contains(VK_RETURN));
	assert(tick(tap)["injected"] == true);
	assert(host.Events.size() == 2);
	auto physical = command("game_key", {{"key", "SHIFT"}, {"action", "press"}});
	host.Down.insert(VK_SHIFT);
	assert(tick(physical).contains("automation_error") && host.Down.contains(VK_SHIFT));
	host.Down.clear();
	auto press = command("game_key", {{"key", "CTRL"}, {"action", "press"}});
	assert(tick(press)["injected"] == true && host.Down.contains(VK_CONTROL));
	input.Maintain(host, now + 10001);
	assert(!host.Down.contains(VK_CONTROL));
	auto click = command("game_mouse_click", {{"x", 100}, {"y", 200}, {"button", "left"}});
	assert(tick(click).is_null() && !host.Down.contains(VK_LBUTTON));
	assert(tick(click).is_null() && host.Down.contains(VK_LBUTTON));
	auto competing = command("game_mouse_move", {{"x", 20}, {"y", 20}, {"mode", "absolute"}});
	assert(tick(competing)["automation_error"] == "input_busy");
	assert(host.Down.contains(VK_LBUTTON));
	input.Cancel(*click, host);
	assert(!host.Down.contains(VK_LBUTTON));
	auto drag = command("game_mouse_drag", {{"start_x", 10}, {"start_y", 20}, {"end_x", 210}, {"end_y", 120}, {"button", "left"}, {"duration_ms", 128}, {"steps", 4}});
	Json result;
	for (int n = 0; n < 20; ++n) { result = tick(drag); if (!result.is_null()) break; }
	assert(result["injected"] == true && 210 == host.X && 120 == host.Y && host.Down.empty());
	auto interrupted = command("game_key", {{"key", "SPACE"}, {"action", "tap"}});
	assert(tick(interrupted).is_null());
	host.Focus = false;
	input.Maintain(host, now);
	assert(host.Down.empty());
	assert(tick(interrupted)["automation_error"] == "engine_input_unavailable");
	host.Focus = true;
	assert(tick(interrupted)["automation_error"] == "input_interrupted");
	auto bounds = command("game_mouse_move", {{"x", 800}, {"y", 0}, {"mode", "absolute"}});
	assert(tick(bounds)["automation_error"] == "coordinates_out_of_bounds");
	auto wheel = command("game_mouse_wheel", {{"delta", -120}});
	for (int n = 0; n < 5; ++n) { result = tick(wheel); if (!result.is_null()) break; }
	assert(result["injected"] == true && -120 == host.WheelDelta);
	auto resized = command("game_key", {{"key", "A"}, {"action", "tap"}});
	assert(tick(resized).is_null());
	assert(input.Tick(*resized, host, 1024, 768, now + 32)["automation_error"] == "resolution_changed");
	assert(host.Down.empty());
	auto slow = command("game_key", {{"key", "ENTER"}, {"action", "tap"}, {"hold_ms", 32}});
	assert(tick(slow).is_null());
	host.QueueEmpty = false;
	for (int i = 0; i < 10; ++i) assert(tick(slow).is_null() && host.Down.contains(VK_RETURN));
	host.QueueEmpty = true;
	assert(tick(slow).is_null() && !host.Down.contains(VK_RETURN));
	host.QueueEmpty = false;
	assert(tick(slow).is_null());
	host.QueueEmpty = true;
	result = tick(slow);
	assert(result["queue_drained"] == true && result["affects_desktop_input"] == false && result["consumed"].is_null());
	std::cout << "Input timing, ownership, cancellation, focus, lease, bounds and resize tests passed\n";
}
