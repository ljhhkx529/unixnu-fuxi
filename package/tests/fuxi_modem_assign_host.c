/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The host check of the modem's memory-ownership step.
 *
 * It checks the real header, not a model of it: FuxiModemAssign.h is the one the probe drives, so if the
 * block's layout changes there this test changes with it.
 *
 * The thing it protects is the shape the secure world reads:
 *
 *   the service is the memory-protection one and the command is the assign one
 *   the argument-type word says seven arguments, the addresses read-only and the sizes plain values
 *   the block holds the source owners, the memory to map and the new owners, each on a 64-byte boundary
 *   the external block holds the arguments that do not fit in the registers, little-endian
 *
 * The layout comes from Linux drivers/firmware/qcom/qcom_scm.c, __qcom_scm_assign_mem, and the transport
 * from drivers/firmware/qcom/qcom_scm-smc.c.
 */
#include <stdio.h>
#include <string.h>

#include "FuxiModemAssign.h"
#include "QcomScm.h"

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

/* Reads a little-endian 64-bit value out of a buffer. */
static uint64_t le64_at(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned b = 0; b < 8; ++b)
        v |= (uint64_t)p[b] << (8 * b);
    return v;
}

/* Reads a little-endian 32-bit value out of a buffer. */
static uint32_t le32_at(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- the service and the command ---- */

static void test_service_and_command(void)
{
    /* Linux drivers/firmware/qcom/qcom_scm.h: QCOM_SCM_SVC_MP is 0x0c and QCOM_SCM_MP_ASSIGN is 0x16. */
    CHECK(FUXI_MODEM_MP_SVC == 0x0c, "the memory-protection service is 0x0c");
    CHECK(FUXI_MODEM_MP_ASSIGN == 0x16, "the assign command is 0x16");
    /* The PAS service is 0x02, so the assign does not go to it. */
    CHECK(FUXI_MODEM_MP_SVC != 0x02, "the assign is not a peripheral authentication call");
}

/* ---- the argument-type word ---- */

/* The argument-type word upstream builds with QCOM_SCM_ARGS(7, RO, VAL, RO, VAL, RO, VAL, VAL).
   The count is in bits 3:0 and the type of argument i is in bits 5+2i:4+2i, with 0 for a plain value
   and 1 for a read-only argument. */
static void test_arginfo(void)
{
    const uint32_t arginfo = fuxi_modem_assign_arginfo();
    CHECK((arginfo & 0xf) == 7, "the call has seven arguments, not %u", arginfo & 0xf);
    CHECK(((arginfo >> 4) & 0x3) == QCOM_SCM_RO_TYPE, "the first argument is read-only");
    CHECK(((arginfo >> 6) & 0x3) == QCOM_SCM_VAL_TYPE, "the second argument is a plain value");
    CHECK(((arginfo >> 8) & 0x3) == QCOM_SCM_RO_TYPE, "the third argument is read-only");
    CHECK(((arginfo >> 10) & 0x3) == QCOM_SCM_VAL_TYPE, "the fourth argument is a plain value");
    CHECK(((arginfo >> 12) & 0x3) == QCOM_SCM_RO_TYPE, "the fifth argument is read-only");
    CHECK(((arginfo >> 14) & 0x3) == QCOM_SCM_VAL_TYPE, "the sixth argument is a plain value");
    CHECK(((arginfo >> 16) & 0x3) == QCOM_SCM_VAL_TYPE, "the seventh argument is a plain value");
    CHECK(arginfo == 0x1117, "the argument-type word is 0x1117, not 0x%x", arginfo);
}

/* ---- the block's layout ---- */

/* The three parts start on 64-byte boundaries and the sizes of the first two are rounded up to 64
   before the third one starts. This is upstream's ALIGN(x, SZ_64). */
static void test_block_offsets(void)
{
    CHECK(fuxi_modem_assign_src_offset(1) == 0, "the source owners are at the start");
    CHECK(fuxi_modem_assign_mem_map_offset(1) == 64, "the memory to map is at 64, not %llu",
          (unsigned long long)fuxi_modem_assign_mem_map_offset(1));
    CHECK(fuxi_modem_assign_perm_offset(1, 1) == 128, "the new owners are at 128, not %llu",
          (unsigned long long)fuxi_modem_assign_perm_offset(1, 1));
    /* The whole block: 64 for the source owners, 64 for the memory to map, 64 for the new owners. */
    CHECK(fuxi_modem_assign_block_size(1, 1) == 192, "the block is 192 bytes, not %llu",
          (unsigned long long)fuxi_modem_assign_block_size(1, 1));
    /* The alignment helper rounds up, it does not round down. */
    CHECK(fuxi_modem_assign_align_up(0) == 0, "zero stays zero");
    CHECK(fuxi_modem_assign_align_up(1) == 64, "one byte rounds up to a whole line");
    CHECK(fuxi_modem_assign_align_up(64) == 64, "a whole line stays a whole line");
    CHECK(fuxi_modem_assign_align_up(65) == 128, "one byte over a line rounds up to the next");
}

/* The bytes of the block, as the secure world reads them. */
static void test_block_bytes(void)
{
    uint8_t block[192];
    memset(block, 0xa5, sizeof block);

    fuxi_modem_assign_write_block(block, FUXI_MODEM_DSM_BASE, FUXI_MODEM_DSM_SIZE,
                                  FUXI_MODEM_VMID_HLOS, FUXI_MODEM_VMID_MSS_MSA, FUXI_MODEM_PERM_RW);

    /* The source owner: the hypervisor's own VMID, 0x3, as a 32-bit little-endian value. */
    CHECK(le32_at(block) == 0x3, "the source owner is the hypervisor's VMID 3, not 0x%x", le32_at(block));
    /* The memory to map: the DSM region's address and its size, as two 64-bit little-endian values. */
    const uint8_t *map = block + fuxi_modem_assign_mem_map_offset(1);
    CHECK(le64_at(map) == 0xd4d00000ull, "the region is 0xd4d00000, not 0x%llx",
          (unsigned long long)le64_at(map));
    CHECK(le64_at(map + 8) == 0x3300000ull, "the region's size is 0x3300000, not 0x%llx",
          (unsigned long long)le64_at(map + 8));
    /* The new owner: the modem's MSA VMID 0xF, and read plus write, which is 0x4 + 0x2 = 0x6. */
    const uint8_t *perm = block + fuxi_modem_assign_perm_offset(1, 1);
    CHECK(le32_at(perm) == 0xf, "the new owner is the modem's MSA VMID 0xf, not 0x%x", le32_at(perm));
    CHECK(le32_at(perm + 4) == 0x6, "the permission is read and write, not 0x%x", le32_at(perm + 4));
    /* The context and its size are zero: upstream leaves them at zero. */
    CHECK(le64_at(perm + 8) == 0, "the context is zero");
    CHECK(le32_at(perm + 16) == 0, "the context's size is zero");
}

/* The region and the identifiers come from Linux and from the port's own memory map. */
static void test_values(void)
{
    /* Linux include/dt-bindings/firmware/qcom,scm.h. */
    CHECK(FUXI_MODEM_VMID_HLOS == 0x3, "the hypervisor's own VMID is 3");
    CHECK(FUXI_MODEM_VMID_MSS_MSA == 0xf, "the modem's MSA VMID is 0xf");
    /* Linux include/linux/firmware/qcom/qcom_scm.h: read is 0x4 and write is 0x2. */
    CHECK(FUXI_MODEM_PERM_READ == 0x4, "read is 0x4");
    CHECK(FUXI_MODEM_PERM_WRITE == 0x2, "write is 0x2");
    CHECK(FUXI_MODEM_PERM_RW == 0x6, "read and write together are 0x6");
    /* Linux arch/arm64/boot/dts/qcom/sm8550.dtsi, mpss_dsm_region; the port's memory map reserves
       the same range (../uefi-fuxi MemoryMapLib.c, mpss_dsm_region). */
    CHECK(FUXI_MODEM_DSM_BASE == 0xd4d00000ull, "the DSM region starts at 0xd4d00000");
    CHECK(FUXI_MODEM_DSM_SIZE == 0x3300000ull, "the DSM region is 0x3300000 bytes");
    /* The region has to be inside the machine's memory, and it has to be a whole number of pages. */
    CHECK((FUXI_MODEM_DSM_SIZE % 0x1000) == 0, "the region's size is a whole number of pages");
    CHECK((FUXI_MODEM_DSM_BASE % 0x1000) == 0, "the region's start is page aligned");
}

/* ---- the external arguments ---- */

/* The seven arguments that do not fit in the registers, as the secure world reads them. */
static void test_external_arguments(void)
{
    uint8_t ext[FUXI_MODEM_ASSIGN_EXT_SIZE];
    memset(ext, 0xa5, sizeof ext);

    const uint64_t src_size = 4;
    const uint64_t dest_addr = 0x80001080ull;
    const uint64_t dest_size = 24;

    fuxi_modem_assign_write_ext(ext, src_size, dest_addr, dest_size);

    /* Upstream's transport copies desc.args[3..9] into this block as seven 64-bit little-endian
       values. The call's arguments are, in order: the memory-to-map address, its size, the block's
       address, the source list's size, the new owners' address, the new owners' size, and zero. So
       the block starts at the fourth argument. */
    CHECK(le64_at(ext + 0) == src_size, "the first external value is the source list's size");
    CHECK(le64_at(ext + 8) == dest_addr, "the second external value is the new owners' address");
    CHECK(le64_at(ext + 16) == dest_size, "the third external value is the new owners' records' size");
    CHECK(le64_at(ext + 24) == 0, "the fourth external value is zero");
    CHECK(le64_at(ext + 32) == 0, "the fifth external value is zero");
    CHECK(le64_at(ext + 40) == 0, "the sixth external value is zero");
    CHECK(le64_at(ext + 48) == 0, "the seventh external value is zero");

    /* The block's size: seven 64-bit values. */
    CHECK(sizeof ext == 56, "the external block is 56 bytes, not %zu", sizeof ext);
}

/* The function id the secure world sees: the SMC64 call, the SiP owner, the service and the
   command. This is the one thing the port's own encoder gets right by construction, so the check
   pins it rather than re-deriving it. */
static void test_function_id(void)
{
    const uint32_t fn = qcom_scm_fn(FUXI_MODEM_MP_SVC, FUXI_MODEM_MP_ASSIGN);
    CHECK((fn & 0xff) == FUXI_MODEM_MP_ASSIGN, "the command is in the low byte");
    CHECK(((fn >> 8) & 0xff) == FUXI_MODEM_MP_SVC, "the service is in the second byte");
    CHECK((fn & (2u << 24)) != 0, "the owner is the SiP one");
    CHECK((fn & (1u << 30)) != 0, "the call is an SMC64 one");
}

/* The whole thing fits in one page, so the assign's block and its external block can share one
   allocation. */
static void test_fits_in_a_page(void)
{
    const uint64_t block = fuxi_modem_assign_block_size(1, 1);
    const uint64_t ext = FUXI_MODEM_ASSIGN_EXT_SIZE;
    CHECK(block + ext <= 4096, "the assign's memory fits in one page");
    /* The external block starts on a 64-byte boundary too, so nothing is packed against it. */
    CHECK((block % FUXI_MODEM_ASSIGN_ALIGN) == 0, "the block's size is a whole number of lines");
}

int main(void)
{
    test_service_and_command();
    test_arginfo();
    test_block_offsets();
    test_block_bytes();
    test_values();
    test_external_arguments();
    test_function_id();
    test_fits_in_a_page();
    if (failures) {
        printf("FAIL fuxi_modem_assign_test: %d\n", failures);
        return 1;
    }
    printf("PASS fuxi_modem_assign_test\n");
    return 0;
}
