#pragma once

struct SDL_Window;
union SDL_Event;

// The window next to the game window that the GPU render presents to. It never takes the
// focus away from the game, and follows the game resolution until the user resizes it.
class SecondaryWindow
{
public:
	enum class EventResult
	{
		None,
		CloseRequested,
		NeedsPresent,  // resized or exposed
	};

	static bool Create(const char* pTitle, int width, int height);
	static void Destroy();
	static SDL_Window* Get();

	static void Hide();
	static void OnGameResolutionChanged(int width, int height);
	static EventResult HandleEvent(const SDL_Event& event);

private:
	static void GetInitialPosition(int width, int height, int& x, int& y);

	static SDL_Window* Window;
	static int RequestedWidth;
	static int RequestedHeight;
	static bool ResizedByUser;
};
