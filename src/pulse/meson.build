configure_file(
  input : 'version.h.in',
  output : 'version.h',
  configuration : cdata,
  install_dir : join_paths(includedir, 'pulse'),
)

libpulse_sources = [
  'channelmap.c',
  'context.c',
  'direction.c',
  'error.c',
  'ext-device-manager.c',
  'ext-device-restore.c',
  'ext-stream-restore.c',
  'format.c',
  'internal.h',
  'introspect.c',
  'mainloop-api.c',
  'mainloop-signal.c',
  'mainloop.c',
  'operation.c',
  'proplist.c',
  'rtclock.c',
  'sample.c',
  'scache.c',
  'stream.c',
  'subscribe.c',
  'thread-mainloop.c',
  'timeval.c',
  'utf8.c',
  'util.c',
  'volume.c',
  'xmalloc.c',
]

libpulse_headers = [
  'cdecl.h',
  'channelmap.h',
  'context.h',
  'def.h',
  'direction.h',
  'error.h',
  'ext-device-manager.h',
  'ext-device-restore.h',
  'ext-stream-restore.h',
  'format.h',
  'gccmacro.h',
  'introspect.h',
  'mainloop-api.h',
  'mainloop-signal.h',
  'mainloop.h',
  'operation.h',
  'proplist.h',
  'pulseaudio.h',
  'rtclock.h',
  'sample.h',
  'scache.h',
  'stream.h',
  'subscribe.h',
  'thread-mainloop.h',
  'timeval.h',
  'utf8.h',
  'util.h',
  'volume.h',
  'xmalloc.h',
]

if glib_dep.found()
  libpulse_headers += 'glib-mainloop.h'
endif

versioning_link_args = '-Wl,-version-script=' + join_paths(meson.source_root(), 'src', 'map-file')

libpulse = shared_library('pulse',
  libpulse_sources,
  libpulse_headers,
  version : libpulse_version,
  include_directories : [configinc, topinc],
  c_args : [pa_c_args],
  link_args : [nodelete_link_args, versioning_link_args],
  install : true,
  install_rpath : privlibdir,
  dependencies : [libm_dep, thread_dep, libpulsecommon_dep, dbus_dep, dl_dep, iconv_dep, libintl_dep],
  implicit_include_directories : false)

libpulse_dep = declare_dependency(link_with: libpulse)

install_headers(
  libpulse_headers, 'simple.h',
  subdir : 'pulse'
)

libpulse_simple = shared_library('pulse-simple',
  'simple.c',
  'simple.h',
  version : libpulse_simple_version,
  c_args : [pa_c_args],
  link_args : [nodelete_link_args, versioning_link_args],
  include_directories : [configinc, topinc],
  dependencies : [libpulse_dep, libpulsecommon_dep],
  install : true,
  install_rpath : privlibdir,
)

libpulse_simple_dep = declare_dependency(link_with: libpulse_simple)

if glib_dep.found()
  libpulse_mainloop_glib = shared_library('pulse-mainloop-glib',
    'glib-mainloop.c',
    'glib-mainloop.h',
    version : libpulse_mainloop_glib_version,
    c_args : [pa_c_args],
    link_args : [nodelete_link_args, versioning_link_args],
    include_directories : [configinc, topinc],
    dependencies : [libpulse_dep, libpulsecommon_dep, glib_dep],
    install : true,
    install_rpath : privlibdir,
  )

  libpulse_mainloop_glib_dep = declare_dependency(link_with: libpulse_mainloop_glib)
endif

# Configuration files

client_conf = configuration_data()
client_conf.set('PA_BINARY', cdata.get_unquoted('PA_BINARY'))

client_conf_file = configure_file(
  input : 'client.conf.in',
  output : 'client.conf',
  configuration : client_conf,
  install_dir : pulsesysconfdir,
)
