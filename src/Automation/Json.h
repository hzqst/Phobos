#pragma once

// Parsing external input must return an error, never throw through a game hook.
#define JSON_NOEXCEPTION 1
#include "../../lib/Automation/json.hpp"

namespace Automation
{
	using Json = nlohmann::json;
}
