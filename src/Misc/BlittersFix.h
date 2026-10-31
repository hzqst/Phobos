#pragma once

class BlittersFix
{
public:
	static void Apply();

	// The RLE 25/50/75% AlphaZRead blitters were patched to mix with per-channel rounding.
	static bool Applied;
};
