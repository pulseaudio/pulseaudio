# GSettings helper

gsettings_helper_sources = [
  'gsettings-helper.c',
]

gsettings_helper = executable('gsettings-helper',
  gsettings_helper_sources,
  c_args : pa_c_args,
  include_directories : [configinc, topinc],
  link_with : [libpulsecommon, libpulse],
  dependencies : [gio_dep],
  install_dir : pulselibexecdir,
  install_rpath : privlibdir,
  install : true,
)

# GSettings schemas

compile_schemas = find_program('glib-compile-schemas', required : false)
if compile_schemas.found()
  test('Validate schema files in ' + meson.current_source_dir(),
    compile_schemas,
    args: ['--strict', '--dry-run', meson.current_source_dir()]
  )
endif

install_data('org.freedesktop.pulseaudio.gschema.xml',
  install_dir : join_paths(datadir, 'glib-2.0', 'schemas')
)

meson.add_install_script('meson_post_install.py', datadir)

# Conversion from GConf to GSettings

install_data('pulseaudio.convert',
  install_dir : join_paths(datadir, 'GConf', 'gsettings')
)
