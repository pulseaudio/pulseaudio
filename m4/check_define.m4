AC_DEFUN([AC_CHECK_DEFINE],[
AS_VAR_PUSHDEF([ac_var],[ac_cv_defined_$1_$2])dnl
AC_CACHE_CHECK([for $1 in $2], ac_var,
AC_TRY_COMPILE([#include <$2>],[
  #ifdef $1
  int ok;
  #else
  choke me
  #endif
],AS_VAR_SET(ac_var, yes),AS_VAR_SET(ac_var, no)))
AS_IF([test AS_VAR_GET(ac_var) != "no"], [$3], [$4])dnl
AS_VAR_POPDEF([ac_var])dnl
])
