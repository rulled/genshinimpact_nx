#!/bin/sh

set -eu
export LC_ALL=C

usage() {
  echo "usage: $0 LIBVULKAN_A VULKAN_BRIDGE_O FINAL_ELF" >&2
  exit 2
}

fail() {
  echo "loaderless ABI check failed: $*" >&2
  exit 1
}

[ "$#" -eq 3 ] || usage

archive=$1
bridge_object=$2
final_elf=$3

for input in "$archive" "$bridge_object" "$final_elf"; do
  [ -f "$input" ] || fail "missing input: $input"
done

toolchain_bin=${DEVKITA64:-${DEVKITPRO:-/opt/devkitpro}/devkitA64}/bin
nm=${NM:-$toolchain_bin/aarch64-none-elf-nm}
if [ ! -x "$nm" ]; then
  nm=$(command -v aarch64-none-elf-nm 2>/dev/null || true)
fi
[ -n "$nm" ] && [ -x "$nm" ] || fail "aarch64-none-elf-nm not found"

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/loaderless-abi.XXXXXX") ||
  fail "cannot create temporary directory"
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

archive_defined=$tmp_dir/archive-defined
bridge_defined=$tmp_dir/bridge-defined
bridge_undefined=$tmp_dir/bridge-undefined
elf_defined=$tmp_dir/elf-defined
elf_undefined=$tmp_dir/elf-undefined

"$nm" -g --defined-only "$archive" >"$archive_defined"
"$nm" -g --defined-only "$bridge_object" >"$bridge_defined"
"$nm" -u "$bridge_object" >"$bridge_undefined"
"$nm" -g --defined-only "$final_elf" >"$elf_defined"
"$nm" -u "$final_elf" >"$elf_undefined"

has_symbol() {
  awk -v symbol="$2" '$NF == symbol { found = 1 } END { exit !found }' "$1"
}

undefined_vk_symbols() {
  awk '$NF ~ /^vk[A-Za-z0-9_]*(@.*)?$/ { sub(/@.*/, "", $NF); print $NF }' "$1" |
    sort -u
}

has_symbol "$archive_defined" vkGetInstanceProcAddr ||
  fail "libvulkan.a does not define vkGetInstanceProcAddr"

# Some loaderless Mesa archives keep vkGetDeviceProcAddr internal. The bridge
# intentionally supplies that Android loader root and obtains it through GIPA.
has_symbol "$bridge_defined" nx_vkGetDeviceProcAddr ||
  fail "bridge object does not define nx_vkGetDeviceProcAddr"

bridge_vk_imports=$(undefined_vk_symbols "$bridge_undefined")
if printf '%s\n' "$bridge_vk_imports" | grep -qx vkQueueWaitIdle; then
  fail "bridge object depends on vkQueueWaitIdle"
fi

unexpected_bridge_imports=$(printf '%s\n' "$bridge_vk_imports" |
  awk 'NF && $0 != "vkGetInstanceProcAddr" && $0 != "vkGetDeviceProcAddr"')
[ -z "$unexpected_bridge_imports" ] ||
  fail "bridge object imports non-root Vulkan symbols: $(printf '%s' "$unexpected_bridge_imports" | tr '\n' ' ')"

if printf '%s\n' "$bridge_vk_imports" | grep -qx vkGetDeviceProcAddr &&
   ! has_symbol "$archive_defined" vkGetDeviceProcAddr; then
  fail "bridge imports vkGetDeviceProcAddr but libvulkan.a does not export it"
fi

elf_vk_imports=$(undefined_vk_symbols "$elf_undefined")
if printf '%s\n' "$elf_vk_imports" | grep -qx vkQueueWaitIdle; then
  fail "final ELF depends on unresolved vkQueueWaitIdle"
fi

[ -z "$elf_vk_imports" ] ||
  fail "final ELF has unresolved Vulkan symbols: $(printf '%s' "$elf_vk_imports" | tr '\n' ' ')"

for symbol in \
  nx_vkCreateInstance \
  nx_vkEnumerateInstanceExtensionProperties \
  nx_vkGetInstanceProcAddr \
  nx_vkGetDeviceProcAddr \
  nx_vkCreateAndroidSurfaceKHR \
  nx_vk_lookup; do
  has_symbol "$elf_defined" "$symbol" ||
    fail "final ELF does not define required bridge entry point $symbol"
done

has_symbol "$elf_defined" vkGetInstanceProcAddr ||
  fail "final ELF does not contain the loaderless vkGetInstanceProcAddr root"

echo "Loaderless Vulkan ABI checks passed."
