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
 * fullscreen/multi-monitor handling. This driver explicitly rejects
 * DRIVER_OPENGL (see driver_init() below) -- OpenGL support is a separate
 * driver, gfx_driver_opengl_haiku.cpp, which gfx_haiku.c tries first for
 * GFX_OPENGL requests. See CLAUDE.md for the full list of known gaps.
 */

#include <Application.h>
#include <AppDefs.h>
#include <Window.h>
#include <View.h>
#include <Bitmap.h>
#include <Screen.h>
#include <OS.h>
#include <InterfaceDefs.h>
#include <os/game/WindowScreen.h>  /* set_mouse_position() -- see driver_set_mouse() */

extern "C" {
#include "../fb_gfx.h"
#include "fb_gfx_haiku.h"
}

#include <pthread.h>
#include <string.h>

#include "haiku_input.h"

namespace {

using fb_haiku::KeyToScancode;
using fb_haiku::HaikuButtonsToFbButtons;

class FBHaikuApp;
class FBHaikuWindow;

struct HaikuDriverState {
	pthread_t app_thread;
	sem_id ready_sem;
	FBHaikuApp *app;
	FBHaikuWindow *window;
	BBitmap *bitmap;
	FBMUTEX *mutex;
	/* Separate from `mutex` on purpose -- see the comment above MouseMoved()
	 * below for the deadlock this avoids. Guards only mouse_x/y/z/buttons/
	 * clip, never anything touched while the BWindow lock is held. */
	FBMUTEX *mouse_mutex;
	int w, h, depth, refresh_rate;
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

	/* MouseMoved/MouseDown/MouseUp/wheel handling below all run on the
	 * window's own looper thread while it implicitly holds the BWindow lock
	 * (that's how BLooper dispatches any message). They must never touch
	 * g_state.mutex (the framebuffer/bitmap lock): driver_unlock() acquires
	 * g_state.mutex first and then explicitly locks the BWindow (to safely
	 * call Invalidate()) -- if a handler here acquired g_state.mutex while
	 * already holding the (implicit) BWindow lock, the two threads could
	 * each hold one lock and block on the other, deadlocking (found via a
	 * real hang: gdb showed the main thread stuck in driver_unlock() ->
	 * BWindow::Lock() while the window's looper thread was stuck in
	 * MouseMoved() -> fb_MutexLock(), each waiting on the other). Fixed by
	 * giving mouse state its own mutex that's never combined with the
	 * BWindow lock anywhere else in this file. */
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

			fb_MutexLock(g_state.mouse_mutex);
			g_state.mouse_z -= (int)deltaY;
			int z = g_state.mouse_z;
			fb_MutexUnlock(g_state.mouse_mutex);

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

		fb_MutexLock(g_state.mouse_mutex);
		int oldButtons = g_state.mouse_buttons;
		g_state.mouse_buttons = newButtons;
		fb_MutexUnlock(g_state.mouse_mutex);

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
		fb_MutexLock(g_state.mouse_mutex);
		int dx = (int)where.x - g_state.mouse_x;
		int dy = (int)where.y - g_state.mouse_y;
		g_state.mouse_x = (int)where.x;
		g_state.mouse_y = (int)where.y;
		fb_MutexUnlock(g_state.mouse_mutex);

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
	if (flags & DRIVER_OPENGL)
		return -1;

	/* 8bpp indexed and 32bpp truecolor only -- 15/16/24bpp aren't handled
	 * (no BAS program-visible way to request them via ScreenRes anyway). */
	if (depth != 32 && depth != 8)
		return -1;

	memset(&g_state, 0, sizeof(g_state));
	g_state.w = w;
	g_state.h = h;
	g_state.refresh_rate = (refresh_rate > 0) ? refresh_rate : 60;
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
	g_state.mouse_mutex = fb_MutexCreate();
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
	if (g_state.mouse_mutex != NULL)
		fb_MutexDestroy(g_state.mouse_mutex);

	memset(&g_state, 0, sizeof(g_state));
}

extern "C" void driver_lock(void)
{
	fb_MutexLock(g_state.mutex);
}

extern "C" void driver_wait_vsync(void)
{
	/* wait_vsync is only ever called once a screen is already open (per
	 * fb_gfx.h's contract), so a BApplication always exists by now, and
	 * BScreen can give a real hardware retrace wait -- confirmed empirically
	 * that this matters: querying BScreen *before* any window/app exists
	 * (see driver_fetch_modes's IsValid() check below) returns garbage. */
	BScreen screen;
	if (screen.IsValid() && screen.WaitForRetrace() == B_OK)
		return;

	/* Fallback: the doc-comment-sanctioned approximation of sleeping for
	 * 1/refresh_rate seconds (refresh_rate defaults to 60 if ScreenRes
	 * wasn't given an explicit one). */
	bigtime_t period = 1000000LL / g_state.refresh_rate;
	snooze(period);
}

extern "C" int *driver_fetch_modes(int depth, int *size)
{
	/* This driver isn't limited to fixed hardware modes -- any w/h works
	 * via ScreenRes -- so this is just a curated list of common sizes for
	 * programs that enumerate modes to offer a menu, for the two depths
	 * actually supported. */
	if (depth != 32 && depth != 8) {
		*size = 0;
		return NULL;
	}

	static const int candidates[][2] = {
		{320, 200}, {320, 240}, {640, 480}, {800, 600},
		{1024, 768}, {1280, 720}, {1280, 1024}, {1920, 1080},
	};
	const int count = sizeof(candidates) / sizeof(candidates[0]);

	/* SCREENLIST/fetch_modes is typically called *before* ScreenRes, to
	 * help a program decide what resolution to request -- meaning no
	 * BApplication exists yet. Confirmed empirically: BScreen's Frame()
	 * silently returns a bogus 1x1 rect in that case (IsValid() catches
	 * it) rather than failing loudly, so the desktop-size cap/entry below
	 * is skipped entirely unless a screen session is already live. */
	BScreen screen;
	bool have_desktop_size = screen.IsValid();
	BRect frame = have_desktop_size ? screen.Frame() : BRect();
	int desktop_w = (int)frame.Width() + 1;
	int desktop_h = (int)frame.Height() + 1;

	int *modes = (int *)malloc(sizeof(int) * (count + 1));
	if (modes == NULL) {
		*size = 0;
		return NULL;
	}

	int n = 0;
	for (int i = 0; i < count; i++) {
		if (!have_desktop_size ||
		    (candidates[i][0] <= desktop_w && candidates[i][1] <= desktop_h))
			modes[n++] = (candidates[i][0] << 16) | candidates[i][1];
	}
	if (have_desktop_size)
		modes[n++] = (desktop_w << 16) | desktop_h;

	*size = n;
	return modes;
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
	fb_MutexLock(g_state.mouse_mutex);
	*x = g_state.mouse_x;
	*y = g_state.mouse_y;
	*z = g_state.mouse_z;
	*buttons = g_state.mouse_buttons;
	*clip = g_state.mouse_clip ? 1 : 0;
	fb_MutexUnlock(g_state.mouse_mutex);
	return 0;
}

extern "C" void driver_set_mouse(int x, int y, int cursor, int clip)
{
	if (g_state.app != NULL) {
		if (cursor == 0)
			g_state.app->HideCursor();
		else if (cursor > 0)
			g_state.app->ShowCursor();
	}

	/* set_mouse_position() (libgame.so, <os/game/WindowScreen.h>) takes
	 * absolute screen coordinates, but per fb_gfx.h's GFXDRIVER contract
	 * x/y here are relative to the graphics drawing area -- convert via
	 * the view, which requires the window to be locked. Confirmed working
	 * from a plain windowed BView (not just BWindowScreen/fullscreen, which
	 * is the only usage the Haiku docs actually show): a standalone test
	 * program linked only against -lgame successfully moved the system
	 * cursor with no window at all, and wiring it in here moves the cursor
	 * over this driver's own window correctly, verified via GetMouse()
	 * reading back the new position and a screenshot. */
	if (x >= 0 && y >= 0 && g_state.window != NULL && g_state.window->Lock()) {
		BPoint screenPt = g_state.window->View()->ConvertToScreen(BPoint(x, y));
		g_state.window->Unlock();
		set_mouse_position((int32)screenPt.x, (int32)screenPt.y);
	}

	if (clip == 0 || clip > 0) {
		fb_MutexLock(g_state.mouse_mutex);
		g_state.mouse_clip = (clip != 0);
		fb_MutexUnlock(g_state.mouse_mutex);
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
	driver_wait_vsync,        /* wait_vsync */
	driver_get_mouse,         /* get_mouse */
	driver_set_mouse,         /* set_mouse */
	driver_set_window_title,  /* set_window_title */
	driver_set_window_pos,    /* set_window_pos */
	driver_fetch_modes,       /* fetch_modes */
	NULL,                     /* flip */
	NULL,                     /* poll_events -- BWindow's own looper thread pumps events */
	NULL                      /* update -- driver_unlock() already refreshes the screen */
};
