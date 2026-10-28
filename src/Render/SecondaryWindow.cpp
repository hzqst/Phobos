#include "SecondaryWindow.h"

#include <Unsorted.h>

#include <Utilities/Debug.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

#include <Windows.h>

SDL_Window* SecondaryWindow::Window = nullptr;
int SecondaryWindow::RequestedWidth = 0;
int SecondaryWindow::RequestedHeight = 0;
bool SecondaryWindow::ResizedByUser = false;

namespace
{
	constexpr int GameWindowGap = 64;
	RECT LastGameRect {};
	bool PositionNeedsUpdate = false;

	bool FitsInWorkArea(const RECT& rect)
	{
		const HMONITOR hMonitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONULL);
		MONITORINFO info { sizeof(info) };

		if (!hMonitor || !GetMonitorInfoW(hMonitor, &info))
			return false;

		const RECT& work = info.rcWork;
		return rect.left >= work.left && rect.top >= work.top && rect.right <= work.right && rect.bottom <= work.bottom;
	}

	bool FindOtherMonitorWorkArea(HMONITOR hExcluded, RECT& work)
	{
		struct Search
		{
			HMONITOR hExcluded;
			RECT* pWork;
			bool Found;
		} search { hExcluded, &work, false };

		EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) -> BOOL
		{
			auto& search = *reinterpret_cast<Search*>(lParam);
			MONITORINFO info { sizeof(info) };

			if (hMonitor == search.hExcluded || !GetMonitorInfoW(hMonitor, &info))
				return TRUE;

			*search.pWork = info.rcWork;
			search.Found = true;
			return FALSE;
		}, reinterpret_cast<LPARAM>(&search));

		return search.Found;
	}
}

bool SecondaryWindow::Create(const char* pTitle, int width, int height)
{
	int x = 0;
	int y = 0;
	GetInitialPosition(width, height, x, y);

	const SDL_PropertiesID properties = SDL_CreateProperties();
	SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, pTitle);
	SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
	SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
	SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
	SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
	SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
	// WS_EX_NOACTIVATE: clicking the window must not take the keyboard focus from the game
	SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, false);

	Window = SDL_CreateWindowWithProperties(properties);
	SDL_DestroyProperties(properties);

	if (!Window)
		return false;

	RequestedWidth = width;
	RequestedHeight = height;
	ResizedByUser = false;
	PositionNeedsUpdate = true;

	Debug::Log("[GPURender] Created the window with %dx%d at (%d, %d).\n", width, height, x, y);
	return true;
}

void SecondaryWindow::Destroy()
{
	if (Window)
	{
		SDL_DestroyWindow(Window);
		Window = nullptr;
	}
}

SDL_Window* SecondaryWindow::Get()
{
	return Window;
}

void SecondaryWindow::Hide()
{
	if (Window)
		SDL_HideWindow(Window);
}

void SecondaryWindow::OnGameResolutionChanged(int width, int height)
{
	PositionNeedsUpdate = true;
	if (!Window || ResizedByUser || (width == RequestedWidth && height == RequestedHeight))
		return;

	RequestedWidth = width;
	RequestedHeight = height;
	SDL_SetWindowSize(Window, width, height);

	Debug::Log("[GPURender] Resized the window to the new game resolution %dx%d.\n", width, height);
}

void SecondaryWindow::UpdatePosition()
{
	RECT game {};
	if (!Window || !Game::hWnd || IsIconic(Game::hWnd) || !GetWindowRect(Game::hWnd, &game))
		return;
	// The wrapper may resize/recenter the game after Set_Video_Mode returns.
	// Check from the message loop, and only move again when the game rectangle changes.
	if (!PositionNeedsUpdate && EqualRect(&game, &LastGameRect))
		return;
	LastGameRect = game;
	PositionNeedsUpdate = false;
	int width = 0, height = 0, x = 0, y = 0;
	if (!SDL_GetWindowSize(Window, &width, &height))
		return;
	GetInitialPosition(width, height, x, y);
	if (SDL_SetWindowPosition(Window, x, y))
		Debug::Log("[GPURender] Requested window position (%d, %d), game rect=(%ld,%ld)-(%ld,%ld).\n",
			x, y, game.left, game.top, game.right, game.bottom);
}

SecondaryWindow::EventResult SecondaryWindow::HandleEvent(const SDL_Event& event)
{
	if (!Window || event.type < SDL_EVENT_WINDOW_FIRST || event.type > SDL_EVENT_WINDOW_LAST
		|| event.window.windowID != SDL_GetWindowID(Window))
	{
		return EventResult::None;
	}

	switch (event.type)
	{
	case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
		return EventResult::CloseRequested;

	case SDL_EVENT_WINDOW_RESIZED:
		// Our own SDL_SetWindowSize reports the requested size back
		if (event.window.data1 != RequestedWidth || event.window.data2 != RequestedHeight)
			ResizedByUser = true;

		return EventResult::NeedsPresent;

	case SDL_EVENT_WINDOW_EXPOSED:
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		return EventResult::NeedsPresent;

	default:
		return EventResult::None;
	}
}

// Prefer the game's right side with a gap, or another monitor if it does not fit.
// On a single monitor, allow overflow rather than moving left over the game.
// SDL positions and sizes the client area, the frame comes on top of it.
void SecondaryWindow::GetInitialPosition(int width, int height, int& x, int& y)
{
	RECT frame { 0, 0, width, height };
	AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);

	const int outerWidth = frame.right - frame.left;
	const int outerHeight = frame.bottom - frame.top;

	const auto setOuterPosition = [&](int outerX, int outerY)
	{
		x = outerX - frame.left;
		y = outerY - frame.top;
	};

	RECT game {};

	if (!Game::hWnd || !GetWindowRect(Game::hWnd, &game))
	{
		x = SDL_WINDOWPOS_CENTERED;
		y = SDL_WINDOWPOS_CENTERED;
		return;
	}

	const RECT beside { game.right + GameWindowGap, game.top,
		game.right + GameWindowGap + outerWidth, game.top + outerHeight };

	if (FitsInWorkArea(beside))
		return setOuterPosition(beside.left, beside.top);

	const HMONITOR hGameMonitor = MonitorFromWindow(Game::hWnd, MONITOR_DEFAULTTONEAREST);
	RECT work {};

	if (FindOtherMonitorWorkArea(hGameMonitor, work))
		return setOuterPosition(work.left, work.top);

	setOuterPosition(beside.left, beside.top);
}
