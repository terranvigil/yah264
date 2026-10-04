test_bitstream = executable('test_bitstream',
  'test_bitstream.c',
  dependencies : yah264_dep)
test('bitstream', test_bitstream)

test_nal = executable('test_nal',
  'test_nal.c',
  dependencies : yah264_dep)
test('nal', test_nal)

test_transform = executable('test_transform',
  'test_transform.c',
  dependencies : yah264_dep)
test('transform', test_transform)

test_cavlc = executable('test_cavlc',
  'test_cavlc.c',
  dependencies : yah264_dep)
test('cavlc', test_cavlc)

test_mc = executable('test_mc',
  'test_mc.c',
  dependencies : yah264_dep)
test('mc', test_mc)

test_cabac = executable('test_cabac',
  'test_cabac.c',
  dependencies : yah264_dep)
test('cabac', test_cabac)

test_trellis = executable('test_trellis',
  'test_trellis.c',
  dependencies : yah264_dep)
test('trellis', test_trellis)

test_threadpool = executable('test_threadpool',
  'test_threadpool.c',
  dependencies : yah264_dep)
test('threadpool', test_threadpool)

test_api = executable('test_api',
  'test_api.c',
  dependencies : yah264_dep)
test('api', test_api)

test_denoise = executable('test_denoise',
  'test_denoise.c',
  dependencies : yah264_dep)
test('denoise', test_denoise)

test_cutscan = executable('test_cutscan',
  'test_cutscan.c',
  dependencies : yah264_dep)
test('cutscan', test_cutscan)

# The rate controller, every mode, through record/replay on a synthetic clip
# (no corpus): each replay must reproduce its recording's slice QPs.
rc_python = find_program('python3', required : false)
if rc_python.found()
  test('rc_replay', rc_python,
    args : [files('rc_selftest.py'), yah264_cli_exe],
    timeout : 120)
endif
