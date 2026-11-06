# 05 — Tomar los datos del teléfono (M0)

Esta fase la ejecuta el usuario en el teléfono. Es **solo lectura**: nada de lo
que hay aquí escribe particiones ni cambia ajustes. El objetivo es rellenar
`hw/manaus.hw.json`, que hoy tiene 20 valores a `null`.

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

## 1. Go/no-go #1: ¿acepta `fastboot boot`?

La duda más grande es si el LK de Motorola/MTK arranca un kernel desde RAM en
vez de desde la partición `boot`. Se prueba con el `boot.img` **stock** que ya
tienes, sin escribir nada:

```bash
fastboot boot backup/boot.img
```

- Si arranca Android normal: el LK sí soporta `fastboot boot`. Es el mejor
  escenario y todo el bring-up se hace sin tocar `boot`.
- Si contesta `FAILED (remote: 'unknown command')` o similar: no lo soporta.
  Entonces el plan B es empaquetar con `tools/mkbootimg-manaus.sh`, flashear
  `boot` y volver con `fastboot flash boot backup/boot.img`. Se decidirá
  contigo antes de flashear nada.
- Si se queda colgado: se fuerza apagado (botón 10 s) y arranca Android
  normalmente. `fastboot boot` no modifica `boot`, así que no hay riesgo de
  brick por esta prueba.

Opcional, también informativo y no destructivo:

```bash
fastboot oem device-info     # algunos Motorola: estado de unlock
fastboot getvar current-slot
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
