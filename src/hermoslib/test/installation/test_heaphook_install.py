"""Check installed library resolution without executing heap-hook initialization."""

import os
from pathlib import Path
import subprocess


def test_installed_hook_resolves_its_real_sibling():
    prefix = Path(os.environ['HERMOS_TEST_PREFIX'])
    hook = (prefix / 'lib/libhermos_heaphook.so').resolve(strict=True)
    library = (prefix / 'lib/libhermos.so').resolve(strict=True)
    assert hook.parent == library.parent
    environment = dict(os.environ)
    environment.pop('LD_PRELOAD', None)
    environment['LD_LIBRARY_PATH'] = str(prefix / 'lib') + ':' + environment.get(
        'LD_LIBRARY_PATH', '')
    result = subprocess.run(['ldd', str(hook)], env=environment,
                            capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stderr
    resolved = [line.split('=>', 1)[1].strip().split(' (', 1)[0]
                for line in result.stdout.splitlines()
                if line.strip().startswith('libhermos.so =>')]
    assert len(resolved) == 1, result.stdout
    assert Path(resolved[0]).resolve() == library
