/* Native Haiku (BeAPI) gfx driver.
 *
 * gfxlib2's core does all software rasterization (PSET/LINE/circle/etc.)
 * into a plain malloc'd buffer, __fb_gfx->framebuffer -- this driver's job
 * is just: open a window, get that buffer onto the screen, and feed
 * keyboard/mouse input back in as EVENTs. See fb_gfx.h's GFXDRIVER doc
 * comments for the full contract.
 *
 * Haiku's windowing API (BApplication/BWindow/BView) has no C bindings,
 * hence this is a .cpp file (see the LIBFBGFX_CXX bits in the top-level
 * makefile) linked against libbe/libstdc++, unlike the rest of gfxlib2.
 *
 * Scope: 32bpp truecolor and 8bpp indexed/palette SCREEN modes, no
 * fullscreen/multi-monitor handling, no OpenGL, and SetMouse() can show/hide
 * the cursor and track clip state but can't reposition the system cursor.
 * See CLAUDE.md for the full list of known gaps.
 */

#include <Application.h>
#include <AppDefs.h>
#include <Window.h>
#include <View.h>
#include <Bitmap.h>
#include <Screen.h>
#include <OS.h>
#include <InterfaceDefs.h>

extern "C" {
#include "../fb_gfx.h"
#include "fb_gfx_haiku.h"
}

#include <pthread.h>
#include <string.h>

namespace {

class FBHaikuApp;
class FBHaikuWindow;

struct HaikuDriverState {
	pthread_t app_thread;
	sem_id ready_sem;
	FBHaikuApp *app;
	FBHaikuWindow *window;
	BBitmap *bitmap;
	FBMUTEX *mutex;
	int w, h, depth;
	bool inited;
	/* NULL for depth==32 (the direct-memcpy fast path in driver_unlock() is
	 * used instead); for indexed depths, a ready-made gfxlib2 core blitter
	 * (see fb_hGetBlitter()/gfx_blitter.c) that converts __fb_gfx->framebuffer
	 * (indexed pixels) to 32bpp using __fb_gfx->device_palette, which the
	 * core already maintains -- the driver doesn't need its own palette
	 * storage at all, just to report success from driver_set_palette(). */
	BLITTER *blitter;
	/* Set by driver_exit() before it calls window->Quit(). BWindow::Quit()
	 * called from another thread posts B_QUIT_REQUESTED and goes through
	 * the QuitRequested() hook just like a user clicking the close box
	 * does -- this flag is how QuitRequested() tells the two apart, so a
	 * user close click can be left to the FB program's discretion (posts
	 * EVENT_WINDOW_CLOSE, doesn't actually close) while driver_exit()'s own
	 * programmatic shutdown still actually closes the window. */
	bool exiting;

	/* Mouse state, updated from FBHaikuView's input callbacks (which run on
	 * the window's own looper thread) and read back synchronously from
	 * driver_get_mouse() (GFXDRIVER's get_mouse is a poll, not event-based --
	 * see fb_GfxGetMouse()/gfx_getmouse.c, which calls it directly). */
	int mouse_x, mouse_y, mouse_z, mouse_buttons;
	bool mouse_clip;
};

HaikuDriverState g_state;

/* Best-effort DOS-scancode mapping for the handful of non-printable keys
 * FB programs commonly check for (arrows, editing keys, ESC/ENTER/etc, and
 * F1-F12). Haiku's raw hardware key codes don't correspond to AT scancodes:
 * for most keys, the "raw_char" field carries a stable BeOS B_*_ARROW-style
 * constant regardless of keyboard layout, which is enough to map from; for
 * function keys, raw_char is just a generic marker, so the "key" field
 * (matched against the B_F1_KEY..B_F12_KEY raw hardware codes) plus the
 * B_FUNCTION_KEY modifiers bit are used instead.
 */
int32 KeyToScancode(int32 rawChar, int32 key, int32 modifiers)
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

int HaikuButtonsToFbButtons(int32 haikuButtons)
{
	int buttons = 0;
	if (haikuButtons & B_PRIMARY_MOUSE_BUTTON) buttons |= BUTTON_LEFT;
	if (haikuButtons & B_SECONDARY_MOUSE_BUTTON) buttons |= BUTTON_RIGHT;
	if (haikuButtons & B_TERTIARY_MOUSE_BUTTON) buttons |= BUTTON_MIDDLE;
	return buttons;
}

class FBHaikuView : public BView {
public:
	FBHaikuView(BRect frame, BBitmap *bitmap)
		: BView(frame, "fbgfx_view", B_FOLLOW_ALL, B_WILL_DRAW),
		  fBitmap(bitmap)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	void Draw(BRect updateRect) override
	{
		if (fBitmap != NULL)
			DrawBitmap(fBitmap, updateRect, updateRect);
	}

	void MouseDown(BPoint where) override
	{
		(void)where;
		PostButtonTransitions(EVENT_MOUSE_BUTTON_PRESS);
	}

	void MouseUp(BPoint where) override
	{
		(void)where;
		PostButtonTransitions(EVENT_MOUSE_BUTTON_RELEASE);
	}

	void MouseMoved(BPoint where, uint32 code, const BMessage *dragMessage) override
	{
		(void)code; (void)dragMessage;
		PostMoveEvent(where);
	}

	void MessageReceived(BMessage *msg) override
	{
		if (msg->what == B_MOUSE_WHEEL_CHANGED) {
			float deltaY = 0;
			msg->FindFloat("be:wheel_delta_y", &deltaY);

			fb_MutexLock(g_state.mutex);
			g_state.mouse_z -= (int)deltaY;
			int z = g_state.mouse_z;
			fb_MutexUnlock(g_state.mutex);

			EVENT e;
			memset(&e, 0, sizeof(e));
			e.type = EVENT_MOUSE_WHEEL;
			e.z = z;
			fb_hPostEvent(&e);
		} else {
			BView::MessageReceived(msg);
		}
	}

private:
	/* Compares the new button bitmask (from the current message) against
	 * the previously-tracked one and posts one EVENT_MOUSE_BUTTON_PRESS/
	 * RELEASE per button that actually changed state -- matches the X11
	 * driver's convention of a single .button per event, not the full mask.
	 */
	void PostButtonTransitions(int type)
	{
		int32 haikuButtons = 0;
		if (Window() != NULL && Window()->CurrentMessage() != NULL)
			Window()->CurrentMessage()->FindInt32("buttons", &haikuButtons);
		int newButtons = HaikuButtonsToFbButtons(haikuButtons);

		fb_MutexLock(g_state.mutex);
		int oldButtons = g_state.mouse_buttons;
		g_state.mouse_buttons = newButtons;
		fb_MutexUnlock(g_state.mutex);

		int changed = oldButtons ^ newButtons;
		static const int all_buttons[] = { BUTTON_LEFT, BUTTON_RIGHT, BUTTON_MIDDLE };
		for (size_t i = 0; i < sizeof(all_buttons) / sizeof(all_buttons[0]); i++) {
			if (changed & all_buttons[i]) {
				EVENT e;
				memset(&e, 0, sizeof(e));
				e.type = type;
				e.button = all_buttons[i];
				fb_hPostEvent(&e);
			}
		}
	}

	void PostMoveEvent(BPoint where)
	{
		fb_MutexLock(g_state.mutex);
		int dx = (int)where.x - g_state.mouse_x;
		int dy = (int)where.y - g_state.mouse_y;
		g_state.mouse_x = (int)where.x;
		g_state.mouse_y = (int)where.y;
		fb_MutexUnlock(g_state.mutex);

		EVENT e;
		memset(&e, 0, sizeof(e));
		e.type = EVENT_MOUSE_MOVE;
		e.x = (int)where.x;
		e.y = (int)where.y;
		e.dx = dx;
		e.dy = dy;
		fb_hPostEvent(&e);
	}

	BBitmap *fBitmap;
};

class FBHaikuWindow : public BWindow {
public:
	FBHaikuWindow(BRect frame, const char *title, BBitmap *bitmap)
		: BWindow(frame, title, B_TITLED_WINDOW,
			B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS)
	{
		fView = new FBHaikuView(Bounds(), bitmap);
		AddChild(fView);
	}

	bool QuitRequested() override
	{
		if (g_state.exiting)
			return true; /* driver_exit()'s own programmatic shutdown */

		EVENT e;
		memset(&e, 0, sizeof(e));
		e.type = EVENT_WINDOW_CLOSE;
		fb_hPostEvent(&e);
		/* Leave it up to the FB program to decide whether/when to end --
		 * matches how other drivers surface EVENT_WINDOW_CLOSE. */
		return false;
	}

	void MessageReceived(BMessage *msg) override
	{
		switch (msg->what) {
		case B_KEY_DOWN:
		case B_KEY_UP:
		{
			int32 rawChar = 0, key = 0, modifiers = 0;
			msg->FindInt32("raw_char", &rawChar);
			msg->FindInt32("key", &key);
			msg->FindInt32("modifiers", &modifiers);
			const char *bytes = NULL;
			msg->FindString("bytes", &bytes);

			EVENT e;
			memset(&e, 0, sizeof(e));
			e.type = (msg->what == B_KEY_DOWN) ? EVENT_KEY_PRESS : EVENT_KEY_RELEASE;
			e.scancode = KeyToScancode(rawChar, key, modifiers);
			e.ascii = (bytes != NULL && bytes[0] != '\0' && (unsigned char)bytes[0] < 0x80)
				? (unsigned char)bytes[0] : 0;
			fb_hPostEvent(&e);
			break;
		}
		default:
			BWindow::MessageReceived(msg);
		}
	}

	FBHaikuView *View() const { return fView; }

private:
	FBHaikuView *fView;
};

class FBHaikuApp : public BApplication {
public:
	FBHaikuApp() : BApplication("application/x-vnd.fbc-gfx") {}
};

void *AppThreadEntry(void *)
{
	g_state.app = new FBHaikuApp();

	BRect frame(0, 0, g_state.w - 1, g_state.h - 1);
	g_state.bitmap = new BBitmap(frame, B_RGB32);
	g_state.window = new FBHaikuWindow(frame, __fb_window_title ? __fb_window_title : "FreeBASIC",
		g_state.bitmap);
	g_state.window->CenterOnScreen();
	g_state.window->Show();

	release_sem(g_state.ready_sem);

	g_state.app->Run();

	delete g_state.app;
	g_state.app = NULL;
	return NULL;
}

extern "C" int driver_init(char *title, int w, int h, int depth, int refresh_rate, int flags)
{
	(void)refresh_rate;

	if (flags & DRIVER_OPENGL)
		return -1;

	/* 8bpp indexed and 32bpp truecolor only -- 15/16/24bpp aren't handled
	 * (no BAS program-visible way to request them via ScreenRes anyway). */
	if (depth != 32 && depth != 8)
		return -1;

	memset(&g_state, 0, sizeof(g_state));
	g_state.w = w;
	g_state.h = h;
	g_state.depth = depth;
	if (depth != 32) {
		/* is_rgb=TRUE (the straight-copy blitter variant, fb_hBlit8to32RGB)
		 * confirmed empirically on a real Haiku box: a 4-color-band test
		 * with is_rgb=FALSE swapped red and blue (band 1 came out blue
		 * instead of red, band 3 red instead of blue, band 4 -- yellow --
		 * came out cyan); is_rgb=TRUE renders all four correctly. So
		 * __fb_gfx->device_palette's r | g<<8 | b<<16 packing already
		 * matches B_RGB32's needed byte order directly, same as the 32bpp
		 * truecolor path (which also needs no swap, just a plain memcpy). */
		g_state.blitter = fb_hGetBlitter(32, TRUE);
		if (g_state.blitter == NULL)
			return -1;
	}
	g_state.mutex = fb_MutexCreate();
	g_state.ready_sem = create_sem(0, "fbgfx haiku ready");
	if (g_state.ready_sem < 0)
		return -1;

	if (pthread_create(&g_state.app_thread, NULL, AppThreadEntry, NULL) != 0) {
		delete_sem(g_state.ready_sem);
		return -1;
	}

	acquire_sem(g_state.ready_sem);
	delete_sem(g_state.ready_sem);
	g_state.ready_sem = -1;

	g_state.inited = true;
	return 0;
}

extern "C" void driver_exit(void)
{
	if (!g_state.inited)
		return;

	/* Must be set before Quit() is called: BWindow::Quit() from a thread
	 * other than the window's own looper thread (this always is one, see
	 * HaikuDriverState::exiting's comment) posts B_QUIT_REQUESTED
	 * asynchronously and returns immediately, so QuitRequested() only runs
	 * later, on the app thread -- but always after this flag is visibly
	 * set, since nothing else touches it concurrently. */
	g_state.exiting = true;

	if (g_state.window != NULL && g_state.window->Lock())
		g_state.window->Quit();

	/* BApplication::Run() doesn't return just because the last window
	 * closed -- it keeps pumping messages until the application itself is
	 * asked to quit. */
	if (g_state.app != NULL)
		g_state.app->PostMessage(B_QUIT_REQUESTED);

	pthread_join(g_state.app_thread, NULL);

	if (g_state.mutex != NULL)
		fb_MutexDestroy(g_state.mutex);

	memset(&g_state, 0, sizeof(g_state));
}

extern "C" void driver_lock(void)
{
	fb_MutexLock(g_state.mutex);
}

extern "C" void driver_unlock(void)
{
	if (g_state.bitmap != NULL && __fb_gfx != NULL && __fb_gfx->framebuffer != NULL) {
		if (g_state.window->Lock()) {
			unsigned char *dst = (unsigned char *)g_state.bitmap->Bits();
			int dst_pitch = g_state.bitmap->BytesPerRow();

			if (g_state.depth == 32) {
				int copy_pitch = MIN(dst_pitch, __fb_gfx->pitch);
				unsigned char *src = __fb_gfx->framebuffer;
				for (int y = 0; y < g_state.h; y++) {
					memcpy(dst, src, copy_pitch);
					dst += dst_pitch;
					src += __fb_gfx->pitch;
				}
			} else if (g_state.blitter != NULL) {
				/* Indexed depths: converts __fb_gfx->framebuffer (index
				 * bytes) to 32bpp via __fb_gfx->device_palette, honoring
				 * __fb_gfx->dirty per-scanline like every other gfxlib2
				 * driver using this same core helper. */
				g_state.blitter(dst, dst_pitch);
			}

			g_state.window->View()->Invalidate();
			g_state.window->Unlock();
		}
	}

	fb_MutexUnlock(g_state.mutex);
}

extern "C" void driver_set_palette(int index, int r, int g, int b)
{
	/* Nothing to do: __fb_gfx->device_palette (which the blitter in
	 * driver_unlock() actually reads) is already maintained by gfxlib2's
	 * own core, in gfx_palette.c, independently of this callback. */
	(void)index; (void)r; (void)g; (void)b;
}

extern "C" void driver_set_window_title(char *title)
{
	if (g_state.window != NULL && g_state.window->Lock()) {
		g_state.window->SetTitle(title);
		g_state.window->Unlock();
	}
}

extern "C" int driver_set_window_pos(int x, int y)
{
	if (g_state.window == NULL || !g_state.window->Lock())
		return 0;

	/* Matches the win32 driver's convention (see fb_hWin32SetWindowPos):
	 * moving an axis is skipped when it's the 0x80000000 sentinel; the
	 * current-position return value is only filled in for a pure query
	 * (both axes ignored), 0 is returned after an actual move. */
	if (x != (int)0x80000000 || y != (int)0x80000000) {
		BPoint origin = g_state.window->Frame().LeftTop();
		g_state.window->MoveTo(
			(x == (int)0x80000000) ? origin.x : x,
			(y == (int)0x80000000) ? origin.y : y);
		g_state.window->Unlock();
		return 0;
	}

	BPoint origin = g_state.window->Frame().LeftTop();
	g_state.window->Unlock();
	return ((int)origin.x & 0xFFFF) | ((int)origin.y << 16);
}

extern "C" int driver_get_mouse(int *x, int *y, int *z, int *buttons, int *clip)
{
	fb_MutexLock(g_state.mutex);
	*x = g_state.mouse_x;
	*y = g_state.mouse_y;
	*z = g_state.mouse_z;
	*buttons = g_state.mouse_buttons;
	*clip = g_state.mouse_clip ? 1 : 0;
	fb_MutexUnlock(g_state.mutex);
	return 0;
}

extern "C" void driver_set_mouse(int x, int y, int cursor, int clip)
{
	/* Repositioning the system cursor isn't implemented yet (Haiku's
	 * set_mouse_position() lives in <WindowScreen.h>, the fullscreen/game
	 * API, and isn't meant for a plain windowed BView -- see CLAUDE.md).
	 * Show/hide and clip-state tracking both work. */
	(void)x; (void)y;

	if (g_state.app != NULL) {
		if (cursor == 0)
			g_state.app->HideCursor();
		else if (cursor > 0)
			g_state.app->ShowCursor();
	}

	if (clip == 0 || clip > 0) {
		fb_MutexLock(g_state.mutex);
		g_state.mouse_clip = (clip != 0);
		fb_MutexUnlock(g_state.mutex);
	}
}

} /* anonymous namespace */

extern "C" const GFXDRIVER fb_gfxDriverHaiku =
{
	"Haiku",                 /* name */
	driver_init,             /* init */
	driver_exit,             /* exit */
	driver_lock,              /* lock */
	driver_unlock,            /* unlock */
	driver_set_palette,       /* set_palette */
	NULL,                     /* wait_vsync */
	driver_get_mouse,         /* get_mouse */
	driver_set_mouse,         /* set_mouse */
	driver_set_window_title,  /* set_window_title */
	driver_set_window_pos,    /* set_window_pos */
	NULL,                     /* fetch_modes */
	NULL,                     /* flip */
	NULL,                     /* poll_events -- BWindow's own looper thread pumps events */
	NULL                      /* update -- driver_unlock() already refreshes the screen */
};
