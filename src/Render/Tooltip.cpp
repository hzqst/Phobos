#include "Tooltip.h"
#include "SidebarSnapshot.h"
#include "GPURender.h"
#include <GameOptionsClass.h>
#include <Surface.h>
#include <Drawing.h>
#include <Syringe.h>

namespace Tooltip
{
	namespace
	{
		using CursorSnapshot::Readable;
		constexpr unsigned MaxText = 1024, MaxCommands = 2048, MaxNesting = 4, MaxDraws = 8;
		constexpr int MaxCoordinate = 16384;
		// Native ESP offsets from the gamemd-reverse #11 scope contract. T is the
		// Draw2 entry ESP; the Blit entry B is T-BCh and DrawText's entry is B+54h.
		constexpr std::uintptr_t FillScope = 0x44, FillText = 0x20, EndScope = 0x38;
		constexpr std::uintptr_t GlyphParent = 0x54, GlyphTarget = 0x5C, GlyphScope = 0xBC;
		constexpr unsigned DrawTextReturn = 0x479041;
		struct Scope
		{
			std::uintptr_t Stack;
			const void* Surface = nullptr;
			bool Filled = false;
			Draw Value;
		};
		std::vector<Scope> Scopes;
		std::vector<Draw> Pending;
		std::array<std::vector<Draw>, 3> Current;
		unsigned TraceCount = 0;
		std::uint64_t LastTraceHash = 0;
		bool BlurLogged = false;

		bool Capturing()
		{
			const auto state = GPURender::GetState();
			return state == GPURender::State::Running || state == GPURender::State::Hidden;
		}

		bool Coordinate(int value) { return value >= -MaxCoordinate && value <= MaxCoordinate; }

		void Push(Draw& draw, const SidebarText::Command& command)
		{
			if (draw.Commands.size() >= MaxCommands) { draw.Known = false; return; }
			draw.Commands.push_back(command);
		}

		void Solid(Draw& draw, const Edge& edge, unsigned color, unsigned opacity)
		{
			if (edge.Width <= 0 || edge.Height <= 0) return;
			SidebarText::Command command;
			command.X = edge.X; command.Y = edge.Y; command.Width = edge.Width; command.Height = edge.Height;
			command.ClipRight = command.ClipBottom = INT_MAX;
			command.Color = color; command.Opacity256 = opacity;
			SidebarText::ColorFormat(command);
			Push(draw, command);
		}

		void TraceDraws()
		{
			if (!SidebarSnapshot::Trace || TraceCount >= 64) return;
			std::uint64_t hash = 0;
			for (const auto& draws : Current)
				for (const auto& draw : draws)
				{
					const unsigned state[] {unsigned(draw.Surface), unsigned(draw.OriginX), unsigned(draw.Commands.size()), draw.Blur};
					hash = SidebarSnapshot::Hash(state, sizeof(state), hash);
					if (!draw.Commands.empty())
						hash = SidebarSnapshot::Hash(&draw.Commands.front(), 4 * sizeof(int), hash);
					hash = SidebarSnapshot::Hash(draw.Text.data(), draw.Text.size() * sizeof(wchar_t), hash);
				}
			if (hash == LastTraceHash) return;
			LastTraceHash = hash;
			const auto& sidebar = Current[size_t(Target::Sidebar)];
			const auto& composite = Current[size_t(Target::Composite)];
			Debug::Log("[TooltipGPU] sample=%u sidebar=%zu composite=%zu\n", ++TraceCount, sidebar.size(), composite.size());
			for (const auto* draws : {&sidebar, &composite})
				for (const auto& draw : *draws)
				{
					const auto& rect = draw.Commands.front();
					std::string units;
					for (const auto unit : draw.Text)
					{
						char hex[6]; std::snprintf(hex, sizeof(hex), "%04X ", unsigned(unit)); units += hex;
					}
					Debug::Log("[TooltipGPU] target=%d origin=%d surface=%dx%d rect=%d,%d,%d,%d commands=%zu blur=%d utf16=%s\n",
						int(draw.Surface), draw.OriginX, draw.Width, draw.Height, rect.X, rect.Y, rect.Width, rect.Height,
						draw.Commands.size(), draw.Blur, units.c_str());
				}
		}
	}

	void BeginDraw(REGISTERS* r)
	{
		if (!Capturing()) return;
		if (Scopes.size() >= MaxNesting) { Scopes.clear(); return; }
		// The wrapped DrawText checks the bound font, as the sidebar items do.
		SidebarText::BindFont();
		Scopes.push_back({r->ESP()});
	}

	void ObserveFill(REGISTERS* r, const RectangleStruct* rect, const ColorStruct& color, int opacity, bool blur)
	{
		if (Scopes.empty() || Scopes.back().Stack != r->ESP() + FillScope) return;
		auto& scope = Scopes.back();
		auto& draw = scope.Value;
		auto* surface = reinterpret_cast<DSurface*>(r->ESI());
		const auto target = surface == DSurface::Composite ? Target::Composite
			: surface == DSurface::Sidebar ? Target::Sidebar : Target::None;
		if (scope.Filled || target == Target::None || !Readable(rect, sizeof(*rect)) || !Coordinate(rect->X) || !Coordinate(rect->Y)
			|| rect->Width < 0 || rect->Height < 0 || rect->Width > MaxCoordinate || rect->Height > MaxCoordinate)
			{ scope.Filled = true; draw.Known = false; return; }
		scope.Filled = true; scope.Surface = surface;
		draw.Surface = target;
		draw.Width = surface->GetWidth(); draw.Height = surface->GetHeight();
		const auto& view = DSurface::ViewBounds;
		const auto& sidebar = DSurface::SidebarBounds;
		draw.OriginX = CanvasOrigin(target, GameOptionsClass::Instance.SidebarMode,
			view.X + view.Width, sidebar.X + sidebar.Width);
		// Draw2 keeps the final text pointer, after Phobos' 478EE1 substitution.
		const auto* text = r->Stack<const wchar_t*>(FillText);
		for (unsigned i = 0; i < MaxText && Readable(text + i, sizeof(wchar_t)) && text[i]; ++i)
			draw.Text.push_back(text[i]);
		const Edge area {rect->X, rect->Y, rect->Width, rect->Height};
		Solid(draw, area, unsigned(Drawing::RGB_To_Int(color)), Opacity256(opacity));
		// EDI holds the packed ToolTipTextColor that DrawRect then uses for the outline.
		for (const auto& edge : Border(rect->X, rect->Y, rect->Width, rect->Height))
			Solid(draw, edge, r->EDI(), 256);
		draw.Blur = blur;
		if (blur && !BlurLogged)
		{
			BlurLogged = true;
			Debug::Log("[TooltipGPU] ToolTipBlur is not supported; the GPU tooltip background is drawn without blur.\n");
		}
	}

	bool ObserveGlyph(REGISTERS* r)
	{
		if (Scopes.empty()) return false;
		auto& scope = Scopes.back();
		const auto direct = r->Stack<unsigned>(0);
		if (r->ESP() + GlyphScope != scope.Stack || (direct != 0x434EA6 && direct != 0x4350E1 && direct != 0x4352BA)
			|| r->Stack<unsigned>(GlyphParent) != DrawTextReturn) return false;
		auto& draw = scope.Value;
		if (!scope.Filled || r->Stack<const void*>(GlyphTarget) != scope.Surface) { draw.Known = false; return true; }
		SidebarText::Command command;
		const auto result = SidebarText::CaptureGlyph(r, command);
		if (result == SidebarText::GlyphResult::Failed) draw.Known = false;
		else if (result == SidebarText::GlyphResult::Captured) Push(draw, command);
		return true;
	}

	void EndDraw(REGISTERS* r)
	{
		const auto stack = r->ESP() + EndScope;
		// Discard scopes abandoned by deeper frames; they never reached this epilogue.
		while (!Scopes.empty() && Scopes.back().Stack < stack) Scopes.pop_back();
		if (Scopes.empty() || Scopes.back().Stack != stack) return;
		auto scope = std::move(Scopes.back()); Scopes.pop_back();
		// Early returns skipped the fill: this call drew nothing.
		if (!scope.Filled) return;
		if (!scope.Value.Known)
		{
			if (SidebarSnapshot::Trace && TraceCount < 64)
				Debug::Log("[TooltipGPU] sample=%u unknown draw target=%d commands=%zu\n", ++TraceCount,
					int(scope.Value.Surface), scope.Value.Commands.size());
			return;
		}
		if (Pending.size() < MaxDraws) Pending.push_back(std::move(scope.Value));
	}

	void PublishSidebar()
	{
		auto& sidebar = Current[size_t(Target::Sidebar)];
		sidebar.clear();
		std::erase_if(Pending, [&sidebar](Draw& draw)
		{
			if (draw.Surface != Target::Sidebar) return false;
			sidebar.push_back(std::move(draw));
			return true;
		});
	}

	void PublishTopLevel()
	{
		// Sidebar draws belong to the preceding sidebar capture only.
		auto& composite = Current[size_t(Target::Composite)];
		composite.clear();
		for (auto& draw : Pending)
			if (draw.Surface == Target::Composite) composite.push_back(std::move(draw));
		Pending.clear(); Scopes.clear();
		TraceDraws();
	}

	void Clear()
	{
		Scopes.clear(); Pending.clear();
		for (auto& draws : Current) draws.clear();
	}

	const std::vector<Draw>& Published(Target target) { return Current[size_t(target)]; }
}
