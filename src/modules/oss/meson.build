liboss_util_sources = [
  'oss-util.c'
]

liboss_util_headers = [
  'oss-util.h'
]

liboss_util = shared_library('oss-util',
  liboss_util_sources,
  liboss_util_headers,
  c_args : [pa_c_args, server_c_args],
  include_directories : [configinc, topinc],
  dependencies : [libpulse_dep, libpulsecommon_dep, libpulsecore_dep],
  install : true,
  install_rpath : privlibdir,
  install_dir : modlibexecdir,
)
