#pragma once

#include "CursorImage.h"

#include <ConvertClass.h>
#include <FileSystem.h>
#include <Unsorted.h>
#include <WWMouseClass.h>

#include <array>
#include <cstring>
#include <limits>

// Main-thread sampler. Never loads a resource or calls the engine renderer. Only
// copies the selected plain SHP frame while MouseMutex protects the mouse state.
namespace CursorSnapshot
{
	DEFINE_REFERENCE(HANDLE, MouseMutex, 0xB78168u)

	struct Frame
	{
		bool Visible = false;
		int X = 0, Y = 0, Width = 0, Height = 0;
		std::array<std::uint32_t, CursorImage::MaxDimension * CursorImage::MaxDimension> Pixels {};
	};

	inline bool Readable(const void* pointer, size_t bytes)
	{
		auto address = reinterpret_cast<std::uintptr_t>(pointer);
		if (!address || bytes > std::numeric_limits<std::uintptr_t>::max() - address)
			return false;
		const auto end = address + bytes;
		while (address < end)
		{
			MEMORY_BASIC_INFORMATION info {};
			if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))
				|| info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD)
				|| !(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
					| PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
				return false;
			const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
			if (next <= address)
				return false;
			address = next;
		}
		return true;
	}

	inline bool CopySelected(Frame& frame)
	{
		auto* mouse = WWMouseClass::Instance;
		if (!Game::IsFocused || !Readable(mouse, sizeof(*mouse)) || !mouse->field_10 || mouse->RefCount != 0)
			return false;

		const SHPStruct* image = mouse->Image;
		if (!Readable(image, sizeof(*image)))
			return false;
		if (image->IsReference())
		{
			const auto* reference = image->AsReference();
			if (!Readable(reference, sizeof(*reference)) || !reference->Loaded)
				return false;
			image = reference->Data;
		}
		if (!Readable(image, sizeof(*image)) || image->IsReference()
			|| image->Width <= 0 || image->Height <= 0 || image->Frames <= 0
			|| mouse->ImageFrameIndex < 0 || mouse->ImageFrameIndex >= image->Frames)
			return false;

		const size_t headersSize = sizeof(SHPStruct) + size_t(image->Frames) * sizeof(SHPFrame);
		if (!Readable(image, headersSize))
			return false;
		const auto& header = image->AsFile()->GetFrameHeader(mouse->ImageFrameIndex);
		if ((header.Flags & 2) || header.Width <= 0 || header.Height <= 0
			|| header.Width > CursorImage::MaxDimension || header.Height > CursorImage::MaxDimension
			|| header.Offset < 0 || size_t(header.Offset) < headersSize)
			return false;
		const size_t count = size_t(header.Width) * header.Height;
		const auto base = reinterpret_cast<std::uintptr_t>(image);
		if (size_t(header.Offset) > std::numeric_limits<std::uintptr_t>::max() - base)
			return false;
		const auto* pixels = reinterpret_cast<const byte*>(base + header.Offset);
		if (!Readable(pixels, count))
			return false;
		// A following frame gives an additional resource-local bound when available.
		for (int i = 0; i < image->Frames; ++i)
		{
			const auto offset = image->AsFile()->GetFrameHeader(i).Offset;
			if (offset > header.Offset && size_t(offset - header.Offset) < count)
				return false;
		}

		const auto* convert = FileSystem::MOUSE_PAL;
		if (!Readable(convert, sizeof(*convert)) || convert->BytesPerPixel != 2
			|| !Readable(convert->PaletteData, 256 * sizeof(std::uint16_t)))
			return false;
		const auto* palette = static_cast<const std::uint16_t*>(convert->PaletteData);
		const auto x = std::int64_t(mouse->field_14.X) - mouse->XY2.X + header.Left;
		const auto y = std::int64_t(mouse->field_14.Y) - mouse->XY2.Y + header.Top;
		if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX)
			return false;
		frame.X = int(x); frame.Y = int(y);
		frame.Width = header.Width; frame.Height = header.Height;
		for (size_t i = 0; i < count; ++i)
			frame.Pixels[i] = CursorImage::PackPixel(pixels[i], palette[pixels[i]]);
		return true;
	}

	inline void Capture(Frame& frame)
	{
		frame.Visible = false;
		const auto mutex = MouseMutex;
		if (!mutex)
			return;
		const DWORD result = WaitForSingleObject(mutex, 0);
		if (result == WAIT_OBJECT_0)
		{
			frame.Visible = CopySelected(frame);
			ReleaseMutex(mutex);
		}
		else if (result == WAIT_ABANDONED)
			ReleaseMutex(mutex); // Ownership acquired, but the protected state is untrusted.
	}
}
