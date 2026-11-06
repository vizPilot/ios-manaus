# 00 — ¿Se puede? Factibilidad de iOS 16 en manaus

## Resumen

Sí es posible *en teoría*, y no es un "no" por licencias ni por ARM: XNU corre
sobre ARMv8 genérico, y el repo base ya resolvió la parte difícil que **no**
depende del SoC (userland de iPhone X, dyld, AMFI permisivo, launchd,
SpringBoard, HFS+, iOSurface en software, identidad D22AP, activación nativa).

Pero `manaus` no es un caso "igual que sweet con otra config": es un SoC
**MediaTek** donde el repo base es **Qualcomm**. Eso obliga a reescribir toda la
capa de plataforma y drivers, y deja tres incógnitas que pueden matar el
proyecto antes de escribir una línea de XNU:

1. **¿Existe `fastboot boot` en el LK de Motorola/MTK?** Todo el método de
   ios-sweet (nada flasheado, arranque temporal) depende de eso. La guía de
   LineageOS para manaus instala recovery **flasheando `vendor_boot`**, no con
   `fastboot boot`, lo que es una mala señal.
2. **¿El bootloader deja un framebuffer vivo?** En sweet, ABL deja un FB lineal
   en `0x9c000000` con splash continuo. En MTK el splash lo maneja LK con
   `mtkfb` y DSI; si LK apaga el panel al ceder el control, no hay consola ni
   GUI hasta escribir un driver DSI completo.
3. **¿Se puede escribir el root de iOS en `userdata`?** Requiere driver UFS para
   `ufs-mediatek` (UFSHCI + PHY/clocks MTK) y HFS+ con escritura. En sweet eso
   fueron los parches 0028–0034 y 0069.

Ninguna de las tres se puede resolver desde aquí: hace falta el teléfono.

## Qué reutiliza el port (esto es mucho)

Del repo base, la mayoría no es específica de sweet:

| Bloque | Parches | Reutilizable en manaus |
| --- | --- | --- |
| Toolchain / compilación XNU no-PAC | 0001–0002, 0005, 0007–0009 | Sí, tal cual |
| Framebuffer consola temprano | 0003, 0010, 0012 | Adaptar (dirección/geometría) |
| PRNG, libpthread, BearSSL, SHA, MD5 | 0013–0015, 0019–0020, 0023, 0027, 0032–0033 | Sí, tal cual |
| Plataforma / device tree / timer / CPU boot | 0004, 0011, 0016–0018 | **Reescribir** (`SWEET.h`, `board_config.h`) |
| Raíz mockfs/ramdisk, HFS+, AMFI, keybag | 0021–0022, 0024–0026, 0029, 0031, 0035–0038 | Sí, tal cual |
| Storage UFS | 0028, 0030, 0034, 0069 | **Reescribir** |
| Watchdog, crash logs, sandbox, muestreo | 0039–0045, 0047–0055, 0066–0067 | Sí, tal cual |
| Display IOSurface / AppleCLCD | 0046, 0056, 0061, 0068, 0079 | Reutilizar el renderer; **nuevo** panel/handoff |
| HID (IOHIDFamily 1915) | 0058 | Sí, tal cual (es 1 MB de diff genérico) |
| Identidad iPhone X | 0059 | Sí, tal cual |
| Teclas y táctil | 0060, 0063, 0071 | **Reescribir** transporte (MTK I²C, PMIC MTK) |
| PMIC / reguladores / batería | 0062 | **Reescribir** (MTK PMIF, no SPMI/RPMh) |
| SMP PSCI | 0065, 0078 | Adaptar (MTK sí usa PSCI, cambian MPIDR y arranque) |
| USB | 0072–0077, 0080–0081 | **Reescribir** (MTK USB, no DWC3/QCOM) |
| Trabajo/prioridad UIKit | 0064 | Sí, tal cual |

Regla práctica: **~55–65 % de los parches se reutilizan**; el 35–45 % restante
es exactamente la capa que hay que escribir de cero para MediaTek.

## Los tres escenarios

| Escenario | Qué implica | Probabilidad estimada* |
| --- | --- | --- |
| XNU arranca y escribe texto en pantalla | Shim + boot image v4 + GIC/timer + FB o UART | 50 % |
| Llega a launchd con el root de iOS | Lo anterior + driver UFS + HFS+ | 30 % |
| Llega a SpringBoard usable | Lo anterior + panel DSI + táctil + PMIC | 15 % |

\* Estimación subjetiva, no un dato. Depende sobre todo del punto 1 y 2 de
arriba, que se resuelven en días de M0, no en meses.

## Esfuerzo

- **M0 (medir):** 1–3 días. Decide si el proyecto sigue.
- **Si las tres incógnitas salen bien:** semanas de iteración intensiva
  (compilar en CI, `fastboot boot`, leer logs, repetir). El repo base se hizo en
  ese orden de magnitud con el teléfono en la mano.
- **Si hay que escribir driver DSI/UFS MTK desde cero:** meses, y sin garantía.
- **Lo que nunca va a funcionar:** GPU/Metal, módem, Wi-Fi/BT, cámara, audio,
  SEP/Face ID. En sweet tampoco funcionan.

## Alternativas honestas

- **Comprar un Redmi Note 10 Pro (`sweet`) usado** (~100 €): el port ya existe y
  arranca a SpringBoard. Es la forma más barata de ver esto funcionando.
- **Rom Android con estética iOS** (LineageOS manaus + launcher/tema): no es
  iOS, pero es 1000× menos trabajo y sí sirve de teléfono.
- **Esperar a un port MTK de otro dispositivo**: a día de hoy no hay ninguno
  público que arranque XNU en MediaTek; el propio repo base es Qualcomm.

## Legal

- XNU, IOHIDFamily, libpthread, HFS: open source de Apple, con sus licencias.
- El IPSW / root filesystem de iPhone X: propietario de Apple, **no
  redistribuible**. Cada persona debe extraerlo de su propio dispositivo/IPSW.
- El port debe mantener fuera de git: dumps del teléfono, seriales, firmware de
  Apple, imágenes de root. (El repo base ya lo hace: `.gitignore`.)
