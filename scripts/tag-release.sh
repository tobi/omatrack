#!/usr/bin/env bash
# Tag only the exact remote-main commit that passed cross-platform CI.
set -euo pipefail

version=${1:?usage: scripts/tag-release.sh VERSION [--check]}
check=${2:-}
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'Expected a stable X.Y.Z version' >&2; exit 2; }
[[ -z $check || $check == --check ]] || { echo 'Only --check is supported' >&2; exit 2; }
cd "$(git rev-parse --show-toplevel)"
[[ -z $(git status --porcelain=v1) ]] || { echo 'Commit changes before tagging a release' >&2; exit 1; }
actual=$(awk '$1 == "project(omatrack" && $2 == "VERSION" { print $3; exit }' CMakeLists.txt)
[[ $actual == "$version" ]] || { echo "CMake version is $actual, not $version" >&2; exit 1; }

git fetch origin main --tags
sha=$(git rev-parse HEAD)
[[ $sha == "$(git rev-parse origin/main)" ]] || { echo 'HEAD must be the exact pushed origin/main commit' >&2; exit 1; }
tag="v$version"
if git show-ref --verify --quiet "refs/tags/$tag"; then
    echo "$tag already exists; refusing to replace it" >&2
    exit 1
fi
repo=$(gh repo view --json nameWithOwner --jq .nameWithOwner)
state=$(gh run list --repo "$repo" --workflow ci.yml --branch main --event push --commit "$sha" --limit 1 \
    --json status,conclusion --jq '.[0] | "\(.status):\(.conclusion)"')
[[ $state == completed:success ]] || {
    echo "Cross-platform CI is not green for $sha ($state); no tag created" >&2
    exit 1
}
echo "Release gate passed: $tag at $sha"
[[ $check == --check ]] && exit 0

git tag -a "$tag" "$sha" -m "Omatrack $version"
if ! git push origin "refs/tags/$tag"; then
    echo "Tag exists locally; retry: git push origin refs/tags/$tag" >&2
    exit 1
fi
