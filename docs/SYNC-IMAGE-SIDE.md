# 同步：镜像侧已被我改了什么（2026-11-15，给同时在做设备侧调试的那位）

**我只动了 `board.toml` 一个文件**。没有碰 `package/qcom-usb-debug/`、没有碰 `workspace/uefi-uke` 固件、
没有提交、没有推送、没有触发任何 CI。

## 一、`kernel_args`：删掉了 `armcpu-probe=1`（**这条会影响你的下一轮**）

2026-11-15 11:51 的照片里，那个 panic 是**我们自己的诊断**，不是故障：

```
panic(cpu 0 caller ...): "ArmCpu: uke interrupt-path diagnostics -- read the lines above"   @ArmCpu.cpp:546
```

`drivers/ArmCpu/ArmCpu.cpp:535-546`：`armcpu-probe==1` → `dumpIrqState()` ×2 → hold 15 s →
`panic(...)`，而且它**故意不走 `registerService()`**。删掉这条 arg 后代码走默认分支
（`probe==0` → 打一行 `ArmCpu: armcpu-probe=0, continuing to registerService` 并继续）。

副作用是好的：命令从 **997 → 982 字节**（上限 1000，`payload.py:38`）。
现在还有 18 字节余量：`vc-text=1` 放得下（→992），`exec-log=1` 放不下（→1003，还差 3 字节）。

**这条会改变你下一轮镜像的行为**：内核不会再在 ArmCpu 那里停住，会继续往下走到
真实的下一个失败点（照片里已经能看到的 `AppleAPFSContainerScheme: Failed to initialize/load the
APFS container` 就是候选）。

## 二、镜像侧的四处（纯主机，不改变内核行为）

| 位置 | 改动 | 依据 |
|---|---|---|
| `[ipsw.sha256]` | 钉了 **6 条**成员 sha256 | 全部**程序化**从 `out/ipsw/26C5035r/<member>.json` 读出来，逐条与 `.json` 复核一致 |
| `[userdata] windows` | 加 **`App` → `Cryptex1,AppOS`** | 该成员在 26C5035r 里**存在**（54,525,952 B，sha256 `797bb1e0…23d1`）+ 它的 trust cache 也在 |
| `[payload.runtime]` | **新增，但 `enabled = false`** | 一旦设 true，`payload`/`bootimg` 会立刻要求 Data seed（`runtime.py:52-56`），而 seed 只能在 Mac 上做 → 会把你的调试链堵死 |
| `[kernel.prelink] kernelcache_sha256` | 注释从"未核实"改成**已确认** | 实测 `out/ipsw/26C5035r/kernelcache.decompressed` 的 sha256 = `d1003b51…9fe9`，与钉住的值**完全一致** |

## 三、照片里最有价值的一条：**defconfig 把 owner-root/Data 整条路关着**

`configs/uke-macos_defconfig` 里这些**全部 `is not set`**：

```
CONFIG_XNU_VERIFIED_ROOT_PROBE / OWNER_ROOT_AUDIT / OWNER_ROOT_TRUST
CONFIG_XNU_OWNER_CACHE_MOUNT / OWNER_CACHE_STARTUP / OWNER_BOOT_MANIFEST
CONFIG_XNU_OWNER_DATA_MOUNT / OWNER_UNION_SYSTEMOS / BOOT_PROGRESS
```

所以照片里的 `AppleAPFSContainerScheme: Failed to initialize/load the APFS container` 是**预期结果**——
内核根本没被配置成挂 owner root / Data。要让 Data 挂上 `/System/Volumes/Data`，缺的是一条链：

1. `[userdata] windows` 三个窗口（✔ 已加）
2. **owner-root 签名策略**：`scripts/make-root-policies.py --tree TREE --output POLICIES --window NAME=IMAGE:VOLUME_UUID`
   → 产出 `POLICIES/NAME/{manifest.header.bin, trusted-key.bin, verification.json}` + `…/hashes.bin`
   （需要真实的 OS/SystemOS 镜像——**现在有了**）
3. 打开 `CONFIG_XNU_OWNER_ROOT_TRUST` / `OWNER_DATA_MOUNT` / `VERIFIED_ROOT_PROBE` / `OWNER_UNION_SYSTEMOS`
4. **Data seed**（`make-macos-data-seed.py`，只能在 Mac 上：OS 成员是 AEA 加密，本机没有 `aea`/`hdiutil`）
5. 把 `[payload.runtime] enabled` 设为 `true` 并带 `--data-ramdisk`

## 四、要你确认/注意的两件事

1. **`kernel_args` 我动了**——如果你这轮正在改 boot args（比如加 `vc-text=1`），先 `git diff board.toml` 看一眼，
   别把 `armcpu-probe=1` 又加回来。剩余字节预算：**18**。
2. **`[userdata] windows` 多了一个窗口**，所以下一次 `build userdata` 出来的镜像会和之前**不一样**
   （多一个 App 卷）。如果你手上还有靠旧 userdata 做的对照实验，先记下这一点。

## 五、我这边接下来要做的（不需要你配合）

- `make-root-policies.py` 的前置调研：它到底要 `IMAGE:VOLUME_UUID` 里的什么、能不能在 Linux 上跑（AEA 解密是拦路虎）。
- 把 `vc-text=1` 加进 `kernel_args`（先确认它和已有的 `-uketextconsole` 是不是同一件事、会不会重复占用字节）。

---

# 追加（2026-11-15 下午，Mac 协助完成的三件事）

那台 x86 黑苹果（macOS 26.7.1，`tonghua@192.168.1.186`）已经用过一轮，下面这些**从今以后在 Linux 上都能自己做**。

## 一、AEA 解密完成（Linux 侧永久可用）

| 成员 | 解密后大小 | sha256 | 缓存记录 |
|---|---|---|---|
| `OS` | 14,493,417,472 | `40d8282eb8650da9b4d7437a981764c48d47ee4bdc5b8b362248f25ecbce0e70` | `141-22233-021.dmg.json` |
| `Cryptex1,SystemOS` | 2,917,138,432 | `14330ed990fe7022114b10718f4defe3049d49b6e5d52fd3ac71a487541cd1f3` | `141-22145-021.dmg.json` |

都在 `out/ipsw/26C5035r/`。用仓库自己的 `sha256_file()` 复刻了 `disk_image()` 的 6 项校验 → **ALL OK**，
所以 **`unixnu userdata` 现在能在 Linux 上跑了**（`udif.py` 原生解析 DMG，不需要 hdiutil）。

坑记一条：`aea decrypt` 的密钥必须带 **`base64:` 前缀整行**传（`ipsw.py:222` 就是那样），只给 base64 正文会 `Error parsing key`。

## 二、Data seed 已生成并回传

- 文件：`out/uke/macos-27.3-uke/uke-macos/runtime/data.dbr`（22,614,176 B）
- 容器 210 GiB，`encoded_sha256 = a43d3ee0162fff133a3748456bdcd3d168e005bd0f0adef4759cae0c55e80924`
- **卷 UUID = `BB2462E2-040E-4F82-9C69-CACB7A872551`**
- 没有 `--skip-setup`（这份 checkout 的脚本没有 `--user`，两者一起用会得到"跳过设置助理但没有账户"的死局）

## 三、owner trust bundle 在 Linux 上构建成功

`runtime/owner-trust.bundle`（451,264 B）由三个 trust cache 组件合成 —— `build-boot-runtime.py`
只要 seed 存在且不要求 GPU addon，就**完全不需要 macOS**。

## 四、`[payload.runtime]` 已按 sheng 的形态写好，但 `enabled = false`

原因写进 board.toml 了：`runtime.py:110` 拿 seed 的卷 UUID 和**编译进内核的**
`CONFIG_XNU_OWNER_DATA_VOLUME_UUID` 比，而当前内核是 owner-Data 之前的（defconfig 里那四个
`CONFIG_XNU_OWNER_*` 全关）。**`enabled = true` 会让每次 `unixnu payload`/`bootimg` 都报
"Data seed UUID differs from the compiled configuration"，会把设备侧调试堵死**，所以先关着。

翻开的三个前置：①defconfig 里正好一处 `CONFIG_XNU_OWNER_DATA_VOLUME_UUID="BB2462E2-…"`；
②打开 owner-root 那四个 CONFIG；③CI 重编内核。

## 五、我没动的

`-uketextconsole` 我**没有**重新加回去 —— `board.toml:550` 是你为 b226 主动注释掉的
（"PARKED for the b226-card round"）。当前 kernel_args **50 条 / 966 字节**，余量 34 字节。
