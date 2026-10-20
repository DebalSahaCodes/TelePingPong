#!/bin/sh
# usage: mkdeb.sh ARCH BINARY OUT.deb VERSION
set -eu
arch=$1; bin=$2; out=$3; ver=$4
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
chmod 0755 "$stage"

install -D -m 0755 "$bin"                                "$stage/usr/bin/pingpong"
install -D -m 0644 "$here/pingpong-server.service"      "$stage/lib/systemd/system/pingpong-server.service"
install -D -m 0644 "$root/README.md"                     "$stage/usr/share/doc/pingpong-net/README.md"

size=$(du -sk "$stage" | cut -f1)
install -d -m 0755 "$stage/DEBIAN"
sed -e "s/@VERSION@/$ver/" -e "s/@ARCH@/$arch/" -e "s/@SIZE@/$size/" \
    "$here/control" > "$stage/DEBIAN/control"

dpkg-deb --root-owner-group --build "$stage" "$out" >/dev/null
echo "built $out"
