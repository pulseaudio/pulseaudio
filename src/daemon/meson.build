pulseaudio_sources = [
  'caps.c',
  'cmdline.c',
  'cpulimit.c',
  'daemon-conf.c',
  'dumpmodules.c',
  'ltdl-bind-now.c',
  'main.c',
]

pulseaudio_headers = [
  'caps.h',
  'cmdline.h',
  'cpulimit.h',
  'daemon-conf.h',
  'dumpmodules.h',
  'ltdl-bind-now.h',
]

if dbus_dep.found()
  pulseaudio_sources += 'server-lookup.c'
  pulseaudio_headers += 'server-lookup.h'
endif

# FIXME: dependencies
executable('pulseaudio',
  pulseaudio_sources,
  pulseaudio_headers,
  install: true,
  install_rpath : privlibdir,
  include_directories : [configinc, topinc],
  link_args : ['-ffast-math'],
  link_with : [libpulsecore, libpulsecommon, libpulse],
  dependencies : [ltdl_dep, cap_dep, dbus_dep, libsystemd_dep, dl_dep, libintl_dep],
  c_args : pa_c_args,
)

if x11_dep.found()
  conf = configuration_data()
  conf.set('PACTL_BINARY', join_paths(bindir, 'pactl'))

  configure_file(
    input : 'start-pulseaudio-x11.in',
    output : 'start-pulseaudio-x11',
    configuration : conf,
    install : true,
    install_dir : bindir,
  )

  desktop_file = i18n.merge_file(
    input : 'pulseaudio.desktop.in',
    output : 'pulseaudio.desktop',
    po_dir : po_dir,
    type : 'desktop',
    install : true,
    install_dir : join_paths(sysconfdir, 'xdg', 'autostart'),
  )

  desktop_utils = find_program('desktop-file-validate', required: false)
  if desktop_utils.found()
    test('Validate desktop file', desktop_utils,
      args: [ desktop_file ],
    )
  endif
endif

# Configuration files

m4 = find_program('m4', required: true)

daemon_conf = configuration_data()
daemon_conf.merge_from(cdata)
daemon_conf.set('PA_DEFAULT_CONFIG_DIR', cdata.get_unquoted('PA_DEFAULT_CONFIG_DIR'))

daemon_template_file = configure_file(
  input : 'daemon.conf.in',
  output : 'daemon.conf.tmp',
  configuration : daemon_conf,
)

custom_target('daemon.conf',
  input : daemon_template_file,
  output : 'daemon.conf',
  capture : true,
  command : [m4, '@INPUT@'],
  build_by_default : true,
  install : true,
  install_dir : pulsesysconfdir,
)

default_conf = configuration_data()
default_conf.merge_from(cdata)
default_conf.set('PA_BINARY', cdata.get_unquoted('PA_BINARY'))
default_conf.set('PA_SOEXT', cdata.get_unquoted('PA_SOEXT'))
default_conf.set10('HAVE_AF_UNIX', cc.has_header('sys/un.h'))
default_conf.set10('OS_IS_WIN32', host_machine.system() == 'windows')
default_conf.set10('HAVE_MKFIFO', cc.has_function('mkfifo'))
# We don't support the deprecated GConf option in meson
default_conf.set10('HAVE_GCONF', 0)

default_template_file = configure_file(
  input : 'default.pa.in',
  output : 'default.pa.tmp',
  configuration : default_conf,
)

custom_target('default.pa',
  input : default_template_file,
  output : 'default.pa',
  capture : true,
  command : [m4, '@INPUT@'],
  build_by_default : true,
  install : true,
  install_dir : pulsesysconfdir,
)

system_conf = configuration_data()
system_conf.merge_from(cdata)
system_conf.set('PA_BINARY', cdata.get_unquoted('PA_BINARY'))
system_conf.set('PA_SOEXT', cdata.get_unquoted('PA_SOEXT'))

system_template_file = configure_file(
  input : 'system.pa.in',
  output : 'system.pa.tmp',
  configuration : system_conf,
)

custom_target('system.pa',
  input : system_template_file,
  output : 'system.pa',
  capture : true,
  command : [m4, '@INPUT@'],
  build_by_default : true,
  install : true,
  install_dir : pulsesysconfdir,
)

if dbus_dep.found()
  install_data('pulseaudio-system.conf',
    install_dir : join_paths(sysconfdir, 'dbus-1', 'system.d')
  )
endif

if systemd_dep.found()
  sd_user_service_conf = configuration_data()
  sd_user_service_conf.set('PA_BINARY', cdata.get_unquoted('PA_BINARY'))

  sd_user_service_file = configure_file(
    input : 'systemd/user/pulseaudio.service.in',
    output : 'pulseaudio.service',
    configuration : sd_user_service_conf,
    install : true,
    install_dir : systemduserunitdir,
  )

  install_data('systemd/user/pulseaudio.socket',
    install_dir: systemduserunitdir,
  )
endif
