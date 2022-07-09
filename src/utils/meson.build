if get_option('client')
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

  # Windows doesn't support symbolic links.
  if host_machine.system() != 'windows'
    foreach alias : pacat_aliases
      dst = join_paths(bindir, alias)
      cmd = 'ln -fs @0@ $DESTDIR@1@'.format('pacat', dst)
      meson.add_install_script('sh', '-c', cmd)
    endforeach
  endif

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
endif

if get_option('daemon')
  if host_machine.system() != 'windows'
    pasuspender_sources = [
      'pasuspender.c',
    ]

    executable('pasuspender',
      pasuspender_sources,
      install: true,
      install_rpath : privlibdir,
      include_directories : [configinc, topinc],
      dependencies: [libintl_dep, libpulsecommon_dep, libpulse_dep],
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
      dependencies: [libintl_dep, libpulsecommon_dep, libpulse_dep],
      c_args : pa_c_args,
    )
  endif

  if dbus_dep.found() and fftw_dep.found()
    install_data('qpaeq', install_dir : bindir)
  endif
endif

if get_option('client')
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

  if cdata.has('HAVE_OSS_WRAPPER')
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

  install_data('pa-info', install_dir : bindir)
endif
