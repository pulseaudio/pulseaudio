# Order matters! raop needs rtp!
subdir('rtp')
subdir('raop')

# module name, sources, [headers, extra flags, extra deps, extra libs]
all_modules = [
  [ 'module-allow-passthrough', 'module-allow-passthrough.c' ],
  [ 'module-always-sink', 'module-always-sink.c' ],
  [ 'module-always-source', 'module-always-source.c' ],
  [ 'module-augment-properties', 'module-augment-properties.c' ],
#  [ 'module-bonjour-publish', 'macosx/module-bonjour-publish.c' ],
  [ 'module-card-restore', 'module-card-restore.c' ],
  [ 'module-cli', 'module-cli.c', [], [], [], libcli ],
  [ 'module-cli-protocol-tcp', 'module-protocol-stub.c', [], ['-DUSE_PROTOCOL_CLI', '-DUSE_TCP_SOCKETS'], [], libprotocol_cli ],
  [ 'module-cli-protocol-unix', 'module-protocol-stub.c', [], ['-DUSE_PROTOCOL_CLI', '-DUSE_UNIX_SOCKETS'], [], libprotocol_cli ],
  [ 'module-combine', 'module-combine.c' ],
  [ 'module-combine-sink', 'module-combine-sink.c' ],
  [ 'module-console-kit', 'module-console-kit.c', [], [], [dbus_dep] ],
#  [ 'module-coreaudio-detect', 'macosx/module-coreaudio-detect.c' ],
#  [ 'module-coreaudio-device', 'macosx/module-coreaudio-device.c' ],
  [ 'module-default-device-restore', 'module-default-device-restore.c', [], [], [], libprotocol_native ],
  [ 'module-detect', 'module-detect.c' ],
  [ 'module-device-manager', 'module-device-manager.c', [], [], [], libprotocol_native ],
  [ 'module-device-restore', 'module-device-restore.c', [], [], [dbus_dep], libprotocol_native ],
#  [ 'module-echo-cancel', 'module-echo-cancel.c' ],
  [ 'module-esound-compat-spawnfd', 'module-esound-compat-spawnfd.c' ],
  [ 'module-esound-compat-spawnpid', 'module-esound-compat-spawnpid.c' ],
#  [ 'module-esound-protocol-tcp', 'module-protocol-stub.c' ],
#  [ 'module-esound-protocol-unix', 'module-protocol-stub.c' ],
  [ 'module-esound-sink', 'module-esound-sink.c' ],
  [ 'module-filter-apply', 'module-filter-apply.c' ],
  [ 'module-filter-heuristics', 'module-filter-heuristics.c' ],
#  [ 'module-gconf', 'gconf/module-gconf.c' ],
  [ 'module-hal-detect', 'module-hal-detect-compat.c' ],
  [ 'module-http-protocol-tcp', 'module-protocol-stub.c', [], ['-DUSE_PROTOCOL_HTTP', '-DUSE_TCP_SOCKETS'], [], libprotocol_http ],
  [ 'module-http-protocol-unix', 'module-protocol-stub.c', [], ['-DUSE_PROTOCOL_HTTP', '-DUSE_UNIX_SOCKETS'], [], libprotocol_http ],
  [ 'module-intended-roles', 'module-intended-roles.c' ],
  [ 'module-jackdbus-detect', 'jack/module-jackdbus-detect.c', [], [], [dbus_dep] ],
#  [ 'module-ladspa-sink', 'module-ladspa-sink.c' ],
  [ 'module-loopback', 'module-loopback.c' ],
  [ 'module-match', 'module-match.c' ],
  [ 'module-native-protocol-fd', 'module-native-protocol-fd.c', [], [], [], libprotocol_native ],
  [ 'module-native-protocol-tcp', 'module-protocol-stub.c', [], ['-DUSE_PROTOCOL_NATIVE', '-DUSE_TCP_SOCKETS'], [], libprotocol_native ],
  [ 'module-native-protocol-unix', 'module-protocol-stub.c', [], ['-DUSE_PROTOCOL_NATIVE', '-DUSE_UNIX_SOCKETS'], [], libprotocol_native ],
  [ 'module-null-sink', 'module-null-sink.c' ],
  [ 'module-null-source', 'module-null-source.c' ],
#  [ 'module-oss', 'oss/module-oss.c' ],
  [ 'module-pipe-sink', 'module-pipe-sink.c' ],
  [ 'module-pipe-source', 'module-pipe-source.c' ],
  [ 'module-position-event-sounds', 'module-position-event-sounds.c' ],
  [ 'module-remap-sink', 'module-remap-sink.c' ],
  [ 'module-remap-source', 'module-remap-source.c' ],
  [ 'module-rescue-streams', 'module-rescue-streams.c' ],
  [ 'module-role-cork', ['module-role-cork.c', 'stream-interaction.c'], 'stream-interaction.h' ],
  [ 'module-role-ducking', ['module-role-ducking.c', 'stream-interaction.c'], 'stream-interaction.h' ],
  [ 'module-rtp-recv', 'rtp/module-rtp-recv.c', [], [], [], librtp ],
  [ 'module-rtp-send', 'rtp/module-rtp-send.c' , [], [], [], librtp ],
  [ 'module-rygel-media-server', 'module-rygel-media-server.c', [], [], [dbus_dep], libprotocol_http ],
  [ 'module-simple-protocol-tcp', 'module-protocol-stub.c', [], ['-DUSE_PROTOCOL_SIMPLE', '-DUSE_TCP_SOCKETS'], [], libprotocol_simple ],
  [ 'module-simple-protocol-unix', 'module-protocol-stub.c', [], ['-DUSE_PROTOCOL_SIMPLE', '-DUSE_UNIX_SOCKETS'], [], libprotocol_simple ],
  [ 'module-sine', 'module-sine.c' ],
  [ 'module-sine-source', 'module-sine-source.c' ],
#  [ 'module-solaris', 'module-solaris.c' ],
  [ 'module-stream-restore', 'module-stream-restore.c', [], [], [dbus_dep], libprotocol_native ],
  [ 'module-suspend-on-idle', 'module-suspend-on-idle.c' ],
  [ 'module-switch-on-connect', 'module-switch-on-connect.c' ],
  [ 'module-switch-on-port-available', 'module-switch-on-port-available.c' ],
  [ 'module-tunnel-sink', 'module-tunnel.c', [], [], [x11_dep] ],
  [ 'module-tunnel-sink-new', 'module-tunnel-sink-new.c' ],
  [ 'module-tunnel-source', 'module-tunnel.c', [], [], [x11_dep] ],
  [ 'module-tunnel-source-new', 'module-tunnel-source-new.c' ],
  [ 'module-virtual-sink', 'module-virtual-sink.c' ],
  [ 'module-virtual-source', 'module-virtual-source.c' ],
  [ 'module-virtual-surround-sink', 'module-virtual-surround-sink.c' ],
  [ 'module-volume-restore', 'module-volume-restore.c' ],
#  [ 'module-waveout', 'module-waveout.c' ],
]

# Modules enabled by headers

if cc.has_header('linux/input.h')
  all_modules += [
    [ 'module-mmkbd-evdev', 'module-mmkbd-evdev.c' ],
  ]
endif

# Modules enabled by dependencies

if alsa_dep.found()
  subdir('alsa')
  all_modules += [
    [ 'module-alsa-card', 'alsa/module-alsa-card.c', [], [], [alsa_dep, libm_dep], libalsa_util ],
    [ 'module-alsa-sink', 'alsa/module-alsa-sink.c', [], [], [alsa_dep, libm_dep], libalsa_util ],
    [ 'module-alsa-source', 'alsa/module-alsa-source.c', [], [], [alsa_dep, libm_dep], libalsa_util ],
  ]
endif

if avahi_dep.found()
  all_modules += [
    [ 'module-zeroconf-discover', 'module-zeroconf-discover.c', [], [], [avahi_dep], libavahi_wrap ],
    [ 'module-zeroconf-publish', 'module-zeroconf-publish.c', [], [], [avahi_dep, dbus_dep], [libavahi_wrap, libprotocol_native] ],
  ]
endif

if dbus_dep.found()
  all_modules += [
    [ 'module-dbus-protocol',
      [ 'dbus/iface-card.c', 'dbus/iface-card.h',
	'dbus/iface-card-profile.c', 'dbus/iface-card-profile.h',
	'dbus/iface-client.c', 'dbus/iface-client.h',
	'dbus/iface-core.c', 'dbus/iface-core.h',
	'dbus/iface-device.c', 'dbus/iface-device.h',
	'dbus/iface-device-port.c', 'dbus/iface-device-port.h',
	'dbus/iface-memstats.c', 'dbus/iface-memstats.h',
	'dbus/iface-module.c', 'dbus/iface-module.h',
	'dbus/iface-sample.c', 'dbus/iface-sample.h',
	'dbus/iface-stream.c', 'dbus/iface-stream.h',
	'dbus/module-dbus-protocol.c',
      ],
      [], [], [dbus_dep] ]
  ]
endif

if dbus_dep.found() and sbc_dep.found()
  all_modules += [
    [ 'module-bluetooth-discover', 'bluetooth/module-bluetooth-discover.c' ],
    [ 'module-bluetooth-policy', 'bluetooth/module-bluetooth-policy.c', [], [], [dbus_dep] ]
  ]
  if bluez_dep.found()
    subdir('bluetooth')
    all_modules += [
      [ 'module-bluez5-device', 'bluetooth/module-bluez5-device.c', [], [], [sbc_dep], libbluez5_util ],
      [ 'module-bluez5-discover', 'bluetooth/module-bluez5-discover.c', [], [], [dbus_dep], libbluez5_util ],
    ]
  endif
endif

if fftw_dep.found()
  all_modules += [
    [ 'module-equalizer-sink', 'module-equalizer-sink.c', [], [], [dbus_dep, fftw_dep, libm_dep] ],
  ]
endif

if jack_dep.found()
  all_modules += [
    [ 'module-jack-sink', 'jack/module-jack-sink.c', [], [], [jack_dep] ],
    [ 'module-jack-source', 'jack/module-jack-source.c', [], [], [jack_dep] ],
  ]
endif

if lirc_dep.found()
  all_modules += [
    [ 'module-lirc', 'module-lirc.c', [], [], [lirc_dep] ],
  ]
endif

if openssl_dep.found()
  all_modules += [
    [ 'module-raop-sink', 'raop/module-raop-sink.c', [], [], [], libraop ],
  ]
  if avahi_dep.found()
    all_modules += [
      [ 'module-raop-discover', 'raop/module-raop-discover.c', [], [], [avahi_dep], libavahi_wrap ],
    ]
  endif
endif

if systemd_dep.found()
  all_modules += [
    [ 'module-systemd-login', 'module-systemd-login.c', [], [], [systemd_dep] ],
  ]
endif

if udev_dep.found()
  all_modules += [ [ 'module-udev-detect', 'module-udev-detect.c', [], [], [udev_dep] ] ]
endif

if x11_dep.found()
  all_modules += [
    [ 'module-x11-bell', 'x11/module-x11-bell.c', [], [], [x11_dep] ],
    [ 'module-x11-publish', 'x11/module-x11-publish.c', [], [], [x11_dep], libprotocol_native ],
  ]
  if xtst_dep.found()
    all_modules += [
      [ 'module-x11-cork-request', 'x11/module-x11-cork-request.c', [], [], [x11_dep, xtst_dep] ],
    ]
  endif
  if ice_dep.found() and sm_dep.found()
    all_modules += [
      [ 'module-x11-xsmp', 'x11/module-x11-xsmp.c', [], [], [x11_dep, ice_dep, sm_dep] ],
    ]
  endif
endif

foreach m : all_modules
  name = m[0]
  sources = m[1]
  headers = m.get(2, [])
  extra_flags = m.get(3, [])
  extra_deps = m.get(4, [])
  extra_libs = m.get(5, [])

  mod = shared_module(name,
    sources,
    headers,
    include_directories : [configinc, topinc],
    c_args : [pa_c_args, server_c_args, '-DPA_MODULE_NAME=' + name.underscorify()] + extra_flags,
    install : true,
    dependencies : [thread_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep] + extra_deps,
    link_args : [ '-Wl,--no-undefined' ],
    link_with : extra_libs,
    name_prefix : '',
    implicit_include_directories : false)
endforeach
