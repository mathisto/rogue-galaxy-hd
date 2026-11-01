"""Where everything lives: the one module that knows the folder layout.

Phase 1 (this release): the install folder is the program's own folder (the repository or the unpacked release zip):
the source tree, toolchain/ (downloaded build tools), disc/, build/, textures/ and work/ sit side by side, as Install.bat
has always left them. The release that bundles Python, the compiler and the runtime (phase 2) changes this file only:
toolchain_dir(), python() and the build commands (steps.py asks Layout for them) are the seams.
"""
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]      # tools/setup/rgsetup/layout.py -> the program's folder
SERIAL = 'SCUS-97490'
VERSION = 'SCUS_974.90'                         # config/<version>
LAUNCHER = 'Rogue Galaxy.exe'
GAME_EXE = Path('src') / 'roguegalaxy.exe'


def read_release(root=ROOT):
    """The project's version from CMakeLists.txt ('0.1.0'), or 'unknown'."""
    try:
        text = (Path(root) / 'CMakeLists.txt').read_text(encoding='utf-8')
    except OSError:
        return 'unknown'
    m = re.search(r'VERSION\s+(\d+\.\d+\.\d+)', text)
    return m.group(1) if m else 'unknown'


class Layout:
    def __init__(self, dest, root=ROOT, build_dir=None):
        self.root = Path(root).resolve()        # the program (source tree, tools, config)
        self.dest = Path(dest).resolve()        # the install folder
        self.release = read_release(self.root)
        self.in_place = self.dest == self.root
        self._build_dir = Path(build_dir).resolve() if build_dir else None

    # --- the install folder
    @property
    def disc(self):
        return self.dest / 'disc'

    @property
    def build(self):
        return self._build_dir or self.dest / 'build' / 'game'

    @property
    def game_exe(self):
        return self.build / GAME_EXE

    @property
    def launcher(self):
        return self.dest / LAUNCHER

    @property
    def textures(self):
        return self.dest / 'textures' / SERIAL

    @property
    def replacements(self):
        return self.textures / 'replacements'

    @property
    def movies(self):
        return self.textures / 'movies'

    @property
    def hd_work(self):
        return self.dest / 'work' / 'hd'

    @property
    def default_temp(self):
        return self.dest / 'work' / 'setup'

    # --- the program
    @property
    def config(self):
        return self.root / 'config' / VERSION

    @property
    def hashes_json(self):
        return self.config / 'hashes.json'

    @property
    def setup_py(self):
        return self.root / 'setup.py'

    @property
    def hd_tool(self):
        return self.root / 'tools' / 'hd' / 'build_hd_pack.py'

    # --- the toolchain (phase 2 replaces these with the bundled one)
    @property
    def toolchain(self):
        return self.root / 'toolchain'

    def tool_dir(self, name):
        return self.toolchain / name

    def python(self):
        """The interpreter that runs setup.py and the HD tools: the downloaded one, else the running one."""
        own = self.toolchain / 'python' / 'python.exe'
        return own if own.is_file() else Path(sys.executable)

    def build_path(self, use_path_tools=False):
        """The PATH the build sees: the toolchain and Windows only (compilers, Python or CMake settings installed for
        other projects change what the build does)."""
        if use_path_tools:
            return os.environ.get('PATH', '')
        sysroot = os.environ.get('SystemRoot', r'C:\Windows')
        system32 = os.path.join(sysroot, 'System32')
        t = self.toolchain
        return os.pathsep.join([
            str(t / 'llvm-mingw' / 'bin'), str(t / 'cmake' / 'bin'), str(t / 'ninja'), str(t / 'python'),
            str(t / 'git' / 'cmd'), system32, sysroot, os.path.join(system32, 'Wbem'),
            os.path.join(system32, 'WindowsPowerShell', 'v1.0')])

    def setup_py_command(self, *args):
        return [str(self.python()), '-u', str(self.setup_py), *args]

    def hd_command(self, *args):
        return [str(self.python()), '-X', 'utf8', str(self.hd_tool), *args]


# Variables that make the build use something other than the toolchain (install.ps1 removed the same).
POISON_ENV = ('CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH',
              'LIBRARY_PATH', 'INCLUDE', 'LIB', 'CMAKE_GENERATOR', 'CMAKE_GENERATOR_PLATFORM', 'CMAKE_GENERATOR_TOOLSET',
              'CMAKE_PREFIX_PATH', 'CMAKE_TOOLCHAIN_FILE', 'PYTHONHOME', 'PYTHONPATH', 'PYTHONSTARTUP', 'SDL3_DIR',
              'ZLIB_ROOT')


def build_env(layout, use_path_tools=False):
    env = dict(os.environ)
    if not use_path_tools:
        for name in POISON_ENV:
            env.pop(name, None)
    env['PATH'] = layout.build_path(use_path_tools)
    env['PYTHONUTF8'] = '1'
    env['PYTHONUNBUFFERED'] = '1'
    # One library's tests sit deeper than Windows' old path limit; the dependency downloads check out at fixed tags.
    env.update({'GIT_CONFIG_COUNT': '3', 'GIT_CONFIG_KEY_0': 'core.longpaths', 'GIT_CONFIG_VALUE_0': 'true',
                'GIT_CONFIG_KEY_1': 'safe.directory', 'GIT_CONFIG_VALUE_1': '*',
                'GIT_CONFIG_KEY_2': 'advice.detachedHead', 'GIT_CONFIG_VALUE_2': 'false'})
    return env


# What cannot hold the build (install.ps1's Get-PathProblem): the deepest path the build writes is 185 characters below
# the folder, and Windows tools stop at 259 in all.
MAX_ROOT_LENGTH = 60


def path_problem(path):
    """(id, params) of why this folder cannot hold the build, or None."""
    p = str(path)
    if p.startswith('\\\\'):
        return 'err.path_network', {}
    for var in ('OneDrive', 'OneDriveConsumer', 'OneDriveCommercial'):
        cloud = os.environ.get(var)
        if cloud and (p.rstrip('\\') + '\\').lower().startswith(cloud.rstrip('\\').lower() + '\\'):
            return 'err.path_onedrive', {}
    if len(p) > MAX_ROOT_LENGTH:
        return 'err.path_long', {'length': len(p), 'max': MAX_ROOT_LENGTH}
    if re.search(r'[^A-Za-z0-9_.:\\/-]', p):
        return 'err.path_chars', {}
    return None


def classify_dest(dest, root=ROOT):
    """'in_place' (the program's own folder), 'ours' (holds rg-install.json), 'empty', 'missing' or 'foreign'."""
    from . import state
    d = Path(dest)
    if d.resolve() == Path(root).resolve():
        return 'in_place'
    if not d.exists():
        return 'missing'
    if (d / state.NAME).is_file():
        return 'ours'
    try:
        if not any(d.iterdir()):
            return 'empty'
    except OSError:
        return 'foreign'
    return 'foreign'
