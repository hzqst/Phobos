#include <Render/BuildingImage.h>
#include <climits>
#include <cstdio>

namespace
{
	int Failures = 0;
	void Check(bool condition, const char* what)
	{
		if (!condition) { std::fprintf(stderr, "BuildingImage failed: %s\n", what); ++Failures; }
	}
}

int main()
{
	using namespace BuildingImage;
	Check(BodyFrame(3, 10) == 3 && BodyFrame(5, 10) == 5 && BodyFrame(6, 10) == 5 && BodyFrame(4, 9) == 4 && BodyFrame(-1, 10) == -1,
		"body frames stop at half the frames, with no lower bound");
	Check(BodyDepth(0, 0) == -2 && BodyDepth(-20, 15) == -37 && BibDepth(0) == -3 && BibDepth(30) == -33, "body and bib depths");
	Check(DoorDepth(0) == -2 && DoorDepth(-15) == 13 && ShadowDepth(0) == -4 && ShadowDepth(30) == -34, "door and shadow depths");
	Check(BodyDepth(INT_MIN, 1) == INT_MAX - 2, "depths wrap");

	Check(CornerLeptons(1) == 0 && CornerLeptons(3) == 512, "far corner in leptons");
	{
		const auto moved = ZShapeOffset(true, 10, -5, 60, 30);
		Check(moved.X == 148 && moved.Y == 411, "Z shape offset with the point move");
		const auto still = ZShapeOffset(false, 10, -5, 60, 30);
		Check(still.X == 138 && still.Y == 416, "Z shape offset without it");
	}

	// Four stages: opening 0..3, closing reversed, idle 0, open 3, damaged after 5 frames.
	Check(GateFrame(2, 4, false, false, false, false) == 2 && GateFrame(1, 4, true, false, false, false) == 3, "gate opening and closing");
	Check(GateFrame(0, 4, true, false, false, false) == 3 && GateFrame(4, 4, false, false, false, false) == 3, "gate frames clamp to the last");
	Check(GateFrame(3, 4, false, true, false, false) == 0 && GateFrame(0, 4, false, false, true, false) == 3, "idle and open gates");
	Check(GateFrame(2, 4, false, false, false, true) == 7 && GateFrame(-1, 4, false, false, false, false) == 0, "damaged gates, lower clamp");
	Check(DoorFrame(true) == 0 && DoorFrame(false) == 1, "door frames");

	if (Failures) return 1;
	std::puts("BuildingImage: frames, depths, Z shape offsets and gate frames passed.");
	return 0;
}
