if bashcompletiondir != 'no'
  aliases = []

  if get_option('daemon')
    aliases += [
      'pulseaudio',
      'pacmd',
      'pasuspender',
    ]

    # Create target directory for symlinks
    if meson.version().version_compare('>= 0.60.0')
      install_emptydir(bashcompletiondir)
    else
      meson.add_install_script('sh', '-c', 'mkdir -p $DESTDIR@0@'.format(bashcompletiondir))
    endif
  endif

  if get_option('client')
    aliases += [
      'pacat',
      'padsp',
      'paplay',
      'parec',
      'parecord',
    ]
    install_data('pactl', install_dir : bashcompletiondir)
  endif

  foreach alias : aliases
    dst = join_paths(bashcompletiondir, alias)
    cmd = 'ln -fs @0@ $DESTDIR@1@'.format('pactl', dst)
    meson.add_install_script('sh', '-c', cmd)
  endforeach
endif
