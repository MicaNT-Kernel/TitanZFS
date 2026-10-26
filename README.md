# TitanZFS

**Native C++23 ZFS storage pool engine and filesystem for Windows NT and MicaNT.**

Zero external dependencies. Sub-100KB binary footprint. Storage pool virtualization, RAID-Z1 single parity, mirror self-healing, 256-bit Fletcher-4 bit-rot protection, instantaneous Copy-on-Write (CoW) snapshots, and fast LZ4/RLE block compression.

---

## Deployment Scope & Design Philosophy

> [!NOTE]
> **Intended Scope:** TitanZFS is engineered specifically for **secondary storage pools, NVMe/SSD/HDD RAID arrays, media creation volumes, gaming drives, and backup repositories**.
> 
> **Boot Drive Non-Goal:** TitanZFS intentionally does **not** target booting the Windows operating system itself (`C:\`). By design, this avoids the requirement for an unstable, legacy kernel-mode `.sys` boot driver.

### Architecture: User-Mode Filesystem and Direct Block I/O

1. **System Stability:** The core engine (Fletcher-4, DMU, SPA, RAID-Z1, compression) runs purely in user mode. An I/O error or pool fault will not crash or blue-screen your Windows PC.
2. **Zero Driver Signing Hassles:** Does **not** require `bcdedit /set testsigning on`, Test Mode watermarks, or disabling Windows Driver Signature Enforcement.
3. **Secure Boot & HVCI Compliant:** Runs cleanly on stock Windows 10, Windows 11, and Windows Server with Secure Boot, BitLocker, and Core Isolation (Memory Integrity) enabled.
4. **Direct Raw Hardware Access:** Talks directly to raw hardware sectors (`\\.\PhysicalDrive1`, `2`, `3`) via Win32 unbuffered block device handles (`FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH`) for high NVMe/SATA throughput.
5. **Explorer & Win32 Integration:** Projected directly into Windows File Explorer, command line, and applications as a native drive letter (e.g. `Z:\`) via the Microsoft-WHQL-attested WinFsp userland bridge.

---

## Architecture Overview

```mermaid
graph TD
    User["Windows Apps / File Explorer / Shell"] -->|File I/O (Z:\, Y:\)| WinFsp["WinFsp Userland Bridge<br/>(titanzfs_mount.exe / WinFspMountSession)"]
    CLI["CLI Management Tools<br/>(zpool.exe, zfs.exe)"] -->|Named Pipe RPC| IPC["IPC Endpoint<br/>(\\.\pipe\titanzfs_ipc)"]
    IPC --> Svc["TitanZFS Background Daemon<br/>(titanzfs_svc.exe)"]
    WinFsp --> DMU["Data Management Unit (DMU)<br/>Datasets, CoW Snapshots & .zfs Traversal"]
    Svc -->|5s Async Sync Loop| SPA["Storage Pool Allocator (SPA v5000)<br/>Uberblock Ring & Metaslab Spacemaps"]
    DMU --> SPA
    SPA --> Comp["Fast Compression<br/>(LZ4 & RLE with 87.5% Threshold)"]
    Comp --> VDEV["Virtual Device Tree (VDEV Engine)"]
    VDEV --> Mirror["Mirror VDEV<br/>(N-Way Transparent Self-Healing)"]
    VDEV --> RaidZ["RAID-Z1 VDEV<br/>(N+1 Dynamic Parity, XOR Rebuild)"]
    Mirror --> Leaf["Win32BlockDevice Leaf<br/>(\\.\PhysicalDriveN or .raw images)"]
    RaidZ --> Leaf
```

---

## Core Capabilities

### 1. 256-Bit Fletcher-4 End-to-End Checksums
Every 128-byte Block Pointer (`blkptr_t`) embeds a 256-bit Fletcher-4 checksum computed across four 64-bit accumulators over 32-bit words:
$$\begin{aligned}
A &= \sum x_i \pmod{2^{64}} \\
B &= \sum A \pmod{2^{64}} \\
C &= \sum B \pmod{2^{64}} \\
D &= \sum C \pmod{2^{64}}
\end{aligned}$$
Any bit-flip or silent data corruption immediately triggers `STATUS_DATA_CHECKSUM_ERROR (0xC00002EB)` and transparent self-healing.

### 2. RAID-Z1 Dynamic Single Parity Engine
In an $N+1$ stripe configuration (e.g. 2 data disks + 1 parity disk), TitanZFS generates dynamic parity blocks on the fly using fast XOR SIMD passes. If any single drive faults or goes offline, reads to that drive are dynamically reconstructed in-memory without downtime.

### 3. N-Way Mirror Self-Healing Read Repair
When reading from a mirror VDEV, if a child disk reports a checksum verification failure, TitanZFS automatically falls back to an alternate healthy mirror branch, retrieves the verified data, and repairs the corrupt child disk in-place.

### 4. 128-Slot Uberblock Ring Buffer & Metaslab Spacemaps
Storage pool transactions are committed monotonically through a 128-slot cyclical Uberblock ring buffer (`ub_magic = 0x00BAB10C`, SPA version `5000`). Allocations survive pool export/import and power loss via persistent atomic 16-byte spacemap record logging.

### 5. Instantaneous CoW Snapshots & Virtual `.zfs/snapshot` Browsing
Datasets support $O(1)$ instantaneous Copy-on-Write snapshot creation (`zfs snapshot dataset@snap`) and atomic rollback (`zfs rollback dataset@snap`). Mounted drives expose a virtual, read-only `.zfs/snapshot/<snap_name>/` hierarchy directly in Windows File Explorer for drag-and-drop file recovery.

### 6. LZ4 & RLE Block Compression
Automatic block compression with an 87.5% size threshold:
* All-zero blocks compress to 9 bytes via RLE (`BP_COMP_RLE`).
* Blocks compressing to $< 87.5\%$ are stored compressed (`BP_COMP_LZ4`) with Fletcher-4 evaluated over the stored payload.
* Incompressible blocks default to uncompressed (`BP_COMP_OFF`).

### 7. ZFS Send / Recv Stream Replication
Snapshots can be serialized into compressed binary streams (`zfs send dataset@snap backup.zfs`) and received into target pools (`zfs recv tank/replica backup.zfs`) with end-to-end Fletcher-4 stream verification and bit-flip rejection.

---

## Native Windows Daemon & Tooling

### `titanzfs_setup` - 1-Click Standalone Installer & Uninstaller
```text
Self-contained Win32 Dark Mode single-file installer (no zip extraction needed).
Embeds all binaries (titanzfs_admin, titanzfs_svc, titanzfs_mount, zpool, zfs, assets)
directly in the executable payload using clean-room LZ4 compression.
Automatically provisions TitanZFS into C:\Program Files\TitanZFS, configures
system PATH environment variables, registers and starts the Windows NT Auto-Start
Service, creates Start Menu / Desktop shortcuts, and registers standard Windows
"Add or Remove Programs" uninstallation entries.

Usage:
  titanzfs_setup.exe              # Launch interactive standalone installer
  titanzfs_setup.exe /silent      # Unattended silent install
  titanzfs_setup.exe /uninstall   # Clean system removal & service unregistration
```

### `titanzfs_admin` - Native Win32 Management Console
```text
Native Windows NT 10/11 & Windows Server Dark Mode GUI Management Console.
Provides real-time visualization of Storage Pools, Physical/Virtual drives,
Dataset mount points, and CoW snapshots with single-click Scrub, Mount,
and Rollback capabilities. Strictly native Win32/C++23—zero Electron,
zero web bloat, instant startup (<50ms), and minimal memory footprint (~4MB).
```

### `titanzfs_svc` - Windows NT Background Service
```text
Usage: titanzfs_svc [command]

Commands:
  install, --install     Register as Windows NT Auto-Start Service (SERVICE_AUTO_START)
  uninstall, --uninstall Unregister and remove Windows NT Service
  run, --console, -c     Run daemon directly in console (interactive/debug)
```

### `zpool` - Storage Pool Management
```text
usage: zpool <subcommand> <args ...>

Subcommands:
  status [-v] [pool]       Show pool status, VDEV topology, and error statistics
  list [pool]              List storage pool summaries, capacities, and health
  create <pool> [type] ... Create a new storage pool (raidz1, mirror, stripe)
  scrub <pool>             Initiate background bit-rot checksum scrub and repair
  clear <pool>             Clear device error counters and transient fault latches
  destroy <pool>           Destroy an existing storage pool
```

### `zfs` - Dataset & Snapshot Administration
```text
usage: zfs <subcommand> <args ...>

Subcommands:
  list [-r] [dataset]      List datasets, sizes, mounts, and refer properties
  create <dataset>         Create a new ZFS hierarchical dataset
  snapshot <dataset@snap>  Create an instantaneous O(1) Copy-on-Write snapshot
  rollback <dataset@snap>  Atomically revert dataset state to a specific snapshot
  send <dataset@snap> [f]  Serialize snapshot into compressed, Fletcher-4 stream
  recv <dataset> [file]    Receive and deserialize stream into target dataset
  destroy <dataset[@snap]> Destroy a dataset or snapshot
  mount <dataset> <drive:> Mount dataset via WinFsp userland driver bridge
```

---

## Building & Testing

### Requirements
- Any modern C++23 compiler:
  - **Windows**: Clang 18+, MSVC 19.38+ (VS 2022 17.8+), or GCC 13+
  - **Linux**: Clang 18+ or GCC 13+
- CMake 3.24+
- Ninja (optional, recommended)
- WinFsp (for Windows drive letter mounting)

### Build with CMake
```bash
git clone https://github.com/MicaNT-Kernel/TitanZFS.git
cd TitanZFS

# Configure & Build (statically linked, zero DLL dependencies)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j2

# Run Full 12-Test Regression Suite
ctest --test-dir build --output-on-failure
```

---

## Relationship to MicaNT

TitanZFS was originally conceived and designed as the storage pooling and RAID subsystem for **[MicaNT](https://github.com/MicaNT-Kernel/MicaNT)**—an independent C++23 NT-compatible operating system kernel. Because of its freestanding architecture, it is also published as a standalone project for Windows NT.

---

## License

Licensed under the Apache License, Version 2.0 (the "License"). You may obtain a copy of the License in the [`LICENSE`](LICENSE) file or at:
http://www.apache.org/licenses/LICENSE-2.0
