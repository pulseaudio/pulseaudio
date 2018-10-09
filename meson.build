project('pulseaudio', 'c', 'cpp',
        version : run_command(find_program('git-version-gen'), join_paths(meson.current_source_dir(), '.tarball-version')).stdout().strip(),
        meson_version : '>= 0.44.0',
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
pa_protocol_version = 33

apiversion = '1.0'
soversion = 0
# FIXME: this doesn't actually do what we want it to
# maintaining compatibility with the previous libtool versioning
# current = minor * 100 + micro
libversion = '@0@.@1@.0'.format(soversion, pa_version_minor.to_int() * 100 + pa_version_micro.to_int())

# Paths

prefix = get_option('prefix')
if not prefix.startswith('/')
  error('Prefix is not absolute: "@0@"'.format(prefix))
endif

bindir = join_paths(prefix, get_option('bindir'))
libdir = join_paths(prefix, get_option('libdir'))
datadir = join_paths(prefix, get_option('datadir'))
localstatedir = join_paths(prefix, get_option('localstatedir'))
sysconfdir = join_paths(prefix, get_option('sysconfdir'))

pulsedspdir = get_option('pulsedspdir')
if pulsedspdir == ''
  join_paths(libdir, 'pulseaudio')
endif

# Configuration data

cc = meson.get_compiler('c')

cdata = configuration_data()
cdata.set_quoted('PACKAGE', 'pulseaudio')
cdata.set_quoted('PACKAGE_NAME', 'pulseaudio')
cdata.set_quoted('PACKAGE_VERSION', pa_version_str)
cdata.set_quoted('CANONICAL_HOST', host_machine.cpu())
cdata.set('PA_MAJOR', pa_version_major)
cdata.set('PA_MINOR', pa_version_minor)
cdata.set('PA_API_VERSION', pa_api_version)
cdata.set('PA_PROTOCOL_VERSION', pa_protocol_version)
cdata.set_quoted('PA_MACHINE_ID', join_paths(sysconfdir, 'machine-id'))
cdata.set_quoted('PA_MACHINE_ID_FALLBACK', join_paths(localstatedir, 'lib', 'dbus', 'machine-id'))
cdata.set_quoted('PA_SRCDIR', join_paths(meson.current_source_dir(), 'src'))
cdata.set_quoted('PA_BUILDDIR', meson.current_build_dir())
cdata.set_quoted('PA_SOEXT', '.so')
cdata.set_quoted('PA_DEFAULT_CONFIG_DIR', join_paths(sysconfdir, 'pulse'))
cdata.set_quoted('PA_BINARY', join_paths(bindir, 'pulseaudio'))
cdata.set_quoted('PA_SYSTEM_RUNTIME_PATH', join_paths(localstatedir, 'run', 'pulse'))
cdata.set_quoted('PA_SYSTEM_CONFIG_PATH', join_paths(localstatedir, 'lib', 'pulse'))
cdata.set_quoted('PA_SYSTEM_STATE_PATH', join_paths(localstatedir, 'lib', 'pulse'))
cdata.set_quoted('PA_DLSEARCHPATH', join_paths(libdir, 'pulse-' + pa_version_major_minor, 'modules'))
cdata.set_quoted('PA_SYSTEM_USER', get_option('system_user'))
cdata.set_quoted('PA_SYSTEM_GROUP', get_option('system_group'))
cdata.set_quoted('PA_ACCESS_GROUP', get_option('access_group'))
cdata.set_quoted('PA_CFLAGS', 'Not yet supported on meson')
cdata.set_quoted('PA_ALSA_PATHS_DIR', join_paths(datadir, 'pulseaudio', 'alsa-mixer', 'paths'))
cdata.set_quoted('PA_ALSA_PROFILE_SETS_DIR', join_paths(datadir, 'pulseaudio', 'alsa-mixer', 'profile-sets'))
cdata.set_quoted('DESKTOPFILEDIR', join_paths(datadir, 'applications'))

# Headers

check_headers = [
  'arpa/inet.h',
  'cpuid.h',
  'execinfo.h',
  'grp.h',
  'langinfo.h',
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
  'sys/capability.h',
  'sys/ioctl.h',
  'sys/mman.h',
  'sys/prctl.h',
  'sys/resource.h',
  'sys/select.h',
  'sys/socket.h',
  'sys/un.h',
  'sys/wait.h',
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

# Functions

check_functions = [
  'accept4',
  'clock_gettime',
  'fchmod',
  'fchown',
  'fork',
  'fstat',
  'getaddrinfo',
  'getgrgid_r',
  'getpwnam_r',
  'gettimeofday',
  'getuid',
  'lstat',
  'memfd_create',
  'mlock',
  'nanosleep',
  'paccept',
  'pipe',
  'pipe2',
  'posix_madvise',
  'readlink',
  'setegid',
  'seteuid',
  'setregid',
  'setreuid',
  'setresgid',
  'setresuid',
  'setsid',
  'sig2str',
  'sigaction',
  'strtod_l',
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

shm_dep = cc.find_library('rt', required : false)
if shm_dep.found()
  cdata.set('HAVE_SHM_OPEN', 1)
endif

if cc.has_function('SYS_memfd_create', prefix : '#include <sys/syscall.h>')
  cdata.set('HAVE_MEMFD', 1)
endif

# Types

# FIXME: do we ever care about gid_t not being defined / smaller than an int?
cdata.set('GETGROUPS_T', 'gid_t')

# Include paths

configinc = include_directories('.')
topinc = include_directories('src')

# CFLAGS

pa_c_args = ['-DHAVE_CONFIG_H', '-D_GNU_SOURCE']
server_c_args = ['-D__INCLUDED_FROM_PULSE_AUDIO']
cdata.set('MESON_BUILD', 1)

# Core Dependencies

libm_dep = cc.find_library('m', required : true)
thread_dep = dependency('threads')
cap_dep = cc.find_library('cap', required : false)

if get_option('database') == 'tdb'
  database_dep = dependency('tdb')
elif get_option('database') == 'gdbm'
  database_dep = cc.find_library('gdbm', required : true)
endif

atomictest = '''void func() {
  volatile int atomic = 2;
  __sync_bool_compare_and_swap (&atomic, 2, 3);
}
'''
if cc.compiles(atomictest)
  cdata.set('HAVE_ATOMIC_BUILTINS', true)
else
  # FIXME: check if we need libatomic_ops
endif

# FIXME: make sure it's >= 2.2
ltdl_dep = cc.find_library('ltdl', required : true)
# FIXME: can meson support libtool -dlopen/-dlpreopen things?
#        and do we still want to support this at all?
cdata.set('DISABLE_LIBTOOL_PRELOAD', 1)

sndfile_dep = dependency('sndfile', version : '>= 1.0.20')

dbus_dep = dependency('dbus-1', version : '>= 1.4.12', required : false)
if dbus_dep.found()
  cdata.set('HAVE_DBUS', 1)
endif

x11_dep = dependency('x11-xcb', required : false)
if x11_dep.found()
  cdata.set('HAVE_X11', 1)
endif

alsa_dep = dependency('alsa', version : '>= 1.0.24', required : false)
if alsa_dep.found()
  cdata.set('HAVE_ALSA_UCM', 1)
endif

systemd_dep = dependency('libsystemd', required : false)
if systemd_dep.found()
  cdata.set('HAVE_SYSTEMD_DAEMON', 1)
  cdata.set('HAVE_SYSTEMD_LOGIN', 1)
  cdata.set('HAVE_SYSTEMD_JOURNAL', 1)
endif

# FIXME: support ORC
cdata.set('DISABLE_ORC', 1)

# Module dependencies

if cc.has_header('sys/soundcard.h')
  cdata.set('HAVE_OSS_OUTPUT', 1)
  cdata.set('HAVE_OSS_WRAPPER', 1)
  cdata.set_quoted('PULSEDSP_LOCATION',	pulsedspdir)
endif

# X11 deps, we don't define any HAVE_XXX for these one
ice_dep = dependency('ice', required : false)
sm_dep = dependency('sm', required: false)
xtst_dep = dependency('xtst', required: false)

avahi_dep = dependency('avahi-client', version : '>= 0.6.0', required : false)
if avahi_dep.found()
  cdata.set('HAVE_AVAHI', 1)
endif

bluez_dep = dependency('bluez', version : '>= 4.101', required : false)
if bluez_dep.found()
  cdata.set('HAVE_BLUEZ_5_NATIVE_HEADSET', 1)
  cdata.set('HAVE_BLUEZ_5_OFONO_HEADSET', 1) # TODO This should be optional
endif

jack_dep = dependency('jack', version : '>= 0.117.0', required : false)
if jack_dep.found()
  cdata.set('HAVE_JACK', 1)
endif

fftw_dep = dependency('fftw3f', required : false)
if fftw_dep.found()
  cdata.set('HAVE_FFTW', 1)
endif

lirc_dep = dependency('lirc', required : false)
if lirc_dep.found()
  cdata.set('HAVE_LIRC', 1)
endif

openssl_dep = dependency('openssl', version : '>= 0.9', required : false)
if openssl_dep.found()
  cdata.set('HAVE_OPENSSL', 1)
endif

sbc_dep = dependency('sbc', version : '>= 1.0', required : false)
if sbc_dep.found()
  cdata.set('HAVE_SBC', 1)
endif
if sbc_dep.found() and dbus_dep.found()
  cdata.set('HAVE_BLUEZ', 1)
  cdata.set('HAVE_BLUEZ_5', 1)
endif

speex_dep = dependency('speexdsp', version : '>= 1.2', required : false)
if speex_dep.found()
  cdata.set('HAVE_SPEEX', 1)
endif

udev_dep = dependency('libudev', version : '>= 143', required : false)
if udev_dep.found()
  cdata.set('HAVE_UDEV', 1)
endif

webrtc_dep = dependency('webrtc-audio-processing', version : '>= 0.2', required : false)
if webrtc_dep.found()
  cdata.set('HAVE_WEBRTC', 1)
endif

# Now generate config.h from everything above
configure_file(output : 'config.h', configuration : cdata)

subdir('man')
subdir('src')
