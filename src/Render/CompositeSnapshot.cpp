#include "CompositeSnapshot.h"

#include <Surface.h>
#include <TacticalClass.h>

#include <cstdlib>
#include <cstring>

// The single definition of the copies CompositeSnapshot.h declares.
namespace CompositeSnapshot
{
	Copy Input, Output;
	unsigned Failed = 0;

	namespace
	{
		const char* Fill(Copy& copy, DSurface* surface)
		{
			if (!surface) return "composite_target_missing";
			// 6D4639 makes the target Temp until 6D4930; a top-level render's target is Composite,
			// whose pointer scrolling swaps with Tile's, so both are read every time.
			if (*reinterpret_cast<DSurface* const*>(TargetSlot) != surface) return "composite_target_not_temp";
			if (DSurface::Composite != surface) return "composite_target_not_composite";
			const TacticalClass* tactical = TacticalClass::Instance;
			if (!tactical) return "composite_tactical_missing";
			// Already locked by 6D45F2; the copy neither locks nor keeps the buffer.
			if (surface->LockLevel <= 0) return "composite_not_locked";
			if (surface->BytesPerPixel != NativeBytesPerPixel) return "composite_not_16_bit";
			const auto* buffer = static_cast<const std::uint8_t*>(surface->Buffer);
			if (!buffer) return "composite_buffer_missing";
			const int width = surface->GetWidth(), height = surface->GetHeight(), pitch = surface->GetPitch();
			const auto& view = DSurface::ViewBounds;
			if (view.Width <= 0 || view.Height <= 0 || view.X < 0 || view.Y < 0
				|| std::int64_t(view.X) + view.Width > width || std::int64_t(view.Y) + view.Height > height) return "composite_view_outside_surface";
			if (std::abs(std::int64_t(pitch)) < std::int64_t(width) * NativeBytesPerPixel) return "composite_invalid_pitch";
			copy.Pixels.resize(size_t(view.Width) * view.Height);
			for (int y = 0; y < view.Height; ++y)
				std::memcpy(copy.Pixels.data() + size_t(y) * view.Width,
					buffer + std::ptrdiff_t(view.Y + y) * pitch + std::ptrdiff_t(view.X) * NativeBytesPerPixel, size_t(view.Width) * NativeBytesPerPixel);
			copy.Bounds = view;
			copy.Surface = reinterpret_cast<std::uintptr_t>(surface);
			copy.SurfaceWidth = width;
			copy.SurfaceHeight = height;
			copy.TacticalX = tactical->TacticalPos.X;
			copy.TacticalY = tactical->TacticalPos.Y;
			copy.Frame = Unsorted::CurrentFrame;
			return nullptr;
		}
	}

	void Record(Copy& copy, DSurface* surface)
	{
		copy.Valid = false;
		if (const auto* error = Fill(copy, surface))
		{
			copy.Error = error;
			++Failed;
			return;
		}
		copy.Error = "";
		copy.Valid = true;
	}

	void Invalidate()
	{
		Input.Valid = Output.Valid = false;
		Input.Error = Output.Error = "composite_not_copied";
	}
}
