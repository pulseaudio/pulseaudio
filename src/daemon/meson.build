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

# FIXME: man pages, dependencies
executable('pulseaudio',
  pulseaudio_sources,
  pulseaudio_headers,
  install: true,
  include_directories : [configinc, topinc],
  link_with : [libpulsecore, libpulsecommon, libpulse],
  dependencies : [ltdl_dep, cap_dep, dbus_dep],
  c_args : pa_c_args,
)
