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

## Estado actual

Vacío a propósito: no se puede escribir `0004` sin los valores del FDT real
(`hw/manaus.hw.json`). El primer parche que se añada aquí será
`0004-sweet-machine-config.patch`, generado a partir de `hw/manaus.hw.json` y de
`tools/mkadt-manaus.py`.
