#!/bin/sh
set -eu
package_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! command -v python3 >/dev/null 2>&1; then
    printf '%s\n' 'Duke-RT setup requires Python 3. Install your distribution python3 package.' >&2
    exit 1
fi
exec python3 "$package_dir/tools/dist/prepare_linux_content.py" --launch-root "$package_dir" "$@"
