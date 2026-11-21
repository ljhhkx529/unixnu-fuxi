/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The modem's memory-ownership step: the region assign that hands a reserved memory region from the
 * hypervisor's own owner to the modem's, before the modem is started at all.
 *
 * This is the one structural difference between the ADSP's path and the modem's, and the only one the
 * ADSP does not have. Linux drivers/remoteproc/qcom_q6v5_pas.c, sm8550_mpss_resource, carries
 * region_assign_idx 2, region_assign_count 1 and region_assign_vmid QCOM_SCM_VMID_MSS_MSA; the ADSP's
 * entry carries no region_assign_idx. qcom_pas_assign_memory_region runs in the probe, after
 * qcom_pas_alloc_memory_region and before qcom_pas_init_clock, so before any PAS call at all.
 *
 * The call itself is the memory-protection service, not the peripheral authentication service:
 * service 0x0c, command 0x16, with seven arguments. Seven arguments do not fit in SMC64's registers
 * the way the PAS calls do, so the transport is upstream's (drivers/firmware/qcom/qcom_scm-smc.c):
 * the first four arguments go in x3..x5 and x6, and the rest go in a separate block of memory whose
 * physical address is what x5 ends up carrying. That block is built here, byte for byte, the way
 * upstream's qcom_scm_assign_mem builds it.
 *
 * The block is:
 *
 *   the source owners, one 32-bit VMID each;
 *   the memory to map, a 64-bit address and a 64-bit size;
 *   the new owners, one 24-byte record each: VMID, permission, a 64-bit context, the context's size
 *   and one unused word.
 *
 * Each of the three parts starts on a 64-byte boundary, and the sizes of the first two parts are
 * rounded up to 64 before the third one starts.
 *
 * The block does not need to be secure memory. Upstream puts it in the SCM's TrustZone memory pool,
 * but that pool is backed by an ordinary coherent DMA allocation (drivers/firmware/qcom/qcom_tzmem.c,
 * qcom_tzmem_pool_add_memory) unless CONFIG_QCOM_TZMEM_MODE_SHMBRIDGE is on, and that option is off
 * by default and is off in the port this is taken from. So a plain physically contiguous allocation
 * is what upstream ends up using here too.
 *
 * This lives apart from QcomScm.h on purpose: that file is an unchanged copy from UniXNU-sheng and
 * stays that way. It is pure data and pure logic with no kernel dependency, so the host test checks
 * the block's bytes and the argument's shape against it directly.
 */
#ifndef FUXI_MODEM_ASSIGN_H
#define FUXI_MODEM_ASSIGN_H

#include <stdint.h>

/* The memory-protection service and its assign command. Linux drivers/firmware/qcom/qcom_scm.h:
   QCOM_SCM_SVC_MP is 0x0c and QCOM_SCM_MP_ASSIGN is 0x16. */
#define FUXI_MODEM_MP_SVC 0x0cu
#define FUXI_MODEM_MP_ASSIGN 0x16u

/* The virtual machine identifiers. Linux include/dt-bindings/firmware/qcom,scm.h: the hypervisor's own
   owner is 0x3 and the modem's MSA is 0xF. The navigation processor's is 0x2b, which the reference
   hands the same memory as well (hakios.mpss.c, rmtfs_share: VMID_NAV 0x2b). */
#define FUXI_MODEM_VMID_HLOS 0x3u
#define FUXI_MODEM_VMID_MSS_MSA 0xFu
#define FUXI_MODEM_VMID_NAV 0x2bu

/* The permissions. Linux include/linux/firmware/qcom/qcom_scm.h: read is 0x4, write is 0x2, so read
   and write together is 0x6. */
#define FUXI_MODEM_PERM_READ 0x4u
#define FUXI_MODEM_PERM_WRITE 0x2u
#define FUXI_MODEM_PERM_RW (FUXI_MODEM_PERM_READ | FUXI_MODEM_PERM_WRITE)

/* The region that is handed over. It is the board's third reserved memory region, the DSM region,
   from Linux arch/arm64/boot/dts/qcom/sm8550.dtsi, mpss_dsm_region. The port's own memory map
   reserves the same range: ../uefi-fuxi MemoryMapLib.c lists mpss_dsm_region at 0xD4D00000 of
   0x3300000 bytes. */
#define FUXI_MODEM_DSM_BASE 0xd4d00000ull
#define FUXI_MODEM_DSM_SIZE 0x3300000ull

/* The three parts of the block, and their sizes. The source list holds one 32-bit VMID per owner;
   the memory to map is two 64-bit values; the new owner is 24 bytes: a 32-bit VMID, a 32-bit
   permission, a 64-bit context, a 32-bit context size and a 32-bit unused word. */
#define FUXI_MODEM_ASSIGN_SRC_ENTRY 4u
#define FUXI_MODEM_ASSIGN_MEM_MAP_SIZE 16u
#define FUXI_MODEM_ASSIGN_PERM_SIZE 24u

/* The block's parts start on 64-byte boundaries, and the sizes of the first two are rounded up to 64
   before the third one starts. Upstream's ALIGN(x, SZ_64). */
#define FUXI_MODEM_ASSIGN_ALIGN 64u

static inline uint64_t fuxi_modem_assign_align_up(uint64_t value)
{
    return (value + (FUXI_MODEM_ASSIGN_ALIGN - 1)) & ~(uint64_t)(FUXI_MODEM_ASSIGN_ALIGN - 1);
}

/* Where each part of the block starts, given how many source owners there are and how many new owners
   there are. Upstream's qcom_scm_assign_mem computes the same three offsets. */
static inline uint64_t fuxi_modem_assign_src_offset(unsigned src_count)
{
    (void)src_count;
    return 0;
}

static inline uint64_t fuxi_modem_assign_mem_map_offset(unsigned src_count)
{
    return fuxi_modem_assign_align_up((uint64_t)src_count * FUXI_MODEM_ASSIGN_SRC_ENTRY);
}

static inline uint64_t fuxi_modem_assign_perm_offset(unsigned src_count, unsigned dest_count)
{
    (void)dest_count;
    return fuxi_modem_assign_mem_map_offset(src_count) +
           fuxi_modem_assign_align_up(FUXI_MODEM_ASSIGN_MEM_MAP_SIZE);
}

/* The whole block's size. */
static inline uint64_t fuxi_modem_assign_block_size(unsigned src_count, unsigned dest_count)
{
    return fuxi_modem_assign_perm_offset(src_count, dest_count) +
           fuxi_modem_assign_align_up((uint64_t)dest_count * FUXI_MODEM_ASSIGN_PERM_SIZE);
}

/* The seven arguments of the call, in upstream's order (drivers/firmware/qcom/qcom_scm.c,
   __qcom_scm_assign_mem): the memory to map's address and size, the block's address and the size of
   the source list, the new owners' address and the size of the new owners' records. The seventh is
   zero. */
#define FUXI_MODEM_ASSIGN_ARG_COUNT 7u

/* The argument-type word. Upstream: QCOM_SCM_ARGS(7, RO, VAL, RO, VAL, RO, VAL, VAL), which is the
   count in bits 3:0 and the type of argument i in bits 5+2i:4+2i. The arguments that are addresses
   are read-only, the ones that are sizes are plain values. */
#define QCOM_SCM_VAL_TYPE 0u
#define QCOM_SCM_RO_TYPE 1u

static inline uint32_t fuxi_modem_assign_arginfo(void)
{
    return (FUXI_MODEM_ASSIGN_ARG_COUNT & 0xf) |
           (QCOM_SCM_RO_TYPE << 4) | (QCOM_SCM_VAL_TYPE << 6) |
           (QCOM_SCM_RO_TYPE << 8) | (QCOM_SCM_VAL_TYPE << 10) |
           (QCOM_SCM_RO_TYPE << 12) | (QCOM_SCM_VAL_TYPE << 14) |
           (QCOM_SCM_VAL_TYPE << 16);
}

/* Where the seven arguments go.
 *
 * Upstream's transport (drivers/firmware/qcom/qcom_scm-smc.c) puts the function id in x0, the
 * argument-type word in x1, and desc.args[0..3] in x2..x5. When there are more than four arguments it
 * copies desc.args[3..9] into a separate block of seven 64-bit little-endian values, and the
 * block's physical address is what x5 ends up carrying: x5's own value, desc.args[3], is overwritten.
 *
 * So for this call:
 *
 *   x2  desc.args[0]  the address of the memory-to-map record
 *   x3  desc.args[1]  the size of that record, 16
 *   x4  desc.args[2]  the address of the assignment block
 *   x5  the address of the external arguments block
 *
 * and the external block holds:
 *
 *   [0]  desc.args[3]  the size of the source list, 4
 *   [1]  desc.args[4]  the address of the new owners' records
 *   [2]  desc.args[5]  the size of the new owners' records, 24
 *   [3]  desc.args[6]  zero
 *   [4..6]  the rest of the block, zero
 *
 * The quirk's session register is x6 (arm_smccc_quirk.state.a6), and the port's SMC64 quirk puts the
 * session there too, so the argument block's address cannot go in x6. Upstream puts it in x5, and so
 * does this.
 */
#define FUXI_MODEM_ASSIGN_EXT_COUNT 7u

/* The block of external arguments: seven 64-bit little-endian values, of which the first four carry
   what the secure world reads and the rest are zero. */
#define FUXI_MODEM_ASSIGN_EXT_SIZE (FUXI_MODEM_ASSIGN_EXT_COUNT * 8u)

/* Which of the external values the secure world reads for what. */
enum {
    FuxiModemAssignExtSrcSize = 0,
    FuxiModemAssignExtDestAddress = 1,
    FuxiModemAssignExtDestSize = 2,
};

/* The block of external arguments, written little-endian the way upstream writes it (the values go
   into the block with cpu_to_le64).
   The block holds the arguments that do not fit in the registers, which are the call's fourth
   argument onwards: the size of the source list, the address of the new owners' records, the size of
   those records, and then the call's seventh argument, which is zero. */
static inline void fuxi_modem_assign_write_ext(uint8_t *ext, uint64_t src_size,
                                                uint64_t dest_addr, uint64_t dest_size)
{
    const uint64_t values[7] = {src_size, dest_addr, dest_size, 0, 0, 0, 0};
    for (unsigned i = 0; i < 7; ++i) {
        for (unsigned b = 0; b < 8; ++b)
            ext[i * 8 + b] = (uint8_t)(values[i] >> (8 * b));
    }
}

/* The block of the assignment itself, written little-endian the way upstream writes it. The three
   parts are the source owners, the memory to map, and the new owners.
 *
   The one owner is the modem's DSM region, which goes from the hypervisor's own owner to the modem's
   MSA alone. */
static inline void fuxi_modem_assign_write_block(uint8_t *block, uint64_t mem_addr, uint64_t mem_size,
                                                  uint32_t src_vmid, uint32_t dest_vmid, uint32_t dest_perm)
{
    /* The source list: one 32-bit VMID per owner. */
    for (unsigned b = 0; b < 4; ++b)
        block[b] = (uint8_t)(src_vmid >> (8 * b));

    /* The memory to map: a 64-bit address and a 64-bit size. */
    uint8_t *map = block + fuxi_modem_assign_mem_map_offset(1);
    for (unsigned b = 0; b < 8; ++b) {
        map[b] = (uint8_t)(mem_addr >> (8 * b));
        map[8 + b] = (uint8_t)(mem_size >> (8 * b));
    }

    /* The new owner: a 32-bit VMID, a 32-bit permission, a 64-bit context, the context's size and one
       unused word. Upstream leaves the context and its size at zero. */
    uint8_t *perm = block + fuxi_modem_assign_perm_offset(1, 1);
    for (unsigned b = 0; b < 4; ++b) {
        perm[b] = (uint8_t)(dest_vmid >> (8 * b));
        perm[4 + b] = (uint8_t)(dest_perm >> (8 * b));
    }
    for (unsigned b = 0; b < 16; ++b) perm[8 + b] = 0;
}

/* The same block, for several owners on each side.
 *
 * The modem's RMTFS buffer is handed to three owners and not one: the hypervisor's own owner, the
 * modem's MSA and the navigation processor (hakios.mpss.c, rmtfs_share: the destinations are HLOS,
 * MSS_MSA and NAV, all with read and write). The block's shape does not change with the number of
 * owners: the source list grows by one 32-bit VMID each, and the new owners' records grow by one
 * 24-byte record each. Both lists start on 64-byte boundaries, and the sizes of the first two parts
 * are rounded up to 64 before the third one starts.
 *
 * The vmids and the permissions are arrays of the same length, one entry per owner. */
static inline void fuxi_modem_assign_write_block_many(uint8_t *block, uint64_t mem_addr, uint64_t mem_size,
                                                       const uint32_t *src_vmids, unsigned src_count,
                                                       const uint32_t *dest_vmids, const uint32_t *dest_perms,
                                                       unsigned dest_count)
{
    /* The source list: one 32-bit VMID per owner. */
    for (unsigned i = 0; i < src_count; ++i)
        for (unsigned b = 0; b < 4; ++b)
            block[i * FUXI_MODEM_ASSIGN_SRC_ENTRY + b] = (uint8_t)(src_vmids[i] >> (8 * b));

    /* The memory to map: a 64-bit address and a 64-bit size. */
    uint8_t *map = block + fuxi_modem_assign_mem_map_offset(src_count);
    for (unsigned b = 0; b < 8; ++b) {
        map[b] = (uint8_t)(mem_addr >> (8 * b));
        map[8 + b] = (uint8_t)(mem_size >> (8 * b));
    }

    /* The new owners: each a 32-bit VMID, a 32-bit permission, a 64-bit context, the context's size
       and one unused word. Upstream leaves the context and its size at zero. */
    uint8_t *perm = block + fuxi_modem_assign_perm_offset(src_count, dest_count);
    for (unsigned i = 0; i < dest_count; ++i) {
        uint8_t *record = perm + i * FUXI_MODEM_ASSIGN_PERM_SIZE;
        for (unsigned b = 0; b < 4; ++b) {
            record[b] = (uint8_t)(dest_vmids[i] >> (8 * b));
            record[4 + b] = (uint8_t)(dest_perms[i] >> (8 * b));
        }
        for (unsigned b = 0; b < 16; ++b) record[8 + b] = 0;
    }
}

#endif
