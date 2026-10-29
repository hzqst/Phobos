#include "CaptureEncoding.h"
#include "MCPProtocol.h"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace Automation
{
	namespace
	{
		std::string Base64(const std::vector<uint8_t>& bytes)
		{
			constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string result; result.reserve((bytes.size() + 2) / 3 * 4);
			for (size_t i = 0; i < bytes.size(); i += 3)
			{
				const unsigned value = (unsigned(bytes[i]) << 16) | (i + 1 < bytes.size() ? unsigned(bytes[i+1]) << 8 : 0)
					| (i + 2 < bytes.size() ? bytes[i+2] : 0);
				result += alphabet[value >> 18]; result += alphabet[(value >> 12) & 63];
				result += i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=';
				result += i + 2 < bytes.size() ? alphabet[value & 63] : '=';
			}
			return result;
		}
		bool PNG(const std::vector<uint8_t>& rgba, int width, int height, std::vector<uint8_t>& bytes)
		{
			using Microsoft::WRL::ComPtr;
			ComPtr<IWICImagingFactory> factory;
			ComPtr<IWICBitmapEncoder> encoder;
			ComPtr<IWICBitmapFrameEncode> frame;
			ComPtr<IStream> stream;
			if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
				|| FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))
				|| FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
				|| FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))
				|| FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))
				|| FAILED(frame->SetSize(width, height))) return false;
			// The Windows PNG encoder accepts BGRA on Windows 7 and later.
			auto bgra = rgba;
			for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
			WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
			if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA
				|| FAILED(frame->WritePixels(height, width * 4, UINT(bgra.size()), bgra.data()))
				|| FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;
			STATSTG stat {};
			if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > 12u * 1024u * 1024u) return false;
			bytes.resize(size_t(stat.cbSize.QuadPart));
			LARGE_INTEGER start {}; ULONG read = 0;
			return SUCCEEDED(stream->Seek(start, STREAM_SEEK_SET, nullptr))
				&& SUCCEEDED(stream->Read(bytes.data(), ULONG(bytes.size()), &read)) && read == bytes.size();
		}
	}
	Json EncodeScreenshot(const RenderCapture::Image& image, const Json& args, Json metadata)
	{
		const auto fail = [](const char* error) { return MCPProtocol::ToolResult({{"automation_error", error}}, true); };
		if (!image.Ready || !RenderCapture::ValidSize(image.Width, image.Height)
			|| image.Pixels.size() != size_t(image.Width) * image.Height * 4) return fail("invalid_capture_pixels");
		int x = 0, y = 0, width = image.Width, height = image.Height;
		if (args.contains("crop"))
		{
			const auto& crop = args["crop"];
			x = crop["x"]; y = crop["y"]; width = crop["width"]; height = crop["height"];
			if (x < 0 || y < 0 || width <= 0 || height <= 0 || int64_t(x) + width > image.Width || int64_t(y) + height > image.Height)
				return fail("crop_outside_source_image");
		}
		int outWidth = width, outHeight = height;
		const int edge = args.value("max_edge", 1280);
		if (edge > 0 && std::max(width, height) > edge)
		{
			outWidth = std::max(1, int(int64_t(width) * edge / std::max(width, height)));
			outHeight = std::max(1, int(int64_t(height) * edge / std::max(width, height)));
		}
		std::vector<uint8_t> pixels(size_t(outWidth) * outHeight * 4);
		for (int row = 0; row < outHeight; ++row)
			for (int col = 0; col < outWidth; ++col)
			{
				const size_t source = (size_t(y + int64_t(row) * height / outHeight) * image.Width + x + int64_t(col) * width / outWidth) * 4;
				std::memcpy(pixels.data() + (size_t(row) * outWidth + col) * 4, image.Pixels.data() + source, 4);
			}
		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (FAILED(com) && com != RPC_E_CHANGED_MODE) return fail("png_com_initialization_failed");
		std::vector<uint8_t> png;
		const bool encoded = PNG(pixels, outWidth, outHeight, png);
		if (SUCCEEDED(com)) CoUninitialize();
		if (!encoded) return fail("png_encoding_failed_or_size_limit");
		metadata["source"] = image.Source;
		metadata["valid_capture"] = true;
		metadata["width"] = outWidth; metadata["height"] = outHeight;
		if (args.value("include_metadata", true))
		{
			metadata["original_width"] = image.Width; metadata["original_height"] = image.Height;
			metadata["game_width"] = image.LogicalWidth; metadata["game_height"] = image.LogicalHeight;
			metadata["viewport"] = image.Viewport;
			metadata["letterbox"] = {image.Viewport[0], image.Viewport[1], image.Width - image.Viewport[0] - image.Viewport[2], image.Height - image.Viewport[1] - image.Viewport[3]};
			metadata["timestamp_unix_ms"] = image.TimestampMS; metadata["simulation_frame"] = image.SimulationFrame;
			metadata["simulation_frame_semantics"] = "completed_logic_counter_at_capture";
			metadata["timestamp_semantics"] = image.Source == "gpu" ? "draw_recorded" : "surface_copy_started";
			metadata["gpu_present_id"] = image.Source == "gpu" ? Json(image.PresentID) : Json();
			metadata["gpu_present_id_semantics"] = image.Source == "gpu" ? Json("successful_swapchain_submission") : Json();
			metadata["scene_id"] = image.Scene; metadata["backend"] = image.Backend; metadata["output_mode"] = image.OutputMode;
			metadata["cursor"] = image.Source == "ddraw" || image.OutputMode == "mirror" ? "surface_contents_if_drawn" : "rendered_gpu_cursor_if_visible";
			metadata["crop"] = {{"x", x}, {"y", y}, {"width", width}, {"height", height}};
			metadata["pixel_format"] = "RGBA8"; metadata["file_path"] = nullptr;
		}
		auto result = MCPProtocol::ToolResult(std::move(metadata), false);
		result["content"].push_back({{"type", "image"}, {"mimeType", "image/png"}, {"data", Base64(png)}});
		return result;
	}
}
