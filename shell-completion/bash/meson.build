if bashcompletiondir != 'no'
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
    cmd = 'ln -fs @0@ $DESTDIR@1@'.format('pulseaudio', dst)
    meson.add_install_script('sh', '-c', cmd)
  endforeach
endif
