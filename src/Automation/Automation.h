#pragma once

namespace Automation
{
	void Enable();
	bool RequestedAtStartup();
	// Called on the game thread after surfaces exist, independent of GPU enablement.
	void Start();
	void Stop();
}
