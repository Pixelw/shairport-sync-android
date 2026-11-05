/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
#ifndef LX_SMI_ABI_H
#define LX_SMI_ABI_H
#include <stddef.h>
#include "nqptp-shm-structures.h"
_Static_assert(NQPTP_SHM_STRUCTURES_VERSION == 11, "SMI version");
_Static_assert(sizeof(shm_structure_set) == 40, "SMI set size");
_Static_assert(_Alignof(struct shm_structure) == 8, "SMI alignment");
_Static_assert(offsetof(struct shm_structure, main) == 8, "SMI main");
_Static_assert(offsetof(struct shm_structure, secondary) == 48, "SMI secondary");
_Static_assert(sizeof(struct shm_structure) == 88, "SMI total size");
#if defined(__ANDROID__)
_Static_assert(__ANDROID_API__ == 27, "API must be 27");
_Static_assert(sizeof(void *) == 4 && sizeof(long) == 4, "ARM32 ABI");
#if !defined(__arm__) || defined(__ARM_PCS_VFP)
#error "Expected ARM EABI softfp calling convention"
#endif
#endif
#endif
