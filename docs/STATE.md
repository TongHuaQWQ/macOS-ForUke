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

---

## 27. fileset 未匹配调用：把 panic 的现场对到具体函数（Round 17，为下一轮准备）

`--allow-missing` 的语义是"每个未匹配的函数起点都绑到 panic stub"，所以现在每条 panic 都指向一个
具体的未匹配函数。这轮把 b47 内核上的那条 panic 拆开看：

```
fileset: original component called an unmapped kernel function from 0xfffffff00979c38c (x0 0x8 x1 0x2)
```

用 `work/analysis/symbolize-panic.py --artifact work/ci-artifacts/b47 --disasm`（slide `0x42f0000`）：

```
fffffff00979c384: mov  w0, #0x8
fffffff00979c388: bl   0xfffffff00c09e690      ← 被调用者（stub）
fffffff00979c38c: cbnz w0, ...                 ← 返回地址 = panic 里的 0x…c38c
fffffff00979c394: adrp x0, 0xfffffff0075fe000 ; add x0,#0xffd ; add x1,sp,#8 ; mov w2,#4
fffffff00979c3a4: bl   0xfffffff00c02dd84      ← 形状像 PE_parse_boot_argn(name,&v,4)
fffffff00979c3a8: adrp x0, 0xfffffff007602000 ; add x0,#0xf4f ; add x1,sp,#0xc ; mov w2,#4
fffffff00979c3b8: bl   0xfffffff00c02dd84
```

两个信息：

1. 调用者在**解析 boot-arg**（连续两个 `(name, &int, 4)` 的调用），所以这条 panic 发生在很早的
   boot-arg 处理里，还没到 kext 启动；
2. `0xfffffff00c09e690` 在 b47 产物里解析成
   `_set_awl_scratch_exists_flag_and_subscribe_for_pm + 0xc8` —— **不是函数起点**。
   也就是说 emitter 给未匹配函数放的 stub 落在了一个已匹配函数的中段。这一点在写实现之前要先搞清楚
   （是 fileset 的函数 extent 与 native 函数重叠，还是 stub 放置按"原地址"而 native 布局不同）。

### 缺口规模（`ref/classify-b47.json`）

| 类别 | 函数数 | 调用点 | 处理方式 |
|---|---|---|---|
| `anchorable` | 2 | 18 | 已改名加进 `kits/XnuPrelink/fixtures/fileset-25F84-symbols.json`（`_vnode_hold`、`_ml_set_interrupts_enabled_with_debug`） |
| `absent` | 22 | 1018 | **我们的重建内核里根本没有对应函数**，没法 anchor，只能自己实现或按需放过 |

`absent` 里最重的几个（calls 数 / 体积 / 线索）：

```
216  0x190  amcc_rorgn_common.c / "overflow in rorgn_contains: %p + %zu @%s:%d"
173  0x90   callees: IORWLockUnlock, IOStatistics::getKextNodeFromBacktrace, kalloc_ext
137  0x8    （空体，像 getter）
126  0x48
 83  0x2c
 62  0x20
 48  0x8c
 36  0xc
```

下一步的顺序：先用 USB 日志通道确认启动链到底走到哪，再按"实际被调用到的"顺序补实现
（0x8/0xc/0x20 这些一两条指令的大概率是 getter/谓词，风险低；`IORWLockUnlock` 那个明显有副作用，
不能直接空返回）。

---

## 28. fileset 缺口真相：不是"我们没有这些函数"，是 matcher 没配上（Round 18）

### 数据来源：emit 报告把每一处 stub 都记下来了

`kernel.json`（emit 报告，CI 产物里就有）里有两个字段：

```
stubbed   : { owner_kext: { target_address: call_count } }     # 22 个 kext、170 对、760 次调用
stub_sites: { site: target }                                    # 760 条
```

`target` 是**原始内核**（Apple kernelcache）地址 + slide，所以可以直接用 kernelcache 自己的
`LC_SYMTAB` 命名。命名结果（`work/analysis/ref/stubbed-worklist.txt`，127 个函数）：

```
 132  _IORecursiveLockUnlock          [IODARTFamily, apfs, IOGPUFamily]
  87  _csr_check                      [AppleMobileFileIntegrity, apfs, IOGPUFamily]
  50  _vm_kernel_addrhide             [IOGPUFamily, IOSurface]
  38  _kauth_cred_getuid              [apfs, sandbox]
  22  _kernel_thread_start            [AppleARMWatchdogTimer, apfs, …]
  22  _task_deallocate_external
  16  _OSSynchronizeIO
  15  IOReporter::updateAllReports    [AppleARMPlatform, AppleIISController, ApplePMGR]
  12  _vnode_getparent / IOUserClient::copyObjectForPortNameInTask / …
   9  _vm_map_page_mask, _upl_page_present, _cpu_number, _panic_with_options …
```

**这些全是我们内核里就有的标准 XNU 符号** —— 所以 §27 里"absent：只能自己实现"的结论是错的：
`classify-missing.py` 只会用 `__func__` 字符串去认名字，而这些小函数根本不把函数名放进数据表，
于是被误判成"源码没编进来"。

### 修法：按 kernelcache 自己的符号表生成 reviewed anchor

`work/analysis/gen-fileset-anchors.py`：

1. 用 kit 自己的 `kc_image.Image` 打开 kernelcache 的 `com.apple.kernel` 入口；
2. **完全照 matcher 的规则**算函数 extent（`LC_FUNCTION_STARTS` 的 start → 下一个 start，
   夹在 exec 段的 backed 范围内），得到 `original_size`；
3. `original_sha256` = 该 extent 的字节；
4. `native_symbol` = kernelcache `LC_SYMTAB` 里**恰好落在该地址**且唯一的符号名
   （不做"最近符号"猜测）；
5. 写进 `kits/XnuPrelink/fixtures/fileset-25F84-symbols.json`。

结果：**122 个函数 / 727 个调用点**（760 中的 95%），fixture 从 62 条涨到 **182 条**，
并且 182 条全部通过"重新按 matcher 规则校验 extent + sha256"的自检。
kit 自带测试 `pytest test_emit_prelink.py test_fileset_match.py` → 26 passed。

剩下的 5 个地址（22+4+3+2+2 次调用）在该地址上没有唯一符号，留待后续。

### 这一步为什么直接对着目标

下一个内核（`a907ef7` 触发的 CI）如果 anchor 生效，`stubbed functions` 应当从 170 掉到 ~48，
`fileset: original component called an unmapped kernel function` 这条 panic 大概率消失 ——
也就是目标里的"越过 fileset 未匹配调用 panic → kext 启动"。等 USB 日志通道通了，这一步的验证
（kext 有没有真的 start、IOKit 有没有匹配到）就有日志可读，不再靠照片。

### 顺带修正上一轮的两个结论

- §27 说的"stub 落在已匹配函数中段（`set_awl_scratch…+0xc8`）"是**误读**：
  `_unixnu_fileset_unmapped` 是我们内核新增的符号，symbolize 工具用的是 Apple kernelcache 的
  符号表，自然对不上位置。stub 只有一个，所有未匹配调用都指向它。
- §27 说的"22 个 absent 只能实现"作废，见上。

### §28 补充：anchor 的证据强度与一次独立交叉验证

anchor 的主证据是 **kernelcache 自己的 `LC_SYMTAB` 在该地址上给出的唯一符号名**，加上
"extent 由 matcher 自己按 `LC_FUNCTION_STARTS` 推导 + 该 extent 的 sha256"。
为了不全靠单一来源，又用**另一份 Apple kernelcache**（`work/macos-ipsw/kernelcache.mac17p.decompressed`，
mac17p，22,951 个函数、5,850 个具名符号）做了字节级交叉验证：

| 结果 | 数量 |
|---|---|
| mac17p 里能找到**字节完全相同**的函数 | 39 |
| 其中名字相同 | **31** |
| 其中名字不同 | 8 |
| mac17p 里没有（不同 SoC，函数体不同） | 83 |

8 个不同名的都发生在**极小函数体**上（例如 `_OSSynchronizeIO` ↔ `_flush_dcache`、
`_IOMallocPageable` ↔ `_cpu_broadcast_xcall`），即字节相同但两份构建给了不同名字 —— 这是小
桩函数的常见现象，不构成推翻主证据的理由，但也提醒：**这批 anchor 的正确性最终要由设备上的
行为来验证**（这正是 USB 日志通道的用处）。

另外记一条待查：`0xfffffe000b9fcb70` 在早期人工 anchor 里叫 `_vnode_hold`，而 v25F84 的
`LC_SYMTAB` 在该地址上写的是 `_vnode_getparent`；两者 extent（0x178）与 sha256 都能通过 matcher
校验，所以**结构上都是合法 anchor**，语义谁对还没定论。这条只有 12 个调用点（apfs/sandbox），
若后续出现 vnode 相关的诡异行为，先从这里查。

`work/analysis/fetch-kernel-artifact.py <run-id>` 是新加的取件脚本（解析 run 的 artifact id/size →
交给 `fetch-artifact.sh` 分块并发下载 → 解包 → 打印 kernel.macho 的 sha256），
以后每轮取内核不再手搓。

---

## 29. CI 修好了：b47 之后第一个新内核到手（Round 19）

### 两个 run 的归属（先澄清"失败了"那条）

| run | commit | 结果 | 说明 |
|---|---|---|---|
| `42211461355` | `8eaacfb` | **failed** | 只含"开 msgbuf 写 pram"，**不含 prelink 路径修复** → 仍然死在 `tree/kits/NativePrelink/emit_prelink.py: No such file or directory`（日志已核对） |
| `42211693015` | `36d40d0` | **success** | 含 prelink 路径修复 + msgbuf 写 pram；**这是 b47 之后第一个绿灯内核** |
| `42212761913` | `a907ef7` | 进行中 | 再加 120 个 anchor（本轮 §28） |

所以那条失败的 run 是**修复前**的提交；修复提交的 run 已经绿了。

### b56 内核（run 42211693015）

```
kernel.macho  sha256 = dc726af08ed5e648ce2a5189cb528c36da21a81d10ae4d7a19468c3a00bf82c4
artifact      id 13501470224, 45,252,399 bytes；解到 work/ci-artifacts/b56/
logs/archive-15-build-boot-log.log 存在且 PASS:
  "PASS: strict source-kernel ARM64 pram writer, watchdog policy and crash probe build"
```

也就是说 **`UkeLastLog` 真的链接进去了**，`sheng-log=1` 这次有消费者了：
内核每 500 ms 把 msgbuf 尾部写进 `/chosen/pram`（`0x9FFE00000 + 0x200000`），
而固件侧 `UkeUsbLogDxe` 会在下一次启动把它搬进 1 MiB 的 `UKE-LOG` RAM 盘供主机读取。

**通道两头这一次都在同一套镜像里了。**

emit 报告（b56）：`stubbed` 177 对 / `stub_sites` 964 条 —— 比 b47（170/760）略多，
因为这次多链接了 UkeLastLog 等我们自己的代码；anchor 生效后应大幅下降，见 `a907ef7`。

### b56 镜像（本轮要刷的）

```
work/flash/vendor_boot_round-b56-pramlog.img  740ab986…  (85,782,528 B；内含 b56 内核)
work/flash/boot-round-b56-usbonly.img         280ca857…  (1,667,072 B；USB 日志通道，无 UFS 探测)
```

固件与内核这两轮是**同时变**的，所以这次 vendor_boot 也要刷（以后内核不变时可以只换 boot.img）。

### 取件工具

`work/analysis/fetch-kernel-artifact.py <run-id> [--label bNN]`：解析 run 的 artifact →
`fetch-artifact.sh` 分块并发下载 → 解包 → 打印 `kernel.macho` 的 sha256。

### §29 补充：b56 的两个固件变体（同一份 vendor_boot）

内核在 `vendor_boot` 里、固件在 `boot.img` 里，所以同一份 b56 内核可以配两种固件：

```
work/flash/vendor_boot_round-b56-pramlog.img   740ab986…  (85,782,528 B)  ← 刷一次
work/flash/boot-round-b56-usbonly.img          280ca857…  (1,667,072 B) 先试：USB 日志通道，不碰 UFS
work/flash/boot-round-b56-storage.img          c76d3ff5…  (1,724,416 B) 再试：加 UFS LU 暴露
```

`boot-round-b56-storage.img` 的 sha 与 b54 的 `boot.img` 完全相同（`c76d3ff5…`）——
同一 FV + 同一 BootShim + 同一 DTB 得到同一字节，说明这条固件打包链路是确定性的，
以后可以用 sha 直接判断"固件没变，只换了内核"。

---

## 30. b56 实机：固件在派发厂商驱动时栈溢出（Round 20，有实拍）

### 实拍内容（用户 2026-11-06 20:22，b56 那套镜像）

```
SSR : EC 0x25  IL 0x1  ISS 0x00000047      ← Data abort（写）、translation fault, third level
SP  0x00000000A7600F20   ELR 0x00000000C2352524   FAR 0x00000000A7600F30
Data abort: Translation fault, third level
Stack dump:
Recursive exception occurred while dumping the CPU state
```

三个地址一对上就清楚了：

| 值 | 含义 |
|---|---|
| `ELR = 0xC2352524` | 落在 **DXE Heap**（`0xA80C0000..0xC3A00000` 附近）→ 出事的代码是一个从 FV 装进堆里的**厂商 DXE 驱动** |
| `SP = 0xA7600F20` | 栈区是 `0xA760D000 + 0x40000`，SP 从顶部 `0xA764D000` 往下长；这个值比**栈底还低 0xC0E0** ⇒ 已经用了 ≈305 KiB，超过 256 KiB |
| `FAR = 0xA7600F30` | 正好落在 `0xA7600000..0xA7601000` —— **唯一没有列进 MemoryMapLib 的 4 KiB 空洞**（FD 5 MiB 刚好结束在 0xA7600000，`Info Blk` 从 0xA7601000 开始） |

所以：厂商栈把 256 KiB 的固件栈用穿，第一次写落到那个未映射的 4 KiB 洞里 → data abort；
异常处理器本身也在这个已经用穿的栈上跑，于是 `Recursive exception occurred while dumping the CPU state`。

### 修法（`Platforms/Xiaomi/ukePkg`）

1. **栈 256 KiB → 1 MiB**：`PcdCPUCorePrimaryStackSize|0x00100000`，`MemoryMapLib` 的
   `UEFI Stack` 同步改 1 MiB（顶部上移到 `0xA770D000`）；
2. **把那 4 KiB 洞补上**（新条目 `FD Tail`），以后再溢出至少不会立刻成 translation fault；
3. 顺移后面的堆区并保持 DXE Heap 末尾仍是 `0xC3A00000`：
   `SEC Heap 0xA770D000`、`Sched Heap 0xA7799000`、`FV Region 0xA7B99000`、
   `UEFI RESV 0xA7F99000`、`DXE Heap 0xA80C0000 + 0x1B940000`；
4. 顺手修了一个**记账错误**：`UEFI FD` 条目还写着 3 MiB（`0x00300000`），而 FD 早就变成 5 MiB 了，
   现在改成 `0x00500000`；
5. `UkeUsbLogDxe` 在入口先写一行 `UKE-LOG: driver entry` —— 万一还是在派发阶段挂掉，
   这行至少告诉我们"驱动入口跑过没有"。

### b57 镜像（内核未变，**只需换 boot.img，不用再刷 vendor_boot**）

```
work/flash/vendor_boot_round-b57-pramlog.img  f9f9cffe…  (85,782,528 B；b56 内核，与已刷的是同一份 payload)
work/flash/boot-round-b57-usbonly.img         b4e0bcf3…  (1,667,072 B) 先试：USB 日志通道，无 UFS 探测
work/flash/boot-round-b57-storage.img         83ec23f9…  (1,724,416 B) 再试：加 UFS LU
```

---

## 31. anchor 生效了：stub 调用点 964 → 363（Round 21，emit 报告为证）

比较两个内核的 emit 报告（同一 matcher、同一 kernelcache，只差 fixture 里的 anchor 数）：

| 内核 | anchor 数 | `stubbed` 对数 | stub 调用点 |
|---|---|---|---|
| b56（run `42211693015`） | 62 | 177 | **964** |
| b58（run `42212761913`，`a907ef7`） | 182 | 31 | **363** |

分 kext 看（stub 调用点，b56 → b58）：

```
apfs                    182 ->  31      AppleMobileFileIntegrity  147 ->  68
ApplePMGR               144 -> 138      IOGPUFamily               110 ->  19
AppleARMPlatform         68 ->  37      sandbox                    57 ->  24
IODARTFamily             44 ->   0      AppleARMWatchdogTimer      40 ->  21
IOPCIFamily              37 ->   9      IOSurface                  36 ->   7
IOHIDFamily              36 ->   2      IOAVFamily                 19 ->   0
IOGraphicsFamily         14 ->   0      IOMobileGraphicsFamily      8 ->   0
```

**ApplePMGR 几乎没动（144→138）**，剩下的 363 个调用点主要集中在它身上 —— 它正是我们启动链
要过的 kext，所以下一批 anchor 优先级就按这个排。

### 第二批：剩下 16 个函数，全部可 anchor（提交 `139c704`）

`gen-fileset-anchors.py --report work/ci-artifacts/b58/kernel.json` 报"16 anchors from 363
stubbed calls, 0 skipped"：

```
_ml_at_interrupt_context   _ml_set_interrupts_enabled   _sched_perfcontrol_ml_set_interrupts_without_measurement
_task_suspend2   _task_resume2   _task_deallocate   _task_update_physical_writes
_proc_is_rsr   _proc_is_translated   _proc_use_alternative_symlink_ea
IOMemoryDescriptor::getDMAMapLength   IOMemoryDescriptor::setPurgeable
IOMallocZeroDataShareable(_external)   IOMallocDataShareable(_external)   _vnode_hold
```

其中 5 个上一批被丢掉，原因是 **Apple 会在同一地址导出多个名字**（`_task_deallocate_external`
和 `_task_deallocate` 之类），原来的"地址上只能有一个符号"过滤把它们排除了；现在取排序后的第一个
别名。fixture 182 → **197** 条，全部通过 extent + sha256 自检，kit 测试 26 passed。

### 顺手改正一条错的 anchor

`0xfffffe000b9fcb70` 之前按形状证据标成 `_vnode_hold`，但 kernelcache 的 `LC_SYMTAB` 在这个地址上
写的是 **`_vnode_getparent`**；而 b58 的 emit 报告显示真正的 `_vnode_hold` 在
`0xfffffe000ba18610` 仍然处于 stub 状态。已把前者改名、并为后者单独加了一条 anchor。
（这正是 §28 里记下的那个"待查"项，现在有结论了。）

### b59 镜像（1 MiB 栈固件 + b58 内核）

```
work/flash/vendor_boot_round-b59-anchors.img  040bf127…  (85,782,528 B；含 b58 内核)
work/flash/boot-round-b59-usbonly.img         b4e0bcf3…  (1,667,072 B；USB 日志通道，无 UFS)
work/flash/boot-round-b59-storage.img         83ec23f9…  (1,724,416 B；加 UFS LU)
```

`139c704` 已推送，下一个内核（197 anchors）大概率把 stub 调用点压到接近 0 —— 那才是
"越过 fileset 未匹配调用 panic"这一步的终点。

---

## 32. 为"取不到日志"准备两条后路（Round 22）

### 1. ArmCpu 不再故意 panic（提交 `295c481`）

诊断块原本是"只有面板"时代写的：10 ms 计数 → dumpIrqState → 停 15 秒等拍照 → 故意 panic 冻屏。
它**永远走不到 `registerService()`**，也就是设计上就卡住启动链。现在日志能取，改成：

- 仍然做 10 ms 测量，并打一行
  `ArmCpu: <N> timer interrupts, <M> IPIs in the first 10 ms after registration`（这行现在是机读的）；
- 停 15 秒 + dumpIrqState + panic 移到 **`armcpu-probe=1`** 后面；
- 默认打印 `ArmCpu: armcpu-probe=0, continuing to registerService` 然后继续。

也就是说下一个内核会**真的走过 ArmCpu**，进入 IOKit 匹配之后的阶段。

### 2. 即使 USB 不通，也能从照片读到内核日志尾部

`UkeUsbLogDxe` 的 `DumpPreviousBootLog()` 现在除了把 pram 文本塞进 1 MiB 日志盘，还会**保留最后
34 行并打到面板上**（`--- previous boot log, last N lines ---`）。40 秒窗口期间屏幕是静止的，
而这块屏幕的照片能看清 55 行（b56 那个异常 dump 的照片就是证据），所以即使主机完全看不到 U 盘，
**上一次内核日志的尾部仍然能拍下来**。

两条后路互相独立：USB 通了拿全量 192 KiB；USB 不通拿尾部 34 行 × 照片。

---

## 33. b59 实机：能过厂商派发了，但黑屏后自己重启（Round 22）

用户实测 `boot-round-b59-usbonly.img` 与 `…-storage.img` **两个都是**：跑到一半黑屏，然后设备自己重启。

### 已经排除的

- **256 KiB 栈溢出**：b56 那次是能看见的 UEFI 异常屏（`SP=0xA7600F20`/`FAR=0xA7600F30`）；
  b57/b59 用的是 1 MiB 栈那份固件，**这个 data abort 不再出现** —— 说明加大栈确实让厂商驱动派发过去了。
- **内核故意 panic**：b59 的内核是 b58（`a907ef7`，182 anchor），**还带着旧版 ArmCpu 的故意 panic**
  （`armcpu-probe` 那个开关是 `295c481` 才有的，还在 CI）。所以 b59 预期就是走到 ArmCpu 然后冻屏；
  但实际是"黑屏 + 重启"，说明 panic 画面没画出来（此时视频控制台还没起来），
  而重启是别的东西干的。

### 最可能的原因：Gunyah 虚拟看门狗

`UkeBootMemoryDxe` 入口做的三件事说明这台机器上有 Gunyah 的虚拟看门狗，超时 30 s：

```c
UkeScmCall (0x86000008, 0xffff, 30000, …)   // 查询超时
UkeScmCall (0x86000005, 2, 0, …)            // 关闭
UkeScmCall (0x86000007, 0, 0, …)            // 喂狗
```

问题在于：**这个驱动只喂一次**。而 `UkeUsbLogDxe` 在 ReadyToBoot 要按住 CPU 40 秒给主机读盘，
这段时间没人喂狗 —— 30 秒一到就会被复位，症状正好是"跑一半 → 黑屏 → 重启"。

### 这一轮的固件改动（b60 用）

1. `UkeUsbLogDxe` 复用同样的 SMC（`0x86000007`）：进窗口前喂一次，**窗口循环里每 20 ms 喂一次**，
   并把第一次的返回值打进日志（`UKE-LOG: gunyah pet <rc>`）；
2. 窗口 **40 s → 20 s**，即便 SMC 不生效也落在观察到的 ~30 s 生存期内；
3. 加上上一轮的**面板回显**（pram 尾部 34 行）—— 这样即使 USB 完全不通，照片也能读到日志尾部。

配套的内核（`295c481`）里 ArmCpu 默认**不再故意 panic**，会继续 `registerService()`，
所以 b60 是第一次"固件不挡、内核不挡"的组合，能真正看出启动链下一站停在哪。

---

## 34. 关键发现：pram 写手从来没被链接进内核（Round 23）

上一轮我说"`UkeLastLog` 真的链接进去了"，那是**错的** —— 现在有反证：
直接在这两个产物里搜字符串，一个都没有。

```
work/ci-artifacts/b56/kernel.macho: UkeLastLog 0  uke_panic_flush 0  BootWatchdogPolicy 0  CrashLogProbe 0
work/ci-artifacts/b58/kernel.macho: UkeLastLog 0  uke_panic_flush 0  BootWatchdogPolicy 0  CrashLogProbe 0
```

原因在链路的后半段：

1. `scripts/build-boot-log.py` 只负责**编译并打包** `build/uke-last-log/libUkeLastLog.a`
   （它的 PASS 行只说明编译成功）；
2. 真正把归档塞进链接的是 `scripts/build-xnu-4k-baseline.sh:884`：

```sh
if [ "$BOOT_LOG" = 1 ] || [ "$WATCHDOG_POLICY" = 1 ]; then
  LAST_LOG_ARCHIVE=$ROOT/build/uke-last-log/libUkeLastLog.a
  ... KERNEL_EXTRA_ARCHIVES="$KERNEL_EXTRA_ARCHIVES -Wl,-force_load,$LAST_LOG_ARCHIVE"
fi
```

而 `BOOT_LOG=${XNU_BOOT_LOG:-0}`、`WATCHDOG_POLICY=${XNU_WATCHDOG_POLICY:-0}`，
`configs/uke-macos_defconfig` 里两个**都是 0**（`CONFIG_XNU_BOOT_PROGRESS` 也没开），
所以归档就算建好了也永远不会进 `LD_KERNEL_ARCHIVES`。

### 修法（提交 `e01a101`）

```toml
CONFIG_XNU_BOOT_LOG=y      # → XNU_BOOT_LOG=1 → -force_load libUkeLastLog.a
```

`XNU_BOOT_LOG` 只管链接和 `-DXNU_BOOT_LOG=1`，源码里没有 `#if XNU_BOOT_LOG` 分支
（`grep` 确认），所以这次改动只影响"归档有没有被链接"。

这也解释了为什么 `UkeUsbLogDxe` 从 pram 里读不到内核日志：**pram 里本来就没有内核写的东西**。
修完之后 pram 才会有 msgbuf 尾部；在那之前，只有 XNU 自己 panic 时的
`uke_stage_last_panic` 512 字节会落在 pram 里。

---

## 35. stub 调用点 964 → 363 → 87，只剩 `_csr_check`（Round 24，emit 报告为证）

三个内核的 emit 报告（同一 matcher、同一 kernelcache）：

| 内核 | run / commit | anchor | `stubbed` 对数 | stub 调用点 |
|---|---|---|---|---|
| b56 | `42211693015` / `36d40d0` | 62 | 177 | 964 |
| b58 | `42212761913` / `a907ef7` | 182 | 31 | 363 |
| b60 | `42218599309` / `295c481` | 197 | 4 | **87** |

b60 剩下的 4 对全指向**同一个**目标：

```
 87 次  0xfffffe000bd44284  _csr_check   ← 已经在 anchor 里，但绑不上
   AMFI 63 / apfs 14 / sandbox 8 / IOGPUFamily 2
```

### 为什么绑不上

`_csr_check` 在 kernelcache 里是 68 字节的独立函数（`bsd/kern/kern_csr.c`），但**我们的重建内核里
根本没有这个符号**：

- `kernel.map.json` 里没有任何名为 `_csr_check` 的条目；
- `anchor_reviewed()` 对找不到的 native 符号是**静默跳过**（不报错），所以 anchor 写了也不生效。

原因是 LTO + dead-strip：内核里所有 in-tree 调用点都被内联，`_csr_check` 的函数体没有外部引用，
于是被 `-dead_strip` 删掉；而 fileset 里的 AMFI/apfs/sandbox 是**按符号名**调用它的。

### 修法（提交 `7fdbe2c`）

新增 `package/KeepOutOfLine.c`：把一个指向 `csr_check` 的函数指针放进一个活着的全局对象。
地址被活对象引用 ⇒ LTO 必须保留函数体 ⇒ 符号存在 ⇒ anchor 能绑上。
（原型必须与 `bsd/sys/csr.h` 完全一致 `int csr_check(uint32_t)`，否则 LTO 会以类型不匹配拒绝合并。）
这个文件被加进 `build-boot-log.py` 的 `sources`，随 `libUkeLastLog.a` 一起 `-force_load`
进内核 —— 也就是依赖 `CONFIG_XNU_BOOT_LOG=y`（§34）。

### 顺带澄清两个"看起来像坏消息"的字段

`kernel.json` 里的 `mapping_complete: False`、`loadable: False`、
`missing_address_space: 'output collection'` **都是 `fileset_emit.py` 里写死的字面量**
（第 736 行附近），不是这一版真的缺了什么。`personalities_dropped` 里的
`IOGraphicsFamily`/`IOPCIFamily` 是这个移植本来就不要的（无 PCI、legacy display 路径）。

---

## 36. 打开 `CONFIG_XNU_BOOT_LOG` 才发现的两个连锁问题（Round 24）

### (1) 归档一链接就暴露 `NativeUfs` 依赖 → run `42220507424` 失败

```
Undefined symbols for architecture arm64e.kernel:
  "_native_ufs_log_open",  referenced from: _uke_last_log_writer in lto.o
  "_native_ufs_log_write", referenced from: _uke_last_log_writer in lto.o
  "_native_ufs_log_sync",  referenced from: _uke_last_log_writer in lto.o
```

`UkeLastLog.c` 其实有**两半**：pram 写手（我们要的）和 userdata 窗口（需要 `NativeUfs`，
而 `uke-macos_defconfig` 里 `CONFIG_XNU_NATIVE_UFS` 是关的，`board.toml` 也把两条 UFS archive 注释掉了）。

修法：把 userdata 那半（含 `uke_snapshot`/`uke_crc32`/`ukeUfs*` 静态变量/三个外部声明）
包进 `#if UKE_LOG_USERDATA`（默认 0），并把 `tick++` 移到条件外面 —— 否则 `tick == 4`
（重复上次 panic）和 `tick == 240`（vnode 报告）永远不会触发。
用预处理器模拟 `UKE_LOG_USERDATA=0` 自查：`native_ufs_`/`ukeUfs`/`uke_snapshot`/`uke_crc32`
残留引用均为 **0**。提交 `8ba774f`。

### (2) `_csr_check` 是 LTO + dead-strip 的牺牲品（见 §35）

`package/KeepOutOfLine.c` + `build-boot-log.py` 的 sources + `CONFIG_XNU_BOOT_LOG=y`
三者缺一不可：归档不链接 → 引用的对象根本不进链接；`UKE_LOG_USERDATA` 不关 → 链接直接失败。

### 现在的排队情况

| run | commit | 内容 | 预期 |
|---|---|---|---|
| `42226020235` | `8ba774f` | 197 anchor + ArmCpu 继续 + pram 写手链接 + `KeepOutOfLine`(~0 stub) + userdata 关闭 | **b61 候选** |
| `42223121318` | `7fdbe2c` | 同上但 userdata 未关 | 会以同样的 undefined 失败 |
| `42218599309` | `295c481` | 197 anchor + ArmCpu 继续（无 pram 写手） | ✅ 已成功，`stubbed` 87 |

固件侧 b60/b61 已建好（1 MiB 栈 + Gunyah 喂狗 + 面板回显 pram 尾部），等内核到位即可打包。

---

## 37. 优先级纠正：先把启动链拿回来，USB 通道可后置（Round 24）

用户指出（正确）：为了 USB 读日志，结果连"走到 XNU"这一步都丢了。回顾这一串：

| 固件 | 结果 |
|---|---|
| b51（`UKE_VENDOR_HW=0`） | ✅ 走到 XNU，fileset panic 有画面 |
| b53/b54/b56（厂商栈开） | ❌ 派发厂商驱动时 256 KiB 栈溢出（有异常屏） |
| b57/b59（1 MiB 栈） | ❌ 黑屏后自己重启，什么都没走到 |

也就是说：**厂商驱动栈是启动链的障碍，而 USB 日志通道依赖它**。所以两者必须解耦，
启动链优先。

### 现在的默认配置

```
UKE_VENDOR_HW      = 0    # 默认关闭厂商栈（这一版能走到 XNU）
UKE_VENDOR_STORAGE = 1    # 与上面独立，将来开厂商栈时再分 UFS/SD 与否
```

`UkeUsbLogDxe` **保留在 FV 里**，因为它在没有 USB MSD 时会先

1. `DumpPreviousBootLog()` —— 把 pram 里上一次内核的日志搬进日志盘，
2. **并把最后 34 行打到面板上**（`--- previous boot log, last N lines ---`），
3. 然后 `LocateProtocol(usb-msd)` 失败 → 直接返回（不会再有 20 秒等待）。

于是即便没有厂商栈、没有 USB，**上一次内核日志的尾部仍然能拍照读到** ——
这正是当下最需要的能力。

### b61 镜像（厂商栈关闭 + 197 anchor 内核）

```
work/flash/vendor_boot_round-b61-anchors.img  dd69871d…  (85,782,528 B)
work/flash/boot-round-b61-vendorfree.img      4c6725e1…  (1,122,304 B；48 个模块，无厂商驱动)
```

---

## 38. `_csr_check` 的真因：我们的 arm64 配置根本不编译 `kern_csr.c`（Round 25）

上一轮以为 `_csr_check` 是"LTO 内联 + dead-strip 删掉了"，于是加了 `KeepOutOfLine.c` 去保活。
CI 直接反驳了这个判断（run `42226020235`）：

```
Undefined symbols for architecture arm64e.kernel:
  "_csr_check", referenced from:
      _uke_keep_csr_check in lto.o
```

保活**引用**它，链接器却找不到定义 ⇒ 符号从来没被编译出来。查到根源是**配置差异**：

```
config/MASTER.arm64.MacOSX:  SECURITY_BASE = [ config_macf config_audit kernel_integrity \
                                  config_csr config_csr_from_dt config_setuid ... ]   ← Apple 的 Mac
config/MASTER.arm64:         SECURITY_BASE = [ config_macf kernel_integrity config_secure_bsd_root ]  ← 我们用这个
bsd/conf/files:              bsd/kern/kern_csr.c   optional config_csr
```

所以 Apple 内核里有 `_csr_check`（AMFI/apfs/sandbox 调它 87 次），我们的内核里**连文件都没编**。

### 修法（提交 `796b57a`）

`package/KeepOutOfLine.c` 改名为 **`package/MissingKernelSymbols.c`**，职责改成
"提供这个配置不编译、但 fileset 按名字调用的内核入口"：

```c
__attribute__((weak)) int csr_get_active_config(uint32_t *config) { *config = 0xffffffff; return 0; }
__attribute__((weak)) int csr_check(uint32_t mask)               { /* (config & mask) == mask ? 0 : EPERM */ }
```

- **weak**：将来把 `config_csr` 加进 `config/MASTER.arm64`，XNU 自己的实现直接接管，不会重复符号；
- 语义是"全部允许"，与这块板子的现状一致（`boot_args` 里没有 CSR active config、
  payload 带 `amfi_get_out_of_my_way=1`）。

这一条修好后，b60 里最后 87 个 stub 调用点应当归零 —— 也就是
"fileset: original component called an unmapped kernel function" 这条 panic 的最后一个来源。

---

## 39. b61 实机：过了 fileset，撞上"没有 IOPlatformIO"的平台错误（Round 25，有实拍）

### 实拍（2026-11-06 21:56，b61 = 无厂商固件 + 197 anchor 内核）

屏幕上是 DarwinBoot 的绿色进度条 + 两行字：

```
Unhandled implementation specific error
```

### 这条字符串的出处（不是猜的）

```
osfmk/arm64/sleh.c:554:  panic_plain("Unhandled implementation specific error\n");
```

它属于 `arm64_implementation_specific_error()`，唯一调用者是
`arm64_platform_error()`（sleh.c:610）：

```c
if (PE_handle_platform_error(far)) {
        return;
} else if (cdp->platform_error_handler != NULL) {
        cdp->platform_error_handler(cdp->cpu_id, far);
} else {
        arm64_implementation_specific_error(state, esr, far);   /* ← 我们看到的 */
}
```

而 `PE_handle_platform_error()` 在 XNU 自己的 `iokit/Kernel/IOPlatformIO.cpp` 里：

```c
bool PE_handle_platform_error(vm_offset_t far) {
        if (platformIOProvider == NULL) { return false; }        /* ← 我们这里是 NULL */
        else { return platformIOProvider->handlePlatformError(far); }
}
```

`platformIOProvider` 只有在**某个 `IOPlatformIO` 子类被 start 过**之后才非空。`IOPlatformIO` 是
一个 abstract IOService（Apple 上由 `AppleARMIO` 之类实现），这个移植没有任何驱动去实现它，
于是**任何平台错误（SError / 异步 abort）都会变成这条不带 ESR、不带 FAR 的 panic**。

### 两个结论

1. **fileset 那条 panic 没了** —— 内核已经跑到自己的控制台、kext 在起来，这是 b51 之后第一次
   越过"M1"。这一轮真正要解决的问题换了：某个 SError。
2. SError 的现场被 XNU 丢掉了（panic 文本里没有 ESR/FAR），所以必须先把 handler 接上，
   否则下一轮也只能看到同一句话。

### 修法（提交 `64f6fdd`）

把 `ArmDTPlatform` 复制成本仓库的板级副本（`drivers/ArmDTPlatform`，feeds.toml 里 `local` 注册，
和 ArmCpu 同样的 first-wins 理由），并新增 `ArmDTPlatformIO : IOPlatformIO`：

- `handlePlatformError(far)` 打印 `ArmDTPlatformIO: platform error #N far=0x… pc=0x…`，
- 默认**返回 true（已处理）**让被打断的代码继续跑 —— 一个本移植还没有驱动的模块发的 SError
  不值得停住整个启动；
- `uke-platform-error-panic=1` 可恢复"交给机器层 panic"的旧行为。

### §39 补充：把 SError 的现场留住（提交 `1883928`）

`handlePlatformError()` 里那行 `IOLog` 会被后面 `-v` 的输出冲掉，而面板不能回滚。所以同一行文本
另外**写进 pram 的 panic 记录**：

```cpp
extern "C" void uke_stage_last_panic (const char *text) __attribute__((weak));
...
snprintf (text, sizeof (text), "ArmDTPlatformIO: platform error #%lu far=0x%llx pc=0x%llx", …);
IOLog ("%s%s\n", text, strict ? " (panicking as asked)" : " (ignored)");
if (uke_stage_last_panic != nullptr) { uke_stage_last_panic (text); }
```

- `uke-stage-last-panic.patch` 提供的 `uke_stage_last_panic()` 正是"把文本写进 pram"的入口，
  下一次启动 XNU 自己会打印它，`UkeUsbLogDxe` 也会把 pram 尾部回显到面板；
- 声明用 **weak**，所以没有那个 patch 的配置照样能链接（只是这行只留在当次日志里）。

于是下一轮无论 SError 是"被忽略后继续"还是"再次 panic"，`far=` 和 `pc=` 都会留下痕迹：
继续跑 → 下一次启动的面板回显能看到；panic → 屏幕上直接就有。

### 另外两个不需要 CI 的诊断手段（下轮可用）

1. `ArmDTPlatform` 内置**阶段探针**：`sheng-platform-stage=1..5` 会在
   `configure()` 的各个阶段做 PSCI 复位。设备在 stage N 复位 ⇒ 说明平台配置走到了 N，
   不用日志也能定位；
2. `ArmCpu` 的 `ArmCpu: <N> timer interrupts, <M> IPIs in the first 10 ms after registration`
   这一行现在默认就会打（`armcpu-probe=0` 不再冻屏），是"架构定时器有没有进
   `rtclock_intr`"的直接判据。

---

## 40. **fileset 未匹配调用 = 0**（Round 26，emit 报告为证）

`796b57a` 的 CI 绿灯（run `42229244579`），取回的 emit 报告：

```
b63 (197 anchors + csr_check provider):  stubbed pairs = 0   call sites = 0
kernel.macho 里: UkeLastLog 2 处、uke_panic_pram 2 处、csr_check 3 处
```

即：

- **没有任何 fileset 调用再落到 panic stub 上** —— 目标里"M1 越过 fileset 未匹配调用 panic"
  这一步在**产物层面**已经成立（设备层面的确认要等下一次实机，见 §39 的 SError）；
- `UkeLastLog`/`uke_panic_pram` 的字符串出现在内核里 ⇒ **pram 写手这次真的链接进去了**（§34 的修法生效）；
- `csr_check` 也在 ⇒ 最后 87 个调用点的宿主函数有了（§38）。

三个阶段的数据放在一起看：

| 内核 | anchor | stub 调用点 |
|---|---|---|
| b56 | 62 | 964 |
| b60 | 197 | 87 |
| **b63** | **197 + csr_check 实现** | **0** |

### 接下来就是 SError（§39）

b61 的实拍已经证明：fileset 之后的下一个拦路虎是平台错误（SError），而当时没有
`IOPlatformIO` 所以现场被丢掉。`ArmDTPlatformIO` 已经把这条补上，CI 里两次编译错误都是小问题
（`++volatile` 被 `-Werror,-Wdeprecated-volatile` 拒、`snprintf` 需要 `<libkern/libkern.h>`），
都已修（`67149e0`、`4ed407d`）。

### 顺带记两个"配置差异"结论（以后再遇到同类调用点直接用）

1. 我们的 arm64 配置（`config/MASTER.arm64`）和 Apple Mac 的
   （`config/MASTER.arm64.MacOSX`）`SECURITY_BASE` 不同：前者没有 `config_csr`/`config_csr_from_dt`，
   所以 `bsd/kern/kern_csr.c` 整个不编 —— 这类"Apple 有、我们没有"的符号，靠 anchor 是补不出来的，
   只能自己提供实现（weak 定义最省事）；
2. `kernel.json` 的 `loadable`/`mapping_complete`/`missing_address_space` 是写死的字面量，
   别拿它们当证据（§35）。

---

## 41. USB 日志：照同类项目做（Round 27，用户指出的方向）

用户的判断是对的：**面板打印本身不可靠**（会滚屏、会被覆盖），而且**同类 UniXNU 移植早就解决了
这个问题**。查 UniXNU 文档里的设备记录：

```
sirius / sirius-huajifruit / sirius-yooglery:
  "The panel and the serial console are the two evidence channels; since the DWC3 CDC console
   works, the host captures the kernel log directly and photographs are only a fallback."
  证据: 3813 bytes of kernel log read from /dev/ttyACM0 over the DWC3 CDC console
```

也就是说：**内核里做一个 DWC3 设备模式的 CDC-ACM 串口**，主机上就是一个 `/dev/ttyACM0`，
内核日志直接流到主机 —— 不经过 UEFI、不需要厂商 UEFI 栈（正是我们卡住的地方）。

### 它们的做法（sirius `board.toml`）

```
archives: "build-sirius-usb.py"      # 编译 kits/sirius/sirius_usb.c（65 KB）
kernel_args: "sirius_usb=2"          # 1 = 只探测，2 = 开控制台
patches/xnu/sirius-usb-console.patch # _cnputc / console_printbuf_putc 各插一行日志镜像
patches/xnu/sirius-console-ring.patch# bsd_init 里启动 console
```

### 我们已经移植过来的部分（提交 `95af5c8`）

`package/uke_usb.c` = sirius_usb.c + 本板自己的数字。**每个数字都对着
`device/dt/live-runtime.dtb` 核过**，不是照抄：

| 事实 | 本板 DT | 与 sirius(SM7250) 的差异 |
|---|---|---|
| DWC3 基址 | `ssusb@a600000 reg <0x0a600000 0x100000>` | 相同 |
| GCC | `clock-controller@100000 reg <0x100000 0x1f4200>` | size 更大（0x1f4200） |
| USB30 PRIM GDSC | `qcom,gdsc@139004` | **不同**（sirius 用 0xf004） |
| APPS SMMU | `apps-smmu@15000000` | 相同 |
| USB stream id | `iommus = <apps_smmu 0x40 0>` | **不同**（sirius 用 0x740） |
| PHY | `qcom,use-eusb2-phy` | 相同结论（走 UTMI PIPE 时钟） |

一处**刻意的不同**：`pur_ring` 不再由控制台补丁喂，而是**直接读内核 msgbuf**
（`uke_usb_ring_from_msgbuf()`，逻辑与 `UkeLastLog` 相同）。这样：

- 不需要任何 XNU 补丁（省掉 patch 应用失败的风险）；
- `printf`/`kprintf` 写的东西全都在里面，panic 也在；
- `bsd_init` 之后仍然继续喂 —— 这正是 sirius 第二个补丁要解决的问题。

`board.toml` 里加了 archive `"build-uke-usb.py"`（对象并入 `libUkeLastLog.a`，
因为本板 `CONFIG_XNU_SHENG_PMGR` 是关的，借不到 sirius 那个归档）和内核参数 **`uke_usb=1`**：
**本轮只探测**，打印 GCC 电源域状态与 DWC3 的 `GSNPSID`/`DSTS` —— 这回答了"XNU 到底能不能
看到这个控制器"，是后续 CDC-ACM 的前提。

### 顺带：用户这批实拍的第一张"内核自己的输出"

同一轮的照片里，**ArmCpu 的 GIC 探针真的在跑**（不是 UEFI，是内核里 kext 的代码）：

```
gic-probe: MPIDR_EL1 0x81000000  this core affinity 0x0
gic-probe: gicr +0x00000 typer 0000000000000001 aff 0x0 vlpi 0 last 0 waker 00000002 asleep 0 psleep 1 <=
gic-probe: … redistributor 网格 0x40000 步长，slot 0x0（0xfff = absent）
gic-probe: GICD_CTLR after the hold 0x00000002 (Grp1 1 ARE 1 DS 1 RWP 0)
```

`waker=0x2 / psleep=1` 说明**本核的 redistributor 仍处于 ProcessorSleep**，
`GICD_CTLR=0x2` 说明 Group1 已开、ARE=1。这是"ArmCpu 起来了、GIC 被摸到"的实拍证据。

### 这一轮修掉的 CI 错误

`ArmDTPlatformIO` 调 `uke_stage_last_panic`（weak）时，参考内核（RELEASE）里没有该符号，
直接 `b` 到 0 → `branch out of range`（run `42235545609`）。改成**先取地址再判空**的间接调用
（`void (*volatile stage)(const char *) = …`），提交 `ecf90ad`。

### §41 补充：入口与主机侧步骤（提交 `9a65ca3`）

移植过来的驱动一开始**没有入口**——sirius 是靠在 `bsd_init()` 里插一行调用（补丁）启动的，
而本移植刻意不带 XNU 补丁。改用 XNU 自带的启动项：

```c
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, uke_usb_startup);
```

`EARLY_BOOT/STARTUP_RANK_LAST` 正是 `UkeLastLog` 的写手在本核上启动的位置（已验证会跑），
而且比磁盘驱动更早 —— 于是控制台环形缓冲在 `bsd_init` 之前就已经在镜像 msgbuf。

驱动里所有 `printf` 都进 msgbuf，`UkeLastLog` 又会把 msgbuf 尾部搬进 pram，所以**探测打印的数字
即便主机没接上，也会留到下一次启动的面板回显里**。

### 主机侧（下一轮 `uke_usb=2` 成立后用）

```bash
# 设备用 USB 线连着（fastboot 本来就要连着），开机后：
ls /dev/ttyACM*                 # 应出现 /dev/ttyACM0
cat /dev/ttyACM0 > uke-kernel.log    # 环形缓冲每秒回绕重放一次，晚接也能拿到整段日志
```

驱动的 TX 线程每秒把读指针回绕一次，所以**迟到的连接也能拿到完整日志**（这是 sirius 那轮
debug 出来的、被记录为"load-bearing"的行为）。

---

## 42. 两个仓库、两个产物：固件与内核分别装了什么（Round 28 用户提问）

这一轮用户连问两次"无厂商固件是做什么的""uefi-uke 里有吗"，说明分工需要写清楚。

| 位置 | 谁编译 | 里面装的东西 |
|---|---|---|
| **固件**（UEFI FV）→ `boot.img` | `workspace/uefi-uke`（Mu-Silicium fork） | EDK2 核心 + 我们自己的 `UkeBootMemoryDxe`、`UkeUsbLogDxe`；**厂商 DXE 栈只在 `UKE_VENDOR_HW=1` 时进 FV** |
| **内核**（XNU + 自研 kext）→ `vendor_boot` | CI（`work/push/macOS-ForUke` 的 board.toml/archives） | `kernel.macho`：`UkeLastLog.c`（msgbuf→pram）、`uke_usb.c`（DWC3 CDC-ACM 串口）、ArmCpu/ArmDTPlatform 等 |

`uefi-uke` 里**有**：

```
Binaries/uke/QcomPkg/Drivers/      48 目录 / 49 个 .efi（从库存 uefi_a.img 抽出）
  UsbfnDwc3Dxe UsbConfigDxe UsbDeviceDxe UsbMsdDxe UsbInitDxe UsbPwrCtrlDxe UFSDxe ClockDxe PmicDxeLa …
Platforms/Xiaomi/ukePkg/{uke.dsc,uke.fdf,uke.dec,Include/*,Drivers/*,Tools/gen-vendor-binaries.py}
```

`uefi-uke` 里**没有**（那是内核侧的）：`package/uke_usb.c`、`scripts/build-uke-usb.py`、
`package/UkeLastLog.c`。

### 为什么"无厂商固件"

厂商栈只为"UEFI 阶段把日志当 U 盘给主机"服务（`UsbMsdDxe`）。实测它自己把启动搞挂：

- 256 KiB 固件栈被用穿（`SP=0xA7600F20`、`FAR=0xA7600F30` 落在唯一未映射的 4 KiB 页）；
- 栈加到 1 MiB 后变成黑屏 + 自己重启（Gunyah 虚拟看门狗窗口，已加喂狗但未验证）。

于是把"日志"这件事移到内核侧（DWC3 CDC-ACM），固件回到 `UKE_VENDOR_HW=0`——就是 b51 那种
能走到 XNU 的配置。代码和 `.efi` 都还在，将来要回到 UEFI 暴露 U 盘的路，只需把开关改回 1，
并解决栈与看门狗这两个已知问题。

### 移植驱动的编译回合（都是小问题，逐条修完）

| run | 错误 | 修法 |
|---|---|---|
| `42239274488` | `kmem_alloc_contig` / `KMA_*` 未声明 | 改静态页对齐缓冲（16 KiB，本来就要验证物理连续） |
| `42239610271` | 同上一组（另一份提交） | 同上 |
| `42242998639` | `aligned(PAGE_SIZE)` 不是整型常量 | 写 `4096` |
| `42246401123` | `GSNPSID` 未声明 | 该文件里它是字面量 `0xc120` |
| `42249199942`（`066eb74`） | — | 只读探测段（`uke_usb=1 uke_usb_power=11`） |

---

## 43. USB 串口驱动编译通过并入内核（Round 28，产物为证）

run `42249199942`（commit `066eb74`）**绿灯**。取回的产物：

```
kernel.macho sha256 = 4a57ae88bbf9dc5ae66b3667473029626f9a22691b1d90a23a00d5bb6ab0239c
kernel.macho 里: "UKE USB:" 124 处、uke_usb_power 1 处、UkeLastLog 2 处、csr_check 3 处
emit 报告: stubbed pairs = 0   call sites = 0
```

124 处 `UKE USB:` 字符串 ⇒ 那份 1888 行的 DWC3 CDC-ACM 驱动**真的编进内核了**（不是只编了归档），
同时 fileset 的 stub 仍然是 0。

### b67 镜像（本轮设备判据）

```
work/flash/boot-round-b67-vendorfree.img   4c6725e1…  (1,122,304 B；无厂商固件)
work/flash/vendor_boot_round-b67-usbprobe.img 3cb28fc3…  (85,782,528 B；b66 内核，含 USB 探测)
内核参数（已烘进 payload）: uke_usb=1 uke_usb_power=11
```

`uke_usb_power=11` 是**只读**探测：`ml_io_map(0xa600000)` 后只做 `dwc_rd`，打印

```
UKE USB: DWC3 id=… gctl=… dctl=… dsts=…
```

外加 `uke_usb=1` 的 GCC 电源域（GDSCR@0x139004）与 SMMU stream 探测。**判据**：

- `id` 高 16 位应为 `0x5533`（Synopsys DWC3）——说明 XNU 真的看见了控制器；
- `gctl` 的 bit12..13 == 2 —— 说明控制器还在 device 模式（ABL/fastboot 留下的状态）；
- 有这两条，才有资格往下走 `uke_usb=2`（CDC-ACM + `/dev/ttyACM0`）。

这些打印都走 `printf` → msgbuf → `UkeLastLog` 写进 pram，所以**下一次开机拍面板也能再看到一次**
（`UkeUsbLogDxe` 回显 pram 尾部 34 行）。

### 顺带说明：为什么厂商那 6 个 `Usb*Dxe` 不是替代品

它们是"**设备模式 + 大容量存储**"（给主机一块 U 盘），不是串口流；而且要满足 `UsbfnDwc3Dxe` 的
16 条 DEPEX 就得连带 48 个厂商 DXE 一起编进 FV。实测（b56/b57/b59）那套栈一次都没让机器走到 XNU：
先是栈被用穿，加到 1 MiB 后变成黑屏+重启。代码与 `.efi` 都在（`UKE_VENDOR_HW` 可切回），
但它是备选，不是主路。

---

## 44. b67 实机：ArmCpu/GIC 探头全绿，但 USB 探测行没上屏（Round 28 收尾）

### 实拍拿到的新事实（三段视频截图）

UniXNU 启动屏那一帧信息量最大：

```
DARWINBOOT: hand-off to the kernel
kernel:  va 0xFFFFFFF007004000  …
video:   pa 0xE2DA0000  vbar 0x100000000  pages 32 bpp
gic-probe: ID_AA64PFR0_EL1 … (AArch64 4 KiB granule supported)
gic-probe: GICv3 present …
gic-probe: GICD_CTLR 0x00000002 (Grp1 1 ARE 1 DS 1 RWP 0) host entry, Exc: stop timer
gic-probe: wrote 0x00000003, read back 0x00000003 after 0 polls: GICD_CTLR accepts writes
gic-probe: restored 0x00000002, read back 0x00000002 after 0 polls: ok ack 0
```

以及 redistributor 网格：

```
gic-probe: MPIDR_EL1 0x81000000  this core affinity 0x0
gic-probe: gicr +0x00000 … waker 00000002 asleep 0 psleep 1 <= this core
gic-probe: gicr +0x40000/0x80000/…/0x1C0000   aff 0x100…0x700
gic-probe: this core's frame is slot 0x0 of the 0x40000 grid (0xfff = absent)
gic-probe: holding this screen for 20 s so it can be photographed
```

结论（有实拍）：

1. **ArmDTPlatform/ArmCpu 真的在内核里跑起来了**，GICv3 被识别、`GICD_CTLR` 可写、8 个
   redistributor 全在网格里；
2. **本核 redistributor 仍是 `psleep 1 / waker 0x2`** —— 唤醒那一步没做/没生效，这就是
   "架构定时器进不了 `rtclock_intr`"最可能的直接原因（下面要查）；
3. 之后仍然是 `Unhandled implementation specific error`（平台错误/SError）；
4. 主机 `ls /dev/ttyACM*` **无匹配** —— 符合预期：本轮是只读探测（`uke_usb=1`），
   CDC 串口要 `uke_usb=2` 才有。

### 为什么没看到 `UKE USB:` 那几行

`uke_usb.c` 的入口是 `STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, …)` —— 比视频控制台**早**，
所以它的 `printf` 只进 msgbuf，不上屏；要读到得靠"**下一次启动**的空间回显 pram"。
而我们的回显原来只打**最后 34 行**：上一次启动的最后 34 行正好是 panic，早期探测行早被挤掉了。

### 修法（b68）

`UkeUsbLogDxe` 的面板回显改成**两段**：

```
--- previous boot log: first 12 of N kept lines ---     ← 早期探测（UKE USB:、ArmCpu…）在这里
--- and the last 24 ---                                  ← panic 在这里
```

并且回显后**停 15 秒**（新 PCD `PcdUkePanelHoldSeconds=15`，音量上键可提前结束），
因为 ReadyToBoot 之后紧接着就是 loader 自己的输出，不停一下这屏会被立刻冲掉。

### b68 镜像

```
work/flash/boot-round-b68-panelecho.img      6a62af9e…  (1,122,304 B)
work/flash/vendor_boot_round-b68-usbprobe.img 0621b532…  (85,782,528 B；b66 内核)
内核参数仍是: uke_usb=1 uke_usb_power=11（只读探测）
```

用法：**开机两次**。第二次启动时，`ReadyToBoot` 会把上一次内核日志的开头 12 行与最后 24 行
打在面板上并停住 15 秒 —— 那 12 行里应该有 `UKE USB: DWC3 id=…`、`UKE USB: GCC power=…`、
`UKE USB: SMMU …`。这正是"XNU 能不能看见 USB 控制器"的判据。

---

## 45. 别的 UniXNU 移植到底怎么接 USB（Round 28 用户提问，已核对源码）

生态里是**两代做法**，我这边一直做的是第一代。

### A. 老一代（在用）：每设备一份 kit 源码 + 两份 XNU 补丁

代表：`sirius` / `sirius-huajifruit` / `sirius-yooglery`（SDM710 / 骁龙 710）

```
board.toml  archives: "build-sirius-usb.py"      → 编译 package/sirius_usb.c（65 KB 自包含驱动）
            kernel_args: "sirius_usb=2"
patches/xnu/sirius-usb-console.patch             → _cnputc / console_printbuf_putc 各插一行 tee
patches/xnu/sirius-console-ring.patch            → bsd_init 里启动 console
```
主机读 `/dev/ttyACM0`，他们的证据日志是 3813 字节内核日志。
**这就是我移植进 `package/uke_usb.c` 的那条路。**

### B. 新一代（正在推广）：核心组件 `QcomXnuUsbDebug` + SoC профиль

代表：`alioth`（K40）、`munch-erodenine`（K40S），板级 `board.toml` 里：
`components = [… "QcomXnuUsbDebug"]`、`archives = [… "component:QcomXnuUsbDebug"]`、
`kernel_args = "qcom-usb-debug=1"`（双向控制再加 `qcom-usb-ctl=1`）。

组件源码在 `WQSQW/UniXNU-dby_w09/package/QcomXnuUsbDebug/`（110 处引用），结构：

```
kernel/QcomXnuUsbDebug.c    XNU 侧：映射、缓存、计时、CPU 约束；对外 qcom_usb_debug_init/write/poll
usb/Dwc3.c                  EP0、TRB、CDC 描述符与收发队列
soc/QcomUsbQualify.c        检查"固件是否已经准备好 USB"（电源/5 个时钟/复位/DEVICE role/PHY）
soc/QcomUsbDmaDomain.c      SMMU stream/context bank/页表/恢复
soc/<SoC>.h                 唯一的平台参数文件（见下）
patches/darwin22/           0001 console tee（PE 初始化/kprintf/printf/中央控制台/panic）
                            0002 worker（kminit 后启动轮询线程、可选正常重启线程）
scripts/                    capture.py / xnu_usb_control.py / screenshot.py（主机侧采集、status/reboot、PNG 截图）
```

**接入方式**：defconfig `XNU_QCOM_USB_DEBUG=1`，内核参数 `qcom-usb-debug=1`，
主机看到 `1209:316d "QcomXnuUsbDebug"` 即成了。munch 的记录：

> "`qcom-usb-debug=1` starts the CDC-ACM console over the board's DWC3 controller. It tees
> `printf`, `IOLog`, `kprintf` and the **panic drain** onto a USB serial port …
> **The firmware already brings the controller up (uefi-munch DXE) and this kernel has no other
> USB driver, so nothing competes.**"

**关键前提**：这条路的假设是"**固件已经把 DWC3 举起来**（DEVICE 模式、电源、时钟、复位、PHY）"，
内核只做 DMA/TRB/CDC 和日志 tee。soC 适配页明确列出要记录的项，其中一条正是我们踩过的坑：

> "对不可访问总线的读操作，软件预检查也可能遇到异步异常。"

—— 这就是 b61/b67 那个 `Unhandled implementation specific error`（SError）的来源：
我在内核里直接 `ml_io_map` + 读 GCC/DWC3 寄存器，而没有先确认固件是否把它们上电/开时钟。

### 对 uke 的结论（下一步）

1. **改走 B**。`soc/Uke.h` 需要的值我已经从 `device/dt/live-runtime.dtb` 核出 6 项：

   | 项 | 值 | 来源 |
   |---|---|---|
   | `QUD_DWC3_BASE/BYTES` | `0x0a600000 / 0x100000` | `ssusb@a600000 reg` |
   | `QUD_GCC_BASE/BYTES` | `0x00100000 / 0x1f4200` | `clock-controller@100000 reg` |
   | `QUD_SMMU_BASE/BYTES` | `0x15000000 / 0x100000` | `apps-smmu@15000000 reg` |
   | `QUD_STREAM_ID` | `0x40` | `usb0 dwc3 iommus = <apps_smmu 0x40 0>` |
   | `QUD_DMA_IOVA` | `0x90000000` | `qcom,iommu-dma-addr-pool = <0x90000000 0x60000000>` |
   | `QUD_PSCI_SMC_RESET` | 1（待正式确认） | `ArmDTPlatform::platformStageReset` 用过 SMC `0x84000009` |

   **还缺的**：cliffs(SM7675) 的 `GCC_CBCR_OFFSETS{5}`、`BCR`、`GDSCR`、`CFG_GDSCR` 与
   `QSCRATCH_HS_PHY_CTRL_OFFSET`。DT 只给出 `gcc_usb30_prim_gdsc = qcom,gdsc@139004`
   （对 SM8550 是 0x39004，**布局不同，不能照抄**）。
2. **固件必须先准备 USB**：在 uke 的 FV 里只开 **USB function 子集**
   （`UsbConfigDxe`/`UsbDeviceDxe`/`UsbfnDwc3Dxe`/`UsbInitDxe`/`UsbPwrCtrlDxe`），
   不带 UFS/charger/MSD —— 这也回避了 b56 那次整套 48 驱动把栈用穿的问题。
3. **补丁版本**：组件自带的是 `patches/darwin22`（XNU 8796.141.3）；我们是
   `10063.141.1 / Darwin 23.6.0`，需要在 `patches/darwin23` 增一份 profile（`scripts/xnu_patches.py` 里加版本配置）。

---

## 46. uke 接 QcomXnuUsbDebug：SoC 配置已写完并通过断言（Round 28）

按用户指示"直接照别人的做"，把组件原样取下来，只补一个 SoC 配置。

### 组件本体已入库（板级副本，952 KB）

```
drivers/QcomXnuUsbDebug/           ← WQSQW/UniXNU-dby_w09 package/QcomXnuUsbDebug（未改动）
  kernel/QcomXnuUsbDebug.c  usb/Dwc3.c  soc/{QcomUsbQualify,QcomUsbDmaDomain}.c
  soc/{Sm8550,Sm8250,Uke}.h  include/  scripts/  tests/  fixture/
  patches/darwin22/  patches/darwin23/        ← darwin23 已按 xnu-10063.141.1 重基（正是我们的 XNU）
```

**关键确认**：`docs/port-sm8250.md` 的标题就是
"SM8250 / **xnu-10063.141.1** port" —— 说明补丁集已经适配到我们这个 XNU 版本，
不需要再自己写一份 patch profile。

### `soc/Uke.h`（新文件，值全部有出处）

| 宏 | 值 | 出处 |
|---|---|---|
| `QUD_GCC_BASE/BYTES` | `0x100000 / 0x1f4200` | live.dts:13381 `clock-controller@100000` |
| `QUD_DWC3_BASE/BYTES` | `0xa600000 / 0x100000` | live.dts:15967 `ssusb@a600000` |
| `QUD_SMMU_BASE/BYTES` | `0x15000000 / 0x100000` | live.dts:15265 `apps-smmu@15000000` |
| `QUD_STREAM_ID` | `0x40` | live.dts:16001 `iommus = <0x129 0x40 0>` |
| `QUD_DMA_IOVA` | `0x90000000` | live.dts:16000 `qcom,iommu-dma-addr-pool` |
| `QUD_GCC_CBCR_OFFSETS` | `{0x39018,0x39024,0x39028,0x39060,0x39064}` | `gcc-cliffs.c` 的 `halt_reg` |
| `QUD_GCC_CBCR_REQUIRED` | `3` | 本板 `qcom,use-eusb2-phy`，无 SS PHY（aux 时钟可停） |
| `QUD_GCC_GDSCR_OFFSET` | `0x39004` | live.dts:15764 `qcom,gdsc@139004` 是**绝对址** − GCC 基址 |
| `QUD_GCC_CFG_GDSCR_OFFSET` | `0x39008` | GDSCR+4；且该 GDSC 带 `qcom,support-cfg-gdscr`（live.dts:15769） |
| `QUD_GCC_BCR_OFFSET` | `0x39000` | 同代布局（gdscr = bcr+4），**待实机确认** |
| `QUD_QSCRATCH_HS_PHY_CTRL_OFFSET` | `0xf8810` | wrapper 0xa6f8800 + `QSCRATCH_HS_PHY_CTRL 0x10` |
| `QUD_PSCI_SMC_RESET` | **0** | 本板从未观察到 stage 探针真的重启，按模块规定置 0（只影响 reboot 命令） |

**校验**：用组件自己的 `include/QcomUsbSoc.h`（含 12 条 `_Static_assert`：页对齐、窗口不重叠、
偏移在窗口内、SID ≤ 0x7fff、IOVA 2 MiB 对齐 …）编译通过：

```
cc -std=c11 -I. -Iinclude -DQUD_SOC_HEADER='"soc/Uke.h"' …  →  exit 0
```

也就是说 **cliffs 的 GCC 布局与 Kalama(SM8550) 同代**：五个 CBCR 与 GDSCR/CFG_GDSCR/BCR 的相对
关系完全一致，唯一实质差异是 GCC 窗口更大（0x1f4200）。这也解释了为什么之前我在内核里手搓
GCC 探测会踩到总线异常——**那条路本来就不该由内核走**。

### 还差三件事（下一步）

1. **固件要先把 USB 举起来**：在 uke 的 FV 里只加 USB function 子集
   （`UsbConfigDxe`/`UsbDeviceDxe`/`UsbfnDwc3Dxe`/`UsbInitDxe`/`UsbPwrCtrlDxe`），不带 UFS/charger/MSD；
2. **接线**：`feeds.toml` 里注册板级副本（同 ArmCpu）、`board.toml` archives 加
   `"component:QcomXnuUsbDebug"`、defconfig `XNU_QCOM_USB_DEBUG=1`、内核参数
   `qcom-usb-debug=1`（双向控制再加 `qcom-usb-ctl=1`）；
3. **主机侧**：组件自带 `scripts/collect-xnu-usb.py`（采集）、`xnu_usb_control.py`（status/reboot）、
   `screenshot.py`（**内核里抓屏出 PNG**）。

---

## 47. 有了 root adb：`logfs` 是真·可回读的启动日志通道（Round 28）

用户给了关键能力：**Android 侧有 root adb**。按这个前提重新看，发现设备本来就有一条持久化日志通道。

### `logfs` 是 FAT12，里面是 XBL/ABL/UEFI 的启动日志

```
adb shell su -c 'dd if=/dev/block/by-name/logfs of=/sdcard/logfs.img'   # 8 MiB
```

解析出来是 **FAT12**，含 `UEFILOG0..4.TXT` / `EFILOG0..4.TXT`（各 64 KiB，轮转）。内容开头：

```
S - QC_IMAGE_VERSION_STRING=BOOT.MXF.2.1-01919.2-LANAI-1.78639.7
S - IMAGE_VARIANT_STRING=SocPalawanLAA
S - Boot Interface: UFS
S - Secure Boot: On
```

每条启动都记了**复位原因**，例如：

```
B - 551623 - PM: Reset by PWR key S2      /  PM: Reset Type: Hard Reset
B - 551501 - PM: Reset by PSHOLD          /  PM: Reset Type: Shutdown
D - 214    - boot_wdog_disable_check      ← ABL 每次都检查/关闭看门狗
QcomChargerDxe:: ChargerPlatform_Init Enabled WDOG.   ← 厂商 UEFI 的充电驱动会“打开”看门狗
```

**两条新结论**：

1. **ABL 自己会关看门狗**（`boot_wdog_disable_check`），但**厂商 UEFI 的 `QcomChargerDxeLA` 会再开一个**
   —— 我们开 `UKE_VENDOR_HW=1` 的那几版（b53/b54/b56）正好把它编进去了，而没人喂它。
   这与"黑屏后自己重启"高度吻合；厂商栈回归时必须排除 charger 组或喂狗。
2. `logfs` 里的日志**没有我们 payload 的痕迹**（`grep -i 'darwinboot|unixnu'` = 0 行），
   因为写日志的是 XBL/ABL 与厂商 UEFI 的 logging DXE，而我们的 Silicium FV 没有那个驱动。
   → 若把 `BootloaderLogging`/`ULogDxe` 加进 FV（需要 UFS 写），**UEFI 阶段的日志就能持久化到
   `logfs`，重启进 Android 后 `adb pull` 直接读** —— 比 USB CDC 更省事的一条后路。

### `oops` 分区里只有 Android 自己的重启记录

`oops.img`（16 MiB）解析出 7 条 `## REASON: Restart` + `LAST KMSG`，时间戳都是 11-03..11-06 的
Android 重启；**没有**我们 payload 的崩溃记录（那些启动从未进 Android）。

### 顺带用 dmesg 核对了 `soc/Uke.h`

```
msm-dwc3 a600000.ssusb            → DWC3 基址 0xa600000 ✓
msm_eusb2_phy 88e3000.hsphy       → 只有 eUSB2 PHY（无 SuperSpeed）✓ → CBCR_REQUIRED=3 正确
eusb2-repeater …pm7550ba@7…       → eUSB2 repeater 在 PMIC 上（SPMI）
dwc3 a600000.dwc3: Adding to iommu group 47
msm-dwc3: DWC3 in/out of low power mode   → Android 自己完整冷启控制器（固件并不留给它）
```

最后一条很重要：**Android 的 dwc3 驱动自己是冷启动的**（时钟 + PHY + repeater 全自己来）。
所以 "固件已准备好 USB" 这个前提在 uke 上**默认不成立**，必须靠我们固件里的厂商 USB 子集去准备。

### 组件接入的三步已全部落到代码里（提交 `b1fecfe`）

```
drivers/QcomXnuUsbDebug/soc/Uke.h          ← 新 SoC 配置（已通过组件静态断言）
scripts/build-qcom-usb-debug.py            ← 用本树编译记录 + xcrun 的 llvm-ar/nm 构建归档
patches/xnu/zzzzzzzzzzzzzzz-uke-usb-*.patch ← 组件自带 darwin23 补丁（已按 10063.141.1 重基）
patches/xnu/series                          ← 追加在最末（与 panic/ramoops 补丁同文件）
configs/uke-macos_defconfig                 ← CONFIG_XNU_QCOM_USB_DEBUG=y
scripts/build-xnu-4k-baseline.sh            ← -DXNU_QCOM_USB_DEBUG=1 + include 目录 + -force_load 归档
board.toml                                  ← archive: "build-qcom-usb-debug.py"
```

运行时仍是 opt-in（`qcom-usb-debug=1`）。**下一步**：固件侧只开 USB function 子集
（`UsbConfig`/`UsbDevice`/`UsbfnDwc3`/`UsbInit`/`UsbPwrCtrl`，**排除 charger 组**——见上面第 1 条），
让 qualify 阶段能通过。

---

## 48. 固件侧 USB 子集 + 继承状态探针（Round 28，b70）

### `UKE_VENDOR_USB`：只带 USB 需要的厂商驱动

先回答"要哪些驱动"这个问题，机制而不是猜测：`work/analysis/fw-subset.py` 把候选子集里每个
驱动自己的 DEPEX 求一遍，再检查每个协议 GUID 在子集内有没有生产者（GUID 出现在某个模块的
PE32 里才算提供者），最后报 `unsatisfied`。

结论：**31 个厂商模块 + 6 个 USB 模块 = 依赖闭合，unsatisfied: 0**。

```
UsbfnDwc3Dxe 的 DEPEX 一共 16 项，其中只有 4 项不是核心 UEFI 协议：
  fa5f306b-…（PmicDxe 提供）     157a5c45-…（UsbPwrCtrlDxe 提供）
  f4e5c7d0-…（EFI_SMEM_PROTOCOL，SmemDxe 提供）   18a031ab-…（DxeCore）
UsbDeviceDxe  -> fa5f306b + SMEM
UsbConfigDxe  -> PmicDxe 的协议 + SMEM + ClockDxe 的时钟协议 + ACPI SDT + b0760469 + 157a5c45
UsbInitDxe    -> e722b03f-…（UsbConfigDxe 提供）
UsbMsdDxe / UsbPwrCtrlDxe -> DEPEX TRUE
```

所以 `uke.dsc` 现在有三个开关（都只影响 `Include/{DXE,APRIORI}.inc` 里的块）：

| 开关 | 含义 |
|---|---|
| `UKE_VENDOR_HW=1` | 整套厂商 fleet（不含 storage） |
| `UKE_VENDOR_USB=1` | 时钟/电源/PMIC 底座 + USB function 栈 |
| `UKE_VENDOR_STORAGE=1` | 再加上 UFS/Sdcc |

`UKE_VENDOR_USB=1` 编出来的镜像里，厂商模块 **31 进 / 18 出**，被排除的正是
`QcomChargerDxeLA`（logfs 证明它 `ChargerPlatform_Init Enabled WDOG.`）、`ChargerExDxe`、
`PmicGlinkDxe`、`UsbMsdDxe`、`UFSDxe`、`SdccDxe`、`PILDxe/PILProxyDxe/SPSSDxe`、`GpiDxe`、
`SPIDxe`、`TsensDxe`、`LimitsDxe`、`DDRInfoDxe`、`ButtonsDxe`、`FeatureEnablerDxe`、`RngDxe`、
`RscDxe`。保留的 USB 侧是 `UsbfnDwc3Dxe`、`UsbPwrCtrlDxe`、`UsbDeviceDxe`、`UsbConfigDxe`、
`UsbInitDxe`、`UCDxe`。

### 为什么不再需要"猜"内核能不能接管：固件自己把状态打出来

`drivers/QcomXnuUsbDebug/soc/QcomUsbQualify.c` 的内核侧检查是：GDSC 上电、BCR 复位已释放、
五个 CBCR 里前三个没有 CLK_OFF、`GSNPSID` 可读且是 0x5533/0x3331、**DCTL.RUN_STOP=0 且
DSTS.DEVCTRLHLT=1**（`halt_inherited()`）。最后一条是关键：厂商 USB 栈不会在
ExitBootServices 里停控制器，所以固件必须自己停。

`UkeUsbLogDxe` 因此新增两件事，全部在 ReadyToBoot（面板上看得见）：

```
UKE-QUD: gcc bcr=… gdscr=… cfg_gdscr=…        <- 先只读 GCC（常开时钟域）
UKE-QUD: cbcr … … … … …
UKE-QUD: NOT INHERITABLE pwr=0 rst_deasserted=1 clocks=1/3 (DWC3 not touched)
   或  UKE-QUD: dwc3 gsnpsid=5533xxxx gctl=… phycfg=…
UKE-QUD: dwc3 dcfg=… dctl=… dsts=… run_stop=1 halted=0
UKE-QUD: halt 3 ms dctl=… dsts=… halted=1
```

读数顺序照抄 qualifier：**没证明电源/时钟之前绝不去读 DWC3 窗口**（不可访问总线的读是
异步异常，以前的黑屏就是这么来的）。另外注册了 ExitBootServices 事件再清一次 RUN_STOP，
因为厂商栈自己的定时器在 ReadyToBoot 之后还可能把它设回去。

两个面板窗口的停留都加了 `UkeWatchdogPet`：Gunyah 的虚拟看门狗在 ~30 s 会复位没人喂的启动，
而这两段停留（8 s + 10 s）正好在窗口里。

### 顺手补上的一个真 bug

`MemoryMapLib` 里 **没有 GCC（0x00100000）这一段**，而 `ClockDxe` 和这次的探针都要读它。
补上 `{"GCC", 0x00100000, 0x00200000, AddDev, MMIO, UNCACHEABLE, MmIO}`。
（live.dts `gcc: clock-controller@100000 reg <0x100000 0x1f4200>` 与 `soc/Uke.h` 完全一致。）

### 产物与证据

```
work/flash/boot-round-b70-usbsubset.img   1548288 B  sha256 69b597cba8d3cb391c606f11a64edbfd02ad4db513079f43a280e047dde63947
  内含 FD sha256 a82407bf7cf5eae3e1547b02dbdb97ea655a3305a581ab2eb87ac63242eefe10（= 本地新编的 FV）
  boot.img 头：v4 / kernel_size 1538652 / ramdisk 0 / 末尾 DTB d00dfeed
  与 b51（真机启动过）的 boot.img 头结构完全一致；`unixnu bootimg` 打印的
  "header does not match" 是它 verify() 按 v2/v3 布局解析 v4 头的老问题，不是镜像坏了
```

`vendor_boot` 不换：`work/flash/vendor_boot_round-b68-usbprobe.img`（b66 内核），本轮只动固件。

### 内核侧那个失败（不修就没法进下一轮）

CI `build-uke-kernel-min`（run 42263483616, `b1fecfe`）在 **Pass 1** 就挂了：

```
== kernel: patch zzzzzzzzzzzzzzz-uke-usb-console.patch
error: patch failed: pexpert/arm/pe_kprintf.c:13
error: patch failed: osfmk/kern/debug.c:1133
error: patch failed: osfmk/kern/printf.c:911
```

170 个补丁里它是**最后一个**，所以是前面某个补丁把上下文改了。本地取证已经开始：
把 xnu-10063.141.1 拉到 `work/analysis/xnu-src`，按 `patch_series = [arch, os, device]` 的
真实顺序重放全部 170 个补丁，定位冲突点后重新生成这两份补丁（组件文档明确要求：
**旧的按设备 USB 补丁和新的组件补丁不能叠**）。

### 48b. 真正的原因：这棵树的内核是 xnu-12377.121.6，不是 10063.141.1

`UniXNU/profiles/os/macos-26.5.2-25F84/profile.toml` 写着 `"xnu_version" = "12377.121.6"`
（同一文件解释：24A446/25F84 跑的是 xnu-12377 内核），设备补丁序列里也全是
`patches/xnu-12377/…`。而 `QcomXnuUsbDebug` 只带 **darwin22（8796）** 与 **darwin23（10063）** 两套补丁。
CI 报的三处失败里，`pe_kprintf.c`、`printf.c`、`serial_*.c` 在前 169 个补丁里**没有任何人去动**，
所以那不是"被前面的补丁改坏了"，而是**版本本身不对**。

⇒ 结论：要用这个组件，得先把它的两份补丁移植到 12377（那是独立的一轮），而不是改一行路径。
在那之前，板子自己的 `package/uke_usb.c`（读 msgbuf、不需要任何 XNU 补丁）就是 USB 控制台，
而它正需要这次固件做好的"电源/时钟/PHY 已就绪"交接。

已同步改动（提交 `7245b1e`，CI run `42265823188`）：

```
patches/xnu/series            <- 两份组件补丁注释掉，并写明原因与失败的三个文件
configs/uke-macos_defconfig   <- CONFIG_XNU_QCOM_USB_DEBUG=n
board.toml                    <- "build-qcom-usb-debug.py" 注释掉（配置关了就没人链接它）
drivers/QcomXnuUsbDebug/、scripts/build-qcom-usb-debug.py、soc/Uke.h、build-xnu-4k-baseline.sh 的开关 保留
```

---

## 49. b70 真机结果：厂商 fleet 走到 GlinkDxe 就死，原因是 SMP2P 没编进镜像

用户实测 `boot-round-b70-usbsubset.img`（`fastboot boot`）后，面板上最后一次是主板厂商驱动**逐个
被 dispatch**（这是从来没有过的进度：EnvDxe/ReportStatusCode/RuntimeDxe/ArmCpu/ArmGic/Metronome/
ArmTimer/Smem/DALSys/HWIO/ChipInfo/PlatformInfo/ProjectInfo/HALIOMMU/ULog/IPCC 全部通过），
然后：

```
Loading driver at 0x000C25B3000 EntryPoint=0x000C25B4000 GlinkDxe.efi
→ 黑屏，自动重启，连 unixnu 都没出来
```

### 根因：DEPEX 分析漏掉了"消费者不是提供者"

`GlinkDxe` 的 DEPEX 只有一个协议：`52858ad7-8b16-4137-9c64-dfe04d942e80`。用
`work/analysis/guid-providers.py` 查谁带这个 GUID：

```
52858ad7-…: GlinkDxe, IPCCDxe, Smp2pDxe
```

`Smp2pDxe` 在 stock 镜像里，但**我们的 `gen-vendor-binaries.py` 模块表从来没列过它**，所以镜像里
没有它。我原来的 `fw-subset.py` 把"GUID 出现在 PE32 里"当提供者，于是 IPCCDxe/GlinkDxe 自己也被
算成提供者 → 报 `ok`，真实缺口被掩盖。**APRIORI 里的驱动不等 DEPEX 就被 dispatch**，所以
GlinkDxe 的 entry 照跑，`LocateProtocol` 拿到 NULL，vendor 代码直接解引用 → data abort → 挂死 →
看门狗复位。这就是"黑屏 + 重启 + 一行输出都没有"。

修好的两条规则（都已写进工具）：

1. `providers()` 排除"自己的 DEPEX 里也等这个 GUID"的模块（等它的人不是给它的人）；
2. 只剩自己一个候选时报 `SELF-ONLY`（并列出 stock 镜像里但不在子集里的候选），不再默默算过。

### 同时加了心跳，让下一轮不用再猜

两个假设（a 某驱动挂死 / b Gunyah 看门狗 ~30 s 复位）在照片上长得一模一样，所以新增
`Drivers/UkeProgressDxe`，并且**插在 APRIORI 里 ArmTimerDxe 之后、所有厂商驱动之前**：

- 每 1 秒 `smc 0x86000007` 喂 Gunyah 看门狗（和 Android `gh_virt_wdt` 同一条）；
- 每 1 秒在面板**同一行原地**改写 `UKE-BEAT <n>`（`\r` 不换行），所以 dispatcher 打印的
  "Loading driver at …"（定位卡在哪一行的唯一证据）不会被滚掉；
- 每 10 拍把计数写进 `0xA7000000`（MemoryMapLib 的 "FD Reserved"，本固件所有、FD 不用），
  下次启动还能读到"上一轮走到第几拍"。

判读方法：**数字在涨 = CPU 活着，某个驱动在等永远不来的东西（stall）；数字冻住 = CPU 卡死在
最后那行驱动里**。

### 产物

```
work/flash/boot-round-b71-beat.img   1556480 B  sha256 3b3188982db83ed322dbd3565a874bd9aea62fb2f01fe5f3a296e165cccda33d
  FD sha256 0695ca676f76517c…（32 个厂商模块 + Smp2pDxe + UkeProgressDxe，55 images verified）
```

顺带一个 FDF 坑：**只把 INF 写进 `APRIORI.inc` 不会把 FFS 放进 FV**（GUID 只出现在 APRIORI 文件里），
必须同时在 `DXE.inc` 里列一次，否则 APRIORI 指向一个不存在的 FFS。

---

## 50. 内核侧其实已经连续红了三轮：`uke_usb.c` 根本没编过

查 run 列表才发现：**最后一次成功的内核产物是 `066eb74`（16:06，b66）**。之后：

| run | commit | 结果 | 原因 |
|---|---|---|---|
| 42257066165 | `3bed560` | failure | — |
| 42259821603 | `538854f` | failure | — |
| 42263483616 | `b1fecfe` | failure | 组件补丁版本不对（见 §48b） |
| 42265823188 | `7245b1e` | failure | `uke_usb.c` 编译错误 |
| 42269780292 | `33738cf` | 进行中 | 已修下面两处 |

`42265823188` 的日志说得很清楚（archive-16-build-uke-usb.log）：

```
kits/uke/uke_usb.c:2009:2: error: call to undeclared function 'IOSleep'
kits/uke/uke_usb.c:2030:6: error: call to undeclared function 'kernel_thread_start_priority'
kits/uke/uke_usb.c:2030:57: error: use of undeclared identifier 'BASEPRI_KERNEL'
```

- `IOSleep` 在 `<IOKit/IOLib.h>`，而这个归档的编译行是**借用 `bsd_init.o` 的记录**，那行不带 IOKit 头
  → 改成 80 次 `delay(100*1000)`（`<machine/machine_routines.h>` 本来就在 include 里）。
- `kernel_thread_start_priority` + `BASEPRI_KERNEL`（`<kern/sched.h>`）在这棵树里不再公开
  → 改用 `kernel_thread_start`（同一文件 1650 行早就在用）。

也就是说 b68/b69 那两轮"探针没出来"，**不是设备上的结论，而是内核根本没编出来**。
现在 b68 的 vendor_boot 用的仍是 b66 内核，所以 b70/b71 的固件轮次不受影响。

---

## 51. b71 结果：进度+10 个驱动，但**心跳一声都没响** —— 中断根本没在跑

用户实拍（`boot-round-b71-beat.img`）：`UkeProgressDxe` 确实在 APRIORI 里被 dispatch 了
（ArmTimerDxe 之后那一行），`Smp2pDxe` 也进来了，然后**比 b70 多走了 10 个驱动**：

```
… ArmCpuDxe / ArmGicDxe / MetronomeDxe / ArmTimerDxe /
  UkeProgressDxe / SmemDxe / DALSys / HWIODxeDriver / ChipInfo /
  PlatformInfoDxeDriver / ProjectInfoDxe / HALIOMMU / ULogDxe / IPCCDxe / Smp2pDxe ← 最后一行
```

三条结论：

1. **依赖修对了**：GlinkDxe 之前缺的那一环补上后，IPCC/Smp2p 都能过（b70 是 IPCC 之后直接死）。
2. **但终点只前进了一格**：现在死在 `Smp2pDxe` 或它之后（GlinkDxe 没打印出来）。
3. **屏幕上一个 `UKE-BEAT` 都没有** ⇒ 我那个 1 秒定时器事件**从未 fire**。这不是心跳写错，
   而是**这个 UEFI 里 timer event（= 中断）根本没投递**。这个事实比崩溃点更重要：所有依赖时间
   的诊断（包括喂看门狗）都跟着失效。

（旁证：`gBS->Stall` 用的是 TimerDxe 的计数器轮询，不需要中断，所以面板停留、`delay` 都还正常
—— 这就是为什么"喂狗"必须改成挂在 Stall 上。）

### b72 的两处改动

1. `UkeProgressDxe` 不再依赖中断：
   - **包裹 `gBS->Stall`**：厂商驱动所有的超时等待都是 Stall，于是每次等待都（a）喂一次 Gunyah
     看门狗、（b）计数、（c）每累计 10 秒 Stall 打一行 `UKE-BEAT stalls=… time=…ms timer=…`；
   - 启动时先做一次**测量**：arm 一个 100 ms 周期 timer，然后原地 `Stall(2 s)`，打印
     `UKE-BEAT timer probe: N callbacks in 2 s` —— 直接把"中断通不通"写在面板上；
   - 计数同时写进 `0xA7000000`（下次启动还能读）。
2. **把 IPC 三件套整个拿掉**（`IPCCDxe`/`Smp2pDxe`/`GlinkDxe`）：它们是 RPM/PMIC-glink 通道，
   本镜像不需要（USB 只要 SPMI/PMIC、GCC 时钟、DWC3 function）。拿掉后 `fw-subset.py` 仍报
   `unsatisfied: 0`（唯一那条是 ACPI_SDT，由核心 `AcpiTableDxe` 提供，是工具看不到 stock 里的
   该模块）。而且**APRIORI 里的驱动不等 DEPEX 就被 dispatch**，所以少列一个只会让后面真的需要它的
   驱动按正常 DEPEX 排队等（安全失败），不会再出现"拿 NULL 就解引用"的崩溃。

```
work/flash/boot-round-b72-noipc.img   1536000 B  sha256 5d0d91e95d25ebded32a4f17af08b2dcf5409465b7836671c87c4dfdac0432a5
  FD sha256 23ae3ebca41ed67d…（55 images verified）
```

---

## 52. b72 卡住的是我自己的 ASSERT，不是硬件

面板最后一行：

```
ASSERT [UkeProgressDxe] UefiLibPrint.c(201): Console != ((void *) 0)
```

`UkeProgressDxe` 在 APRIORI 里排在 `ArmTimerDxe` 之后，那时 **`gST->ConOut` 还不存在**
（控制台是后面的 GraphicsConsole/ConSplitter 发布的），而 `AsciiPrint` 走的 `UefiLibPrint`
第一件事就是 `ASSERT(Console != NULL)`，DEBUG 构建下直接 `CpuDeadLoop` → "卡住"。

**结论：APRIORI 早期不能打印面板文本。** 但 dispatcher 自己的 `Loading driver at …` 行是有的，
说明它走的是 **DebugLib**（`DEBUG((DEBUG_INFO,…))`）而不是 ConOut —— 所以心跳改成 DEBUG 输出，
与 dispatcher 同一条路。

顺带纠正我上一轮的判断：b71 里 `UKE-BEAT` 一次没出现，**不能**直接推出"中断没投递"——那一版
`OutputString` 前有 `gST->ConOut != NULL` 的检查，所以它只是静默地什么都没打印。计时器到底通不通，
b73 里由 `UKE-BEAT timer probe: N callbacks in 2 s` 直接量出来（arm 100 ms 周期事件 + 原地
`Stall(2 s)`）。

### b73

```
work/flash/boot-round-b73-beat2.img   1536000 B  sha256 48988d4429dafdf331d263318a700278a53d66aff9363e56618b628964f88b7b
  FD sha256 771f77e0cfdca479…（模块里 3 条 "UKE-BEAT" 字符串，且不再引用 UefiLibPrint）
```

同时把 IPC 三件套移出 APRIORI/DXE（见 §51），所以这次应该越过 `Smp2pDxe` 那一格，往
`CmdDbDxe → PwrUtilsDxe → NpaDxe → RpmhDxe → ClockDxe → … → PmicDxe → Usb*` 走。

---

## 53. b73 的异常其实是 **Sec 阶段** 的，而且我上一轮的归因错了

面板：

```
Project Silicium for Xiaomi Pad 7
Firmware Version 4.0 Build at 12:28:05 on Nov 6 2026
Synchronous Exception at 0xA700DFAC
```

这行 banner 来自 **`Silicon/Silicium/SiliciumPkg/Sec/Sec.c:50`**（`PrintFirmwareVersion`，它会先
`ClearFrameBuffer`），而那行异常来自 **`Silicon/Silicium/SiliciumPkg/Sec/AArch64/ArchSec.c:22`**
（Sec 自己的早期异常向量，只打印 LR 就 `CpuDeadLoop`）。所以：

- 这是**最早的固件阶段**（DxeCore 起来之前，`CpuDxe` 还没接管异常向量，所以没有寄存器dump）；
- 屏幕上只有 3 行正是因为 banner 前刚刚清过屏 —— 前面 b70/b71/b72 的满屏 driver 行是被 banner 清掉的，不是"进度丢失"；
- `0xA700DFAC` 落在 **"FD Reserved"（0xA7000000, 1 MiB）** 里，是**跳到了没有代码的地方执行**（野指针），
  和 USB、和厂商驱动都没有关系。

已核对不是镜像损坏：boot.img 解出来的 FD 与本地构建逐字节相同；FV 头校验（含 checksum 字段求和 = 0）
通过；BootShim 里的 `FD_BASE` 字面量确实是 `0xA7100000`（= 设备配置 `[uefi_fd] base`），所以 FD 不在
0xA7000000。

### 取舍：不再猜，改成一次只动一个变量

`UkeProgressDxe` 里两个有风险的行为改成 PCD 开关（默认全关）：

| PCD | 作用 | b74 |
|---|---|---|
| `PcdUkeHeartbeatStallHook` | 包裹 `gBS->Stall`（计数+喂狗） | 0 |
| `PcdUkeHeartbeatMarker` | 往 `0xA7000000` 写计数（持久） | 0 |

另外包裹层加了**重入保护**（`mInWrapper`）：控制台/调试路径也可能调用 `Stall`，在"打印→Stall→打印"
里递归是会爆栈的，这类崩溃同样表现为野指针跳转。

保留的只有只读部分：入口三行 DEBUG（`UKE-BEAT entry: marker=… stallhook=…` → `pet + mark done`）、
以及 `UKE-BEAT timer probe: N callbacks in 2 s`（只读+一次 `Stall(2 s)` 测量）。

```
work/flash/boot-round-b74-safe.img   1536000 B  sha256 bff2f3091925aa8b2d3e4dde28ed16612c249b6f027a1a6cccb1ba7d11595601
  FD sha256 e7e2f8ff89040229…（模块里 "UKE-BEAT entry"×2、"stall hook disabled"×1、无 UefiLibPrint）
```

判读：b74 若又停在同一个 Sec 异常，则**心跳不是原因**，剩下的唯一差异是"IPC 三件套被移出"（下一轮恢复）；
若 b74 正常往下走，则原因是心跳的两个开关之一，用 b75 逐个打开。

---

## 54. b74 正常往下走 → 黑屏；于是给固件加了一条"跨复位进度链"

用户实测 `boot-round-b74-safe.img`：**不再出 Sec 异常，一路往下走，没有卡住，然后黑屏。**
这是这套固件第一次把厂商 fleet 走完还没有当场死掉。黑屏有两种截然不同的解释：

1. 厂商 `ClockDxe`/`DALSYS` 按 BSP 数据重配时钟树，**把 DPU/panel 的时钟或电源关掉** → 屏幕黑、CPU 还活着、
   启动继续（可能一路到 XNU）；
2. 真的死在某个驱动/BDS/payload，只是异常向量已被接管、又没打到屏幕上。

两者在照片上完全一样，所以这次不再猜：`Include/UkePersist.h` 在 **`0xA7000000`（MemoryMapLib 的
"FD Reserved"，1 MiB，本固件所有、FD 从 0xA7100000 起、不可被 DXE 分配）** 里放了一个跨复位的小型
阶段日志：

```
struct UKE_PERSIST_BLOCK { magic "UEK1"; version; boot; count; last_stage; last_detail;
                           records[32] = {stage, detail, tick} }
```

写入点（都是我们自己的驱动，没有任何钩子）：

```
1 UkeProgressDxe 入口              (APRIORI 里，厂商 fleet 之前)
2 UkeProgressDxe 入口完成
3 UkeUsbLogDxe  入口
4 UkeUsbLogDxe  ReadyToBoot
5 UkeUsbLogDxe  USB 探针之前
6 UkeUsbLogDxe  结束
```

读回点在 **`UkeProgressDxe` 入口**，也就是整个启动里**最早、屏幕一定还活着**的时刻：

```
UKE-PREV: boot N reached stage 4 detail 8 after K records
UKE-PREV: 0 stage 1 detail 00000000 tick 0
UKE-PREV: 1 stage 2 detail 00000000 tick 0
UKE-PREV: 2 stage 3 detail 00000000 tick 0
...
```

于是**只要刷两次同一个镜像**就能知道上一次死在哪：第一次黑屏，第二次开机的头几行就把上次的阶段打出来。
（`stage >= 4` ⇒ 固件已经走到 ReadyToBoot/payload，黑屏是内核侧；只有 1–2 ⇒ 死在厂商 fleet 里。）

本轮仍然保留上一轮的取舍：`PcdUkeHeartbeatStallHook=0`、`PcdUkeHeartbeatMarker=0`，
即包裹 `gBS->Stall` 和写 0xA7000000 计数这两个风险动作继续保持关闭；持久日志是纯数据写入，
不带任何钩子。

```
work/flash/boot-round-b75-prevlog.img   1536000 B
sha256 0356dd77fbde307bb7907e601ad73ad36eccd94f9c24ec2c93c870d04641cbf6
  FD sha256 d84e64b3dd4757b2…（55 images verified）
```

---

## 55. 两个真答案：诊断行被 DEBUG 级别过滤掉了；启动停在 `ClockDxe`

### (1) 为什么我的打印一行都没出现 —— 不是崩溃，是级别过滤

`SiliciumPkg.dsc.inc:71` 把 `PcdDebugPrintErrorLevel` 设成 **0x8007EE0F**：

```
DEBUG_INIT  0x00000001  on      DEBUG_INFO      0x00000040  OFF
DEBUG_WARN  0x00000002  on      DEBUG_DISPATCH  0x00000080  OFF
DEBUG_LOAD  0x00000004  on      DEBUG_VARIABLE  0x00000100  OFF
DEBUG_FS    0x00000008  on      DEBUG_ERROR     0x80000000  on
```

dispatcher 打印的 `Loading driver at …` 是 `DEBUG_LOAD|DEBUG_INFO` ⇒ 因为 LOAD 位在，**能打**；
而 `UkeProgressDxe` 里我写的是纯 `DEBUG_INFO` ⇒ 编译进去、运行时被**过滤掉**。
所以 b71–b75 里所有 `UKE-BEAT`/`UKE-PREV` 静默，**原因在这里**，既不是中断没投递，也不是崩溃。
（这也解释了 b72 的 assert：那是 `AsciiPrint`，另一条路径。）

已把这 9 处改成 `DEBUG_WARN`。

### (2) 真正停在哪：`ClockDxe`

b75 的那张照片把整个 APRIORI 走了下来，最后一行是：

```
Loading driver at … ClockDxe.efi     ← 然后黑屏
```

前面依次过了 Smem / DALSys / HWIO / ChipInfo / PlatformInfo / ProjectInfo / HALIOMMU / ULog /
CmdDb / PwrUtils / Npa / Rpmh / Vcs（IPC 三件套已移除，`Smp2pDxe`/`GlinkDxe` 那一段不再出现）。

`ClockDxe` 是按 BSP 数据重配整棵 GCC 时钟树的驱动：它很可能把 **DPU/panel 的时钟或电源关掉**
（屏幕黑但 CPU 继续），也可能自己挂住。这两种情况下一轮就能分辨：

- 下一次开机的 `UKE-PREV` 若报 `stage 4/5/6` ⇒ 固件穿过了 ClockDxe 一路走到 ReadyToBoot/payload，
  黑屏只是**显示被关**，内核侧继续；
- 若只报 `stage 1/2` ⇒ `ClockDxe` 把启动卡死在那里。

```
work/flash/boot-round-b76-visible.img   1536000 B
sha256 8fe14f5727382f15536deb6c68aa192d95cdd1b3550da01d6241a3f45cd16f46
  FD sha256 873aa3958cd69158…（9 处 DEBUG_WARN，UKE-PREV/UKE-BEAT 现在能打出来）
```

---

## 56. b76 复现了 `0xA700DFAC` 异常；于是把 ClockDxe 拿掉，持久记录也搬走

用户实测 `boot-round-b76-visible.img`：**又出现 `Synchronous Exception at 0xA700DFAC`**（和 b73 同一地址）。
但 b74、b75 同一位置是好的，而 b73/b76 之前唯一共同的差别是"我的驱动真的往外打印了" ——
可这行异常来自 **Sec 的早期异常向量**，那时连 `ArmCpuDxe`（第 6 个驱动）都没 dispatch，
`UkeProgressDxe`（第 10 个）**根本还没跑**。

⇒ 结论：这个异常**不是我的代码引起的**，是启动最早期的一段**间歇性**故障（b74/b75 没触发）。
地址 `0xA700DFAC` 落在 MemoryMapLib 的 **"FD Reserved"（0xA7000000, 1 MiB）** 里，即"跳进了没有代码的页"。
注意这片页在所有 Silicium 平台里都是 FD 下方的预留区（例如 pong/aston 的 FD 在 0xA7400000，预留 4 MiB），
而 uke 的 FD 挪到了 0xA7100000。

两处保守措施：

1. **持久阶段日志搬家**：`UKE_PERSIST_BASE` 从 `0xA7000000`（FD Reserved）改到
   **`0xA7605000`（MemoryMapLib 的 "Log Buffer"，32 KiB，RtData，本固件所有，带 XN）** ——
   不再和那片可疑页共用。
2. **`ClockDxe` 移出镜像和 APRIORI**：b75 的面板显示启动死在它那一行（之后黑屏且不复位）。
   它按 BSP 数据重配整棵 GCC 时钟树；而 USB 侧并不需要它 ——
   `QcomXnuUsbDebug` 只检查继承下来的 GDSC/BCR/CBCR 状态，而 `UkeUsbLogDxe` 会把这份状态在
   ReadyToBoot 时读出来打印。所以这轮正好测"没有 ClockDxe 能不能第一次走到 ReadyToBoot"。

顺带确认了移除 ClockDxe 不会让别的驱动卡住 DEPEX：b75 的顺序里 `CmdDb → PwrUtils → Npa → Rpmh → Vcs`
都排在 ClockDxe **之前**并且已经被 dispatch，说明它们等的 `1c34f691` 由 `CmdDbDxe` 提供。

镜像里现在是 28 个厂商模块（含 `PmicDxe` + 5 个 USB 模块），`ClockDxe` 已不在。

```
work/flash/boot-round-b77-noclock.img   1499136 B
sha256 d72c2a55c46df3ebfd46894a04e83d7e08daaa68d3776800e1a7fbcb0ccba531
  FD sha256 058a8ddbe44f06f0…
```

内核侧：run `42269780292` 已经编到 **第 17 步 "Link the kernel (prelink included)"** ——
也就是 Pass 1 / Pass 2 / 各个 archive（包括 `build-uke-usb.py`）都过了，`uke_usb.c` 的编译修复生效。

---

## 57. 诊断通道打通了；厂商链路确认走不通（SPMI 外部中止已解出）

### 面板现在真的能说话了

b77 第一次把我们的行打了出来（`DEBUG_WARN` 生效）：

```
UKE-PREV: no stage log (first boot into this image)
UKE-BEAT entry: marker=0 stallhook=0
UKE-BEAT entry: pet + mark done
UKE-BEAT timer probe: 0 callbacks in 2 s (Success)
UKE-BEAT stall hook disabled by PCD
```

**`0 callbacks in 2 s`** —— 定时器事件（中断）在这套 UEFI 里**确实不投递**，这次是量出来的，
不是推断的。所以以后任何"靠定时器"的诊断都不能用，要看时间就得查计数器。

### 崩溃被我解出来了：SPMI 访问到没上电的控制器

面板上的寄存器（照片 1/2）：

```
ELR 0x0000C256A700 (= SPMI.dll + 0x3700)   ESR 0x96000010   FAR 0x00000000C7E5008
Data abort: Synchronous external abort
```

解 ESR：`EC = 0x25`（同级取指无关的数据中止）、`ISS = 0x10` → **DFSC 0x10 = Synchronous
External abort（不是页表遍历错）** —— 意思是**总线拒绝了这次访问**，不是没映射。
`FAR = 0xC7E5008` 落在 live DT 的 SPMI 通道窗口里：

```
spmi0_bus: qcom,spmi@c42d000
  reg-names = "cnfg" 0xc42d000 / "core" 0xc400000 / "chnls" 0xc500000+0x400000 / ...
                             0xC500000..0xC900000  ← 0xC7E5008 在其中
```

调用栈（照片 3 带符号解析）：`DxeCore → PmicDxe.dll → SPMI.dll`，崩在 `SPMI.dll+0x3700`。
紧邻的上一行是 `ERROR: Fail to init client handle to QDSS clock node` ——
**时钟数据库里查不到 QDSS 节点**，因为 `ClockDxe` 被拿掉了。

⇒ 结论：**厂商链路在这块板子上是个环**——

| 配置 | 结果 |
|---|---|
| 带 `ClockDxe` | 启动停在 ClockDxe 那一行，黑屏且**不复位**（它重配 GCC 时把显示/或 RPMh 投票挂住）|
| 不带 `ClockDxe` | `PmicDxe` → `SPMI` 访问未上电的 SPMI 控制器 → **外部中止** |
| 带 IPC 三件套 | `GlinkDxe` / `Smp2pDxe` 在各自 entry 里死（ICC 传输要的对端在这次启动里不可用）|
| 全不带 | 稳定走到 payload（b66 时代的固件就是这样）|

所以这一轮改回"没有厂商驱动"，但**把 USB 状态读取提前到厂商 fleet 之前**（`UkeProgressDxe`），
于是不管后面发生什么，`UKE-GCC:` 那两行一定会打出来，回答"bootloader 留给我们的 USB 电源/时钟
到底是什么状态"——这正是内核侧 `uke_usb.c` / `QcomXnuUsbDebug` 需要的输入。

```
work/flash/boot-round-b78-novendor.img   1122304 B
sha256 6ed3d44f3d7eb31fb3126e5b944cbfd073de2e703c255478b275e39b6ce13f6a
  FD sha256 355f1f1e12e30ace…（0 个厂商模块；UkeProgressDxe / UkeUsbLogDxe / UkeBootMemoryDxe 都在）
```

要看的只有两条：

```
UKE-GCC: bcr=… cfg_gdscr=… cbcr … … … … …
UKE-GCC: gdsc_powered=? reset_deasserted=? usb_clocks_running=?/3
```

`usb_clocks_running=3/3` + `gdsc_powered=1` ⇒ 时钟/电源已经是好的，内核只需拿下 DWC3；
否则就说明我们得自己写那五个 CBCR 和 GDSC。

---

## 58. 老实回答："现在能用 USB 看日志了吗？" —— 还不行，但可以**用 root adb 看**

USB 那条路目前是堵死的，原因就是 §57 那张表：USB 日志盘靠的是厂商 `UsbMsdDxe` +
`UsbfnDwc3Dxe`，而这两个要的时钟/电源/PMIC 链路（`ClockDxe`/`SPMI`/`PmicDxe`/IPC）在这块板子上
要么把启动卡死、要么触发外部中止。**所以 b78/b79 这两个镜像根本不会把日志盘挂到 USB 上**
（`UkeUsbLogDxe` 在 `Uke-LOG: usb-msd not found` 之后就返回了）。

但"不用拍照就能拿到日志"这件事现在有了一条**真实可用的路**：**Android 的 ramoops**。

Android 自己的设备树写着：

```
ramoops_mem: ramoops_region {
    reg = <0x00 0xa7000000 0x00 0x400000>;   /* 4 MiB */
    console-size = <0x200000>;               /* 前 2 MiB 是 console 区 */
    pmsg-size = <0x200000>;
};
```

而我们的 FD 在 `0xA7100000`（5 MiB，到 0xA7600000），所以这块 console 区的**前 1 MiB
（0xA7000000-0xA7100000）是空的、且复位不清**。pstore 在下次启动时会读取"上一次启动的 console 区"。

于是 `UkeUsbLogDxe` 在 ReadyToBoot 末尾多了一步 `UkeRamoopsPublish()`：把 `mLog` 里的整份日志
（**含上一次 XNU 的 pram 回显**）按 Linux `persistent_ram_buffer` 的格式写进去：

```
struct { u32 sig = 0x43474244 ("DBRG"); u32 start = 0; u32 size; u8 data[size]; }
```

写完 `WriteBackDataCacheRange()` 落盘，然后才交给 payload。**这套格式和内核侧
`UkeLastLog.c` 写 ramoops 用的是同一个**（`UKE_RAMOOPS_MAGIC 0x43474244`）。

### 用法（这就是"不用拍照取日志"）

```
fastboot boot work/flash/boot-round-b79-pstore.img
# 让它跑完（黑屏/卡住都没关系），然后电源键长按重启进 Android

adb shell su -c 'ls -l /sys/fs/pstore/'
adb shell su -c 'cat /sys/fs/pstore/dmesg-ramoops-0'      # 名字可能是 console-ramoops-0
```

里面应该有：固件这一轮的全部 `Log()` 行（含 `UKE-GCC:`、`UKE-QUD:`、`--- pram … ---`）
**加上** 上一次 XNU 的内核日志正文。这样内核侧的 `UKE USB:` 行也能通过 adb 取回，
不必再靠拍照。

```
work/flash/boot-round-b79-pstore.img   1122304 B
sha256 ca92dede043fd900a934fe57d52cc21caead3e3f78ab185a82249831056c1d7e
  FD sha256 b2a4980cd293a414…（0 个厂商模块；新增 UkeRamoopsPublish + 两个 PCD）
```

风险与备选：若 pstore 里看不到文件名（不同 Android 内核注册的名字不同），退一步
`adb shell su -c 'cat /proc/pstore* /sys/fs/pstore/* 2>/dev/null | head -50'`，
或者直接把这块区域当普通内存读（本机 root 可用 `dd if=/dev/mem` 会被 STRICT_DEVMEM 拦，
所以优先用 pstore）。

USB 那一侧并没有被放弃：等内核产物出来（run `42269780292` 已到第 17 步链接），
用 `uke_usb.c` 在 XNU 里自己冷启 DWC3 是当前唯一不被厂商链路卡住的路径，
而它的日志现在可以通过上面这条 adb/pstore 通道取回。

---

## 59. 内核终于编出来了（带 `uke_usb.c` 修复），新 payload 已配对；pstore 这条路暂时是空的

### 内核：run `42269780292` 成功

```
kernel.macho  136527872 B  sha256 e868737c62af4a8cf8c71ea2f70f76c3cde439cd671dfd7bdd6c9b5d1967aab2
prelink: stubbed 0 / stub_sites 0        （362 个未匹配调用依然是 0，没有回归）
strings "UKE USB:" 125 处，含 "UKE USB: probe thread running"
```

也就是说 `uke_usb.c` 的 `delay()` / `kernel_thread_start` 修复生效，**内核里第一次真正带着
CDC-ACM 探测线程**（b66 那版是没有它的）。

另外为了让 payload 过得去，把 `iokit_print_verbose_match_logs=1` 从 kernel_args 里删了：
payload 步骤会追加 `native-ufs-root=0`，990 + 18 字节会撞上 1000 字节上限（`io=0x10` 本来
就会打印每个 nub 的匹配候选）。

```
work/flash/boot-round-b80-usbkernel.img        1122304 B  sha256 ca92dede…（= b79 固件，无厂商驱动）
work/flash/vendor_boot_round-b80-usbkernel.img  85782528 B  sha256 63d0cf66…（内核 e868737c + ADT）
```

### pstore：`/sys/fs/pstore/` 是空的

用户实测（b79 跑完 → 刷回 stock `vendor_boot_a` → 进 Android）：

```
$ adb shell su -c 'ls -l /sys/fs/pstore/'
total 0
```

目录存在但没有任何记录，说明 pstore core 在、但要么 ramoops 后端没把这块区域注册成 backend，
要么我们的记录没通过 `persistent_ram` 的校验。要分辨只需要几条命令（见下），
比继续猜快得多。

### 59b. dmesg 把 pstore 为空的原因说清楚了，而且顺带给了一条更好的路

用户贴的 Android dmesg：

```
[0.000000] OF: reserved mem: 0x00000000a7000000..0x00000000a73fffff (4096 KiB) map non-reusable ramoops_region
[0.077553] printk: console [ramoops-1] enabled
[0.077683] pstore: Registered ramoops as persistent store backend
[0.077683] ramoops: using 0x400000@0xa7000000, ecc: 0
[0.177379] pstore: Invalid compression size for deflate: 0
[2.661000] mtdoops: pares mtd_dt, mem_address =0xa7000000, mem_size =0x400000
[2.661006] mtdoops: pares mtd_dt, pmsg_size =0x200000,console-size =0x200000
[2.668164] mtdoops: Attached to MTD device 0
[2.781102] mtdoops: ready 3, 140
```

三条结论：

1. **区域是对的**：`ramoops: using 0x400000@0xa7000000`，正好是我们写的那块。
2. **pstore 后端确实注册了**，但这个内核开了 `CONFIG_PSTORE_COMPRESS` +
   `CONFIG_PSTORE_COMPRESS_DEFAULT="deflate"`，记录要带压缩头和长度校验
   （`pstore: Invalid compression size for deflate: 0`）；我们写的是裸文本，
   于是被**静默丢掉** —— 不是地址错，是格式不合它的压缩约定。
3. **更好的一条路**：Xiaomi 自己的 `mtdoops` 把同一块区域接成了 **MTD device 0**
   （`mtdoops: Attached to MTD device 0`）。也就是说 Android 侧有 `/dev/mtd0`，
   **可以绕过 pstore 的格式校验直接 dd 原始字节**：

```sh
adb shell su -c 'cat /proc/mtd'
adb shell su -c 'dd if=/dev/mtd0 bs=1M count=1 2>/dev/null | strings | head -100'
```

我们的记录在 `0xa7000000`（区域起点、console 区前 1 MiB），所以 `count=1` 就够。

顺带确认 `printk: console [ramoops-1] enabled` **不会**覆盖我们的记录：pstore 的 console
只在 oops/panic 时才写（否则 `pstore.console=force`），正常开机不动这块。

### 59c. pstore 在这台设备上本来就是死的；`mtd0` 是 oops 分区，不是那块 RAM

两条都查实了：

1. **`/dev/mtd0` 不是 ramoops**：`/proc/mtd` 写着
   `mtd0: 01000000 00200000 "block2mtd: /dev/block/by-name/oops"` ——
   它是 **oops 分区**（16 MiB，UFS 上）的 block2mtd 视图，读出来的是小米自己写的
   `oops report start--- / ## REASON: Restart / LAST KMSG`，跟我们的 RAM 记录无关。
2. **pstore 的 console 区在这套内核里根本不会出现在 `/sys/fs/pstore`**：
   Linux `fs/pstore/ram.c` 的 `ramoops_get_next_prz()` 只对
   `PSTORE_TYPE_DMESG` 调用 `persistent_ram_save_old()`：

   ```c
   /* Update old/shadowed buffer. */
   if (prz->type == PSTORE_TYPE_DMESG)
           persistent_ram_save_old(prz);
   if (!persistent_ram_old_size(prz))
           return NULL;
   ```

   而这块设备的 DT 只定义了 `console-size` 和 `pmsg-size`，**没有任何 dump/record 区**，
   所以 `max_dump_cnt = 0`，console 区的 `old_size` 永远是 0 —— 不管我们写什么格式都不会出现。
   （`pstore: Invalid compression size for deflate: 0` 只是压缩工作区在 init 时算不出来，
   和我们的记录无关。）

⇒ **pstore/ramoops 这条路在本机是死路**，别再花时间。要"不拍照"只剩两条：
USB（内核侧 `uke_usb.c`，新内核已就绪）与 **面板分页**（本轮新增）。

### 面板分页：把整份日志用拍照也能取全

`UkeUsbLogDxe` 现在在原有的"首 12 行 + 末 24 行"之后进入分页：

```
--- boot log page N (vol-up next, vol-down continue) ---
<22 行>
```

- **音量加** → 下一页（每页最多等 `PcdUkePageHoldSeconds=20` 秒，等待期间照常喂狗）；
- **音量减** → 立即继续启动；
- 不按键 → 20 秒后自动继续启动。

于是"上一次 XNU 的整份内核日志（pram 里那 192 KiB 文本）"可以用多张照片取全，
不需要 USB、也不需要 Android。

```
work/flash/boot-round-b81-paged.img       1122304 B  sha256 084cb2adc1ca0d9c0786c5e73cbfe4bca6b00b4981610bab7d83090ffa04a1a3
work/flash/vendor_boot_round-b80-usbkernel.img  85782528 B  sha256 63d0cf66…（内核 e868737c，带 USB 探测线程）
```

---

## 60. b81 + 新内核真机：固件正常，内核起到达尔文，然后仍是那个已知的 SError panic

实拍三张（同一轮）：

```
Unhandled implementation specific error            ← XNU panic 屏（绿/蓝条 = 视频控制台）
```

另外两张是控制台尾部：我们的 `gic-probe:` 全部行 + `Exiting boot services. A green bar across the
top marks the loader handoff before the kernel branch.` + `SecurityLock: […] LockType: SOFTWARE_LOCK …`。

也就是说这一轮：

1. **固件侧没有卡住、没有异常** —— 无厂商驱动的 b81（分页 + UKE-GCC + 阶段日志）走完了整个 DXE；
2. **内核起来了、EARLY_BOOT 走完、退出 boot services、进了 kext 阶段**；
3. 然后命中 `panic_plain("Unhandled implementation specific error")` —— 就是 §46/§47 记过的那条：
   `arm64_platform_error()` → `PE_handle_platform_error(far)` 返回 false（因为没有 `IOPlatformIO`
   服务发布）→ `sleh.c` 的 SError 兜底 panic。**这轮没有新的坏消息，是同一个老关口。**

关键点：`UKE USB:` 那几行在**上一屏**（`uke_usb` 的线程在 EARLY_BOOT + 8 s 打印），
被后面的 `gic-probe` 20 秒停留和 boot-services 退出刷掉了 —— 所以照片里看不到，
不等于驱动没跑。**正好用这次新加的分页来取**：再启动一次同一对镜像，
ReadyToBoot 的 `--- boot log page N ---` 里就是**上一次启动的内核日志**（从 pram 读回来的），
翻页找 `UKE USB:` 即可。

### 60b. 主机侧 dmesg：我们的 payload 期间**没有任何 USB 设备枚举**

```
usb 3-1.2: New USB device found, idVendor=18d1, idProduct=d00d … Product: Android
usb 3-1.2: USB disconnect, device number 34
usb 3-1.2: new full-speed USB device number 35 … device descriptor read/64, error -32
```

`18d1:d00d` 是 **Android 自己的 adb gadget**（Google/Android），出现在 Android 启动/重启时。
在跑我们 payload 的那段时间里**没有任何设备出现**，也就是说 `uke_usb.c` 的 DWC3 到这一轮
**还没有把控制器拉到 device 模式**（没拉 D+ / 没使能 RUN_STOP）。

### 关于"用 Android 读日志"：现在不行，两个硬阻塞

| 通道 | 状态 | 原因 |
|---|---|---|
| 写 flash（`oops`/`logfs`）让 Android `dd` 出来 | ✗ | 固件没有 UFS 驱动；厂商 `UFSDxe` 卡在同一条 Clock/PMIC 链上（§57） |
| 写 RAM 区域让 Android 读 | ✗ | 见下面两行 |
| pstore / ramoops | ✗ | `ramoops_get_next_prz()` 只对 `PSTORE_TYPE_DMESG` 调 `persistent_ram_save_old()`；本机 DT 的 4 MiB 全给了 console+pmsg，`dump_mem_sz = 0`，所以 console 区的 `old_size` 恒为 0（源码已核，`fs/pstore/ram.c:130-137`、`:768-782`）|
| `/dev/mem` | 大概 ✗ | arm64 默认 `CONFIG_STRICT_DEVMEM=y` |
| `/proc/kcore` | 待测 | root 可读；pram 在 `0x9FFE00000` 落在 Android 的 `/memory` 高段里（线性映射），若能算出 ELF 段偏移就可以精确 `dd` 一小段 |
| 小米的 `debug_kinfo`（4 KiB，`no-map`）| 待测 | 容量只有 4 KiB，够写日志尾部；要看它有没有被 Android 暴露成 proc/sysfs 节点 |

### 60c. Android 侧读 RAM 这条路彻底关闭（三条都实测）

```
$ adb shell su -c 'ls -l /proc/kcore /dev/mem /proc/kallsyms'
ls: /proc/kcore: No such file or directory
ls: /dev/mem: No such file or directory
-r--r--r-- ... /proc/kallsyms

$ zcat /proc/config.gz | grep -E "STRICT_DEVMEM|DEVKMEM"
# CONFIG_STRICT_DEVMEM is not set        ← 但 /dev/mem 压根不存在（CONFIG_DEVMEM 没开）
```

- `/proc/kcore` 不存在；
- `/dev/mem` 不存在（`CONFIG_STRICT_DEVMEM` 关了也没用，因为 `CONFIG_DEVMEM` 本身没编）；
- 小米的 `debug_kinfo` 没有被暴露成 proc/sysfs 节点（`ls /proc | grep -i boot|kinfo` 只匹配到 `bootconfig`）。

加上 §60b 里那条 pstore 的源码结论，**"固件写 → Android 读"这条路在本机不可行**，
除非固件自己有 UFS 驱动（那又回到 Clock/PMIC 那条死链）。

⇒ 结论：**不拍照的日志只有 USB 一条路**（XNU 侧 `uke_usb.c`，或将来固件自写 DWC3 device 栈）。
在那之前，面板分页是唯一能把整份日志带出来的手段。

---

## 61. 分页没出现的原因：我们所有面板输出走的是一条**看不见的路**

用户刷 b81 后按说明翻页，但屏幕上根本没有 `--- boot log page 1 ---`，只有内核的
`gic-probe:` 那一屏。

对比两条输出通道就明白了：

| 通道 | 谁在用 | 屏幕上可见？ |
|---|---|---|
| `DebugLib` → `FrameBufferSerialPortLib`（直接写 framebuffer） | Sec 的 banner、DxeCore 的 `Loading driver at …` | **可见** |
| `ConOut` → `AsciiPrint` → GraphicsConsole/GOP | `UkeUsbLogDxe` 的 `Log()`、`DumpPreviousBootLog` 回显、`DumpPagedLog` | **一直不可见** |

也就是说：**从 b70 到 b81，`UkeUsbLogDxe` 的每一行面板输出都没真正显示过**
（包括那句 `--- previous boot log: first N lines ---`）—— 这也是为什么我们从来没在照片里
看到过 pram 回显。这不是新 bug，是一开始就走错了通道。

修法很简单：`UkeUsbLogDxe` 的全部面板打印（`Log()` 的镜像输出、pram 头/尾回显、分页器）
统一改成 `DEBUG ((DEBUG_WARN, …))`，与 dispatcher 同一条路；`mLog` 缓冲照旧（USB 盘和 ramoops 还要用）。

```
work/flash/boot-round-b82-visiblelog.img   1122304 B
sha256 678493aeab142a43fd5517f07f1a8225d19d401b9b10a9de08623e871d8de9a0
  FD sha256 d8cd439e22700311…（模块里已无任何 AsciiPrint 调用）
```

下一轮同一个镜像启动时，`--- boot log page 1 (vol-up next, vol-down continue) ---` 应该终于出现在屏幕上，
里面就是上一次启动的内核日志（pram 里那 192 KiB），可以按音量加翻页找 `UKE USB:`。

---

## 62. b82 真机：固件终于会说话了，而且一屏回答了三个关键问题

### (1) USB 的继承状态是**好的**

```
UKE-GCC: bcr=00000000 cfg_gdscr=04070000 cbcr 00000001 00000001 00000001 00000001 00000001
UKE-GCC: gdsc_powered=1 reset_deasserted=1 usb_clocks_running=3/3
UKE-GCC: dwc3 gsnpsid=…31… gctl=00100201 phycfg=00102400 dcfg=0048082c dctl=… dsts=…
UKE-QUD: halt 0 ms dctl=… dsts=… halted=1
```

- `BCR = 0` ⇒ USB 复位已释放；
- `CFG_GDSCR = 0x04070000` ⇒ bit16（POWER_UP_COMPLETE）已置 ⇒ **GDSC 已上电**；
- 五个 CBCR 全是 `0x00000001`（CLK_OFF 位为 0）⇒ **五个 USB 时钟全部在跑**；
- DWC3 窗口可读（GSNPSID/GCTL/PHYCFG/DCFG/DCTL/DSTS 都拿到了值）⇒ 控制器是活的。

⇒ **bootloader 已经把 USB 的电源与时钟全部留好了。** `uke_usb.c` 完全不需要自己开时钟/GDSC，
只要做 PHY + DWC3 初始化即可（`uke_usb_power=11` 那条"只读取证"路径正好）。

### (2) pram 一直是空的 —— 因为我们**读错了地址**

面板上每一轮都重复这几行：

```
--- pram 0x9FFE00000 (2048 KiB) from the previous boot ---
pram head: 00 00 00 00 00 00 00 00 ...
--- pram end: 0 KiB of text kept in 0 lines ---
pram first text: <none: DRAM did not survive, or the kernel never wrote>
```

而 `scripts/make-uke-adt.py` 里写死的是：

```python
PANIC_LOG_BASE = 0xFF800000     # llcc_lpi_region@ff800000（live DT 里就有，UEFI 内存图也按这个名字保留）
PANIC_LOG_SIZE = 0x00380000
...
node('pram', [('reg', struct.pack('<QQ', PANIC_LOG_BASE, PANIC_LOG_SIZE))]),
('embedded-panic-log-size', struct.pack('<I', PANIC_LOG_SIZE)),
```

**XNU 写的是 `0xFF800000`（0x380000 字节），固件读的是 `0x9FFE00000`（0x200000）** ——
`board.toml` 里的 `panic_region` 根本没有被生成器使用。所以"内核从没写过"是假象，我们一直在看空地。

修法：把固件的 `PcdUkePramBase`/`PcdUkePramSize` 改成 `0xFF800000` / `0x00380000`，
与 ADT 常量、以及内存图里的 `llcc_lpi_region@ff800000` 三者一致（地址本来就在内存图里，
`GetMemorySpaceDescriptor` 也不会失败）。

### (3) 固件输出通道修好了

`--- previous boot log: first N lines ---`、`UKE-QUD:`、`UKE-GCC:`、ramoops 发布、
`--- boot log page 1 (vol-up next, vol-down continue) ---` 全部真的出现在屏幕上了
（`ramoops: publishing 605 of 605 bytes at 0xA7000000` 也确认执行）。

```
work/flash/boot-round-b83-pramfix.img   1122304 B
sha256 b7a673d1017b5586ddfb9daf7f8f29a7a8cab3fc8df0a036516481cec0e1c5c6
  FD sha256 a56c702ee2eb260a…（pram 指向 0xFF800000/0x380000）
```

下一轮的流程：刷 b83 → 内核跑（这次会真的往 pram 写）→ 再启动一次 b83 → 分页里就能看到
**上一次内核日志**，包括 `UKE USB:` 那几行。

---

## 63. b83 真机：pram 地址对了，但**内核根本没往那儿写**；两个内核线程都没跑出来

照片里 pram 现在读的是对的地方（`--- pram 0xFF800000 (3584 KiB) ---`），头部有内容：

```
pram head: 10 30 A0 C0 86 40 00 00  0C 00 81 00 10 00 00 A2
--- pram end: 0 KiB of text kept in 0 lines ---
pram first text: <none: DRAM did not survive, or the kernel never wrote>
```

`0xC0A03010` **不是** XNU 的 `EMBEDDED_PANIC_MAGIC`（`0x46554E4B` "FUNK"，见
`osfmk/kern/debug.h:345`）。所以那 16 字节只是**残留 DRAM**，不是我们写的记录 ——
内核从未写过这块 pram。

已核对的三件事都"应该"是通的：

| 检查 | 结果 |
|---|---|
| ADT 里 `pram` 节点位置 | `make-uke-adt.py` 把它放在**根节点**（`node('pram', …)` 与 `cpus`/`defaults` 同级），正是 `pexpert/arm/pe_init.c` 里 `SecureDTLookupEntry(0, "pram", &entry)` 找的位置 |
| `embedded-panic-log-size` | 0x380000，`pram.reg[1]` 也是 0x380000，`<=` 通过 |
| 编译开关 | `XNU_DT_PANIC_REGION=1`（defconfig `CONFIG_XNU_DT_PANIC_REGION=y` → `build-xnu-4k-baseline.sh:639` 传给 `pe_init.o`）|

⇒ 那么 `gPanicBase` 应该是非 0，`UkeLastLog` 的写线程应该每 500 ms 写一次 FUNK 记录。
**但它没写。** 而 `uke_usb` 的探针线程同样一行输出都没有。

两个驱动有个共同点：**它们都是 `STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, …)` 里用
`kernel_thread_start*` 起的线程**。所以这一轮怀疑的对象不是寄存器、不是地址，而是
**"这个内核里从 EARLY_BOOT STARTUP 起的线程没有在跑"**。

### 因此改的是实验方式，不是猜寄存器

`uke_usb_sync=1`（新增启动参数，已烘进 args）：

- 在 STARTUP 上下文里**同步**跑探针（不再起线程），跑完按 `uke_usb_panic=1` panic；
- panic 屏在这块板子上是**可见的**（SError 的 panic 屏我们就拍到过），所以
  `UKE USB PROBE: step=… gcc=… dwc=… dsts=… clk=… smmu=… pll=…` 一定能被拍下来；
- `kernel_thread_start` 失败也会打印返回值了；
- 为腾命令行空间，删掉 `fileset-wait-cap-ms=5000`（它的作用已被 `uke-aes-stub.patch` 的无条件打印取代）。

提交 `486f48e`，CI 已触发（约 45 分钟）。产物出来后配 b83 固件即可。

---

## 64. 同步探针的内核已出（b84）：这次 panic 会把 USB 面包屑直接打到屏幕上

```
run 42356668405 (486f48e) success
kernel.macho sha256 de75832d2b50f20b68db45971379f2fba9c7273bb764a52ddc06f965437014bb
prelink stubbed 0 / stub_sites 0
strings: "uke_usb_sync" / "UKE USB: synchronous probe (uke_usb_sync=1)" /
         "UKE USB: kernel_thread_start failed (%d)" 全部在；"UKE USB" 共 128 处
args: … uke_usb=1 uke_usb_power=11 uke_usb_sync=1 uke_usb_panic=1 …（fileset-wait-cap-ms 已让位）
```

配好的镜像对（固件仍是 b83 那版，即 pram 指向 0xFF800000 的）：

```
work/flash/boot-round-b84-syncprobe.img          1122304 B  sha256 b7a673d1017b5586…（= b83 固件）
work/flash/vendor_boot_round-b84-syncprobe.img   85782528 B  sha256 fef39fbff7d3fcd4…（内核 de75832d）
```

预期：内核在 EARLY_BOOT 的 STARTUP 里**同步**跑探针，然后立刻 panic：

```
UKE USB PROBE: step=N gcc=… dwc=… dsts=… clk=… smmu=… pll=…
```

三种结果分别意味着：

| 屏幕上出现 | 含义 | 下一步 |
|---|---|---|
| `UKE USB PROBE: step=N …` | 探针跑通了，卡在第 N 步 | 直接改那一步（PF 表/PHY/DWC3 初始化）|
| `UKE USB: kernel_thread_start failed (…)` 或什么都不出现，仍是 SError panic | 同步路径也被更早的东西挡住 | 说明 STARTUP 阶段本身有问题，下一步把探针挪到更早/换上下文 |

---

## 65. b84：同步探针也没留下任何痕迹 —— 于是改成"往 pram 里写字"，让内核自己报告

b84 实拍（视频逐帧）里只有这些：

- 固件侧一切正常（`UKE-GCC: gdsc_powered=1 … usb_clocks_running=3/3`、`UKEMEM:`、DarwinBoot 交接、Apple logo + UniXNU）；
- 内核侧 `gic-probe:` 一屏 + `Exiting boot services. A green bar across the top marks the loader handoff before the kernel branch.`；
- **没有出现 `UKE USB PROBE:` 的 panic 屏**；
- pram 依然是 `0 KiB of text kept in 24 lines`、头部还是那串残留 `10 30 A0 C0 86 40 00 00 …`。

⇒ 这一轮排除掉的正是"线程没被调度"这个假设：**改成同步调用后依然什么都没有**。
于是怀疑对象再往下一层：**`STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, …)` 注册的函数到底跑没跑**。
（`uke_usb` 和 `UkeLastLog` 两个都是这样注册的，两个都没有任何可观测输出。）

### 解法：不再依赖 console / 线程 / panic，直接往 pram 写 ASCII

内核里唯一**必然可被固件读到**的东西就是 pram 那块内存 —— 固件每次启动都会打印它的头 16 字节，
并把整块扫描成可打印文本。所以内核现在把面包屑直接写进去（提交 `2fec95e`，CI run `42360110438`）：

```
uke_usb.c    : 0xFF800000 + 0x1000，64 字节 ASCII
  "UKEUSB-STARTUP-ENTERED"         ← STARTUP 一开始就写
  "UKEUSB-STEP-NNN"                ← 29 处 uke_usb_step 赋值全部改成写这行
  "UKEUSB-DISABLED"                ← uke_usb 没解析到时

UkeLastLog.c : 0xFF800000 + 0x1100，同样 64 字节
  "UKELASTLOG-STARTUP-ENTERED" / "-DISABLED" / "-THREAD-STARTED" / "-THREAD-FAILED"
```

映射方式和 `pe_init.c` 里映射 pram 一样（`ml_io_map_wcomb`），写完 `dsb sy`。
下一轮启动时，固件的 `--- pram … ---` 与 `pram first text:` 就会把**内核自己写的最后一行**打出来：

| 固件屏上出现 | 结论 |
|---|---|
| `pram first text: "UKEUSB-STARTUP-ENTERED"` | STARTUP 跑了，但 `uke_usb` 参数没解析到 → 查 boot-arg |
| `... "UKEUSB-STEP-0NN"` | STARTUP 跑了、探针也跑了，卡在第 NN 步 → 直接改那一步 |
| `... "UKELASTLOG-THREAD-FAILED"` | 线程创建失败 → pram 写者改同步 |
| 两个都没有（仍是残留字节） | **STARTUP 根本没执行** → 那问题在 STARTUP 机制/链接（下一步查 `__startup` 段） |

---

## 66. 面包屑内核出来了（b87）：`uke_usb` 的 beacon 已进内核，内核会自己往 pram 写字

```
run 42363611270 (452bbf6) success
kernel.macho sha256 145283526af5cecbd7db77b259440d13c93738899ec0c3bf138e4f368f5f78d0
prelink stubbed 0 / stub_sites 0
strings: "UKEUSB-STARTUP-ENTERED" ✓、"UKEUSB-STEP-001/002/003/…" 共 18 条 ✓
```

一个需要记录的偏差：`UKELASTLOG-*` 那几条**没有**进这一版内核（`strings` 里 0 条），
而 `UKEUSB-*` 全在。同一份 `libUkeLastLog.a` 里的 `UkeLastLog.o` 确实进了内核
（它的 `UkeLastLog: vnodes desired=` 字符串在），所以这不是"归档没链"，更像是那一个对象用了
树里 `kits/uke/UkeLastLog.c` 的旧副本。**不影响本轮结论**：判断"STARTUP 到底跑没跑"
只需要 `uke_usb` 这一条 beacon；如果下一轮 pram 里出现 `UKEUSB-STARTUP-ENTERED` 或
`UKEUSB-STEP-NNN`，机制就是通的（顺带把 `UkeLastLog` 的 beacon 补进树里再验证一次）。

```
work/flash/boot-round-b87-beacon.img          1122304 B  sha256 b7a673d1017b5586…（b83 固件：pram @0xFF800000）
work/flash/vendor_boot_round-b87-beacon.img   85782528 B  sha256 adc45b88d8c53d0a…（内核 145283526af5cecb）
```

**两段式读法**：

1. 刷 + `fastboot boot` → 内核跑到 EARLY_BOOT，`uke_usb_startup` 把
   `UKEUSB-STARTUP-ENTERED` 写进 pram 的 `0xFF800000 + 0x1000`，然后同步探针（`uke_usb_sync=1`）
   每进一步就改写成 `UKEUSB-STEP-NNN`；
2. **再启动一次同一对镜像** → 固件的 `--- pram 0xFF800000 (3584 KiB) from the previous boot ---`
   与 `pram first text:` 就会把内核写进去的最后一行打出来。

### 66b. 现在可以"读视频"了：`ffmpeg` 在本机，抽帧 + 读图

用户把 51 秒 4K/120fps 的录屏 `VID_20261107_190413.mp4` 放进项目根目录。我这边用
`ffmpeg` 按时间抽帧、裁剪、缩放，再把帧当图片读，等价于"看视频"：

```bash
ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1 VID_*.mp4
ffmpeg -i VID_*.mp4 -an -vf "fps=1,scale=1280:-1" frames/s%02d.png          # 全片导览
ffmpeg -ss 9.35 -t 0.9 -i VID_*.mp4 -an -vf "fps=10,scale=1920:-1" e%02d.png  # 精确定位关键 0.9 秒
ffmpeg -i VID_*.mp4 -an -vf "select='gt(scene,0.03)',metadata=print:file=-" -f null -  # 场景切换时刻
```

（本机没有 tesseract/gocr，所以是"人眼读帧"而不是 OCR；4K 帧足够看清状态栏级别的字。）

从这段视频里读到的关键一帧（t≈9.6 s）：

```
UKE-LOG: ready-to-boot hold=20
--- pram 0xFF800000 (3584 KiB) from the previous boot ---
pram head: 10 30 A0 C0 86 40 00 00  04 00 81 00 10 00 00 22
--- pram end: 0 KiB of text kept in 24 lines ---
pram first text: <none: DRAM did not survive, or the kernel never wrote>
--- previous boot log: first 12 lines ---
```

这段视频是**刷入 b87 后的第一次启动**，所以它读到的 pram 还是上一版（无 beacon）留下的；
而 b87 内核的 beacon 是在**这次启动**里才写进去的。⇒ 还差**第二次启动**才能读到。

### 66c. 第二段录像 + 让固件直接把 beacon 槽位读出来（b88）

第二段录像 `VID_20261107_191325.mp4`（48.7 s）同样是**一次启动**，pram 头部与上一段逐字节相同：

```
--- pram 0xFF800000 (3584 KiB) from the previous boot ---
pram head: 10 30 A0 C0 86 40 00 00  04 00 81 00 10 00 00 22
```

`pram first text:` 那行在 9.5–9.9 秒之间一闪而过，逐帧找代价太大。于是改成**让固件直接报**：
`DumpPreviousBootLog()` 现在在 `pram head:` 之后多打两行，直接把两个 beacon 槽位按可打印文本读出来：

```
pram slot 0x1000: "UKEUSB-STEP-0NN"      ← uke_usb 的面包屑
pram slot 0x1100: "UKELASTLOG-..."       ← UkeLastLog 的
```

这样"内核的 STARTUP 到底跑没跑 / 探针到第几步"就是**一行确定的字**，不再依赖整窗扫描的阈值，
也不需要在录像里找那一帧 —— 拍一张 ReadyToBoot 那屏就够。

```
work/flash/boot-round-b88-slots.img   1122304 B  sha256 d86e37f0e4db96d7f1b3aa4b4eff7a931292564699a6e5a1a1c44f04d2a56ac9
  FD sha256 fa2508eb1cd0f50d…（在 b87 固件基础上加了 LogPramSlot）
```

因为 pram 里现在存的是**刚才那次 b87 内核启动**写下的内容，所以刷 b88 **启动一次**就能读出结论，
不需要两段式。

### 66d. 固件侧再补三件"把不确定性压掉"的读法（b89）

在等内核那轮 CI 的同时，固件这边加了（本机构建，不占 CI）：

1. **多读一个槽位**：`pram slot 0x1200`（`csr_check` 的 beacon 写在这里）；
2. **原始 hex**：`pram slot 0x1000 raw:` / `0x1200 raw:` 各 16 字节 —— 万一内核写进去的不是可打印文本
   （半条记录、对齐不同、或别的路径写的），hex 是唯一能看见它的方式；
3. **整窗搜索**：`pram find "UKEUSB"` / `pram find "UKELASTLOG"` ——
   不再假设内核一定写在 `+0x1000`；命中就把**偏移 + 后面 40 字节文本**打出来（最多 4 处）。

于是下一次启动的那一屏会同时给出：

```
pram slot 0x1000: "…"            pram slot 0x1000 raw: …
pram slot 0x1100: "…"            pram slot 0x1200 raw: …
pram slot 0x1200: "…"
pram find "UKEUSB" at 0x…: "…"   pram find "UKELASTLOG": not present in 3584 KiB
```

```
work/flash/boot-round-b89-pramscan.img   1122304 B  sha256 3f4d004bf024a46ceca22e74f4434ea0a25ff0b915f71a6ff99b0c87b2aa52e2
  FD sha256 b364c568f203f594…
```

---

## 67. 走生态的组件（选项 A）：`QcomXnuUsbDebug` 移植到 xnu-12377.121.6

### 为什么换路线

自己那份 `package/uke_usb.c`（sirius 移植）从 b66 起一直**没有一行输出被观测到**：
`STARTUP()` 注册的入口既不写 pram、也不 panic、`kernel_thread_start` 返回值也看不到 —— 说明
"从 `package/*.c` 归档里注册的 STARTUP 在这个内核里是否执行"本身就是个未解问题。

生态的组件 `QcomXnuUsbDebug`（alioth、munch 等真机在用，自带 `collect-xnu-usb.py` /
`xnu_usb_control.py` / `screenshot.py` 与 28+20 项测试）**不依赖 STARTUP**：它的 worker 由

```c
bsd_autoconf() { kminit();
#if XNU_QCOM_USB_DEBUG
	qcom_usb_start_worker_once();
#endif
```

启动 —— 一个必然执行的调用点。所以改用组件比继续调自己的驱动更快接近目标。

### 版本缺口与移植（本地完成，可验证）

- 组件只带 `darwin22`（xnu-8796）和 `darwin23`（xnu-10063）；本板内核是 **xnu-12377.121.6**
  （profile pin `ac9718f`，已本地 clone 验证）。
- 用 `patch -p1 --fuzz=5` 把 darwin23 两套补丁打到 12377 上，再 `git diff` 重新生成：

```
drivers/QcomXnuUsbDebug/patches/darwin24/0001-qcom-usb-debug-console.patch   git apply --check ✓
drivers/QcomXnuUsbDebug/patches/darwin24/0002-qcom-usb-debug-worker.patch    git apply --check ✓
```

- 落点已核对：console tee 在 `pe_kprintf.c`/`debug.c`/`printf.c`/`serial_console.c`/`serial_general.c`；
  worker 函数在 `bsd_init.c` 文件作用域，`kminit()` 之后调用。

### 接入改动

```
feeds.toml                         登记 QcomXnuUsbDebug 为板级 local 源（树里才有 drivers/QcomXnuUsbDebug）
patches/xnu/zzzzzzzzzzzzzzzzzz-…   darwin24 两份补丁 + series 条目（放在最后）
configs/uke-macos_defconfig        CONFIG_XNU_QCOM_USB_DEBUG=y
board.toml                         重新启用 build-qcom-usb-debug.py；参数加 qcom-usb-debug=1、
                                   去掉 uke_usb_power=11（固件已证明时钟/GDSC 是好的）
patches/xnu/zzzzzzzzzzzzzzzzz-uke-bsd-init-hook.patch   备选挂点（#if XNU_QCOM_USB_DEBUG 守住的弱调用）
```

### 这几轮 CI 上踩的四个坑（全部是编辑级错误，不是设计问题）

| run | 错误 | 正解 |
|---|---|---|
| 42369432329 | 手写补丁 hunk 行数错 → `corrupt patch at line 20` | 按 body 实际行数重算 `@@` |
| 42371643241 | `__attribute__((weak))` 函数在 **arm64e** 上仍要 `l_…$auth_ptr$ia$` 符号 | 不能靠 weak 跨 pass |
| 42373133806 | `uke_last_log_start` 未先声明 → 归档编译失败 → 链接缺 `corecrypto_kext_start`/`mockfs_*` | 加前置声明（一个因、两个错）|
| 42375406774 | `module missing: <tree>/drivers/QcomXnuUsbDebug` | feeds.toml 必须登记 `local = "drivers/QcomXnuUsbDebug"` |

现在跑的是 `de5c79c`（run `42377211143`）。

---

## 68. 生态文档核对（unixnu.com）：三条 USB 路线，以及 uke 的数值逐项吻合

用户给了四篇文档：`/zh/drivers/qcom-dwc3`、`/zh/porting/usb`、`/zh/porting/arm64e`、
`/zh/guide/debugging#usb-调试终端`。抓下来读过之后，结论如下。

### 三条路线（生态里 USB 日志/栈的三个层次）

| 路线 | 是什么 | 板级需要提供 | 我们 |
|---|---|---|---|
| **(a) `QcomXnuUsbDebug`** 组件 | 内核 console tee + CDC-ACM；主机端 `collect-xnu-usb.py` / `xnu_usb_control.py` / `screenshot.py` | 只要 DWC3 到 **device 模式** | **本轮 CI 正在做** |
| **(b) `QcomDwc3`** 组件 | 派生自苹果 `AppleUSBXDCI` / `AppleUSBXHCI`，跑**原版苹果 USB 栈**（枚举、重新枚举、信任弹窗、与 Mac 配对、lockdown、服务启动）| eUSB2 中继器 + QMP PHY + retimer + 电源轨时序、SMMU **stage-1 一致(write-back inner-shareable)上下文**、UCSI(充电器 GLINK)报线缆角色、`board.toml [kernel.prelink] roots` 加入 USB 栈 | 未开始 |
| **(c) `scripts/debug_terminal.py`** | giulia 协议 2 的电脑端终端：CDC 采集、`status`/`replay`、按主题导出、GSC1 帧缓冲投屏 | **设备侧要实现 giulia 协议 2**（只改 USB ID 没用）| 未开始 |

文档明确：路线 (b) 在 **sheng（小米平板 6S Pro, SM8550, iPadOS 27.0.1/24A446）** 上已是
"USB3 10 Gbps / USB2 480 Mbps 枚举、重新枚举、信任弹窗、与 Mac 配对、lockdown 会话"可用；
主机模式部分可用。这正是"arm64e + xnu 12377"那条线 —— **与我们同代**。

### uke 的数值逐项吻合（live DT 核对）

| 文档要求（sheng 值）| uke 实况 | |
|---|---|---|
| DWC3 基址 `0x0a600000`，**苹果寄存器窗口 +0xc000** | `msm-dwc3 reg <0xa600000 0x100000>`；`dwc3@a600000 reg <0xa600000 0xd93c>`（0xd93c > 0xc000）| ✅ |
| SMMU 流 **0x40** | `dwc3 iommus = <0x129 0x40 0x00>` | ✅ |
| GIC **INTID 165** | `dwc3 interrupts = <0 0x85 4>` → SPI 0x85=133，INTID = 133+32 = **165** | ✅ |
| eUSB2 PHY + PMIC 中继器 | `hsphy@88e3000` (`qcom,usb-snps-eusb2-phy`, `qcom,use-eusb2-phy`) + `eusb2-repeater@fd00` (`qcom,pmic-eusb2-repeater`) | ✅ |
| 无 SuperSpeed PHY（uke 只有 eUSB2）| `usb_qmp_dp_phy: ssphy@88e8000` 是 DP-altmode 的 QMP，不是 USB3 SS | ✅ 与 `soc/Uke.h` 的 `CBCR_REQUIRED=3` 一致 |
| eUSB2 参考时钟 **38.4 MHz** | cliffs GCC，待用时再核 | ⏳ |

⇒ **不需要重新摸硬件参数**，文档常量可直接引用；这省掉了 `Qsoc/Uke.h` 现有值的再验证成本。

### 对当前工作的三条影响

1. **arm64e 那篇解释了前几轮的 CI 错误**：27 线（xnu 12377.121.6）以 **arm64e + PAC** 编译，
   所以 `__attribute__((weak))` 函数仍需 `l_…$auth_ptr$ia$` 签名符号（§67 第 2 条）。
2. **路线 (a) 是最短路径**：只要 DWC3 device 模式，不需要苹果 USB 栈、不需要 prelink roots。
   而固件 `UKE-GCC` 读出的继承状态（GDSC 上电、5 个 CBCR 全跑、`BCR=0`、`DEVCTRLHLT=1`）
   恰好就是 `QcomUsbQualify` 要的状态。
3. **路线 (b) 的缺口清单（以后要苹果 USB 栈时）**：
   - `board.toml [kernel.prelink] roots` 里**一个 USB 栈都没有**（查过：`IOUSBDeviceFamily` /
     `AppleUSBDeviceMux` / `IOUSBHostFamily` / `USBDeviceConfiguration` 均不在）；
   - 类布局脚本 `gen-usb27-abi.py` / `gen-usb27-host-abi.py` 是按 **24A446** kernelcache 生成的，
     我们是 **25F84** —— 要么按其说明改脚本里的 kernelcache 与类地址，要么接受布局风险；
   - 启动参数 `sheng-usb=1`（2 = 加寄存器采样，3 = 让控制器离线重接）与 `log-sys=1`
     （打印 `native_usb_stats` 的 `[usb]` 行，含 bringUp 9 个阶段里失败的那一步）；
   - UCSI/充电器 GLINK（线缆角色）—— 我们固件链里没有 vendor charger（§57 里它还会开看门狗）。

### 文档里两条特别值得记住的排查提示

- **"chirp 通过、高速数据失败 → eUSB2 参考时钟表"**：枚举到一半断，先查 38.4 MHz 参考时钟。
- **`ep0OutEventOccurred: event 0` → stage 1 SMMU 上下文**：苹果栈不做缓存维护，
  stream 0x40 必须给它独立的 write-back inner-shareable 恒等映射上下文。

---

## 69. 工作区整理（文件都搬到哪了 + 三份索引）

根目录此前混着录屏、分区 dump、KDK dmg；`work/` 里还有十几份旧 commit message。已整理并写了三份索引：

| 新位置 | 内容 |
|---|---|
| `work/INDEX.md` | **第一页**：三仓库/两镜像/常用命令/目录地图/当前状态/取证方式 |
| `work/flash/INDEX.md` | 45 个镜像的表格（类型、字节、sha256 前 16、时间、用途、是否被取代）|
| `work/analysis/README.md` | 工具索引，按用途分组（prelink、AppleImage4/vtable、固件分析、内核产物、取证）|

搬动清单：

```
VID_20261107_190413.mp4 / VID_20261107_191325.mp4   → work/captures/
logfs.img / oops.img                                → work/device-dumps/
Kernel_Debug_Kit_26.5.2_build_25F84.dmg (1.1 GB)    → work/apple/
work/commit-msg*.txt (12 份)                        → work/archive/commit-msgs/
uke-kernel.log (0 字节)                             → 删除
```

没有动的：`work/tablet-backup-20261103/`（14 GB 整机备份，**不要删**）、`device/uke-backup/`（恢复用 stock）、
`work/ci-artifacts/`（3 GB，旧轮次可删）。`work/analysis/xnu-12377/` 是内核真实源码（`ac9718f`），
以后验证补丁不用再 clone。

---

## 70. `--target=` 被拒的真相：它是 `-Xclang` 递给 cc1 的参数，不是 driver 的；以及 preflight 的盲区

上一轮（run `42384716333`，commit `0fc1cf0`）的日志看起来自相矛盾：

```
record: dropped -mlinker-version=1267 (this compiler rejects it for a compile)
record: keeping --target=arm64e-apple-ios26.5.2 (protected); probe said: error: unknown argument: '--target=...'
preflight: 0 error(s) across 5 units
--- QcomXnuUsbDebug.log ---
error: unknown argument: '--target=arm64e-apple-ios26.5.2'
```

### 两个独立的错误，都被我自己的代码挡住看不见

1. **`--target=…` 是 cc1 参数。** 编译记录里它是成对出现的：

   ```
   '-Xclang', '--target=arm64e-apple-ios26.5.2'
   ```

   cc1 没有 `--target`（cc1 只有 `-triple`），所以 driver 在 `-c` 路径上直接拒绝。
   而我的探针**确实报告了拒绝**，只是保护名单里第一个前缀就是 `--target`，于是走了
   "keeping (protected)" 并 `break` 出循环 —— 那对参数原封不动留在记录里，交给 `build.py`
   编译，于是必然失败。保护名单本意是别丢掉 target，但它没区分 "driver 级 target" 和
   "`-Xclang` 转发给 cc1 的伪 target"。

2. **`preflight: 0 error(s)` 是假的。** 过滤条件是 `': error:' in line`，而 driver 级诊断
   `error: unknown argument: '…'` **没有 `file:line:` 前缀**，所以五个单元的失败全被过滤掉了。
   这是让这一轮白跑的第二个原因：报告说干净，实际全灭。

### 修法（`scripts/build-qcom-usb-debug.py`，commit `c952140`）

- 探针报出被拒参数时先定位它在列表里的下标；若**紧前一个 token 是 `-Xclang`**，就判定它是
  cc1 选项，把这一对一起删掉，并且**不受保护名单限制** —— 因为 driver 级 `-arch arm64e`
  已经设好 triple（记录里实测确有：`'-D', 'ARM64_BOARD_CONFIG_BCM2837', '-arch', 'arm64e', '-D', 'arm64'`）。
  保护名单从此只对 driver 级参数生效。
- 记录只算一次，preflight 与 `build.py` 共用同一份（之前在日志里跑两遍，看起来像探针跑了两次）。
- preflight 的错误过滤改成 `(^|: )error: `，并且在 `returncode != 0` 却匹配不到诊断时，
  直接把 stderr 前几行当作错误打印 —— 再也不允许出现 "0 error(s)" 伴随失败。

### 本地验证（用桩编译器，不需要 macOS）

桩编译器只做两件真实行为：拒绝 `-mlinker-version*`，拒绝 `-Xclang --target=*`。断言结果：

```
dropped -mlinker-version=1267 (this compiler rejects it for a compile)
dropped --target=arm64e-apple-ios26.5.2 with its -Xclang (forwarded to cc1, which has no such option;
        -arch already sets the triple)
ASSERTIONS OK (-Xclang pair dropped, -arch kept, -W + linker flags dropped, idempotent)
PREFLIGHT OK: driver-level rejections are counted, clean records report 0
```

即：假 target 与链接器参数被删、两对真 `-Xclang` 与 `-arch arm64e` 保留、`-W` 全集仍被删、
二次运行是 no-op；而 driver 级拒绝现在会让 preflight 报 1 个错误而不是 0 个。

### 本轮状态

- 分支 `ci-ref-lto2` 已推到 `c952140`，内核 run **`42386664238`**（`build-uke-kernel-min`，约 45 分钟）。
- 工作流最后一步是 `Upload artifacts (always)`，所以**即使归档编译失败，日志也会回来**：
  下一步要读的是 preflight 的逐单元错误行与 `archive-17-build-qcom-usb-debug.log`。
- 若这一轮 preflight 终于吐出真实的 12377 不兼容清单，那是**好消息**：一轮就能拿到全部单元的错误，
  一次性修完（这正是 preflight 存在的理由）。

---

## 71. 归档真正的编译阻碍（用 12377 真实头文件逐符号核对出来的）

上一轮只死在参数层，五个单元一个都没编到。所以在等 CI 的时间把
`package/qcom-usb-debug` 的五个单元对着 `work/analysis/xnu-12377/` 做了一次**逐符号**核对
（用 clang 真实编译 + 在 12377 里 grep 声明/定义）。结论：

### 真阻碍（已修，commit 见下）

| 位置 | 症状 | 12377 里的事实 | 修法 |
|---|---|---|---|
| `kernel/QcomXnuUsbDebug.c` `ml_io_map` ×3 | `call to undeclared function 'ml_io_map'` | 声明在 `osfmk/arm/machine_routines.h:603`，被 `#ifdef __APPLE_API_PRIVATE` **且** (`PEXPERT_KERNEL_PRIVATE` \|\| `MACH_KERNEL_PRIVATE`) 两层包住 | 记录里有 `PEXPERT_KERNEL_PRIVATE`，**但没有 `__APPLE_API_PRIVATE`** → 本文件内直接 `extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);`（原型与头文件逐字相同）|
| `scripts/build.py` 导入审计 | `RuntimeError: unexpected XNU imports: ['_boot_cpu_id']` | 12377 的启动 CPU 全局是 `boot_cpu_id`（`osfmk/kern/cpu_number.h:62`，`XNU_KERNEL_PRIVATE` 内），10063 才是 `master_cpu` | 白名单加 `_boot_cpu_id`，同时保留 `_master_cpu`（旧 profile 仍可用）|
| 同上，`append_hex` | clang ≥19 的 `-Wunterminated-string-initialization` | 与内核无关，只是长度写死 16 | `digits[16]` → `digits[]`（唯一用途是 `& 0xfu` 下标，没有 `sizeof`）|

### 已排除的两个"看起来是阻碍"的点

- **`boot_cpu_id` 需要 `XNU_KERNEL_PRIVATE`？** 记录里**已经有** `-D XNU_KERNEL_PRIVATE`（实测：
  记录中 `XNU_KERNEL_PRIVATE` ×1、`PEXPERT_KERNEL_PRIVATE` ×1、`KERNEL_PRIVATE` ×3、
  `MACH_KERNEL_PRIVATE` ×0、`__APPLE_API_PRIVATE` ×0）。所以声明可见，只需改名不需要加宏。
- **`<mach_assert.h> not found`？** 它只经 `osfmk/machine/machine_routines.h` 在
  `MACH_KERNEL_PRIVATE` 下被拉到，而记录里没有这个宏 —— 我们**没有**用 `-DMACH_KERNEL_PRIVATE`
  去修 `ml_io_map`，正是为了不踩这个坑（它只存在于 `build/xnu/ios-obj/EXPORT_HDRS`，源码树里没有）。
  若将来某份记录真的带 `MACH_KERNEL_PRIVATE`，正确做法是补
  `-I<tree>/build/xnu/ios-obj/EXPORT_HDRS`，而不是硬塞 API 宏。

### 无需处理的告警

- `boot_progress_persist` 在 12377 里不存在，是本站 display 补丁的符号，模块里以 `weak_import`
  声明 → 链接成 NULL，代码里已判空（只是丢掉那两行报告）。
- `-Wunsafe-buffer-usage` / `-Wshadow-header`：只在 clang ≥19 且 `-Weverything` 下出现；
  归档用的记录 `-W` 全集已被 wrapper 丢弃，所以不会变成错误。

### 顺带核对清楚的"链接契约"（省一轮"

补丁调用的 13 个 `qcom_usb_debug_*` 符号，**每一个**都在 `include/QcomXnuUsbDebug.h` 声明、
在组件某个 `.c` 里**恰好定义一次**；归档路径
`<tree>/.cache/qcom-usb-debug/libQcomXnuUsbDebug.a` 与 `build-xnu-4k-baseline.sh` 里
`-Wl,-force_load` 的路径一致；`Upload artifacts (always)` 保证即使归档失败日志也会回来。

---

## 72. 归档第一次真的编完了五个单元 —— 死于导入审计（`_boot_cpu_id`），已修

run `42386664238`（`c952140`，只带参数修正、不带单符号修正）的实测结果，是到目前为止最有信息量的一轮：

```
preflight: 0 error(s) across 5 units          ← 这次是真的 0
== kernel: archive-17-build-qcom-usb-debug
  Dwc3.c:299/300/409: warning: implicit conversion changes signedness [-Wsign-conversion]
  QcomUsbQualify.c:145:19: warning: variable 's2cr' may be uninitialized [-Wconditional-uninitialized]
  build-qcom-usb-debug.py: component build failed (exit 1)
  File "kits/uke/qcom-usb-debug/scripts/build.py", line 125, in main
  RuntimeError: unexpected XNU imports: ['_boot_cpu_id']
```

也就是说：**五个翻译单元全部编译成功**（只剩两条 warning），卡在 `build.py` 自己的导入审计 ——
白名单里还是 10063 的 `_master_cpu`，而这台内核导出的是 `_boot_cpu_id`。这正是子代理审计预测的
那一条，`d77649e` 已经把它修掉（两个名字都接受）。

### 对 §71 的两处更正（以实测为准）

1. **`ml_io_map` 不需要在本文件里 extern。** §71 把它列为"真阻碍"，实测是**误报**：
   `__APPLE_API_PRIVATE` 是在 `bsd/sys/appleapiopts.h:52` 无条件（除非
   `__APPLE_API_STRICT_CONFORMANCE`）定义的，而 `<mach/message.h>` 会包含它 —— 内核里的每次编译
   都天然有这个宏。`c952140` 里**没有**那句 extern，五个单元照样编译通过。
   所以 `d77649e` 里加的那句成了冗余声明（同型、无害），已在下一个 commit 里删掉；
   附带的好处是 §71 里那条 `<mach_assert.h>` 的隐忧也随之消失。
2. 真正必要的只有两处：`scripts/build.py` 的导入白名单（`_boot_cpu_id`）和
   `digits[16]` → `digits[]`（clang ≥19 的 `-Wunterminated-string-initialization`）。

### `s2cr` 那条 warning 是误报，但值得记下来

`QcomUsbQualifyIdentityDma`：`extended` 分支在 `:129` 读 `s2cr`（失败即 return），非 extended 分支
在 `:140` 读（失败即 return），两条路径到 `:145` 的 `state->s2cr = s2cr;` 之前都赋过值；clang 的流分析
不认这种"逗号条件里的 read32 失败即返回"，于是报可能未初始化。**不改代码**（归档现在能编，动它是无收益的风险）。

### 这轮还确认了一个流程事实

`uke-macos_defconfig` 走的是 driver 自己构建归档的路径：workflow 的 step 14/15
（`Pass 2`、`Build the first-console archives`）只对 `uke_defconfig`/`uke-nodisplay_defconfig` 生效，
对 `uke-macos_defconfig` 是 **skipped**；板级 `[kernel] archives` 那 17 个归档在
**step 17「Link the kernel」内部**构建，然后才链接。所以：

- 归档失败会在 step 17 开始后**约 8 分钟**就暴露（`archive-17-…log`），不用等满 45 分钟；
- 反过来，step 17 跑过 10 分钟还在继续，就说明归档已经过了。

---

## 73. 归档真的编出来了（`5931e57a…`），然后死在两个**补丁移植**错误上

run `42387297776`（`d77649e`）的结果，一句话：**`libQcomXnuUsbDebug.a` 成功生成**，
然后内核编译挂在两处补丁上 —— 都是我把 darwin23 补丁搬到 12377 时搬错的地方，不是设计问题。

```
== kernel: archive-17-build-qcom-usb-debug
QcomXnuUsbDebug: 5931e57a553207f7 soc=soc/Uke.h          ← 归档成功，sha256 前 16 位
...
osfmk/kern/printf.c:965:3: error: expected identifier or '('
  965 |                 if (!qcom_usb_pe_kputc_reenters_console())
bsd/kern/bsd_init.c:1338/1348/1426/1437/1442/1444: error: use of undeclared identifier 'master_cpu'
```

### 缺陷一：`printf.c` 的 guard 落到了函数外面

darwin23 原补丁的意图是：在两个 fan-out 里，给 **`PE_kputc(c);`** 前面加条件
（`if (!qcom_usb_pe_kputc_reenters_console())`），避免 USB sink 自己再把字节喂回 `_cnputs`。
12377 的 fan-out 形状变了（多了 `(PE_kputc != NULL)` 与 `sptm_serial_putc` 分支），我移植时：

- 第一个 hunk 把 guard 加到了 **`debug_putc(c);`** 前面（目标错了，而且 `debug_putc` 只写 msgbuf、
  根本不会重入控制台，加了反而会丢掉内核日志字节）；
- 第二个 hunk 把 guard 加到了 **`consdebug_putc_unbuffered` 的函数定义之前** —— 也就是文件作用域，
  于是 `error: expected identifier or '('`。

修法：按 darwin23 的语义重写，两处都改成在 `PE_kputc(c);` 之前加 guard。

### 缺陷二：worker 补丁还写着 10063 的 `master_cpu`

`bsd_init.c` 新增的 7 处 `master_cpu` 在 12377 里不存在（改名 `boot_cpu_id`）。
`-DXNU_KERNEL_PRIVATE` 在 `makedefs/MakeInc.def:157` 是**全局**定义的，所以
`<kern/cpu_number.h>`（补丁已经 include）能声明 `boot_cpu_id`，直接改名即可。

### 这次不再手写补丁

两份补丁都用**本地 12377 clone 重新生成**，不再手写 hunk 行数：

```sh
# 在 work/analysis/xnu-12377（checkout ac9718f，干净树）
# 1) 只保留原补丁中除 printf.c 以外的 hunk 并 git apply
# 2) 手动把 printf.c 改成"guard 在 PE_kputc 前"
# 3) git diff -- <五个文件> > patches/darwin24/0001-…patch
#    sed -i s/master_cpu/boot_cpu_id/g bsd/kern/bsd_init.c
#    git diff -- bsd/kern/bsd_init.c > patches/darwin24/0002-…patch
```

验证（都过）：

- `git apply --check` 对两份补丁在**干净树**上通过；
- 与 darwin23 原补丁逐行比对"新增行"：`serial_console.c`/`serial_general.c`/`debug.c`/`pe_kprintf.c`
  的新增行**逐字相同**，只有 `printf.c` 的注释措辞不同（结构相同，两处 guard）；
- 用花括号深度扫一遍所有新增行：**没有任何语句落在函数作用域外**（剩下 21 条 depth=0 的都是
  `extern`/`static` 变量/函数定义，合法）。

### 下一轮的预期

只剩"内核编译 + 链接"要过：归档已在（同一次构建里会重建），`-force_load` 的契约已核对，
`boot_progress_persist` 是 weak（顶多是丢两行报告）。若还失败，错误一定在**别的补丁落点**或链接符号，
而 `-k` 会让一轮就把它们全报出来。

### 73b. 差点又被同一个坑咬一次：`patches/xnu/` 里放的是**副本**

修好 `package/qcom-usb-debug/patches/darwin24/` 的两份补丁并推上去之后，立刻核对了一件本该更早核对的事：
**树的 `patches/xnu/series` 应用的是 `patches/xnu/zzzz…-qcom-usb-debug-*.patch` 这两份"副本"**，
sha256 与组件里的原件不同 —— 也就是说我修的是**永远不会被应用的那一份**，而 run `42389458330` 会拿旧的
（含 `master_cpu`、含错位 guard 的）副本再失败一次。

```
83d4907…  patches/xnu/zzzzzzzzzzzzzzzzzz-qcom-usb-debug-console.patch   ← 树应用这份
43d4907…  package/qcom-usb-debug/patches/darwin24/0001-…console.patch    ← 组件持有这份
```

处理：

1. 两份副本改成与组件原件**逐字节相同**（`cp` 覆盖），两份都在干净 12377 树上
   `git apply --check` 通过；
2. 新增 `scripts/check-patch-copies.py`：比对两份副本与组件原件、并检查 `series` 里确实列了它们，
   不一致就 `exit 1` 并直接打印该执行的 `cp` 命令；已实测"一致→通过、加一行→报错"；
3. 把它挂到 workflow 的**第 3 步**（checkout 之后、一切构建之前），漂移从此在 30 秒内暴露，
   而不是 8 分钟（YAML 已用 `yaml.safe_load` 校验，步骤位置与 `working-directory: UniXNU-uke` 都确认过）。

教训写在这里：**"补丁改在哪一份"必须每次核对 sha256**，不能凭路径相似就假设是同一条链。

---

## 74. 补丁全过了，内核编到链接 —— 死在 `_boot_progress_persist`（`weak_import` 在 LTO/arm64e 下不够）

run `42389591998`（`c89b8b1`）：新增的补丁副本一致性检查（step 5）**通过**，归档与内核编译全部走完
（link 步跑了 **21 分钟**，不再是一开始就炸），然后链接只剩一个符号：

```
Undefined symbols for architecture arm64e.kernel:
  "_boot_progress_persist", referenced from:
      _qcom_usb_debug_init in lto.o
      _fail in lto.o
      l_boot_progress_persist$auth_ptr$ia$54535 in lto.o
```

### 为什么 `weak_import` 没能让它"可选"

组件里原本写的是：

```c
extern void boot_progress_persist(const char *, uint32_t) __attribute__((weak_import));
if (&boot_progress_persist) { … }
```

`weak_import` 在普通 Mach-O 里确实会"找不到就绑成 0"，但这里有两个前提不成立：

1. **LTO**（本分支内核就是 LTO）：调用点从 bitcode 降级时才落成引用，ld 看到的是一个普通未定义符号；
2. **arm64e**：引用被物化成**带 PAC 的指针 stub**（`l_boot_progress_persist$auth_ptr$ia$54535`），
   这个 stub 本身是普通符号，`weak_import` 的语义没有传到它身上。

而且这个符号在**本仓库里根本没有提供者**：`boot_progress_persist` 属于显示补丁（`uke-nodisplay`
那套图形归档在这个配置里是关掉的），`grep -rn boot_progress_persist patches/ package/` 除组件自身外为空。
所以它永远解析不到，必须让"可选"变成"有定义"。

### 修法：一个 weak **定义**（不是声明）

```c
__attribute__((weak)) void
boot_progress_persist(const char *text, uint32_t colour) { (void)text; (void)colour; }
```

- 这正是本仓库已经验证可行的手法：`package/MissingKernelSymbols.c` 里 `csr_check` /
  `csr_get_active_config` 就是 weak 定义，`-force_load` 进内核后一直链接正常；
- 显示补丁一旦回来，内核实实现会**覆盖**这个空实现，不会产生重复符号；
- 顺带删掉两处已经恒真的 `if (&boot_progress_persist)` 守卫并订正注释；花括号计数前后都是
  168→167（两侧同步减一）；组件里已无任何 `weak_import`。

### 顺带说明：`kernel-bootstrap.log` 里的 corecrypto/mockfs 未定义符号**不是**新问题

那是 bootstrap 配置（不带任何归档）**按设计**会链接失败的结果，workflow 的 step 13 一直容忍它；
失败报告步骤只是把每个日志里的 error 行都列出来，所以它和真正的失败出现在同一屏。

### 下一轮只剩一件事要验证

内核编译 + 链接。归档契约已逐符号核对（13 个 `qcom_usb_debug_*` 各有唯一定义），
`boot_progress_persist` 现在有定义，剩下的未定义集会由 `-k` 一次报全。

---

## 75. 内核**链接成功**了；卡在 prelink：4 MiB 的日志环把 `__DATA` 撑出了"苹果内核数据区"

run `42392182678`（`90d0872`）是到目前为止最好的一轮：

```
built kernel at …/DEVELOPMENT_ARM64_BCM2837//mach.development.bcm2837
Source kernel POST integrity field populated
```

**`mach.development.bcm2837` 生成成功** —— `_boot_progress_persist` 的 weak 定义解决了问题，
`-force_load` 的归档、六个 hook 单元、`-Werror` 全都过了。失败点往后移到了 **prelink**：

```
ValueError: Native __DATA overlaps a retained segment
  kits/XnuPrelink/fileset_layout.py:203 in validate()
```

### 原因：`Layout` 把我们的 `__DATA` 放进"苹果内核原来占的那块"，而这块装不下 4 MiB

读 `fileset_layout.py` 就清楚：`Layout.__init__` 里

```python
self.roots = {s['name']: dict(s) for s in cache.segments}     # 苹果 kernelcache 的映射
for name, (_, offset) in cache.entries.items():
    if name == 'com.apple.kernel' or …:  continue             # 苹果内核本身不算"保留段"
    …self.occupied.append((segment['va'], segment['va']+segment['size']))   # 被保留的 kext 段
…
self.reserve('__DATA', self.roots['__DATA']['va'])            # 我们的 __DATA 放在映射开头
```

也就是说：**原生内核的 `__DATA` 必须塞进"苹果内核数据原来占的那段空隙"**，一页之后就可能是
第一个被保留的 kext 的 `__DATA`。而组件里有：

```
SOC_USB_LOG_BUFFER_BYTES = 4 MiB      ← kernel/QcomUsbBuffer.h 里的静态日志环
usb_dma        32 KiB  (DWC3_DMA_BYTES)
usb_tables     12 KiB  (QUSB_DOMAIN_PT_BYTES)
usb_context    20 KiB  (DWC3_CONTEXT_BYTES)
```

4 MiB 的环直接把原生 `__DATA` 从"刚好装得下"推到溢出。

### 75b. 用 b87 的产物把"空隙到底多大"量出来（结果：64 KiB 装得下，余量约 100 KiB）

上一段里我写的"b87 的 `__DATA` 是 4,849,664 B"其实是 **顶层 `__DATA` 映射**的大小，不是原生内核数据段。
把 `work/ci-artifacts/b87/kernel.macho`（上一版能过的 collection，MH_FILESET）逐条 fileset entry 解开后：

```
顶层 __DATA 映射          0xfffffff00c54c000 .. 0xfffffff00c9ec000   (0x4a0000)
原生内核 __DATA（本条）    0xfffffff00c54c000 + 0x248000              → 0x248000 = 2,392,064 B
第一个被保留 kext 的 __DATA 0xfffffff00c7cb8b0 + 0x13f9
                             ⇒ 空隙 = 0xc7cb8b0 − 0xc792000 = 0x398b0 = 236,720 B
```

也就是说原生内核 `__DATA` 必须 ≤ **2,628,784 B**（2,392,064 + 236,720）。本组件的静态数据：

```
64 KiB 日志环 + 32 KiB DMA + 12 KiB SMMU 表 + 20 KiB DWC3 上下文 ≈ 128 KiB
⇒ 2,392,064 + ~130 KiB ≈ 2.52 MB < 2.63 MB        余量 ≈ 100 KiB  ✓
4 MiB 环时代：2.39 MB + 4.26 MB ≈ 6.65 MB ≫ 2.63 MB  超出 ≈ 4 MB  ✗（与报错一致）
```

量法本身也记下来（以后要复核就用它）：解析顶层 `LC_SEGMENT_64` 拿到映射，再用 `LC_FILESET_ENTRY`
（0x80000035）逐条解析嵌套 Mach-O 的 `__DATA`，取"第一个 ≥ 原生数据起始地址的 kext 段"。

### 修法

1. **环从 4 MiB 降到 64 KiB**（`kernel/QcomUsbBuffer.h`）。归档全部静态数据因此约 128 KiB：
   64 + 32 + 12 + 20。并在宏上方写清"这个大小是 __DATA 预算，不是随便调的" ——
   它受 prelink 空隙约束，overflow 有计数、不静默丢。CDC-ACM 一起来 worker 就持续排空，
   容量只需要覆盖"传输通道还不存在"的那段窗口。
2. **把失败信息变成数字**：`fileset_layout.py` 新增 `overlap_report()`，冲突时直接打印
   放置区间、需要的字节数、该映射的空闲区间列表，以及"超了多少字节"。示例（合成 layout 实测）：

   ```
   Native __DATA overlaps a retained segment: placed 0x70000000..0x70140000, needs 0x140000 bytes;
   __DATA mapping 0x70000000..0x70200000 free: 0x70000000..0x70010000 (0x10000),
   0x70018000..0x70020000 (0x8000), 0x70030000..0x70200000 (0x1d0000);
   the range the placement landed in holds 0x10000 bytes, so the kernel exceeds it by 0x130000
   ```

   下次若还装不下，日志会直接告诉我们要么还要再缩多少、要么空隙有多大。

### 备选方案（先不用，但已经能看到）
空闲区间列表里通常**还有别的空隙**（上例尾部有 0x1d0000），机械上完全可以把原生 `__DATA`
放进后面的空隙。但 XNU 会从原生数据段的起始推导 kext 数据范围（对照 `__TEXT_EXEC` 那段注释），
换位置属于"能过 prelink 但语义变了的改动"，要花一次真机验证。**优先收缩，不换语义。**

### 现在的位置
从"归档能不能编"一路推进到：归档 ✅ → 内核编译 ✅ → 内核链接 ✅ → **只剩 prelink 的布局约束**。

---

## 76. 真正的元凶是**16 MiB 的截图缓冲**（不是 4 MiB 的日志环）；两个都缩，`__DATA` 才落进空隙

run `42396020234`（`3ccca8d`，把日志环从 4 MiB 缩到 64 KiB）**还是**同样的错，但这一轮我上一段加的诊断
把数字直接打出来了 —— 于是我第一次拿到了真实账本：

```
Native __DATA overlaps a retained segment: placed 0xfffffff00c54c000..0xfffffff00d7b4000,
needs 0x1268000 bytes; __DATA mapping 0xfffffff00c54c000..0xfffffff00c9ec000 free:
0xfffffff00c54c000..0xfffffff00c7cb8b0 (0x27f8b0), … ; the range the placement landed in holds
0x27f8b0 bytes, so the kernel exceeds it by 0xfe8750
```

- **原生 `__DATA` 需要 0x1268000 = 19,300,352 B（18.4 MiB）**
- **空隙只有 0x27f8b0 = 2,619,568 B（2.5 MiB）**，超出 0xfe8750 ≈ 16 MiB

把 19,300,352 减去组件的静态数据（16 MiB 截图缓冲 + 64 KiB 环 + 32 KiB DMA + 12 KiB SMMU 表 +
20 KiB DWC3 上下文 = 16,908,288），剩下的正是 **2,392,064 = 0x248000** —— 与 b87 那次能过的
原生 `__DATA` **一模一样**。也就是说：基础内核没变，全部增量都来自组件，而其中 **16 MiB 是
`kernel/QcomUsbScreenshot.c:5` 的 `static uint8_t pixels[QUD_SCREENSHOT_MAX_BYTES]`**（默认 16 MiB），
4 MiB 的环只是第二号。（§75 把环当成唯一原因，这里更正。）

### 修法：32 KiB 截图上限，且用"共享编译记录"同时喂给 preflight 和归档编译

`include/QcomUsbScreenshot.h` 本来就是 `#ifndef QUD_SCREENSHOT_MAX_BYTES` 设计，板级可以覆盖。
但**必须保证 preflight 与归档编译看到同一个值**，否则 preflight 就是在骗人。做法是把它作为
`BOARD_DEFINES` 追加进 `scripts/build-qcom-usb-debug.py` 写出的**那份 compile record** ——
preflight 与组件 `build.py` 都从这份记录取参数（已用 `compile_flags()` 实测该 `-D` 不被丢弃）。

```
-DQUD_SCREENSHOT_MAX_BYTES=32768
```

预算（现在全部对得上）：

```
基础内核 __DATA            2,392,064
组件静态（32 KiB 像素 + 64 KiB 环 + 32 KiB DMA + 12 KiB 表 + 20 KiB 上下文）  163,840
合计                       2,555,904   <  空隙 2,619,568   余量 63,664 B  ✓
（若像素留 64 KiB：2,588,672，余量 30,896 B —— 也能过，但余量只有一半）
```

代价说清楚：32 KiB 的 RGB888 只够 ~104×104 的帧，**这个板子现在也根本没有显示驱动**，所以截屏
功能等于保留接口、实际拒绝大帧。真要截屏，正确做法是先把 prelink 的数据区间放宽（或者让截图缓冲
改成运行时分配），而不是把 16 MiB 静态塞进内核 `__DATA`。

### 一句话现状
`fileset_layout.py` 的 `overlap_report()` 只花了一次构建就把"到底差多少字节"变成确定的数字 ——
这是这几轮里投入产出比最高的一处改动。

---

## 77. ✅ CI 全绿：带 `QcomXnuUsbDebug` 的 prelinked 内核出来了（run `42447661352`，commit `80dbaf1`）

```
step 18  Link the kernel (prelink included)   success
== kernel: prelink
== kernel: …/uke-macos/kernel/kernel.macho sha256 4f6f0d654e29dfef9d09fedc66a77dd5e81e0381454b6e580d7691e01822f485
-rw-------  1 runner  staff  136577024 Nov  8 05:12 kernel.macho
RUN 42447661352 success
```

- 产物：`work/ci-artifacts/b90/kernel.macho`，**136,577,024 B**，
  sha256 `4f6f0d654e29dfef9d09fedc66a77dd5e81e0381454b6e580d7691e01822f485`
  （配对数据 `kernel.map.json` 也在同一目录）
- **组件确实在集合里**（直接在 kernel.macho 里数出现次数）：

```
"QCOM XNU USB CDC ACM v1"        1
"QCOM USB ACTIVE (CDC-ACM up)"   1
qcom_usb_debug_write             1
QcomXnuUsbDebug                  2
```

### 这条链现在全通（每一环都有 CI 证据）

| 环节 | 证据 |
|---|---|
| 组件归档 `libQcomXnuUsbDebug.a` | `QcomXnuUsbDebug: fdf4ca14fac52ddc soc=soc/Uke.h` |
| 六个 hook 单元（console tee / worker） | 内核编译通过（`kernel.log` 无 error） |
| 归档符号契约（13 个 `qcom_usb_debug_*`） | 链接通过；唯一遗留 `boot_progress_persist` 用 weak 定义解决 |
| prelink 布局（`__DATA` 预算） | 32 KiB 截图上限 + 64 KiB 日志环 → 2,555,904 ≤ 2,619,568 |
| 最终集合 | 136,577,024 B，组件字符串在里面 |

### 接下来是真机（b90）

payload 已用 b90 内核重建（`boot.img` + `vendor_boot_uke.img`）。真机这一轮的看点：

1. **主机侧**：`lsusb` / `dmesg` 是否出现 **CDC-ACM 设备**；`/dev/ttyACM*` 是否存在；
   若有，`cat /dev/ttyACM0`（或 `work/analysis/collect-xnu-usb.py`）就能**不拍屏幕**读到内核日志 ——
   这是本项目的原始目标。
2. **若枚举失败**：屏幕/固件里 `QCOM USB REFUSED error=N` 会直接点名是哪一项预检没过
   （`fail()` 会把原因画到板上 diagnostic page，走 `boot_progress_persist`；本板没有显示驱动，
   所以更可能是固件侧 `pram slot` 与我们自己的 beacon 兜底）。
3. 固件侧（b89 pram 扫描）仍会给 `pram slot 0x1000/0x1100/0x1200`、`pram find "UKEUSB"`、
   `UKE-GCC:` —— 内核崩溃时这些是唯一证据。

### 77b. b90 镜像已就位（可直接刷）

```
work/flash/vendor_boot_round-b90-usbcdc.img  85782528 B  sha256 b7fe516d078624f7…
    ← CI 全绿内核 4f6f0d654e29dfef…（136,577,024 B，内含 QcomXnuUsbDebug）打成的 80 MiB payload
work/flash/boot-round-b90-usbcdc.img          1122304 B  sha256 3f4d004bf024a46c…
    ← 与 b89 pramscan 固件**逐字节相同**（同一次 payload 构建顺带产出，省得再等一次）
```

刷法（两段式，和之前一样）：

```sh
adb reboot bootloader
fastboot flash vendor_boot_a work/flash/vendor_boot_round-b90-usbcdc.img   # 写 flash 分区
fastboot boot work/flash/boot-round-b90-usbcdc.img                          # 只进 RAM，UEFI 临时启动
```

**这一轮第一次有"不需要拍屏幕"的读法**：主机侧同时观察

```sh
lsusb                       # 期待出现 CDC-ACM（或 Qualcomm/DWC3 相关）设备
dmesg | tail -30            # 期待 cdc_acm 绑定：ttyACM0
ls /dev/ttyACM*             # 存在即成功枚举
cat /dev/ttyACM0            # 内核日志通道（也可用 work/analysis/collect-xnu-usb.py）
```

若枚举不出来，证据链仍然是：固件的 `pram slot 0x1000 / 0x1100 / 0x1200`、`pram find "UKEUSB"`、
`UKE-GCC:` 三段读数，以及面板上的 `QCOM USB REFUSED error=N`（`fail()` 的唯一出口；
本板没有显示驱动，这一行更可能表现为 pram 里的 beacon）。

---

## 78. b90 真机没有 CDC-ACM 的原因找到了：**内核参数让旧探针驱动故意 panic**（b91 已修）

真机结果（用户的 `dmesg`/`lsusb`）：主机看到 Android/fastboot 设备（`18d1:d00d`）一次次来去，
**没有任何 `1209:316d`**（组件的 VID/PID：`QUD_USB_VID 0x1209` / `QUD_USB_PID 0x316d`，
Manufacturer "UniXNU"、Product "Qualcomm XNU Debug"、Serial "QcomXnuUsbDebug"），也没有 `/dev/ttyACM*`。

不是枚举时序问题，是**这一版 payload 的内核参数根本不允许启动走到那一步**。`payload.kernel_args`
里还留着上一阶段的三个"决定性试验"开关：

| 参数 | 代码行为 |
|---|---|
| `uke_usb_sync=1` | `uke_usb_startup()` 里**第一件事就是 panic**："UKE USB: startup entry reached (uke_usb_sync=1)"。STARTUP 在 `bsd_autoconf` 之前跑，所以**组件 worker 永远轮不到** |
| `uke_usb_hook=1` + `uke_usb=1` | 走到 `uke_usb_probe_and_freeze()`，配 `uke_usb_panic=1` 再 panic 一次 |
| `uke_usb=1`（无 `uke_usb_power`） | 默认路径把 GDSC + 五个 USB 分支时钟全部打开并初始化 DWC3 —— 和组件抢同一个控制器 |

三条任何一个都足以让 CDC-ACM 不可能出现；它们同时开着。

### 修法（本机重建 payload，6 分钟，不用 CI）

`board.toml` 的 `payload.kernel_args`：

```diff
-  "uke_usb=1",
-  "uke_usb_sync=1",
-  "uke_usb_hook=1",
-  "uke_usb_panic=1",
+  "uke_usb=0",
+  "uke_usb_sync=0",
+  "uke_usb_hook=0",
+  "uke_usb_panic=0",
   "qcom-usb-debug=1",
```

- 旧驱动从此**完全惰性**（`if (!enabled) return;` 之前只做一次 pram 写：
  `UKEUSB-STARTUP-ENTERED` / `UKEUSB-DISABLED`，这是**保留下来**的有用证据，不碰 DWC3、不 panic）；
- 组件独占 DWC3；
- 参数总长 978 B（+`native-ufs-root=0` = 996 B），仍在 1000 B 上限内（已用 `tomllib` 复核）。

```
work/flash/vendor_boot_round-b91-cdcacm.img  85782528 B  sha256 092f784b8de788dd…
work/flash/boot-round-b91-cdcacm.img          1122304 B  sha256 3f4d004bf024a46c…（= b89 固件）
```

b90 那组标记为**已被取代**（`work/flash/INDEX.md`）。

### 这一轮的判读方法（第一次真正"不拍屏幕"）

1. 刷 b91 前，先在主机开一个抓包终端：`sudo dmesg -C && sudo dmesg -w | tee /tmp/usb-boot.log`；
2. 刷 + `fastboot boot`，等 60–90 秒；
3. 在日志里找 **`1209:316d`**，或 `New USB device found … idVendor=1209, idProduct=316d`，
   以及 `cdc_acm` / `ttyACM0`。出现 → `cat /dev/ttyACM0` 就能读内核日志（本项目的原始目标）。
4. 若仍不出现，下一份证据是**固件读 pram**（第二次启动）：`UKEUSB-STARTUP-ENTERED` 在 → 内核确实
   跑到 STARTUP；配合主机侧"有没有 `new high-speed USB device` 事件"就能分清"内核没跑到组件"还是
   "组件跑了但 gadget 没上来"。

---

## 79. b91 真机：gadget 上来了但 ep0 STALL —— 于是把相位/拒绝码/收到的 SETUP 写进 pram

### 主机侧证据（这一轮第一次有）

```
13:22:19  new full-speed USB device number 10/11
13:22:19  device descriptor read/64, error -32        ← -32 = EPIPE：我们回了 STALL，不是超时
13:22:23  #12 → 18d1:d00d Android（设备回到 Android/fastboot）
13:24:07  #15 new high-speed … 之后再无任何行
```

- **high-speed 能协商成功** ⇒ 上拉/PHY/时钟都是好的；而固件交接时 DWC3 是 `halted=1` 且没有上拉，
  所以这次的上拉只能来自组件 ⇒ **`qcom_usb_debug_init` 真的跑了，soft-connect 成功**。
- `-32` 说明主机的 **GET_DESCRIPTOR 到达了我们的 gadget**，而且我们回了 STALL（或超时后 stall），
  即事件环至少被处理过一次、ep0 分支走到了。剩下的是 ep0 那一小段的问题。

### pram 读数（b91 第二次启动，固件读上一次启动的窗口）

`pram slot 0x1000/0x1100/0x1200` 从 **全 00 变成非零**（b90 时是全 00），说明内核这次走得更远、
确实写进了这块窗口；但 `pram find "UKEUSB"` 仍是 not present，且 `pram head` 的 magic
`0xC0A03010` **不是** xnu 的 `0x46554E4B`（"FUNK"）—— 也就是 panic 头从未被写过（正常：它只在
`PESavePanicInfo` 里写），所以不能靠它判断内核日志。

顺带弄清了这块窗口的真正归属：`pexpert/arm/pe_init.c:check_for_panic_log()` 用
`pram` 节点 + `/chosen/embedded-panic-log-size` 映射它，`debug_log_init()` 把
`debug_buf_base` 设在 `gPanicBase + sizeof(struct embedded_panic_header)` —— **窗口开头就是 xnu 的
msgbuf 环形区**。所以旧驱动往 `+0x1000` 写 beacon 等于往内核日志环里写，会被日志覆盖。

### 新增：组件把状态写进窗口**尾部**（`0x37F000` / `0x37F100`）

环形区从头往后长、不回头，所以最后一页是记录唯一不会被覆盖的位置。两条记录，各 63 字节 ASCII：

```
pram slot 0x37F000: "QCOMUSB PHASE=12 ERR=0 TR=345 P=1024 LAST=7   …"
pram slot 0x37F100: "QCOMUSB REQ rt8106 rq0006 v0100 i0000 l0040 n1 …"
```

- `PHASE`：`diag_checkpoint()` 的最后一个相位号（现在**不受 diag boot-arg 门控**，每次相位都写）；
- `ERR`：`fail()` 的拒绝码（这就是"组件为什么放弃"的答案）；
- `TR` / `LAST`：`core_trace()` 调用次数与最近一次 trace 类型 ⇒ **事件到底有没有被控制器投递、被吃到**；
- `P`：`qcom_usb_debug_poll()` 的调用次数 ⇒ worker 循环是否还活着；
- `REQ`：最近一次 SETUP 包（`rt/rq/v/i/l` + 计数）。`usb/Dwc3.c` 已经把整包塞进
  `DWC3_TRACE_SETUP` 的两个 uint32（`a = rt<<24|rq<<16|wValue`，`b = wIndex<<16|wLength`），
  所以**核心一行没改**，只在板级 `core_trace()` 里解码；
- `qcom_usb_debug_init()` 第一行会写 `QCOMUSB INIT (worker reached init)` ⇒ 区分"组件根本没被调用"
  和"调用了但停在相位 N"。

新增代码用 `gcc` 与 `clang` 的 `-Wall -Wextra -Werror` 各自编译过（把 arm64 的 `dsb sy` 在探针里
省掉）。下一步：固件把这两个槽位打印出来并搜索 `QCOMUSB`（本机构建，不占 CI）。

### 79b. 读这两个槽位用的对照表（拍下来直接查）

`QCOMUSB PHASE=n` 对应组件内部的相位（`diag_checkpoint()` 的枚举顺序）：

| 相位 | 名称 |
|---|---|
| 1 | BEFORE_ACQUIRE |
| 2 | AFTER_ACQUIRE |
| 3 | COUNTER_CACHE |
| 4 | DATA_VTOP |
| 5 | TABLE_VTOP |
| 6 | MAP_GCC |
| 7 | MAP_USB |
| 8 | MAP_SMMU |
| 9 | INSPECT |
| 10 | GATES_WIDTH |
| 11 | HALT |
| 12 | DOMAIN |
| 13 | VERIFY_DOMAIN |
| 14 | VBUS |
| 15 | CORE_INIT |
| 16 | CORE_START |
| 17 | CORE_STARTED |
| 18 | FIRST_POLL |
| 19 | ENUM_DONE |

`QCOMUSB ... ERR=n` 对应拒绝码（`enum qcom_usb_debug_error`）：

| 值 | 名称 |
|---|---|
| 0 | `QCOM_USB_DEBUG_OK` |
| 1 | `QCOM_USB_DEBUG_CONTEXT_TOO_SMALL` |
| 2 | `QCOM_USB_DEBUG_BAD_COUNTER` |
| 3 | `QCOM_USB_DEBUG_CACHE_GRANULE_UNSUPPORTED` |
| 4 | `QCOM_USB_DEBUG_DMA_NOT_CONTIGUOUS` |
| 5 | `QCOM_USB_DEBUG_MAP_FAILED` |
| 6 | `QCOM_USB_DEBUG_INHERITED_STATE_REFUSED` |
| 7 | `QCOM_USB_DEBUG_UNSTABLE_POWER_STATE` |
| 8 | `QCOM_USB_DEBUG_CORE_NOT_DEVICE` |
| 9 | `QCOM_USB_DEBUG_HALT_TIMEOUT` |
| 10 | `QCOM_USB_DEBUG_DMA_REFUSED` |
| 11 | `QCOM_USB_DEBUG_CORE_INIT_FAILED` |
| 12 | `QCOM_USB_DEBUG_CORE_START_FAILED` |
| 13 | `QCOM_USB_DEBUG_CORE_POLL_FAILED` |
| 14 | `QCOM_USB_DEBUG_DEADLINE_EXPIRED` |
| 15 | `QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED` |
| 16 | `QCOM_USB_DEBUG_RESTORE_FAILED` |
| 17 | `QCOM_USB_DEBUG_VBUS_FAILED` |
| 18 | `QCOM_USB_DEBUG_DMA_WIDTH_UNSUPPORTED` |
| 19 | `QCOM_USB_DEBUG_TABLES_NOT_CONTIGUOUS` |
| 20 | `QCOM_USB_DEBUG_DOMAIN_STATE_INVALID` |
| 21 | `QCOM_USB_DEBUG_ACQUIRE_REFUSED` |
| 22 | `QCOM_USB_DEBUG_PHY_LEASE_FAILED` |

判读要点：

- **没有 `QCOMUSB INIT`** ⇒ worker 根本没调用到 `qcom_usb_debug_init()`（问题在 `bsd_init.c` 的 worker 起点）；
- **有 INIT、`PHASE` 停在 12 之前** ⇒ 卡在预检（GCC/DWC3 映射、SMMU 域名、继承状态），`ERR` 会给出具体项；
- **`PHASE>=17` 且 `TR=0`** ⇒ 控制器初始化成功但**事件一个都没投递**（事件环/DMA 地址问题）；
- **`TR>0` 但有 `REQ`** ⇒ 主机的 SETUP 到了我们手上，问题在应答路径（描述符/传输完成）；
- **`P` 不再增长** ⇒ worker 的 `qcom_usb_debug_poll()` 循环已停。

### 79c. b92 镜像已就位（这一轮第一次能"读到组件自己的说法"）

```
work/flash/vendor_boot_round-b92-pramq.img  85782528 B  sha256 f4e2f00efd4ce007…
    ← CI run 42451159858 全绿的内核 f13ef454bd5c3073…（136,577,024 B），组件带 pram 上报
work/flash/boot-round-b92-pramq.img          1122304 B  sha256 f950c7a4d8855751…
    ← 新固件：多读 pram slot 0x37F000 / 0x37F100，并搜 "QCOMUSB"
```

（b92 的 boot.img 与 b89 不再逐字节相同，正是固件改动生效的证明；`QCOMUSB` 字面量在
boot.img 里搜不到是正常的 —— UEFI FV 是压缩后塞进 boot 分区的。）

刷 + 启动两次（第一次让内核写，第二次让固件读）：

```sh
adb reboot bootloader
fastboot flash vendor_boot_a work/flash/vendor_boot_round-b92-pramq.img
fastboot boot work/flash/boot-round-b92-pramq.img          # 第一次：内核跑，写 pram
# 等它起来/挂掉，然后再来一次，让固件打印上一次的 pram：
fastboot boot work/flash/boot-round-b92-pramq.img          # 第二次：读 pram
```

照片只需要这两行（在 `pram first text:` 之后新增的两行）：

```
pram slot 0x37F000: "QCOMUSB PHASE=… ERR=… TR=… P=… LAST=…"
pram slot 0x37F100: "QCOMUSB REQ rt… rq… v… i… l… n…"   或 "QCOMUSB INIT (worker reached init)"
```

---

## 80. pram 这条通道不可靠（记录写得进去、留不住）——改用**panic 屏**当诊断口

b92 的读数（多帧）：

```
pram slot 0x1000/0x1100/0x1200/0x37F000/0x37F100: <no printable text>
pram find "UKEUSB" / "UKELASTLOG" / "QCOMUSB": not present in 3584 KiB
pram first text: <none: DRAM did not survive, or the kernel never wrote>
```

但另一帧里明确出现过 `pram slot 0x37F100: "P=0"` —— 也就是**我们的记录确实写过、后来自不见了**。
结合这块窗口的身份（`make-uke-adt.py` 里注明它对齐 live DT 的 **`llcc_lpi_region@ff800000`**，
即 LLCC 低功耗 SRAM 区，跨复位保留是有条件的），结论是：

- **不能把 pram 当可靠通道**：写入可能落在，但跨复位保留是碰运气；
- 这也一次性解释了历史现象：`UKEUSB-*` beacon 从来读不到、`UKELASTLOG` 从来读不到、
  `pram first text` 永远是 none —— 不是我们的代码没写，是这块区域留不住。

### 换成 panic 屏（这块板子唯一确认可见的控制台）

`board.toml` 里 `panic_screen = true`，之前的 SError panic 就是靠它读出来的。于是给组件加
boot-arg `qcom-usb-panic=N`：

| N | 行为 |
|---|---|
| 1 | `fail()` 时 panic，打印 `err=核心结果=phase=tr=last=` |
| **2** | **每次控制请求到达时 panic**，打印 `rt/rq/v/i/l` + phase（本轮用这个）|
| 3 | 第 64 次 poll 时 panic，打印 phase/err/tr/last/trace_a/trace_b |

本轮参数选 `qcom-usb-panic=2`：主机那次 `GET_DESCRIPTOR` 一到，屏上就会直接出现
`QCOMUSB request rt=… rq=… v=… i=… l=… phase=…`。这一行同时回答三件事：

1. 请求到底有没有到达我们的 gadget（到了就一定有这行）；
2. 到达的字节是不是**正常的** `rt=8000 rq=0006 v=0100 i=0000 l=0040`（若不是 ⇒ SETUP 读到垃圾 ⇒
   SMMU/DMA 翻译问题 ⇒ 才会走 `stall()` ⇒ 正好对应主机的 `-32`）；
3. 卡在哪个 phase（`phase<12` 就是预检被拒，但那种情况会走 N=1 的 fail 分支）。

顺带把三个全 0 的旧开关（`uke_usb_sync/hook/panic`）从命令行删掉（`uke_usb=0` 已足够让旧驱动惰性），
参数总长 949 + 18 = 967 B，仍在 1000 B 上限内。

固件侧同时升级：`pram slot 0x37F000/0x37F100` 现在除了文本还打 **4 行 × 16 字节的 hex**，
这样"记录在但前缀被覆盖"也能看出来（本机已编译通过，等下一组镜像）。

### 80b. b92b：只想先确认"pram 到底留不留得住"

```
work/flash/boot-round-b92b-hexdump.img        1122304 B  sha256 …
work/flash/vendor_boot_round-b92b-hexdump.img 85782528 B  sha256 …
    ← 内核仍是 b92（带 pram 上报），固件升级为：0x37F000/0x37F100 除文本外再打 4×16 字节 hex
```

这一组和下一组的分工：b92b 用来**判定 pram 保留性**（hex 里能不能看到 `51 43 4F 4D 55 53 42`＝"QCOMUSB"）；
真正能回答"ep0 为什么 stall"的是 b93（内核带 `qcom-usb-panic=2`，一收到控制请求就在屏上 panic 打印请求字节）。

### 80c. 被 `panic` 是宏这件事绊了一次（run `42472454743`）

```
kits/uke/qcom-usb-debug/kernel/QcomXnuUsbDebug.c:141:13: error: expected identifier or '('
kits/uke/qcom-usb-debug/kernel/QcomXnuUsbDebug.c:141:13: error: expected ')'
```

原因：xnu 里 **`panic` 是宏**（`osfmk/kern/panic_call.h:46`），
`#define panic(ex, ...) ({ __asm__("" ::: "memory"); (panic)(ex " @%s:%d", …, __FILE_NAME__, __LINE__); })`。
我加的 `extern void panic(const char *format, ...);` 也被这个宏展开，自然变成语法垃圾。
而在**真实**的头文件顺序里，函数先声明（`panic_call.h:35`）、宏随后才定义，所以**调用**是没问题的 ——
只有"自己再声明一次"会炸。删掉那行 extern、直接调用即可（本地用 gcc + clang 把宏展开过的三种调用各编译过一遍）。

顺带记住：panic 的消息会自动带上 ` @文件:行` 后缀，这反而对判读有利。

### 80d. 再加一个自造障碍：导入审计不认识 `_panic`

run `42473522667`：这次**编译全过**（`preflight: 0 error(s) across 5 units`），死在我自己写的导入审计上：

```
RuntimeError: unexpected XNU imports: ['_panic']
```

`package/qcom-usb-debug/scripts/build.py` 的白名单里没有 `_panic`，加上即可（`panic` 是真导出符号，
内核里必然存在）。这条审计本意是防止意外引入外部符号，这次它按设计工作了 —— 只是新增的**有意的**依赖
也要同步登记。

### 80e. 确认 panic 屏这条路是成立的（b93 的前提）

`work/push/UniXNU/scripts/xnu-panic-screen.c` 是一段**freestanding 渲染器**，由 payload 构建时
（`patch-xnu-entry-probe.py --panic-screen --no-reset`）打进内核副本，把 `panic()` 的消息**直接画到
framebuffer** 然后停机。它的迷你 printf 支持 `%s/%x/%X/%u/%d/%p`（含 0 标志、宽度、精度、
`l/ll/z` 长度修饰），所以 `panic("QCOMUSB request rt=%x rq=%x … phase=%u")` 会原样显示在屏上。

这条通道不依赖 pram（已判死）、也不依赖 USB 枚举，是目前唯一可靠的内核 → 人 的输出。
另外核对过 SMMU 参数：`QUD_DMA_IOVA = 0x90000000` 与 live DT 的
`qcom,iommu-dma-addr-pool = <0x90000000 0x60000000>` 起点一致，`QUD_STREAM_ID = 0x40`
与 `dwc3@a600000` 的 `iommus = <0x129 0x40 0x00>` 一致 —— 若 b93 显示 SETUP 是垃圾字节，
问题就不在这些常量上，而在域安装/一致性细节（`soc/QcomUsbDmaDomain.c` 的 CB 与 SMR/S2CR 编程）。

### 80f. b93 就位（收请求即 panic，屏上直接给答案）

```
work/flash/vendor_boot_round-b93-panicreq.img  85782528 B  sha256 3f0130b9374a7f92…
    ← CI run 42474768766 全绿，内核 34bad0851937f557…（内含 "QCOMUSB request rt"/"refused err"/"poll64" 三条）
work/flash/boot-round-b93-panicreq.img          1122304 B  sha256 6bab1734de52bcbb…（= b92b 固件）
```

内核命令行关键两项：`uke_usb=0`（旧探针驱动惰性）、`qcom-usb-panic=2`、`qcom-usb-debug=1`。

判读表（同 §80）：

| 屏上 | 结论 |
|---|---|
| `QCOMUSB request rt8000 rq0006 v0100 i0000 l0040` | 请求正常到达 ⇒ 问题在应答/传输完成 |
| 字段乱码/全 0 | SETUP 读到垃圾 ⇒ 域安装/一致性问题（常量已核对无误，见 §80e）|
| 没有这行、但主机仍看到 high-speed | 事件没投递 ⇒ 事件环 |
| `QCOMUSB refused err=…` | 预检被拒（本轮 N=2 不该出现，若出现说明 init 提前失败）|

---

## 81. b93 真机：第一个 GET_DESCRIPTOR **成功**了，死在 SET_ADDRESS —— 但 panic 没打出来

```
[5422.807997] usb 3-1.2: new high-speed USB device number 42 using xhci_hcd
[5422.808071] usb 3-1.2: Device not responding to setup address.
[5423.014333] usb 3-1.2: Device not responding to setup address.
[5423.223128] usb 3-1.2: device not accepting address 42, error -71
[5423.223549] usb 3-1-port2: unable to enumerate USB device
```

两点新信息：

1. **失败点往后移了**：之前是 `device descriptor read/64, error -32`（GET_DESCRIPTOR 被 stall），
   现在是 `Device not responding to setup address`（SET_ADDRESS 无应答）—— 也就是**第一个 8 字节
   设备描述符是被成功读走的**（否则 xhci 会先打 descriptor read 错误）。
2. **`qcom-usb-panic=2` 没有生效**：如果它生效，第一个控制请求就会 panic，那样连描述符都读不到。
   所以要么 boot-arg 没被解析到，要么 panic 路径/渲染没走到。

### 于是做了两件事

**(a) panic 模式不再依赖 boot-arg**：`QCOM_USB_PANIC_DEFAULT` 由板级编译记录注入
（`BOARD_DEFINES` 里加 `-DQCOM_USB_PANIC_DEFAULT=2`），并把 `mode=%u` 打进 panic 文本，
这样"参数没生效"和"panic 没渲染"在下一屏就能区分。

**(b) 一个 6 分钟、不用 CI 的对照实验（b94，组件整体关闭）**：

```
work/flash/vendor_boot_round-b94-nocomp.img  85782528 B  sha256 8d57702167ede766…（内核同 b93，但 qcom-usb-debug=0）
work/flash/boot-round-b94-nocomp.img         1122304 B  sha256 6bab1734de52bcbb…（= b93 固件）
```

`qcom-usb-debug=0` 之后内核里没有任何代码碰 DWC3。此时若主机**仍然**看到 high-speed 设备、
甚至仍然能读走描述符，那它就是**固件自己的 USB gadget**（UEFI 的 UsbFn/MSD 栈在交接时没有断连），
我们之前所有"组件已上拉/已应答"的推断都要推翻；若什么都看不到，则之前的设备确实是组件的，
问题就锁定在 SET_ADDRESS 的状态阶段处理。

### 81b. 对照实验结果 + 找到真正的时序问题

**实验（b94，`qcom-usb-debug=0`）**：主机看到的形态变成了"全速 + `device descriptor read/64, error -32`"，
而 b93（组件开）是"高速 + `Device not responding to setup address`"。也就是说**设备的形态随组件开关而变**
⇒ 之前那个高速设备确实是组件连上去的，不是固件残留的 gadget。方向没跑偏。

**真正的可疑点：轮询节奏太慢**。读代码就明白了：

```c
/* worker 循环（bsd_init.c 补丁） */
qcom_usb_debug_poll();
qcom_usb_worker_wait();          /* clock_interval_to_deadline(5, NSEC_PER_MSEC) —— 每轮睡 5 ms */

/* poll_owned()（组件） */
ret = dwc3_poll(native.core, 1); /* 每次只取 1 个事件 */
```

一次控制传输 = SETUP 事件 + 数据/状态事件，而主机给 **SET_ADDRESS 的窗口只有几十毫秒**。
按"1 个事件 / 5 ms"的节奏，SET_ADDRESS 极易超窗 —— 这正是 `Device not responding to setup address`
（而不是描述符阶段失败）的原因；也解释了为什么第一个 `GET_DESCRIPTOR(8)` 有时成功、有时 `-110` 超时：
完全取决于那几次轮询是不是刚好落在窗口里。

**修法（两处，都是"把排空做快"）**：

1. 组件：`dwc3_poll(native.core, 1)` → `dwc3_poll(native.core, 128)`（核心上限 256），
   一次调用就能把一串事件排空；
2. worker 补丁：poll 之后**只要还有事件就继续 poll**（burst 上限 64 轮，防止话多的端点饿死线程），
   空闲等待 5 ms → **1 ms**。

两份补丁副本已同步（`scripts/check-patch-copies.py` 通过），`git apply --check` 在干净 12377 树上通过。
另外 `board.toml` 把 `qcom-usb-debug` 改回 `1`（对照实验已读完），参数 967 B。
