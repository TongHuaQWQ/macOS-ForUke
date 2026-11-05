# uke 移植现状与下一步

这份文件是给**下一次会话**的交接。先读它，再读 `PROGRESS.md`（历史）和 `RECOVERY-LOG.md`（事故）。
最后更新：2026-11-06（第 b49 轮之后）

---

## 1. 目标与形态

Xiaomi Pad 7（uke，SM7675）上跑 macOS 26.5.2（25F84）：重编 XNU（4 KiB 页）+
把 Apple mac17g 的 kext 用 `XnuPrelink` 重新链进自建 kernel collection。

```
fastboot flash vendor_boot_a <vendor_boot>.img   # payload（ADT + 文件系统 + UEFI FV）
fastboot boot <boot>.img                         # 内核 + 同一份 FV 的引导入口
```

引导链：EDK2（DxeCore→BDS）→ `UniXNU.efi` → `DarwinBoot`（FV 内）→ XNU @ EL1。
DarwinBoot 打印 `DARWINBOOT: hand-off to the kernel` 即交接成功。

---

## 2. 已经证实的事实（带证据，别再重测）

| 事实 | 证据 |
|---|---|
| 内核能编译、重链、跑到 kext 启动 | 屏幕上 `security policy loaded: AppleMobileFileIntegrity` |
| **定时器从未触发过** | `uke-stall-probe: samples=0 last_pc=0 cpus=0`（每一轮都一样） |
| **`ArmCpu::start()` 从未被调用** | b47：ArmCpu 打完诊断会 panic，实物**没有** panic，也没有任何 `ArmCpu:` 行 |
| GIC 在 EL1 手上、可读写 | `GICD_CTLR … GICD_CTLR accepts writes`；`GICD_PIDR2 0x30`（GICv3） |
| redistributor 步长 **0x40000** | 我们的探针 `gicr +0x00000/0x40000/…/0x1C0000`；Linux dmesg `CPU0 0x17180000 / CPU1 0x171c0000` |
| 启动核是 affinity 0（slot 0） | `MPIDR_EL1 0x81000000 → slot 0x0`；LINUX `CPU0: found redistributor 0` |
| 本核那一帧交接时 `ProcessorSleep=1` | `gicr +0x00000 … waker 00000002 asleep 0 psleep 1` |
| 定时器：19.2 MHz，虚拟定时器 PPI 27 | Linux `arch_timer: cp15 and mmio timer(s) running at 19.20MHz (virt/virt)` |
| **板级组件在内核集合里没有 personality** | 132 个 IOClass 全部是 Apple 的；`org.blu-sharky.ArmCpu` 出现 0 次 |
| 但 `KernelConfigTables` 里**有**它们 | 核心里有 `"IOClass' = ArmCpu;"` / `= ArmDTPlatform;`，无 `#if` 守卫 |
| `/chosen` 里补 nonce/security 属性**无效** | b46：仍然 14 行一模一样的 `failed to read nonce slot data: 2` |
| Android 保留区与我们的 DRAM 窗口重叠 | `probe-uke-android2.md`：`9f3e08000-9fd3fffff` 等落在 `0x8C0000000+` 窗口内 |

---

## 3. 未解问题（按优先级）

1. **定时器不跳** → 一切带超时的等待都变成永久等待，这是 Image4 之后"安静停住"的最可能原因。
2. **板级驱动不启动**（`ArmCpu`/`ArmDTPlatform` 都不跑）→ 平台专家那一层没被 IOKit 启动；
   personality 明明在 `KernelConfigTables` 里，所以疑点在匹配/实例化，或 `__PRELINK_INFO` 的加载。
3. **Image4 读不到 nonce slot**（14 行）→ 已排除 `/chosen` 属性；`AppleImage4` 有一张 128 槽 × 40 字节的表，
   每个 magazine 读一次；真机 nonce 来自 SEP/NVRAM（Apple DT 里是 `syscfg/RCHL.boot-nonce`）。
4. **日志看不见** → 面板只放约 14 行，Image4 自己就占满；持久化需要 SoC 的跨复位持久页，**不是普通 DRAM**。

---

## 4. 正在进行的工作（先看这里）

- CI 提交 `e4e6ff1`：**重新启用 `patches/xnu/uke-gic-timer.patch`** —— 不从驱动、而是在核心里直接
  映射 GIC、唤醒本核 redistributor、把 PPI 27 放进 **group 1（IRQ，XNU 的 `sleh_irq` 会 EOI）**、
  再派发给 `rtclock_intr`。`uke-armcpu-probe.patch` 暂时挂起（两者改同样三个文件，上下文冲突）。
- 判定行：`uke-stall-probe: samples=?`
  - 非 0 → 定时器通了（十几轮以来第一次突破）
  - 0 且有 `uke-gic: …` → 补丁自己会说卡在哪一步
  - 百万级 → 中断进来了但 EOI 不对
- 下一版 payload 要同时做的改动：**把 DRAM 窗口从 `0x13FE00000` 收到 `0x133000000`**（4.8 GiB），
  避开 `0x9F3E08000` 起的固件保留区；`[payload.adt] panic_region` 随之改到窗口顶部之下。

---

## 5. 工具与流程（照抄）

```sh
# 1. 触发内核编译（约 30–35 分钟）：改完推 ci-* 分支即可
cd work/push/macOS-ForUke && git push mine HEAD

# 2. 取产物
T=$(tr -d '[:space:]' < work/.gh-token)
curl -sS -H "Authorization: Bearer $T" \
  "https://api.github.com/repos/TongHuaQWQ/macOS-ForUke/actions/runs/<RUN>/artifacts"
./work/analysis/fetch-artifact.sh <ARTIFACT_ID> <SIZE> work/ci-artifacts/b<N>-uke-kernel-min.zip 12
cd work/ci-artifacts && mkdir b<N> && cd b<N> && unzip -q ../b<N>-uke-kernel-min.zip

# 3. 打 payload（本地，约 6 分钟）。注意 kernel 必须放在"没有 kernel.macho.sha256"的目录，
#    或者把 sha 文件写对，否则脚本会 sha256 mismatch 拒绝。
O=work/push/UniXNU/out/uke/macos-26.5.2-25F84/uke-macos/boot
./tools/build-payload-linux.sh --core work/push/UniXNU --device work/push/macOS-ForUke \
  --uefi workspace/uefi-uke --kernel work/ci-artifacts/b<N>/kernel.macho --defconfig uke-macos_defconfig
cp $O/boot.img work/flash/boot-round-b<N>.img
cp $O/vendor_boot_uke.img work/flash/vendor_boot_round-b<N>.img

# 4. 固件（改 DarwinBoot 后必须重编，10 秒）
cd workspace/uefi-uke && ./.venv-edk2/bin/python3 build_uefi.py -d uke -r DEBUG

# 5. 只改 ADT/boot-arg（纯 payload 侧，不用 CI）：直接重跑第 3 步
```

其它工具：
- `work/analysis/symbolize-panic.py` — panic 地址 → 符号（带 `--disasm`）
- `work/analysis/add-codeless-kexts.py` — 往集合的 `_PRELINK_INFO` 加 codeless kext（已用过，无效但无害）
- `work/analysis/qemu-boot.sh` — QEMU virt 里跑同一份内核（`-device ramfb` 给 BGRA 的 GOP 才放行）
- `./tools/probe-uke.sh` — 从 Android 侧采集硬件事实（GIC/timer/iomem/持久区），已扩过段落
- `work/analysis/ref/mac17g.adt` — 解出来的 **Apple 真机设备树**，nonce/security 词汇表都在里面

---

## 6. 已踩过的坑（别再踩）

| 坑 | 现象 | 正确做法 |
|---|---|---|
| `feeds.toml` 的 `git` pin 对 core 自带子模块无效 | 连续两轮 CI 编的都是子模块版本，探针"没出现" | 板级副本 + `local = "drivers/<Name>"`（第 4 步解析，赢过子模块） |
| 面板只放 ~14 行 | 诊断被 Image4 输出顶掉，看起来像"驱动没跑" | 诊断完**主动 panic** 或长时间停留，把屏幕冻住 |
| 两个设备补丁改同样文件 | `patch does not apply`，编译 0 步失败 | 同一时间只启用其中之一，或先 rebase |
| EDK2 的 DEBUG 字符串是 **ASCII** | 用 utf-16 搜索会以为补丁没生效 | `DEBUG((…, "…"))` 是 CHAR8；`Print(L"…")` 才是 UTF-16 |
| macOS runner 的 `--kernel` 校验 sha | payload 构建报 `sha256 mismatch` | 把内核放到独立目录，或写一份匹配的 `.sha256` |
| 命令行上限 1000 字节 | 加参数会超 | 现在是 987 字节，加参数前先删等价长度的 |
| 普通 DRAM 不跨复位 | `PREVIOUS XNU LOG (pram=0 …)` 永远是 0 | 持久页在 SoC 特定低地址（kebab 是 `0x17F00FE0`；uke 候选 `0x9FE7FF000`，未验证） |
| CI 排队 | 一次只跑一个 macOS runner | 一次只推一个提交，别连推 |

---

## 7. 下一步建议（按顺序）

1. **等 `e4e6ff1` 的产物**，打 payload，刷入，看 `uke-stall-probe: samples`。
   - 若非 0 → 立刻回到 b43/b44 的 ArmCpu 探针（把 `uke-armcpu-probe.patch` 换回来），
     看 PPI 27/EOI 是否还需要 ArmCpu 参与；同时准备接 `ShengUfs` 拿持久日志。
2. **窗口收缩**：`dram_size → 0x133000000`，避开固件保留区（这是"踩固件内存"这个变量的唯一来源）。
3. **驱动启动**：如果定时器通了但仍要驱动，用 `IOService`/`OSKext` 侧取证（personality 已在
   `KernelConfigTables`，下一步看 `__PRELINK_INFO` 有没有被 OSKext 加载，以及根设备名字是否真是 `device-tree`）。
4. **Image4 nonce**：要么找到 Apple 期望的 nonce 表来源（bootloader/SEP 接口），要么用 `text_fixups`
   机制把"读失败"分支改成继续（注意：kext 在集合里，`text_fixups` 只覆盖 payload 卷里的文件，
   集合内文本补丁要另想办法）。
5. **日志**：验证 `0x9FE7FF000` 是否真的跨复位；若是，把它当 `/chosen/pram` 的区域即可拿到上一轮完整日志。

---

## 8. 更正（2026-11-06，三方交叉验证之后）

之前记录里有四条结论**站不住**，别再依据它们推理：

1. **"板级组件没有 personality 所以从不启动"——错。**
   它们由 `gIOKernelConfigTables`（`iokit/KernelConfigTables.cpp`）声明，由 `IOCatalogue` 消费，**不需要**在
   `__PRELINK_INFO` 里出现。实测 b47 内核：`__DATA_CONST,__mod_init_func` 有 99 项，其中
   `[97] = __GLOBAL__sub_I_ArmDTPlatform.cpp`、`[98] = __GLOBAL__sub_I_ArmCpu.cpp`，元类确实注册了。
   `org.blu-sharky.ArmCpu` 在 `__PRELINK_INFO` 里出现 0 次是**正确的**。
   （注意：`kernel.macho` 里还有一份**过期副本**的 `gIOKernelConfigTables`，在 fileoffset≈`0xe4c00` 的
   `__PRELINK_TEXT` 里、不属于任何 fileset entry；只按字符串 grep 会被它骗。活的副本在 kernel entry 内。）

2. **"`samples=0` 证明定时器不来"——是同义反复。**
   `uke-stall-probe` 的采样点就在 `rtclock_intr()` 内部：它是被它要检测的中断驱动的，中断不来必然是 0。
   同理 `uke-heartbeat` 用 `thread_call_enter_delayed(…, 2 s)`，没有定时器一次也不会触发。这两个仪器在
   定时器修好之前**测不出任何东西**。

3. **"ArmDTPlatform::configure 从未被调用"——那条证据不存在。**
   b47（`247f5ed`）里**没有** ArmDTPlatform 的 panic：那个探针是 `700928d` 才加的，随后被 `4bf15ac` 替换、
   在 `6070098` 里撤销。而且 `AppleImage4` 的 personality 是
   `IOProviderClass=IOPlatformDevice, IONameMatch='defaults'`，而 `/defaults` 只由
   `IODTPlatformExpert::configure` → `processTopLevel()` 发布 —— **Image4 能启动就说明平台专家跑到了那里**。

4. **"`loadable: false` 说明构建不完整"——它是写死的常量。**
   `kits/XnuPrelink/fileset_emit.py:737` 无条件写 `'loadable': False`。真正有意义的是 `missing`
   （b47：170 个被 stub 的函数 / 109 个未解析引用，`--allow-missing` 下正常产出）。

### 定时器的真正根因（已核实源码）

`pexpert/arm/pe_fiq.c:15` 的整个函数体在 `#if HAS_GIC_V3` 内，而 `HAS_GIC_V3` **只由**
`pexpert/pexpert/arm64/VMAPPLE.h:41` 定义。我们的板级头 `BCM2837.h` 没有它 → XNU 里唯一的 redistributor
唤醒 + PPI 27 使能代码被编成空函数。又因为 `CONFIG_XNU_NATIVE_ARCH_TIMER=y` 关掉了内核自己的定时器路由，
**唯一**的使能路径是 `ArmCpu::start()` → `enableCPUTimeBase()`。这就是"驱动不启动 ⇒ 定时器永远不来"
的耦合机制（不是两个独立问题）。

### 那次 800 kHz 风暴的真正原因（已定位到行）

`uke-gic-timer.patch` 里 `if (uke_gic_timer_irq(cdp->rtcPop != (uint64_t)-1))` 这个闸门：
`rtclock_init` 在编第一个截止时间之前把 `rtcPop` 设为 `EndOfAllTime`（全 1），闸门为 false →
只 EOI 不重新编程 `CNTV_TVAL` → EOImode 0 下电平线仍有效 → 立刻重进。
修复：默认模式改 2（在 gate 探针处、`rtclock_init` 之后使能）+ **无条件 pop**（`f9c74b5`）。

### 当前待验证的断点（b50 就是测它）

平台专家已经跑到 `processTopLevel`，所以断点在"CPU nub 发布"到 `ArmCpu::start()` 之间，两个候选：
- **R1**：`IODTPlatformExpert::configure`/`processTopLevel` 内部卡住（无任何日志）
- **R2**：`ArmCpu::probe` 静默拒绝了 nub —— 它的 personality **没有 `IONameMatch`**，`probe` 是唯一过滤器，
  而 `probe` 有五条无输出的 return nullptr（`ArmCpu.cpp:209-228`），最可疑的是
  `ml_get_cpu_number(reg)` 返回 -1（拓扑里没有该 `phys_id`）

b50 用 XNU 自带的匹配日志判定：`io=0x10` + `iokit_print_verbose_match_logs=1`（`IOKitDebug.cpp:50`、
`IOService.cpp:3845,4145-4170`）会为每个 nub 的每个候选打印 `matched <class>, probe score <n>`。
- 完全没有这行 → ArmCpu 从未成为候选（R1）
- 有 `matched ArmCpu, probe score 2000` 但没有驱动输出 → probe 拒绝了（R2）
- 出现 `Couldn't alloc class "ArmCpu"` → 元类注册有问题（与第 1 条矛盾，需重查）

---

## 9. QEMU 抓到的真实 panic（2026-11-06）

在 QEMU 里跑同一个内核（`-device ramfb` + `io=0x10`），**启动线程的 PC 在 120/240/420/600 秒恒为
`0xfffffff00b62d9b0`**，符号化结果：

```
_panic_spin_forever + 0xc
fffffff00b62d9ac:  wfe
fffffff00b62d9b0:  b 0xfffffff00b62d9ac     ← 死转
```

**内核不是卡住，是 panic 后停在自旋里。** panic 文本画在帧缓冲上（无锁 panic 屏，字符双写），
解出来是 `Panic(cpu 0 caller 0x…): …`，而核心里对应的字符串是：

```
panic: non-sensical crypto hash method: %s
"Invalid crypto hash method: %d" @%s:%d
IOReturn AppleMobileApNonce::_retrieveNonce(uint64_t *)
```

### 原因：我们的 ADT 缺 `crypto-hash-method`

Apple 真机 Mac17,2 的设备树里有：

```
/chosen:  … effective-security-mode-ap, crypto-hash-method = "sha2-384", secure-boot …
```

而我们的 ADT 里 **`crypto-hash-method` 出现 0 次、`sha2-384` 0 次**（生成器不产出、board.toml 也没写）。
Apple 的 nonce 路径读不到散列方法 → 判定"non-sensical"→ panic；这也解释了每个 magazine 都报
`failed to read nonce slot data`：没有散列方法，slot 内容根本无法解释。

第 b51 轮补上了 `[adt.chosen.string] crypto-hash-method = "sha2-384"`（payload 侧，已验收 ADT 内含该键）。

### 方法论教训（重要）

- 在 QEMU 里"串口没有输出"**不等于内核没跑** —— 内核把 panic 画在帧缓冲上，只有截图才看得到。
  之前几轮只看 `serial.log`，所以一直以为 QEMU 里内核"沉默"。
- `info registers` 的 PC 在多个时间点不变 + `PSTATE` 的 DAIF 全屏蔽，就是"panic 自旋"的签名。
- 台面上同样的 panic 屏幕被 Image4 的 14 行顶掉了，**QEMU 是唯一能稳定读到它的地方**。
  以后任何"设备上说不清"的问题，先在 QEMU 里跑一遍再刷机。

---

## 10. b51 结果：panic 换成了更深的一条（2026-11-06）

`crypto-hash-method = "sha2-384"` 生效 —— **`non-sensical crypto hash method` 的 panic 消失了**，
设备上出现新的 panic：

```
fileset: original component called an unmapped kernel function from 0xfffffff00979c38c (x0 0x8 x1 0x2)
```

反汇编定位（`symbolize-panic.py --artifact work/ci-artifacts/b47 --disasm`）：

```
0xfffffff00979c384:  mov  w0, #0x8
0xfffffff00979c388:  bl   0xfffffff00c09e690        ← 目标
0xfffffff00979c38c:  cbnz w0, …
```

- **调用方** `0xfffffff00979c38c`：`__TEXT_EXEC+0xe4838c`，**没有符号**（属于某个原版 Apple 组件）
- **目标** `0xfffffff00c09e690`：`_set_awl_scratch_exists_flag_and_subscribe_for_pm + 0xc8` —— **是内核自己的函数**
- prelink 记录里**这两个地址都没有 stub**：`stub_sites`（760 条）无匹配，`stubbed`/`missing`（各 22 个模块）也无匹配

解读：XNU 的 fileset 校验认为"某个原版组件调用了一个未映射的内核函数"。目标明明是内核符号，所以真正
可疑的是**调用方所在的 extent 归属**：prelink 重排后，那个组件登记的代码范围可能没有覆盖这个地址
（`kernel.json` 里没有 `extents` 字段，`fileset_emit.py` 的记录不含它，所以从产物这一侧查不到）。

### 下一步（顺序固定）

1. 找到这条 panic 的判定代码：字符串是 `fileset: original component called an unmapped kernel function
   from %p (x0 0x%llx x1 0x%llx)`，**不在 `out/feeds/xnu-3199e8bc` 里**（可能在 `libsa/`、某个 kext，或
   `native_prelink` 的补丁层），先确认它校验的是"调用方"还是"目标"。
2. 据此决定修在哪一侧：
   - 若是调用方 extent 缺失 → **prelink 的 extent 记账问题**（`fileset_emit.py` / `native_prelink.py`）
   - 若是目标需要 stub → 用 `stub_sites` 机制补上（工具已支持）
   - 若是 reviewed data anchor 缺失 → `fileset_match.py:457-473` 的 reviewed data 路径（当前 `reviewed data: 0`）
3. 定位调用方所属组件：`work/analysis/symbolize-panic.py` 查不到符号时，用 `kc_image.py` 遍历
   `LC_FILESET_ENTRY` 各自的 `__TEXT_EXEC` 范围，找包含 `0xfffffff00979c38c` 的那个 entry。

### 已知可用的判定手段

- **QEMU + `-device ramfb` + 截图**：能读到台面上被顶掉的 panic 文本（第 9 节）。
- `info registers`：PC 恒为 `_panic_spin_forever+0xc` = 已 panic；PC 变化但无输出 = 真卡住。
- `io=0x10 iokit_print_verbose_match_logs=1`：每个 nub 的候选驱动匹配日志（b50/b51 已带）。

---

## 11. 当前 panic 的真正性质：kernel 在"未匹配即 panic"模式下运行（2026-11-06）

b51 的 panic 来自 **UniXNU 自己的桩**，不是 XNU：`patches/xnu-12377/fileset-unmapped-call.patch` 定义的
`unixnu_fileset_unmapped()`，而 `emit_prelink.py:362` 说明它的启用条件：

```python
stub_symbol = '_unixnu_fileset_unmapped' if args.allow_missing else None
```

注释原文："**binds every unmatched function start to the panic stub**, so an unknown call reports its
site instead of jumping to a stale address"。

也就是说：板子一直用 `--allow-missing` 出内核，**任何一次调用未匹配上的内核函数都会 panic**。
b47 的记录：`stubbed functions: 170`，所以这个内核**结构上不可能走远**。

调用方定位（已确认）：panic 的返回地址 `0xfffffff00979c38c` 在
**`com.apple.driver.AppleMobileFileIntegrity`（AMFI）** 的 `__TEXT_EXEC`（`0xfffffff009788520 + 0x34964`）；
`bl` 的目标 `0xfffffff00c09e690` 是桩本身（不是真函数）。AMFI 有 29 处这种桩，`x0=0x8 x1=0x2`。

### 真正的修法：把这 120 个缺失函数绑上去

`work/analysis/missing-calls.py`（旧工具，一直没用）已经把范围算出来了：

```
1203 code call sites to 120 distinct missing kernel functions
  216 处  0xfffffe000bf3decc   ~ OSSymbol::withString+0xe4      [IOPCIFamily, IOGraphicsFamily, AppleARMPlatform]
  173 处  0xfffffe000bf72058   ~ IOMallocData+0x90              [apfs, AppleM2ScalerCSCDriver, AppleARMPlatform]
  137 处  0xfffffe000b9625f4   ~ lck_spin_assert+0xf8           [ApplePMGR, AppleARMPlatform, AppleImage4]
   83 处  0xfffffe000bd44284   ~ cs_blob_get+0xf8               [AppleMobileFileIntegrity, apfs, sandbox]
  …
```

完整清单：`work/analysis/ref/missing-calls-b47.txt`。

绑定机制**已存在**：`kits/XnuPrelink/fileset_match.py` 的 reviewed anchors，
fixture `fixtures/fileset-25F84-symbols.json`（正好是本系统版本）已绑 **60 个函数**，每条记录形如
`{original, original_size, original_sha256, native_symbol, evidence}`，由
`inputs.kernelcache_sha256` 精确匹配（我们的 kernelcache sha 与之一致：`697e01d6…`）。
`reviewed data: 0` 表示**数据**锚定一个都没有（AMFI 的 4 条 `missing` 正是数据地址，无法桩）。

### 下一轮的具体任务

1. 从 `missing-calls-b47.txt` 里取 top-N 缺失函数，用「original 字节 + 原生符号」补进 reviewed anchors
   （工具已支持，改的是 fixture JSON，不需要改 XnuPrelink 代码）。
2. 优先处理 **AMFI 会调到的**那几个（当前 panic 就是它），以及 `cs_blob_get` / `lck_spin_assert` 这些
   启动早期必经的路径。
3. 每轮的判据：`stubbed functions` 数下降；设备上 panic 的调用点换到下一个缺失函数。
4. 如果某些函数确实是源码分支差异（不是匹配失败），才考虑给它们写"安全返回"桩而不是 panic ——
   但默认先绑，别放宽语义。

---

## 12. 缺失函数的性质：被折叠掉的导出函数（2026-11-06，Round 2–3）

### 已排除的假设

**"关掉 LTO 就能让它们重新变独立"——不成立。** `configs/uke-macos_defconfig:63` 就是
`CONFIG_XNU_BUILD_LTO=y`，参照内核也用 `build-fileset-release-reference.py --lto`，
两边都是 LTO。`scripts/build-xnu-4k-baseline.sh:167-171` 的注释说明了原因：Apple 的
`kernelcache.release.*` 是 LTO 构建，非 LTO 重建在"Apple 内联过的地方"必然形状不同。

### 真实情况：邻居全在，只有中间那个不在

以 `work/analysis/ref/missing-calls-b47.txt` 的头几条为例，对照我们的 `kernel.map.json`：

| 缺失目标 | 邻居（都在 map 里 ✅） | map 里有没有 |
|---|---|---|
| `0xbf72058` (0x90) | `_IOMallocData` ✅ / `_IOFreeData` ✅ | `_IOMallocDataShareable` ✗ 不存在 |
| `0xb9625f4` (8) | `_lck_spin_assert` ✅ / `_IOSimpleLockTryLock` ✅ | 该 8 字节函数 ✗ |
| `0xbf72fa4` (0x48) | `_IORecursiveLockLock` ✅ | ✗ |
| `0xbd44284` (0x2c) | `_cs_blob_get` ✅ | ✗ |
| `0xbd82a88` (0x20) | `_proc_ucred_smr` ✅ / `_proc_ucred` ✅ | ✗ |

源码顺序也印证了身份：`iokit/Kernel/IOLib.cpp` 的定义顺序是
`IOMallocData_external`(1099) → **`IOMallocDataShareable_external`**(1117) → `IOFreeData`(1135)
→ `IOFreeDataShareable`(1141)。第一条缺失目标正好夹在 `IOMallocData` 与 `IOFreeData` 之间，
所以它就是 **`IOMallocDataShareable`**（我们这边被折叠成 IOMallocData 的别名，没有独立符号）。

### 修法（两条，按优先级）

1. **给这些名字补 out-of-line 实现**（设备补丁，最小改动）：在核心里为 top 缺失函数写同名包装，
   例如 `IOMallocDataShareable(size)` 直接转调 `IOMallocData(size)`。matcher 的 `reference-symbol`
   策略会按符号名绑定（b47 里已有 3561 个函数是这么配上的），于是调用落到真实实现而不是 panic 桩。
   优先顺序按 `missing-calls-b47.txt` 的 calls 列（216 / 173 / 137 / 126 / 83 …）。
2. 若某函数确实不该存在（语义上可以退化成失败），才考虑把它从 panic 桩改成"返回失败"，
   但这会放宽语义，默认不做。

### 下一步要用的工具链改动（已提交 `49a3e19`）

CI 现在会把 **native 内核**（`artifacts/native/native.macho`）一并上传。拿到之后：
`kits/XnuPrelink/fileset_match.py <kernelcache> <native> --out /tmp/pair.json` 可以在**本地**跑，
从而秒级验证"补了这个符号之后 stub 数是否下降"，不必每轮 35 分钟。

---

## 13. 结论：这些函数必须"补实现"，anchor 绑定走不通（Round 4）

写了本地形状比对工具 `work/analysis/find-unmatched-shape.py`：把原始函数的字节与 native 内核里
**同长度、同指令形状**（只有重定位会改写的指令——分支、adrp/adr、literal load——被掩码）的函数逐一比对。
索引了 16689 个 native 形状（`kernel.map.json` 的键是原始地址，native 地址在 `value['native']`，且 pre-slide）。

结果（`missing-calls-b47.txt` 的前几条）：

| 原始目标 | 尺寸 | 结果 |
|---|---|---|
| `0xbf3decc` (216 calls) | 0x190 | **no same-length masked match** |
| `0xbf72058` (173) | 0x90 | **no same-length masked match** |
| `0xb9625f4` (137) | 8 | ambiguous（形状太通用） |
| `0xbf72fa4` (126) | 0x48 | **no same-length masked match** |
| `0xbd44284` (83) | 0x2c | **no same-length masked match** |
| `0xbd82a88` (62) | 0x20 | **no same-length masked match** |

**判定**：这些原版函数在我们构建的 native 内核里没有"同长度同形状"的对应物 —— 不是位置/名字不同，
而是**根本没有独立的 out-of-line 版本**（被折叠）。所以 `anchor_reviewed` 没有可绑的目标。

### 于是修法确定为：写 shim

在设备补丁里给这些导出名补 out-of-line 实现（放进一个**已确定会被编译**的文件，例如
`iokit/Kernel/IOLib.cpp` 里紧挨 `IOMallocData_external`，避免改 `conf/files.*` 引入新的构建风险），
语义上直接转调非 shareable 版本：

```c
void *
IOMallocDataShareable(vm_size_t size)      /* 0xbf72058, 173 个调用点 */
{
	return IOMallocData(size);             /* 本板不做 shareable 分配 */
}
```

补上之后：native 与 reference 内核都有同名符号 → matcher 的 `anchor_reference`
（`fileset_match.py:195-214`，先由 shape 配对、再按**名字**转译）就能绑上；调用落到真实实现，
而不再是 panic 桩。每补一个，`kernel.json` 的 `stubbed functions` 计数应下降。

### 复现命令

```sh
python3 work/analysis/find-unmatched-shape.py --limit 20      # 形状比对，看还有没有可绑的
python3 work/analysis/missing-calls.py                        # 重新排缺失函数清单
```

`native.macho` 到手后，把 `--native` 指向它即可（现在默认用 b47 集合里的内核文本当替身）。

---

## 14. 更正 Round 4：那些函数是**存在**的，只是没被配对（Round 5）

### 我的错误

`kernel.map.json` 里的 `functions` **只列出已配对的函数**。我在 Round 4 把它当成"native 全部函数"建索引，
于是"找不到同形状匹配"是假象。

用 **native 内核自己的符号表**（b47 集合里 `com.apple.kernel` entry 的 `LC_SYMTAB`，**64216 个地址**，
而不是 map 里的 17868 个）按名字查，全都在：

```
_IOMallocData                        18446744005190144980
_IOMallocData_external               （同一个地址，别名）
_IOMallocDataShareable               18446744005190145028
_IOMallocDataShareable_external      （同一个地址，别名）
_IOMallocZeroData_external           18446744005190145004
_IOMallocZeroDataShareable_external  18446744005190145052
_IOFreeData / _IOFreeDataShareable   都在
```

### 但"同长度同形状"确实一个都没有

`work/analysis/find-unmatched-shape.py`（已改为索引 native **符号表**）对 top 缺失函数仍然全部
`no same-length masked match` —— 也就是说代码确实不同（编译差异），但函数**存在**。

`work/analysis/name-by-shape.py`（按掩码形状相似度在 64k 符号里排序）给出的部分候选：
- `0xbf3decc` (216 calls) → **0.58 `OSSymbol::withCString`**（与邻居 `withString`/`withCString` 一致）
- `0xbd82a88` (62) → 0.75 `_proc_task`
- 8 字节和 0xc 的那两个是通用形状，1.00 但无意义

### 结论：`reviewed` anchor 仍可写，但**名字需要实证**

anchor 只接受 `native_symbol` 名字，而 Apple 没有发布 macOS 26 的 KDK（workflow 注释：
"Apple publishes no KDK for macOS 26, so the stripped kernelcache has no symbols"），
所以名字只能靠源码顺序 + 形状相似度 + 调用目标推断，**不能瞎填**。

### 本地 matcher 的限制（已实测）

想用出厂集合当 native 跑 `fileset_match.py`：`functions: 0/23025` —— 集合的文本被 emitter
合并/重排过，形状全对不上。**所以必须用 CI 产物里的 native 内核**（`artifacts/native/native.macho`，
已在 `49a3e19` 加入上传）。拿到后：

```sh
python3 work/push/UniXNU/kits/XnuPrelink/fileset_match.py \
    work/push/UniXNU/out/ipsw/25F84/kernelcache.decompressed \
    work/ci-artifacts/<N>/native/native.macho --out /tmp/pair.json
```

复现 17868/23025 之后，就能在本地加 anchor、看 `stubbed functions` 是否下降。

---

## 15. 定名工具：调用图 + 字符串指纹（Round 6）

形状相似度在跨编译差异下不可靠，但**函数调用了谁、引用了哪些字符串**通常不变。
`work/analysis/fingerprint-missing.py` 的做法：解析每个缺失函数的全部 `bl` 目标（用已配对的名字解析）
和 `adrp+add` 字符串引用，然后在 native 侧按共享程度排序。

实测输出（前三条）：

| 原始目标 | 调用点数 | 指纹 |
|---|---|---|
| `0xbf3decc` | 216 | 字符串 **`amcc_rorgn_common.c`**、**`overflow in rorgn_contains: %p + %zu @%s:%d`**；被调 `OSSymbolPool::{insert,find}Symbol`、`_panic`、`_strnlen` |
| `0xbf72058` | 173 | 被调 `_IORWLockUnlock`、`IOStatistics::getKextNodeFromBacktrace`、`_kalloc_ext`；最佳 native 候选 `_IOMalloc_internal` / `_IOMallocAligned_internal` / `_IOKernelAllocateWithPhysicalRestrict`（各 3 个共享被调） |
| `0xb9625f4` | 137 | 8 字节，无被调、无字符串（纯叶子，形状通用） |

**解读**：
- `0xbf3decc` 是 Apple **AMCC 受限区域**的检查函数（`rorgn_contains`，见字符串），被 216 处调用 ——
  它一定是启动路径上的公共校验入口，优先处理。
- `0xbf72058` 是 **IOMalloc 家族的统计包装**（`getKextNodeFromBacktrace` 只在 IOStatistics 打开时出现），
  与源码顺序（`IOMallocData_external` → `IOMallocZeroData_external` → `IOMallocDataShareable_external`
  → `IOMallocZeroDataShareable_external`）吻合，但**具体是哪一个还需要一步区分**
  （零化版本应引用 `bzero`/`memset`，shareable 版本传 `KHEAP_DATA_SHARED`）。

**注意**：`missing-calls.py` 给出的 `size` 是"到下一个已知函数的距离"，可能跨过多个函数，
所以指纹里出现混合被调是正常的；写 anchor 前要按实际边界切分。

### 下一步（拿到 native.macho 后）

1. 用 `fingerprint-missing.py` 对全部 120 个缺失函数出指纹表；
2. 对每个缺失函数，在 native 侧找出**共享被调 ≥2 且字符串命中**的**唯一**符号 → 写 `reviewed` anchor；
3. 本地跑 `fileset_match.py` 验证 `stubbed functions` 从 170 下降；
4. 剩下的（如纯叶子函数）单独处理。
