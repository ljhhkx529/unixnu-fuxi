/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The two CS35L41 speaker amplifiers of the fuxi board (Xiaomi 13).
 *
 * The program tables are derived from Kerfand's UniXNU-Fuxi-Arm64e-iOS17
 * (device/package/FuxiCs35l41.c, BSD-2-Clause-Patent, 2026-11-13), which in
 * turn derives them from the Linux mainline driver (include/sound/cs35l41.h,
 * sound/soc/codecs/cs35l41-lib.c and cs35l41.c).  The board facts come from
 * this board's own device tree (uefi-fuxi
 * Platforms/Xiaomi/fuxiPkg/FdtBlob/sm8550-xiaomi-fuxi.dtb), not from the
 * reference: the I2C addresses, the reset pins, the boost values and the
 * channel order all come from the fuxi tree.
 *
 * The programs:
 *   init      - software reset, OTP boot wait, identity (DEVID, REVID,
 *               IRQ1_STATUS4, IRQ1_STATUS3), the revision errata, the
 *               interrupt masks, the boost converter, the serial port
 *               format, the PLL and the routing for this amplifier's slot.
 *               The amplifier is left powered down: nothing can make noise.
 *   status    - IRQ1_STATUS1 and IRQ1_STATUS3, then acknowledges both.
 *   power up  - the pup/pdn patch, the global enable and the main amplifier
 *               bit.  Not run during P4.
 *   power down- the reverse.  Not run during P4.
 *
 * Registers and values are 32-bit big-endian on the wire.  The step runner
 * is the local one (Cs35Run.h), whose opcodes and layout are the same. */
#ifndef FUXI_CS35L41_H
#define FUXI_CS35L41_H
#include <stdint.h>

#include "Cs35Run.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The part's registers (Linux include/sound/cs35l41.h). */
#define CS35_DEVID 0x00000000u
#define CS35_REVID 0x00000004u
#define CS35_OTPID 0x00000010u
#define CS35_SFT_RESET 0x00000020u
#define CS35_TEST_KEY 0x00000040u
#define CS35_PWR_CTRL1 0x00002014u
#define CS35_PWR_CTRL2 0x00002018u
#define CS35_PWR_CTRL3 0x0000201Cu
#define CS35_PROTECT_REL_ERR_IGN 0x00002034u
#define CS35_AMP_OUT_MUTE 0x00002024u
#define CS35_GPIO_PAD_CONTROL 0x0000242Cu
#define CS35_PWRMGT_STS 0x00002908u
#define CS35_PLL_CLK_CTRL 0x00002C04u
#define CS35_GLOBAL_CLK_CTRL 0x00002C0Cu
#define CS35_TST_FS_MON0 0x00002D10u
#define CS35_BSTCVRT_PEAK_CUR 0x00003808u
#define CS35_BSTCVRT_COEFF 0x00003810u
#define CS35_BSTCVRT_SLOPE_LBST 0x00003814u
#define CS35_BSTCVRT_DCM_CTRL 0x0000381Cu
#define CS35_VIMON_SPKMON_RESYNC 0x00004100u
#define CS35_VPVBST_FS_SEL 0x00004400u
#define CS35_SP_ENABLES 0x00004800u
#define CS35_SP_FORMAT 0x00004808u
#define CS35_SP_HIZ_CTRL 0x0000480Cu
#define CS35_SP_FRAME_RX_SLOT 0x00004820u
#define CS35_SP_RX_WL 0x00004840u
#define CS35_DAC_PCM1_SRC 0x00004C00u
#define CS35_ASP_TX3_SRC 0x00004C28u
#define CS35_ASP_TX4_SRC 0x00004C2Cu
#define CS35_AMP_DIG_VOL_CTRL 0x00006000u
#define CS35_AMP_GAIN_CTRL 0x00006C04u
/* The digital volume lives in bits 13:3 of AMP_DIG_VOL_CTRL.  The scale
 * runs against the register name: the value 0 is 0 dB, that is the loudest
 * setting, and the count climbs into the attenuation until 0x4CF, which is
 * the part's mute code (Linux's cs35l41.h: CS35L41_AMP_PCM_VOL_MUTE, with
 * the field itself shifted by CS35L41_AMP_PCM_VOL_SHIFT).
 *
 * The direction matters: the reference client writes 0 there and its
 * system makes sound, so 0 is not the quiet end.  This port writes the mute
 * code instead, so an amplifier that is powered up before the volume is
 * chosen comes up silent rather than at full scale with 8.5 dB of analog
 * gain on top. */
#define CS35_AMP_PCM_VOL_SHIFT 3u
#define CS35_AMP_PCM_VOL_MASK (0x7ffu << CS35_AMP_PCM_VOL_SHIFT)
#define CS35_AMP_PCM_VOL_MUTE 0x4cfu
#define CS35_IRQ1_STATUS1 0x00010010u
#define CS35_IRQ1_STATUS3 0x00010018u
#define CS35_IRQ1_STATUS4 0x0001001Cu
#define CS35_IRQ1_MASK1 0x00010110u
#define CS35_IRQ1_MASK2 0x00010114u
#define CS35_IRQ1_MASK3 0x00010118u
#define CS35_IRQ1_MASK4 0x0001011Cu
#define CS35_DSP1_CCM_CORE_CTRL 0x02BC1000u
#define CS35_DSP1_XM_ACCEL_PL0_PRI 0x02BC2020u
#define CS35_DSP1_YM_ACCEL_PL0_PRI 0x02BC20E0u

/* The chip id the part answers with. */
#define CS35_CHIP_ID 0x00035A40u
#define CS35_OTP_BOOT_DONE 0x00000002u   /* IRQ1_STATUS4 */
#define CS35_OTP_BOOT_ERR 0x80000000u    /* IRQ1_STATUS3 */
#define CS35_PUP_DONE 0x01000000u        /* IRQ1_STATUS1 */
#define CS35_PDN_DONE 0x00800000u        /* IRQ1_STATUS1 */
/* The status program reads IRQ1_STATUS1 (reads[0]) and IRQ1_STATUS3
 * (reads[1]).  The PLL lock is a bit of STATUS3, not of STATUS2. */
#define CS35_EINT2_PLL_LOCK 0x00000002u  /* IRQ1_STATUS3 bit 1 */
#define CS35_EINT1_ERRORS 0x800281C0u    /* AMP_SHORT, TEMP_ERR, TEMP_WARN, BST_SHORT, BST_DCM_UVP, BST_OVP */

/* The board's amplifiers (fuxi device tree): T at 0x42 is the left channel
 * (slot 0), B at 0x40 is the right one (slot 1).  The reset pins are
 * gpio182 (T) and gpio183 (B); both are eGPIOs. */
#define CS35_AMPS 2
/* "Analog PCM Volume" index (0.5 dB + 1 dB per step, at most 20):
 * 17 = 17.5 dB, near sheng's 18 dB. */
#define CS35_GAIN_INDEX 17u

/* The maximum number of steps any of these programs has. */
#define FUXI_CS35_MAX_STEPS 48

/* Each program returns its step count.  The init program reads four words
 * (DEVID, REVID, IRQ1_STATUS4, IRQ1_STATUS3), the status program two
 * (IRQ1_STATUS1, IRQ1_STATUS3). */
unsigned fuxi_cs35_init_program(Cs35Step *out, unsigned slot);
unsigned fuxi_cs35_status_program(Cs35Step *out);
unsigned fuxi_cs35_power_up_program(Cs35Step *out);
unsigned fuxi_cs35_power_down_program(Cs35Step *out);

/* The quiet program: the digital volume to the part's mute code and the
 * analog gain to zero.  It runs before the power-up program, so the
 * amplifier comes up silent.
 *
 * The reference client (Kerfand's UniXNU-Fuxi-Arm64e-iOS17) does not have
 * this program: its init program leaves the digital volume at 0 dB and the
 * analog gain at 17.5 dB and it powers the amplifier up in that state, so
 * its amplifier makes sound at full scale the moment it is enabled.  That
 * is not a property worth carrying into a bring-up: this port runs the
 * quiet program first and unmutes only after the amplifier's PLL has
 * locked. */
unsigned fuxi_cs35_mute_program(Cs35Step *out);

/* The sound program: the digital volume back to 0 dB and the analog gain
 * to the given index (the board's index is 17, that is 17.5 dB).
 *
 * The gain index is a parameter because the first test runs at index 0:
 * the amplifier is then at unity gain, which is quiet enough that a wrong
 * period buffer cannot be heard across the room.  The board's own index is
 * only used once the path has proved itself. */
unsigned fuxi_cs35_unmute_program(Cs35Step *out, unsigned gain_index);

/* The address of amplifier n (0 = T at 0x42, 1 = B at 0x40) and the slot it
 * plays.  The board's own tree gives the order. */
static inline uint8_t fuxi_cs35_address(unsigned amp) {return amp == 0 ? 0x42u : 0x40u;}
static inline unsigned fuxi_cs35_slot(unsigned amp) {return amp == 0 ? 0u : 1u;}

#ifdef __cplusplus
}
#endif
#endif
