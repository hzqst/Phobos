#pragma once
#include "AutomationDispatcher.h"
namespace Automation
{
	Json CaptureTick(AutomationDispatcher::Command& command);
	Json CaptureCancel(AutomationDispatcher::Command& command);
}
