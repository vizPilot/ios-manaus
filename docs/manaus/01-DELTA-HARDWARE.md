# 01 — Delta de hardware: sweet (SM7150) vs manaus (MT6879)

Fuentes: `../ios-sweet/tools/mkadt.py`, `../ios-sweet/NOTES.md`,
`../ios-sweet/shim/*`, y para manaus: LineageOS wiki (manaus),
`android_device_motorola_mt6879-common/BoardConfigCommon.mk`,
`android_device_motorola_manaus/BoardConfig.mk`.
Los valores de manaus marcados **TBD** son los que hay que leer del FDT real
(ver `tools/manaus-intake.sh`).

## Tabla

| Subsistema | sweet (Qualcomm SM7150) | manaus (MediaTek MT6879) | Impacto |
| --- | --- | --- | --- |
| CPU | 6×A55 + 2×A76, ARMv8.2, sin PAC | 6×A55 (part `0xd05`, MPIDR 0x000–0x500, 2.0 GHz) + 2×A76 (part `0xd41`, MPIDR 0x600/0x700, 2.5 GHz), ARMv8.2, sin PAC | Bajo: mismo XNU no-PAC |
| Página MMU | 16 KB verificado | A76/A55 soportan 16 KB (confirmar TRM) | Bajo |
| SMP | PSCI por `smc`; MPIDR Aff1 por núcleo | PSCI también; MPIDR/arranque MTK distintos | Medio: adaptar `SweetCPU.cpp`/`start.s` |
| Timer | 19.2 MHz, virtual timer INTID 19, Grupo 1 | ARM generic timer, 13 MHz, virtual timer INTID 27 (PPI 11), Grupo 1 | Bajo/medio |
| GIC | GICD `0x17a00000`, GICR `0x17a60000`, GICv3+security | GICD `0x0c000000`+`0x40000`, GICR `0x0c040000`+`0x200000`, GICv3+security; stride por `GICR_TYPER.VLPIS` en runtime | Bajo: solo direcciones |
| RAM | bancos `<0x80000000 2G>` + `<0x1_00000000 0xfcc00000>`; usa banco alto | LPDDR5X 8 GB; DRAM `0x40000000`–`0x23fffffff`; ventana limpia elegida `0x140000000`+`0xa40f8000` (2.56 GiB) | Medio: elegir banco sobre carve-outs |
| Cadena de arranque | ABL (Xiaomi) → `fastboot boot shim-boot.img` | Preloader → LK (Motorola) → boot/vendor_boot | **Alto / go-no-go** |
| Imagen de arranque | Android boot v2 + DTB, `fastboot boot` | **header v4**, `BOARD_KERNEL_PAGESIZE=4096`, base `0x3fff8000`, `dtb_offset=0x07c88000`, ramdisk `0x26f08000`, `bootopt=64S3,32N2,64N2`, GKI + `vendor_boot` (A/B) | **Alto** |
| Recovery | OrangeFox temporal por `fastboot boot` | Lineage Recovery **flasheada** en `vendor_boot` | Alto (cambia el flujo de logs) |
| Storage | UFS propio en `0x01d84000` (`qcom,ufshc`), root HFS+ en `userdata` | UFS `ufshci@112b0000` (`mediatek,mt8183-ufshci`, IRQ 137), main GPT `sdc`, `userdata` = `sdc90` (229.7 GiB) | **Alto**: driver nuevo |
| Display | FB lineal de ABL en `0x9c000000`, 1080×2400, splash continuo, panel DSC | pOLED 1080×2400 144 Hz, DSI MTK (`mtk_drm`/`mtkfb`), DSC; handoff de LK TBD | **Alto / go-no-go** |
| Táctil | Goodix GT9896 por I²C GENI `0xa84000`, raíl por RPMh LDO11 | Táctil Goodix (huella `goodix` visible en el device tree), bus I²C MTK (`i2c-mt65xx`), raíl por PMIC MTK; modelo TBD | Medio: protocolo quizá reutilizable |
| Teclas | PON/GPIO PM6150 vía SPMI | PMIC MTK (MT6359/MT6369 TBD) vía PMIF | Medio: driver nuevo |
| Batería | `IOPMPowerSource` estático + lectura de fuel gauge Qualcomm | fuel gauge/charger MTK TBD | Medio |
| USB | DWC3 Qualcomm; parches 0072–0081 | controlador MTK (mtu3/musb) distinto | Alto, pero no bloquea |
| GPU | Adreno 618, sin driver → render CPU | Mali-G610 MC3, sin driver → render CPU | Ninguno (misma limitación) |
| Módem / Wi-Fi / BT / cámara / SEP | no funcionan | no funcionarán | Ninguno |

## Lo que cambia conceptualmente

1. **No hay ABL.** El repo base asume un bootloader Qualcomm que deja el panel
   encendido, un FB lineal y un FDT vivo en una dirección conocida. En MTK el
   que manda es LK, el device tree viene en `boot.img`/`dtbo`, y el estado del
   panel al ceder el control no está documentado.
2. **El "kernel" que carga LK no es un ELF arbitrario.** En Android/GKI es
   `Image.gz` + DTB dentro de un header v4. El shim tendrá que empaquetarse como
   ese "kernel" (o como `vendor_boot`), no como el `shim-boot.img` v2 de sweet.
3. **Los reguladores no son RPMh.** `sweet_rpmh.c` y `sweet_spmi.c` no tienen
   equivalente directo: MTK usa PMIF + `mtk-regulator`, con votos por
   `spmi`/`pmic-wrapper`.
4. **El almacenamiento es el mismo protocolo, otro host.** UFS es estándar
   (UFSHCI/UIC); el PHY, clocks y power sí son MTK. Es el trabajo más mecánico
   de los tres grandes.

## Checklist de datos a medir (M0)

Rellenar en `hw/manaus.inventory.json`:

- [x] `fastboot getvar all` (`max-download-size 268435456`; `fastboot boot` y `fastboot fetch` **no** implementados)
- [x] `fastboot boot` con un `boot.img` stock: rechazado por el LK
- [x] `/proc/device-tree` completo (`hw/manaus.hw.json`, `manaus-intake-2/device-tree/`)
- [x] `memory@*` del FDT: bancos y `reserved-memory` (carve-outs)
- [x] `interrupt-controller@*`: GICD/GICR (`#redistributor-regions=1`)
- [x] `timer`: `clock-frequency` 13 MHz, PPI 11 ⇒ INTID 27
- [x] `ufshc@*` / `ufs_mem`: base `0x112b0000`, clocks/resets/supplies MTK
- [x] `dsi@*`, `panel`, `mtkfb`: resolución, DSC, dirección del splash
- [x] `i2c@*` + nodo táctil (`compatible`, dirección, IRQ, reset, raíl)
- [x] PMIC: modelo, `pwrap`, teclas (PON), charger, gauge
- [x] `chosen/bootargs` y `bootopt` reales
- [x] Estado AVB: `ro.boot.verifiedbootstate`, `ro.boot.flash.locked`
- [ ] Puntos de test UART en la placa (¿hay? ¿a 921600?)
- [ ] ¿Se puede escribir en `userdata` desde recovery y montar HFS+?
