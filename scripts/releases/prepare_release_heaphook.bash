#!/bin/bash
set -eux

# Historical upstream release entries are retained; initialize fork release metadata first.
if [ "$(dpkg-parsechangelog -l hermos_heaphook/debian/changelog -S Source)" != "hermos-heaphook-v2.3.5" ]; then
  echo "Error: HERMOS Debian release metadata is not configured. Add a new fork changelog entry first." >&2
  exit 1
fi

if ! command -v debuild &> /dev/null; then
  echo "Error: debuild is not installed. Install it with: sudo apt install devscripts"
  exit 1
fi

VERSION="2.3.5"

cp -r hermos_heaphook hermos-heaphook-v${VERSION}-${VERSION}
rm -f hermos-heaphook-v${VERSION}-${VERSION}/Cargo.lock
rm -rf hermos-heaphook-v${VERSION}-${VERSION}/target
tar czf hermos-heaphook-v${VERSION}_${VERSION}.orig.tar.gz hermos-heaphook-v${VERSION}-${VERSION}

cd hermos-heaphook-v${VERSION}-${VERSION}
debuild -S -sa

# When re-upload with a next release number
# debuild -S -sd

cd ..
rm -rf hermos-heaphook-v${VERSION}-${VERSION}
