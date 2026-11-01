#!/bin/sh
# Print what the surfcpu-openxc7 image holds, one item per line (copied into the image as
# surfcpu-openxc7-versions; its output is /etc/surfcpu-openxc7.txt, which every openXC7 build
# copies into its manifest).
v() { printf '%-22s %s\n' "$1" "$(sh -c "$2" 2>&1 | head -n 1)"; }
v image-arch      'uname -m'
v debian          'cat /etc/debian_version'
v yosys           'yosys -V'
v nextpnr         'nextpnr-himbaechel --version'
v python3         'python3 --version'
v fasm            'python3 -W ignore -c "import fasm.version as v; print(v.version_str)"'
v fasm-parser     'python3 -W ignore -c "import fasm.parser as p; print(p.implementation)"'
v textx           'python3 -c "import importlib.metadata as m; print(m.version(\"textx\"))"'
v z3-library      'dpkg-query -W -f "\${Version}" libz3-4'
v eigen-at-build  'echo "3.4.0, Debian libeigen3-dev 3.4.0-5 (the placer solver)"'
for f in /opt/openxc7/share/openxc7/source-*.txt; do
  printf '%-22s %s\n' "source-$(basename "$f" .txt | sed 's/^source-//')" "$(cat "$f")"
done
while read -r sum file; do
  printf '%-22s sha256:%s %s bytes\n' "$file" "$sum" "$(wc -c < "/opt/openxc7/share/chipdb/$file" | tr -d ' ')"
done < /opt/openxc7/share/chipdb/SHA256SUMS
