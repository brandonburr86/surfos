#!/bin/sh
# SurfCPU: the release procedure from a fresh checkout (docs/RELEASE.md section 5, milestone 9).
#
#   tools/fresh-checkout.sh [--commit C] [--reference DIR|SHA256SUMS] [--work DIR] [--keep]
#                           [--build-images] [--evidence FILE] [-- MAKE-RELEASE-VARIABLES...]
#   make fresh-checkout [COMMIT=C] [REFERENCE=build/release/<name>] [EVIDENCE=file] [KEEP=1]
#                       [ARGS="PRERELEASE=1 SKIP_DDR=1"]
#
# Clones this repository (git clone: a checkout of its own, not a worktree) at commit C (default
# HEAD) into a new temporary directory and follows what docs/RELEASE.md tells a person to do there:
#   1. the setup: the two Docker images. Present (docker image inspect) they are used as they are;
#      missing, the script stops, or with --build-images runs `make -C surfcpu image openxc7-image`
#      in the clone (docs/RELEASE.md section 8: about 10 minutes, from pinned sources);
#   2. `make -C surfcpu release` with the given variables (PRERELEASE=1 while the DDR designs do not
#      build), nothing else;
#   3. with --reference, the new release compared with the reference artifact by artifact
#      (tools/release.py compare: SHA-256, and for a reference directory the first differing byte
#      and its likely cause).
# --evidence FILE writes a summary there (the commands, their times, the comparison). The clone is
# removed at the end unless --keep (or the release failed). Exit status: 0 the release was made
# (and matched the reference), 1 it failed or differed, 2 bad arguments or no images.
# Nothing here touches the board, a card or the network beyond what make release itself does
# (make litedram-gen fetches pinned sources the first time, for the DDR designs).
set -eu

here=$(cd "$(dirname "$0")/.." && pwd -P)            # surfcpu/
repo=$(cd "$here/.." && pwd -P)
me=fresh-checkout
die() { echo "$me: $*" >&2; exit 2; }

commit= reference= work= keep=0 images=0 evidence=
while [ $# -gt 0 ]; do
    case "$1" in
        --commit)       [ $# -ge 2 ] || die "--commit needs a value"; commit=$2; shift 2 ;;
        --reference)    [ $# -ge 2 ] || die "--reference needs a value"; reference=$2; shift 2 ;;
        --work)         [ $# -ge 2 ] || die "--work needs a value"; work=$2; shift 2 ;;
        --evidence)     [ $# -ge 2 ] || die "--evidence needs a value"; evidence=$2; shift 2 ;;
        --keep)         keep=1; shift ;;
        --build-images) images=1; shift ;;
        --)             shift; break ;;
        -h|--help)      sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)              die "unknown argument $1 (make variables go after --)" ;;
    esac
done
for v in "$@"; do
    case "$v" in
        PRERELEASE=*|SKIP_DDR=*|DESIGNS=*|BOARDS=*) ;;
        *) die "only PRERELEASE=, SKIP_DDR=, DESIGNS= and BOARDS= go to make release, not '$v'" ;;
    esac
done
commit=$(git -C "$repo" rev-parse --verify "${commit:-HEAD}^{commit}") || die "no such commit"
if [ -n "$reference" ]; then
    case "$reference" in /*) ;; *) reference=$PWD/$reference ;; esac
    [ -e "$reference" ] || die "no reference $reference"
fi
case "$evidence" in ''|/*) ;; *) evidence=$PWD/$evidence ;; esac
docker=${DOCKER:-docker}
command -v "$docker" >/dev/null 2>&1 || docker=/Applications/Docker.app/Contents/Resources/bin/docker
"$docker" info >/dev/null 2>&1 || die "Docker is not running (open -a Docker)"

if [ -z "$work" ]; then
    mkdir -p "$here/build/fresh-checkout"
    work=$(mktemp -d "$here/build/fresh-checkout/run.XXXXXX")
else
    case "$work" in /*) ;; *) work=$PWD/$work ;; esac
    [ ! -e "$work" ] || [ -z "$(ls -A "$work")" ] || die "$work exists and is not empty"
    mkdir -p "$work"
fi
clone=$work/surfos
log=$work/fresh-checkout.txt
t0=$(date +%s)
note() { echo "$me: $*"; echo "$*" >> "$log"; }
stamp() { echo "$(( $(date +%s) - t0 )) s"; }

note "$(date -u +%Y-%m-%dT%H:%M:%SZ): commit $commit of $repo, in $clone"
# 1. a checkout of its own: a full clone (local objects, hard-linked), then the commit, detached
git clone -q --no-checkout "$repo" "$clone"
git -C "$clone" checkout -q --detach "$commit"
[ -z "$(git -C "$clone" status --porcelain)" ] || die "the clone is not clean"
note "cloned: $(git -C "$clone" log -1 --format='%h %ci %s') ($(stamp))"

# 2. the setup (docs/RELEASE.md section 2): the images the scripts of this commit name
tc=surfcpu-toolchain:$(sed -n 's/^IMAGE_TAG=//p' "$clone/surfcpu/devenv/run.sh")
ox=surfcpu-openxc7:$(sed -n 's/^IMAGE_TAG=//p' "$clone/surfcpu/tools/openxc7-build.sh")
for img in "$tc" "$ox"; do
    if id=$("$docker" image inspect --format '{{.Id}}' "$img" 2>/dev/null); then
        note "image $img present: $id"
    elif [ "$images" = 1 ]; then
        note "image $img missing: building it (make -C surfcpu image openxc7-image)"
        case "$img" in
            surfcpu-toolchain:*) make -C "$clone/surfcpu" image >> "$log" 2>&1 ;;
            *)                   make -C "$clone/surfcpu" openxc7-image >> "$log" 2>&1 ;;
        esac
        note "image $img built ($(stamp))"
    else
        die "the image $img is missing: make -C surfcpu image / openxc7-image (or --build-images)"
    fi
done

# 3. the release, as docs/RELEASE.md says
note "make -C surfcpu release $*"
if make -C "$clone/surfcpu" release "$@"; then rc=0; else rc=$?; fi
note "make release: exit status $rc ($(stamp))"
rel=
for d in "$clone/surfcpu/build/release/"*; do
    [ -f "$d/MANIFEST" ] && rel=$d
done
result=0
if [ "$rc" != 0 ] || [ -z "$rel" ]; then
    note "no release was made; the clone stays in $work"
    keep=1
    result=1
else
    note "release: $rel"
    grep -E '^(release|status|commit) = ' "$rel/MANIFEST" | sed 's/^/  /' | tee -a "$log"
    # 4. against the reference
    if [ -n "$reference" ]; then
        if python3 "$clone/surfcpu/tools/release.py" compare "$reference" "$rel" --report "$work/compare.txt"; then
            note "the release matches the reference $reference"
        else
            note "the release does NOT match the reference $reference (details: $work/compare.txt)"
            result=1
            keep=1
        fi
        cat "$work/compare.txt" >> "$log"
    fi
fi
note "done in $(stamp)"
if [ -n "$evidence" ]; then
    cp "$log" "$evidence"
    echo "$me: evidence in $evidence"
fi
if [ "$keep" = 1 ]; then
    echo "$me: kept $work"
else
    rm -rf "$work"
    echo "$me: removed $work (--keep keeps it)"
fi
exit $result
