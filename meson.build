project('pulseaudio', 'c', 'cpp',
        version : run_command(find_program('git-version-gen'), join_paths(meson.current_source_dir(), '.tarball-version')).stdout().strip(),
        meson_version : '>= 0.50.0',
        default_options : [ 'c_std=gnu11', 'cpp_std=c++11' ]
        )

pa_version_str = meson.project_version()
# For tarballs, the first split will do nothing, but for builds in git, we
# split out suffixes when there are commits since the last tag
# (e.g.: v11.99.1-3-gad14bdb24 -> v11.99.1)
version_split = pa_version_str.split('-')[0].split('.')
pa_version_major = version_split[0].split('v')[0]
pa_version_minor = version_split[1]
if version_split.length() > 2
  pa_version_micro = version_split[2]
else
  pa_version_micro = '0'
endif
pa_version_major_minor = pa_version_major + '.' + pa_version_minor

pa_api_version = 12
pa_protocol_version = 34

# The stable ABI for client applications, for the version info x:y:z
# always will hold x=z
libpulse_version_info = [23, 0, 23]

# A simplified, synchronous, ABI-stable interface for client
# applications, for the version info x:y:z always will hold x=z
libpulse_simple_version_info = [1, 1, 1]

# The ABI-stable GLib adapter for client applications, for the version
# info x:y:z always will hold x=z
libpulse_mainloop_glib_version_info = [0, 6, 0]

libpulse_version = '@0@.@1@.@2@'.format(
  libpulse_version_info[0] - libpulse_version_info[2],
  libpulse_version_info[0],
  libpulse_version_info[1],
)

libpulse_simple_version = '@0@.@1@.@2@'.format(
  libpulse_simple_version_info[0] - libpulse_simple_version_info[2],
  libpulse_simple_version_info[0],
  libpulse_simple_version_info[1],
)

libpulse_mainloop_glib_version = '@0@.@1@.@2@'.format(
  libpulse_mainloop_glib_version_info[0] - libpulse_mainloop_glib_version_info[2],
  libpulse_mainloop_glib_version_info[0],
  libpulse_mainloop_glib_version_info[1],
)

# Paths

prefix = get_option('prefix')
assert(prefix.startswith('/'), 'Prefix is not absolute: "@0@"'.format(prefix))

bindir = join_paths(prefix, get_option('bindir'))
includedir = join_paths(prefix, get_option('includedir'))
libdir = join_paths(prefix, get_option('libdir'))
libexecdir = join_paths(prefix, get_option('libexecdir'))
mandir = join_paths(prefix, get_option('mandir'))
datadir = join_paths(prefix, get_option('datadir'))
localedir = join_paths(prefix, get_option('localedir'))
localstatedir = join_paths(prefix, get_option('localstatedir'))
sysconfdir = join_paths(prefix, get_option('sysconfdir'))
privlibdir = join_paths(libdir, 'pulseaudio')

alsadatadir = get_option('alsadatadir')
if alsadatadir == ''
  alsadatadir = join_paths(datadir, 'pulseaudio', 'alsa-mixer')
endif

pkgconfigdir = join_paths(libdir, 'pkgconfig')
pulselibexecdir = join_paths(libexecdir, 'pulse')
pulsesysconfdir = join_paths(sysconfdir, 'pulse')

modlibexecdir = get_option('modlibexecdir')
if modlibexecdir == ''
  modlibexecdir = join_paths(libdir, 'pulse-' + pa_version_major_minor, 'modules')
endif

padsplibdir = get_option('padsplibdir')
if padsplibdir == ''
  padsplibdir = privlibdir
endif

pulsedsp_location = get_option('pulsedsp-location')
if pulsedsp_location == ''
  pulsedsp_location = join_paths(prefix, padsplibdir)
endif

systemduserunitdir = get_option('systemduserunitdir')
# the default value is set below

udevrulesdir = get_option('udevrulesdir')
if udevrulesdir == ''
  # absolute path, otherwise meson prepends the prefix
  udevrulesdir = '/lib/udev/rules.d'
endif

vapidir = join_paths(datadir, 'vala', 'vapi')

bashcompletiondir = get_option('bashcompletiondir')
if bashcompletiondir == ''
  bash_completion_dep = dependency('bash-completion', required : false)
  if bash_completion_dep.found()
    bashcompletiondir = bash_completion_dep.get_pkgconfig_variable('completionsdir')
  else
    bashcompletiondir = join_paths(datadir, 'bash-completion', 'completions')
  endif
endif

zshcompletiondir = get_option('zshcompletiondir')
if zshcompletiondir == ''
  zshcompletiondir = join_paths(datadir, 'zsh', 'site-functions')
endif

# Configuration data

cc = meson.get_compiler('c')

cdata = configuration_data()
cdata.set_quoted('PACKAGE', 'pulseaudio')
cdata.set_quoted('PACKAGE_NAME', 'pulseaudio')
cdata.set_quoted('PACKAGE_VERSION', pa_version_str)
cdata.set('PA_MAJOR', pa_version_major)
cdata.set('PA_MINOR', pa_version_minor)
cdata.set('PA_API_VERSION', pa_api_version)
cdata.set('PA_PROTOCOL_VERSION', pa_protocol_version)
cdata.set_quoted('PA_MACHINE_ID', join_paths(sysconfdir, 'machine-id'))
cdata.set_quoted('PA_MACHINE_ID_FALLBACK', join_paths(localstatedir, 'lib', 'dbus', 'machine-id'))
cdata.set_quoted('PA_SRCDIR', join_paths(meson.current_source_dir(), 'src'))
cdata.set_quoted('PA_BUILDDIR', meson.current_build_dir())
cdata.set_quoted('PA_SOEXT', '.so')
cdata.set_quoted('PA_DEFAULT_CONFIG_DIR', pulsesysconfdir)
cdata.set_quoted('PA_BINARY', join_paths(bindir, 'pulseaudio'))
cdata.set_quoted('PA_SYSTEM_RUNTIME_PATH', join_paths(localstatedir, 'run', 'pulse'))
cdata.set_quoted('PA_SYSTEM_CONFIG_PATH', join_paths(localstatedir, 'lib', 'pulse'))
cdata.set_quoted('PA_SYSTEM_STATE_PATH', join_paths(localstatedir, 'lib', 'pulse'))
cdata.set_quoted('PA_DLSEARCHPATH', modlibexecdir)
cdata.set_quoted('PA_SYSTEM_USER', get_option('system_user'))
cdata.set_quoted('PA_SYSTEM_GROUP', get_option('system_group'))
cdata.set_quoted('PA_ACCESS_GROUP', get_option('access_group'))
cdata.set_quoted('PA_CFLAGS', 'Not yet supported on meson')
cdata.set_quoted('PA_ALSA_PATHS_DIR', join_paths(alsadatadir, 'paths'))
cdata.set_quoted('PA_ALSA_PROFILE_SETS_DIR', join_paths(alsadatadir, 'profile-sets'))
cdata.set_quoted('DESKTOPFILEDIR', join_paths(datadir, 'applications'))
cdata.set_quoted('PULSE_LOCALEDIR', localedir)
cdata.set_quoted('GETTEXT_PACKAGE', 'pulseaudio')
cdata.set('ENABLE_NLS', 1)
cdata.set('top_srcdir', meson.source_root())

# Platform specifics
# First some defaults to keep config file generation happy
cdata.set('HAVE_COREAUDIO', 0)
cdata.set('HAVE_WAVEOUT', 0)
# FIXME: This was not tested. Maybe some flags should better be CFLAGS,
# rather than ending up in the config.h file?
if host_machine.system() == 'darwin'
  cdata.set('OS_IS_DARWIN', 1)
  cdata.set('_DARWIN_C_SOURCE', '200112L') # Needed to get NSIG on Mac OS
elif host_machine.system() == 'windows'
  cdata.set('OS_IS_WIN32', 1)
  cdata.set('WIN32_LEAN_AND_MEAN', 1) # Needed to avoid including unnecessary headers on Windows
#elif host_machine.system() == 'solaris'
#  # Apparently meson has no solaris support?
#  # Needed to get declarations for msg_control and msg_controllen on Solaris
#  cdata.set('_XOPEN_SOURCE', 600)
#  cdata.set('__EXTENSIONS__', 1)
endif

if cc.has_type('_Bool')
  cdata.set('HAVE_STD_BOOL', 1)
endif

if host_machine.cpu_family() == 'x86_64' or cc.sizeof('void *') >= 8
  cdata.set('HAVE_FAST_64BIT_OPERATIONS', 1)
endif

# Headers

check_headers = [
  'arpa/inet.h',
  'byteswap.h',
  'cpuid.h',
  'dlfcn.h',
  'execinfo.h',
  'grp.h',
  'langinfo.h',
  'linux/sockios.h',
  'locale.h',
  'netdb.h',
  'netinet/in.h',
  'netinet/in_systm.h',
  'netinet/ip.h',
  'netinet/tcp.h',
  'pcreposix.h',
  'poll.h',
  'pwd.h',
  'regex.h',
  'sched.h',
  'stdint.h',
  'sys/atomic.h',
  'sys/capability.h',
  'sys/conf.h',
  'sys/dl.h',
  'sys/eventfd.h',
  'sys/filio.h',
  'sys/ioctl.h',
  'sys/mman.h',
  'sys/prctl.h',
  'sys/resource.h',
  'sys/select.h',
  'sys/socket.h',
  'sys/syscall.h',
  'sys/uio.h',
  'sys/un.h',
  'sys/wait.h',
  'syslog.h',
  'valgrind/memcheck.h',
  'xlocale.h',
]

foreach h : check_headers
  if cc.has_header(h)
    define = 'HAVE_' + h.underscorify().to_upper()
    cdata.set(define, 1)
  endif
endforeach

# FIXME: move this to the above set
if cc.has_header('pthread.h')
  cdata.set('HAVE_PTHREAD', 1)
endif

if cc.has_header_symbol('pthread.h', 'PTHREAD_PRIO_INHERIT')
  cdata.set('HAVE_PTHREAD_PRIO_INHERIT', 1)
endif

# Functions

check_functions = [
  'accept4',
  'clock_gettime',
  'ctime_r',
  'fchmod',
  'fchown',
  'fork',
  'fstat',
  'getaddrinfo',
  'getgrgid_r',
  'getgrnam_r',
  'getpwnam_r',
  'getpwuid_r',
  'gettimeofday',
  'getuid',
  'lrintf',
  'lstat',
  'memfd_create',
  'mkfifo',
  'mlock',
  'nanosleep',
  'open64',
  'paccept',
  'pipe',
  'pipe2',
  'posix_fadvise',
  'posix_madvise',
  'posix_memalign',
  'ppoll',
  'readlink',
  'setegid',
  'seteuid',
  'setpgid',
  'setregid',
  'setresgid',
  'setresuid',
  'setreuid',
  'setsid',
  'sig2str',
  'sigaction',
  'strerror_r',
  'strtod_l',
  'strtof',
  'symlink',
  'sysconf',
  'uname',
]

foreach f : check_functions
  if cc.has_function(f)
    define = 'HAVE_' + f.underscorify().to_upper()
    cdata.set(define, 1)
  endif
endforeach

if cc.has_header_symbol('sys/syscall.h', 'SYS_memfd_create')
  cdata.set('HAVE_MEMFD', 1)
endif

if cc.has_function('dgettext')
  libintl_dep = []
else
  libintl_dep = cc.find_library('intl')
endif

# Symbols

if cc.has_header_symbol('signal.h', 'SIGXCPU')
  cdata.set('HAVE_SIGXCPU', 1)
endif

if not cc.has_header_symbol('netinet/in.h', 'INADDR_NONE')
  if not cc.has_header_symbol('winsock2.h', 'INADDR_NONE')
    # Define INADDR_NONE if not found (Solaris)
    cdata.set('INADDR_NONE', '0xffffffff')
  endif
endif

check_decls = [
  [ 'environ', 'unistd.h', '#define _GNU_SOURCE' ],
  [ 'SOUND_PCM_READ_RATE', 'sys/soundcard.h', '' ],
  [ 'SOUND_PCM_READ_CHANNELS', 'sys/soundcard.h', '' ],
  [ 'SOUND_PCM_READ_BITS', 'sys/soundcard.h', '' ],
]

foreach s : check_decls
  if cc.has_header_symbol(s[1], s[0], prefix : s[2])
    define = 'HAVE_DECL_' + s[0].to_upper()
    cdata.set(define, 1)
  endif
endforeach

# Types

# FIXME: do we ever care about gid_t not being defined / smaller than an int?
cdata.set('GETGROUPS_T', 'gid_t')

# Include paths

configinc = include_directories('.')
topinc = include_directories('src')

# CFLAGS/LDFLAGS

pa_c_args = ['-DHAVE_CONFIG_H', '-D_GNU_SOURCE']
server_c_args = ['-D__INCLUDED_FROM_PULSE_AUDIO']
cdata.set('MESON_BUILD', 1)

# On ELF systems we don't want the libraries to be unloaded since we don't clean them up properly,
# so we request the nodelete flag to be enabled.
# On other systems, we don't really know how to do that, but it's welcome if somebody can tell.
nodelete_link_args = ['-Wl,-z,nodelete']

# Code coverage

if get_option('gcov')
  add_project_arguments('--coverage', language: ['c', 'cpp'])
  add_project_link_arguments('--coverage', language: ['c', 'cpp'])
endif

# Core Dependencies

libm_dep = cc.find_library('m', required : true)

thread_dep = dependency('threads')
foreach f : [
  'pthread_getname_np',
  'pthread_setaffinity_np',
  'pthread_setname_np',
]
  if cc.has_function(f, dependencies : thread_dep)
    define = 'HAVE_' + f.underscorify().to_upper()
    cdata.set(define, 1)
  endif
endforeach

cap_dep = cc.find_library('cap', required : false)

shm_dep = cc.find_library('rt', required : false)
if shm_dep.found()
  cdata.set('HAVE_SHM_OPEN', 1)
endif

dl_dep = cc.find_library('dl', required : false)
if dl_dep.found()
  cdata.set('HAVE_DLADDR', 1)
endif

have_iconv = false
if cc.has_function('iconv_open')
  iconv_dep = dependency('', required : false)
  have_iconv = true
else
  iconv_dep = cc.find_library('iconv', required : false)
  have_iconv = iconv_dep.found()
endif
if have_iconv
  cdata.set('HAVE_ICONV', 1)
  iconvconsttest = '''#include <iconv.h>
size_t iconv (iconv_t cd, char * *inbuf, size_t *inbytesleft, char * *outbuf, size_t *outbytesleft);
'''
  if cc.compiles(iconvconsttest, dependencies : iconv_dep)
    cdata.set('ICONV_CONST', '')
  else
    cdata.set('ICONV_CONST', 'const')
  endif
endif

# Atomic operations

if get_option('atomic-arm-memory-barrier')
    cdata.set('ATOMIC_ARM_MEMORY_BARRIER_ENABLED', 1)
endif

need_libatomic_ops = false

atomictest = '''void func() {
  volatile int atomic = 2;
  __sync_bool_compare_and_swap (&atomic, 2, 3);
}
'''

if cc.compiles(atomictest)
  cdata.set('HAVE_ATOMIC_BUILTINS', 1)

  newatomictest = '''void func() {
    int c = 0;
    __atomic_store_n(&c, 4, __ATOMIC_SEQ_CST);
  }
  '''

  if(cc.compiles(newatomictest))
    cdata.set('HAVE_ATOMIC_BUILTINS_MEMORY_MODEL', 1)
  endif

elif host_machine.cpu_family() == 'arm'
  if host_machine.system() == 'linux' and get_option('atomic-arm-linux-helpers')
    cdata.set('ATOMIC_ARM_LINUX_HELPERS', 1)
  else
    armatomictest = '''int func() {
      volatile int a=0;
      int o=0, n=1, r;
      asm volatile (
	      "ldrex    %0, [%1]\n"
	      "subs  %0, %0, %2\n"
	      "strexeq %0, %3, [%1]\n"
	      : "=&r" (r)
	      : "r" (&a), "Ir" (o), "r" (n)
      : "cc");
      return (a==1 ? 0 : -1);
    }
    '''

    if cc.compiles(armatomictest)
      cdata.set('ATOMIC_ARM_INLINE_ASM', 1)
    else
      need_libatomic_ops = true
    endif
  endif # arm && !linux

elif not ['freebsd', 'netbsd'].contains(host_machine.system())
  need_libatomic_ops = true
endif # !atomic helpers && !arm

if need_libatomic_ops
  assert(cc.has_header('atomic_ops.h'), 'Need libatomic_ops')

  cdata.set('AO_REQUIRE_CAS', 1)

  if host_machine.system() != 'windows'
    libatomic_ops_dep = cc.find_library('atomic_ops', required : true)
  else
    libatomic_ops_dep = dependency('', required: false)
  endif
else
  libatomic_ops_dep = dependency('', required: false)
endif

# ARM checks
# ARMV6 instructions we need
if host_machine.cpu_family() == 'arm'
  armv6test = '''int func() {
    volatile int a = -60000, b = 0xaaaabbbb, c = 0xccccdddd;
    asm volatile ("ldr r0, %2 \n"
                  "ldr r2, %3 \n"
                  "ldr r3, %4 \n"
                  "ssat r1, #8, r0 \n"
                  "str r1, %0 \n"
                  "pkhbt r1, r3, r2, LSL #8 \n"
                  "str r1, %1 \n"
                  : "=m" (a), "=m" (b)
                  : "m" (a), "m" (b), "m" (c)
                  : "r0", "r1", "r2", "r3", "cc");
    return (a == -128 && b == 0xaabbdddd) ? 0 : -1;
  }
  '''

  if cc.compiles(armv6test)
    cdata.set('HAVE_ARMV6', 1)
  endif
endif
# NEON checks are automatically done by the unstable-simd module

# FIXME: make sure it's >= 2.2
ltdl_dep = cc.find_library('ltdl', required : true)
# FIXME: can meson support libtool -dlopen/-dlpreopen things?
#        and do we still want to support this at all?
cdata.set('DISABLE_LIBTOOL_PRELOAD', 1)

if get_option('database') == 'tdb'
  database_dep = dependency('tdb')
elif get_option('database') == 'gdbm'
  database_dep = cc.find_library('gdbm', required : true)
else
  database_dep = dependency('', required: false)
endif

if get_option('ipv6')
  cdata.set('HAVE_IPV6', 1)
endif

if get_option('legacy-database-entry-format')
  cdata.set('ENABLE_LEGACY_DATABASE_ENTRY_FORMAT', 1)
endif

if get_option('stream-restore-clear-old-devices')
  cdata.set('STREAM_RESTORE_CLEAR_OLD_DEVICES', 1)
endif

if get_option('running-from-build-tree')
  cdata.set('HAVE_RUNNING_FROM_BUILD_TREE', 1)
endif

alsa_dep = dependency('alsa', version : '>= 1.0.24', required : get_option('alsa'))
if alsa_dep.found()
  cdata.set('HAVE_ALSA', 1)
  cdata.set('HAVE_ALSA_UCM', 1)
endif

asyncns_dep = dependency('libasyncns', version : '>= 0.1', required : get_option('asyncns'))
if asyncns_dep.found()
  cdata.set('HAVE_LIBASYNCNS', 1)
endif

dbus_dep = dependency('dbus-1', version : '>= 1.4.12', required : get_option('dbus'))
if dbus_dep.found()
  cdata.set('HAVE_DBUS', 1)
endif

gio_dep = dependency('gio-2.0', version : '>= 2.26.0', required : get_option('gsettings'))
if gio_dep.found()
  cdata.set('HAVE_GSETTINGS', 1)
endif

glib_dep = dependency('glib-2.0', version : '>= 2.4.0', required: get_option('glib'))
if glib_dep.found()
  cdata.set('HAVE_GLIB', 1)
endif

gtk_dep = dependency('gtk+-3.0', required : get_option('gtk'))
if gtk_dep.found()
  cdata.set('HAVE_GTK', 1)
endif

have_orcc = false
orcc_args = []
orc_dep = dependency('orc-0.4', version : '>= 0.4.11', required : get_option('orc'))
orcc = find_program('orcc', required : get_option('orc'))
if orc_dep.found() and orcc.found()
  have_orcc = true
  orcc_args = [orcc]
  #orcc_args = [orcc, '--include', 'glib.h']
  cdata.set('HAVE_ORC', 1)
else
  cdata.set('DISABLE_ORC', 1)
endif

samplerate_dep = dependency('samplerate', version : '>= 0.1.0', required : get_option('samplerate'))
if samplerate_dep.found()
  cdata.set('HAVE_LIBSAMPLERATE', 1)
endif

sndfile_dep = dependency('sndfile', version : '>= 1.0.20')

soxr_dep = dependency('soxr', version : '>= 0.1.1', required : get_option('soxr'))
if soxr_dep.found()
  cdata.set('HAVE_SOXR', 1)
endif

libsystemd_dep = dependency('libsystemd', required : get_option('systemd'))
if libsystemd_dep.found()
  cdata.set('HAVE_SYSTEMD_DAEMON', 1)
  cdata.set('HAVE_SYSTEMD_LOGIN', 1)
  cdata.set('HAVE_SYSTEMD_JOURNAL', 1)
endif
systemd_dep = dependency('systemd', required : get_option('systemd'))
if systemd_dep.found() and systemduserunitdir == ''
  systemduserunitdir = systemd_dep.get_pkgconfig_variable('systemduserunitdir')
endif

x11_dep = dependency('x11-xcb', required : get_option('x11'))
if x11_dep.found()
  xcb_dep  = dependency('xcb',  required : true, version : '>= 1.6')
  ice_dep  = dependency('ice',  required : true)
  sm_dep   = dependency('sm',   required : true)
  xtst_dep = dependency('xtst', required : true)
  cdata.set('HAVE_X11', 1)
endif

# Module dependencies

if cc.has_header('sys/soundcard.h')
  cdata.set('HAVE_OSS_OUTPUT', 1)
  cdata.set('HAVE_OSS_WRAPPER', 1)
  cdata.set('PULSEDSP_LOCATION', pulsedsp_location)
endif

if get_option('hal-compat')
  cdata.set('HAVE_HAL_COMPAT', 1)
endif

avahi_dep = dependency('avahi-client', version : '>= 0.6.0', required : get_option('avahi'), disabler : true)
if avahi_dep.found()
  cdata.set('HAVE_AVAHI', 1)
endif

sbc_dep = dependency('sbc', version : '>= 1.0', required : false)
if get_option('bluez5')
  assert(dbus_dep.found(), 'BlueZ requires D-Bus support')
  assert(sbc_dep.found(), 'BlueZ requires SBC support')
  cdata.set('HAVE_SBC', 1)
  cdata.set('HAVE_BLUEZ', 1)
  cdata.set('HAVE_BLUEZ_5', 1)
  if get_option('bluez5-native-headset')
    cdata.set('HAVE_BLUEZ_5_NATIVE_HEADSET', 1)
  endif
  if get_option('bluez5-ofono-headset')
    cdata.set('HAVE_BLUEZ_5_OFONO_HEADSET', 1)
  endif
endif

fftw_dep = dependency('fftw3f', required : get_option('fftw'))
if fftw_dep.found()
  cdata.set('HAVE_FFTW', 1)
endif

jack_dep = dependency('jack', version : '>= 0.117.0', required : get_option('jack'))
if jack_dep.found()
  cdata.set('HAVE_JACK', 1)
endif

lirc_dep = dependency('lirc', required : get_option('lirc'))
if lirc_dep.found()
  cdata.set('HAVE_LIRC', 1)
endif

openssl_dep = dependency('openssl', version : '>= 0.9', required : get_option('openssl'))
if openssl_dep.found()
  cdata.set('HAVE_OPENSSL', 1)
endif

speex_dep = dependency('speexdsp', version : '>= 1.2', required : get_option('speex'))
if speex_dep.found()
  cdata.set('HAVE_SPEEX', 1)
endif

udev_dep = dependency('libudev', version : '>= 143', required : get_option('udev'))
if udev_dep.found()
  cdata.set('HAVE_UDEV', 1)
endif

webrtc_dep = dependency('webrtc-audio-processing', version : '>= 0.2', required : get_option('webrtc-aec'))
if webrtc_dep.found()
  cdata.set('HAVE_WEBRTC', 1)
endif

gst_dep = dependency('gstreamer-1.0', version : '>= 1.14', required : get_option('gstreamer'))
gstapp_dep = dependency('gstreamer-app-1.0', required : get_option('gstreamer'))
gstrtp_dep = dependency('gstreamer-rtp-1.0', required : get_option('gstreamer'))

have_gstreamer = false
if gst_dep.found() and gstapp_dep.found() and gstrtp_dep.found()
  have_gstreamer = true
endif

# These are required for the CMake file generation
cdata.set('PA_LIBDIR', libdir)
cdata.set('PA_INCDIR', includedir)
if glib_dep.found()
  cdata.set('HAVE_GLIB20', 1) # to match the AM_CONDITIONAL for CMake file generation
endif

# Test dependencies

check_dep = dependency('check', version : '>= 0.9.10', required : get_option('tests'))

# Subdirs

subdir('doxygen')
subdir('po')
if get_option('man')
  subdir('man')
endif
subdir('shell-completion/bash')
subdir('shell-completion/zsh')
subdir('src')
subdir('vala')

# Now generate config.h from everything above
configure_file(output : 'config.h', configuration : cdata)

# pkg-config files

pc_cdata = configuration_data()

pc_cdata.set('prefix', prefix)
pc_cdata.set('exec_prefix', prefix)
pc_cdata.set('libdir', libdir)
pc_cdata.set('includedir', includedir)
pc_cdata.set('modlibexecdir', modlibexecdir)
pc_cdata.set('PACKAGE_VERSION', pa_version_str)
pc_cdata.set('PA_MAJORMINOR', pa_version_major_minor)
# FIXME: the line below is wrong. Currently the meson thread dep lacks documentation,
# and doesn't allow introspection, ie. none of get_pkgconfig_variable() or
# get_configtool_variable() work with it, so we have no way to get this flag right,
# unless we do all the work ourselves. See current work in glib, also meson #553.
pc_cdata.set('PTHREAD_LIBS', '-pthread')

pc_files = [
  'libpulse.pc',
  'libpulse-simple.pc',
]

if glib_dep.found()
  pc_files += 'libpulse-mainloop-glib.pc'
endif

foreach file : pc_files
  configure_file(
    input : file + '.in',
    output : file,
    configuration : pc_cdata,
    install_dir : pkgconfigdir)
endforeach

# CMake files

m4 = find_program('m4', required: true)

cmakedir = join_paths(libdir, 'cmake', 'PulseAudio')

cmake_template_file = configure_file(
  input : 'PulseAudioConfig.cmake.in',
  output : 'PulseAudioConfig.cmake.tmp',
  configuration: cdata,
)

custom_target('PulseAudioConfig.cmake',
  input : cmake_template_file,
  output : 'PulseAudioConfig.cmake',
  capture : true,
  command : [m4, '@INPUT@'],
  build_by_default : true,
  install : true,
  install_dir : cmakedir,
)

configure_file(
  input : 'PulseAudioConfigVersion.cmake.in',
  output : 'PulseAudioConfigVersion.cmake',
  configuration: cdata,
  install : true,
  install_dir : cmakedir,
)

############################################################

# Final summary

summary = [
  '',
  '---{ @0@ @1@ }---'.format(meson.project_name(), meson.project_version()),
  '',
  'prefix:                        @0@'.format(prefix),
  'bindir:                        @0@'.format(bindir),
  'libdir:                        @0@'.format(libdir),
  'libexecdir:                    @0@'.format(libexecdir),
  'mandir:                        @0@'.format(mandir),
  'datadir:                       @0@'.format(datadir),
  'sysconfdir:                    @0@'.format(sysconfdir),
  'localstatedir:                 @0@'.format(localstatedir),
  'modlibexecdir:                 @0@'.format(modlibexecdir),
  'alsadatadir:                   @0@'.format(alsadatadir),
  'System Runtime Path:           @0@'.format(cdata.get_unquoted('PA_SYSTEM_RUNTIME_PATH')),
  'System State Path:             @0@'.format(cdata.get_unquoted('PA_SYSTEM_STATE_PATH')),
  'System Config Path:            @0@'.format(cdata.get_unquoted('PA_SYSTEM_CONFIG_PATH')),
  'Bash completions directory:    @0@'.format(bashcompletiondir),
  'Zsh completions directory:     @0@'.format(zshcompletiondir),
  'Compiler:                      @0@ @1@'.format(cc.get_id(), cc.version()),
#  'CFLAGS:                        @0@'.format(${CFLAGS}),
#  'CPPFLAGS:                      @0@'.format(${CPPFLAGS}),
#  'LIBS:                          @0@'.format(${LIBS}),
  '',
  'Enable memfd shared memory:    @0@'.format(cdata.has('HAVE_MEMFD')),
  'Enable X11:                    @0@'.format(x11_dep.found()),
#  'Enable OSS Output:             @0@'.format(${ENABLE_OSS_OUTPUT}),
#  'Enable OSS Wrapper:            @0@'.format(${ENABLE_OSS_WRAPPER}),
#  'Enable EsounD:                 @0@'.format(${ENABLE_ESOUND}),
  'Enable Alsa:                   @0@'.format(alsa_dep.found()),
#  'Enable CoreAudio:              @0@'.format(${ENABLE_COREAUDIO}),
#  'Enable Solaris:                @0@'.format(${ENABLE_SOLARIS}),
#  'Enable WaveOut:                @0@'.format(${ENABLE_WAVEOUT}),
  'Enable GLib 2:                 @0@'.format(glib_dep.found()),
#  'Enable GConf:                  @0@'.format(${ENABLE_GCONF}),
  'Enable GSettings:              @0@'.format(gio_dep.found()),
  'Enable Gtk+ 3:                 @0@'.format(gtk_dep.found()),
  'Enable Avahi:                  @0@'.format(avahi_dep.found()),
  'Enable Jack:                   @0@'.format(jack_dep.found()),
  'Enable Async DNS:              @0@'.format(asyncns_dep.found()),
  'Enable LIRC:                   @0@'.format(lirc_dep.found()),
  'Enable D-Bus:                  @0@'.format(dbus_dep.found()),
  '  Enable BlueZ 5:              @0@'.format(get_option('bluez5')),
  '    Enable native headsets:    @0@'.format(get_option('bluez5-native-headset')),
  '    Enable  ofono headsets:    @0@'.format(get_option('bluez5-ofono-headset')),
  'Enable udev:                   @0@'.format(udev_dep.found()),
  '  Enable HAL->udev compat:     @0@'.format(get_option('hal-compat')),
  'Enable systemd:                @0@'.format(libsystemd_dep.found()),
#  'Enable TCP Wrappers:           @0@'.format(${ENABLE_TCPWRAP}),
  'Enable libsamplerate:          @0@'.format(samplerate_dep.found()),
  'Enable IPv6:                   @0@'.format(get_option('ipv6')),
  'Enable OpenSSL (for Airtunes): @0@'.format(openssl_dep.found()),
  'Enable FFTW:                   @0@'.format(fftw_dep.found()),
  'Enable ORC:                    @0@'.format(have_orcc),
  'Enable GStreamer:              @0@'.format(have_gstreamer),
  'Enable Adrian echo canceller:  @0@'.format(get_option('adrian-aec')),
  'Enable Speex (resampler, AEC): @0@'.format(speex_dep.found()),
  'Enable SoXR (resampler):       @0@'.format(soxr_dep.found()),
  'Enable WebRTC echo canceller:  @0@'.format(webrtc_dep.found()),
  'Enable Gcov coverage:          @0@'.format(get_option('gcov')),
  'Enable man pages:              @0@'.format(get_option('man')),
  'Enable unit tests:             @0@'.format(get_option('tests')),
  '',
  'Database:                      @0@'.format(get_option('database')),
  'Legacy Database Entry Support: @0@'.format(get_option('legacy-database-entry-format')),
  'module-stream-restore:',
  '  Clear old devices:           @0@'.format(get_option('stream-restore-clear-old-devices')),
  'Running from build tree:       @0@'.format(get_option('running-from-build-tree')),
  'System User:                   @0@'.format(cdata.get_unquoted('PA_SYSTEM_USER')),
  'System Group:                  @0@'.format(cdata.get_unquoted('PA_SYSTEM_GROUP')),
  'Access Group:                  @0@'.format(cdata.get_unquoted('PA_ACCESS_GROUP')),
#  'Enable per-user EsounD socket: @0@'.format(${ENABLE_PER_USER_ESOUND_SOCKET}),
#  'Force preopen:                 @0@'.format(${FORCE_PREOPEN}),
#  'Preopened modules:             @0@'.format(${PREOPEN_MODS}),
]

message('\n    '.join(summary))

# Sanity checks

if not speex_dep.found() and not webrtc_dep.found() and not get_option('adrian-aec')
  error('At least one echo canceller implementation must be available!')
endif

if samplerate_dep.found()
  warning('Support for libsamplerate is DEPRECATED')
endif

if host_machine.system() != 'windows'
  if not dbus_dep.found()
    message = [
      'You do not have D-Bus support enabled. It is strongly recommended',
      'that you enable D-Bus support if your platform supports it.',
      'Many parts of PulseAudio use D-Bus, from ConsoleKit interaction',
      'to the Device Reservation Protocol to speak to JACK, Bluetooth',
      'support and even a native control protocol for communicating and',
      'controlling the PulseAudio daemon itself.',
    ]
    warning('\n' + '\n'.join(message))
  endif
  if not udev_dep.found()
    message = [
      'You do not have udev support enabled. It is strongly recommended',
      'that you enable udev support if your platform supports it as it is',
      'the primary method used to detect hardware audio devices (on Linux)',
      'and is thus a critical part of PulseAudio on that platform.',
    ]
    warning('\n' + '\n'.join(message))
  endif
  if not speex_dep.found()
    message = [
      'You do not have speex support enabled. It is strongly recommended',
      'that you enable speex support if your platform supports it as it is',
      'the primary method used for audio resampling and is thus a critical',
      'part of PulseAudio on that platform.',
    ]
    warning('\n' + '\n'.join(message))
  endif
endif
