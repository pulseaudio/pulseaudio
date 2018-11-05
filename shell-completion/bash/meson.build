aliases = [
  'pacat',
  'pacmd',
  'pactl',
  'padsp',
  'paplay',
  'parec',
  'parecord',
  'pasuspender',
]

install_data('pulseaudio', install_dir : bashcompletiondir)

foreach alias : aliases
  dst = join_paths(bashcompletiondir, alias)
  meson.add_install_script('ln', '-fs', 'pulseaudio', dst)
endforeach
