''
''
'' sys\types -- Haiku type definitions, verified against
'' /boot/system/develop/headers/posix/sys/types.h (x86_64, hrev59979)
''
#ifndef __crt_sys_haiku_types_bi__
#define __crt_sys_haiku_types_bi__

#include once "crt/stddef.bi"
#include once "crt/long.bi"

type __clock_t as long
type __time_t as longint

type dev_t as long
type ino_t as longint
type mode_t as ulong
type nlink_t as long
type uid_t as ulong
type gid_t as ulong
type off_t as longint
type pid_t as long
#ifndef ssize_t
type ssize_t as integer
#endif

type blkcnt_t as longint
type blksize_t as long
type fsblkcnt_t as longint
type fsfilcnt_t as longint

type int8_t as byte
type int16_t as short
type int32_t as long
type int64_t as longint
type u_int8_t as ubyte
type u_int16_t as ushort
type u_int32_t as ulong
type u_int64_t as ulongint
type register_t as long

#endif
