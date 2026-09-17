# One source per kernel family plus the driver. Adding a family is one line
# here and one line in main.c's `families`; adding a KERNEL is one row in its
# family's table and touches nothing else, which is what lets several kernel
# worktrees land at once without colliding.
checkasm_sources = [
  'main.c',
  'pixel.c',
  'transform.c',
  'mc.c',
  'predict.c',
  'deblock.c',
]

checkasm = executable('checkasm',
  checkasm_sources,
  dependencies : yah264_dep,
  install : false)

test('checkasm', checkasm, timeout : 60)
