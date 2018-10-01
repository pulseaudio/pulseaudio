# The webrtc code is split off into a helper library to avoid having automake
# link module-echo-cancel with C++ (which it does if there are any C++ deps,
# even conditional ones).

# Defining this library as shared fails, as it actually requires a symbol
# from module-echo-cancel.c. Setting it to static solves that.

libwebrtc_util_sources = [
  'webrtc.cc'
]

libwebrtc_util = static_library('libwebrtc_util',
  libwebrtc_util_sources,
  cpp_args : [pa_c_args, server_c_args],
  include_directories : [configinc, topinc],
  dependencies : [libpulse_dep, libpulsecommon_dep, libpulsecore_dep, webrtc_dep],
  install : true
)
