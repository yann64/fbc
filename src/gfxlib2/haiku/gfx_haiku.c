#include "../fb_gfx.h"
#include "fb_gfx_haiku.h"

const GFXDRIVER *__fb_gfx_drivers_list[] = {
	&fb_gfxDriverHaiku,
	NULL
};

void fb_hScreenInfo(ssize_t *width, ssize_t *height, ssize_t *depth, ssize_t *refresh)
{
	/* TODO: query the real desktop resolution/depth via BScreen; not
	 * needed for a fixed-size SCREEN mode window. */
	*width = *height = *depth = *refresh = 0;
}
