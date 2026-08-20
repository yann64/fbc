''
''
'' wchar -- Haiku bindings. mbstate_t layout verified against
'' /boot/system/develop/headers/posix/_mbstate_t.h (sizeof 1112 on x86_64).
''
#ifndef __crt_haiku_wchar_bi__
#define __crt_haiku_wchar_bi__

#include once "crt/stdio.bi"
#include once "crt/stdarg.bi"
#include once "crt/stddef.bi"
#include once "crt/long.bi"
#include once "crt/stdint.bi"

type mbstate_t
	converter as any ptr
	charset(0 to 63) as byte
	count as ulong
	data(0 to 1031) as byte
end type

#ifndef wint_t
type wint_t as ulong
#endif

#ifndef WEOF
const WEOF = cast(wint_t, -1)
#endif

#endif
