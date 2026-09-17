project('yah264', 'c',
  # Keep in step with YAH264_VERSION_* in include/yah264.h, which is what
  # yah264_version() is built from. 0.1.0 is the x264 enum renumbering: it
  # broke the ABI (YAH264_ABI_VERSION 1) and callers must recompile.
  version : '0.1.0',
  license : 'GPL-2.0-or-later',
  default_options : [
    'c_std=c11',
    'warning_level=3',
    'buildtype=debugoptimized',
  ],
  meson_version : '>=1.1.0')

cc = meson.get_compiler('c')
cpu_family = host_machine.cpu_family()

# libm for log2/pow/lround etc. (implicit on macOS/clang, explicit on Linux/gcc).
m_dep = cc.find_library('m', required : false)
threads_dep = dependency('threads')

add_project_arguments('-D_GNU_SOURCE', language : 'c')

# Item C3-10bit: the sample width is a compile-time type (src/common/
# bitdepth.h), so there are two encoder libraries and there always will be;
# what there is no longer is two command lines. EVERY build directory now
# builds BOTH, because one yah264 binary links both archives and picks the
# encoder from the input's bit depth.
#
# The depth therefore moved off the project arguments and onto each library
# TARGET (`-UY264_BIT_DEPTH -DY264_BIT_DEPTH=<d>`; the -U ahead of the -D is
# what lets the target win over the project argument without a
# macro-redefined warning). The 10-bit library force-includes its symbol
# namespace so the two archives share no name.
#
# What `bit_depth` still selects is the depth the TESTS are built at -
# checkasm's kernel families, the unit tests, the in-tree tools - and nothing
# else. It no longer decides what the CLI can encode.
bit_depth = get_option('bit_depth')
add_project_arguments('-DY264_BIT_DEPTH=@0@'.format(bit_depth), language : 'c')
is_8bit = bit_depth == 8
symbols10_h = meson.current_source_dir() / 'src' / 'common' / 'symbols10.h'

inc = include_directories('include', 'src')

lib_sources = [
  'src/common/bitstream.c',
  'src/common/ledger.c',
  'src/common/stgprof.c',
  'src/common/nal.c',
  'src/common/cpu.c',
  'src/common/threadpool.c',
  'src/dsp/pixel.c',
  'src/dsp/transform.c',
  'src/dsp/predict.c',
  'src/dsp/mc.c',
  'src/dsp/deblock.c',
  'src/encoder/params.c',
  'src/encoder/cavlc.c',
  'src/encoder/cabac.c',
  'src/encoder/me.c',
  'src/encoder/macroblock.c',
  'src/encoder/skiporacle.c',
  'src/encoder/deblock.c',
  'src/encoder/set.c',
  'src/encoder/encoder.c',
  'src/encoder/gpu.c',
]

# NEON kernels are uint8/int16-specific (8-bit only). At BD>8 the scalar C path
# is used, so the NEON translation units are simply not compiled in. The test
# is now per LIBRARY rather than per build directory, since both depths are
# compiled here; 16-bit lanes are phase E of docs/high-bit-depth-plan.md.
neon_sources = []
if cpu_family == 'aarch64'
  # NEON is baseline on aarch64; no extra flags needed.
  neon_sources = ['src/dsp/pixel_neon.c', 'src/dsp/mc_neon.c',
                  'src/dsp/transform_neon.c', 'src/dsp/deblock_neon.c',
                  'src/dsp/predict_neon.c']
endif

# nextgpu, the shared Metal compute library (docs/gpu-shared-library-design.md).
# Pinned by subprojects/nextgpu.wrap; the pin is the revision this encoder has
# asserted its conventions against, never a floating branch.
#
# The dependency is OPTIONAL in the strong sense: at 'disabled' nothing here
# references the subproject, and even when linked, ngc_open() returning NULL is
# the presence test the call sites use, so a machine with no Metal takes the CPU
# path with no #ifdef anywhere. The CPU implementation stays complete and
# default either way -- that is the contract the conformance gate rests on.
fs = import('fs')
gpu_opt = get_option('gpu')
want_gpu = gpu_opt == 'enabled' or (gpu_opt == 'auto'
  and host_machine.system() == 'darwin' and cpu_family == 'aarch64')
ngc_dep = dependency('', required : false)
if want_gpu
  # At 'auto' the dependency is optional and the build goes without it if the
  # wrap is missing; meson's own error in that case is about a missing
  # subprojects directory and explains nothing, so say what is actually wrong.
  if not fs.exists('subprojects' / 'nextgpu.wrap')
    if gpu_opt == 'enabled'
      error('-Dgpu=enabled needs subprojects/nextgpu.wrap, which is not in this ' +
            'tree. Fetch it from the yah264 repository, or build without it: ' +
            'the CPU path is complete and every published number used it.')
    endif
    warning('subprojects/nextgpu.wrap is missing; building without GPU offload.')
    want_gpu = false
  endif
endif
if want_gpu
  ngc_dep = dependency('nextgpu', fallback : ['nextgpu', 'ngc_dep'],
                       required : gpu_opt == 'enabled')
endif
lib_deps = [m_dep, threads_dep]

# The VideoToolbox hardware mode (docs/videotoolbox-plan.md).
vt_opt = get_option('videotoolbox')
want_vt = vt_opt == 'enabled' or (vt_opt == 'auto' and host_machine.system() == 'darwin')
if want_vt
  vt_dep = dependency('appleframeworks',
                      modules : ['VideoToolbox', 'CoreMedia', 'CoreVideo', 'CoreFoundation'],
                      required : vt_opt == 'enabled')
  if vt_dep.found()
    lib_sources += ['src/hw/vt.c', 'src/hw/vt_map.c']
    lib_deps += vt_dep
    add_project_arguments('-DY264_HAVE_VT=1', language : 'c')
  else
    lib_sources += ['src/hw/vt_stub.c', 'src/hw/vt_map.c']
  endif
else
  lib_sources += ['src/hw/vt_stub.c', 'src/hw/vt_map.c']
endif
if ngc_dep.found()
  lib_deps += ngc_dep
  add_project_arguments('-DY264_HAVE_GPU=1', language : 'c')
endif

# depth -> the sources and the arguments of one encoder library.
depth_sources = {}
depth_args = {}
foreach d : [8, 10]
  srcs = lib_sources
  args = ['-UY264_BIT_DEPTH', '-DY264_BIT_DEPTH=@0@'.format(d)]
  if d == 8
    srcs += neon_sources
  else
    # Every symbol this library defines carries a _10 suffix - the public API
    # through include/yah264.h (YAH264_API), the internals through this header -
    # so that both archives can be linked into one executable without a
    # definition in one resolving a reference in the other.
    args += ['-include', symbols10_h]
  endif
  depth_sources += { '@0@'.format(d) : srcs }
  depth_args += { '@0@'.format(d) : args }
endforeach

# BOTH forms, and the distinction is worth 12-14% of wall time.
#
# Callers outside this tree need a shared object -- the ffmpeg encoder wrapper is
# the case that prompted it. But when this was plain `library()` the CLI linked
# the dylib too, and every hot call it made crossed a shared-library boundary:
# no cross-TU inlining, and exported symbols reached indirectly. That alone moved
# the speed board from 0.96x/0.95x/1.12x to 1.10x/1.07x/1.15x, which read as a
# whole-encoder regression until the linkage was the thing that got measured.
#
# So: the CLI and the test tools link the STATIC form and are unchanged from
# before packaging existed; the shared form is what gets installed. Hidden
# visibility keeps the dylib's own internal calls direct as well, so an external
# caller is not paying for the export table on every kernel dispatch.
#
# soversion is the ABI version the public header declares (YAH264_ABI_VERSION),
# not the project version: the picture struct's planes became void* and the
# public `pixel` typedef went away, so a caller must recompile across it.
#
# Both depths are built and both are installed, under DIFFERENT names: the two
# are no longer alternative builds of one library, they are two libraries a
# single program links at once.
libyah264_both = both_libraries('yah264',
  depth_sources['8'],
  c_args : depth_args['8'],
  include_directories : inc,
  dependencies : lib_deps,
  version : meson.project_version(),
  soversion : '2',
  gnu_symbol_visibility : 'hidden',
  install : true)

libyah264_10_both = both_libraries('yah264_10',
  depth_sources['10'],
  c_args : depth_args['10'],
  include_directories : inc,
  dependencies : lib_deps,
  version : meson.project_version(),
  soversion : '2',
  gnu_symbol_visibility : 'hidden',
  install : true)

libyah264 = libyah264_both.get_shared_lib()
lib8_static = libyah264_both.get_static_lib()
libyah264_10 = libyah264_10_both.get_shared_lib()
lib10_static = libyah264_10_both.get_static_lib()

libyah264_static = is_8bit ? lib8_static : lib10_static

# ONE header, and it is depth-agnostic now: Y264_BIT_DEPTH defaults to 8 inside
# it, so a consumer of the 8-bit library needs no define. A consumer of the
# 10-bit library defines it to 10 and the header renames its calls to the
# suffixed symbols. The two libraries no longer collide in a prefix, so the
# header is installed once and says nothing about which one you linked.
install_headers('include/yah264.h')

# Two pkg-config files, because there are two libraries. The depth has to reach
# the consumer's compiler: the header renames the public entry points according
# to the Y264_BIT_DEPTH it is compiled with, so `pkg-config --cflags yah264_10`
# is what makes a caller's yah264_encoder_open() resolve to the 10-bit one. The
# `bit_depth` variable is there so a build system can branch on the depth
# without parsing Cflags.
pkg = import('pkgconfig')
pkg.generate(libyah264,
  name : 'yah264',
  filebase : 'yah264',
  description : 'H.264/AVC encoder (8-bit)',
  url : 'https://github.com/terranvigil/yah264',
  subdirs : '.',
  extra_cflags : ['-DY264_BIT_DEPTH=8'],
  variables : ['bit_depth=8'])

pkg.generate(libyah264_10,
  name : 'yah264_10',
  filebase : 'yah264_10',
  description : 'H.264/AVC encoder (10-bit, High 10)',
  url : 'https://github.com/terranvigil/yah264',
  subdirs : '.',
  extra_cflags : ['-DY264_BIT_DEPTH=10'],
  variables : ['bit_depth=10'])

# In-tree consumers (checkasm, tests, tools) take the static form, so the
# binaries this repo's own boards time are linked exactly as they were before
# the library was made installable. They take ONE depth, the one `bit_depth`
# names, with the 10-bit namespace force-included where that depth is 10 so
# they see the names the archive actually defines.
yah264_dep = declare_dependency(
  link_with : libyah264_static,
  dependencies : lib_deps,
  compile_args : is_8bit ? [] : ['-include', symbols10_h],
  include_directories : inc)

# The ONE command line: both archives linked, the encoder picked from the
# input's sample width. The CLI is compiled at Y264_BIT_DEPTH 8, so the names
# the header gives it are the 8-bit library's; the 10-bit entry points it
# declares for itself, suffixed.
yah264_cli_dep = declare_dependency(
  link_with : [lib8_static, lib10_static],
  dependencies : lib_deps,
  compile_args : ['-UY264_BIT_DEPTH', '-DY264_BIT_DEPTH=8'],
  include_directories : inc)

subdir('cli')
subdir('tools/checkasm')
subdir('tools/cabacbench')
subdir('tools/reconcmp')
if want_vt
  subdir('tools/vtprobe')
endif
subdir('tests')
