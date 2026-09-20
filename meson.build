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
  'src/encoder/sei.c',
  'src/encoder/encoder.c',
  'src/encoder/gpu.c',
]

# ---- the SIMD tiers --------------------------------------------------------
#
# `simd` is a BUILD-time cap and nothing more. What runs is the CPU's own
# feature word, narrowed at runtime by Y264_SIMD_FORCE and YAH264_NO_ASM; this
# decides what is in the binary for those to narrow. The macros it resolves to
# are read by src/common/cpu.h (Y264_CPU_SIMD_ANY, y264_cpu_tier) and by
# src/dsp/arch.h, and they are project arguments as well as per-library ones
# because tests/ and tools/ compile the same headers and must see the same
# tiers the library was built with.
simd_opt = get_option('simd')
want_avx512 = get_option('avx512')
simd_rank = {'none' : 0, 'sse4' : 1, 'avx2' : 2, 'avx512' : 3}

have_neon = false
have_sse4 = false
have_avx2 = false
have_avx512 = false

if cpu_family == 'aarch64'
  if simd_opt == 'sse4' or simd_opt == 'avx2' or simd_opt == 'avx512'
    error('-Dsimd=@0@ names an x86-64 tier. On aarch64 there is one tier, so the choices are auto and none.'.format(simd_opt))
  endif
  have_neon = simd_opt == 'auto'
elif cpu_family == 'x86_64'
  simd_cap = simd_opt
  if simd_opt == 'auto'
    simd_cap = want_avx512 ? 'avx512' : 'avx2'
  endif
  if simd_cap == 'avx512' and not want_avx512
    error('-Dsimd=avx512 also needs -Davx512=true: the tier is a gated ' +
          'experiment and that option is the gate.')
  endif
  have_sse4   = simd_rank[simd_cap] >= 1
  have_avx2   = simd_rank[simd_cap] >= 2
  have_avx512 = simd_rank[simd_cap] >= 3
elif simd_opt != 'auto' and simd_opt != 'none'
  error('-Dsimd=@0@ has no kernels on @1@.'.format(simd_opt, cpu_family))
endif

simd_tiers = {'NEON' : have_neon, 'SSE4' : have_sse4,
              'AVX2' : have_avx2, 'AVX512' : have_avx512}

# NEON kernels are uint8/int16-specific (8-bit only). At BD>8 the scalar C path
# is used, so the NEON translation units are simply not compiled in. The test
# is now per LIBRARY rather than per build directory, since both depths are
# compiled here; 16-bit lanes are phase E of docs/high-bit-depth-plan.md.
neon_sources = []
if have_neon
  # NEON is baseline on aarch64; no extra flags needed.
  neon_sources = ['src/dsp/pixel_neon.c', 'src/dsp/mc_neon.c',
                  'src/dsp/transform_neon.c', 'src/dsp/deblock_neon.c',
                  'src/dsp/predict_neon.c']
endif

# The x86 tiers are NOT baseline, so each one is its own translation unit with
# its own -m flags and its own static library, and the dispatching .c files
# stay baseline x86-64: a file compiled with -mavx2 may emit an AVX2
# instruction anywhere in itself, dispatch test included, so a dispatcher built
# that way would fault before it could decide not to.
#
# The flags are -m<feature> and never the whole-architecture flag: that one
# raises the TUNING as well, and on a tier meant to run everywhere it quietly
# bakes in one vendor's scheduling. hygiene_check.sh refuses it in a meson
# file for that reason.
#
# The family lists are the per-family, per-depth opt-in: a family appears at a
# tier when it HAS files there. `pixel` carries kernels at sse4 and avx2 since
# wave 1, `mc` since wave 2, `transform` since wave 3a, and `deblock` and
# `predict` since wave 3b. What the lists bought while a family was still an
# empty placeholder is that the shape -- three libraries, their flags, their
# macros -- was compiled by every x86 build rather than written blind on the
# day its first kernel landed. The
# 10-bit library opts in to nothing, exactly as NEON does, because the lanes
# are 8-bit.
x86_all_families = ['pixel', 'mc', 'transform', 'predict', 'deblock']
x86_tier_families = {
  'sse4'   : ['pixel', 'mc', 'transform', 'predict', 'deblock'],
  'avx2'   : ['pixel', 'mc', 'transform', 'predict', 'deblock'],
  'avx512' : ['pixel', 'mc'],
}
x86_tier_args = {
  'sse4'   : ['-msse4.2'],
  'avx2'   : ['-mavx2', '-mfma', '-mbmi2'],
  'avx512' : ['-mavx512f', '-mavx512bw', '-mavx512vl'],
}
x86_tiers = []
if have_sse4
  x86_tiers += 'sse4'
endif
if have_avx2
  x86_tiers += 'avx2'
endif
if have_avx512
  x86_tiers += 'avx512'
endif

# The tier and family macros for THIS build's depth, so tests/ and tools/ --
# which compile the headers themselves -- agree with the library.
foreach t, on : simd_tiers
  tv = '0'
  if on and is_8bit
    tv = '1'
  endif
  add_project_arguments('-DY264_HAVE_' + t + '=' + tv, language : 'c')
endforeach
x86_families_built = []
foreach f : x86_all_families
  foreach t : x86_tiers
    if f in x86_tier_families[t] and not (f in x86_families_built)
      x86_families_built += f
    endif
  endforeach
endforeach
foreach f : x86_families_built
  if is_8bit
    add_project_arguments('-DY264_SIMD_' + f.to_upper() + '=1', language : 'c')
  endif
endforeach

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
  # Which tiers and which families THIS library holds. Every tier's kernels
  # are 8-bit lanes today, so the 10-bit library is told it has none rather
  # than being left to infer it from the architecture -- that inference is
  # what made the old dispatch aarch64-only by construction.
  foreach t, on : simd_tiers
    tv = '0'
    if on and d == 8
      tv = '1'
    endif
    args += ['-UY264_HAVE_' + t, '-DY264_HAVE_' + t + '=' + tv]
  endforeach
  foreach f : x86_all_families
    args += '-UY264_SIMD_' + f.to_upper()
    if d == 8 and f in x86_families_built
      args += '-DY264_SIMD_' + f.to_upper() + '=1'
    endif
  endforeach
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

# One static library per x86 tier, built with the 8-bit library's exact
# arguments plus that tier's -m flags, and folded into the 8-bit library as
# objects. `pic : true` because those objects go into the shared form as well,
# and hidden visibility because a kernel is an internal name: an external
# caller reaches the encoder, never a SAD.
x86_objects = []
foreach t : x86_tiers
  tier_srcs = []
  foreach f : x86_tier_families[t]
    tier_srcs += 'src/dsp/x86/' + f + '_' + t + '.c'
  endforeach
  x86_tier_lib = static_library('yah264_x86_' + t,
    tier_srcs,
    c_args : depth_args['8'] + x86_tier_args[t],
    include_directories : inc,
    dependencies : [m_dep],
    pic : true,
    gnu_symbol_visibility : 'hidden',
    install : false)
  x86_objects += x86_tier_lib.extract_all_objects(recursive : false)
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
# 3 moves the parameter struct: it carries its own size in a new first field.
# The soname bump is what stops an ffmpeg built against ABI 2 from loading an
# ABI 3 install at all. yah264_encoder_open's size check is what catches the
# same mistake when the soname did NOT move -- a field added mid-struct inside
# one ABI, which is the shape the 2026-09-19 board tripped over.
#
# Both depths are built and both are installed, under DIFFERENT names: the two
# are no longer alternative builds of one library, they are two libraries a
# single program links at once.
libyah264_both = both_libraries('yah264',
  depth_sources['8'],
  objects : x86_objects,
  c_args : depth_args['8'],
  include_directories : inc,
  dependencies : lib_deps,
  version : meson.project_version(),
  soversion : '3',
  gnu_symbol_visibility : 'hidden',
  install : true)

libyah264_10_both = both_libraries('yah264_10',
  depth_sources['10'],
  c_args : depth_args['10'],
  include_directories : inc,
  dependencies : lib_deps,
  version : meson.project_version(),
  soversion : '3',
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
