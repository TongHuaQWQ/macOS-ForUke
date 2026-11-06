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

---

## 16. 分类与定名的实证结果（Round 7）

### 工具

- `work/analysis/fingerprint-missing.py` — 调用图 + 字符串指纹，对全部 120 个缺失函数出表
  （结果：`work/analysis/ref/fingerprints-b47.txt`）
- `work/analysis/classify-missing.py` — 用"函数自己字符串里的 `__func__` 名字"在 native 符号表里查，
  把缺失函数分成"可 anchor"和"我们没编"
  （结果：`work/analysis/ref/classify-b47.json`）

### 结果一：只有少数能用字符串定名

```
0xb9fcb70  (10 calls) → _vnode_hold                          ✅ 可 anchor
0xc034c3c  ( 8 calls) → _ml_set_interrupts_enabled_with_debug ✅ 可 anchor
```

调用量最大的六个（216 / 173 / 137 / 126 / 83 / 62）**完全没有字符串** —— 小函数没有 `__func__` 消息。

### 结果二：`rorgn_contains` 是"我们根本没编"

`0xbf3decc`（**216 个调用点，13 个 kext**）引用的字符串是 `amcc_rorgn_common.c` 和
`"overflow in rorgn_contains: %p + %zu @%s:%d"`，而 native 符号表里 **`rorgn` 相关符号一个都没有**。
它是 Apple AMCC 的受限区域检查，被我们的配置门控掉了 —— 这类只能补实现（或打开对应配置），
anchor 无解。

### 结果三：IOMalloc 家族可以用"立即数常量"区分

`movz w2, #imm` 就是 flags：`4096` = `Z_VM_TAG_BT_BIT`（非零化）、`4100` = `Z_ZERO_VM_TAG_BT_BIT`（零化）。
原生四个变体的常量已核对，而原始函数：

```
0xbf72058 (0x90, 173 calls): movz w2, #4100     → 是 IOMallocZeroData* 家族（零化）
0xbf720e8 (0x120):           #4096 和 #4100     → 包装了两个变体
```

差一步：private 与 shareable 只差 `w0` 的常量（`KHEAP_DATA_PRIVATE` / `KHEAP_DATA_SHARED`），
我的常量提取只抓了 `movz w2`，`w0` 用的是别名 `mov` 形式，需要补齐掩码。

### 下一轮

1. 补齐 `w0` 常量提取 → 定死 `IOMallocZeroData_external` vs `IOMallocZeroDataShareable_external`；
2. 把已确认的两条（`vnode_hold`、`ml_set_interrupts_enabled_with_debug`）先写成 `reviewed` anchor，
   验证工具链能接受；
3. 对"我们没编"的那一堆（`rorgn_contains` 等）按调用量排序，逐个决定"补实现"还是"开配置"。

---

## 17. "USB 读日志"的可行性：硬件层没有驱动（Round 9）

用户把当前目标改成"先让日志能通过 USB 读回来"。核查结论：

| 环节 | 现状 |
|---|---|
| **UEFI（我们控制）** | FV 里**没有任何 USB 主机驱动**。`uke.dsc`/`uke.fdf` 的 `[Components]` 里没有 UsbBus/UsbMassStorage/Fat；树里也没有 Qualcomm 的 `Dwc3`/`UsbConfig`/`Xhci` 驱动，更没有 `QcomPkg`。EDK2 的通用 USB 栈必须有 SoC 的 HC 驱动才能跑。 |
| **XNU（我们移植的）** | 没有 USB 组件。AppleUSB 栈不支持 Qualcomm DWC3。 |
| **ABL（bootloader）** | 只有 fastboot 协议：能写、能 boot，标准命令**不能读内存**。 |

也就是说：**当前固件与内核都无法跟这个 USB 口通信**，唯一能通的是 bootloader 的 fastboot。

### 但有一条便宜的实测路径（值得先试）

Qualcomm ABL 常带 vendor 命令，**如果支持内存 dump，就是真正的 USB 读日志**。请在设备处于 fastboot 时跑：

```
fastboot oem help
fastboot getvar all
```

把输出发我。若出现 `dump`/`readmem`/`oem read` 之类，我们就能：内核把控制台环形缓冲写到固定物理地址 →
panic 后 `fastboot oem dump <addr> <len>` 取回。这条路成本最低、收益最大。

### 同时：本地的"无拍照全量日志"其实已经可用

QEMU 里同一个内核能跑，而且 QEMU 能**直接读 guest 内存**：

```
(qemu) pmemsave <addr> <size> file.bin        # 例如把 XNU 的 msgbuf/控制台环拷出来
(qemu) screendump …                          # panic 文本（已在用，读到了 non-sensical crypto hash method）
```

所以"内核日志不靠拍照"这件事，**在本地已经成立**；缺的是把它变成常规流程（下一轮做成脚本）。

### 结论

- 若 `fastboot oem help` 有内存读能力 → 优先做"内核写固定地址 + fastboot 取回"。
- 否则 USB 读日志需要先有 SoC 的 USB 驱动（UEFI 的 DWC3 host，或 XNU 的 DWC3），属于新项目；
  在此之前，**QEMU 是本地的日志通道**（已可用），设备上仍靠 panic 冻屏拍照。

---

## 18. USB 日志：厂商固件里有完整 USB 主栈（Round 10）

### fastboot 路线关闭

```
fastboot oem help   → FAILED (remote: 'unknown command')
```
ABL **没有任何 OEM 命令**，所以"内存 dump 经 fastboot 取回"这条路不存在。

`getvar all` 里值得记的事实：分区表完整（`boot_a/b`、`vendor_boot_a/b` 各 0x6000000、`userdata` f2fs、
`super` 0x2A0000000、`logfs` 0x800000、`oops` 0x1000000、`rawdump` 0x12C00000），`kernel:uefi`、
`is-userspace:no`、`max-download-size:805306368`（768 MiB）。

### 但设备备份里有厂商 UEFI，而它带着 USB 驱动

`work/tablet-backup-20261103/images/{uefi_a,imagefv_a}.img` 是 **ELF 包着 FV**（`_FVH` 在 0x1028，
即 FV 起始 0x1000）。用 `uefi_firmware`（venv 在 `work/analysis/.venv-fw`）解析 `uefi_a.img`
（968 个对象），发现：

| 驱动 | 体积 | 作用 |
|---|---|---|
| **Dwc3** | 98,630 B | Qualcomm USB **主机控制器**驱动 |
| **UsbConfig** | 53,322 / 90,258 / 28,726 B | USB 配置（角色切换、PHY） |
| **FatDxe** | 61,456 B | FAT 文件系统驱动 |

已抽出到 `work/analysis/ref/vendor-usb/`（含 `index.json`）。

### 计划（下一轮）

1. 把这 5 个 FFS 文件加进我们的 `uke.fdf`/`uke.dsc`（或用 VTF 方式追加到 FV）；
2. 加 EDK2 通用的 `UsbBusDxe` + `UsbMassStorageDxe`（树里都有）+ `Fat` 的 library 依赖；
3. 启动后看 UEFI 是否出现 `SimpleFileSystem` 句柄（DarwinBoot 里可以枚举并打印）；
4. 若出现 → DarwinBoot 把日志写到 U 盘上的 `uke-log.txt`，**这台设备从此有了可回读的日志通道**；
5. 若 Dwc3 依赖 QcomPkg 的协议/PCD 而我们的 FV 没有，就只加 `UsbBusDxe`+`UsbMassStorage`+`Fat`，
   看是否能借厂商 Dwc3 的 `EFI_USB2_HC_PROTOCOL` 实现（它需要 `gQcomTokenSpaceGuid` 等，风险点在这里）。

### 顺带：本地的"无拍照日志"已跑通一半

`work/analysis/qemu-dump-log.sh`：QEMU 里 `pmemsave 0x48000000 0x20000000`（内核装在 pa 0x48004000）
把 512 MB 内核数据拷出来再提取可打印串。实测 dump 已成功写出（536,870,912 字节），
下一步只需在提取阶段按 XNU 的 msgbuf 布局切分（现在是无差别提取可打印 run）。

---

## 19. 抽取进度与下一步（Round 11）

已确认（见 §18）：厂商 `uefi_a.img` 里有 `Dwc3`(98,630 B)、`UsbConfig`(53,322 / 90,258 / 28,726 B)、
`FatDxe`(61,456 B)，内容搜索命中。

**但抽出来的 5 个 `.ffs` 目前是 section payload，不是独立可用的 FFS 文件**：
`uefi_firmware.AutoParser` 对它们返回 `unknown`，说明缺 FFS 头；库的 `attrs` 里也没有 offset，
无法直接按偏移切分原镜像。

### 下一轮的正确做法（按顺序）

1. **自己写 FV→FFS→section 游走器**（约 60 行，FFS2 头 24 字节 / FFS3 头 32 字节，按
   `attributes` 的 `FFS_ATTRIB_LARGE_FILE` 位选择），把每个目标驱动的**完整 FFS 字节**切出来；
2. 解出其中的 **DEPEX 段** 并打印协议 GUID —— 这决定"我们的 FV 里会不会 dispatch"：
   - 若只依赖通用协议（`EFI_PCI_IO`/`EFI_USB2_HC`/`EFI_SIMPLE_FILE_SYSTEM` 等）→ 直接加 FV 即可；
   - 若依赖 `gQcomTokenSpaceGuid`/QcomPkg 的私有协议 → 需要从 `imagefv_a.img` 里连依赖一起搬，
     或者放弃厂商驱动、只加 EDK2 通用的 `UsbBusDxe`+`UsbMassStorageDxe`（但没有 HC 驱动，USB 起不来）；
3. 加 FV 的方式有两种：把切出的 FFS 追加进 FV 的自由空间（需重算 FV header 的 free space 与 checksum），
   或按 EDK2 的 binary INF 流程（`[Binaries] PE32|xxx.efi|DEBUG` + `[Depex]`）纳入 DSC/FDF；
4. 启动后在 DarwinBoot 里枚举 `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` 句柄并打印数量 —— 这就是
   "U 盘是否可见"的判据，不需要真的写文件就能确认通道成立。

### 结论

USB 日志这条路是**可行的但需要一轮工程**（FV 解析 + DEPEX 判定 + 集成）。在它打通之前，
`work/analysis/qemu-dump-log.sh`（QEMU + `pmemsave`）是本地的日志通道，实测 dump 已成功。

---

## 20. 厂商驱动已抽成 PE32（含 UFS 驱动）— Round 12

用 `uefi-firmware-parser`（venv `work/analysis/.venv-fw`）先把 `uefi_a.img` 的 ELF 外壳剥掉
（`_FVH - 0x28` 起 5,238,784 字节 → `work/analysis/ref/vendor-usb/uefi-fv.bin`），
再 `-e -O` 递归导出（98 个 `.pe`），按内容筛出：

| 驱动 | 体积 | 用途 |
|---|---|---|
| **`pe-Dwc3-98304.efi`** | 98,304 B | Qualcomm USB **主机控制器**驱动 |
| `pe-UsbConfig-{28672,53248,90112}.efi` | 28/52/88 KB | USB 配置（角色/PHY） |
| `pe-Fat-{57344,61440,131072}.efi` | 56/60/128 KB | FAT 文件系统 |
| **`pe-Ufs-147456.efi`** | 147,456 B | **UFS 存储驱动**（意外收获） |

`work/analysis/.venv-fw/bin/uefi-firmware-parser` 是可复现的抽取工具；导出目录是
`work/analysis/ref/vendor-usb/uefi-fv.bin_output/`（保留完整 volume/file/section 层级）。

### 为什么 UFS 驱动是意外收获

如果厂商的 UFS DXE 能在我们的 FV 里 dispatch，那 **UEFI 阶段就能读分区**，于是：
- 内核把日志写到某个分区（仍需内核侧存储驱动）**或**
- 至少 UEFI 能读 `oops` / `logfs` / `rawdump` 这些已有日志分区（0x1000000 / 0x800000 / 0x12C00000）

### 下一步（Round 13）

1. 解出 `Dwc3` / `Ufs` / `Fat` 三个驱动的 **DEPEX**（导出目录里对应的 `section*.raw`），
   列出它们要的协议 GUID —— 这是"能不能在我们 FV 里 dispatch"的判据；
2. 用 EDK2 的 binary INF 流程把它们纳入 `uke.dsc`/`uke.fdf`
   （`[Binaries] PE32|pe-Dwc3.efi|DEBUG` + 对应 `[Depex]`），FV 里再加通用 `UsbBusDxe`/`UsbMassStorageDxe`；
3. DarwinBoot 枚举 `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` / `EFI_BLOCK_IO_PROTOCOL`，
   打印句柄数量作为"U 盘 / 分区可见"的判据（不写文件也能验收）。

---

## 21. DEPEX 判定：厂商 USB 驱动依赖私有协议（Round 13）

从导出目录里解出各驱动的 DEPEX（`section0.dxe.depex`）：

**Dwc3（16 个依赖）** 里能认出的都是 EDK2 通用协议：
`665e3ff6…`(ComponentName)、`26baccb1/b2/b3…`(DevicePath 家族)、`27cfac87/88…`(DriverConfiguration/Diagnostics)、
`1e5668e2…` 等；但**另有约 6 个认不出的 GUID**，
其中 `fa5f306b-f47d-4ac4-a47d-882f8204ec30` 和 `f4e5c7d0-d239-47cb-aacd-7f66ef763238`
**同时出现在 Dwc3 与 UsbConfig 的 DEPEX 里** —— 这是 QcomPkg 的私有契约（时钟/PMIC/USB 配置），
我们的 FV 里没有对应 provider。

**UsbConfig** 各自的 DEPEX 也包含同样的两个私有 GUID（外加 `e722b03f…`、`b0760469…`、`1c34f691…` 等）。

**Fat**(131072) 只有 `ae37b942-457f-4c91-a196-d9669fd347a3`；**Ufs**(147456) 未找到 DEPEX 段。

### 判定

- **单把 Dwc3 放进我们的 FV 不会 dispatch** —— 缺私有 provider。
- 要让它在我们的 FV 里跑，得把依赖链一起搬（clock/PMIC/USB-config 的 provider），
  工作量在"多个 DXE + 它们的 PCD/协议契约"，不确定性高。
- **UFS 驱动**没有可见 DEPEX，反而是最可能"直接能用"的一个（但同样可能依赖私有协议，需实测）。

### 建议的排序（给决策用）

1. **先把 QEMU 日志通道收尾**（确定收益、当天可见）：`pmemsave` 已成功，只差按 XNU msgbuf 布局切片；
2. USB 路线保留为独立项目：先只把 `pe-Ufs-*.efi` 加进 FV 实测（它的依赖最少），
   能 dispatch 就顺势拿到分区读取能力；再决定是否搬 Dwc3 的整条依赖链。

---

## 22. 达成：内核日志可以机读了（本地/QEMU）— Round 14

`work/analysis/qemu-dump-log.sh` 已经端到端跑通：QEMU 跑同一份内核 →
`pmemsave 0x48000000 0x20000000`（512 MB 内核数据窗口）→ 用控制台前缀 `[%5d.%06d]:` 抽出**真实日志行**。

**这是"日志不再依赖拍照"的本地达成**，而且比照片信息量大得多（带时间戳、可 grep）。
已保存样本：`work/analysis/ref/qemu-kernel-log-b47.txt`（23 行，b47 内核）。

### 抽到的日志已经给出两条新事实

```
[   29.671879]: pe_arm_map_interrupt_controller: soc_phys:  0x8000000
[   30.457132]: IONVRAM.cpp:start:1447 - ...
[   30.460078]: IONVRAM.cpp:handleEphDM:1373 - ephemeral-data-mode not supported
[   30.466594]: vm: creating swapfile creation thread
[   30.481639]: AppleImage4: no legacy blob present
[   30.485722]: AppleImage4: magazine[c1gn]: failed to read nonce slot data: 2
```

1. **`pe_arm_map_interrupt_controller` 真的执行了** —— 说明平台专家把 `intc` nub 发布出来了，
   内核的 pexpert 中断控制器映射路径走到了（这比之前"没看到 ArmCpu 输出"强得多）。
2. **IONVRAM 起来了**（`IONVRAM.cpp:start`），且报告 `ephemeral-data-mode not supported`
   —— 这是 nonce/NVRAM 相关的能力缺口，可作为后续线索。

magazine 名字第一次读清（之前照片太小）：`pdmg, cptx, dvdi, c1bt, c1ab, c1gn, c1sm, d1ma, c1ge, c1er, c1ai, c1al`。

### 局限与下一步

- 只捞到 23 行：XNU 的 msgbuf 环会滚动覆盖，且 `pmemsave` 窗口里混着 kext 的格式串表（已按前缀过滤）。
  下一步把窗口缩到 msgbuf 所在页（用 `_msgbufp` 符号定位），并考虑用 `msgbuf=` 之类调大环形缓冲。
- QEMU 只覆盖"能在 virt 上跑的路径"；设备上的等价物仍是 USB/UFS 那条线（§20/§21）。

---

## 23. USB 日志：设备树里本来就有的厂商栈，被 `UKE_VENDOR_HW=0` 关掉了（Round 15）

### 结论先行：§18/§19/§20 的前提是错的

§18–§20 一直假设"我们的 FV 里没有 USB/UFS 驱动，所以要把厂商驱动搬进来"。实际情况是：
固件树 `workspace/uefi-uke`（Project Silicium / Mu-Silicium 的 fork）里，
`Platforms/Xiaomi/ukePkg/{Include/APRIORI.inc,Include/DXE.inc}` **早就写好了整套 USB/UFS 驱动**，
只是被 `uke.dsc` 的 `UKE_VENDOR_HW = 0` 整块条件编译掉了：

```
  INF Binaries/uke/QcomPkg/Drivers/UFSDxe/UFSDxe.inf            # 存储
  INF Binaries/uke/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf # DWC3 function controller
  INF Binaries/uke/QcomPkg/Drivers/UsbConfigDxe/UsbConfigDxe.inf
  INF Binaries/uke/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf
  INF Binaries/uke/QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe.inf       # ← 关键：把块设备当 U 盘给主机
  INF Binaries/uke/QcomPkg/Drivers/UsbInitDxe/UsbInitDxe.inf
  INF Binaries/uke/QcomPkg/Drivers/{Clock,Rpmh,Npa,Vcs,ICB,Smem,SPMI,I2C,TLMM,ChipInfo,Pmic}Pkg...
```

`UKE_VENDOR_HW=1` 缺的唯一东西是 `Binaries/uke/QcomPkg/**`（设备子模块里 uke 只有 `RawFiles/`）。

### 已经做的事（全部本地可复现）

1. **从库存固件抽出整套 DXE**：`work/analysis/fw-extract-mods.py`（venv `work/analysis/.venv-fw`）
   解析 `work/tablet-backup-20261103/images/uefi_a.img`，得到 **97 个 PE32** + 各自的
   **FFS GUID + DEPEX 原始字节 + UI 名字**，落在 `work/analysis/ref/vendor-dxe/`
   （`manifest.json` 里含 `ffs_guid`/`bytes`/`depex`）。DEPEX 解码表：`PUSH=0x02 … END=0x08`。
2. **生成二进制 INF**：`Platforms/Xiaomi/ukePkg/Tools/gen-vendor-binaries.py` 把 49 个驱动按
   Silicium 的路径约定写成 `Binaries/uke/QcomPkg/Drivers/<Dir>/<Name>.{inf,efi,depex}`
   （`FILE_GUID` 用库存 FFS 的 GUID；`UFSDxe`/`EnvDxe` 的 GUID 与其它机型 `Binaries/9707f` 完全一致，
   说明这批二进制就是同一套厂商 QcomPkg 产物）。
3. **打开开关**：`uke.dsc` `UKE_VENDOR_HW = 1`；`uke.fdf` FV 尺寸 3 MiB → **5 MiB**。
   5 MiB 不是随便选的：`MemoryMapLib.c` 早就把 `0xA7100000+0x300000`（UEFI FD）和
   `0xA7400000+0x200000`（UEFI FD Reserved）都留出来了，库存 `uefi_a.img` 本身就是 5 MiB。
4. **注释掉两个不存在的**：库存固件里没有 `XhciDxe`/`XhciPciEmulationDxe`（uke 只有 device mode，
   没有 USB host），`APRIORI.inc`/`DXE.inc` 里这两行改为注释。
5. **本地构建通过**：
   ```
   cd workspace/uefi-uke
   PATH="$PWD/.venv-edk2/bin:$PATH" .venv-edk2/bin/python \
     Platforms/Xiaomi/ukePkg/DeviceBuild.py TARGET=DEBUG ENABLE_SECUREBOOT=0 \
     FD_BASE=0xa7100000 FD_SIZE=0x500000 FD_BLOCKS=0x500 DEVICE_MODEL=0
   ```
   产物 `Build/ukePkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd` 里现在有 97 个模块，
   实测含 `UFSDxe/UfsfnDwc3Dxe/UsbMsdDxe/UsbDeviceDxe/UsbConfigDxe/UsbInitDxe/UsbPwrCtrlDxe`
   与 `ClockDxe/PmicDxeLa/DALSYSDxe/TLMMDxe/I2CDxe/SPMIDxe/RpmhDxe/NpaDxe/VcsDxe/ICBDxe/SmemDxe/
   TzDxeLA/ScmDxeCompat/ShmBridgeDxeLA/ChipInfoDxe/HALIOMMUDxe/HWIODxe/GLinkDxe/IPCCDxe/ULogDxe…`。
   **这一步不需要 CI，本地 ~1 分钟增量**。

### 关键机制（读厂商代码得到，不是猜的）

- `Silicon/Qualcomm/QcomPkg/Include/Protocol/EFIUsbMsd.h` 暴露
  `EFI_USB_MSD_PROTOCOL`：`AssignBlkIoHandle(This, EFI_BLOCK_IO_PROTOCOL*, Lun)` /
  `QueryMaxLun` / `StartDevice` / `StopDevice` / `EventHandler`。
  GUID `C8591FAF-DBCC-479E-9EF2-FD085BC37BC7`。
- `UsbMsdDxe` 的字符串里有 `UFS-LUN 0…7`、`UsbMsdFindPartitions`、`CheckBlkIoLunAssociation`
  —— 它就是把 **UFS 的块设备映射成 USB 大容量存储 LUN**。
- 厂商自带 `MassStorage` 应用（`Silicon/Qualcomm/QcomPkg/Applications/MassStorage/MassStorage.c`，
  并且已经在我们的 FV 里）示范了完整流程：按 `gEfiUfsLUnGuid` 找到块设备 →
  `AssignBlkIoHandle` → `StartDevice` → 循环 `EventHandler` → 音量上键退出。
- BDS 派发顺序不会困住任何驱动：`BdsDxe` 的 **DriverEntry 只安装 BDS arch protocol 就返回**，
  `DxeMain` 在 `CoreDispatcher()` 返回后才调 `gBds->Entry()`，而 `BdsEntry` 里的
  `EfiBootManagerConnectAll → BmConnectAllDriversToAllControllers` 会循环 `gDS->Dispatch()`。
  所以 APRIORI 里排在 `BdsDxe` 后面的驱动照样会起来（这也解释了为什么以前的控制台驱动能生效）。

### 新增 `UkeUsbLogDxe`（本轮写的代码）

`Platforms/Xiaomi/ukePkg/Drivers/UkeUsbLogDxe/`：
- 发布一个 **256 KiB 可写 RAM 块设备 `UKE-LOG`**，内容是它自己的日志文本（主机读到的就是磁盘内容，
  日志是"活"的：追加一行，下次主机读就能看到）；
- 在 **ReadyToBoot** 回调里（此时 UFS/USB 都已 dispatch）枚举所有 BlockIO、按 device path 里的
  UFS LU GUID 命名打印，然后 `AssignBlkIoHandle`（`UKE-LOG` 固定 LUN0）→ `StartDevice` →
  循环 `EventHandler` 保持 **`PcdUkeUsbLogHoldSeconds`（本轮设 40 秒）**，音量上键提前结束 →
  `StopDevice` → 继续启动 XNU；
- 面板上也会打同样几行，所以照片仍然能拿到诊断结果。

写这个驱动踩到的两个坑（记下来省时间）：
- `Protocol/EFIUsbfnIo.h` 会拉进 `EFIUsbEx.h`，与 MdePkg 的 USB 描述符 typedef 冲突，
  **不能**直接 include QcomPkg 的 USB 头；MSD 协议结构+GUID 在本文件里本地声明。
- 现在 MdePkg 的设备路径宏是 `HARDWARE_DEVICE_PATH`/`HW_VENDOR_DP`（不是老的 `HARDWARE_DEVICE`/`HW_VENDOR`），
  `IsDevicePathEnd`/`SetDevicePathNodeLength` 在 `<Library/DevicePathLib.h>`。

### 下一轮（b52）要看的

1. 主机侧：`fastboot boot boot.img` 后 **40 秒窗口内**，`lsblk` / `dmesg` 应出现新的 USB 磁盘；
   新增的第一块（256 KiB，无分区表）是 `UKE-LOG`，`dd if=/dev/sdX bs=4096 count=64 | strings` 就是 UEFI 阶段日志。
2. 其余块设备应被命名成 `UFS-LU0…`（面板上会打印），说明 UFS 真的起来了 —— 这是**内核日志落盘 → 主机读取**的前提。
3. 若面板出现 `usb-msd … Not Found`，说明 `UsbMsdDxe` 没 dispatch（DEPEX 未满足），下一轮就只剩
   `UsbfnDwc3Dxe` 那 16 个 GUID 依赖要逐个对上（`fa5f306b…`/`f4e5c7d0…`/`157a5c45…` 等 Qcom 私有协议）。
4. 若卡在 `start …` 之前/之后不动，是 DWC3/PHY 的问题（Type-C 角色、`UsbPwrCtrl`+`QcomCharger` 链路），
   面板会停在最后一行，照片可以定位。

### §23 补充：把 USB 栈的 DEPEX 全部对上，发现 `AcpiTableDxe` 缺失（b53）

把 `UsbfnDwc3Dxe`/`UsbConfigDxe`/`UsbDeviceDxe`/`UsbInitDxe` 的 DEPEX 逐个翻译成协议名
（GUID→名字用树里 94 个 `.dec` 反查，MdePkg 的用 `Mu_Basecore`）：

| 协议 | 提供者 | 在我们 FV 里 |
|---|---|---|
| DriverBinding / CPU / Metronome / Timer / RTC / Reset / Runtime / Security / MonotonicCounter / Variable(+Write) / Watchdog / BDS arch | MdeModulePkg/ArmPkg/EmbeddedPkg 开源件 | 有 |
| `gEfiSMEMProtocolGuid` (`f4e5c7d0…`) | `SmemDxe` | 有 |
| `gEfiPlatformInfoProtocolGuid` (`157a5c45…`) | `PlatformInfoDxe` | 有 |
| `gEfiChipInfoProtocolGuid` (`b0760469…`) | `ChipInfoDxe` | 有 |
| PMIC 协议 (`fa5f306b…`，只在 PmicDxe/SPMI/UsbConfigDxe 里出现) | `PmicDxeLa` | 有 |
| NPA 协议 (`1c34f691…`，Clock/Rpmh/Vcs/ICB/PwrUtils/CmdDb/CPR 都在用) | `NpaDxe` | 有 |
| USB config 协议 (`e722b03f…`，只在 Usb* 三个模块里) | `UsbConfigDxe` | 有 |
| **`gEfiAcpiSdtProtocolGuid` (`eb97088e…`)** | **`AcpiTableDxe`** | **之前没有！** |

`uke.fdf` 里 `AcpiTableDxe`/`AcpiPlatformDxe` 是被注释掉的（这个平台给 XNU 交 DT，不要 ACPI），
但 `UsbConfigDxe` 的 DEPEX 要求 `gEfiAcpiSdtProtocolGuid` —— 生产者不存在 ⇒ **整条 USB device 栈
永远不会 dispatch**，`StartDevice` 必然失败。已加回 `AcpiTableDxe`（只要协议，不要表，
`AcpiPlatformDxe` 仍然注释）并放进 APRIORI 的 USB 块之前。

配置地图也核对过：`ukePkg/Library/ConfigurationMapLib` 已经有 `UsbFnIoRevNum`/`USBHS1_Config`/
`EnableUfsIOC`/`UfsSmmuConfigForOtherBootDev` 这些键（与其它机型一致），不需要补。

重建后 FV 98 个模块，USB 链所需的 14 个提供者全部在位（`AcpiTableDxe/UFSDxe/UsbMsdDxe/UsbConfigDxe/
UsbDeviceDxe/UsbInitDxe/UsbfnDwc3Dxe/PmicDxeLa/SmemDxe/NpaDxe/ChipInfoDxe/PlatformInfoDxe/ClockDxe/UkeUsbLogDxe`）。

### b53 镜像（要刷的就是这个，b52 作废）

```
work/flash/vendor_boot_round-b53-ukelog.img   ed50aa6b…  (85,782,528 B)
work/flash/boot-round-b53-ukelog.img          2d0e9915…  (1,724,416 B，内含 5 MiB FV，sha 已核对)
```

---

## 24. 内核日志 → pram → USB：通道两头都接上了（Round 16）

### 先把 pram 地址钉死（之前没有验证过）

`board.toml [payload.adt] panic_region = "0x9FFE00000:0x200000"` 从来没被核对过。这次直接读
**构建出来的 ADT**（`out/.../Payloads/kernel.macho.adt`，14,192 B）：

```
pram occurrences: ['0x28c4']           # 只有一个 pram 节点
0x9ffe00000 as LE u64 at: ['0x28f0']   # reg = 0x9FFE00000
0xff800000  as LE u64 at: []           # make-uke-adt.py 里 PANIC_LOG_BASE 的那个值根本没进 ADT
reg 数据: 00 00 e0 ff 09 00 00 00 | 00 00 20 00 00 00 00 00  → 0x9FFE00000 + 0x200000 ✅
```

结论：**XNU 的 panic/日志窗口就是 `0x9FFE00000 + 0x200000`**（`unixnu/devicetree.py` 从
`panic_region` 生成 `/chosen/pram`）。`scripts/make-uke-adt.py` 里的
`PANIC_LOG_BASE = 0xFF800000 / PANIC_LOG_SIZE = 0x380000` 是死代码，但它把
`/chosen/embedded-panic-log-size` 写成了 `0x380000`（ADT 里实测 `00 00 38 00` = LE `0x00380000`），
与 reg 的 `0x200000` 不一致 —— **待修**（见下）。

### 固件侧：`UkeUsbLogDxe` 现在会把上一次启动的内核日志搬进日志盘

- `PcdUkePramBase = 0x9FFE00000`、`PcdUkePramSize = 0x200000`（与 ADT 实测一致）。
- ReadyToBoot 时先 `gDS->GetMemorySpaceDescriptor()` 确认窗口在 GCD 里（未映射就读会 fault），
  再扫一遍、只保留长度 ≥8 的可打印串（换行连接），最多 192 KiB 写进日志盘，
  并打一行摘要到面板：`pram first text: "..."` / `<none: DRAM did not survive…>`。
- 日志盘从 256 KiB 扩到 **1 MiB**（`UKE_LOG_BLOCKS 256 × 4096`）。

所以现在的链路是：

```
XNU 运行 → msgbuf 尾部写进 pram（内核侧）            ┐
                                                   ├→ 下一次 UEFI 启动读 pram → 1 MiB RAM 盘 → USB MSD → 主机 dd
UEFI 阶段自己的日志（dispatch/枚举/UFS LUN）────────┘
```

### 内核侧：`UkeLastLog` 终于被链接进去了（CI run `42211461355`）

`board.toml [kernel] archives` 里 `build-boot-log.py` 一直是被注释的，理由是"本仓库没有
`kits/uke`"—— 实际上源就在 `package/`（`kit = "uke"` 让它在构建树里显示为 `kits/uke`）。
已让 `scripts/build-boot-log.py` 在 `kits/uke/` 缺失时回退到 `package/`，并打开这个 archive。

`UkeLastLog.c` 的行为（读代码确认，不是猜）：
- 由 `sheng-log=1` 打开（这个 boot-arg 一直在 kernel_args 里，之前没有消费者）；
- 每 500 ms 把 **msgbuf 环的最新 cap 字节**拷到 `gPanicBase` 的 FUNK 头后面
  （`cap = min(msgbuf size, 2 MiB − header)`，msgbuf 默认远小于 2 MiB，所以不会越界）；
- ramoops 那半边有 `gPanicSize < 0x200000+0x200000` 的守卫，我们只有 2 MiB → 自动跳过（不会写飞）；
- `XNU_WATCHDOG_POLICY` / `CONFIG_XNU_BOOT_PROGRESS` 都没开，所以另外两个源文件是惰性的。

也就是说：CI 产物一旦到位，下一次 payload 就能取到**内核控制台日志尾部**，不再只有 panic 文本。

### 待修的小坑（下一轮）

`/chosen/embedded-panic-log-size` = 0x380000 比 `pram.reg` 的 0x200000 大：XNU 若按这个长度写，
尾部会越过 DRAM 高区末端 `0xA00000000`。等通道验通后把它改成与 reg 一致（改 make-uke-adt.py 的
`PANIC_LOG_SIZE`，或让 devicetree.py 覆盖该属性）。

### b54 镜像（要刷的是这个；b53 作废）

```
work/flash/vendor_boot_round-b54-pramlog.img  826eb3de…  (85,782,528 B)
work/flash/boot-round-b54-pramlog.img         c76d3ff5…  (1,724,416 B，内含 5 MiB FV)
```

**要 boot 两次**：第一次让 XNU panic 把日志写进 pram，第二次 UEFI 就会把它搬进日志盘。

---

## 25. 真正的"推进受阻"根因：CI 的 prelink 一直是断的（Round 16）

上一轮我以为是内核/固件问题在原地打转，其实**内核产物已经很多轮没有更新过**了。
b47 之后的每一轮 payload 都还在用 `work/ci-artifacts/b47/kernel.macho`，原因是 CI 在
pass 3 就挂：

```
+ python3 .../tree/kits/NativePrelink/emit_prelink.py --kernel ...
Python: can't open file '.../tree/kits/NativePrelink/emit_prelink.py': [Errno 2] No such file or directory
```

（run `42080292233`，commit `7efcc06`）

### 原因

- `feeds.toml` 里板级 kit 的名字是 **`XnuPrelink`**（`[[source]] name = "XnuPrelink", kind = "kit",
  local = "kits/XnuPrelink"`），所以构建树里只会有 `kits/XnuPrelink`；
- `components.toml` 的目录也是 `path = "kits/XnuPrelink"`（它只把 `NativePrelink` 放在
  `runtime_identifiers` 里，那不是目录别名）；
- 但 `board.toml [kernel.prelink] tool` 一直写着 `kits/NativePrelink/emit_prelink.py`。

以前能用，是因为当时 `feeds.toml` 里还注册过一个 `NativePrelink` 的兼容 alias（后来因为
"第二个 kit 条目会让树不再创建 kits/NativePrelink" 被删掉），删掉之后这条路径就没人创建了 ——
于是 prelink 直接找不到工具，内核链接步骤整体失败。本地复现：`out/.../tree/kits/` 里只有
`XnuPrelink`，没有 `NativePrelink`。

### 修法

1. `board.toml [kernel.prelink] tool = "kits/XnuPrelink/emit_prelink.py"`（改回 catalog 自己的路径）；
2. 仓库里保留 `kits/NativePrelink -> XnuPrelink` 这个符号链接，给那些硬编码
   `kits/NativePrelink` 的脚本（`audit-air-nir-ios.py`、`build-native-accelerator-abi.py`、
   `build-native-legacy-display.py` 等）继续用。

顺带：`scripts/build-boot-log.py` 也修了 —— 它按 `ROOT/'kits/uke'/name` 找 `UkeLastLog.c`，
但直接跑脚本时 ROOT 是设备仓库（源在 `package/`），现在缺 `kits/uke/` 时回退到 `package/`。

### 在跑/待观察

- run `42211461355`（`8eaacfb`，开 msgbuf 写 pram）—— 会照旧在 prelink 挂掉，拿不到内核；
- run `42211693015`（`36d40d0`，含 prelink 修复）—— **这个才是关键**，只要它绿，
  `work/ci-artifacts/<run>/kernel.macho` 就会第一次带上 `UkeLastLog` 的 msgbuf 写盘。

---

## 26. 两个固件变体：先取日志，再谈 UFS（Round 17）

上一轮的 b54 把整套厂商栈（含 `UFSDxe`/`SdccDxe`）都编进去了。问题是 UFS/SD 是真的去摸硬件控制器：
如果时钟/电源树和驱动的预期不一致，它会直接把启动挂在那里，而**日志通道本身根本不需要存储**
（`UKE-LOG` 是 RAM 盘）。所以这轮把存储做成独立开关：

```
uke.dsc
  UKE_VENDOR_HW      = 1   # 厂商 DXE 全套（USB device 栈、PMIC、Clock、NPA、SMEM…）
  UKE_VENDOR_STORAGE = 1   # 0：不装 UFSDxe/SdccDxe，只留日志通道
```

`APRIORI.inc`/`DXE.inc` 里 `UFSDxe`、`SdccDxe` 现在都挂在 `UKE_VENDOR_STORAGE` 上
（注意 `SdccDxe` 原来在 `DXE.inc` 里还有第二处，只加在 APRIORI 会漏，实测第一版变体里它还在，
已一并修掉）。

### 两个镜像（固件全在 `boot.img` 里，`vendor_boot` 只带 payload，刷一次即可）

| 镜像 | FV sha | 内容 |
|---|---|---|
| `boot-round-b55-usbonly.img` (1,667,072 B) | `1e158103…`（96 模块） | **先试这个**：厂商 USB device 栈 + `UkeUsbLogDxe`，不碰 UFS/SD |
| `boot-round-b54-pramlog.img` (1,724,416 B) | `95d83175…`（98 模块） | 全量：再加 `UFSDxe`+`SdccDxe`，成功的话 UFS 各 LU 会作为 U 盘出现 |

两次都用同一个 `vendor_boot_round-b55-usbonly.img`（`4aa9690e…`）。换固件只要换 `boot.img`，
不用再刷 vendor_boot —— 这一点以后每轮都省一次 85 MB 传输。

### 固件源码已提交到 `uefi-uke`（`ac9352b`）

`Platforms/Xiaomi/ukePkg/{uke.dsc,uke.dec,uke.fdf,Include/{APRIORI,DXE}.inc}`、
`Drivers/UkeUsbLogDxe/`、`Tools/gen-vendor-binaries.py`、`Resources/Configs/uke.toml`（FD 0x500000）
全部纳入版本控制；`Binaries/uke/QcomPkg/**` 由 `Tools/gen-vendor-binaries.py` 从
`work/analysis/ref/vendor-dxe/`（库存 `uefi_a.img` 的抽取结果）重新生成，不入库。
