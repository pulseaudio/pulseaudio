pacat_sources = [
  'pacat.c',
]

# FIXME: man pages
executable('pacat',
  pacat_sources,
  install: true,
  include_directories : [configinc, topinc],
  link_with : [libpulsecommon, libpulse],
  dependencies : [sndfile_dep],
  c_args : pa_c_args,
)

pactl_sources = [
  'pactl.c',
]

# FIXME: man pages
executable('pactl',
  pactl_sources,
  install: true,
  include_directories : [configinc, topinc],
  link_with : [libpulsecommon, libpulse],
  dependencies : [sndfile_dep],
  c_args : pa_c_args,
)

pasuspender_sources = [
  'pasuspender.c',
]

# FIXME: man pages
executable('pasuspender',
  pasuspender_sources,
  install: true,
  include_directories : [configinc, topinc],
  link_with : [libpulsecommon, libpulse],
  c_args : pa_c_args,
)

pacmd_sources = [
  'pacmd.c',
]

# FIXME: man pages
executable('pacmd',
  pacmd_sources,
  install: true,
  include_directories : [configinc, topinc],
  link_with : [libpulsecommon, libpulse],
  c_args : pa_c_args,
)

pax11publish_sources = [
  'pax11publish.c',
]

# FIXME: man pages
executable('pax11publish',
  pax11publish_sources,
  install: true,
  include_directories : [configinc, topinc],
  link_with : [libpulsecommon, libpulse],
  dependencies : [x11_dep],
  c_args : pa_c_args,
)
