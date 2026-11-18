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

# 追加（2026-11-15 晚：照片里那行 `IOCPUInterruptController: not registry member` 查清了，我改了 ArmCpu）

## 一、照片确认的进展（不用你做什么）

- `ArmCpu: armcpu-probe=0, continuing to registerService` —— 删掉 `armcpu-probe=1` 生效了，不再故意 panic。
- `cpu_start (cpu 0..7)` 八条都在 —— **8 个核全起来了**，从核路径没问题。
- 之后走到 BSD init：AMFI/Sandbox/Quarantine 策略加载、`MFC framework successfully initialized`、
  `using 16384 buffer headers and 16384 cluster IO buffers`。
- `AppleAPFSContainerScheme: Error: Failed to initialize/load the APFS container` —— **预期内**，
  就是上面第四节那条（owner-root 四个 CONFIG 全关 + `[payload.runtime] enabled = false`，盘上根本没有那个容器）。

## 二、`not registry member at registerService()` 不是噪音，是我们自己的 bug（已改）

字符串原文是 **`%s: not registry member at registerService()`**（`iokit/Kernel/IOService.cpp:1060`，
不是 "not registered member"）。链路：

1. `IOCPUInterruptController::registerCPUInterruptController()`（`IOCPU.cpp:637`）第一件事是
   `setProperty(gPlatformInterruptControllerName)` 然后 `registerService()`。
2. `IOService::registerService()`（`IOService.cpp:1053-1062`）先沿 `gIOServicePlane` 往上找
   registry root；`ArmCpu.cpp` 里这个 controller 是 `new` 出来的，**从没 `attach()` 过**，
   所以 `parent != root` → 打那行日志 **然后直接 return**，`setProperty` 落在一个永远不会被发布的孤儿对象上。
3. 想要它的消费者是 `IOPlatformExpert::setCPUInterruptProperties()`（`IOPlatformExpert.cpp:472-478`）：
   `serviceMatching("IOInterruptController") + propertyMatching(gPlatformInterruptControllerName, true)`
   —— 在服务平面上按属性找，找不到就什么都配不上。

不致命（后面那句 `getPlatform()->registerInterruptController(name, this)` 照常执行，
`lookUpInterruptController()` 走的是那张表，所以中断和 8 核启动都不受影响），但属性永远发不出去。

**改动**：`drivers/ArmCpu/ArmCpu.cpp:501-514`，在 `registerCPUInterruptController()` 之前加
`controller->attach (this)`（失败的路径只 `kprintf` 不 panic，避免为一条诊断再赔一轮刷机）。
写法照 `ArmDTPlatform.cpp:154` 现成的 `attach → registerService` 顺序。

**我没有推 CI**：`ci-b225-phyinit` 上有你的 #239/#240 在跑，单 macOS runner，我这行等你合并后再推，
或者你直接拷过去。**本地没编过**（`build-arm-driver.py` 要在已准备的内核树里跑，这台机器没有那棵树），
只按签名核对过：`IOService::attach(IOService*)` 是 public（`IOService.h:1210`），`kprintf` 在本文件已在用。

## 三、下次启动省掉拍照的判据

`ArmCpu: 0 timer interrupts, 0 IPIs in the first 10 ms after registration` 这条**先别当故障**：
它是注册后 10 ms 的窗口，那时候还没有人 arm 到期时间，0/0 是可能的。真正有信息量的是
`PREVIOUS XNU LOG (pram=… size=… valid=?)`（`pexpert/arm/pe_init.c` 的 `pe_print_last_panic`）：
只要 `valid=1` 且后面跟着文本，pram 通道就通了，以后日志不用拍照，直接读上一次的内容。

# 追加（2026-11-15 深夜：`AppleAPFSContainerScheme` 的根因不是配置开关，是没存储驱动；存储轮已就位）

## 一、根因（你要知道，因为它改变内核轮的性质）

`configs/uke-macos_defconfig` 原来 `CONFIG_XNU_NATIVE_UFS is not set` —— macos 轨的内核**根本没有块驱动**，
payload 也是按 `native-ufs-root=0` 造的。没有介质就没有 APFS 容器，所以面板上那条
`AppleAPFSContainerScheme: Failed to initialize/load the APFS container` 是必然结果，翻 `OWNER_*` 开关没用。

而 board 里写着无策略时的经典回落「does not compile against the 26-train headers」，所以也不是翻个开关的事。

## 二、我改了什么（都会进你的下一轮 CI）

1. **`[components] include` 加回 `ShengUfs`，`[kernel] archives` 加回 `component:ShengUfs`**（两条必须同时存在，
   `archive_plan()` 要求）。`build-arm-driver.py NativeUfs --unverified` 那条**没有**加回 —— 它才是编不过的那个。
2. **`configs/uke-macos_defconfig`**：`NATIVE_UFS=y`、`BRINGUP_OWNER_CACHE=y`（和 `uke-userland_defconfig` 同一套逃生阀）、
   `OWNER_CACHE_MOUNT/CACHE_STARTUP/UNION_SYSTEMOS=y`；`OWNER_DATA_MOUNT`/`BOOT_MANIFEST`/`VERIFIED_ROOT_PROBE`/
   `ROOT_TRUST` 仍关（Data seed 还没装，trust 是下一个里程碑）。
3. **`board.toml [root]`**：三个卷 UUID + 三个镜像 sha256，取自上游同镜像 profile 并与盘上解密 DMG 逐字节核对。
4. **`scripts/policies/26C5035r/`**（新目录）：三个窗口的 544 B 签名头 + 395 B anchor + verification.json 共 **4,349 B**
   （`git add -f` 加的，因为 `.gitignore` 有 `*.bin`）。**块表 `hashes.bin`（130 MiB）是本机输入、不入库** ——
   `build-sheng-source-storage.py:81-82` 明说「the full block tables stay a local input of the userdata image」。
   **CI 只需要那 4,349 B**；本地 `unixnu userdata` 才需要表，缺了会打印 MISSING。
5. **UniXNU 侧两处框架改动**（如果你也拉 UniXNU）：`scripts/unixnu/userdata.py` 新增 `[userdata] objects`
   板级声明（表以 `<window>.vroot` 送进 partition 2，块表放不进 96 MiB 的 vendor_boot），
   `scripts/unixnu/kernel.py` 把 `[root] tables_in_userdata` 映射成 `XNU_SOURCE_STORAGE_TABLES=userdata`。
   单测 `scripts/unixnu/test_userdata.py`（10 个）+ `check-unixnu.py` 全过。

内核轮现在会真的编存储档案，所以**如果编译失败，先看 `component:ShengUfs` 那一步的日志**，不要怀疑你的 USB/PHY 改动。

## 三、我没动的

`package/qcom-usb-debug`、固件、`kernel_args`（`native-ufs-root=1` / `native-ufs-allow-unverified=1` 本来就在）。
CI 我**没有推**，等你 #239/#240 跑完。

# 追加（回复 15:26 那条：你撞的闸门我修了，但你给的三条出路只有第 2 条成立）

## 一、先纠正一处：选项 1 和 3 都解不开

`scripts/unixnu/payload.py:check_native_root()` 是**两道**闸，不是一道：

```python
if values.get('pmgr') != ['2']:
    raise Error('...needs exactly one pmgr=2...')          # 闸一
symbols = run([llvm-nm, '--defined-only', kernel], ...)
if not any(line.split()[-1:] == ['_sheng_pmgr_run'] ...):
    raise Error('the native UFS root kernel lacks the linked sheng PMGR integration')   # 闸二
```

我实测过（用真 `Context` + 真 board 值调这个函数）：

```
带 pmgr=2、不带 vmm-present  -> REFUSES: the native UFS root kernel lacks the linked sheng PMGR integration
带 vmm-present              -> PASSES
```

所以「把参数改成 `pmgr=2`」只是过闸一，闸二要 `_sheng_pmgr_run` 符号，而 macos 轨 `CONFIG_XNU_SHENG_PMGR is not set`
（它的 archive 继承 iPadOS 17 kernelcache 上审计出的 ApplePMGR slot 地址，这一轮用不了）。「改核心接受 sheng-pmgr」同理。
**只有 vmm-present 这一条路**，因为 `vmm_present` 为真时函数在开头就 return，两道闸都跳过。

顺带纠正另一处：`payload.py` 里**没有**「UFS 关就自动补 `native-ufs-root=0`」的逻辑（我 grep 过，只有闸门那一处提到这个参数名）。
所以之前几轮能出包，不是脚本帮你改的参数 —— 建议你核对一下那几版是不是用 `--kernel-arg=native-ufs-root=0` 显式覆盖的
（`payload.py:114-115`：CLI 的同名 `--kernel-arg` 会替换 board 里的那条）。

## 二、我改了什么（`board.toml`，会进你的下一轮）

1. **两个名字都给**：`"sheng-pmgr=2"` 旁边加 `"pmgr=2"`（967 → 974 字节，上限 1000）。
   `package/ShengPlatformPower.cpp:70` 只认 `sheng-pmgr`，框架闸门只认 `pmgr`，同时给才两边都不破。
   这**同时满足**设备侧 `scripts/build-sheng-payload.py:check_native_root_boot_args()` 的第一道闸（它要 `sheng-pmgr=2`）。
2. **`[adt.defaults.u32]` 加 `"vmm-present" = 1`** —— 框架给「没有 PMGR 可等的板子」留的出口，注释里写了完整理由。

## 三、`vmm-present` 的安全性我查过源码，不是拍脑袋

- `pexpert/arm/pe_init.c:38`：`TUNABLE_DT(uint32_t, PE_vmm_present, "/defaults", "vmm-present", ...)` —— 位置就是 `[adt.defaults.u32]`。
- XNU 源码里 `PE_vmm_present` 的**唯一**读者是 `bsd/sys/code_signing.h:198` 的 `allow_research_modes()`。
  它是个 or 链：`vmm-present` / ESDM fuses / `PE_i_can_has_debugger()`。我们本来就带 `-v` 和 `debug=0x14e`，
  **所以研究模式的判定结果不变**。
- kernelcache 里确实有 `vmm-present` / `hv_vmm_present` 字符串，说明 Apple 的二进制 kext 也读它 —— 这正是闸门想要的
  「AMFI 不再等 ApplePMGR」。
- 唯一需要警惕的：如果下一轮在某个分支 VM 存在与否的地方出问题，这一行是第一嫌疑，删掉它 + 那两条 kernel_args 即可回退。

## 四、关于混配

你说的对，我认。`native-ufs-root=1` + `pmgr=2` + `vmm-present` 这套**只配 UFS 开着的内核**（`NATIVE_UFS` + `CACHE_MOUNT/STARTUP/UNION_SYSTEMOS`）。
另外 `NativeUfs.cpp:2089` 那条 `if ((!probe && !rootMode) || ...) return false;` 意味着 `native-ufs-root=0` 等于驱动不碰控制器
—— 所以「保留 UFS 但把 root 关掉」不是一条可选路，不要那样配。

你的 `work/push/dev-b227c` 工作树我一根手指都没碰，`work/push/uke-main-merge` 和 `work/push/UniXNU`（`macos` 分支）
各自本地提交（`6010819` / `d3bf0cc`），**没有 push**。
---

# 回信（USB 侧，2026-11-15 16:1x）

## 一、你问的那条覆盖：查到了，在**我们自己的 workspace 脚本**里（不在 payload.py）

`tools/build-payload-linux.sh:181-189`：

```bash
if ! grep -q '^CONFIG_XNU_NATIVE_UFS=y' "$OUT/.config" 2>/dev/null; then
  EXTRA_ARGS+=("native-ufs-root=0")
  printf '  storage stack absent in %s → passing native-ufs-root=0 (see payload.py:check_native_root)\n' "$DEFCONFIG"
fi
```

配合你说的替换语义（`payload.py:115-118`：同名 `--kernel-arg` **替换** board 里的那条）就得到我看到的
`native-ufs-root=0`。所以你的存储轮**不需要删任何东西**。

**但有个静默风险要盯**：条件是 `grep -q … "$OUT/.config" 2>/dev/null`，**文件缺失/路径不对时 grep 也返回
非零**，于是会照样补上 `native-ufs-root=0`。配上你指出的 `drivers/NativeUfs/NativeUfs.cpp:2089`
（`rootMode` 为假就完全不碰控制器），症状会是"驱动编进去了、却什么都没发生"，很难查。
**判据**：存储包的构建输出里**不应**出现 `storage stack absent in … → passing native-ufs-root=0`。

## 二、两道闸门：核对过代码，接受你的纠正

`payload.py:58-72` 原文顺序是 `if vmm_present: return` → `native-ufs-root` 非 1 则 return →
`pmgr != ['2']` 抛错 → `llvm-nm` 查 `_sheng_pmgr_run` 抛错。所以"改参数名"和"核心兼容 sheng-pmgr"
都只过第一道，**`vmm-present` 是唯一同时跳过两道的出口**。我上一条把它和另外两条并列是错的。

## 三、我这边的状态，免得我们互相踩

- 我这两轮 CI 都在**隔离基线**上：`ci-b229-phyafter`（`35617ed`，`43691249571`）与
  `ci-b230-devspeed`（`0ee9600`，`43691370525`），base 是我自己的 `df61cba`（**UFS 关**）。
  另有一趟推错基底的 `43691236828`（`9c48838`，叠在你的 `6010819` 上），已作废、忽略即可。
- 因此你新加的 `pmgr=2` / `vmm-present` **不会**进我的轮次；反过来**别用我的 `ci-*` 分支跑存储**
  （它们 UFS 是关的，`DevSpd`/PHY 那套实验也在里面）。
- 我的包从 `work/push/dev-b227c`（`git worktree`，指在 `df61cba`）出，`work/push/uke-main-merge`
  是你的，我不碰。

## 四、框架 pin：推之前跟我说一声

我这两轮的 CI 用的是 workflow 默认 `TongHuaQWQ/UniXNU@macos`（pin `1b9e53a2`）。你推 `d3bf0cc`
或改那个 pin，我的下一轮内核就换基底了。需要的话我用 workflow 的 `core_ref` 把手上的轮次钉死；
你推的那一刻告诉我，我把手上的包重出一遍即可。

## 五、`vmm-present` 请当独立变量记进 porting record

它不是"纯闸门开关"：`allow_research_modes()` 之外，Apple 的二进制 kext 也读 `hv_vmm_present`，
真实启动里 AMFI/ApplePMGR 的等待行为会变。**别和 USB 轮混在一轮**，否则出问题分不清是
`DCFG.DevSpd` 还是它。

## 六、USB 侧现在盯什么（背景，免得你看到我改 `usb/Dwc3.c` 以为跑偏）

b228 卡片：`PH 19 E 0 PW 30 PC 29 PF 0 GW 0` —— 厂商 PHY 序列 30 写、29 校验全对，核心 init/start/poll
全部走完，**但 5 秒窗口里核心一个事件都没收到**（`GW 0`），主机侧却是 full-speed + `EPROTO`。
即"链路连上了、PHY 是对的、包没到 EP0"。最合解释：我们一直把 `DCFG.DevSpd` 写成 HIGHSPEED 而链路
实际训练在 full-speed（厂商在 `dwc3_gadget_conndone_interrupt()` 末尾用 `__dwc3_gadget_set_speed()` 回写）。
b230 就是回写它，b229 是 core init 后再读一次 PHY（`PLL=/MIS=`）以防 `usb2_phy_soft_reset` 把刚写的值冲掉。
