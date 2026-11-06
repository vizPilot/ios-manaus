# 03 — Plan de port (M0–M6)

Regla del proyecto: **no se escribe código de un subsistema hasta tener su dato
de hardware**. En `sweet` cada dirección salió del FDT vivo; adivinar en MTK es
perder semanas.

## M0 — Medir el teléfono (1–3 días) · go/no-go

Objetivo: saber si el proyecto es viable **antes** de tocar XNU.

Tareas:

1. Backup completo (recovery temporal o `fastboot boot` si existe):
   `persist`, `modemst1`, `modemst2`, `fsg`, `fsc`, `boot`, `dtbo`, `vbmeta`,
   `super` si se puede.
2. `tools/manaus-intake.sh` → rellenar `hw/manaus.inventory.json`.
3. Probar el camino de arranque:
   - `fastboot boot boot_stock.img` (si el LK lo permite);
   - si no, plan B: flashear un `boot.img` propio (con backup y firmware stock a
     mano). Confirmar si hace falta `fastbootd`.
4. Comprobar el handoff de pantalla: bootear recovery y ver si el panel queda
   encendido y con FB accesible (o si hay `mtkfb`/`simple-framebuffer` en el
   FDT). Si LK apaga el panel, M1 cambia de alcance.
5. Buscar UART: esquemas/teardown de manaus, pads de test, `dmesg` con
   `console=`/`earlycon`. Si hay UART, el proyecto gana un canal de debug enorme.

**Criterio de éxito:** `fastboot boot` (o flash de boot propio) + consola en FB
o UART + base de UFS conocida.
**Criterio de abandono:** sin forma de arrancar una imagen propia y sin consola
de ningún tipo → no se puede iterar; parar y replantear (usar un `sweet`).

## M1 — Shim + XNU escribe texto (1–2 semanas)

1. `xnu/patches-manaus/`: 0004 reescrito (`MANAUS.h`, `board_config.h`,
   `start.s`) con GIC/timer/RAM/MPIDR del FDT.
2. Validar la config en QEMU (`tools/qemu-sweet.py` adaptado): mismo kernel,
   GIC/timer de QEMU. Detecta errores de plataforma sin tocar el teléfono.
3. Empaquetar el shim como **Android boot header v4** (`mkbootimg
   --header_version 4 --dtb mt6879.dtb`, base `0x3fff8000`, páginas 4096) y
   arrancarlo. Alternativa: `vendor_boot`.
4. Consola temprana en el FB de LK (0003/0010/0012 adaptados) con geometría
   1080×2400 y stride real.
5. SMP: arrancar 1 núcleo, luego 6 A55, luego los 2 A78 (0065/0078).

**Salida:** pantalla con texto del kernel y versión de XNU.

## M2 — Log persistente y estabilidad

- Reservar el `pram`/panic log (ya está en `mkadt.py`).
- Si MTK tiene `ramoops` en `reserved-memory`, escribir ahí y leer desde
  recovery (`/sys/fs/pstore`): resuelve "¿por qué no arrancó?" sin cámara.
- Watchdog MTK: desactivarlo o alimentarlo (0040/0045 ya traen la lógica).

**Salida:** un panic se puede diagnosticar después de reiniciar.

## M3 — Storage y root de iOS

1. `manaus_ufs.c`: UFSHCI + PHY/clocks/reguladores MTK. Empezar en lectura,
   luego escritura (0028/0030/0034).
2. Construir el root HFS+ del IPSW de iPhone X (`tools/mkiosroot.sh`) y
   flashearlo en `userdata` (o en un volumen dentro de `userdata`).
3. Arrancar a `launchd` con el ramdisk primero y luego con el root real
   (0021–0026, 0035–0038, 0047–0055).
4. Log persistente en el anillo de `userdata` (0047).

**Salida:** consola con daemons de iOS arrancando; panic reports simbólicos.

## M4 — Pantalla y SpringBoard

1. Backend de panel MTK para `manaus_framebuffer` (0046/0056/0061): DSI, DSC,
   timing, brillo. Si el FB de LK sobrevive, empezar por copiar ahí (como sweet).
2. `IOSurface` + `AppleCLCD` + `IOMobileFramebuffer` presentados como panel
   iPhone X (1125×2436).
3. `backboardd` y `SpringBoard`, con paciencia: 4–5 min de boot en sweet.

**Salida:** home screen de iOS en el Edge 40 Neo.

## M5 — Entrada y energía

- Táctil Goodix por I²C MTK (0060/0063/0071) + raíl por PMIC MTK (0062).
- Teclas PON/volumen como HID (0060).
- Batería estática primero (100 %), gauge real después (0062).

**Salida:** desbloquear, abrir Ajustes, usar el centro de control.

## M6 — USB (opcional)

0072–0081 sobre el controlador USB MTK: logs en vivo y red ECM. No bloquea
nada de lo anterior.

## Lo que no se intenta

GPU/Metal, módem/IMS, Wi-Fi/BT (combo MTK + `wmt`), cámara, audio, sensores,
GPS, SEP/Face ID. En sweet tampoco existen; aquí es el mismo "no" por diseño.

## Orden de trabajo recomendado (y por qué)

```
M0 (medir) → M1 (texto en pantalla) → M2 (logs) → M3 (launchd) → M4 (GUI) → M5 (input)
```

No adelantar M4 antes de M3: sin storage no hay userland, y sin logs no se puede
depurar M4.

## Presupuesto de tiempo (estimación)

| Fase | Si los supuestos salen bien | Si hay que escribir desde cero |
| --- | --- | --- |
| M0 | 1–3 días | — |
| M1 | 1–2 semanas | 1–2 meses (si no hay FB handoff) |
| M2 | 3–5 días | — |
| M3 | 2–4 semanas | 2–3 meses (driver UFS MTK) |
| M4 | 2–5 semanas | 3–6 meses (driver DSI/panel) |
| M5 | 1–2 semanas | 2–4 semanas |

## Seguridad y reversibilidad

- Nunca tocar `preloader`, `lk`, `bootloader`, `modem`, `persist`.
- Mantener siempre una imagen `boot.img` stock y el firmware completo de
  Motorola para restaurar.
- Probar primero con `fastboot boot`; flashear solo cuando el arranque temporal
  funcione de forma repetible.
- El root de iOS va en `userdata`: eso borra Android. Confirmar antes.
