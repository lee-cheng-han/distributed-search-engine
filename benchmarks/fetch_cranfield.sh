#!/bin/sh
set -eu
destination="${1:-/tmp/dse-cranfield}"
archive="${TMPDIR:-/tmp}/dse-cranfield.tar.gz"
curl -L --fail --silent --show-error \
  https://ir.dcs.gla.ac.uk/resources/test_collections/cran/cran.tar.gz -o "$archive"
printf '%s  %s\n' '8c48a8412e4a7e6e0dd5af99c959f8d75fdf88135602adc1a56aa22770652d64' "$archive" | sha256sum --check -
mkdir -p "$destination"
tar -xzf "$archive" -C "$destination"
