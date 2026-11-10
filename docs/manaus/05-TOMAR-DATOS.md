# 05 — Tomar los datos del teléfono (M0)

Esta fase la ejecuta el usuario en el teléfono. Las secciones 0, 2 y 3 son
**solo lectura**: no escriben particiones ni cambian ajustes. Su objetivo era
rellenar `hw/manaus.hw.json`, que ya está **completo** (`python3
tools/mkadt-manaus.py --check` sale 0). La sección 1 es la excepción: explica el
único paso que escribe (`boot_b`), y solo cuando exista un respaldo verificado.

> **Estado 2026-11-08:** los dos `manaus-intake` (2026-11-05 y 2026-11-07) ya
> respondieron GIC, timer, RAM, FB, UFS, panel, táctil y PMIC. Solo queda **una
> orden corta** para cerrar el storage (LUN principal y tamaño de bloque): ver
> la sección 6. El resto de este documento queda como referencia de cómo se
> obtuvieron los datos.

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

Nunca `vbmeta`, `lk` ni `preloader`. Tampoco `vendor_boot_a`. `boot_a` solo se
toca con la excepción del 2026-11-09 (experimento de fallback de slot): se
respalda antes y se restaura después; ver sección 7.

**El kernel debe ir comprimido en gzip.** El LK (`boot_linux_fdt:510`)
descomprime siempre; una `Image` ARM64 cruda provoca
`gzip header is not correct` → `decompress kernel image fail` →
`SW Panic(9)`. `tools/mkbootimg-manaus.sh` y `shim/build.sh` comprimen ahora
la imagen automáticamente.

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

| Dato del dump | Campo de `hw/manaus.hw.json` | Valor obtenido |
| --- | --- | --- |
| `04-cpuinfo` + `device-tree/cpus/cpuN/reg` | `cpus[].mpidr`, `cpus[].part` | A55 `0xd05` 0x000–0x500, A76 `0xd41` 0x600/0x700 |
| `27-cpufreq` (`cpuinfo_max_freq`) | `cpus[].clock_hz` | 2.0 GHz / 2.5 GHz |
| `device-tree/interrupt-controller/reg` | `gic.gicd`, `gic.gicr`, `gic.gicr_size`, `gic.redistributor_regions` | GICD `0x0c000000`+`0x40000`, GICR `0x0c040000`+`0x200000`, 1 región |
| `GICR_TYPER.VLPIS` (runtime) | `gic.gicr_pe_size_v3/v4` | stride `0x20000`/`0x40000`, se lee en cada arranque |
| `device-tree/timer/clock-frequency` | `timer.timebase_hz` | 13 MHz; PPI 11 ⇒ INTID 27 |
| `06-iomem` (`System RAM`) | `ram.base`, `ram.size`, `ram.live_banks` | `0x140000000`+`0xa40f8000` |
| `device-tree/ufshci@112b0000` | `ufs.base/size/compatible/irq/clocks/resets/supplies` | `0x112b0000`+`0x2300`, `mediatek,mt8183-ufshci`, IRQ 137 |
| `08-partitions` + `23-block-dev` | `ufs.main_disk`, `ufs.partition_dev`, `ufs.boot_luns` | `sdc`, `sdc90`, `sda`/`sdb` = preloader A/B |
| `29-bootimg-hdr` | confirma `kernel_base`, offset del DTB | v4, DTB en `vendor_boot` |
| `28-fb` + `/chosen` | `fb.base`, `fb.size`, `fb.panel_name` | `0xfe0d0000`+`0x1df0000` |
| `device-tree` panel/touch/PMIC | `display.*`, `touch.*`, `pmic.*`, `gpio.*` | ver `_facts` de cada bloque |

Con eso: `python3 tools/mkadt-manaus.py --check` sale 0, y los overrides
`0003`, `0004`, `0010` y `0012` de `xnu/patches-manaus/` llevan los valores de
MT6879 en vez de los de SM7150.

## 5. Lo que no se puede saber desde el teléfono

- ~~**Si el LK deja el framebuffer encendido y mapeado tras el handoff.**~~
  **RESUELTO (2026-11-09):** el FDT vivo da `fb_base_l=0xfe0d0000`,
  `vramSize=0x1df0000`, pero `islcm_inited=0`: el panel está apagado en el
  handoff. `con_init()` lee los valores vivos y deja el dibujo desactivado
  (`LCM=off`). El log persistente (ramoops pmsg) es el canal real.
- ~~**Si `userdata` (UFS) responde con el driver `sweet-ufs` portado a
  `ufshcd-mtk`.**~~ **PARCIAL (2026-11-09):** los registros UFSHCI se leen
  (`ver=0x200`, `cap=0x3587071f`, `hce=1`, `hcs=0x10f`, `nutrs=0x20`) y el
  host acepta nuestra lista (`ours=0x40016000 rsr=1`), pero los comandos SCSI
  fallan (TUR `OCS=5`, CAP/READ timeout). Falta el vendor-init de MTK / la
  ruta UPIU. Sigue siendo el mayor riesgo de M3.
- ~~**El `CurrentEL` de entrada.**~~ **RESUELTO (2026-11-09):** el LK entrega
  en **EL1** (`PROBE EL1` en pstore). `vbar_el1`/`spsel` del shim son válidos;
  no hace falta bajar de EL2.

## 6. Lo único que faltaba del teléfono — **RESUELTO (2026-11-08)**

Se consultó en el teléfono con root, solo lectura:

```bash
adb shell su -c 'readlink -f /sys/block/sda /sys/block/sdb /sys/block/sdc; \
  cat /sys/block/sdc/queue/logical_block_size /sys/block/sdc/queue/physical_block_size'
```

Resultado medido:

| Dato | Valor | Cómo se comprobó |
| --- | --- | --- |
| `sda` | `0:0:0:0` | `readlink -f /sys/block/sda` |
| `sdb` | `0:0:0:1` | `readlink -f /sys/block/sdb` |
| `sdc` (GPT principal) | **`0:0:0:2` → LUN 2** | `readlink -f /sys/block/sdc` |
| bloque lógico | **4096** | `queue/logical_block_size` |
| bloque físico | **4096** | `queue/physical_block_size` |

Confirmación cruzada: el sector de `sdc` en el byte `0x1000` (LBA 1 con bloques
de 4 KiB) contiene `EFI PART`; en LUN 0 y LUN 1 el sector 0 contiene la firma
`UFS_BOOT`. Los descriptores del dispositivo dan `number_of_luns = 3`,
`bBootLunEn = 0x02` y `max_number_of_luns = 1`.

Consecuencias:

- El **LUN principal es 2**, no 0. Se corrigió `hw/manaus.hw.json`
  (`ufs.main_lun = 2`, `number_of_luns = 3`, `logical_block_assumed = false`) y
  el override `xnu/patches-manaus/0028-sweet-ufs-root-device.patch`
  (`su.lun = 2`).
- Los accesos raw a la GPT y a `userdata` se hacen en bloques de 4 KiB; el
  driver expone sectores lógicos de 512 B por encima.
- LUN 0/1 son los boot LUNs del preloader (4 MiB, `mmcblk0boot0/1`), no el
  almacenamiento de Android.

El storage queda cerrado; solo queda el primer arranque de hardware (con
respaldos verificados).

## 7. Primer arranque de hardware — **HECHO (2026-11-09)**

### 7.1 Causa raíz de los "cuelgues": el LK exige gzip

`boot_linux_fdt:510` llama siempre al descompresor gzip. Una `Image` ARM64
cruda produce:

```
boot_linux_fdt:510: decompress kernel image...
gzip header is not correct. Error!
panic (caller ...): decompress kernel image fail!!!
HALT: action = reboot(0), reason = SW Panic(9)
```

Nunca se ejecutaba ni el shim ni la sonda. Los `kernel_sz` de los intentos del
slot `a` en la partición `logs` (`0x2c000`, `0x2f000`, `0x2f000`, `0x2f000`,
`0xe0`) lo confirman. El kernel stock del slot `b` sí es gzip
(`kernel_size=0x1297a7d`, primeros bytes `1f 8b 08`).

Solución: comprimir la `Image` antes de empaquetar. Ya está en
`tools/mkbootimg-manaus.sh` (detecta si ya es gzip y comprime si no) y en
`shim/build.sh` (`out/Image.gz`). El header ARM64 del shim es
`code0 = b _start` (`_start` en `0x800` por la alineación de `vectors`),
`image_size = _end`, `flags = 0xa`, magic `ARMd`; el LK salta al inicio de la
imagen descomprimida, así que es compatible.

### 7.2 Fallback de slot sin fastboot

`fastboot boot`/`fetch` no existen en este LK. Visitar fastboot borra el
ramoops. La secuencia que funciona:

1. respaldar `boot_a` (hecho: `sha256=bbc67027…`, idéntico a `boot_b`);
2. `fastboot flash boot_a shim-boot.img` (v4, solo el kernel);
3. `fastboot set_active a` (deja `slot-retry-count:_a = 7`);
4. arrancar y **no tocar fastboot**: el shim reinicia (PSCI), el LK agota los
   7 reintentos, marca `a` no arrancable y arranca Android del slot `b`.

`logs` muestra `retry 7→6→…→1` con `decompress kernel image...` +
`jump to linux kernel 64Bit` en cada pasada, y luego el arranque de `b`.

### 7.3 Resultados medidos

- **Sonda mínima** (`shim/probe.S`, 224 B → 179 B gzip): pstore =
  `PROBE EL1`. El LK entrega en **EL1**; PSCI `SYSTEM_RESET` funciona.
- **Shim completo** (`-DSHIM_MILESTONE_A=0`, sin payload, 192,512 B →
  8,607 B gzip), solo `boot_a`, `set_active a`. Android volvió solo en 167 s.
  pstore (`pmsg-ramoops-0` y `console-ramoops-0`, 873 B cada uno):

```
EL=1 DTB=0x47c80000 FBBASE=0xfe0d0000 FBSIZE=0x1df0000 LCM=off
INITRD 0x66f00000..0x6883926f
LOADER=no SWEETXNU payload
UFS ver=0x200 cap=0x3587071f hce=1 hcs=0x10f rsr=1 nutrs=0x20
UFS ours=0x40016000 rsr=1
UFS TUR rc=2 ocs/st=0x0500
UFS CAP rc=1 ocs/st=0x0500
UFS READ rc=1 ocs/st=0x0500
STAGE=done
```

- **UFS:** registros y lista de transferencia OK; los comandos SCSI fallan
  (`OCS=5` en el primer TUR, luego timeouts). Falta el vendor-init de MTK o
  corregir la UPIU/PRDT. Es el siguiente trabajo de M3.
- **Estado del teléfono tras la prueba:** `boot_a` restaurado desde el backup
  verificado, `set_active b`, Android `_b` en 28 s. `vendor_boot_a` y
  `vendor_boot_b` intactos.
- **Pendiente para probar el payload completo en el slot `a`:** el
  `linux,initrd-start` del LK es el vendor ramdisk (`0x66f00000`), así que el
  payload debe ir en `vendor_boot_a`. Eso requiere aprobación explícita del
  usuario, backup de `vendor_boot_a` y una prueba de que el UFS responde.
