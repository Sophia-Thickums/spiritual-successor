#!/bin/sh
# build.sh — build and install the present counter.
#
# WHAT IT IS: a Vulkan implicit layer that counts vkQueuePresentKHR calls. Placed BELOW the
# frame-gen layer in the chain, it is the only instrument that can see the frames frame
# generation INSERTS — no in-game FPS counter, no overlay above the layer, and no compositor
# scan-out counter can. It is a witness, not a participant: it changes no image, no present
# mode and no pacing, and it counts exactly one function.
#
# SAFE BY DEFAULT: the layer is completely INERT unless SPIRIT_PRESENT_COUNT names a file.
# Installed and forgotten, it does nothing. To remove it entirely:
#     sudo rm /usr/local/lib/libspirit_present_counter.so \
#             /usr/share/vulkan/implicit_layer.d/VkLayer_SPIRIT_present_counter.json
# To disable without removing: export SPIRIT_PRESENT_COUNTER_DISABLE=1
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
LIB=/usr/local/lib/libspirit_present_counter.so
MAN=/usr/share/vulkan/implicit_layer.d/VkLayer_SPIRIT_present_counter.json

echo "building…"
cc -O2 -Wall -Wextra -shared -fPIC -o "$HERE/libspirit_present_counter.so" \
   "$HERE/spirit_present_counter.c"
echo "  built $HERE/libspirit_present_counter.so"

echo "installing (needs root)…"
sudo install -Dm755 "$HERE/libspirit_present_counter.so" "$LIB"
sudo install -Dm644 "$HERE/VkLayer_SPIRIT_present_counter.json" "$MAN"
echo "  $LIB"
echo "  $MAN"

echo
echo "verify — this must print the layer name:"
VK_LOADER_DEBUG=layer vkcube --c 1 2>&1 | grep -i 'SPIRIT_present_counter' | head -3 || \
  echo "  (vkcube not found; run any Vulkan program with VK_LOADER_DEBUG=layer to check)"
echo
echo "the layer is now installed and INERT. It counts nothing until a program sets"
echo "  SPIRIT_PRESENT_COUNT=/path/to/count-file"
echo "which is exactly what 'spirit play --framegen N' does for you."
