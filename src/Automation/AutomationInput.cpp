#include "AutomationInput.h"
#include <Windows.h>
#include <charconv>

namespace Automation
{
	namespace
	{
		int KeyCode(const std::string& name)
		{
			if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9'))) return name[0];
			if (name.starts_with('F'))
			{
				int number = 0;
				const auto parsed = std::from_chars(name.data() + 1, name.data() + name.size(), number);
				if (parsed.ec == std::errc() && parsed.ptr == name.data() + name.size() && number >= 1 && number <= 12) return VK_F1 + number - 1;
			}
			static const std::map<std::string, int> keys {
				{"ESC", VK_ESCAPE}, {"ENTER", VK_RETURN}, {"SPACE", VK_SPACE}, {"UP", VK_UP}, {"DOWN", VK_DOWN},
				{"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"CTRL", VK_CONTROL}, {"SHIFT", VK_SHIFT}, {"ALT", VK_MENU},
				{"TAB", VK_TAB}, {"BACKSPACE", VK_BACK}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME}, {"END", VK_END},
				{"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT}, {"INSERT", VK_INSERT}};
			const auto found = keys.find(name);
			return found == keys.end() ? 0 : found->second;
		}
		int ButtonCode(const std::string& name) { return name == "left" ? VK_LBUTTON : name == "right" ? VK_RBUTTON : VK_MBUTTON; }
	}
	bool AutomationInput::Release(InputHost& host, int key)
	{
		const auto held = HeldInputs.find(key);
		if (held == HeldInputs.end()) return true;
		if (!(held->second.Mouse ? host.Button(key, false) : host.Key(key, false))) return false;
		HeldInputs.erase(held);
		return true;
	}
	void AutomationInput::ReleaseAll(InputHost& host)
	{
		for (auto i = HeldInputs.begin(); i != HeldInputs.end();)
		{
			const auto key = (i++)->first;
			Release(host, key); // Failed releases stay tracked and are retried.
		}
		Active = {};
	}
	void AutomationInput::Maintain(InputHost& host, uint64_t now)
	{
		if (!host.Available()) { ReleaseAll(host); return; }
		for (auto i = HeldInputs.begin(); i != HeldInputs.end();)
		{
			const auto key = i->first;
			const bool expired = now >= i->second.Expires;
			++i;
			if (expired) Release(host, key);
		}
	}
	Json AutomationInput::Fail(InputHost& host, const char* reason)
	{
		const bool keyReleased = Release(host, Active.Key);
		const bool buttonReleased = Release(host, Active.Button);
		Active = {};
		return {{"automation_error", reason}, {"injected", false}, {"partial_input_possible", true}, {"consumed", nullptr}, {"release_confirmed", keyReleased && buttonReleased}};
	}
	Json AutomationInput::Done(InputHost& host)
	{
		int x = 0, y = 0;
		const bool position = host.Position(x, y);
		Active = {};
		return {{"injected", true}, {"consumed", nullptr}, {"queue_drained", host.Drained()}, {"backend", "engine_queue"}, {"input_route", host.Route()},
			{"affects_desktop_input", false}, {"x", position ? Json(x) : Json()}, {"y", position ? Json(y) : Json()}};
	}
	Json AutomationInput::Cancel(AutomationDispatcher::Command& command, InputHost& host)
	{
		if (Active.ID == command.ID) return Fail(host, "request_cancelled");
		return Json::object();
	}
	Json AutomationInput::Tick(AutomationDispatcher::Command& command, InputHost& host, int width, int height, uint64_t now)
	{
		if (!host.Available()) { ReleaseAll(host); return AutomationDispatcher::Failure("engine_input_unavailable"); }
		if (width <= 0 || height <= 0) return AutomationDispatcher::Failure("game_window_unavailable");
		if (Active.ID && Active.ID != command.ID) return AutomationDispatcher::Failure("input_busy");
		const auto& args = command.Arguments;
		const auto& name = command.Operation;
		const auto inside = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height; };
		if (!Active.ID)
		{
			if (!command.Progress.is_null()) return AutomationDispatcher::Failure("input_interrupted");
			command.Progress = {{"started", true}, {"width", width}, {"height", height}};
			Active.ID = command.ID;
			Active.Hold = args.value("hold_ms", 64);
			if (name == "game_key")
			{
				Active.Key = KeyCode(args["key"].get<std::string>());
				if (!Active.Key) return Fail(host, "unknown_key");
				const auto action = args["action"].get<std::string>();
				if (action == "release")
				{
					if (!HeldInputs.contains(Active.Key)) return Fail(host, "key_not_owned");
					if (!Release(host, Active.Key)) return Fail(host, "release_failed");
					Active.Stage = 2; Active.Due = now + MessageGapMS;
					return nullptr;
				}
				if (HeldInputs.contains(Active.Key) || host.IsDown(Active.Key))
				{
					Active.Key = 0; // Do not release a previously held or physical key.
					return Fail(host, "key_already_down");
				}
				if (!host.Key(Active.Key, true)) return Fail(host, "injection_failed");
				HeldInputs[Active.Key] = {now + LeaseMS, false};
				if (action == "press")
				{
					auto result = Done(host);
					result["lease_ms"] = LeaseMS;
					return result;
				}
				Active.Stage = 1; Active.Due = now + Active.Hold;
				return nullptr;
			}
			if (!host.Position(Active.X, Active.Y)) return Fail(host, "cursor_unavailable");
			if (name == "game_mouse_drag")
			{
				Active.X = args["start_x"]; Active.Y = args["start_y"];
				Active.StartX = Active.X; Active.StartY = Active.Y;
				Active.EndX = args["end_x"]; Active.EndY = args["end_y"];
				Active.Duration = args["duration_ms"]; Active.Steps = args["steps"];
				if (!inside(Active.EndX, Active.EndY) || Active.Duration / Active.Steps < 16) return Fail(host, "invalid_drag_bounds_or_interval");
			}
			else if (args.contains("x"))
			{
				if (!args.contains("y")) return Fail(host, "coordinates_must_be_paired");
				const bool relative = args.value("mode", std::string()) == "relative";
				Active.X = (relative ? Active.X : 0) + args["x"].get<int>();
				Active.Y = (relative ? Active.Y : 0) + args["y"].get<int>();
			}
			else if (args.contains("y")) return Fail(host, "coordinates_must_be_paired");
			if (!inside(Active.X, Active.Y)) return Fail(host, "coordinates_out_of_bounds");
			if (!host.Move(Active.X, Active.Y)) return Fail(host, "move_failed");
			if (name == "game_mouse_click" || name == "game_mouse_drag")
			{
				const int button = ButtonCode(args["button"].get<std::string>());
				if (host.IsDown(button) || HeldInputs.contains(button)) return Fail(host, "button_already_down");
				Active.Button = button;
				Active.Clicks = args.value("click_count", 1);
			}
			Active.Stage = 0; Active.Due = now + MessageGapMS;
			return nullptr;
		}
		if (command.Progress["width"] != width || command.Progress["height"] != height) return Fail(host, "resolution_changed");
		if (now < Active.Due) return nullptr;
		// Do not release a press until the engine has removed it from its queue.
		// Draining confirms transport consumption, not the intended gameplay effect.
		if (!host.Drained()) return nullptr;
		if (name == "game_key")
		{
			if (Active.Stage == 2) return Done(host);
			if (!Release(host, Active.Key)) return Fail(host, "release_failed");
			Active.Stage = 2; Active.Due = now + MessageGapMS; return nullptr;
		}
		if (name == "game_mouse_move") return Done(host);
		if (name == "game_mouse_wheel")
		{
			if (Active.Stage == 1) return Done(host);
			if (!host.Wheel(args["delta"].get<int>())) return Fail(host, "wheel_injection_failed");
			Active.Stage = 1; Active.Due = now + MessageGapMS; return nullptr;
		}
		if (Active.Stage == 0)
		{
			if (!host.Button(Active.Button, true)) return Fail(host, "button_injection_failed");
			HeldInputs[Active.Button] = {now + LeaseMS, true};
			Active.Stage = 1; Active.Due = now + (name == "game_mouse_drag" ? Active.Duration / Active.Steps : Active.Hold);
			return nullptr;
		}
		if (Active.Stage == 1)
		{
			if (name == "game_mouse_drag" && Active.Step < Active.Steps)
			{
				++Active.Step;
				const int x = Active.StartX + (Active.EndX - Active.StartX) * Active.Step / Active.Steps;
				const int y = Active.StartY + (Active.EndY - Active.StartY) * Active.Step / Active.Steps;
				if (!host.Move(x, y)) return Fail(host, "drag_move_failed");
				Active.Due = now + (Active.Step == Active.Steps ? MessageGapMS : Active.Duration / Active.Steps);
				return nullptr;
			}
			if (!Release(host, Active.Button)) return Fail(host, "button_release_failed");
			Active.Stage = 2; Active.Due = now + MessageGapMS; return nullptr;
		}
		if (--Active.Clicks > 0 && name == "game_mouse_click")
		{
			Active.Stage = 0; Active.Due = now + MessageGapMS; return nullptr;
		}
		return Done(host);
	}
}
