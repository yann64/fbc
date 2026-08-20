''
''
'' stdio -- Haiku bindings.
''
'' Haiku's own <stdio.h> declares FILE as a fully opaque incomplete type
'' (sizeof(FILE) doesn't even compile there) -- callers only ever get a
'' FILE ptr back from fopen() etc. and pass it around, never dereference
'' it, so an empty placeholder type is correct here, not just convenient.
''
#ifndef __crt_haiku_stdio_bi__
#define __crt_haiku_stdio_bi__

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define BUFSIZ 8192
#define FILENAME_MAX 256
#define FOPEN_MAX 128
#define P_tmpdir "/tmp"
#define L_tmpnam 512
#define TMP_MAX 32768

type FILE
	_opaque as byte
end type

'' Confirmed via `nm -D libroot.so`: Haiku exports plain stdin/stdout/stderr
'' (POSIX-standard naming), not BSD's __stdinp/__stdoutp/__stderrp.
extern stdin alias "stdin" as FILE ptr
extern stdout alias "stdout" as FILE ptr
extern stderr alias "stderr" as FILE ptr

type fpos_t as longint

extern "c"
declare function snprintf (byval s as zstring ptr, byval n as size_t, byval format as zstring ptr, ...) as long
declare function vsnprintf (byval s as zstring ptr, byval n as size_t, byval format as zstring ptr, byval arg as va_list) as long
declare function popen (byval as zstring ptr, byval as zstring ptr) as FILE ptr
declare function pclose (byval as FILE ptr) as long
declare function getw (byval as FILE ptr) as long
declare function putw (byval as long, byval as FILE ptr) as long
end extern

#endif
