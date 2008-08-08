AC_DEFUN([ACX_LIRC], [
LIRC_CFLAGS=
LIRC_LIBS=
AC_CHECK_HEADER(lirc/lirc_client.h,[AC_CHECK_LIB(lirc_client,lirc_init,[HAVE_LIRC=1
LIRC_LIBS=-llirc_client],HAVE_LIRC=0)],HAVE_LIRC=0)
])
