# TODO How are symlinks installed by meson?
#      We might need meson.add_install_script()

# man page name, section, [aliases]
manpages = [
  ['default.pa', '5'],
  ['pacat', '1', ['paplay', 'parec', 'parecord', 'pamon']],
  ['pacmd', '1'],
  ['pactl', '1'],
  ['padsp', '1'],
  ['pasuspender', '1'],
  ['pax11publish', '1'],
  ['pulse-cli-syntax', '5'],
  ['pulse-client.conf', '5'],
  ['pulse-daemon.conf','5'],
  ['pulseaudio', '1'],
  ['start-pulseaudio-x11', '1'],
]

# FIXME: Add esdcompat if HAVE_ESOUND
#manpages += ['esdcompat', '1'],

# FIXME: This should be conditional
want_man = true

man_data = configuration_data()
man_data.set('PACKAGE_URL', 'http://pulseaudio.org/')
man_data.set('PACKAGE_BUGREPORT', 'pulseaudio-discuss (at) lists (dot) freedesktop (dot) org')
man_data.set('PA_DEFAULT_CONFIG_DIR', cdata.get_unquoted('PA_DEFAULT_CONFIG_DIR'))

perl = find_program('perl', required: want_man == true)
xmltoman_cmd = [perl, join_paths(meson.current_source_dir(), 'xmltoman')]
xmllint = find_program('xmllint', required: want_man == true)

foreach tuple : manpages
  stem = tuple[0]
  section = tuple[1]
  aliases = tuple.get(2, [])

  man = stem + '.' + section
  xmlin = man + '.xml.in'
  xml = man + '.xml'

  xml_file = configure_file(
    input : xmlin,
    output : xml,
    configuration : man_data,
  )

  mandirn = join_paths(get_option('mandir'), 'man' + section)

  custom_target(
    man + '-target',
    input : xml_file,
    output : man,
    capture : true,
    command : xmltoman_cmd + ['@INPUT@'],
    install : want_man,
    install_dir : mandirn,
  )

  foreach alias : aliases
    link = custom_target(
      alias,
      output : alias,
      command : ['ln', '-fs', man, '@OUTPUT@'],
      install : want_man,
      install_dir : mandirn,
    )
  endforeach

  if xmllint.found()
    test(
      'Validate xml file ' + xml,
      xmllint,
      args : [
	'--noout', '--valid',
	'--path', meson.current_source_dir(),
	join_paths(meson.current_build_dir(), xml)]
    )
  endif
endforeach
