#!/bin/sh

# Copyright 2009 The Go Authors. All rights reserved.
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.

# Create sysinfo.go.

# This shell script creates the sysinfo.go file which holds types and
# constants extracted from the system header files.  This relies on a
# hook in gcc: the -fdump-go-spec option will generate debugging
# information in Go syntax.

# We currently #include all the files at once, which works, but leads
# to exposing some names which ideally should not be exposed, as they
# match grep patterns.  E.g., WCHAR_MIN gets exposed because it starts
# with W, like the wait flags.

CC=${CC:-gcc}
OUT=tmp-sysinfo.go

set -e

rm -f sysinfo.c
cat > sysinfo.c <<EOF
#include "config.h"

#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
/* <netinet/tcp.h> needs u_char/u_short, but <sys/bsd_types> is only
   included by <netinet/in.h> if _SGIAPI (i.e. _SGI_SOURCE
   && !_XOPEN_SOURCE.
   <sys/termios.h> only defines TIOCNOTTY if !_XOPEN_SOURCE, while
   <sys/ttold.h> does so unconditionally.  */
#ifdef __sgi__
#include <sys/bsd_types.h>
#include <sys/ttold.h>
#endif
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#if defined(HAVE_SYSCALL_H)
#include <syscall.h>
#endif
#if defined(HAVE_SYS_SYSCALL_H)
#include <sys/syscall.h>
#endif
#if defined(HAVE_SYS_EPOLL_H)
#include <sys/epoll.h>
#endif
#if defined(HAVE_SYS_MMAN_H)
#include <sys/mman.h>
#endif
#if defined(HAVE_SYS_PTRACE_H)
#include <sys/ptrace.h>
#endif
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/un.h>
#if defined(HAVE_SYS_USER_H)
#include <sys/user.h>
#endif
#if defined(HAVE_SYS_UTSNAME_H)
#include <sys/utsname.h>
#endif
#if defined(HAVE_SYS_SELECT_H)
#include <sys/select.h>
#endif
#include <unistd.h>
#include <netdb.h>
#include <pwd.h>
#if defined(HAVE_LINUX_FILTER_H)
#include <linux/filter.h>
#endif
#if defined(HAVE_LINUX_NETLINK_H)
#include <linux/netlink.h>
#endif
#if defined(HAVE_LINUX_RTNETLINK_H)
#include <linux/rtnetlink.h>
#endif
#if defined(HAVE_NET_IF_H)
#include <net/if.h>
#endif

/* Constants that may only be defined as expressions on some systems,
   expressions too complex for -fdump-go-spec to handle.  These are
   handled specially below.  */
enum {
#ifdef TIOCGWINSZ
  TIOCGWINSZ_val = TIOCGWINSZ,
#endif
};
EOF

${CC} -fdump-go-spec=gen-sysinfo.go -std=gnu99 -S -o sysinfo.s sysinfo.c

echo 'package syscall' > ${OUT}
echo 'import "unsafe"' >> ${OUT}
echo 'type _ unsafe.Pointer' >> ${OUT}

# Get all the consts and types, skipping ones which could not be
# represented in Go and ones which we need to rewrite.  We also skip
# function declarations, as we don't need them here.  All the symbols
# will all have a leading underscore.
grep -v '^// ' gen-sysinfo.go | \
  grep -v '^func' | \
  grep -v '^type _timeval ' | \
  grep -v '^type _timespec_t ' | \
  grep -v '^type _timespec ' | \
  grep -v '^type _timestruc_t ' | \
  grep -v '^type _epoll_' | \
  grep -v 'in6_addr' | \
  grep -v 'sockaddr_in6' | \
  sed -e 's/\([^a-zA-Z0-9_]\)_timeval\([^a-zA-Z0-9_]\)/\1Timeval\2/g' \
      -e 's/\([^a-zA-Z0-9_]\)_timespec_t\([^a-zA-Z0-9_]\)/\1Timespec\2/g' \
      -e 's/\([^a-zA-Z0-9_]\)_timespec\([^a-zA-Z0-9_]\)/\1Timespec\2/g' \
      -e 's/\([^a-zA-Z0-9_]\)_timestruc_t\([^a-zA-Z0-9_]\)/\1Timestruc\2/g' \
    >> ${OUT}

# The errno constants.  These get type Errno.
echo '#include <errno.h>' | ${CC} -x c - -E -dM | \
  egrep '#define E[A-Z0-9_]+ ' | \
  sed -e 's/^#define \(E[A-Z0-9_]*\) .*$/const \1 = Errno(_\1)/' >> ${OUT}

# The O_xxx flags.
egrep '^const _(O|F|FD)_' gen-sysinfo.go | \
  sed -e 's/^\(const \)_\([^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}
if ! grep '^const O_ASYNC' ${OUT} >/dev/null 2>&1; then
  echo "const O_ASYNC = 0" >> ${OUT}
fi
if ! grep '^const O_CLOEXEC' ${OUT} >/dev/null 2>&1; then
  echo "const O_CLOEXEC = 0" >> ${OUT}
fi

# The signal numbers.
grep '^const _SIG[^_]' gen-sysinfo.go | \
  grep -v '^const _SIGEV_' | \
  sed -e 's/^\(const \)_\(SIG[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}

# The syscall numbers.  We force the names to upper case.
grep '^const _SYS_' gen-sysinfo.go | \
  sed -e 's/const _\(SYS_[^= ]*\).*$/\1/' | \
  while read sys; do
    sup=`echo $sys | tr abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ`
    echo "const $sup = _$sys" >> ${OUT}
  done

# Stat constants.
grep '^const _S_' gen-sysinfo.go | \
  sed -e 's/^\(const \)_\(S_[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}

# Mmap constants.
grep '^const _PROT_' gen-sysinfo.go | \
  sed -e 's/^\(const \)_\(PROT_[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}
grep '^const _MAP_' gen-sysinfo.go | \
  sed -e 's/^\(const \)_\(MAP_[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}

# Process status constants.
grep '^const _W' gen-sysinfo.go |
  sed -e 's/^\(const \)_\(W[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}
# WSTOPPED was introduced in glibc 2.3.4.
if ! grep '^const _WSTOPPED = ' gen-sysinfo.go >/dev/null 2>&1; then
  if grep '^const _WUNTRACED = ' gen-sysinfo.go > /dev/null 2>&1; then
    echo 'const WSTOPPED = _WUNTRACED' >> ${OUT}
  else
    echo 'const WSTOPPED = 2' >> ${OUT}
  fi
fi
if grep '^const ___WALL = ' gen-sysinfo.go >/dev/null 2>&1 \
   && ! grep '^const _WALL = ' gen-sysinfo.go >/dev/null 2>&1; then
  echo 'const WALL = ___WALL' >> ${OUT}
fi

# Networking constants.
egrep '^const _(AF|SOCK|SOL|SO|IPPROTO|TCP|IP|IPV6)_' gen-sysinfo.go |
  sed -e 's/^\(const \)_\([^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}
grep '^const _SOMAXCONN' gen-sysinfo.go |
  sed -e 's/^\(const \)_\(SOMAXCONN[^= ]*\)\(.*\)$/\1\2 = _\2/' \
    >> ${OUT}
grep '^const _SHUT_' gen-sysinfo.go |
  sed -e 's/^\(const \)_\(SHUT[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}

# The net package requires some const definitions.
for m in IPV6_V6ONLY IPPROTO_IPV6 IPV6_JOIN_GROUP IPV6_LEAVE_GROUP; do
  if ! grep "^const $m " ${OUT} >/dev/null 2>&1; then
    echo "const $m = 0" >> ${OUT}
  fi
done

# pathconf constants.
grep '^const __PC' gen-sysinfo.go |
  sed -e 's/^\(const \)__\(PC[^= ]*\)\(.*\)$/\1\2 = __\2/' >> ${OUT}

# epoll constants.
grep '^const _EPOLL' gen-sysinfo.go |
  sed -e 's/^\(const \)_\(EPOLL[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}
# Make sure EPOLLRDHUP and EPOLL_CLOEXEC are defined.
if ! grep '^const EPOLLRDHUP' ${OUT} >/dev/null 2>&1; then
  echo "const EPOLLRDHUP = 0x2000" >> ${OUT}
fi
if ! grep '^const EPOLL_CLOEXEC' ${OUT} >/dev/null 2>&1; then
  echo "const EPOLL_CLOEXEC = 02000000" >> ${OUT}
fi

# Ptrace constants.
grep '^const _PTRACE' gen-sysinfo.go |
  sed -e 's/^\(const \)_\(PTRACE[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}
# We need some ptrace options that are not defined in older versions
# of glibc.
if ! grep '^const PTRACE_SETOPTIONS' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_SETOPTIONS = 0x4200" >> ${OUT}
fi
if ! grep '^const PTRACE_O_TRACESYSGOOD' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_O_TRACESYSGOOD = 0x1" >> ${OUT}
fi
if ! grep '^const PTRACE_O_TRACEFORK' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_O_TRACEFORK = 0x2" >> ${OUT}
fi
if ! grep '^const PTRACE_O_TRACEVFORK' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_O_TRACEVFORK = 0x4" >> ${OUT}
fi
if ! grep '^const PTRACE_O_TRACECLONE' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_O_TRACECLONE = 0x8" >> ${OUT}
fi
if ! grep '^const PTRACE_O_TRACEEXEC' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_O_TRACEEXEC = 0x10" >> ${OUT}
fi
if ! grep '^const PTRACE_O_TRACEVFORKDONE' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_O_TRACEVFORKDONE = 0x20" >> ${OUT}
fi
if ! grep '^const PTRACE_O_TRACEEXIT' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_O_TRACEEXIT = 0x40" >> ${OUT}
fi
if ! grep '^const PTRACE_O_MASK' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_O_MASK = 0x7f" >> ${OUT}
fi
if ! grep '^const _PTRACE_GETEVENTMSG' ${OUT} > /dev/null 2>&1; then
  echo "const _PTRACE_GETEVENTMSG = 0x4201" >> ${OUT}
fi
if ! grep '^const PTRACE_EVENT_FORK' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_EVENT_FORK = 1" >> ${OUT}
fi
if ! grep '^const PTRACE_EVENT_VFORK' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_EVENT_VFORK = 2" >> ${OUT}
fi
if ! grep '^const PTRACE_EVENT_CLONE' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_EVENT_CLONE = 3" >> ${OUT}
fi
if ! grep '^const PTRACE_EVENT_EXEC' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_EVENT_EXEC = 4" >> ${OUT}
fi
if ! grep '^const PTRACE_EVENT_VFORK_DONE' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_EVENT_VFORK_DONE = 5" >> ${OUT}
fi
if ! grep '^const PTRACE_EVENT_EXIT' ${OUT} > /dev/null 2>&1; then
  echo "const PTRACE_EVENT_EXIT = 6" >> ${OUT}
fi
if ! grep '^const _PTRACE_TRACEME' ${OUT} > /dev/null 2>&1; then
  echo "const _PTRACE_TRACEME = 0" >> ${OUT}
fi

# The registers returned by PTRACE_GETREGS.  This is probably
# GNU/Linux specific; it should do no harm if there is no
# _user_regs_struct.
regs=`grep '^type _user_regs_struct struct' gen-sysinfo.go || true`
if test "$regs" != ""; then
  regs=`echo $regs | sed -e 's/type _user_regs_struct struct //' -e 's/[{}]//g'`
  regs=`echo $regs | sed -e s'/^ *//'`
  nregs=
  while test -n "$regs"; do
    field=`echo $regs | sed -e 's/^\([^;]*\);.*$/\1/'`
    regs=`echo $regs | sed -e 's/^[^;]*; *\(.*\)$/\1/'`
    # Capitalize the first character of the field.
    f=`echo $field | sed -e 's/^\(.\).*$/\1/'`
    r=`echo $field | sed -e 's/^.\(.*\)$/\1/'`
    f=`echo $f | tr abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ`
    field="$f$r"
    nregs="$nregs $field;"
  done
  echo "type PtraceRegs struct {$nregs }" >> ${OUT}
fi

# Some basic types.
echo 'type Size_t _size_t' >> ${OUT}
echo "type Ssize_t _ssize_t" >> ${OUT}
if grep '^const _HAVE_OFF64_T = ' gen-sysinfo.go > /dev/null 2>&1; then
  echo "type Offset_t _off64_t" >> ${OUT}
else
  echo "type Offset_t _off_t" >> ${OUT}
fi
echo "type Mode_t _mode_t" >> ${OUT}
echo "type Pid_t _pid_t" >> ${OUT}
echo "type Uid_t _uid_t" >> ${OUT}
echo "type Gid_t _gid_t" >> ${OUT}
echo "type Socklen_t _socklen_t" >> ${OUT}

# The long type, needed because that is the type that ptrace returns.
sizeof_long=`grep '^const ___SIZEOF_LONG__ = ' gen-sysinfo.go | sed -e 's/.*= //'`
if test "$sizeof_long" = "4"; then
  echo "type _C_long int32" >> ${OUT}
elif test "$sizeof_long" = "8"; then
  echo "type _C_long int64" >> ${OUT}
else
  echo 1>&2 "mksysinfo.sh: could not determine size of long (got $sizeof_long)"
  exit 1
fi

# Solaris 2 needs _u?pad128_t, but its default definition in terms of long
# double is commented by -fdump-go-spec.
if grep "^// type _pad128_t" gen-sysinfo.go > /dev/null 2>&1; then
  echo "type _pad128_t struct { _l [4]int32; }" >> ${OUT}
fi
if grep "^// type _upad128_t" gen-sysinfo.go > /dev/null 2>&1; then
  echo "type _upad128_t struct { _l [4]uint32; }" >> ${OUT}
fi

# The time structures need special handling: we need to name the
# types, so that we can cast integers to the right types when
# assigning to the structures.
timeval=`grep '^type _timeval ' gen-sysinfo.go`
timeval_sec=`echo $timeval | sed -n -e 's/^.*tv_sec \([^ ]*\);.*$/\1/p'`
timeval_usec=`echo $timeval | sed -n -e 's/^.*tv_usec \([^ ]*\);.*$/\1/p'`
echo "type Timeval_sec_t $timeval_sec" >> ${OUT}
echo "type Timeval_usec_t $timeval_usec" >> ${OUT}
echo $timeval | \
  sed -e 's/type _timeval /type Timeval /' \
      -e 's/tv_sec *[a-zA-Z0-9_]*/Sec Timeval_sec_t/' \
      -e 's/tv_usec *[a-zA-Z0-9_]*/Usec Timeval_usec_t/' >> ${OUT}
timespec=`grep '^type _timespec ' gen-sysinfo.go || true`
if test "$timespec" = ""; then
  # IRIX 6.5 has __timespec instead.
  timespec=`grep '^type ___timespec ' gen-sysinfo.go || true`
fi
timespec_sec=`echo $timespec | sed -n -e 's/^.*tv_sec \([^ ]*\);.*$/\1/p'`
timespec_nsec=`echo $timespec | sed -n -e 's/^.*tv_nsec \([^ ]*\);.*$/\1/p'`
echo "type Timespec_sec_t $timespec_sec" >> ${OUT}
echo "type Timespec_nsec_t $timespec_nsec" >> ${OUT}
echo $timespec | \
  sed -e 's/^type ___timespec /type Timespec /' \
      -e 's/^type _timespec /type Timespec /' \
      -e 's/tv_sec *[a-zA-Z0-9_]*/Sec Timespec_sec_t/' \
      -e 's/tv_nsec *[a-zA-Z0-9_]*/Nsec Timespec_nsec_t/' >> ${OUT}

timestruc=`grep '^type _timestruc_t ' gen-sysinfo.go || true`
if test "$timestruc" != ""; then
  timestruc_sec=`echo $timestruc | sed -n -e 's/^.*tv_sec \([^ ]*\);.*$/\1/p'`
  timestruc_nsec=`echo $timestruc | sed -n -e 's/^.*tv_nsec \([^ ]*\);.*$/\1/p'`
  echo "type Timestruc_sec_t $timestruc_sec" >> ${OUT}
  echo "type Timestruc_nsec_t $timestruc_nsec" >> ${OUT}
  echo $timestruc | \
    sed -e 's/^type _timestruc_t /type Timestruc /' \
        -e 's/tv_sec *[a-zA-Z0-9_]*/Sec Timestruc_sec_t/' \
        -e 's/tv_nsec *[a-zA-Z0-9_]*/Nsec Timestruc_nsec_t/' >> ${OUT}
fi

# The stat type.
# Prefer largefile variant if available.
stat=`grep '^type _stat64 ' gen-sysinfo.go || true`
if test "$stat" != ""; then
  grep '^type _stat64 ' gen-sysinfo.go
else
  grep '^type _stat ' gen-sysinfo.go
fi | sed -e 's/type _stat64/type Stat_t/' \
         -e 's/type _stat/type Stat_t/' \
         -e 's/st_dev/Dev/' \
         -e 's/st_ino/Ino/g' \
         -e 's/st_nlink/Nlink/' \
         -e 's/st_mode/Mode/' \
         -e 's/st_uid/Uid/' \
         -e 's/st_gid/Gid/' \
         -e 's/st_rdev/Rdev/' \
         -e 's/st_size/Size/' \
         -e 's/st_blksize/Blksize/' \
         -e 's/st_blocks/Blocks/' \
         -e 's/st_atim/Atime/' \
         -e 's/st_mtim/Mtime/' \
         -e 's/st_ctim/Ctime/' \
         -e 's/\([^a-zA-Z0-9_]\)_timeval\([^a-zA-Z0-9_]\)/\1Timeval\2/g' \
         -e 's/\([^a-zA-Z0-9_]\)_timespec_t\([^a-zA-Z0-9_]\)/\1Timespec\2/g' \
         -e 's/\([^a-zA-Z0-9_]\)_timespec\([^a-zA-Z0-9_]\)/\1Timespec\2/g' \
         -e 's/\([^a-zA-Z0-9_]\)_timestruc_t\([^a-zA-Z0-9_]\)/\1Timestruc\2/g' \
         -e 's/Godump_[0-9] struct { \([^;]*;\) };/\1/g' \
       >> ${OUT}

# The directory searching types.
# Prefer largefile variant if available.
dirent=`grep '^type _dirent64 ' gen-sysinfo.go || true`
if test "$dirent" != ""; then
  grep '^type _dirent64 ' gen-sysinfo.go
else
  grep '^type _dirent ' gen-sysinfo.go
fi | sed -e 's/type _dirent64/type Dirent/' \
         -e 's/type _dirent/type Dirent/' \
         -e 's/d_name \[0+1\]/d_name [0+256]/' \
         -e 's/d_name/Name/' \
         -e 's/]int8/]byte/' \
         -e 's/d_ino/Ino/' \
         -e 's/d_off/Off/' \
         -e 's/d_reclen/Reclen/' \
         -e 's/d_type/Type/' \
      >> ${OUT}
echo "type DIR _DIR" >> ${OUT}

# The rusage struct.
rusage=`grep '^type _rusage struct' gen-sysinfo.go`
if test "$rusage" != ""; then
  rusage=`echo $rusage | sed -e 's/type _rusage struct //' -e 's/[{}]//g'`
  rusage=`echo $rusage | sed -e 's/^ *//'`
  nrusage=
  while test -n "$rusage"; do
    field=`echo $rusage | sed -e 's/^\([^;]*\);.*$/\1/'`
    rusage=`echo $rusage | sed -e 's/^[^;]*; *\(.*\)$/\1/'`
    # Drop the leading ru_, capitalize the next character.
    field=`echo $field | sed -e 's/^ru_//'`
    f=`echo $field | sed -e 's/^\(.\).*$/\1/'`
    r=`echo $field | sed -e 's/^.\(.*\)$/\1/'`
    f=`echo $f | tr abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ`
    # Fix _timeval _timespec, and _timestruc_t.
    r=`echo $r | sed -e s'/ _timeval$/ Timeval/'`
    r=`echo $r | sed -e s'/ _timespec$/ Timespec/'`
    r=`echo $r | sed -e s'/ _timestruc_t$/ Timestruc/'`
    field="$f$r"
    nrusage="$nrusage $field;"
  done
  echo "type Rusage struct {$nrusage }" >> ${OUT}
else
  echo "type Rusage struct {}" >> ${OUT}
fi

# The RUSAGE constants.
grep '^const _RUSAGE_' gen-sysinfo.go | \
  sed -e 's/^\(const \)_\(RUSAGE_[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}

# The utsname struct.
grep '^type _utsname ' gen-sysinfo.go | \
    sed -e 's/_utsname/Utsname/' \
      -e 's/sysname/Sysname/' \
      -e 's/nodename/Nodename/' \
      -e 's/release/Release/' \
      -e 's/version/Version/' \
      -e 's/machine/Machine/' \
      -e 's/domainname/Domainname/' \
    >> ${OUT}

# The iovec struct.
iovec=`grep '^type _iovec ' gen-sysinfo.go`
iovec_len=`echo $iovec | sed -n -e 's/^.*iov_len \([^ ]*\);.*$/\1/p'`
echo "type Iovec_len_t $iovec_len" >> ${OUT}
echo $iovec | \
    sed -e 's/_iovec/Iovec/' \
      -e 's/iov_base/Base/' \
      -e 's/iov_len *[a-zA-Z0-9_]*/Len Iovec_len_t/' \
    >> ${OUT}

# The msghdr struct.
msghdr=`grep '^type _msghdr ' gen-sysinfo.go`
msghdr_controllen=`echo $msghdr | sed -n -e 's/^.*msg_controllen \([^ ]*\);.*$/\1/p'`
echo "type Msghdr_controllen_t $msghdr_controllen" >> ${OUT}
echo $msghdr | \
    sed -e 's/_msghdr/Msghdr/' \
      -e 's/msg_name/Name/' \
      -e 's/msg_namelen/Namelen/' \
      -e 's/msg_iov/Iov/' \
      -e 's/msg_iovlen/Iovlen/' \
      -e 's/_iovec/Iovec/' \
      -e 's/msg_control/Control/' \
      -e 's/msg_controllen *[a-zA-Z0-9_]*/Controllen Msghdr_controllen_t/' \
      -e 's/msg_flags/Flags/' \
    >> ${OUT}

# The ip_mreq struct.
grep '^type _ip_mreq ' gen-sysinfo.go | \
    sed -e 's/_ip_mreq/IPMreq/' \
      -e 's/imr_multiaddr/Multiaddr/' \
      -e 's/imr_interface/Interface/' \
      -e 's/_in_addr/[4]byte/g' \
    >> ${OUT}

# We need IPMreq to compile the net package.
if ! grep 'type IPMreq ' ${OUT} >/dev/null 2>&1; then
  echo 'type IPMreq struct { Multiaddr [4]byte; Interface [4]byte; }' >> ${OUT}
fi

# The size of the ip_mreq struct.
echo 'var SizeofIPMreq = int(unsafe.Sizeof(IPMreq{}))' >> ${OUT}

# The ipv6_mreq struct.
grep '^type _ipv6_mreq ' gen-sysinfo.go | \
    sed -e 's/_ipv6_mreq/IPv6Mreq/' \
      -e 's/ipv6mr_multiaddr/Multiaddr/' \
      -e 's/ipv6mr_interface/Interface/' \
      -e 's/_in6_addr/[16]byte/' \
    >> ${OUT}

# We need IPv6Mreq to compile the net package.
if ! grep 'type IPv6Mreq ' ${OUT} >/dev/null 2>&1; then
  echo 'type IPv6Mreq struct { Multiaddr [16]byte; Interface uint32; }' >> ${OUT}
fi

# Try to guess the type to use for fd_set.
fd_set=`grep '^type _fd_set ' gen-sysinfo.go || true`
fds_bits_type="_C_long"
if test "$fd_set" != ""; then
    fds_bits_type=`echo $fd_set | sed -e 's/.*[]]\([^;]*\); }$/\1/'`
fi
echo "type fds_bits_type $fds_bits_type" >> ${OUT}

# The addrinfo struct.
grep '^type _addrinfo ' gen-sysinfo.go | \
    sed -e 's/_addrinfo/Addrinfo/g' \
      -e 's/ ai_/ Ai_/g' \
    >> ${OUT}

# The addrinfo flags and errors.
grep '^const _AI_' gen-sysinfo.go | \
  sed -e 's/^\(const \)_\(AI_[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}
grep '^const _EAI_' gen-sysinfo.go | \
  sed -e 's/^\(const \)_\(EAI_[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}

# The passwd struct.
grep '^type _passwd ' gen-sysinfo.go | \
    sed -e 's/_passwd/Passwd/' \
      -e 's/ pw_/ Pw_/g' \
    >> ${OUT}

# The ioctl flags for the controlling TTY.
grep '^const _TIOC' gen-sysinfo.go | \
    grep -v '_val =' | \
    sed -e 's/^\(const \)_\(TIOC[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}
# We need TIOCGWINSZ.
if ! grep '^const TIOCGWINSZ' ${OUT} >/dev/null 2>&1; then
  if grep '^const _TIOCGWINSZ_val' ${OUT} >/dev/null 2>&1; then
    echo 'const TIOCGWINSZ = _TIOCGWINSZ_val' >> ${OUT}
  fi
fi

# The ioctl flags for terminal control
grep '^const _TC[GS]ET' gen-sysinfo.go | \
    sed -e 's/^\(const \)_\(TC[GS]ET[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}

# ioctl constants.  Might fall back to 0 if TIOCNXCL is missing, too, but
# needs handling in syscalls.exec.go.
if ! grep '^const _TIOCSCTTY ' gen-sysinfo.go >/dev/null 2>&1; then
  if grep '^const _TIOCNXCL ' gen-sysinfo.go >/dev/null 2>&1; then
    echo "const TIOCSCTTY = TIOCNXCL" >> ${OUT}
  fi
fi

# The nlmsghdr struct.
grep '^type _nlmsghdr ' gen-sysinfo.go | \
    sed -e 's/_nlmsghdr/NlMsghdr/' \
      -e 's/nlmsg_len/Len/' \
      -e 's/nlmsg_type/Type/' \
      -e 's/nlmsg_flags/Flags/' \
      -e 's/nlmsg_seq/Seq/' \
      -e 's/nlmsg_pid/Pid/' \
    >> ${OUT}

# The nlmsg flags and operators.
grep '^const _NLM' gen-sysinfo.go | \
    sed -e 's/^\(const \)_\(NLM[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}

# NLMSG_HDRLEN is defined as an expression using sizeof.
if ! grep '^const NLMSG_HDRLEN' ${OUT} > /dev/null 2>&1; then
  if grep '^type NlMsghdr ' ${OUT} > /dev/null 2>&1; then
    echo 'var NLMSG_HDRLEN = int((unsafe.Sizeof(NlMsghdr{}) + (NLMSG_ALIGNTO-1)) &^ (NLMSG_ALIGNTO-1))' >> ${OUT}
  fi
fi

# The rtmsg struct.
grep '^type _rtmsg ' gen-sysinfo.go | \
    sed -e 's/_rtmsg/RtMsg/' \
      -e 's/rtm_family/Family/' \
      -e 's/rtm_dst_len/Dst_len/' \
      -e 's/rtm_src_len/Src_len/' \
      -e 's/rtm_tos/Tos/' \
      -e 's/rtm_table/Table/' \
      -e 's/rtm_protocol/Procotol/' \
      -e 's/rtm_scope/Scope/' \
      -e 's/rtm_type/Type/' \
      -e 's/rtm_flags/Flags/' \
    >> ${OUT}

# The size of the rtmsg struct.
if grep 'type RtMsg ' ${OUT} > /dev/null 2>&1; then
  echo 'var SizeofRtMsg = int(unsafe.Sizeof(RtMsg{}))' >> ${OUT}
fi

# The rtgenmsg struct.
grep '^type _rtgenmsg ' gen-sysinfo.go | \
    sed -e 's/_rtgenmsg/RtGenmsg/' \
      -e 's/rtgen_family/Family/' \
    >> ${OUT}

# The routing message flags.
grep '^const _RTA' gen-sysinfo.go | \
    sed -e 's/^\(const \)_\(RTA[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}
grep '^const _RTM' gen-sysinfo.go | \
    sed -e 's/^\(const \)_\(RTM[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}

# The size of the rtgenmsg struct.
if grep 'type RtGenmsg ' ${OUT} > /dev/null 2>&1; then
  echo 'var SizeofRtGenmsg = int(unsafe.Sizeof(RtGenmsg{}))' >> ${OUT}
fi

# The ifinfomsg struct.
grep '^type _ifinfomsg ' gen-sysinfo.go | \
    sed -e 's/_ifinfomsg/IfInfomsg/' \
      -e 's/ifi_family/Family/' \
      -e 's/ifi_type/Type/' \
      -e 's/ifi_index/Index/' \
      -e 's/ifi_flags/Flags/' \
      -e 's/ifi_change/Change/' \
    >> ${OUT}

# The interface information types and flags.
grep '^const _IFA' gen-sysinfo.go | \
    sed -e 's/^\(const \)_\(IFA[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}
grep '^const _IFLA' gen-sysinfo.go | \
    sed -e 's/^\(const \)_\(IFLA[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}
grep '^const _IFF' gen-sysinfo.go | \
    sed -e 's/^\(const \)_\(IFF[^= ]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}

# The size of the ifinfomsg struct.
if grep 'type IfInfomsg ' ${OUT} > /dev/null 2>&1; then
  echo 'var SizeofIfInfomsg = int(unsafe.Sizeof(IfInfomsg{}))' >> ${OUT}
fi

# The ifaddrmsg struct.
grep '^type _ifaddrmsg ' gen-sysinfo.go | \
    sed -e 's/_ifaddrmsg/IfAddrmsg/' \
      -e 's/ifa_family/Family/' \
      -e 's/ifa_prefixlen/Prefixlen/' \
      -e 's/ifa_flags/Flags/' \
      -e 's/ifa_scope/Scope/' \
      -e 's/ifa_index/Index/' \
    >> ${OUT}

# The size of the ifaddrmsg struct.
if grep 'type IfAddrmsg ' ${OUT} > /dev/null 2>&1; then
  echo 'var SizeofIfAddrmsg = int(unsafe.Sizeof(IfAddrmsg{}))' >> ${OUT}
fi

# The rtattr struct.
grep '^type _rtattr ' gen-sysinfo.go | \
    sed -e 's/_rtattr/RtAttr/' \
      -e 's/rta_len/Len/' \
      -e 's/rta_type/Type/' \
    >> ${OUT}

# The size of the rtattr struct.
if grep 'type RtAttr ' ${OUT} > /dev/null 2>&1; then
  echo 'var SizeofRtAttr = int(unsafe.Sizeof(RtAttr{}))' >> ${OUT}
fi

# The termios struct.
grep '^type _termios ' gen-sysinfo.go | \
    sed -e 's/_termios/Termios/' \
      -e 's/c_iflag/Iflag/' \
      -e 's/c_oflag/Oflag/' \
      -e 's/c_cflag/Cflag/' \
      -e 's/c_lflag/Lflag/' \
      -e 's/c_line/Line/' \
      -e 's/c_cc/Cc/' \
      -e 's/c_ispeed/Ispeed/' \
      -e 's/c_ospeed/Ospeed/' \
    >> ${OUT}

# The termios constants.
for n in IGNBRK BRKINT IGNPAR PARMRK INPCK ISTRIP INLCR IGNCR ICRNL IUCLC \
    IXON IXANY IXOFF IMAXBEL IUTF8 OPOST OLCUC ONLCR OCRNL ONOCR ONLRET \
    OFILL OFDEL NLDLY NL0 NL1 CRDLY CR0 CR1 CR2 CR3 TABDLY BSDLY VTDLY \
    FFDLY CBAUD CBAUDEX CSIZE CSTOPB CREAD PARENB PARODD HUPCL CLOCAL \
    LOBLK CIBAUD CMSPAR CRTSCTS ISIG ICANON XCASE ECHO ECHOE ECHOK ECHONL \
    ECHOCTL ECHOPRT ECHOKE DEFECHO FLUSHO NOFLSH TOSTOP PENDIN IEXTEN VINTR \
    VQUIT VERASE VKILL VEOF VMIN VEOL VTIME VEOL2 VSWTCH VSTART VSTOP VSUSP \
    VDSUSP VLNEXT VWERASE VREPRINT VDISCARD VSTATUS TCSANOW TCSADRAIN \
    TCSAFLUSH TCIFLUSH TCOFLUSH TCIOFLUSH TCOOFF TCOON TCIOFF TCION B0 B50 \
    B75 B110 B134 B150 B200 B300 B600 B1200 B1800 B2400 B4800 B9600 B19200 \
    B38400 B57600 B115200 B230400; do

    grep "^const _$n " gen-sysinfo.go | \
	sed -e 's/^\(const \)_\([^=]*\)\(.*\)$/\1\2 = _\2/' >> ${OUT}
done

exit $?
