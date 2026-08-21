/* Native Haiku OpenGL gfx driver, using the OpenGL Kit's BGLView.
 *
 * Separate from the plain driver (gfx_driver_haiku.cpp) on purpose: that
 * driver blits __fb_gfx->framebuffer into a BBitmap shown by a plain BView,
 * and its BView::Draw() runs on the window's own looper thread reading that
 * bitmap -- there is real, hard-won history here (see CLAUDE.md) around
 * getting the locking between that thread and the FB program's own thread
 * right. This driver has no BBitmap and no Draw() override at all: OpenGL
 * content lives entirely in the BGLView's own front buffer, touched only
 * from gl_driver_unlock()/gl_driver_flip(), both called on the FB program's own
 * thread (gfxlib2 core already serializes calls to the driver's lock/unlock
 * via FB_GRAPHICS_LOCK -- see gfx_access.c). That removes the whole class of
 * cross-thread framebuffer-access bug the plain driver had to fix, so this
 * driver needs its own framebuffer mutex, only a mouse-state one (same
 * reasoning as the plain driver: mouse callbaks run on the window's own
 * looper thread, which already implicitly holds the BWindow lock while
 * dispatching a message).
 *
 * gfxlib2's OpenGL support (see gfx_opengl.c) has two independent modes,
 * selected by ScreenControl(SET_GL_2D_MODE, ...) before SCREEN is called:
 *  - OGL_2D_NONE (the default): the FB program does real OpenGL rendering
 *    itself, via function pointers obtained from GfxGetGLProcAddress() (or
 *    just linking directly against libGL, which Haiku ships) and calls
 *    Flip/PageCopy to trigger a buffer swap (gl_driver_flip() below).
 *  - OGL_2D_MANUAL_SYNC / OGL_2D_AUTO_SYNC: the FB program keeps using the
 *    ordinary 2D drawing primitives (LINE/CIRCLE/PSET/etc, which still
 *    write into __fb_gfx->framebuffer exactly as with any other driver) and
 *    gfxlib2 core (fb_hGL_SetupProjection(), gfx_opengl.c) uploads that
 *    buffer as a texture and draws a fullscreen textured quad -- MANUAL_SYNC
 *    does this only when the FB program calls Flip explicitly, AUTO_SYNC
 *    does it automatically on every gl_driver_unlock() (i.e. after every
 *    drawing primitive), matching the X11 OpenGL driver's behaviour exactly
 *    (see src/gfxlib2/unix/gfx_driver_opengl_x11.c, the closest upstream
 *    precedent, reused here as the template for this mode-dispatch logic).
 *
 * Unlike GLX's MakeCurrent (a persistent per-thread binding), BGLView's
 * context is only current for the calling thread *while LockGL() is held*
 * -- so gl_driver_init() takes that lock and deliberately never releases it
 * (until gl_driver_exit()), keeping the context current for the FB
 * program's own thread for the program's whole lifetime, which raw-GL
 * programs (OGL_2D_NONE, calling gl* directly, not through this driver at
 * all) depend on. gl_driver_unlock()/gl_driver_flip() re-lock/unlock around
 * their own GL work on top of that; safe because BLocker (what LockGL()
 * wraps) is recursive for the owning thread. See the comment at the
 * LockGL() call in gl_driver_init() for how this was found (a raw-GL test
 * rendered nothing at all without it).
 *
 * GL_SCALE is supported -- see the __fb_gl_params.scale handling in
 * gl_driver_init() below; the window is created at w*scale x h*scale while
 * the logical framebuffer stays at w x h.
 */

#include <Application.h>
#include <AppDefs.h>
#include <Window.h>
#include <opengl/GLView.h>
#include <Screen.h>
#include <OS.h>
#include <InterfaceDefs.h>
#include <os/game/WindowScreen.h>  /* set_mouse_position() -- see gl_driver_set_mouse() */

extern "C" {
#include "../fb_gfx.h"
#include "../fb_gfx_gl.h"
#include "fb_gfx_opengl_haiku.h"
}

/* fb_gfx_gl.h's own declarations (FB_GL_PARAMS, fb_hGL_*, etc.) are already
 * compiled out under DISABLE_OPENGL, so this whole file must be too --
 * matches src/gfxlib2/unix/gfx_driver_opengl_x11.c's own guard. Not
 * currently set for Haiku (see the makefile), but kept for robustness. */
#ifndef DISABLE_OPENGL

#include <pthread.h>
#include <string.h>

#include "haiku_input.h"

namespace {

using fb_haiku::KeyToScancode;
using fb_haiku::HaikuButtonsToFbButtons;

class FBHaikuGLApp;
class FBHaikuGLWindow;
class FBHaikuGLView;

struct HaikuGLDriverState {
	pthread_t app_thread;
	sem_id ready_sem;
	FBHaikuGLApp *app;
	FBHaikuGLWindow *window;
	FBMUTEX *mouse_mutex;
	FB_DYLIB gl_lib;
	int w, h, depth, refresh_rate;
	bool inited;
	bool exiting;
	int mouse_x, mouse_y, mouse_z, mouse_buttons;
	bool mouse_clip;
};

HaikuGLDriverState g_state;

class FBHaikuGLView : public BGLView {
public:
	FBHaikuGLView(BRect frame, ulong glOptions)
		: BGLView(frame, "fbgfx_glview", B_FOLLOW_ALL, 0, glOptions)
	{
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

	/* Same reasoning as the plain driver's FBHaikuView: these run on the
	 * window's own looper thread, implicitly holding the BWindow lock, so
	 * they must only ever touch g_state.mouse_mutex, never anything else
	 * that could be held by a thread that itself wants the BWindow lock. */
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
			BGLView::MessageReceived(msg);
		}
	}

private:
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
};

class FBHaikuGLWindow : public BWindow {
public:
	FBHaikuGLWindow(BRect frame, const char *title, ulong glOptions)
		: BWindow(frame, title, B_TITLED_WINDOW,
			B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS)
	{
		fView = new FBHaikuGLView(Bounds(), glOptions);
		AddChild(fView);
	}

	bool QuitRequested() override
	{
		if (g_state.exiting)
			return true; /* gl_driver_exit()'s own programmatic shutdown */

		EVENT e;
		memset(&e, 0, sizeof(e));
		e.type = EVENT_WINDOW_CLOSE;
		fb_hPostEvent(&e);
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

	FBHaikuGLView *View() const { return fView; }

private:
	FBHaikuGLView *fView;
};

class FBHaikuGLApp : public BApplication {
public:
	FBHaikuGLApp() : BApplication("application/x-vnd.fbc-gfx-gl") {}
};

ulong BuildGLOptions(void)
{
	ulong options = BGL_RGB | BGL_DOUBLE | BGL_DEPTH;
	if (__fb_gl_params.color_alpha_bits > 0)
		options |= BGL_ALPHA;
	if (__fb_gl_params.stencil_bits > 0)
		options |= BGL_STENCIL;
	if (__fb_gl_params.accum_bits > 0)
		options |= BGL_ACCUM;
	return options;
}

void *AppThreadEntry(void *)
{
	g_state.app = new FBHaikuGLApp();

	BRect frame(0, 0, g_state.w - 1, g_state.h - 1);
	g_state.window = new FBHaikuGLWindow(frame,
		__fb_window_title ? __fb_window_title : "FreeBASIC", BuildGLOptions());
	g_state.window->CenterOnScreen();
	g_state.window->Show();

	release_sem(g_state.ready_sem);

	g_state.app->Run();

	delete g_state.app;
	g_state.app = NULL;
	return NULL;
}

} /* anonymous namespace */

extern "C" void *fb_hGL_GetProcAddress(const char *proc)
{
	void *addr = NULL;
	const char *names[1] = { proc };
	if (g_state.gl_lib == NULL || fb_hDynLoadAlso(g_state.gl_lib, names, &addr, 1))
		return NULL;
	return addr;
}

extern "C" int gl_driver_init(char *title, int w, int h, int depth, int refresh_rate, int flags)
{
	(void)title; (void)depth;

	if (!(flags & DRIVER_OPENGL))
		return -1;

	memset(&g_state, 0, sizeof(g_state));
	g_state.refresh_rate = (refresh_rate > 0) ? refresh_rate : 60;
	g_state.depth = depth;

	fb_hGL_NormalizeParameters(flags);

	/* GL_SCALE (ScreenControl SET_GL_SCALE, before ScreenRes): render into
	 * a physically larger window (w*scale x h*scale) than the logical
	 * framebuffer (__fb_gfx->w/h, unchanged) -- fb_hGL_SetupProjection()
	 * (gfx_opengl.c, shared by every OpenGL driver) already sizes its
	 * glViewport() by __fb_gl_params.scale and uploads the framebuffer at
	 * its logical size as a texture with GL_LINEAR filtering (set up in
	 * fb_hGL_ScreenCreate()), so the upscale-with-smoothing is entirely
	 * generic core behavior -- the only thing a driver needs to do is
	 * create its window/view at the scaled physical size, same as the X11
	 * OpenGL driver's `fb_hX11Init(title, w * scale, h * scale, ...)`. */
	if (__fb_gl_params.init_scale >= 1)
		__fb_gl_params.scale = __fb_gl_params.init_scale;
	g_state.w = w * __fb_gl_params.scale;
	g_state.h = h * __fb_gl_params.scale;

	g_state.mouse_mutex = fb_MutexCreate();
	g_state.ready_sem = create_sem(0, "fbgfx haiku gl ready");
	if (g_state.ready_sem < 0)
		return -1;

	if (pthread_create(&g_state.app_thread, NULL, AppThreadEntry, NULL) != 0) {
		delete_sem(g_state.ready_sem);
		g_state.ready_sem = -1;
		return -1;
	}

	acquire_sem(g_state.ready_sem);
	delete_sem(g_state.ready_sem);
	g_state.ready_sem = -1;

	if (g_state.window == NULL || g_state.window->View() == NULL)
		return -1;

	static const char *const init_funcs[] = { "glGetString", NULL };
	void *init_ptr[1];
	g_state.gl_lib = fb_hDynLoad("libGL.so", init_funcs, init_ptr);
	if (g_state.gl_lib == NULL)
		return -1;

	__fb_gl_params.mode_2d = __fb_gl_params.init_mode_2d;

	/* LockGL() here is deliberately never matched by an UnlockGL() before
	 * returning -- see the file-header comment: unlike GLX's MakeCurrent,
	 * which binds a context to a thread until explicitly released, BGLView
	 * only makes its context current for the calling thread *while locked*.
	 * An FB program doing raw GL rendering (OGL_2D_NONE, the default) calls
	 * gl* functions directly -- linked straight against libGL, not routed
	 * through this driver at all -- and needs a current context for that to
	 * work at all. Locking here and never unlocking keeps it current for
	 * the calling thread (the FB program's own, same thread that called
	 * driver_init) for the rest of the program's life; gl_driver_unlock()/
	 * gl_driver_flip() below re-lock/unlock around their own GL work, which
	 * is safe and correctly stays current throughout since BLocker (what
	 * LockGL()/UnlockGL() are built on) is recursive for the owning thread.
	 * Confirmed necessary the hard way: without this, a raw-GL test program
	 * rendered nothing (window came up solid black) because glClear/glBegin
	 * had no current context to operate on. */
	FBHaikuGLView *view = g_state.window->View();
	view->LockGL();
	int gl_init_failed = fb_hGL_Init(g_state.gl_lib, NULL);
	if (!gl_init_failed && __fb_gl_params.mode_2d != DRIVER_OGL_2D_NONE)
		fb_hGL_ScreenCreate();

	if (gl_init_failed) {
		view->UnlockGL();
		return -1;
	}

	g_state.inited = true;
	return 0;
}

extern "C" void gl_driver_exit(void)
{
	/* Release the persistent LockGL() held since gl_driver_init() -- see
	 * the comment there. Only taken (and left locked) on the success path,
	 * so only release it if init actually got that far. */
	if (g_state.inited && g_state.window != NULL && g_state.window->View() != NULL)
		g_state.window->View()->UnlockGL();

	if (g_state.gl_lib != NULL)
		fb_hDynUnload(&g_state.gl_lib);

	g_state.exiting = true;

	if (g_state.window != NULL && g_state.window->Lock())
		g_state.window->Quit();

	if (g_state.app != NULL)
		g_state.app->PostMessage(B_QUIT_REQUESTED);

	if (g_state.app_thread != 0)
		pthread_join(g_state.app_thread, NULL);

	if (g_state.mouse_mutex != NULL)
		fb_MutexDestroy(g_state.mouse_mutex);

	memset(&g_state, 0, sizeof(g_state));
}

extern "C" void gl_driver_lock(void)
{
	/* Nothing to protect here -- see the file-header comment: this driver
	 * has no second thread touching GL-drawn content. Still required to be
	 * non-NULL per the GFXDRIVER contract. */
}

extern "C" void gl_driver_unlock(void)
{
	if (g_state.window == NULL || g_state.window->View() == NULL)
		return;

	if (__fb_gl_params.mode_2d == DRIVER_OGL_2D_AUTO_SYNC) {
		FBHaikuGLView *view = g_state.window->View();
		view->LockGL();
		fb_hGL_SetupProjection();
		view->SwapBuffers();
		view->UnlockGL();
	}
}

extern "C" void gl_driver_flip(void)
{
	if (g_state.window == NULL || g_state.window->View() == NULL)
		return;

	FBHaikuGLView *view = g_state.window->View();
	view->LockGL();
	if (__fb_gl_params.mode_2d == DRIVER_OGL_2D_MANUAL_SYNC)
		fb_hGL_SetupProjection();
	view->SwapBuffers();
	view->UnlockGL();
}

extern "C" void gl_driver_wait_vsync(void)
{
	BScreen screen;
	if (screen.IsValid() && screen.WaitForRetrace() == B_OK)
		return;
	bigtime_t period = 1000000LL / g_state.refresh_rate;
	snooze(period);
}

extern "C" int *gl_driver_fetch_modes(int depth, int *size)
{
	if (depth != 32) {
		*size = 0;
		return NULL;
	}

	static const int candidates[][2] = {
		{320, 200}, {320, 240}, {640, 480}, {800, 600},
		{1024, 768}, {1280, 720}, {1280, 1024}, {1920, 1080},
	};
	const int count = sizeof(candidates) / sizeof(candidates[0]);

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

extern "C" int gl_driver_get_mouse(int *x, int *y, int *z, int *buttons, int *clip)
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

extern "C" void gl_driver_set_mouse(int x, int y, int cursor, int clip)
{
	if (g_state.app != NULL) {
		if (cursor == 0)
			g_state.app->HideCursor();
		else if (cursor > 0)
			g_state.app->ShowCursor();
	}

	/* See the plain driver's driver_set_mouse() for the full explanation --
	 * same approach, same View()->ConvertToScreen() + set_mouse_position(). */
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

extern "C" void gl_driver_set_window_title(char *title)
{
	if (g_state.window != NULL && g_state.window->Lock()) {
		g_state.window->SetTitle(title);
		g_state.window->Unlock();
	}
}

extern "C" int gl_driver_set_window_pos(int x, int y)
{
	if (g_state.window == NULL || !g_state.window->Lock())
		return 0;

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

extern "C" const GFXDRIVER fb_gfxDriverHaikuOpenGL =
{
	"Haiku OpenGL",           /* name */
	gl_driver_init,              /* init */
	gl_driver_exit,              /* exit */
	gl_driver_lock,              /* lock */
	gl_driver_unlock,            /* unlock */
	fb_hGL_SetPalette,        /* set_palette */
	gl_driver_wait_vsync,        /* wait_vsync */
	gl_driver_get_mouse,         /* get_mouse */
	gl_driver_set_mouse,         /* set_mouse */
	gl_driver_set_window_title,  /* set_window_title */
	gl_driver_set_window_pos,    /* set_window_pos */
	gl_driver_fetch_modes,       /* fetch_modes */
	gl_driver_flip,              /* flip */
	NULL,                     /* poll_events -- BWindow's own looper thread pumps events */
	NULL                      /* update */
};

#endif /* !DISABLE_OPENGL */
