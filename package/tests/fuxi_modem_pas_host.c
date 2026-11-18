/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The host check of the modem's PAS sequence.
 *
 * It checks the real header, not a model of it: FuxiModemPas.h is the one the probe drives, so if the order
 * changes there this test changes with it. The thing it protects is the order the secure world expects:
 *
 *   the blob first, then the modem image, because the blob is what tells the modem where its own memory is
 *   INIT_IMAGE, then MEM_SETUP, then AUTH_AND_RESET
 *   MEM_SETUP only for a relocatable image
 *   success is x0 == 0 and x1 == 0, and nothing else
 *
 * The order comes from Linux drivers/remoteproc/qcom_q6v5_pas.c, qcom_pas_start.
 */
#include <stdio.h>
#include <string.h>

#include "FuxiModemPas.h"

static int failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ++failures;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* The command the secure world sees for each step, as the probe would send it. */
static uint32_t command_of(unsigned step)
{
    return fuxi_modem_pas_command(step);
}

/* ---- the steps of one image ---- */

static void test_relocatable_steps(void)
{
    unsigned steps[FuxiModemPasStepCount];
    const unsigned n = fuxi_modem_pas_steps(1, steps, FuxiModemPasStepCount);
    CHECK(n == 3, "a relocatable image has three steps, not %u", n);
    if (n != 3) return;
    CHECK(steps[0] == FuxiModemPasInitImage, "the first step is INIT_IMAGE");
    CHECK(steps[1] == FuxiModemPasMemSetup, "the second step is MEM_SETUP");
    CHECK(steps[2] == FuxiModemPasAuthAndReset, "the third step is AUTH_AND_RESET");
}

static void test_fixed_steps(void)
{
    unsigned steps[FuxiModemPasStepCount];
    const unsigned n = fuxi_modem_pas_steps(0, steps, FuxiModemPasStepCount);
    CHECK(n == 2, "a fixed-address image has two steps, not %u", n);
    if (n != 2) return;
    CHECK(steps[0] == FuxiModemPasInitImage, "the first step is INIT_IMAGE");
    CHECK(steps[1] == FuxiModemPasAuthAndReset, "a fixed-address image skips MEM_SETUP");
}

static void test_step_commands(void)
{
    CHECK(command_of(FuxiModemPasInitImage) == QCOM_SCM_PAS_INIT_IMAGE, "INIT_IMAGE is command 1");
    CHECK(command_of(FuxiModemPasMemSetup) == QCOM_SCM_PAS_MEM_SETUP, "MEM_SETUP is command 2");
    CHECK(command_of(FuxiModemPasAuthAndReset) == QCOM_SCM_PAS_AUTH_AND_RESET, "AUTH_AND_RESET is command 5");
    CHECK(command_of(FuxiModemPasStepCount) == 0, "an unknown step has no command");
}

static void test_step_arginfo(void)
{
    /* INIT_IMAGE takes the metadata's address, and says the argument is read and write. */
    CHECK(fuxi_modem_pas_arginfo(FuxiModemPasInitImage) == qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0),
          "INIT_IMAGE takes two arguments, the second read-write");
    /* MEM_SETUP takes the image's base and its span. */
    CHECK(fuxi_modem_pas_arginfo(FuxiModemPasMemSetup) == qcom_scm_args(3, 0, 0, 0),
          "MEM_SETUP takes three arguments");
    /* AUTH_AND_RESET takes nothing. */
    CHECK(fuxi_modem_pas_arginfo(FuxiModemPasAuthAndReset) == qcom_scm_args(1, 0, 0, 0),
          "AUTH_AND_RESET takes one argument");
}

static void test_pas_ids(void)
{
    /* Linux drivers/remoteproc/qcom_q6v5_pas.c, the sm8550_mpss_resource entry. */
    CHECK(fuxi_modem_pas_id(FuxiModemPasImageModem) == 4, "the modem image is PAS id 4");
    CHECK(fuxi_modem_pas_id(FuxiModemPasImageDtb) == 0x26, "the modem device tree blob is PAS id 0x26");
    CHECK(fuxi_modem_pas_id(FuxiModemPasImageDtb) != fuxi_modem_pas_id(FuxiModemPasImageModem),
          "the two images have different PAS ids");
}

/* ---- the order of the two images ---- */

/* The probe's stepPas: the blob is authenticated, loaded and reset before the modem image is touched at all.
   This reproduces that order from the header's own numbering, so a change to the numbering shows up here. */
static void test_image_order(void)
{
    /* The images are numbered in the order they are brought up: the blob is 0, the modem image is 1. */
    CHECK(FuxiModemPasImageDtb < FuxiModemPasImageModem, "the blob comes before the modem image");

    /* The blob's PAS id is the one the secure world authenticates first. A wrong order here would mean the
       modem image was reset while the blob that describes its memory was not yet in place. */
    CHECK(fuxi_modem_pas_id(FuxiModemPasImageDtb) == FUXI_MODEM_DTB_PAS, "the blob keeps its own PAS id");
    CHECK(fuxi_modem_pas_id(FuxiModemPasImageModem) == FUXI_MODEM_PAS, "the modem image keeps its own PAS id");
}

/* ---- the success test ---- */

static void test_success_is_strict(void)
{
    CHECK(fuxi_modem_scm_ok(0, 0), "x0 == 0 and x1 == 0 is success");
    /* The cookie the first ADSP rounds saw. It was the artifact of shifted SMC register arguments, not a
       success value: an invalid PAS id returned the same cookie. */
    CHECK(!fuxi_modem_scm_ok(0x00cfffe5, 0), "a nonzero cookie is not success");
    CHECK(!fuxi_modem_scm_ok(0, 1), "a nonzero x1 is not success");
    CHECK(!fuxi_modem_scm_ok(-1, 0), "a negative x0 is not success");
    CHECK(!fuxi_modem_scm_ok(-12, 0), "a busy answer is not success");
}

int main(void)
{
    test_relocatable_steps();
    test_fixed_steps();
    test_step_commands();
    test_step_arginfo();
    test_pas_ids();
    test_image_order();
    test_success_is_strict();
    if (failures) {
        printf("FAIL fuxi_modem_pas_test: %d\n", failures);
        return 1;
    }
    printf("PASS fuxi_modem_pas_test\n");
    return 0;
}
