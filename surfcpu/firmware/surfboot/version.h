/*
SurfBoot: version
--------------------
File: version.h Date: 10/15/26 (SurfCPU)
--------------------
SurfBoot's own version (major << 16 | minor << 8 | patch, as in the boot information).
The build identity (the first 8 hex digits of the source commit, and whether the tree was
dirty) comes from fw_build.h, which the build generates; host builds get zeros.
*/
#ifndef SURFBOOT_VERSION_H
#define SURFBOOT_VERSION_H

#define SURFBOOT_VERSION_MAJOR 1
#define SURFBOOT_VERSION_MINOR 0
#define SURFBOOT_VERSION_PATCH 0
#define SURFBOOT_VERSION ((SURFBOOT_VERSION_MAJOR << 16) | (SURFBOOT_VERSION_MINOR << 8) | SURFBOOT_VERSION_PATCH)

#if defined(__has_include)
#if __has_include("fw_build.h")
#include "fw_build.h"
#endif
#endif
#ifndef FW_BUILD_ID
#define FW_BUILD_ID 0x00000000u
#endif
#ifndef FW_BUILD_DIRTY
#define FW_BUILD_DIRTY 0
#endif

#endif /* SURFBOOT_VERSION_H */
