#ifndef foocdeclhfoo
#define foocdeclhfoo

/** \file
 * C++ compatibility support */

#ifdef __cplusplus
/** If using C++ this macro enables C mode, otherwise does nothing */
#define PA_C_DECL_BEGIN extern "C" {
/** If using C++ this macros switches back to C++ mode, otherwise does nothing */
#define PA_C_DECL_END }

#else
/** If using C++ this macro enables C mode, otherwise does nothing */
#define PA_C_DECL_BEGIN
/** If using C++ this macros switches back to C++ mode, otherwise does nothing */
#define PA_C_DECL_END

#endif

#endif
