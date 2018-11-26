libraop_sources = [
  'raop-client.c',
  'raop-crypto.c',
  'raop-packet-buffer.c',
  'raop-sink.c',
  'raop-util.c',
]

libraop_headers = [
  'raop-client.h',
  'raop-crypto.h',
  'raop-packet-buffer.h',
  'raop-sink.h',
  'raop-util.h',
]

libraop = shared_library('raop',
  libraop_sources,
  libraop_headers,
  c_args : [pa_c_args, server_c_args],
  include_directories : [configinc, topinc],
  dependencies : [libpulse_dep, libpulsecommon_dep, libpulsecore_dep, librtp_dep, openssl_dep],
  install : true,
  install_dir : modlibexecdir,
)
