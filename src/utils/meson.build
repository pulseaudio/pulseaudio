pacat_sources = [
  'pacat.c',
]

pacat_aliases = [
  'pamon',
  'paplay',
  'parec',
  'parecord',
]

executable('pacat',
  pacat_sources,
  install: true,
  install_rpath : privlibdir,
  include_directories : [configinc, topinc],
  link_with : [libpulsecommon, libpulse],
  dependencies : [sndfile_dep, libintl_dep],
  c_args : pa_c_args,
)

foreach alias : pacat_aliases
  # FIXME How to handle extension (.exe on windows)?
  dst = join_paths(bindir, alias)
  cmd = 'ln -fs @0@ $DESTDIR@1@'.format('pacat', dst)
  meson.add_install_script('sh', '-c', cmd)
endforeach

pactl_sources = [
  'pactl.c',
]

executable('pactl',
  pactl_sources,
  install: true,
  install_rpath : privlibdir,
  include_directories : [configinc, topinc],
  link_with : [libpulsecommon, libpulse],
  dependencies : [sndfile_dep, libintl_dep],
  c_args : pa_c_args,
)

pasuspender_sources = [
  'pasuspender.c',
]

executable('pasuspender',
  pasuspender_sources,
  install: true,
  install_rpath : privlibdir,
  include_directories : [configinc, topinc],
  link_with : [libpulsecommon, libpulse],
  dependencies: [libintl_dep],
  c_args : pa_c_args,
)

pacmd_sources = [
  'pacmd.c',
]

executable('pacmd',
  pacmd_sources,
  install: true,
  install_rpath : privlibdir,
  include_directories : [configinc, topinc],
  link_with : [libpulsecommon, libpulse],
  dependencies: [libintl_dep],
  c_args : pa_c_args,
)

if x11_dep.found()
  pax11publish_sources = [
    'pax11publish.c',
  ]

  executable('pax11publish',
    pax11publish_sources,
    install: true,
    install_rpath : privlibdir,
    include_directories : [configinc, topinc],
    link_with : [libpulsecommon, libpulse],
    dependencies : [x11_dep, libintl_dep],
    c_args : pa_c_args,
  )
endif

if cc.has_header('sys/soundcard.h')
  libpulsecommon_sources = [
    'padsp.c',
  ]

  libpulsedsp = shared_library('pulsedsp',
    libpulsecommon_sources,
    install: true,
    install_dir : padsplibdir,
    install_rpath : privlibdir,
    include_directories : [configinc, topinc],
    link_with : [libpulsecommon, libpulse],
    link_args : [nodelete_link_args],
    dependencies: [thread_dep, dl_dep],
    c_args : [pa_c_args, '-Wno-nonnull-compare']
  )

  configure_file(
    input : 'padsp.in',
    output : 'padsp',
    configuration : cdata,
    install : true,
    install_dir : bindir,
  )
endif

if dbus_dep.found() and fftw_dep.found()
  install_data('qpaeq', install_dir : bindir)
endif

install_data('pa-info', install_dir : bindir)
