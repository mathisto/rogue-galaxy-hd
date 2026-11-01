"""rg-install.json in the install folder: what built it and which steps finished.

    {"format": 1, "release": "0.1.0", "region": "SCUS-97490", "components": ["textures", ...],
     "inputs": {"disc": {"kind", "name", "size", "validated"}},
     "steps": {"<step>": {"key": "<fingerprint of the step's inputs>", "release": "...", "finished": "<UTC time>",
                          "seconds": 12.3, ...the step's own fields}}}

A step's key fingerprints everything its output depends on (runner.py), so a step whose recorded key equals the
current one and whose output still checks out is not run again. The file is replaced atomically after each step.
It never holds a disc byte, a path to the disc image or a secret.
"""
import json
import os
from pathlib import Path

NAME = 'rg-install.json'
FORMAT = 1


class StateError(Exception):
    def __init__(self, id, path):
        super().__init__(id)
        self.id, self.path = id, path


def empty(release):
    return {'format': FORMAT, 'release': release, 'region': None, 'components': [], 'inputs': {}, 'steps': {}}


def load(dest):
    """The state of dest, or None when the folder has none. StateError for an unreadable or newer file."""
    path = Path(dest) / NAME
    if not path.is_file():
        return None
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        raise StateError('err.state_unreadable', path) from None
    if not isinstance(data, dict) or not isinstance(data.get('steps'), dict):
        raise StateError('err.state_unreadable', path)
    if data.get('format', 0) > FORMAT:
        raise StateError('err.state_newer', path)
    return data


def save(dest, data):
    path = Path(dest) / NAME
    tmp = path.with_name(NAME + '.tmp')
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
