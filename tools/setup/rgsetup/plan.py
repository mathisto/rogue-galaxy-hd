"""What the player can choose (presets and components) and what it costs (disk and time, from measured numbers).

Presets and components (v1):

    Faithful     the game only
    Recommended  the game + HD textures + the modern font and Xbox button prompts
    Everything   Recommended + HD movies

Every preset is only a set of components; the window expands it to per-component choices. Components never depend on
each other, and the installer never takes one away (a later run with fewer components keeps what is there).

The numbers are those of docs/HD-BUILD.md and tools/hd/build_hd_pack.py (GB_TEXTURES, GB_MOVIES, EST_*), and of the
Install.bat measurements (README): disk in bytes, time in minutes on the author's PC (Ryzen 9 7900X, RTX 5070 Ti, NVMe).
A weaker graphics card is several times slower: the time is a guide, the disk figure is not.
"""
import importlib.util
import os

GiB = 1 << 30

COMPONENT_ORDER = ('textures', 'movies', 'prompts_font')

PRESETS = {
    'faithful': (),
    'recommended': ('textures', 'prompts_font'),
    'everything': ('textures', 'movies', 'prompts_font'),
}

# peak: disk the component needs while it is built (work files kept); final: what stays once the work files are
# deleted (--clean-work). minutes: (low, high). gpu: needs a usable Vulkan graphics card. tools: pinned downloads.
COMPONENTS = {
    'textures': {'step': 'hd_textures', 'gpu': True, 'tools': ('realesrgan', 'pylibs'),
                 'peak': 11 * GiB, 'final': 6 * GiB, 'minutes': (20, 30)},
    'movies': {'step': 'hd_movies', 'gpu': True, 'tools': ('realesrgan', 'ffmpeg'),
               'peak': 11 * GiB, 'final': 7 * GiB, 'minutes': (90, 90)},
    'prompts_font': {'step': 'prompts_font', 'gpu': False, 'tools': (), 'peak': 0, 'final': 0, 'minutes': (0, 0)},
}

# The game itself (install.ps1 asked for 9 GB for the build, and 7 more when the disc is not extracted yet).
DISC_BYTES = 7 * GiB          # the extracted disc: 6.3 GiB
BUILD_BYTES = 9 * GiB         # build/ (about 4 GB of generated C and objects) and toolchain/
MARGIN_BYTES = 3 * GiB        # the HD builder's own margin (build_hd_pack.preflight)
DOWNLOAD_BYTES = 1 * GiB      # downloads in flight and unpacking beside them (temp folder)
# Minutes on the author's PC. The build is README's "about 20 minutes on a fast PC, up to an hour on a slower one".
# The extraction was measured at 17 seconds on the author's NVMe disk (docs/SESSION-NOTES-setup.md); a hard disk or
# a slow USB drive takes minutes.
EXTRACT_MINUTES = (1, 6)
BUILD_MINUTES = (20, 60)
CONVERT_MINUTES = (3, 10)     # CHD or BIN/CUE to ISO: not measured (no CHD of the game was available)


class PlanError(ValueError):
    pass


def parse_components(text):
    """'textures,prompts_font' -> ('textures', 'prompts_font') in canonical order. PlanError for an unknown id."""
    if text is None:
        return None
    ids = [x.strip() for x in str(text).split(',') if x.strip()]
    bad = [x for x in ids if x not in COMPONENTS]
    if bad:
        raise PlanError(', '.join(bad))
    return tuple(c for c in COMPONENT_ORDER if c in ids)


def resolve_components(preset=None, components=None, recorded=None):
    """The components to install.

    A first install: exactly --components if given, else the preset (default Recommended). On a folder that already has
    components: what it has plus what is asked (the installer never removes one), and with neither option, what it has.
    """
    asked = parse_components(components) if isinstance(components, str) else components
    if asked is None and preset:
        asked = PRESETS[preset]
    have = tuple(recorded) if recorded is not None else None
    if have is None:
        chosen = asked if asked is not None else PRESETS['recommended']
    else:
        chosen = tuple(set(have) | set(asked or ()))
    return tuple(c for c in COMPONENT_ORDER if c in chosen)


def preset_of(components):
    """The preset whose components these are, or 'custom'."""
    for name, comps in PRESETS.items():
        if set(comps) == set(components):
            return name
    return 'custom'


def hd_disk(components, clean_work=False):
    """(peak, final) bytes of the HD components, built one after the other (textures, then movies)."""
    peak = retained = 0
    for c in COMPONENT_ORDER:
        if c in components:
            spec = COMPONENTS[c]
            peak = max(peak, retained + spec['peak'])
            retained += spec['final'] if clean_work else spec['peak']
    return peak, retained


def estimate(components, extract=True, build=True, conversion=False, hd_pending=None, clean_work=False):
    """Disk and time for what is left to do.

    components: the chosen ones; hd_pending: the HD components whose step will run (default: all chosen);
    extract / build: whether those steps will run; conversion: the source needs converting to an ISO first.
    """
    pend = [c for c in components if c in ('textures', 'movies') and (hd_pending is None or c in hd_pending)]
    peak_hd, final_hd = hd_disk(pend, clean_work)
    game = (DISC_BYTES if extract else 0) + (BUILD_BYTES if build else 0)
    anything = bool(game or pend)
    need = game + peak_hd + (MARGIN_BYTES if anything else 0)
    low = (EXTRACT_MINUTES[0] if extract else 0) + (BUILD_MINUTES[0] if build else 0) + (CONVERT_MINUTES[0] if conversion else 0)
    high = (EXTRACT_MINUTES[1] if extract else 0) + (BUILD_MINUTES[1] if build else 0) + (CONVERT_MINUTES[1] if conversion else 0)
    for c in pend:
        low += COMPONENTS[c]['minutes'][0]
        high += COMPONENTS[c]['minutes'][1]
    return {
        'need': need,
        'game': game,
        'peak_hd': peak_hd,
        'final': game + final_hd,
        'per_component': {c: {'peak': COMPONENTS[c]['peak'], 'final': COMPONENTS[c]['final'],
                              'minutes_min': COMPONENTS[c]['minutes'][0], 'minutes_max': COMPONENTS[c]['minutes'][1]}
                          for c in COMPONENT_ORDER},
        'minutes_min': low,
        'minutes_max': high,
    }


def hd_module(root):
    """tools/hd/build_hd_pack.py as a module: the one place that knows the pinned HD tools and the step numbers."""
    path = os.path.join(root, 'tools', 'hd', 'build_hd_pack.py')
    spec = importlib.util.spec_from_file_location('rg_build_hd_pack', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def download_specs(root, chdman):
    """{id: spec} of everything the setup can fetch, each with name, url, bytes, sha256, license, dest (relative to
    toolchain/). The HD tools come from build_hd_pack.py; chdman from download.py."""
    hd = hd_module(root)
    out = {}
    for key, spec in (('realesrgan', hd.REALESRGAN), ('ffmpeg', hd.FFMPEG)):
        out[key] = {'id': key, 'name': spec['name'], 'url': spec['url'], 'bytes': spec['mb'] * 1_000_000,
                    'sha256': spec['sha256'], 'license': spec['license'], 'dest': os.path.join('toolchain', spec['dir'])}
    out['pylibs'] = {'id': 'pylibs', 'name': 'numpy and Pillow (pip, from pypi.org)', 'url': 'https://pypi.org/',
                     'bytes': 40_000_000, 'sha256': '', 'license': 'BSD-3-Clause (numpy), MIT-CMU (Pillow)',
                     'dest': os.path.join('toolchain', 'pylibs')}
    out['chdman'] = dict(chdman, id='chdman')
    return out
