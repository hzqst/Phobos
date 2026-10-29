#pragma once
#include "Json.h"

namespace Automation
{
	Json InputToolDefinitions();
	bool ValidateArguments(const Json& schema, const Json& arguments);
}
