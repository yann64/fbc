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
 * v1 scope: 32bpp truecolor only (no palette/indexed-mode support), no
 * mouse support, no fullscreen/multi-monitor handling, no OpenGL. See
 * CLAUDE.md for the full list of known gaps.
 */

#include <Application.h>
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

private:
	BBitmap *fBitmap;
};

/* Best-effort DOS-scancode mapping for the handful of non-printable keys
 * FB programs commonly check for (arrows, editing keys, ESC/ENTER/etc.).
 * Haiku's raw hardware key codes don't correspond to AT scancodes, but the
 * "raw_char" field for these keys carries the stable BeOS B_*_ARROW-style
 * constants regardless of keyboard layout, which is enough to map from.
 */
int32 RawCharToScancode(int32 rawChar)
{
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
			int32 rawChar = 0;
			msg->FindInt32("raw_char", &rawChar);
			const char *bytes = NULL;
			msg->FindString("bytes", &bytes);

			EVENT e;
			memset(&e, 0, sizeof(e));
			e.type = (msg->what == B_KEY_DOWN) ? EVENT_KEY_PRESS : EVENT_KEY_RELEASE;
			e.scancode = RawCharToScancode(rawChar);
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

struct HaikuDriverState {
	pthread_t app_thread;
	sem_id ready_sem;
	FBHaikuApp *app;
	FBHaikuWindow *window;
	BBitmap *bitmap;
	FBMUTEX *mutex;
	int w, h;
	bool inited;
};

HaikuDriverState g_state;

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

	/* v1: truecolor only -- no palette/indexed-mode emulation yet. */
	if (depth != 32)
		return -1;

	memset(&g_state, 0, sizeof(g_state));
	g_state.w = w;
	g_state.h = h;
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

	if (g_state.window != NULL && g_state.window->Lock()) {
		g_state.window->Quit();
	}
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
			int copy_pitch = MIN(g_state.bitmap->BytesPerRow(), __fb_gfx->pitch);
			unsigned char *dst = (unsigned char *)g_state.bitmap->Bits();
			unsigned char *src = __fb_gfx->framebuffer;
			for (int y = 0; y < g_state.h; y++) {
				memcpy(dst, src, copy_pitch);
				dst += g_state.bitmap->BytesPerRow();
				src += __fb_gfx->pitch;
			}
			g_state.window->View()->Invalidate();
			g_state.window->Unlock();
		}
	}

	fb_MutexUnlock(g_state.mutex);
}

extern "C" void driver_set_palette(int index, int r, int g, int b)
{
	/* No indexed-mode support in v1 -- see driver_init's depth check. */
	(void)index; (void)r; (void)g; (void)b;
}

extern "C" void driver_set_window_title(char *title)
{
	if (g_state.window != NULL && g_state.window->Lock()) {
		g_state.window->SetTitle(title);
		g_state.window->Unlock();
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
	NULL,                     /* get_mouse -- not yet implemented, see CLAUDE.md */
	NULL,                     /* set_mouse */
	driver_set_window_title,  /* set_window_title */
	NULL,                     /* set_window_pos */
	NULL,                     /* fetch_modes */
	NULL,                     /* flip */
	NULL,                     /* poll_events -- BWindow's own looper thread pumps events */
	NULL                      /* update -- driver_unlock() already refreshes the screen */
};
