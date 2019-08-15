librtp_sources = [
  'rtp.c',
  'sdp.c',
  'sap.c',
  'rtsp_client.c',
  'headerlist.c',
]

librtp_headers = [
  'rtp.h',
  'sdp.h',
  'sap.h',
  'rtsp_client.h',
  'headerlist.h',
]

librtp = shared_library('rtp',
  librtp_sources,
  librtp_headers,
  c_args : [pa_c_args, server_c_args],
  link_args : [nodelete_link_args],
  include_directories : [configinc, topinc],
  dependencies : [libpulse_dep, libpulsecommon_dep, libpulsecore_dep, libatomic_ops_dep],
  install : true,
  install_rpath : privlibdir,
  install_dir : modlibexecdir,
)

librtp_dep = declare_dependency(link_with: librtp)
