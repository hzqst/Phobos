#include "AutomationCapture.h"
#include "AutomationFrameControl.h"
#include <Render/SurfaceMirror.h>
#include <Render/GPURender.h>
#include <chrono>

namespace Automation
{
	Json CaptureTick(AutomationDispatcher::Command& command)
	{
		if (!command.Image)
		{
			command.Image = std::make_shared<RenderCapture::Image>();
			auto& image = *command.Image;
			image.Scene = GameFrameHost().Scene();
			image.SimulationFrame = GameFrameHost().CompletedFrames();
			image.TimestampMS = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
			const auto& source = command.Arguments["source"];
			if (source == "gpu") GPURender::Capture(command.Image);
			else if (source == "native_terrain") SurfaceMirror::CaptureTerrainPixels(image);
			else SurfaceMirror::CapturePixels(image);
		}
		const auto& image = *command.Image;
		if (command.Arguments["source"] == "gpu") GPURender::PollCapture();
		if (!image.Error.empty()) return {{"automation_error", "capture_unavailable"}, {"reason", image.Error}};
		if (!image.Ready) return nullptr;
		return {{"capture_id", command.ID}};
	}
	Json CaptureCancel(AutomationDispatcher::Command& command)
	{
		if (command.Image) command.Image->Cancelled = true;
		return AutomationDispatcher::Failure("capture_cancelled");
	}
}
