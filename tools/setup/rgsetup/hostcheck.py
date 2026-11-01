"""What this PC can do: free disk, a usable Vulkan graphics card, the CPU, a running game.

Nothing is installed or downloaded to find out. The Vulkan probe loads the graphics driver's own loader (vulkan-1.dll)
and lists its devices, so the answer comes before Real-ESRGAN (which needs the same driver) is downloaded.

Test hook (not a user option): RG_SETUP_FAKE_VULKAN = "none" (no loader), "software" (a CPU device only) or
"Name:discrete" / "Name:integrated" replaces the probe.
"""
import ctypes
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

NO_WINDOW = 0x08000000 if os.name == 'nt' else 0

DEVICE_TYPES = {0: 'other', 1: 'integrated', 2: 'discrete', 3: 'virtual', 4: 'cpu'}


def free_bytes(path):
    p = Path(path)
    while not p.exists():
        if p.parent == p:
            break
        p = p.parent
    return shutil.disk_usage(p).free


def drive_of(path):
    """A key that is equal for two paths on the same drive."""
    p = Path(path).resolve()
    return (p.drive or '/').lower()


def vulkan_devices():
    """[{'name', 'type'}] of the Vulkan devices the driver lists; [] when there is a loader but no device; None when
    there is no Vulkan loader at all."""
    fake = os.environ.get('RG_SETUP_FAKE_VULKAN')
    if fake:
        if fake == 'none':
            return None
        if fake == 'software':
            return [{'name': 'llvmpipe (fake)', 'type': 'cpu'}]
        name, _, kind = fake.partition(':')
        return [{'name': name, 'type': kind or 'discrete'}]
    if os.name != 'nt':
        return None
    try:
        vk = ctypes.WinDLL('vulkan-1.dll')
    except OSError:
        return None
    try:
        p = ctypes.c_void_p
        # VkApplicationInfo { sType=0, pNext, pApplicationName, applicationVersion, pEngineName, engineVersion, apiVersion }
        app = struct.pack('<IxxxxQQIxxxxQII', 0, 0, 0, 0, 0, 0, 0x00400000)   # apiVersion 1.0.0
        app_buf = ctypes.create_string_buffer(app)
        # VkInstanceCreateInfo { sType=1, pNext, flags, pApplicationInfo, layers, ppLayers, extensions, ppExtensions }
        info = struct.pack('<IxxxxQIxxxxQIxxxxQIxxxxQ', 1, 0, 0, ctypes.addressof(app_buf), 0, 0, 0, 0)
        info_buf = ctypes.create_string_buffer(info)
        instance = p()
        vk.vkCreateInstance.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(p)]
        vk.vkCreateInstance.restype = ctypes.c_int
        if vk.vkCreateInstance(ctypes.addressof(info_buf), None, ctypes.byref(instance)) != 0:
            return []
        try:
            count = ctypes.c_uint32(0)
            vk.vkEnumeratePhysicalDevices.argtypes = [p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
            vk.vkEnumeratePhysicalDevices.restype = ctypes.c_int
            if vk.vkEnumeratePhysicalDevices(instance, ctypes.byref(count), None) != 0 or count.value == 0:
                return []
            devices = (p * count.value)()
            vk.vkEnumeratePhysicalDevices(instance, ctypes.byref(count), devices)
            out = []
            vk.vkGetPhysicalDeviceProperties.argtypes = [p, ctypes.c_void_p]
            for i in range(count.value):
                props = ctypes.create_string_buffer(2048)    # VkPhysicalDeviceProperties is 824 bytes
                vk.vkGetPhysicalDeviceProperties(devices[i], props)
                device_type = struct.unpack_from('<I', props.raw, 16)[0]
                name = props.raw[20:20 + 256].split(b'\0')[0].decode('utf-8', 'replace')
                out.append({'name': name, 'type': DEVICE_TYPES.get(device_type, 'other')})
            return out
        finally:
            vk.vkDestroyInstance.argtypes = [p, ctypes.c_void_p]
            vk.vkDestroyInstance(instance, None)
    except Exception:
        return []


def usable_gpu(devices):
    """The best real graphics device, or None: a discrete one before an integrated one; a CPU or virtual one never."""
    if not devices:
        return None
    real = [d for d in devices if d['type'] in ('discrete', 'integrated')]
    if not real:
        return None
    return sorted(real, key=lambda d: 0 if d['type'] == 'discrete' else 1)[0]


def cpu_features():
    """{'sse41': bool, 'avx2': bool} through IsProcessorFeaturePresent; both True where it cannot be asked."""
    if os.name != 'nt':
        return {'sse41': True, 'avx2': True}
    fn = ctypes.windll.kernel32.IsProcessorFeaturePresent
    return {'sse41': bool(fn(37)), 'avx2': bool(fn(40))}


def is_64bit_windows():
    import platform
    return os.name == 'nt' and platform.machine().lower() in ('amd64', 'x86_64')


def game_processes(install_dir):
    """PIDs of Rogue Galaxy processes that hold this install's files: the game or its launcher under the install folder
    (build\\game, build\\game-dev ..., "Rogue Galaxy.exe"). Used before the game's executable is relinked, which Windows
    refuses while it runs. A game running from another folder is not in the way and is not counted (a player may have
    two installs, or play one while the other updates), but a process of the game's name whose path cannot be read is
    counted, to be safe. Found by path under the install folder (Get-Process) and by image name."""
    if os.name != 'nt':
        return []
    pids = set()
    base = str(Path(install_dir).resolve()).rstrip('\\') + '\\'
    try:
        script = ("Get-Process | Where-Object { ($_.Path -and $_.Path.StartsWith('%s', "
                  "[StringComparison]::OrdinalIgnoreCase)) -or (@('roguegalaxy', 'rogue galaxy') -contains "
                  "$_.ProcessName.ToLower()) } | ForEach-Object { '' + $_.Id + '|' + $_.Path }"
                  % base.replace("'", "''"))
        out = subprocess.run(['powershell.exe', '-NoProfile', '-Command', script], capture_output=True, text=True,
                             timeout=60, creationflags=NO_WINDOW).stdout
        for line in out.splitlines():
            pid, _, path = line.partition('|')
            if pid.isdigit() and not path.strip() and int(pid) != os.getpid():
                pids.add(int(pid))               # named like the game, path hidden from us: assume it is in the way
                continue
            if not path.lower().startswith(base.lower()):
                continue                         # the game, but running from another folder
            rel = path[len(base):].lower().replace('/', '\\')
            # a game build or test executable (build\game, build\game-dev ...) or the launcher; not the toolchain, the
            # build tools under build\tools, or this installer's own Python
            is_game = (rel.startswith('build\\') and not rel.startswith(('build\\tools\\', 'build\\_'))
                       and rel.endswith('.exe')) or rel == 'rogue galaxy.exe'
            if pid.isdigit() and is_game and int(pid) != os.getpid():
                pids.add(int(pid))
    except (OSError, subprocess.SubprocessError):
        pass
    return sorted(pids)


def python_ok(minimum=(3, 11)):
    return sys.version_info >= minimum
