# set_zte_imei

Change the IMEI of a **ZTE MU5250 / U60 Pro** (5G mobile router, Qualcomm SDX75,
OpenWrt) on firmware **`…HK…MU5250V1.0.0B03`**, directly on the device.

One self-contained script you copy onto the modem and run there. The helper
binaries are embedded; the EFS `/config` "plan" is built from **your own device's**
`/config` at runtime, so nothing device-specific is baked into the file.

> ⚠️ **Legal / risk.** Changing a device's IMEI is **illegal in many countries**.
> Only use this on hardware **you own**, where it is lawful for you to do so. It
> writes modem NV/EFS; a bad flash can brick the radio. **No warranty.** You are
> solely responsible. See [LICENSE](LICENSE).

---

## Why it exists

On B03 the IMEI lives in modem **NV item 550**, and a direct NV write is rejected
with **`status 7 = NV_READONLY`**. The read-only gate is not lifted by `AT+CFUN`
modes or an SPC/password — it is a **flag (`flag102`) inside the EFS `/config`
object**. The bootloader reads that flag at startup: if it is `1`, NV writing is
enabled **for that boot session**, and `/config` is restored to factory (flag `0`).

So the full sequence is:

1. raise `flag102` in `/config` (`zte_config_b03 --enable-flag`)
2. **reboot** → this boot has NV writing enabled
3. write NV 550 in that session (`diagnv write 0 …`) → `status 0 = NV_DONE`
4. **reboot** → flag is `0` again → NV read-only (locked)

Because the write-enable needs a reboot, and a reboot kills the running process,
the tool is **resumable**: it does one phase, reboots, and you run it again.

---

## Supported / tested

- **Device:** ZTE MU5250 (sold as U60 Pro), SDX75, OpenWrt 23.05.4.
- **Firmware:** `BD_XCBZHK…MU5250V1.0.0B03` (HK). Other firmwares (B02, CN B31, …)
  are **not** targeted here — the stock vendor tool targets B31; this one unlocks B03.
- **Requirements on the modem:** root shell (ADB or SSH), `/usr/lib/libdiag.so.1`
  present (it is, stock), busybox `awk` + `base64`. The device is **single-IMEI**:
  only NV 550 **index 0** is written.

---

## Install

Copy the single file to the modem (pick whichever access you have):

```sh
# via ADB:
adb push set_zte_imei /data/local/tmp/
# or via SSH (scp is usually unavailable on these modems — no sftp-server):
ssh root@<modem> 'cat > /data/local/tmp/set_zte_imei' < set_zte_imei
```

Everything else runs **on the modem**.

---

## Usage

```sh
sh /data/local/tmp/set_zte_imei read          # show current IMEI (also saves a backup)
sh /data/local/tmp/set_zte_imei check          # read-only: /config factory & DIAG OK?

# change IMEI — three runs, two reboots:
sh /data/local/tmp/set_zte_imei <NEW_IMEI>     # phase 1: backup + enable-flag, reboot
#   ... modem reboots, reconnect ...
sh /data/local/tmp/set_zte_imei                # phase 2: write IMEI, reboot (re-lock)
#   ... modem reboots, reconnect ...
sh /data/local/tmp/set_zte_imei                # phase 3: verify -> done

sh /data/local/tmp/set_zte_imei restore        # write back the IMEI this tool backed up
sh /data/local/tmp/set_zte_imei abort          # drop a half-finished job (no reboot)
```

- `<NEW_IMEI>` is 15 digits and must pass the Luhn checksum.
- The **first** `read`/change saves your current IMEI record to
  `szi_imei_backup.hex` next to the tool; `restore` writes it back. Keep that file.
- State between phases lives in `.szi_state`. Each phase checks a real signal
  (`outcome=0`, `status=0000`, `VERIFY OK`, final read-back) and **will not lock
  over a failed write**.

**Important:** there must be **exactly one** reboot between phase 1 and phase 2 —
the one the tool triggers. An extra manual reboot in between clears the flag, and
phase 2 then returns `status 7`; if that happens, `abort` and start over.

---

## Build from source

Needs Docker (for the aarch64/musl cross-compile). Rebuilds `set_zte_imei` with
freshly compiled, embedded helpers:

```sh
./build.sh
```

Sources in [`src/`](src/):

| file | role |
|------|------|
| `set_zte_imei.template` | the on-device shell driver (the blobs are appended to it) |
| `zte_config.c`          | EFS `/config` flag102 writer (`--enable-flag` / `--check-original` / …) |
| `zte_config_read.c`     | read-only EFS `/config` dumper |
| `mkplan.c`              | builds the enable-flag plan from this device's `/config` |
| `diagnv.c`              | NV 550 read/write over libdiag |
| `zte_nv.c`              | reference NV pair writer (not used by this tool; single-IMEI device) |
| `plan_io.h`             | shared plan-file reader |

---

## How it's verified

- The `flag102 → reboot → NV write` mechanism was confirmed on a live B03 unit:
  `enable-flag` (`outcome=0`), one reboot, then `diagnv write 0` returns
  **`status 0`** (vs `7` without the flag), `VERIFY OK`, and `/config` is restored
  to factory by the boot.
- `mkplan` is checked to reproduce, byte-for-byte, the plan that was proven on that
  unit; IMEI⇄record encoding round-trips and rejects non-Luhn input.
- This repackaged, device-agnostic build (per-device plan + IMEI backup) has **not**
  been run end-to-end on-device after packaging. Run `read` and `check` first — both
  are read-only — before attempting a change, and make sure you have a backup.

---

## Credits

The DIAG/EFS helper design (`zte_config`, `zte_config_read`, `zte_nv`, `plan_io`)
is derived from the **`SadykovIV/ZTE_U60Pro`** project
(<https://github.com/SadykovIV/ZTE_U60Pro>), which targets B31. The B03 unlock
(accepting the B03 `/config` EFS mode), `mkplan`, `diagnv`, and this on-device
resumable driver are the additions here.

## License

MIT — see [LICENSE](LICENSE).
