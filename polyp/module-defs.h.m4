dnl $Id$
changecom(`/*', `*/')dnl
define(`module', patsubst(patsubst(fname, `-symdef.h$'), `[^0-9a-zA-Z]', `_'))dnl
define(`c_symbol', patsubst(module, `[^0-9a-zA-Z]', `_'))dnl
define(`c_macro', patsubst(module, `[^0-9a-zA-Z]', `'))dnl
define(`incmacro', `foo'c_macro`symdeffoo')dnl
define(`gen_symbol', `#define $1 'module`_LTX_$1')dnl
#ifndef incmacro
#define incmacro

gen_symbol(pa__init)
gen_symbol(pa__done)
gen_symbol(pa__get_author)
gen_symbol(pa__get_description)
gen_symbol(pa__get_usage)
gen_symbol(pa__get_version)

int pa__init(struct pa_core *c, struct pa_module*m);
void pa__done(struct pa_core *c, struct pa_module*m);

#endif
