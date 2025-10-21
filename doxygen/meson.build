doxygen = find_program('doxygen', required: get_option('doxygen'))
if not doxygen.found()
  subdir_done()
endif

cdata.set('DOXYGEN_OUTPUT_DIRECTORY', meson.current_build_dir())

doxygen_conf = configure_file(
  input : 'doxygen.conf.in',
  output : 'doxygen.conf',
  configuration : cdata,
)

run_target('doxygen',
  command : [doxygen, doxygen_conf])
