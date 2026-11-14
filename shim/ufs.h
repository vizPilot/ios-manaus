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
