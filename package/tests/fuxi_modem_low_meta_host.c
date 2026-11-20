/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The host check of the modem's low metadata window.
 *
 * The window is the answer to a question the device asked: the secure world refused the modem's
 * device tree blob with 0xFFCFFFBB, and upstream's metadata always lives below 4 GiB while this
 * port's mask API cannot put anything there (FuxiModemLowMeta.h has the whole reason). So the
 * address is fixed, and the fixed address has to be somewhere safe.
 *
 * This checks the geometry, on the host, without a device:
 *
 *   the window is below 4 GiB and page aligned
 *   the two slots are inside the window and do not overlap each other
 *   the device tree blob's metadata fits in the first slot and the modem image's in the second
 *   the window overlaps none of the regions it must not touch
 *
 * The sizes are the ones the device log recorded on 2026-11-20: the blob's metadata is 4044 bytes
 * and the modem image's is 6364.
 */
#include <stdio.h>

#include "FuxiModemLowMeta.h"

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

/* The metadata sizes the device reported on 2026-11-20. */
#define DTB_METADATA_BYTES 4044ull
#define IMAGE_METADATA_BYTES 6364ull

static void test_window_address(void)
{
    CHECK(FUXI_MODEM_LOW_META_BASE < 0x100000000ull,
          "the window's base is below 4 GiB, not 0x%llx",
          (unsigned long long)FUXI_MODEM_LOW_META_BASE);
    CHECK((FUXI_MODEM_LOW_META_BASE & (FUXI_MODEM_LOW_META_PAGE - 1)) == 0,
          "the window's base is page aligned");
    CHECK((FUXI_MODEM_LOW_META_SPAN & (FUXI_MODEM_LOW_META_PAGE - 1)) == 0,
          "the window's span is a whole number of pages");
    CHECK(FUXI_MODEM_LOW_META_SPAN > 0, "the window has room in it");
}

static void test_slots(void)
{
    /* Both slots are inside the window. */
    for (unsigned which = 0; which < 2; ++which) {
        const uint64_t offset = fuxi_modem_low_meta_offset(which);
        const uint64_t span = fuxi_modem_low_meta_span(which);
        CHECK(offset + span <= FUXI_MODEM_LOW_META_SPAN,
              "slot %u is inside the window", which);
        CHECK((offset & (FUXI_MODEM_LOW_META_PAGE - 1)) == 0, "slot %u is page aligned", which);
        CHECK(span > 0, "slot %u has room in it", which);
    }
    /* The two slots do not overlap: both metadata buffers are alive at the same time, because Linux
       keeps them until the modem has come up. */
    CHECK(!fuxi_modem_low_meta_overlaps(fuxi_modem_low_meta_address(FuxiModemLowMetaDtb),
                                         fuxi_modem_low_meta_span(FuxiModemLowMetaDtb),
                                         fuxi_modem_low_meta_address(FuxiModemLowMetaImage),
                                         fuxi_modem_low_meta_span(FuxiModemLowMetaImage)),
          "the two slots do not overlap");
    /* The addresses are what the secure world is given, so they are checked directly. */
    CHECK(fuxi_modem_low_meta_address(FuxiModemLowMetaDtb) < 0x100000000ull,
          "the blob's metadata is below 4 GiB");
    CHECK(fuxi_modem_low_meta_address(FuxiModemLowMetaImage) < 0x100000000ull,
          "the image's metadata is below 4 GiB");
}

static void test_the_metadata_fits(void)
{
    /* The blob's metadata is 4044 bytes, which is one page, and the image's is 6364, which is two. */
    CHECK(fuxi_modem_low_meta_fits(FuxiModemLowMetaDtb, DTB_METADATA_BYTES),
          "the blob's metadata fits in the first slot");
    CHECK(fuxi_modem_low_meta_fits(FuxiModemLowMetaImage, IMAGE_METADATA_BYTES),
          "the image's metadata fits in the second slot");
    CHECK(fuxi_modem_low_meta_occupied(DTB_METADATA_BYTES) == FUXI_MODEM_LOW_META_PAGE,
          "the blob's metadata takes one page");
    CHECK(fuxi_modem_low_meta_occupied(IMAGE_METADATA_BYTES) == 2 * FUXI_MODEM_LOW_META_PAGE,
          "the image's metadata takes two pages");
    /* A metadata that is too large must be refused rather than overrun the slot. */
    CHECK(!fuxi_modem_low_meta_fits(FuxiModemLowMetaDtb, 0x1001ull),
          "a metadata of more than one page does not fit the blob's slot");
    CHECK(!fuxi_modem_low_meta_fits(FuxiModemLowMetaImage, 0x2001ull),
          "a metadata of more than two pages does not fit the image's slot");
}

static void test_the_window_is_clear(void)
{
    for (unsigned i = 0; i < FUXI_MODEM_LOW_META_FORBIDDEN_COUNT; ++i) {
        const struct FuxiModemLowMetaRange r = fuxi_modem_low_meta_forbidden(i);
        CHECK(!fuxi_modem_low_meta_overlaps(FUXI_MODEM_LOW_META_BASE, FUXI_MODEM_LOW_META_SPAN,
                                             r.base, r.size),
              "the window does not overlap %s (0x%llx of 0x%llx)", r.name,
              (unsigned long long)r.base, (unsigned long long)r.size);
    }
    /* The window sits exactly between the xbl ramdump region and the xbl device tree log, which is
       where the device tree's reserved-memory list leaves free space below 4 GiB. */
    CHECK(FUXI_MODEM_LOW_META_BASE >= 0x81200000ull + 0x280000ull,
          "the window starts after the xbl ramdump region ends");
    CHECK(FUXI_MODEM_LOW_META_BASE + FUXI_MODEM_LOW_META_SPAN <= 0x81a00000ull,
          "the window ends before the xbl device tree log starts");
}

int main(void)
{
    test_window_address();
    test_slots();
    test_the_metadata_fits();
    test_the_window_is_clear();
    if (failures) {
        printf("FAIL fuxi_modem_low_meta_test: %d\n", failures);
        return 1;
    }
    printf("PASS fuxi_modem_low_meta_test\n");
    return 0;
}
