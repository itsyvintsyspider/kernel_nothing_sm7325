# kernel_nothing_sm7325

Custom Linux kernel for the **Nothing Phone (1)** (codename `spacewar`), built for [Neoteric OS](https://github.com/Neoteric-OS).

## Device

| | |
|---|---|
| Device | Nothing Phone (1) |
| Codename | `spacewar` |
| Model | A063 |
| SoC | Qualcomm Snapdragon 778G+ (SM7325 "Lahaina") |
| GPU | Adreno 642L |
| Kernel base | `LineageOS/android_kernel_nothing_sm7325` (`lineage-23.2`) |
| Kernel version | Linux 5.4.y |
| Defconfig | `arch/arm64/configs/vendor/lahaina-qgki_defconfig` |

## What's different from stock

- **BORE (Burst-Oriented Response Enhancer) scheduler** — `linux5.4.y-bore5.1.0` from [firelzrd/bore-scheduler](https://github.com/firelzrd/bore-scheduler), tuned for battery-first behavior (`penalty_offset=18`, `penalty_scale=1536`, `cache_lifetime=90000000`, `smoothness_short=1`), compiled in as defaults — no runtime tuning required.
- **EROFS with compression** — `system`, `system_ext`, `product`, `vendor`, `vendor_dlkm`, and `odm` build as compressed (`lz4hc`) EROFS images. Full `EROFS_FS_ZIP`/`EROFS_FS_ZIP_LZ4`/`EROFS_FS_XATTR`/`EROFS_FS_POSIX_ACL`/`EROFS_FS_SECURITY` support enabled.
- **ZRAM default compressor: zstd** — set at the driver level (`drivers/block/zram/zram_drv.c`), not via runtime/init.rc write, for better effective memory headroom under the fixed ZRAM pool size.
- **`include/linux/pgtable.h` reorganization** — cherry-picked upstream page-table header consolidation, clearing the way for future patches that assume this layout.
- **CoreSight/STM stripped** — full debug tracing stack removed (`# CONFIG_CORESIGHT is not set`, `# CONFIG_STM is not set`); not needed for daily-driver use, trims build size/time.
- **Kyber** set as the default I/O scheduler.
- **AVB (Android Verified Boot)** enabled with release-key signing, in place of stock's disabled verification / test keys.

## Building

Kernel source path expected by the device tree: `kernel/nothing/sm7325`.

```bash
source build/envsetup.sh
lunch phone1-userdebug
m bootimage   # fast kernel-only iteration
m updatepackage   # full flashable package
```

Flash `boot.img` and `dtbo.img` together — never `boot.img` alone.

## Credits

- [firelzrd](https://github.com/firelzrd) — BORE scheduler, ADIOS, le9uo
- [Willay24](https://github.com/Willay24) — reference tree, VFS umount backport, KFENCE/FUSE_BPF groundwork
- [Tashar02 / Atom-X-Devs](https://github.com/Atom-X-Devs) — KernelSU ARM64 branch-link hooking driver reference
- LineageOS — kernel base

## Maintainer

[itsyvintsyspider](https://github.com/itsyvintsyspider)
