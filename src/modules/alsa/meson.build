libalsa_util_sources = [
  'alsa-util.c',
  'alsa-ucm.c',
  'alsa-mixer.c',
  'alsa-sink.c',
  'alsa-source.c',
  '../reserve-wrap.c',
]

libalsa_util_headers = [
  'alsa-util.h',
  'alsa-ucm.h',
  'alsa-mixer.h',
  'alsa-sink.h',
  'alsa-source.h',
  '../reserve-wrap.h',
]

if dbus_dep.found()
    libalsa_util_sources += [ '../reserve.c', '../reserve-monitor.c' ]
    libalsa_util_headers += [ '../reserve.h', '../reserve-monitor.h' ]
endif

if udev_dep.found()
  libalsa_util_sources += [ '../udev-util.c' ]
  libalsa_util_headers += [ '../udev-util.h' ]
endif

libalsa_util = shared_library('alsa-util',
  libalsa_util_sources,
  libalsa_util_headers,
  c_args : [pa_c_args, server_c_args],
  link_args : [nodelete_link_args],
  include_directories : [configinc, topinc],
  dependencies : [libpulse_dep, libpulsecommon_dep, libpulsecore_dep, alsa_dep, dbus_dep, libatomic_ops_dep, libm_dep, udev_dep, libintl_dep],
  install : true,
  install_rpath : privlibdir,
  install_dir : modlibexecdir,
)

alsa_udevrules = [
  '90-pulseaudio.rules',
]

if udev_dep.found()
  install_data(alsa_udevrules,
    install_dir : udevrulesdir,
  )
endif

subdir('mixer')
