"""Source archive identity is independent of the development checkout location."""

import json
from pathlib import Path
import subprocess
import sys


SCRIPT = Path(__file__).resolve().parents[2] / 'cmake/generate_source_identity.py'


def identity(root, output):
    subprocess.run([sys.executable, str(SCRIPT), str(root), str(output)],
                   check=True, capture_output=True, text=True, timeout=10)
    return json.loads(output.read_text())


def test_archive_identity_changes_with_source_but_not_build_artifacts(tmp_path):
    source = tmp_path / 'archive/src/hermoslib/src/runtime.cpp'
    source.parent.mkdir(parents=True)
    source.write_text('first\n')
    root = tmp_path / 'archive'
    before = identity(root, tmp_path / 'before.json')
    assert before['revision'] is None
    assert len(before['tree_sha256']) == 64
    artifact = root / 'hermos_heaphook/target/release/artifact.so'
    artifact.parent.mkdir(parents=True)
    artifact.write_bytes(b'generated')
    assert identity(root, tmp_path / 'artifacts.json') == before
    source.write_text('second\n')
    assert identity(root, tmp_path / 'after.json')['tree_sha256'] != before['tree_sha256']


def test_archive_identity_is_relocatable_and_tracks_executable_mode(tmp_path):
    source = tmp_path / 'one/hermos_kmod/Makefile'
    source.parent.mkdir(parents=True)
    source.write_text('all:\n')
    before = identity(tmp_path / 'one', tmp_path / 'before.json')
    (tmp_path / 'one').rename(tmp_path / 'two')
    assert identity(tmp_path / 'two', tmp_path / 'moved.json') == before
    (tmp_path / 'two/hermos_kmod/Makefile').chmod(0o755)
    assert identity(tmp_path / 'two', tmp_path / 'mode.json') != before
