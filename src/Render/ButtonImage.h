#pragma once

namespace ButtonImage
{
	// Native ShapeButton::Draw priority. Input and flash AI remain engine-owned.
	constexpr int SelectFrame(bool disabled, bool useFlash, bool flashing, bool on, bool pressed)
	{
		return disabled ? 2 : useFlash ? (flashing ? 3 : 0) + int(on) : int(pressed);
	}
}
