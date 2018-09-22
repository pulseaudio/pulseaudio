libbluez5_util_sources = [
  'bluez5-util.c',
]

libbluez5_util_headers = [
  'a2dp-codecs.h',
  'bluez5-util.h',
  'rtp.h',
]

if bluez_dep.found()
  libbluez5_util_sources += [ 'backend-native.c' ]
endif

# TODO This should be optional
libbluez5_util_sources += [ 'backend-ofono.c' ]

libbluez5_util = shared_library('libbluez5_util',
  libbluez5_util_sources,
  libbluez5_util_headers,
  c_args : [pa_c_args, server_c_args],
  include_directories : [configinc, topinc],
  dependencies : [libpulse_dep, libpulsecommon_dep, libpulsecore_dep, dbus_dep],
  install : true
)
