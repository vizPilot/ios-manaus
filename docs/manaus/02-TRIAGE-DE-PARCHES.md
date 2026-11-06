# 02 — Triage de los 81 parches de ios-sweet

El repo base aplica `xnu/patches/0001..0081` sobre **xnu-8796** vía
`.github/workflows/xnu.yml`. Para manaus se propone una serie paralela
`xnu/patches-manaus/` (mismos números cuando el parche se reutiliza) para poder
seguir sincronizando con upstream.

## Verdicto por bloque

| Parches | Contenido | Verdicto | Nota |
| --- | --- | --- | --- |
| 0001, 0002, 0005 | Compilación con clang moderno, `UIO_STACKBUF`, no-PAC | **Reusar** | Nada de hardware |
| 0003 | Consola framebuffer temprana | **Adaptar** | Cambia base/geometría/stride del panel |
| 0004 | Machine config `SWEET.h`, `board_config.h`, `start.s` | **Reescribir** | Es el corazón del port MTK |
| 0006, 0009 | Config de build, símbolos Apple-only | **Reusar** | Renombrar target `sweet` → `manaus` |
| 0007, 0008 | Stubs de headers/carpetas cerradas | **Reusar** | |
| 0010, 0012 | Debug de FB temprano, consola tras TTBR0 | **Adaptar** | Depende de 0003 |
| 0011 | Alineación/padding del physmap | **Revisar** | Depende del mapa de RAM MTK |
| 0013–0015 | PRNG sin corecrypto, HMAC, trust cache | **Reusar** | |
| 0016, 0017 | Platform expert por device tree, IRQ del virtual timer | **Reusar/adaptar** | INTID del timer TBD |
| 0018 | Driver de CPU de arranque | **Adaptar** | Arranque MTK |
| 0019, 0020, 0023, 0027, 0032, 0033 | libpthread, BearSSL (AES/MD5/SHA1/SHA-384/512) | **Reusar** | |
| 0021, 0022, 0024 | Root ramdisk/mockfs sin IOMedia | **Reusar** | Sirve para el primer arranque iOS |
| 0025, 0026 | AMFI sin entitlements, HFS+ open source | **Reusar** | |
| 0028, 0030, 0034, 0069 | Driver UFS, escrituras, DMA >4 GiB, flush | **Reescribir** | UFSHCI se parece; PHY/clocks/reguladores MTK no |
| 0029, 0031 | Volumen root sin sellar, root RW | **Reusar** | |
| 0035–0038 | AMFI/entitlements, code signing, bindfs, AppleKeyStore stub | **Reusar** | |
| 0039–0045 | Coredump off, IOWatchdog, crash log | **Reusar** | |
| 0046 | IOSurface + `IOMobileFramebuffer`/`AppleCLCD` + `SweetPlatformExpert` | **Adaptar** | El renderer software sirve; el "panel" es nuevo |
| 0047–0055 | Log persistente, tee /dev/null, trazas, sandbox, muestreo | **Reusar** | El log persistente depende del UFS |
| 0056 | Present throttled | **Reusar** | |
| 0057 | Sin resolver de identidad externo | **Reusar** | |
| 0058 | HID event services (IOHIDFamily 1915, ~1 MB) | **Reusar** | Independiente del SoC |
| 0059 | Identidad iPhone X (D22AP / iPhone10,3) | **Reusar** | Solo cambia el `compatible` final |
| 0060, 0063, 0071 | Teclas/táctil, packing GENI, borde táctil | **Reescribir** | MTK no tiene GENI; el protocolo Goodix puede sobrevivir |
| 0061 | Tamaño de panel iPhone X (1125×2436) | **Reusar** | Es lógico, no físico |
| 0062 | PMIC (RPMh/SPMI), batería, raíl del táctil | **Reescribir** | PMIF + `mtk-regulator` |
| 0064 | `PRIV_WORK_INTERVAL` para UIKit | **Reusar** | |
| 0065, 0078 | SMP PSCI, 8 núcleos | **Adaptar** | PSCI existe en MTK; MPIDR y orden de clusters cambian |
| 0066, 0067 | Crash con fault region, mute de consola | **Reusar** | |
| 0068 | Lookup de IOSurface ya mapeada | **Reusar** | |
| 0070 | PID hibernate | **Reusar** | |
| 0072–0077 | USB: probe, EP0, VBUS, ECM, RX, log en vivo | **Reescribir** | Controlador USB MTK distinto |
| 0079 | Encendido/apagado de display | **Adaptar** | Depende del panel MTK |
| 0080, 0081 | Trazas de red, link state | **Reusar/reescribir** | Sobre el USB nuevo |

## Archivos que hay que reescribir (lista corta y concreta)

Del árbol XNU, tras aplicar los parches base:

| Archivo | Origen | Qué hacer |
| --- | --- | --- |
| `pexpert/pexpert/arm64/SWEET.h` | 0004 | Mismo archivo, valores MTK: GIC, timer, UART, RAM, FB, MPIDR |
| `pexpert/pexpert/arm64/board_config.h` | 0004 | Constantes MTK |
| `osfmk/arm64/start.s` | 0004, 0065 | Arranque de CPU MTK (EL2→EL1, PSCI) |
| `iokit/Kernel/arm/SweetCPU.cpp` | 0065 | PSCI con MPIDR de MT6879 |
| `bsd/dev/sweet_ufs.c` | 0028, 0030, 0034, 0069 | Mismo archivo, driver `ufs-mediatek` |
| `bsd/dev/sweet_touch.c` | 0060, 0063, 0071 | Mismo archivo, I²C MTK + Goodix |
| `bsd/dev/sweet_keys.c` | 0060 | Mismo archivo, teclas PON MTK |
| `bsd/dev/sweet_spmi.c`, `sweet_rpmh.c` | 0062 | Sustituir cuerpo por PMIF/pwrap |
| `iokit/Kernel/arm/SweetFramebuffer.cpp` | 0046 | Nuevo backend de panel MTK |
| `iokit/Kernel/arm/SweetPlatformExpert.cpp` | 0046 | Añadir match `motorola,manaus` |
| `bsd/dev/sweet_usb.c` | 0072–0077 | Mismo archivo, USB MTK |
| `shim/ufs.c`, `shim/shim.c` | repo | Direcciones, PSCI, consola, UART MTK |
| `tools/mkadt.py` | repo | Sustituido por `tools/mkadt-manaus.py` |
| `tools/mkpayload.py` | repo | Boot args MTK, sin `rd=ufsroot0` al inicio |

## Convención: no renombrar

El 95 % de los parches menciona `sweet` (funciones, nodos, comentarios). Renombrar
a `manaus_*` obligaría a regenerar la serie entera y a divergir de upstream para
siempre. **`sweet` aquí es el nombre de la capa de plataforma, no del teléfono**:

- se mantienen `sweet_ufs.c`, `sweet_touch.c`, `SweetFramebuffer.cpp`, los nodos
  `sweet-fbcon`/`sweet-ufs`/`sweet-io` y `MACHINE_CONFIG=SWEET`;
- solo cambian los **valores** (direcciones, IRQs, geometría) y el cuerpo de los
  drivers que se reescriben.

La única excepción es el `compatible` raíz del ADT: empieza por `D22AP` e
`iPhone10,3` (identidad iPhone X) y puede llevar `motorola,manaus` y
`mediatek,mt6879` detrás. `xiaomi,sweet` se queda mientras `SweetPlatformExpert`
lo busque (ver `0046`).

## Cómo montar el fork

No se copia la serie: se usa **override por nombre de archivo**.

```
xnu/patches/          # upstream, intacta (81 parches)
xnu/patches-manaus/   # solo los que difieren, mismo nombre
```

`.github/workflows/xnu-manaus.yml` copia la base y luego sobrescribe:

```bash
cp xnu/patches/*.patch xnu/patches/*.sh dxb/patches/
cp xnu/patches-manaus/*.patch dxb/patches/   # gana manaus
```

Ventajas: `git merge` con upstream limpio en ~70 parches, y el número de parche
significa lo mismo en los dos dispositivos. Detalle y lista de overrides:
`xnu/patches-manaus/README.md`.

```bash
git clone <tu-fork> ios-manaus && cd ios-manaus
git checkout -b manaus
# primer override: 0004, en cuanto hw/manaus.hw.json esté completo
```

