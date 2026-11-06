"""Read hermoslib's per-process tmpfs type announcements.

Each Hermos process appends tab-separated topic/type/role/node/domain/start-time lines to
``/dev/shm/hermos_type_registry/<ipc_ns_inode>/<pid>.txt`` on each
``Publisher<T>`` / ``Subscription<T>`` construction (see
``hermoslib::internal::TypeRegistryWriter``). The daemon walks the per-NS
directory once per tick. PID hints require an exact domain and verified process
start identity. Type hints use the domain when available, falling back to an
unambiguous legacy type only when no matching domain row exists; they never
supply a PID identity.

Forward-compatibility rules (parser contract):

* Lines must terminate in ``\\n``. A trailing unterminated tail (writer
  died mid-write) is silently dropped.
* Each line is tab-split. The first four fields form the legacy format:
  ``topic``, ``type``, ``role``, ``node_name``. Fields five and six identify domain and process start
  ticks; PID queries require both. Later fields remain ignored.
* Lines with fewer than four fields are skipped; a ``WARN`` is logged
  the first time each malformed file is seen (across the reader's
  lifetime, not per rebuild) — corrupt files don't re-spam the log.
"""

import os
from dataclasses import dataclass
from typing import Dict, Optional, Tuple


def _process_start_ticks(pid: int) -> int | None:
    """Return Linux's process start identity, or None when it cannot be read."""
    try:
        with open(f'/proc/{pid}/stat') as source:
            # comm may contain spaces or parentheses; the last ')' ends it.
            return int(source.read().rsplit(')', 1)[1].split()[19])
    except (OSError, ValueError, IndexError):
        return None


def _default_base_dir() -> str:
    """Return the tmpfs root the writer agreed on.

    Mirrors `hermoslib::internal::TypeRegistryWriter`: read
    ``HERMOS_TMPFS_DIR`` if set (for hardened containers where
    ``/dev/shm`` is unavailable or too small) and append the
    ``hermos_type_registry`` suffix.
    """
    root = os.environ.get('HERMOS_TMPFS_DIR') or '/dev/shm'
    return os.path.join(root, 'hermos_type_registry')


BASE_DIR = _default_base_dir()


@dataclass(frozen=True)
class RegistryEntry:
    pid: int
    type_name: str
    domain_id: int | None = None
    process_start_ticks: int | None = None


# Keep distinct processes and domains; a conflicting duplicate maps to None.
# role is "pub" or "sub".
RegistryTable = Dict[Tuple[str, str, str, Optional[int], int], Optional[RegistryEntry]]


class TypeRegistryReader:
    """Scan ``${HERMOS_TMPFS_DIR:-/dev/shm}/hermos_type_registry/<ns_inode>/``
    and build an in-memory lookup table.

    The reader's *table* is stateless across ticks; each ``rebuild()`` call
    replaces it from scratch so entries whose source process died (and
    whose file was removed by either the writer's ``atexit`` cleanup or
    our own ``cleanup_dead_pids()``) are forgotten naturally. The
    ``_warned_malformed_files`` set, however, persists across rebuilds —
    a corrupt file logs a single WARN the first time it is observed and
    stays quiet afterwards.
    """

    def __init__(self, ipc_ns_inode: int, base_dir: str = BASE_DIR, logger=None):
        self._ns_dir = os.path.join(base_dir, str(ipc_ns_inode))
        self._table: RegistryTable = {}
        self._by_endpoint: Dict[Tuple[str, str, str], RegistryTable] = {}
        self._process_starts: Dict[int, Optional[int]] = {}
        self._logger = logger
        self._warned_malformed_files: set = set()

    @property
    def ns_dir(self) -> str:
        return self._ns_dir

    @property
    def table(self) -> RegistryTable:
        return self._table

    def rebuild(self) -> None:
        """Re-scan ``ns_dir`` and replace the internal table."""
        new_table: RegistryTable = {}
        self._by_endpoint = {}
        self._process_starts = {}
        try:
            entries = list(os.scandir(self._ns_dir))
        except FileNotFoundError:
            self._table = new_table
            return
        except PermissionError as exc:
            if self._logger is not None:
                self._logger.warn(f'type_registry: cannot read {self._ns_dir}: {exc}')
            self._table = new_table
            return

        for entry in entries:
            if not entry.is_file() or not entry.name.endswith('.txt'):
                continue
            try:
                pid = int(entry.name[:-len('.txt')])
            except ValueError:
                # Unexpected filename; skip silently.
                continue
            self._ingest_file(entry.path, pid, new_table)

        self._table = new_table
        for key, value in new_table.items():
            self._by_endpoint.setdefault(key[:3], {})[key] = value

    def _ingest_file(self, path: str, pid: int, table: RegistryTable) -> None:
        try:
            with open(path, 'rb') as fp:
                data = fp.read()
        except (FileNotFoundError, PermissionError):
            return

        if pid not in self._process_starts:
            self._process_starts[pid] = _process_start_ticks(pid)
        observed_start = self._process_starts[pid]
        text = data.decode('utf-8', errors='replace')
        # Only fully terminated lines are accepted. `splitlines(keepends=True)`
        # preserves the trailing `\n` so we can distinguish complete lines
        # from a torn tail.
        for raw_line in text.splitlines(keepends=True):
            if not raw_line.endswith('\n'):
                # Writer was interrupted mid-line; skip the tail.
                continue
            line = raw_line[:-1]
            fields = line.split('\t')
            if len(fields) < 4:
                if path not in self._warned_malformed_files and self._logger is not None:
                    self._logger.warn(
                        f'type_registry: malformed line in {path}: '
                        f'expected at least 4 tab-separated fields, got {len(fields)}')
                    self._warned_malformed_files.add(path)
                continue
            topic, type_name, role, node_name = fields[0], fields[1], fields[2], fields[3]
            if role not in ('pub', 'sub'):
                continue
            domain_id = start_ticks = None
            if len(fields) >= 6:
                try:
                    domain_id, start_ticks = int(fields[4]), int(fields[5])
                except ValueError:
                    pass  # Unknown legacy extensions cannot supply scoped metadata.
                else:
                    if (not 0 <= domain_id <= 0xFFFFFFFF or start_ticks < 0 or
                            (start_ticks > 0 and observed_start is not None and
                             observed_start != start_ticks)):
                        continue
                    # The writer emits zero when /proc could not supply an identity.
                    # Keep its domain/type hint without making it a verified PID hint.
                    if start_ticks == 0:
                        start_ticks = None
            key = (topic, role, node_name, domain_id, pid)
            value = RegistryEntry(pid, type_name, domain_id, start_ticks)
            if key in table and table[key] != value:
                table[key] = None
            else:
                table[key] = value

    def cleanup_dead_pids(self) -> None:
        """Remove tmpfs files whose owning PID is no longer alive.

        Called once per tick before ``rebuild()``. The writer process is
        responsible for unlinking its own file on graceful exit; this
        sweep covers SIGKILL / crash cases.
        """
        try:
            entries = list(os.scandir(self._ns_dir))
        except FileNotFoundError:
            return
        except PermissionError as exc:
            if self._logger is not None:
                self._logger.warn(f'type_registry: cannot list {self._ns_dir} for cleanup: {exc}')
            return

        for entry in entries:
            if not entry.is_file() or not entry.name.endswith('.txt'):
                continue
            try:
                pid = int(entry.name[:-len('.txt')])
            except ValueError:
                continue
            if pid <= 0:
                continue
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                pass
            except PermissionError:
                # hidepid can hide /proc entries for a process that is alive.
                continue
            except OverflowError:
                # A numeric filename need not fit the operating system's pid_t.
                continue
            else:
                continue
            try:
                os.unlink(entry.path)
            except FileNotFoundError:
                pass
            except PermissionError as exc:
                if self._logger is not None:
                    self._logger.warn(
                        f'type_registry: failed to unlink stale {entry.path}: {exc}')

    def lookup(
        self, topic_name: str, role: str, node_name: str, *,
        domain_id: int, pid: int | None = None,
    ) -> Optional[RegistryEntry]:
        """Return only one unambiguous live match; unknown scope is not inferred.

        A PID hint requires an extended row with the exact domain and
        process start identity. Supplying the native PID distinguishes repeated
        node names. Type-only compatibility queries use ``lookup_type``.
        """
        matches = []
        candidates = self._by_endpoint.get((topic_name, role, node_name), {})
        for key, entry in candidates.items():
            if key[3] != domain_id:
                continue
            if pid is not None and key[4] != pid:
                continue
            if entry is None:
                return None
            if (entry.process_start_ticks is None or
                    _process_start_ticks(entry.pid) != entry.process_start_ticks):
                continue
            matches.append(entry)
        return matches[0] if len(matches) == 1 else None

    def lookup_type(
        self, topic_name: str, role: str, node_name: str, *,
        domain_id: int, pid: int | None = None,
    ) -> str:
        """Return an unambiguous type hint without attributing a process identity.

        Matching extended rows take precedence over legacy rows. Legacy rows
        cannot prove their domain, so they are a compatibility fallback only.
        A native PID, when supplied, must match either kind of row. Known
        start-time mismatches are rejected; inaccessible /proc metadata does
        not prevent a type hint from preserving bridge discovery.
        Conflicts remain unknown rather than falling back to weaker metadata.
        """
        candidates = self._by_endpoint.get((topic_name, role, node_name), {})
        scoped = []
        legacy = []
        for key, entry in candidates.items():
            if pid is not None and key[4] != pid:
                continue
            if key[3] == domain_id:
                scoped.append(entry)
            elif key[3] is None:
                legacy.append(entry)
        matches = scoped or legacy
        if any(entry is None for entry in matches):
            return ''
        types = set()
        for entry in matches:
            if entry.process_start_ticks is not None:
                observed_start = _process_start_ticks(entry.pid)
                if observed_start is not None and observed_start != entry.process_start_ticks:
                    continue
            types.add(entry.type_name)
        return next(iter(types)) if len(types) == 1 else ''
