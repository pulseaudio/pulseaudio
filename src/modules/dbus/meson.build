libdbus_util_sources = [
  'iface-card.c',
  'iface-card-profile.c',
  'iface-client.c',
  'iface-core.c',
  'iface-device.c',
  'iface-device-port.c',
  'iface-memstats.c',
  'iface-module.c',
  'iface-sample.c',
  'iface-stream.c',
]

libdbus_util_headers = [
  'iface-card.h',
  'iface-card-profile.h',
  'iface-client.h',
  'iface-core.h',
  'iface-device.h',
  'iface-device-port.h',
  'iface-memstats.h',
  'iface-module.h',
  'iface-sample.h',
  'iface-stream.h',
]

libdbus_util = shared_library('libdbus_util',
  libdbus_util_sources,
  libdbus_util_headers,
  c_args : [pa_c_args, server_c_args],
  include_directories : [configinc, topinc],
  dependencies : [libpulse_dep, libpulsecommon_dep, libpulsecore_dep, dbus_dep],
  install : true
)
