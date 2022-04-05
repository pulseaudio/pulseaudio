libbluez5_util_sources = [
  'a2dp-codec-sbc.c',
  'a2dp-codec-util.c',
  'bluez5-util.c',
  'bt-codec-cvsd.c',
  'bt-codec-msbc.c',
]

libbluez5_util_headers = [
  'a2dp-codec-api.h',
  'a2dp-codecs.h',
  'a2dp-codec-util.h',
  'bluez5-util.h',
  'rtp.h',
]

if get_option('bluez5-native-headset')
  libbluez5_util_sources += [ 'backend-native.c' ]
  libbluez5_util_sources += [ 'upower.c' ]
  libbluez5_util_headers += [ 'upower.h' ]
endif

if get_option('bluez5-ofono-headset')
  libbluez5_util_sources += [ 'backend-ofono.c' ]
endif

if have_bluez5_gstreamer
  libbluez5_util_headers += [ 'a2dp-codec-gst.h' ]
  libbluez5_util_sources += [ 'a2dp-codec-gst.c' ]
  libbluez5_util_sources += [ 'a2dp-codec-ldac-gst.c' ]
  libbluez5_util_sources += [ 'a2dp-codec-aptx-gst.c' ]
endif

libbluez5_util = shared_library('bluez5-util',
  libbluez5_util_sources,
  libbluez5_util_headers,
  c_args : [pa_c_args, server_c_args],
  link_args : [nodelete_link_args],
  include_directories : [configinc, topinc],
  dependencies : [libpulse_dep, libpulsecommon_dep, libpulsecore_dep, bluez_dep, dbus_dep, sbc_dep, libintl_dep, bluez5_gst_dep, bluez5_gstapp_dep, libm_dep],
  install : true,
  install_rpath : privlibdir,
  install_dir : modlibexecdir,
)
