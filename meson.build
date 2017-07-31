project('pulseaudio', 'c', 'cpp',
        version : '10.99.1',
        meson_version : '>= 0.31.0',
        default_options : [ 'c_std=gnu11', 'cpp_std=c++11' ]
        )

pa_version = meson.project_version()
version_split = pa_version.split('.')
pa_version_major = version_split[0]
pa_version_minor = version_split[1]
pa_version_micro = version_split[2]
pa_version_major_minor = pa_version_major + '.' + pa_version_minor

pa_api_version = 12
pa_protocol_version = 31

apiversion = '1.0'
soversion = 0
# FIXME: this doesn't actually do what we want it to
# maintaining compatibility with the previous libtool versioning
# current = minor * 100 + micro
libversion = '@0@.@1@.0'.format(soversion, pa_version_minor.to_int() * 100 + pa_version_micro.to_int())

prefix = get_option('prefix')
datadir = join_paths(prefix, get_option('datadir'))
localstatedir = join_paths(prefix, get_option('localstatedir'))
sysconfdir = join_paths(prefix, get_option('sysconfdir'))

cc = meson.get_compiler('c')

cdata = configuration_data()
cdata.set_quoted('PACKAGE', 'pulseaudio')
cdata.set_quoted('PACKAGE_NAME', 'pulseaudio')
cdata.set_quoted('PACKAGE_VERSION', pa_version)
cdata.set_quoted('CANONICAL_HOST', host_machine.cpu())
cdata.set_quoted('PA_MACHINE_ID', join_paths(sysconfdir, 'machine-id'))
cdata.set_quoted('PA_MACHINE_ID_FALLBACK', join_paths(localstatedir, 'lib', 'dbus', 'machine-id'))
cdata.set_quoted('PA_SRCDIR', join_paths(meson.current_source_dir(), 'src'))
cdata.set_quoted('PA_BUILDDIR', meson.current_build_dir())
cdata.set_quoted('PA_SOEXT', '.so')
cdata.set_quoted('PA_DEFAULT_CONFIG_DIR', join_paths(sysconfdir, 'pulse'))
cdata.set_quoted('PA_BINARY', join_paths(prefix, get_option('bindir'), 'pulseaudio'))
cdata.set_quoted('PA_SYSTEM_RUNTIME_PATH', join_paths(localstatedir, 'run', 'pulse'))
cdata.set_quoted('PA_SYSTEM_CONFIG_PATH', join_paths(localstatedir, 'lib', 'pulse'))
cdata.set_quoted('PA_SYSTEM_STATE_PATH', join_paths(localstatedir, 'lib', 'pulse'))
cdata.set_quoted('PA_DLSEARCHPATH', join_paths(prefix, get_option('libdir'), 'pulse-' + pa_version_major_minor, 'modules'))
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

# FIXME: support ORC
cdata.set('DISABLE_ORC', 1)

# Module dependencies
udev_dep = dependency('libudev', version : '>= 143', required : false)

# Now generate config.h from everything above
configure_file(output : 'config.h', configuration : cdata)

subdir('src')
