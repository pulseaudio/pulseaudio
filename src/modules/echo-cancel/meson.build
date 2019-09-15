# The webrtc code is split off into a helper library to avoid having automake
# link module-echo-cancel with C++ (which it does if there are any C++ deps,
# even conditional ones).

# This library requires a symbol from module-echo-cancel, hence we need
# '-Wl,--unresolved-symbols=ignore-in-object-files' otherwise it fails
# at link time.

libwebrtc_util_sources = [
  'webrtc.cc'
]

libwebrtc_util = shared_library('webrtc-util',
  libwebrtc_util_sources,
  cpp_args : [pa_c_args, server_c_args],
  include_directories : [configinc, topinc],
  dependencies : [libpulse_dep, libpulsecommon_dep, libpulsecore_dep, libatomic_ops_dep, webrtc_dep, libintl_dep],
  link_args : [nodelete_link_args, '-Wl,--unresolved-symbols=ignore-in-object-files'],
  install : true,
  install_rpath : privlibdir,
  install_dir : modlibexecdir,
)
