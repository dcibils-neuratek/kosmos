# Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **LLVM, as Mesa's build is handed it on Kosmos** (`docs/gpu3d.md` G1).
# Mesa looks for LLVM with `llvm-config`, and falls back to a subproject
# called `llvm` that declares `dep_llvm` - the door Windows builds use for a
# prebuilt LLVM. Kosmos has no `llvm-config` of its own: its LLVM is the
# static libraries `tools/llvm_build.py` built, and this says where they
# are. `tools/mesa_build.py` asks for it with `--force-fallback-for=llvm`
# and passes the two options: the libraries, one path a line, and the two
# include directories - the release's and the build's generated one.
project('llvm', ['cpp'], version : '23.1.2')

fs = import('fs')
cpp = meson.get_compiler('cpp')

_libs = []
foreach path : fs.read(get_option('libs')).strip().split('\n')
  _libs += path
endforeach

_args = []
foreach dir : get_option('include').split(':')
  _args += '-isystem' + dir
endforeach

# Built without RTTI (`runtime/cxx/llvm.cmake`), which Mesa checks
# against its own C++ flags.
has_rtti = false

dep_llvm = declare_dependency(
  compile_args : _args,
  link_args : ['-Wl,--start-group'] + _libs + ['-Wl,--end-group'],
  version : meson.project_version(),
)
