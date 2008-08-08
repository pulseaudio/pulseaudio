AC_DEFUN([CC_CHECK_TLS], [
  AC_CACHE_CHECK([whether $CC knows __thread for Thread-Local Storage],
    cc_cv_tls___thread,
    [AC_COMPILE_IFELSE(
      AC_LANG_PROGRAM(
        [[static __thread int a = 6;]],
        [[a = 5;]]),
      [cc_cv_tls___thread=yes],
      [cc_cv_tls___thread=no])
    ])
  
  AS_IF([test "x$cc_cv_tls___thread" = "xyes"],
    [AC_DEFINE([SUPPORT_TLS___THREAD], 1,
     [Define this if the compiler supports __thread for Thread-Local Storage])
     $1],
    [$2])
])
