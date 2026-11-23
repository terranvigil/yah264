test_bitstream = executable('test_bitstream',
  'test_bitstream.c',
  dependencies : yah264_dep)
test('bitstream', test_bitstream, timeout : test_timeout_floor)

test_nal = executable('test_nal',
  'test_nal.c',
  dependencies : yah264_dep)
test('nal', test_nal, timeout : test_timeout_floor)

test_transform = executable('test_transform',
  'test_transform.c',
  dependencies : yah264_dep)
test('transform', test_transform, timeout : test_timeout_floor)

test_cavlc = executable('test_cavlc',
  'test_cavlc.c',
  dependencies : yah264_dep)
test('cavlc', test_cavlc, timeout : test_timeout_floor)

test_mc = executable('test_mc',
  'test_mc.c',
  dependencies : yah264_dep)
test('mc', test_mc, timeout : test_timeout_floor)

test_cabac = executable('test_cabac',
  'test_cabac.c',
  dependencies : yah264_dep)
test('cabac', test_cabac, timeout : test_timeout_floor)

test_trellis = executable('test_trellis',
  'test_trellis.c',
  dependencies : yah264_dep)
test('trellis', test_trellis, timeout : test_timeout_floor)

test_threadpool = executable('test_threadpool',
  'test_threadpool.c',
  dependencies : yah264_dep)
test('threadpool', test_threadpool, timeout : test_timeout_floor)

test_api = executable('test_api',
  'test_api.c',
  dependencies : yah264_dep)
test('api', test_api, timeout : test_timeout_floor)

test_control = executable('test_control',
  'test_control.c', '../cli/ctl_log.c',
  dependencies : yah264_dep)
test('control', test_control, timeout : test_timeout_floor)

# these two drive the encoder through Y264_* knobs, which only a dev_knobs
# build reads
if get_option('dev_knobs')
  test_aqdc = executable('test_aqdc',
    'test_aqdc.c',
    dependencies : yah264_dep)
  test('aqdc', test_aqdc, timeout : test_timeout_floor)
endif

test_deblockqp = executable('test_deblockqp',
  'test_deblockqp.c',
  dependencies : yah264_dep)
test('deblockqp', test_deblockqp, timeout : test_timeout_floor)

test_denoise = executable('test_denoise',
  'test_denoise.c',
  dependencies : yah264_dep)
test('denoise', test_denoise, timeout : test_timeout_floor)

test_darkaq = executable('test_darkaq',
  'test_darkaq.c',
  dependencies : yah264_dep)
test('darkaq', test_darkaq, timeout : test_timeout_floor)

test_cutscan = executable('test_cutscan',
  'test_cutscan.c',
  dependencies : yah264_dep)
test('cutscan', test_cutscan, timeout : test_timeout_floor)

# The rate controller, every mode, through record/replay on a synthetic clip
# (no corpus): each replay must reproduce its recording's slice QPs.
rc_python = find_program('python3', required : false)
if rc_python.found() and get_option('dev_knobs')
  test('rc_replay', rc_python,
    args : [files('rc_selftest.py'), yah264_cli_exe],
    timeout : 120)
  # The dark-cut gate's fade test marks the same frames at 8 and 10 bits.
  test('fade', rc_python,
    args : [files('fade_check.py'), yah264_cli_exe],
    timeout : 120)
endif

# A GOP-parallel encode from a file must code every input frame when the
# schedule reorders GOPs behind a reader that has already started. Spliced
# from CIF corpus clips when tests/corpus has them, else synthetic.
if rc_python.found()
  test('gop_drop', rc_python,
    args : [files('gop_drop_check.py'), yah264_cli_exe,
            meson.project_source_root() / 'tests' / 'corpus'],
    timeout : 300)
endif

# narrow and short pictures (one or two macroblocks wide, one row tall): the
# row wavefront's rate estimate has the fewest columns to snapshot from there,
# and a sanitizer build turns this into the out-of-range context check
if rc_python.found()
  test('narrow', rc_python,
    args : [files('narrow_check.py'), yah264_cli_exe],
    timeout : 300)
endif

# --ref 15 and 16 with flat B: P frames keep all 15 references the SPS
# declares, and a B's list 0 stops at 14 because its future anchor holds a slot
if rc_python.found()
  test('refwindow', rc_python,
    args : [files('refwindow_check.py'), yah264_cli_exe],
    timeout : 300)
endif

# Live control through the CLI: bad --control logs refused before a byte is
# written, a live --control-fd run replayed from its --control-out log
if rc_python.found()
  test('control_cli', rc_python,
    args : [files('control_check.py'), yah264_cli_exe],
    timeout : 300)
endif

# --frame-stats against the stream: one line per frame whose bytes cover all
# of its slices and, under --tff/--bff, both of its fields
if rc_python.found()
  test('frame_stats', rc_python,
    args : [files('frame_stats_check.py'), yah264_cli_exe],
    timeout : 300)
endif

# The CLI on damaged or awkward input files: a truncated Y4M, a missing
# or empty stats file, a FIFO given as one.
if rc_python.found()
  test('cli_robust', rc_python,
    args : [files('cli_robust_check.py'), yah264_cli_exe],
    timeout : 300)
endif
