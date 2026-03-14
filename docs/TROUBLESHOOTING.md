# Troubleshooting

## Recovering from a corrupted `/etc/crypttab` on an encrypted Linux host

The NVR server is Linux-only and is commonly deployed on systems that use LUKS
full-disk encryption (the default for Ubuntu/Linux Mint installers). If the host
machine cannot boot because `/etc/crypttab` was edited incorrectly, follow the
steps below to fix it from a live USB.

### Typical partition layout (Linux Mint / Ubuntu with LUKS)

```
/dev/nvme0n1p1  –  vfat         EFI System Partition  (mounted at /boot/efi)
/dev/nvme0n1p2  –  ext4         /boot                 (unencrypted)
/dev/nvme0n1p3  –  crypto_LUKS  encrypted container   (contains LVM: root + swap)
```

A `blkid` output that matches this layout looks like:

```
/dev/nvme0n1p1: TYPE="vfat"      PARTLABEL="EFI System Partition"
/dev/nvme0n1p2: TYPE="ext4"
/dev/nvme0n1p3: TYPE="crypto_LUKS"
```

Additional `crypto_LUKS` devices (e.g. `/dev/sda1`, `/dev/sdb1`) are separate
encrypted drives – often used for NVR recording storage – and are **not** the
system root.

### Step-by-step recovery

Boot from a live USB (e.g. Linux Mint "Try Linux Mint" mode) and open a
terminal.

#### 1. Unlock the LUKS container

```bash
sudo cryptsetup luksOpen /dev/nvme0n1p3 root_crypt
# Enter your disk encryption passphrase when prompted.
```

A mapped device `/dev/mapper/root_crypt` is now available.

#### 2. Activate LVM volume groups (if applicable)

Most Linux Mint / Ubuntu LUKS installations place LVM inside the LUKS
container:

```bash
sudo vgchange -ay
```

List the resulting logical volumes:

```bash
sudo lvs
# Example output:
#   LV     VG        ...
#   root   ubuntu-vg ...
#   swap_1 ubuntu-vg ...
```

The mapper path for the root volume is `/dev/mapper/<VG_NAME>-<LV_NAME>`,
e.g. `/dev/mapper/ubuntu--vg-root`.

> If `lvs` shows nothing (no LVM), skip to step 3 and mount
> `/dev/mapper/root_crypt` directly.

#### 3. Mount the root filesystem

```bash
sudo mount /dev/mapper/ubuntu--vg-root /mnt
# (replace ubuntu--vg-root with the name shown by lvs, or use root_crypt
#  directly if there is no LVM layer)
```

Also mount `/boot` so the full `/etc` tree is accessible:

```bash
sudo mount /dev/nvme0n1p2 /mnt/boot
```

#### 4. Edit `/etc/crypttab`

```bash
sudo nano /mnt/etc/crypttab
```

The correct format for a single LUKS root partition is:

```
# <target name>  <source device>           <key file>  <options>
root_crypt        UUID=<UUID-of-nvme0n1p3>  none        luks,discard
```

Replace `<UUID-of-nvme0n1p3>` with the UUID shown by `blkid /dev/nvme0n1p3`.

> **Common mistakes that prevent booting**
> - Wrong UUID (copy-paste error or referencing the wrong partition)
> - Missing or misspelled target name (must match what `initramfs` expects)
> - Extra or missing whitespace/tabs in the line
> - Referencing a device path (`/dev/nvme0n1p3`) instead of a stable
>   `UUID=…` identifier

Save the file (`Ctrl+O`, `Enter`, `Ctrl+X` in nano).

#### 5. Unmount and reboot

```bash
sudo umount /mnt/boot
sudo umount /mnt
sudo vgchange -an          # deactivate LVM (if used)
sudo cryptsetup luksClose root_crypt
sudo reboot
```

Remove the live USB when prompted; the system should now decrypt and boot
normally.

### Identifying the correct LUKS partition

If you are unsure which `crypto_LUKS` device holds your root filesystem, look
for the one on your **primary NVMe or SATA system drive** (usually
`/dev/nvme0n1p3` or `/dev/sda3`). Additional encrypted drives used for NVR
recording storage (e.g. `/dev/sdb1`, `/dev/sdc1`) will not contain `/boot` or
`/etc`.

You can also inspect a LUKS header to confirm it is the right device:

```bash
sudo cryptsetup luksDump /dev/nvme0n1p3 | grep UUID
```

Match the UUID printed there against what is currently in `/etc/crypttab` to
confirm you are editing the correct entry.
