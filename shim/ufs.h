#pragma once
/*
 * Bring the internal UFS link up and make the device usable (device RST_n,
 * host reset + HCE, DME_LINK_STARTUP, UTRL takeover, NOP OUT + fDeviceInit).
 * Read-only: no SCSI command is issued. Returns 1 on success. The payload
 * path calls this before the jump when the cmdline asks for rd=ufsroot0, so
 * XNU's sweet_ufs.c finds the link already up.
 */
int ufs_bringup(void);
/* Milestone 5 probe: the same bring-up, then TUR/CAP/READ and a GPT dump. */
void test_ufs(void);
/* Negotiated UniPro link parameters (for the panel diagnostic). */
void ufs_link_info(uint32_t *tx, uint32_t *rx, uint32_t *mode, uint32_t *lanes);
/*
 * Raise the UniPro link from the PWM-G1/SLOWAUTO state DME_LINK_STARTUP leaves
 * to HS FAST_MODE (the step Linux does in ufshcd_probe_hba but the shim used to
 * skip). Call only with the link already up. Returns 0 when the link is HS.
 */
int ufs_raise_gear(void);
/*
 * Diagnostic: read the persistent console log (xnu patch 0047) the previous
 * boot left in the last blocks of `userdata` and print its header and tail to
 * the panel. Read-only. Call after ufs_bringup().
 */
void sweetlog_dump(void);
