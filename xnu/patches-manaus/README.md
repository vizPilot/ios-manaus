# xnu/patches-manaus — overrides del port a manaus

La serie base `xnu/patches/` (81 parches) se mantiene **idéntica** a la de
ios-sweet/sweet. Este directorio contiene solo los parches que cambian para
MediaTek, con **el mismo nombre de archivo**. El workflow `xnu-manaus.yml` copia
primero la serie base y luego sobrescribe con lo que haya aquí:

```
cp xnu/patches/*.patch xnu/patches/*.sh dxb/patches/
cp xnu/patches-manaus/*.patch dxb/patches/      # gana el de manaus
```

Consecuencias:

- Se puede hacer `git merge`/rebase con upstream sin conflictos en 70 parches.
- El número de parche sigue significando lo mismo en ambos dispositivos.
- Nunca hay dos archivos con el mismo número en el build: el override sustituye.

## No renombrar `sweet_*`

77 de los 81 parches mencionan `sweet` (nombres de funciones, nodos, comentarios).
Renombrar a `manaus_*` obligaría a regenerar toda la serie y a mantener un fork
 divergente para siempre. **El nombre `sweet` aquí es solo el nombre de la capa de
plataforma**, no del dispositivo. Se mantienen:

- `sweet_ufs.c`, `sweet_touch.c`, `SweetFramebuffer.cpp`, `SweetPlatformExpert.cpp`
- nodos `sweet-fbcon`, `sweet-ufs`, `arm-io` con `sweet-io`
- `compatible` raíz con `xiaomi,sweet` (lo busca `SweetPlatformExpert`)
- `MACHINE_CONFIG=SWEET` (el directorio `pexpert/pexpert/arm64/`)

Solo cambian **los valores** (direcciones, IRQs, geometría) y, cuando toque, el
cuerpo de un driver. Si algún día se reescribe `SweetPlatformExpert`, se puede
quitar `xiaomi,sweet` del ADT; hasta entonces, dejarlo.

## Qué hay que sobrescribir (M1 → M6)

| Parche | Estado | Qué cambia |
| --- | --- | --- |
| `0004-sweet-machine-config.patch` | **M1** | `SWEET.h`, `board_config.h`, `start.s`: GIC/timer/UART/RAM/MPIDR de MT6879 |
| `0003-sweet-framebuffer-serial-console.patch` | M1 | base/geometría/stride del FB de manaus |
| `0010`, `0012` | M1 | dependen de 0003 (consola temprana) |
| `0011` | M1 | physmap según los bancos de RAM MTK |
| `0018` | M1 | driver de CPU de arranque (arranque MTK) |
| `0065`, `0078` | M1 | PSCI con MPIDR/clusters de MT6879 |
| `0028`, `0030`, `0034`, `0069` | M3 | `ufs-mediatek` en lugar de `qcom,ufshc` |
| `0046`, `0056`, `0061`, `0079` | M4 | backend de panel MTK (el renderer software no cambia) |
| `0060`, `0063`, `0071` | M5 | I²C MTK + Goodix; teclas PON MTK |
| `0062` | M5 | PMIF/pwrap/reguladores MTK + batería |
| `0072`–`0077` | M6 | USB MTK |

El resto se aplica tal cual.

## Dependencias que hay que respetar

Estos parches **parchean `bsd/dev/sweet_ufs.c`**, así que no compilan sin la
familia UFS reescrita:

```
0047 (log persistente)  -> 0028
0060, 0062              -> 0028
0046 (display)          -> ninguno, pero 0079 depende de 0046
0063, 0071              -> 0060
```

Por eso M3 (storage) va antes de M4 (display) y M5 (input): el log persistente
del 0047 es la herramienta de debug para todo lo demás.

## Cómo crear un override

1. Consigue un árbol de xnu-8796 compilable (el artifact
   `xnu-manaus-buildtree` del workflow ya trae fuente + headers generados).
2. Aplica la serie base, edita el archivo, y regenera **solo ese parche**:

```bash
# en el árbol con la serie aplicada
git add -A
git diff <commit-base> -- pexpert/pexpert/arm64/SWEET.h \
    pexpert/pexpert/arm64/board_config.h osfmk/arm64/start.s \
    > xnu/patches-manaus/0004-sweet-machine-config.patch
```

3. Comprueba que aplica limpio sobre la serie base:

```bash
cd xnu-src && git checkout -- . && for p in ../patches/*.patch; do git apply "$p"; done
git apply --check ../patches-manaus/0004-sweet-machine-config.patch
```

## Estado actual (2026-11-08)

Hay cuatro overrides, todos copias completas del parche base con los cambios
indicados (nunca se aplican encima del base):

| Override | Cambios |
| --- | --- |
| `0003-sweet-framebuffer-serial-console.patch` | `fbcon_probe()` prefiere `PE_current_console()`/`boot_args.Video` y solo cae al nodo ADT `sweet-fbcon` si `Video` viene vacío. LK elige el FB de splash en cada arranque. |
| `0004-sweet-machine-config.patch` | `SWEET_VTIMER_INTID 19 -> 27`; stride de GICR por `GICR_TYPER.VLPIS` en runtime (`0x40000` GICv4 / `0x20000` GICv3) en `pe_fiq/find_gicr_pe_base()`, con `GICR_PE_SIZE_V3/V4` y `GICR_TYPER_VLPIS` en `SWEET.h`; comentarios MT6879. |
| `0010-sweet-early-framebuffer-debug.patch` | `start.s` lee `BA_VIDEO_BASE/ROWBYTES/HEIGHT`; el mapeo V=P de 32 MiB usa el bloque que contiene el FB vivo (ya no `0x9c000000`); `genassym.c` publica esos offsets; `sweet_early_init(args)` en `arm_init()`. |
| `0012-sweet-keep-debug-console-after-ttbr0.patch` | `sweet_early_map_final()` mapea el bloque de 32 MiB del FB vivo y `sweet_early_relocate()` mueve la consola al mismo offset dentro del alias VA del kernel; marcas 10–20; `arm_vm_init`/`arm_init`/`pe_serial`/`Makefile`. |

Hecho de 16K: `ARM_TT_L2_SIZE = 0x2000000` (32 MiB). Tanto el banco de RAM
(`0x140000000`) como el FB (`0xfe0d0000`) están por debajo de 64 GiB, así que
comparten el L1 índice 0 del mapa V=P y el primer L2 sigue siendo
`bootstrap_pagetables + 2*PGBYTES`; el FB ocupa el slot 127 y la RAM desde el
160, sin solaparse. Un solo bloque de 32 MiB cubre los `0x1df0000` bytes del FB.

Verificado sobre `xnu-8796.141.3`: `0001`–`0003`, override `0004`, `0005`–`0009`,
override `0010`, `0011`, override `0012`, y después `0013`–`0081` aplican limpios
(solo `0031`/`0041` necesitan `0026-stage-hfs.sh`). Los parches posteriores que
tocan archivos del override (`0017` en `protos.h`, `0065` en `start.s`) no
conflictúan.

El stride de GICR **ya no** es una constante de compilación: se lee
`GICR_TYPER.VLPIS` (bit 1) del primer redistribuidor en `pe_fiq` y en el shim,
y se elige `0x40000` (GICv4) o `0x20000` (GICv3). El tamaño del `reg` del DT
(`0x200000`) es sobre-provisión y no distingue las dos variantes.

Pendiente antes de SMP: confirmar el `CurrentEL` de entrada. Nada de esto cambia
la serie base.
