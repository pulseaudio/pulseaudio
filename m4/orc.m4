dnl pkg-config-based checks for Orc

dnl specific:
dnl ORC_CHECK([REQUIRED_VERSION])

AC_DEFUN([ORC_CHECK],
[
  ORC_REQ=ifelse([$1], , "0.4.5", [$1])

  AC_ARG_ENABLE(orc,
  AC_HELP_STRING([--enable-orc],[use Orc if installed]),
  [case "${enableval}" in
    auto) enable_orc=auto ;;
    yes) enable_orc=yes ;;
    no)  enable_orc=no ;;
    *) AC_MSG_ERROR(bad value ${enableval} for --enable-orc) ;;
  esac
  ],
  [enable_orc=auto]) dnl Default value

  if test "x$enable_orc" != "xno" ; then
    PKG_CHECK_MODULES(ORC, orc-0.4 >= $ORC_REQ, [
      AC_DEFINE(HAVE_ORC, 1, [Use Orc])
      ORCC=`$PKG_CONFIG --variable=orcc orc-0.4`
      AC_SUBST(ORCC)
      HAVE_ORC=yes
    ], [
      if test "x$enable_orc" = "xyes" ; then
        AC_MSG_ERROR([--enable-orc specified, but Orc >= $ORC_REQ not found])
      fi
      AC_DEFINE(DISABLE_ORC, 1, [Disable Orc])
      HAVE_ORC=no
    ])
  else
    AC_DEFINE(DISABLE_ORC, 1, [Disable Orc])
    HAVE_ORC=no
  fi
  AM_CONDITIONAL(HAVE_ORC, test "x$HAVE_ORC" = "xyes")

]))

AC_DEFUN([ORC_OUTPUT],
[
  if test "$HAVE_ORC" = yes ; then
    printf "configure: *** Orc acceleration enabled.\n"
  else
    if test "x$enable_orc" = "xno" ; then
      printf "configure: *** Orc acceleration disabled by --disable-orc.  Slower code paths\n"
      printf "               will be used.\n"
    else
      printf "configure: *** Orc acceleration disabled.  Requires Orc >= $ORC_REQ, which was\n"
      printf "               not found.  Slower code paths will be used.\n"
    fi
  fi
  printf "\n"
])
