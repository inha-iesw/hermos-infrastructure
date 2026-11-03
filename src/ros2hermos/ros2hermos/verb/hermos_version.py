import ctypes
import ctypes.util
import os
import re

from ament_index_python.packages import get_package_prefix
from ros2cli.verb import VerbExtension


def _parse_semver(version_str):
    """Parse a version string into (major, minor, patch) tuple, or None on failure."""
    m = re.match(r'^(\d+)\.(\d+)\.(\d+)$', version_str)
    if m:
        return (int(m.group(1)), int(m.group(2)), int(m.group(3)))
    return None


def _get_lib_path(package_name, lib_filename):
    """Resolve absolute path to a shared library from its ROS2 package prefix."""
    try:
        prefix = get_package_prefix(package_name)
        path = os.path.join(prefix, 'lib', lib_filename)
        if os.path.exists(path):
            return path
    except Exception:
        pass
    return None


def _check_version_compatibility(kmod_version, hermoslib_version, heaphook_version):
    """Check version compatibility using the same rules as hermoslib's is_version_consistent.

    Rules:
      - heaphook and hermoslib must match exactly (major.minor.patch)
      - kmod and hermoslib must share the same major.minor
    """
    warnings = []

    # Extract the leading version string (strip trailing warnings like "(WARN: ...)")
    def extract_version(v):
        return v.split()[0] if v else v

    lib_ver = _parse_semver(extract_version(hermoslib_version))
    heaphook_ver = _parse_semver(extract_version(heaphook_version))
    kmod_ver = _parse_semver(extract_version(kmod_version))

    if lib_ver and heaphook_ver:
        if lib_ver != heaphook_ver:
            warnings.append(
                'Hermos Heaphook and Hermoslib versions must match exactly '
                '(major.minor.patch). '
                f'heaphook={extract_version(heaphook_version)}, '
                f'hermoslib={extract_version(hermoslib_version)}')

    if lib_ver and kmod_ver:
        if lib_ver[:2] != kmod_ver[:2]:
            warnings.append(
                'Hermos Kernel Module and Hermoslib must share the same '
                'major.minor version. '
                f'kmod={extract_version(kmod_version)}, '
                f'hermoslib={extract_version(hermoslib_version)}')

    return warnings


class VersionVerb(VerbExtension):
    """Show version information for Hermos components."""

    def add_arguments(self, parser, cli_name):
        pass

    def main(self, *, args):
        kmod_version = self._get_kmod_version()
        hermoslib_version = self._get_hermoslib_version()
        heaphook_version = self._get_heaphook_version()
        ros2hermos_version = self._get_ros2hermos_version()

        print(f'hermos_kmod:     {kmod_version}')
        print(f'hermoslib:       {hermoslib_version}')
        print(f'hermos_heaphook: {heaphook_version}')
        print(f'ros2hermos:      {ros2hermos_version}')

        # Version compatibility checks (same rules as hermoslib's is_version_consistent)
        warnings = _check_version_compatibility(
            kmod_version, hermoslib_version, heaphook_version)
        for w in warnings:
            print(f'\nWARN: {w}')

        return 0

    def _get_kmod_version(self):
        # Try /sys/module first (available when MODULE_VERSION is set)
        sys_path = '/sys/module/hermos/version'
        if os.path.exists(sys_path):
            try:
                with open(sys_path) as f:
                    return f.read().strip()
            except OSError:
                pass

        # Fall back to ioctl via wrapper library
        lib_path = _get_lib_path('hermos_ioctl_wrapper', 'libhermos_ioctl_wrapper.so')
        if lib_path is None:
            return '(not available - ioctl wrapper not found)'
        try:
            lib = ctypes.CDLL(lib_path)
            lib.get_hermos_kmod_version.argtypes = []
            lib.get_hermos_kmod_version.restype = ctypes.c_void_p
            lib.free_hermos_kmod_version.argtypes = [ctypes.c_void_p]
            lib.free_hermos_kmod_version.restype = None

            version_ptr = lib.get_hermos_kmod_version()
            if version_ptr:
                version = ctypes.c_char_p(version_ptr).value.decode('utf-8')
                lib.free_hermos_kmod_version(version_ptr)
                return version
            return '(not available - module not loaded?)'
        except OSError:
            return '(not available - ioctl wrapper not found)'
        except AttributeError:
            return '(not available - ioctl wrapper is outdated or not installed)'

    def _get_hermoslib_version(self):
        lib_path = _get_lib_path('hermoslib', 'libhermos.so')
        if lib_path is None:
            return '(not available - library not found)'
        try:
            lib = ctypes.CDLL(lib_path)
            lib.hermos_get_version.argtypes = []
            lib.hermos_get_version.restype = ctypes.c_char_p
            version = lib.hermos_get_version()
            if version:
                return version.decode('utf-8')
            return '(not available)'
        except OSError:
            return '(not available - library not found)'
        except AttributeError:
            return '(not available - hermoslib is outdated or not installed)'

    def _get_heaphook_version(self):
        # Find heaphook from LD_PRELOAD, which is how it's loaded at runtime.
        lib_path = None
        not_in_ld_preload = False
        ld_preload = os.environ.get('LD_PRELOAD', '')
        for entry in ld_preload.split(':'):
            if 'libhermos_heaphook.so' in entry:
                if os.path.isabs(entry) and os.path.exists(entry):
                    lib_path = entry
                else:
                    # Bare soname (e.g., "libhermos_heaphook.so") — resolve via
                    # the dynamic linker search path.
                    resolved = ctypes.util.find_library('hermos_heaphook')
                    if resolved:
                        lib_path = resolved
                    else:
                        # find_library returns the soname; try loading directly
                        lib_path = entry
                break

        if lib_path is None:
            not_in_ld_preload = True
            # LD_PRELOAD not set; fall back to the default install path.
            ros_distro = os.environ.get('ROS_DISTRO', '')
            if ros_distro:
                candidate = f'/opt/ros/{ros_distro}/lib/libhermos_heaphook.so'
                if os.path.exists(candidate):
                    lib_path = candidate

        if lib_path is None:
            return '(not available - library not found)'

        # Load with RTLD_LOCAL | RTLD_LAZY to prevent the heaphook's malloc/free
        # symbols from replacing the process-wide allocator.
        try:
            lib = ctypes.CDLL(
                lib_path,
                mode=os.RTLD_LAZY | ctypes.RTLD_LOCAL,
            )
            lib.hermos_heaphook_get_version.argtypes = []
            lib.hermos_heaphook_get_version.restype = ctypes.c_char_p
            version = lib.hermos_heaphook_get_version()
            if version:
                ver = version.decode('utf-8')
                if not_in_ld_preload:
                    return f'{ver} (WARN: not in LD_PRELOAD, found at {lib_path})'
                return ver
            return '(not available)'
        except OSError:
            return '(not available - library not found)'
        except AttributeError:
            return '(not available - heaphook is outdated or not installed)'

    def _get_ros2hermos_version(self):
        try:
            from importlib.metadata import version
            return version('ros2hermos')
        except Exception:
            return '(not available)'
