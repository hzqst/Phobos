#pragma once
#include <cstdint>

namespace Automation
{
	// The WWKeyboard ring has 256 slots and leaves one empty. A mouse event is
	// one key word followed by two coordinate words; never publish a partial event.
	template<typename Keyboard>
	bool EnqueueEngineInput(Keyboard& keyboard, uint16_t key, bool release, bool mouse, uint16_t x, uint16_t y)
	{
		if (keyboard.Head < 0 || keyboard.Head >= 256 || keyboard.Tail < 0 || keyboard.Tail >= 256) return false;
		const int free = (keyboard.Head - keyboard.Tail - 1 + 256) % 256;
		if (free < (mouse ? 3 : 1)) return false;
		const int tail = keyboard.Tail;
		keyboard.Put_Key_Message(key, release);
		if (keyboard.Tail == tail) return false; // The engine can suppress a key but return success.
		if (mouse)
		{
			keyboard.Put_Element(x);
			keyboard.Put_Element(y);
		}
		return true;
	}
}
