# Note that a few tests have dependencies on src/modules.
#
# The syntax for tests declaration is:
#
#   test name, sources, deps, [extra libs, extra flags]
#

# Default tests

default_tests = [
  [ 'asyncmsgq-test', 'asyncmsgq-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'asyncq-test', 'asyncq-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'channelmap-test', 'channelmap-test.c',
    [ check_dep, libpulse_dep ] ],
  [ 'close-test', 'close-test.c',
    [            libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'core-util-test', 'core-util-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep ] ],
  [ 'cpu-mix-test', [ 'cpu-mix-test.c', 'runtime-test-util.h' ],
    [ check_dep, libm_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'cpu-remap-test', [ 'cpu-remap-test.c', 'runtime-test-util.h' ],
    [ check_dep, libm_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'cpu-sconv-test', [ 'cpu-sconv-test.c', 'runtime-test-util.h' ],
    [ check_dep, libm_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'cpu-volume-test', [ 'cpu-volume-test.c', 'runtime-test-util.h' ],
    [ check_dep, libm_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'format-test', 'format-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'get-binary-name-test', 'get-binary-name-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep ] ],
  [ 'hashmap-test', 'hashmap-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep ] ],
  [ 'hook-list-test', 'hook-list-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'json-test', 'json-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep ] ],
  [ 'lfe-filter-test', 'lfe-filter-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'lock-autospawn-test', 'lock-autospawn-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'mainloop-test', 'mainloop-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep ] ],
  [ 'memblock-test', 'memblock-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'memblockq-test', 'memblockq-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'mix-test', 'mix-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'mult-s16-test', [ 'mult-s16-test.c', 'runtime-test-util.h' ],
    [ check_dep, libm_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'proplist-test', 'proplist-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'queue-test', 'queue-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'resampler-test', 'resampler-test.c',
    [            libpulse_dep, libpulsecommon_dep, libpulsecore_dep, libintl_dep ] ],
  [ 'rtpoll-test', 'rtpoll-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'smoother-test', 'smoother-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'strlist-test', 'strlist-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'thread-mainloop-test', 'thread-mainloop-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'thread-test', 'thread-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'utf8-test', 'utf8-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep ] ],
  [ 'volume-test', 'volume-test.c',
    [ check_dep, libm_dep, libpulse_dep, libpulsecommon_dep ] ],
]

if host_machine.system() != 'windows'
  default_tests += [
    [ 'sigbus-test', 'sigbus-test.c',
      [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
    [ 'usergroup-test', 'usergroup-test.c',
      [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  ]
endif

if host_machine.system() != 'darwin'
  default_tests += [
    [ 'once-test', 'once-test.c',
      [ check_dep, thread_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep, libatomic_ops_dep ] ],
  ]
endif

if cc.has_header('sys/eventfd.h')
  default_tests += [
    [ 'srbchannel-test', 'srbchannel-test.c',
      [ check_dep, libpulse_dep, libpulsecommon_dep ] ]
  ]
endif

if alsa_dep.found()
  default_tests += [
    [ 'alsa-mixer-path-test', 'alsa-mixer-path-test.c',
      [ alsa_dep, check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ],
      libalsa_util ]
  ]
endif

if glib_dep.found()
  default_tests += [
    [ 'mainloop-test-glib', 'mainloop-test.c',
      [ check_dep, glib_dep, libpulse_dep, libpulsecommon_dep, libpulse_mainloop_glib_dep ],
      [], ['-DGLIB_MAIN_LOOP'] ]
  ]
endif

# No-run tests

norun_tests = [
  [ 'flist-test', 'flist-test.c',
    [ libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'ipacl-test', 'ipacl-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'lo-latency-test', [ 'lo-latency-test.c', 'lo-test-util.c', 'lo-test-util.h' ],
    [ check_dep, libm_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'mcalign-test', 'mcalign-test.c',
    [ libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'pacat-simple', 'pacat-simple.c',
    [ libpulse_dep, libpulse_simple_dep ] ],
  [ 'parec-simple', 'parec-simple.c',
    [ libpulse_dep, libpulse_simple_dep ] ],
  [ 'remix-test', 'remix-test.c',
    [ libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'rtstutter', 'rtstutter.c',
    [ thread_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'sig2str-test', 'sig2str-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
  [ 'stripnul', 'stripnul.c',
    [ libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
]

# echo-cancel test is a bit tedious to handle
echo_cancel_test_sources = []
foreach s : module_echo_cancel_sources
  echo_cancel_test_sources += '../modules/' + s
endforeach
echo_cancel_test_sources += module_echo_cancel_orc_sources

norun_tests += [
  [ 'echo-cancel-test', echo_cancel_test_sources,
    module_echo_cancel_deps + [ libpulse_dep, libpulsecommon_dep, libpulsecore_dep, libintl_dep ],
    module_echo_cancel_libs,
    module_echo_cancel_flags + server_c_args + [ '-DPA_MODULE_NAME=module_echo_cancel', '-DECHO_CANCEL_TEST=1' ] ]
]

if cc.has_header_symbol('signal.h', 'SIGXCPU')
  norun_tests += [
    [ 'cpulimit-test', [ 'cpulimit-test.c', '../daemon/cpulimit.c', '../daemon/cpulimit.h' ],
      [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
    [ 'cpulimit-test2', [ 'cpulimit-test.c', '../daemon/cpulimit.c', '../daemon/cpulimit.h' ],
      [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ],
      [], ['-DTEST2'] ],
  ]
endif

if cc.has_function('pthread_setaffinity_np', dependencies : thread_dep)
  norun_tests += [
    [ 'atomic-test', 'atomic-test.c',
      [ check_dep, libpulsecommon_dep, thread_dep ] ]
  ]
endif

if alsa_dep.found()
  norun_tests += [
    [ 'alsa-time-test', 'alsa-time-test.c', [ alsa_dep ] ]
  ]
endif

if gtk_dep.found() and glib_dep.found()
  norun_tests += [
    [ 'gtk-test', 'gtk-test.c',
      [ gtk_dep, libpulse_dep, libpulse_mainloop_glib_dep ] ]
  ]
endif

# Generate tests

test_env = environment()
test_env.set('MAKE_CHECK', '1')

foreach t : default_tests + norun_tests
  name = t[0]
  sources = t[1]
  deps = t[2]
  extra_libs = t.get(3, [])
  extra_flags = t.get(4, [])

  exe = executable(name, sources,
    c_args : pa_c_args + extra_flags,
    include_directories : [ configinc, topinc ],
    dependencies : deps,
    link_with : extra_libs,
  )

  if default_tests.contains(t)
    test(name, exe,
      env : test_env,
      timeout : 300,
    )
  endif
endforeach

# These tests need a running pulseaudio daemon

daemon_tests = [
  [ 'extended-test', 'extended-test.c',
    [ check_dep, libm_dep, libpulse_dep ] ],
  [ 'sync-playback', 'sync-playback.c',
    [ check_dep, libm_dep, libpulse_dep ] ],
]

daemon_tests_long = [
  [ 'connect-stress', 'connect-stress.c',
    [ check_dep, libpulse_dep ] ],
  [ 'interpol-test', 'interpol-test.c',
    [ check_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep ] ],
]

daemon_test_names = []
daemon_test_long_names = []

foreach t : daemon_tests + daemon_tests_long
  name = t[0]
  sources = t[1]
  deps = t[2]

  if daemon_tests.contains(t)
    daemon_test_names += name
  else
    daemon_test_long_names += name
  endif

  executable(name, sources,
    c_args : pa_c_args,
    include_directories : [ configinc, topinc ],
    dependencies : deps,
  )
endforeach

test_daemon_meson_sh = find_program('test-daemon.meson.sh')
run_target('test-daemon',
  command : [ test_daemon_meson_sh ] + daemon_test_names
)
run_target('test-daemon-long',
  command : [ test_daemon_meson_sh ] + daemon_test_long_names
)
