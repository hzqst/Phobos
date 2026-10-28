#include <Render/ButtonImage.h>
#include <cstdio>

int main()
{
	// Expected truth table from 69DEDD..69DF0F; bits: disabled, useFlash, flashing, on, pressed.
	constexpr int expected[32] = {
		0,2,0,2,0,2,3,2,0,2,1,2,0,2,4,2,
		1,2,0,2,1,2,3,2,1,2,1,2,1,2,4,2};
	for (int bits = 0; bits < 32; ++bits)
		if (expected[bits] != ButtonImage::SelectFrame(bits & 1, bits & 2, bits & 4, bits & 8, bits & 16))
		{
			std::fprintf(stderr, "Frame priority failed: state=%d\n", bits);
			return 1;
		}
	std::puts("ButtonImage: all 32 state combinations passed.");
}
