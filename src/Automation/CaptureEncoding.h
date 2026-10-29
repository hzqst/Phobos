#pragma once
#include "Json.h"
#include <Render/CaptureImage.h>

namespace Automation
{
	// Only owned pixels enter this function; call on an HTTP worker, never the game thread.
	Json EncodeScreenshot(const RenderCapture::Image& image, const Json& arguments, Json metadata);
}
