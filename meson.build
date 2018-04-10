project('encoder', 'c')

cc = meson.get_compiler('c')

lame_dep = cc.find_library('mp3lame')
thread_dep = dependency('threads')
args = []

executable('encoder', 'main.c',
  dependencies: [lame_dep, thread_dep],
  c_args: args
)
