/* get the executable's name */

#include "../fb.h"
#include <kernel/image.h>

char *fb_hGetExeName( char *dst, ssize_t maxlen )
{
	int32 cookie = 0;
	image_info info;
	char *p;

	p = NULL;
	while( get_next_image_info( 0, &cookie, &info ) == B_OK ) {
		if( info.type == B_APP_IMAGE ) {
			strncpy( dst, info.name, maxlen );
			dst[maxlen - 1] = '\0';

			p = strrchr( dst, '/' );
			if( p != NULL )
				++p;
			else
				p = dst;
			break;
		}
	}

	return p;
}
