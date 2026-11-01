"""The game's settings.ini: write defaults for keys that are absent, change nothing else.

The F1 menu keeps its settings in an INI file (thirdparty/dq8-gfx/src/ui/ui_settings.cpp): RG_SETTINGS_FILE if set, else
settings.ini in SDL's per-user folder, %APPDATA%\\RogueGalaxy\\Recompiled on Windows. A key is a line `name = value`
in the [game] section. The game reads whatever keys the file has and keeps its own default for the others, so a file
with only the two lines below is valid; the menu rewrites the whole file the next time it saves.

A key that is already there is a choice the player made (in the menu, or by hand): it is never overwritten. That also
makes an update safe, and a run on a folder with an existing install writes nothing.
"""
import os
import re
from pathlib import Path

# prompts_font: Xbox button prompts (button_prompts: 0 PlayStation, 1 Xbox, 2 Auto) and the modern font
# (font_style: 0 original, 1 modern). The only keys the setup ever writes.
PROMPTS_FONT = {'button_prompts': '1', 'font_style': '1'}


def settings_path(override=None):
    """The settings file the game will read."""
    if override:
        return Path(override)
    env = os.environ.get('RG_SETTINGS_FILE')
    if env:
        return Path(env)
    appdata = os.environ.get('APPDATA')
    if appdata:
        return Path(appdata) / 'RogueGalaxy' / 'Recompiled' / 'settings.ini'
    return Path.home() / '.local' / 'share' / 'RogueGalaxy' / 'Recompiled' / 'settings.ini'


def _section_bounds(lines, name):
    """(index of the header, index after the section's last line) of [name], or None."""
    start = None
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith('[') and s.endswith(']'):
            if start is not None:
                return start, i
            if s[1:-1].strip().lower() == name:
                start = i
    return (start, len(lines)) if start is not None else None


def present_keys(text, section='game'):
    lines = text.splitlines()
    b = _section_bounds(lines, section)
    keys = {}
    if b:
        for line in lines[b[0] + 1:b[1]]:
            m = re.match(r'\s*([^=;#\s][^=]*?)\s*=\s*(.*)$', line)
            if m:
                keys[m.group(1)] = m.group(2).strip()
    return keys


def apply_defaults(text, defaults, section='game'):
    """-> (new text, the keys added). Keys already in [section] are left as they are; the rest of the file is kept byte
    for byte (line endings included)."""
    have = present_keys(text, section)
    missing = {k: v for k, v in defaults.items() if k not in have}
    if not missing:
        return text, []
    nl = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(nl) if text else []
    b = _section_bounds(lines, section)
    new_lines = [f'{k} = {v}' for k, v in missing.items()]
    if b:
        end = b[1]
        while end > b[0] + 1 and not lines[end - 1].strip():
            end -= 1     # after the section's last key, before the blank line that separates it from the next
        lines[end:end] = new_lines
        out = nl.join(lines)
    else:
        body = nl.join(lines)
        if body and not body.endswith(nl):
            body += nl
        out = body + (nl if body else '') + f'[{section}]' + nl + nl.join(new_lines) + nl
    return out, list(missing)


def write_defaults(path, defaults):
    """Add the missing keys of defaults to the [game] section of the file at path (created when absent), through a
    temporary name so a crash never leaves a half-written file. Returns the keys that were added."""
    path = Path(path)
    text = path.read_bytes().decode('utf-8') if path.is_file() else ''
    new, added = apply_defaults(text, defaults)
    if added:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(path.name + '.setup-tmp')
        tmp.write_bytes(new.encode('utf-8'))
        os.replace(tmp, path)
    return added
