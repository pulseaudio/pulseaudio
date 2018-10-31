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
  include_directories : [configinc, topinc],
  link_with : [libpulsecore, libpulsecommon, libpulse],
  dependencies : [ltdl_dep, cap_dep, dbus_dep, systemd_dep],
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
  install : true,
  install_dir : pulsesysconfdir,
)

default_conf = configuration_data()
default_conf.merge_from(cdata)
default_conf.set('PA_BINARY', cdata.get_unquoted('PA_BINARY'))
default_conf.set('PA_SOEXT', cdata.get_unquoted('PA_SOEXT'))
default_conf.set10('HAVE_AF_UNIX', cc.has_header('sys/un.h'))

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
  install : true,
  install_dir : pulsesysconfdir,
)
