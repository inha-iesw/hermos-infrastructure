"""Record backend revision and actual source contents without installed paths."""

import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys


SOURCE_ROOTS = ('src/hermoslib', 'hermos_heaphook', 'hermos_kmod')
GENERATED_DIRECTORIES = {'.git', '__pycache__', '.pytest_cache', 'target', 'build'}
GENERATED_FILES = {'Module.symvers', 'modules.order', 'sparse.log'}
GENERATED_SUFFIXES = ('.o', '.a', '.so', '.ko', '.pyc', '.cmd', '.mod', '.mod.c')


def source_identity(root):
    """Hash source contents and modes, including uncommitted source changes."""
    revision = None
    if (root / '.git').exists():
        revision = subprocess.check_output(
            ['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
        names = subprocess.check_output(
            ['git', '-C', str(root), 'ls-files', '-z', '--cached', '--others',
             '--exclude-standard', '--', *SOURCE_ROOTS]).split(b'\0')
        paths = {root / os.fsdecode(name) for name in names if name}
    else:
        paths = set()
        for subtree in SOURCE_ROOTS:
            for directory, children, files in os.walk(root / subtree):
                children[:] = [name for name in children if name not in GENERATED_DIRECTORIES]
                paths.update(Path(directory) / name for name in files
                             if name not in GENERATED_FILES
                             and not name.endswith(GENERATED_SUFFIXES))
    digest = hashlib.sha256()
    for path in sorted(paths):
        if not path.exists() and not path.is_symlink():
            continue
        metadata = path.lstat()
        data = os.fsencode(os.readlink(path)) if path.is_symlink() else path.read_bytes()
        digest.update(os.fsencode(path.relative_to(root)) + b'\0')
        digest.update(str(stat.S_IMODE(metadata.st_mode)).encode() + b'\0')
        digest.update(b'link\0' if path.is_symlink() else b'file\0')
        digest.update(hashlib.sha256(data).digest())
    return {'revision': revision, 'tree_sha256': digest.hexdigest()}


if __name__ == '__main__':
    root, output = map(Path, sys.argv[1:])
    output.write_text(json.dumps(source_identity(root.resolve()), sort_keys=True) + '\n')
