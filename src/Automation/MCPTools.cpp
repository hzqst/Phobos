#include "MCPTools.h"

namespace Automation
{
	bool ValidateArguments(const Json& schema, const Json& value)
	{
		const auto type = schema.value("type", std::string());
		if (type == "object")
		{
			if (!value.is_object()) return false;
			if (schema.contains("required")) for (const auto& key : schema["required"])
				if (!value.contains(key.get<std::string>())) return false;
			const auto properties = schema.value("properties", Json::object());
			for (auto it = value.begin(); it != value.end(); ++it)
			{
				if (!properties.contains(it.key())) return false;
				if (!ValidateArguments(properties[it.key()], it.value())) return false;
			}
		}
		else if (type == "integer")
		{
			if (!value.is_number_integer()) return false;
			if (schema.contains("minimum") && value < schema["minimum"]) return false;
			if (schema.contains("maximum") && value > schema["maximum"]) return false;
		}
		else if (type == "string") { if (!value.is_string()) return false; }
		else if (type == "boolean") { if (!value.is_boolean()) return false; }
		else return false;
		if (schema.contains("enum"))
		{
			bool found = false;
			for (const auto& choice : schema["enum"]) found |= value == choice;
			if (!found) return false;
		}
		return true;
	}
	Json InputToolDefinitions()
	{
		const auto integer = [](int min, int max) { return Json{{"type", "integer"}, {"minimum", min}, {"maximum", max}}; };
		const auto choice = [](Json options) { return Json{{"type", "string"}, {"enum", std::move(options)}}; };
		const auto coord = integer(0, 16383), hold = integer(32, 2000);
		const auto button = choice({"left", "right", "middle"});
		Json keys = {"ESC", "ENTER", "SPACE", "UP", "DOWN", "LEFT", "RIGHT", "CTRL", "SHIFT", "ALT",
			"TAB", "BACKSPACE", "DELETE", "HOME", "END", "PAGEUP", "PAGEDOWN", "INSERT"};
		for (char c = 'A'; c <= 'Z'; ++c) keys.push_back(std::string(1, c));
		for (char c = '0'; c <= '9'; ++c) keys.push_back(std::string(1, c));
		for (int i = 1; i <= 12; ++i) keys.push_back("F" + std::to_string(i));
		Json tools = Json::array();
		const auto add = [&](const char* name, const char* description, Json props, Json required, bool readOnly = false)
		{
			tools.push_back({{"name", name}, {"description", description},
				{"inputSchema", {{"type", "object"}, {"properties", std::move(props)}, {"required", std::move(required)}, {"additionalProperties", false}}},
				{"outputSchema", {{"type", "object"}}},
				{"annotations", {{"readOnlyHint", readOnly}, {"idempotentHint", readOnly}, {"openWorldHint", false}}}});
		};
		add("game_status", "Get a fresh main-thread snapshot. Unavailable fields are null.", Json::object(), Json::array(), true);
		add("game_key", "Enqueue an engine key event without desktop input. Engine must be active; OS foreground is not required. Gameplay effect remains unconfirmed. Press leases expire after 10 seconds. Mutations require _meta com.phobos/instance_id and com.phobos/request_id.",
			{{"key", choice(keys)}, {"action", choice({"tap", "press", "release"})}, {"hold_ms", hold}}, {"key", "action"});
		add("game_mouse_move", "Move the engine cursor in original game logical coordinates without moving the desktop pointer.",
			{{"x", integer(-16383, 16383)}, {"y", integer(-16383, 16383)}, {"mode", choice({"absolute", "relative"})}}, {"x", "y", "mode"});
		add("game_mouse_click", "Move the engine cursor, enqueue press, wait for queue drain, then enqueue release. Gameplay effect remains unconfirmed.",
			{{"x", coord}, {"y", coord}, {"button", button}, {"click_count", integer(1, 2)}, {"hold_ms", hold}}, {"x", "y", "button"});
		add("game_mouse_drag", "Drag the engine cursor in logical coordinates over multiple message-loop iterations. Engine must remain active.",
			{{"start_x", coord}, {"start_y", coord}, {"end_x", coord}, {"end_y", coord}, {"button", button},
			 {"duration_ms", integer(64, 5000)}, {"steps", integer(2, 128)}}, {"start_x", "start_y", "end_x", "end_y", "button", "duration_ms", "steps"});
		add("game_mouse_wheel", "Inject wheel delta in Win32 units (120 per notch). Optional x and y must be provided together.",
			{{"delta", integer(-1200, 1200)}, {"x", coord}, {"y", coord}}, {"delta"});
		add("game_pause", "Pause active singleplayer simulation; wait for the paused main-loop barrier.", Json::object(), Json::array());
		add("game_step", "Advance exactly the requested completed simulation frames while paused. Returns actual progress on timeout.",
			{{"frames", integer(1, 10000)}, {"timeout_ms", integer(100, 28000)}}, {"frames"});
		add("game_resume", "Clear pending steps and resume singleplayer simulation; wait for a completed logic frame.", Json::object(), Json::array());
		return tools;
	}
}
