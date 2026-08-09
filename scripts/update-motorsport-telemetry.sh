#!/usr/bin/env bash

set -euo pipefail

upstream_url="https://github.com/tobi/motorsport-telemetry-rs"
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/.." && pwd)"
workspace="${repo_root}/third_party/motorsport-telemetry"
manifest="${workspace}/bridge/Cargo.toml"
lockfile="${workspace}/Cargo.lock"
verify=true
ref="HEAD"
ref_set=false
smoke_files=()

# Cargo discovers the repository's MSRV-aware resolver configuration from the
# current directory and its parents.
cd "${repo_root}"

usage() {
    echo "Usage: $0 [--no-verify] [--smoke-file PATH]... [REF]"
    echo
    echo "Pin every motorsport-telemetry-rs crate to REF (default: upstream HEAD),"
    echo "refresh Cargo.lock, and run the Rust and Omatrack test suites."
    echo "Each --smoke-file is parsed with omatrack-cli after the test suites pass."
}

while (($# > 0)); do
    case "$1" in
        --no-verify)
            verify=false
            ;;
        --smoke-file)
            if (($# < 2)); then
                echo "--smoke-file requires a path." >&2
                exit 2
            fi
            smoke_files+=("$2")
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            if [[ "${ref_set}" == true ]]; then
                echo "Only one REF may be supplied." >&2
                exit 2
            fi
            ref="$1"
            ref_set=true
            ;;
    esac
    shift
done

if [[ "${verify}" != true && ${#smoke_files[@]} -ne 0 ]]; then
    echo "--smoke-file cannot be combined with --no-verify." >&2
    exit 2
fi

for smoke_file in "${smoke_files[@]}"; do
    if [[ ! -f "${smoke_file}" ]]; then
        echo "Smoke-test telemetry file not found: ${smoke_file}" >&2
        exit 1
    fi
done

for command in cargo git sed; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "Required command not found: ${command}" >&2
        exit 1
    fi
done

if [[ "${ref}" =~ ^[0-9a-fA-F]{40}$ ]]; then
    revision="${ref,,}"
else
    revision="$(git ls-remote "${upstream_url}" "${ref}" | awk 'NR == 1 { print $1 }')"
    if [[ -z "${revision}" && "${ref}" != refs/* ]]; then
        revision="$(git ls-remote "${upstream_url}" "refs/heads/${ref}" | awk 'NR == 1 { print $1 }')"
    fi
    if [[ ! "${revision}" =~ ^[0-9a-f]{40}$ ]]; then
        echo "Unable to resolve upstream ref: ${ref}" >&2
        exit 1
    fi
fi

crates=(
    aim-telemetry
    cosworth-telemetry
    motec-telemetry
    motorsport-telemetry-core
    racelogic-telemetry
)

for crate in "${crates[@]}"; do
    if ! grep -Eq "^${crate} = \\{ git = \"${upstream_url}\", rev = \"[0-9a-f]{40}\" \\}$" "${manifest}"; then
        echo "Unexpected or missing dependency declaration for ${crate}." >&2
        exit 1
    fi
done

manifest_backup="$(mktemp)"
lockfile_backup="$(mktemp)"
cp "${manifest}" "${manifest_backup}"
cp "${lockfile}" "${lockfile_backup}"
completed=false

cleanup() {
    if [[ "${completed}" != true ]]; then
        cp "${manifest_backup}" "${manifest}"
        cp "${lockfile_backup}" "${lockfile}"
        echo "Update failed; restored Cargo.toml and Cargo.lock." >&2
    fi
    rm -f -- "${manifest_backup}" "${lockfile_backup}"
}
trap cleanup EXIT

sed -E -i \
    "s#(git = \"${upstream_url}\", rev = \")[0-9a-f]{40}(\" \\})#\\1${revision}\\2#g" \
    "${manifest}"

for crate in "${crates[@]}"; do
    if ! grep -Fqx "${crate} = { git = \"${upstream_url}\", rev = \"${revision}\" }" "${manifest}"; then
        echo "Failed to update dependency declaration for ${crate}." >&2
        exit 1
    fi
done

echo "Pinned motorsport-telemetry-rs crates to ${revision}."
cargo update --manifest-path "${workspace}/Cargo.toml"

if [[ "${verify}" == true ]]; then
    cargo test --manifest-path "${workspace}/Cargo.toml" --workspace --locked
    (
        cd "${repo_root}"
        cmake --preset release
        cmake --build --preset release --parallel
        ctest --preset release --output-on-failure
        for smoke_file in "${smoke_files[@]}"; do
            echo "Parsing smoke-test telemetry: ${smoke_file}"
            ./build/omatrack-cli parse "${smoke_file}" >/dev/null
        done
    )
fi

completed=true
echo "motorsport-telemetry-rs update complete."
