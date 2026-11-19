#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **The CTS's target for Kosmos** (`docs/gpu3d.md` G2), chosen with
# `-DDEQP_TARGET=kosmos` by `tools/cts_build.py`: no window system and no
# GL, and a platform of Kosmos's own whose Vulkan is lavapipe, linked into
# the image rather than opened from a file.

message("*** Using the Kosmos target")

set(DEQP_TARGET_NAME "Kosmos")

set(TCUTIL_PLATFORM_SRCS
    kosmos/tcuKosmosPlatform.cpp
    kosmos/tcuKosmosMain.cpp
)
