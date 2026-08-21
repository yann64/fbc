/* Shared keyboard-scancode and mouse-button mapping helpers, used by both
 * the plain Haiku gfxlib2 driver (gfx_driver_haiku.cpp) and the OpenGL one
 * (gfx_driver_opengl_haiku.cpp) -- pure functions, no shared state, so a
 * header is simpler than exporting symbols across the two translation
 * units.
 */

#ifndef __FB_GFX_HAIKU_INPUT_H__
#define __FB_GFX_HAIKU_INPUT_H__

#include <InterfaceDefs.h>

extern "C" {
#include "../fb_gfx.h"
}

namespace fb_haiku {

/* Best-effort DOS-scancode mapping for the handful of non-printable keys
 * FB programs commonly check for (arrows, editing keys, ESC/ENTER/etc, and
 * F1-F12). Haiku's raw hardware key codes don't correspond to AT scancodes:
 * for most keys, the "raw_char" field carries a stable BeOS B_*_ARROW-style
 * constant regardless of keyboard layout, which is enough to map from; for
 * function keys, raw_char is just a generic marker, so the "key" field
 * (matched against the B_F1_KEY..B_F12_KEY raw hardware codes) plus the
 * B_FUNCTION_KEY modifiers bit are used instead.
 */
inline int32 KeyToScancode(int32 rawChar, int32 key, int32 modifiers)
{
	if (modifiers & B_FUNCTION_KEY) {
		switch (key) {
		case B_F1_KEY:  return SC_F1;
		case B_F2_KEY:  return SC_F2;
		case B_F3_KEY:  return SC_F3;
		case B_F4_KEY:  return SC_F4;
		case B_F5_KEY:  return SC_F5;
		case B_F6_KEY:  return SC_F6;
		case B_F7_KEY:  return SC_F7;
		case B_F8_KEY:  return SC_F8;
		case B_F9_KEY:  return SC_F9;
		case B_F10_KEY: return SC_F10;
		case B_F11_KEY: return SC_F11;
		case B_F12_KEY: return SC_F12;
		}
	}

	switch (rawChar) {
	case B_ESCAPE: return SC_ESCAPE;
	case B_BACKSPACE: return SC_BACKSPACE;
	case B_ENTER: return SC_ENTER;
	case B_TAB: return SC_TAB;
	case B_SPACE: return SC_SPACE;
	case B_LEFT_ARROW: return SC_LEFT;
	case B_RIGHT_ARROW: return SC_RIGHT;
	case B_UP_ARROW: return SC_UP;
	case B_DOWN_ARROW: return SC_DOWN;
	case B_INSERT: return SC_INSERT;
	case B_DELETE: return SC_DELETE;
	case B_HOME: return SC_HOME;
	case B_END: return SC_END;
	case B_PAGE_UP: return SC_PAGEUP;
	case B_PAGE_DOWN: return SC_PAGEDOWN;
	}
	return 0;
}

inline int HaikuButtonsToFbButtons(int32 haikuButtons)
{
	int buttons = 0;
	if (haikuButtons & B_PRIMARY_MOUSE_BUTTON) buttons |= BUTTON_LEFT;
	if (haikuButtons & B_SECONDARY_MOUSE_BUTTON) buttons |= BUTTON_RIGHT;
	if (haikuButtons & B_TERTIARY_MOUSE_BUTTON) buttons |= BUTTON_MIDDLE;
	return buttons;
}

} /* namespace fb_haiku */

#endif
