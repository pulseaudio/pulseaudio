if bashcompletiondir != 'no'
  aliases = []

  if get_option('daemon')
    aliases += [
      'pacmd',
      'pasuspender',
    ]
  endif

  if get_option('client')
  aliases += [
    'pacat',
    'pactl',
    'padsp',
    'paplay',
    'parec',
    'parecord',
  ]
  endif

  install_data('pulseaudio', install_dir : bashcompletiondir)

  foreach alias : aliases
    dst = join_paths(bashcompletiondir, alias)
    cmd = 'ln -fs @0@ $DESTDIR@1@'.format('pulseaudio', dst)
    meson.add_install_script('sh', '-c', cmd)
  endforeach
endif
