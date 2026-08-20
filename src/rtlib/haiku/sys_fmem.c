/* fre() function */

#include "../fb.h"
#include <OS.h>

FBCALL size_t fb_GetMemAvail( int mode )
{
	system_info info;

	if( get_system_info( &info ) != B_OK )
		return 0;

	return (size_t)(info.max_pages - info.used_pages) * B_PAGE_SIZE;
}
