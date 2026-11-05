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
