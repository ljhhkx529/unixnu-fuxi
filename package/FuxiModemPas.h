/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The modem's PAS sequence: the steps one image goes through, and the order the two images go through them in.
 *
 * This lives apart from QcomScm.h on purpose: that file is an unchanged copy from UniXNU-sheng and stays that
 * way. It also lives apart from FuxiModemProbe.cpp because it is pure data and pure logic with no kernel
 * dependency, so the host test can check the order against it directly.
 *
 * The order is Linux's (drivers/remoteproc/qcom_q6v5_pas.c, qcom_pas_start):
 *
 *   INIT_IMAGE      the secure world is told where the metadata lives and how to read it
 *   MEM_SETUP       the image is relocatable, so the secure world is told where it will live
 *   AUTH_AND_RESET  the image is authenticated and the processor is reset
 *
 * MEM_SETUP only happens for a relocatable image: a fixed-address image already knows where it goes, and
 * asking the secure world to place it again is an error.
 *
 * The two images go through this in one order only: the modem's device tree blob first, then the modem image.
 * The blob is what tells the modem where its own memory is, so it has to be in place before the modem runs.
 */
#ifndef FUXI_MODEM_PAS_H
#define FUXI_MODEM_PAS_H

#include <stdint.h>

#include "QcomScm.h"

/* The steps one image goes through. */
enum {
    FuxiModemPasInitImage = 0,
    FuxiModemPasMemSetup = 1,
    FuxiModemPasAuthAndReset = 2,
    FuxiModemPasStepCount = 3
};

/* Which of the two images a step belongs to. The order is the list's order: the blob first. */
enum {
    FuxiModemPasImageDtb = 0,
    FuxiModemPasImageModem = 1,
    FuxiModemPasImageCount = 2
};

/* The PAS id of each image. These come from Linux drivers/remoteproc/qcom_q6v5_pas.c, the sm8550_mpss_resource
   entry: the modem image is PAS id 4 and the modem device tree blob is PAS id 0x26 (38). */
#define FUXI_MODEM_PAS 4u
#define FUXI_MODEM_DTB_PAS 0x26u

/* The steps of one image, in order. A relocatable image gets MEM_SETUP between INIT_IMAGE and AUTH_AND_RESET;
   a fixed-address one does not. The count is returned so the caller can check what it got. */
static inline unsigned fuxi_modem_pas_steps(int relocatable, unsigned *out, unsigned max)
{
    unsigned n = 0;
    if (max < 2) return 0;
    out[n++] = FuxiModemPasInitImage;
    if (relocatable) out[n++] = FuxiModemPasMemSetup;
    if (n < max) out[n++] = FuxiModemPasAuthAndReset;
    return n;
}

/* The steps that come before the segments are in the carveout: where the metadata lives, and, for a
   relocatable image, where the image will live. The reset is not one of them.

   Linux's qcom_mdt_pas_load does the metadata and the segment load and leaves "authentication of
   the segments done by a separate call", so the reset belongs after the load. The caller asks for
   the header's steps with this, loads the segments, and then asks for the reset with
   fuxi_modem_pas_reset_step. */
static inline unsigned fuxi_modem_pas_header_steps(int relocatable, unsigned *out, unsigned max)
{
    unsigned n = 0;
    if (max < 1) return 0;
    out[n++] = FuxiModemPasInitImage;
    if (relocatable && n < max) out[n++] = FuxiModemPasMemSetup;
    return n;
}

/* The step that authenticates the image and releases the processor's reset. It is the last step of
   one image, and it happens once, after the segments are in the carveout. */
#define fuxi_modem_pas_reset_step FuxiModemPasAuthAndReset

/* The secure world's command for a step, and its argument-type word. The argument counts and types are the
   ones Linux's qcom_pas_start passes: INIT_IMAGE takes the metadata's address and says it is readable and
   writable, MEM_SETUP takes the image's base and its span, AUTH_AND_RESET takes nothing. */
static inline uint32_t fuxi_modem_pas_command(unsigned step)
{
    switch (step) {
    case FuxiModemPasInitImage: return QCOM_SCM_PAS_INIT_IMAGE;
    case FuxiModemPasMemSetup: return QCOM_SCM_PAS_MEM_SETUP;
    case FuxiModemPasAuthAndReset: return QCOM_SCM_PAS_AUTH_AND_RESET;
    default: return 0;
    }
}

static inline uint32_t fuxi_modem_pas_arginfo(unsigned step)
{
    switch (step) {
    case FuxiModemPasInitImage: return qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0);
    case FuxiModemPasMemSetup: return qcom_scm_args(3, 0, 0, 0);
    case FuxiModemPasAuthAndReset: return qcom_scm_args(1, 0, 0, 0);
    default: return 0;
    }
}

/* The PAS id of an image. */
static inline uint32_t fuxi_modem_pas_id(unsigned image)
{
    return image == FuxiModemPasImageDtb ? FUXI_MODEM_DTB_PAS : FUXI_MODEM_PAS;
}

/* The secure world says success with x0 == 0 and x1 == 0, and nothing else. The positive cookies the first ADSP
   rounds saw (0x00cfffe5 and friends) were the artifact of shifted SMC register arguments, not success values:
   the ADSP record in docs/fuxi-audio-test-results.md shows that an invalid PAS id returned the same cookie. */
static inline int fuxi_modem_scm_ok(int64_t x0, uint64_t x1)
{
    return x0 == 0 && x1 == 0;
}

#endif
