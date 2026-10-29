#include <Automation/CaptureEncoding.h>
#include <cassert>
#include <fstream>
#include <iostream>
using namespace Automation;
int __cdecl main()
{
	std::vector<uint8_t> rgba;
	assert(RenderCapture::Decode16({0xF800, 0x07E0, 0x001F}, {11,5,0}, {5,6,5}, rgba));
	assert((rgba == std::vector<uint8_t>{255,0,0,255, 0,255,0,255, 0,0,255,255}));
	assert(RenderCapture::Decode16({0x7C00, 0x03E0, 0x001F}, {10,5,0}, {5,5,5}, rgba));
	assert((rgba == std::vector<uint8_t>{255,0,0,255, 0,255,0,255, 0,0,255,255}));
	assert(!RenderCapture::Decode16({0}, {10,5,0}, {8,5,5}, rgba));
	assert(!RenderCapture::Decode16({0}, {5,5,0}, {5,5,5}, rgba));
	assert(!RenderCapture::ValidSize(0, 10) && !RenderCapture::ValidSize(4096,4096));
	RenderCapture::Image image;
	image.Width = image.LogicalWidth = 3; image.Height = image.LogicalHeight = 1;
	image.Pixels = rgba; image.Ready = true; image.Source = "ddraw";
	const auto full = EncodeScreenshot(image, {{"max_edge", 0}}, {{"capture_id", 1}});
	assert(full["isError"] == false && full["content"][1]["type"] == "image");
	const auto crop = EncodeScreenshot(image, {{"crop", {{"x",1},{"y",0},{"width",2},{"height",1}}}, {"max_edge", 1}}, Json::object());
	assert(crop["isError"] == false && crop["structuredContent"]["width"] == 1);
	assert(EncodeScreenshot(image, {{"crop", {{"x",2},{"y",0},{"width",2},{"height",1}}}}, Json::object())["isError"] == true);
	image.Source = "gpu"; image.OutputMode = "mirror";
	assert(EncodeScreenshot(image, Json::object(), Json::object())["structuredContent"]["cursor"] == "surface_contents_if_drawn");
	const auto minimal = EncodeScreenshot(image, {{"include_metadata", false}}, {{"capture_id", 2}});
	assert(minimal["structuredContent"]["capture_id"] == 2 && !minimal["structuredContent"].contains("viewport"));
	assert(full["structuredContent"]["native_terrain"].is_null());
	image.Source = "native_terrain"; image.OutputMode = "original";
	image.Terrain = RenderCapture::SurfaceRegion {0x1234, 856, 768, {0, 16, 856, 736}, 0, 16, 120, -45};
	const auto terrain = EncodeScreenshot(image, Json::object(), Json::object())["structuredContent"];
	assert(terrain["cursor"] == "not_included" && terrain["native_terrain"]["surface"] == "00001234");
	assert((terrain["native_terrain"]["view_bounds"] == Json{0, 16, 856, 736}));
	assert((terrain["native_terrain"]["placed_at"] == Json{0, 16} && terrain["native_terrain"]["tactical_position"] == Json{120, -45}));
	assert(terrain["native_terrain"]["surface_width"] == 856 && terrain["native_terrain"]["surface_height"] == 768);
	std::ofstream("Debug/AutomationTests/capture-fixtures.json") << Json{{"full",full},{"crop",crop}}.dump();
	std::cout << "Capture format, crop and PNG encoding tests passed\n";
}
