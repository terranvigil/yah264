threads_dep = dependency('threads')

yah264_cli_exe = executable('y264',
  'yah264_cli.c',
  'ctl_log.c',
  dependencies : [yah264_cli_dep, threads_dep],
  install : true)

# the binary was called yah264 before; the alias keeps existing command lines
# and out-of-tree harnesses that run build/cli/yah264 working
# the build machine's python: a cross file's python entry names a host binary
alias_py = find_program('python3', native : true)

# y264 as an input relinks the alias whenever y264 relinks, and only then
custom_target('yah264-alias',
  input : yah264_cli_exe,
  output : 'yah264',
  command : [alias_py, files('alias.py'), 'build', '@INPUT@', '@OUTPUT@'],
  build_by_default : true)

# a script, not install_symlink: that refuses to replace the real yah264
# binary an earlier install left in bindir, and aborts the install half done
meson.add_install_script(alias_py, files('alias.py'), 'install',
  get_option('bindir'),
  install_tag : 'runtime')
