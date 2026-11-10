#!/bin/sh
# Replace include/clang-c and LICENSE.TXT with the files from an LLVM release
# tag, and record the tag and commit in NOTICE.md and README.md.
#
#   scripts/update-headers.sh llvmorg-19.1.7
#
# Needs curl and git. Only the headers and the license are downloaded; the
# LLVM repository is not cloned.
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <llvm-release-tag, for example llvmorg-19.1.7>" >&2
    exit 2
fi
tag=$1
case $tag in
    llvmorg-[0-9]*) ;;
    *) echo "error: expected a tag like llvmorg-19.1.7, got '$tag'" >&2; exit 2 ;;
esac

root=$(cd "$(dirname "$0")/.." && pwd)
upstream=https://github.com/llvm/llvm-project.git
raw=https://raw.githubusercontent.com/llvm/llvm-project
api=https://api.github.com/repos/llvm/llvm-project/contents/clang/include/clang-c

# The commit the tag points to (the peeled entry for annotated tags).
commit=$(git ls-remote "$upstream" "refs/tags/$tag" "refs/tags/$tag^{}" |
    awk '{ sha = $1 } /\^\{\}$/ { peeled = $1 } END { print (peeled != "" ? peeled : sha) }')
if [ -z "$commit" ]; then
    echo "error: $upstream has no tag $tag" >&2
    exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

# List the headers at that commit rather than assuming last release's set.
curl -fsSL "$api?ref=$commit" -o "$work/listing.json"
headers=$(sed -n 's/^ *"name": *"\([A-Za-z0-9_]*\.h\)",*$/\1/p' "$work/listing.json")
if [ -z "$headers" ]; then
    echo "error: no headers listed in clang/include/clang-c at $tag" >&2
    exit 1
fi

mkdir "$work/clang-c"
for header in $headers; do
    curl -fsSL "$raw/$commit/clang/include/clang-c/$header" -o "$work/clang-c/$header"
done
curl -fsSL "$raw/$commit/llvm/LICENSE.TXT" -o "$work/LICENSE.TXT"
test -s "$work/clang-c/Index.h"

rm -rf "$root/include/clang-c"
mkdir -p "$root/include"
mv "$work/clang-c" "$root/include/clang-c"
mv "$work/LICENSE.TXT" "$root/LICENSE.TXT"

version=${tag#llvmorg-}
cat > "$root/NOTICE.md" <<NOTICE
# Notice

The files in \`include/clang-c/\` are the libclang C API headers from the
LLVM Project, copied without modification from:

- Repository: <https://github.com/llvm/llvm-project>
- Tag: \`$tag\` (LLVM $version)
- Commit: \`$commit\`
- Directory: \`clang/include/clang-c/\`

\`LICENSE.TXT\` is \`llvm/LICENSE.TXT\` from the same commit. The LLVM Project
is under the Apache License v2.0 with LLVM Exceptions; the full text and the
notices for other licenses LLVM contains are in \`LICENSE.TXT\`.

The loader in \`src/\`, the build files, the scripts, and the tests in this
repository are original to neura-libclang and are under the same license.

\`scripts/update-headers.sh\` writes this file.
NOTICE

# README.md names the pin on the line marked "Pinned to".
sed -i.bak "s|^Pinned to .*|Pinned to LLVM $version (tag \`$tag\`, commit \`$commit\`).|" \
    "$root/README.md"
rm -f "$root/README.md.bak"

echo "include/clang-c now matches $tag ($commit):"
ls "$root/include/clang-c"
