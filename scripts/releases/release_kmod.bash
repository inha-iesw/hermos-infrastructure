#!/bin/bash
set -eux

# Historical upstream release entries are retained; initialize fork release metadata first.
if [ "$(dpkg-parsechangelog -l hermos_kmod/debian/changelog -S Source)" != "hermos-kmod-v2.3.5" ]; then
  echo "Error: HERMOS Debian release metadata is not configured. Add a new fork changelog entry first." >&2
  exit 1
fi
: "${HERMOS_PPA:?Set HERMOS_PPA to the authorized fork publication target.}"

# Preparation example:
#   export DEBEMAIL="sykwer@gmail.com"
#   export DEBFULLNAME="Takahiro Ishikawa-Aso"
if [ -z "${DEBEMAIL:-}" ] || [ -z "${DEBFULLNAME:-}" ]; then
  echo "Error: Please set DEBEMAIL and DEBFULLNAME environment variables to match your GPG key identity."
  exit 1
fi

if ! command -v dput &> /dev/null; then
  echo "Error: dput is not installed. Install it with: sudo apt install dput"
  exit 1
fi

if ! command -v backportpackage &> /dev/null; then
  echo "Error: backportpackage is not installed. Install it with: sudo apt install ubuntu-dev-tools"
  exit 1
fi

VERSION="2.3.5"
RELEASE=$(dpkg-parsechangelog -l hermos_kmod/debian/changelog -S Version | cut -d'-' -f2)

CHANGES_FILE="hermos-kmod-v${VERSION}_${VERSION}-${RELEASE}_source.changes"
DSC_FILE="hermos-kmod-v${VERSION}_${VERSION}-${RELEASE}.dsc"

# Safety check: ensure dh-dkms is in the .dsc
if ! grep -q "dh-dkms" "${DSC_FILE}"; then
  echo "Error: dh-dkms not found in ${DSC_FILE}. Build-Depends may be incorrect."
  exit 1
fi

# Upload to noble
dput "${HERMOS_PPA}" ${CHANGES_FILE}

# Backport to jammy
backportpackage -u "${HERMOS_PPA}" -d jammy ${DSC_FILE} -y -sa
