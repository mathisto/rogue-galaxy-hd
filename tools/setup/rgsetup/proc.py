"""Running the tools the setup drives (setup.py, the HD builder, chdman, pip) as child processes.

One line of output at a time goes to a callback and to the step's log file. A cancel (Ctrl+Break, SIGTERM, Ctrl+C: they
all end as KeyboardInterrupt in the main thread) kills the child and everything it started, and only those: the setup
never stops a process it did not start.
"""
import os
import queue
import signal
import subprocess
import threading
from pathlib import Path

NO_WINDOW = 0x08000000 if os.name == 'nt' else 0
BELOW_NORMAL = 0x00004000 if os.name == 'nt' else 0
IDLE = 0x00000040 if os.name == 'nt' else 0


def kill_tree(proc):
    if proc.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(proc.pid)], capture_output=True, creationflags=NO_WINDOW)
        return
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except OSError:
        proc.kill()


def run(command, on_line=None, log_path=None, env=None, cwd=None, gentle=False, heartbeat=None, on_idle=None):
    """Run command; returns (exit code, the last 30 lines). on_line(line) gets every line; on_idle() is called each
    `heartbeat` seconds of silence (and nothing else breaks the wait). KeyboardInterrupt kills the tree and propagates."""
    flags = NO_WINDOW | (IDLE if gentle else BELOW_NORMAL) if os.name == 'nt' else 0
    log = None
    if log_path:
        Path(log_path).parent.mkdir(parents=True, exist_ok=True)
        log = open(log_path, 'a', encoding='utf-8', buffering=1)
        log.write('$ ' + ' '.join(f'"{c}"' if ' ' in str(c) else str(c) for c in command) + '\n')
    proc = subprocess.Popen([str(c) for c in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            encoding='utf-8', errors='replace', env=env, cwd=cwd, creationflags=flags,
                            start_new_session=(os.name != 'nt'))
    q = queue.Queue()

    def reader():
        for ln in proc.stdout:
            q.put(ln.rstrip('\r\n'))
        q.put(None)

    threading.Thread(target=reader, daemon=True).start()
    last = []
    try:
        while True:
            try:
                ln = q.get(timeout=heartbeat or 0.5)
            except queue.Empty:
                if on_idle:
                    on_idle()
                continue
            if ln is None:
                break
            last.append(ln)
            del last[:-30]
            if log:
                log.write(ln + '\n')
            if on_line:
                on_line(ln)
        proc.wait()
    except BaseException:
        kill_tree(proc)
        raise
    finally:
        if proc.poll() is None:
            kill_tree(proc)
        if log:
            log.close()
    return proc.returncode, last
