threads_dep = dependency('threads')

yah264_cli_exe = executable('y264',
  'yah264_cli.c',
  'ctl_log.c',
  dependencies : [yah264_cli_dep, threads_dep],
  install : true)

# the binary was called yah264 before; the alias keeps existing command lines
# and out-of-tree harnesses that point at build/cli/yah264 working
install_symlink('yah264',
  pointing_to : 'y264',
  install_dir : get_option('bindir'))

custom_target('yah264-alias',
  output : 'yah264',
  command : [find_program('ln'), '-sf', 'y264', '@OUTPUT@'],
  depends : yah264_cli_exe,
  build_by_default : true)
