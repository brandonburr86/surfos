#!/bin/sh
# Fetch Digilent's master XDC files for the Arty A7-35 and A7-100 (and the repository's
# licence) from a pinned commit of github.com/Digilent/digilent-xdc into
# build/third_party/digilent-xdc/<commit>/ and check each file's SHA-256. Prints the
# directory. Runs on the Mac or in either container (needs curl and sha256sum or shasum).
# Files already present with the right hash are not downloaded again.
set -eu
COMMIT=c8644a9fe31cc803789643124545d56b3bef4600
here=$(cd "$(dirname "$0")/.." && pwd -P)            # surfcpu/
dest=${OPENXC7_THIRD_PARTY:-$here/build/third_party}/digilent-xdc/$COMMIT
url=https://raw.githubusercontent.com/Digilent/digilent-xdc/$COMMIT

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

mkdir -p "$dest"
while read -r file sum; do
  if [ -f "$dest/$file" ] && [ "$(sha256 "$dest/$file")" = "$sum" ]; then continue; fi
  curl -fsSL --retry 3 -o "$dest/$file.part" "$url/$file"
  got=$(sha256 "$dest/$file.part")
  if [ "$got" != "$sum" ]; then
    rm -f "$dest/$file.part"
    echo "openxc7-fetch.sh: $file has SHA-256 $got, expected $sum" >&2
    exit 1
  fi
  mv "$dest/$file.part" "$dest/$file"
done <<EOF
Arty-A7-35-Master.xdc 3884f49d657924e2903e7cc275ee6d4acc6e5b17a55b818ed742fc59a0818492
Arty-A7-100-Master.xdc 5c0c84302cbce49ac85f4812e8f1f7371e686964ec2eb29fd67307da9ed6835f
License.txt fbdfae05e542ea6ad7e11e3818076b46d2b6bd81dac49c59bc9ac78025ba5339
EOF
echo "$dest"
