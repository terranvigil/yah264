project('next264', 'c',
  # Keep in step with NEXT264_VERSION_* in include/next264.h, which is what
  # next264_version() is built from. 0.1.0 is the x264 enum renumbering: it
  # broke the ABI (NEXT264_ABI_VERSION 1) and callers must recompile.
  version : '0.1.0',
  license : 'BSD-2-Clause',
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

bit_depth = get_option('bit_depth')
add_project_arguments('-DN264_BIT_DEPTH=@0@'.format(bit_depth), language : 'c')

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
# is used, so the NEON translation units are simply not compiled in.
if cpu_family == 'aarch64' and bit_depth == 8
  # NEON is baseline on aarch64; no extra flags needed.
  lib_sources += ['src/dsp/pixel_neon.c', 'src/dsp/mc_neon.c',
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
gpu_opt = get_option('gpu')
want_gpu = gpu_opt == 'enabled' or (gpu_opt == 'auto'
  and host_machine.system() == 'darwin' and cpu_family == 'aarch64')
ngc_dep = dependency('', required : false)
if want_gpu
  ngc_dep = dependency('nextgpu', fallback : ['nextgpu', 'ngc_dep'],
                       required : gpu_opt == 'enabled')
endif
lib_deps = [m_dep, threads_dep]
if ngc_dep.found()
  lib_deps += ngc_dep
  add_project_arguments('-DN264_HAVE_GPU=1', language : 'c')
endif

libnext264 = static_library('next264',
  lib_sources,
  include_directories : inc,
  dependencies : lib_deps,
  install : false)

next264_dep = declare_dependency(
  link_with : libnext264,
  dependencies : lib_deps,
  include_directories : inc)

subdir('cli')
subdir('tools/checkasm')
subdir('tools/cabacbench')
subdir('tests')
