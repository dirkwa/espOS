# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# Header-only: the AudioDriver contract plus a null implementation for
# boards with no audio. The board driver itself lives in the application,
# which is what keeps espOS free of codec-specific code.
idf_component_register(INCLUDE_DIRS include)
