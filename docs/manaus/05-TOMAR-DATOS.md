# 05 — Tomar los datos del teléfono (M0)

Esta fase la ejecuta el usuario en el teléfono. Las secciones 0, 2 y 3 son
**solo lectura**: no escriben particiones ni cambian ajustes. Su objetivo es
rellenar `hw/manaus.hw.json`, que hoy tiene 20 valores a `null`. La sección 1 es
la excepción: explica el único paso que escribe (`boot_b`), y solo cuando exista
un respaldo verificado.

Si algo de esto falla, se para y se revisa: no hay prisa y no hay nada que
"arreglar" con un flash.

---

## 0. Antes de tocar nada: guardar la configuración actual

Con el teléfono en el bootloader (unlocked), desde el PC:

```bash
fastboot getvar all       2> getvar-all.txt
fastboot devices
```

`getvar all` no escribe nada y contesta a dos preguntas de las tres que
bloquean el port:

- `unlocked: yes` (confirma que el bootloader sigue abierto).
- `has-slot:*`, `slot-count`, `partition-size:*`: geometría A/B real.
- `max-download-size`, `variant`, `is-userspace`: lo que acepta el LK.

Guarda `getvar-all.txt` (contiene el serial: **no lo subas a git**).

## 1. Go/no-go #1: `fastboot boot` — **RESUELTO: no existe**

Probado el 2026-11-05: este LK **no implementa** `fastboot boot`. No es un
fallo de permisos ni del cable: el comando no está en la lista del bootloader.
Los datos completos (saneados) están en `hw/manaus.getvar.json`.

Consecuencias, en orden:

- **El kernel se arranca flasheando `boot`.** No hay forma de probar un kernel
  sin escribir una partición. Eso hace que el respaldo de `boot` sea
  obligatorio antes de M1, no opcional.
- **La rollback por slot no sirve.** `getvar` dice `slot-successful:_a: no` y
  `slot-retry-count:_a: 0`: si `boot_b` no arranca, el LK no tiene un slot `a`
  sano al que caer. El único rollback fiable es tener el `boot_b` original.
- `max-download-size` = 256 MiB: cualquier `boot.img` que empaquetemos tiene
  que caber en una sola descarga (el nuestro son decenas de MiB, sin problema).

### Antes de nada: intento sin root (`fastboot fetch`, 10 segundos)

El protocolo fastboot tiene `fetch` para leer una partición. Muchos LK de
fábrica no lo implementan, pero si este lo hace te ahorra el root:

```bash
fastboot fetch boot_b        boot_b.img
fastboot fetch vendor_boot_b vendor_boot_b.img
```

- Si sale `FAILED (remote: 'unknown command')` → no está, se pasa a la tabla.
- Si funciona: `python3 tools/bootimg.py info boot_b.img` para confirmarlo y ya
  tienes respaldo exacto sin tocar el teléfono.

### Si no, elige una vía

| Vía | Requisito | Comentario |
| --- | --- | --- |
| `dd` con root en Android | Magisk / KernelSU | `dd if=/dev/block/by-name/boot_b of=/sdcard/boot_b.img` |
| `adb` en recovery | recovery con adb root | `adb shell dd if=/dev/block/by-name/boot_b of=/tmp/boot_b.img` y `adb pull` |
| Firmware stock completo | descarga para XT2307-1 | Se extrae `boot.img` y sirve tanto de respaldo como de vuelta atrás |

Comprobación del respaldo **antes** de usarlo:

```bash
python3 ios-sweet/tools/bootimg.py info boot_b.img
python3 ios-sweet/tools/bootimg.py info vendor_boot_b.img
```

Tiene que salir `header_version 4`, `kernel_size` > 0 y (en `vendor_boot_b`)
`dtb_size` > 0. Ese DTB es el que usa M1: no hay que buscarlo en otro sitio.

Si no tienes ninguna de las tres vías, dímelo: la descarga del firmware
`XT2307-1` es la más segura que no depende de root.

### Firmware oficial (sin root)

El canal de tu unidad es `opencl` (`ro.carrier: opencl`), y los paquetes están
en el mirror de lolinet:

<https://mirrors.lolinet.com/firmware/lenomola/2023/manaus/official/OPENCL/>

Tu build instalada es `V1TMS35H.3-45-1`; la más parecida publicada es
`XT2307-1_MANAUS_OPENPUB_15_V1TMS35H.3-45-3-2-7_...zip` (5,2 GB). Sirve para
sacar `boot.img` y `vendor_boot.img`, pero **no es la misma build**:

- como red, restaurar un `boot` de otra build puede dejar Android en bootloop
  por desajuste con los módulos de `vendor_dlkm`;
- por eso el `dd` del teléfono (arriba) es siempre preferible, y el firmware es
  el plan B.

Si acabas usando el firmware: antes de flashear nada, compara el kernel que
lleva (`tools/bootimg.py info`) con el que reporta el teléfono
(`kernel.version` en `hw/manaus.getvar.json`).

### Sacar las imágenes stock con root o recovery

```bash
# Android con root (Magisk/KernelSU)
adb shell su -c 'dd if=/dev/block/by-name/boot_b of=/sdcard/boot_b.img bs=4M'
adb shell su -c 'dd if=/dev/block/by-name/vendor_boot_b of=/sdcard/vendor_boot_b.img bs=4M'
adb pull /sdcard/boot_b.img
adb pull /sdcard/vendor_boot_b.img

# recovery (adb ya es root)
adb shell dd if=/dev/block/by-name/boot_b of=/tmp/boot_b.img bs=4M
adb shell dd if=/dev/block/by-name/vendor_boot_b of=/tmp/vendor_boot_b.img bs=4M
adb pull /tmp/boot_b.img
adb pull /tmp/vendor_boot_b.img
```

`by-name` resuelve al slot actual; `getvar` dice que es `b`. Comprueba con
`sha256sum` que lo descargado coincide con lo que hay en el teléfono
(`adb shell su -c 'sha256sum /dev/block/by-name/boot_b'`).

### El día que toque flashear (M1)

```bash
fastboot getvar current-slot          # debe decir b
# modo v4 (el que usa el stock): dos particiones
fastboot flash boot_b         shim-boot.img
fastboot flash vendor_boot_b  shim-vendor_boot.img
fastboot reboot
```

Nunca `boot_a`, `vendor_boot_a`, `vbmeta`, `lk` ni `preloader`: el slot `a` no
está sano (`slot-successful:_a: no`) y no sirve de red.

- Si arranca XNU: objetivo de M1.
- Si no arranca o se queda en el logo: apagar 10 s y restaurar **las dos**
  particiones (`boot_b` y `vendor_boot_b`) con los `.img` del respaldo. Sin drama.
- `secure: yes` con `securestate: flashing_unlocked` acepta imágenes sin firmar
  (así se instalan ROMs en este teléfono). Si el LK rechazara el arranque por
  verificación, el siguiente intento sería un `vbmeta` vacío; `vbmeta` es
  compartido entre slots, así que se decide entonces y con el respaldo delante,
  no ahora.

Los datos de `getvar` que sí sirven para el port (UFS Micron, arranque en
LUN 2, 8 GB de RAM, `MT6879V_T/ZA`, build y baseband exactos, 256 MiB de
descarga máxima) están en `hw/manaus.getvar.json`.

Opcional, informativo y no destructivo:

```bash
fastboot oem device-info
fastboot getvar partition-size:boot_b
```

## 2. Inventario completo (necesita root o recovery)

El script `tools/manaus-intake.sh` recoge FDT, `/proc/iomem`, dmesg, sysfs de
UFS/DRM/I2C/PMIC, geometría del framebuffer y las cabeceras de los `boot.img`.
Nada de eso es alcanzable sin root en Android 13.

### Opción A — Android rooteado (Magisk/KernelSU)

```bash
adb push ios-sweet/tools/manaus-intake.sh /data/local/tmp/
adb shell su -c 'sh /data/local/tmp/manaus-intake.sh /sdcard/manaus-intake'
adb pull /sdcard/manaus-intake.tgz
```

### Opción B — recovery (Lineage Recovery, OrangeFox, TWRP)

En recovery `adb shell` ya es root:

```bash
adb push ios-sweet/tools/manaus-intake.sh /tmp/
adb shell sh /tmp/manaus-intake.sh /tmp/manaus-intake
adb pull /tmp/manaus-intake.tgz
```

Si `tar` falla dentro del recovery, saca el directorio entero:

```bash
adb pull /tmp/manaus-intake
```

### Qué tiene que salir en el resumen

```
device tree: yes
dmesg bytes: >100000
ufs lines:   >20
panel lines: >20
touch lines: >0
```

`device tree: NO` significa que el script corrió sin root: sin FDT no hay
direcciones de GIC, timer ni PMIC, y no se puede escribir `0004`.

## 3. Saneado antes de compartir

El `.tgz` contiene `ro.serialno` y `ro.boot.serialno`. Antes de subirlo a
cualquier sitio:

```bash
mkdir san && tar xzf manaus-intake.tgz -C san
grep -rilE 'serialno|serial' san/manaus-intake | head
# sustituye el valor por REDACTED en 01-props y 02-boot-props
grep -ri 'REDACTED' san/manaus-intake
tar czf manaus-intake-san.tgz -C san manaus-intake
```

Comprueba también que no haya IMEI (`persist`, `radio`) en `01-props`: si
aparece `gsm.sim.operator` o similar, bórralo.

Nada de esto se commitea: `.gitignore` ya excluye `hw/raw/` y `backup/`. El
dump se queda fuera del repo.

## 4. Qué hago yo con el `.tgz`

| Dato del dump | Campo de `hw/manaus.hw.json` |
| --- | --- |
| `device-tree/cpus/cpuN/reg` | `cpus[].mpidr` |
| `27-cpufreq` (`cpuinfo_max_freq`) | `cpus[].clock_hz` |
| `device-tree/interrupt-controller@*/reg` | `gic.gicd`, `gic.gicr`, `gic.gicr_size` |
| `06-iomem` (GIC) | contraste de las dos anteriores |
| `device-tree/timer/clock-frequency` | `timer.timebase_hz` |
| `05-meminfo` + `06-iomem` (`System RAM`) | `ram.base`, `ram.size` |
| `12-dmesg-hw` (`ufshcd`, `mtk-ufs`) | `ufs.reg`, `ufs.irq`, `ufs.ref_clk` |
| `29-bootimg-hdr` | confirma `kernel_base`, offset del DTB |
| `28-fb` | geometría real del panel (contraste con el ADT) |

Con eso: `python3 tools/mkadt-manaus.py --check` debe salir 0, y escribo
`xnu/patches-manaus/0004-sweet-machine-config.patch` con los valores de MT6879
en vez de los de SM7150. Ese es el primer parche de verdad del port.

## 5. Lo que no se puede saber desde el teléfono

- Si el LK deja el framebuffer encendido y mapeado tras el handoff (solo se ve
  arrancando el shim; por eso M1 empieza con consola por UART/`psci` y luego FB).
- Si `userdata` (UFS) se puede hablar con el driver `sweet-ufs` portado a
  `mtk-ufshc`. Depende de la versión de UFSHCI del MT6879; se ve en `12-dmesg-hw`
  y en `25-ufs-sysfs`.
