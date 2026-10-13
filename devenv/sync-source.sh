#!/bin/sh
# Synchronize a snapshot without allowing its old timestamps to hide new edits.
set -eu
# Compare contents: source edits can precede the previous build completion, or
# even keep the same length and timestamp. Give transferred files a fresh mtime
# so make rebuilds them; leave identical files alone for incremental builds.
rsync -ac --no-times --delete --exclude=/build/ "$1/" "$2/"
