
# man page name, section, [aliases]
manpages = []

if get_option('daemon')
  manpages += [
    ['default.pa', '5'],
    ['pacmd', '1'],
    ['pasuspender', '1'],
    ['pulse-cli-syntax', '5'],
    ['pulse-daemon.conf', '5'],
    ['pulseaudio', '1'],
    ['start-pulseaudio-x11', '1'],
  ]
endif

if get_option('client')
  manpages += [
    ['pacat', '1', ['paplay', 'parec', 'parecord', 'pamon']],
    ['pactl', '1'],
    ['pulse-client.conf', '5'],
  ]

  if cdata.has('HAVE_OSS_WRAPPER')
    manpages += [
      ['padsp', '1'],
    ]
  endif

  if x11_dep.found()
    manpages += [
      ['pax11publish', '1'],
    ]
  endif
endif

# FIXME: Add esdcompat if HAVE_ESOUND
#manpages += ['esdcompat', '1'],

man_data = configuration_data()
man_data.set('PACKAGE_URL', 'http://pulseaudio.org/')
man_data.set('PACKAGE_BUGREPORT', 'pulseaudio-discuss (at) lists (dot) freedesktop (dot) org')
man_data.set('PA_DEFAULT_CONFIG_DIR', cdata.get_unquoted('PA_DEFAULT_CONFIG_DIR'))

perl = find_program('perl', required: true)
xmltoman_cmd = [perl, join_paths(meson.current_source_dir(), 'xmltoman')]
xmllint = find_program('xmllint', required: false)

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

  mandirn = join_paths(mandir, 'man' + section)

  custom_target(
    man + '-target',
    input : xml_file,
    output : man,
    capture : true,
    command : xmltoman_cmd + ['@INPUT@'],
    install : true,
    install_dir : mandirn,
  )

  foreach alias_stem : aliases
    alias = alias_stem + '.' + section
    dst = join_paths(mandirn, alias)
    cmd = 'ln -fs @0@ $DESTDIR@1@'.format(man, dst)
    meson.add_install_script('sh', '-c', cmd)
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
