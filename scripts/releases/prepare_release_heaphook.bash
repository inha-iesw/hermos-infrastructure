#!/bin/bash
set -eux

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
