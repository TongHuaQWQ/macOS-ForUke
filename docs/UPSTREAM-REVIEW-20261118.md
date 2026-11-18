# 审视：我们哪些东西是自己手搓的，能不能直接用上游/参考实现

结论先行：**我们唯一真正有害的手搓是 `drivers/ArmCpu`**（它给启动路径加了读 GICR SGI 帧的诊断，
就是 b170 之后每一轮风暴的成因）。其余本地覆盖要么是上游没有的功能，要么是这台板子专有的东西。

对照源（本地副本，`work/` 下）：
`ref-sheng`（UniXNU-sheng，**跑 macOS**）、`ref-allenwdk`（Allenwdk/UniXNU-uke，**这台机器跑 iPadOS**）、
`ref-armcpu`（Allenwdk/ArmCpu@8721288）、核心仓库 `work/push/UniXNU`（上游 `UniXNU/ArmCpu`）。

## 1. `feeds.toml` 里的五个本地覆盖

| 组件 | 本地路径 | 上游有没有 | 判定 |
|---|---|---|---|
| **ArmCpu** | `drivers/ArmCpu` | ✅ 核心 `drivers/ArmCpu`（895 行），**已含** `redistributor-stride` 解析和 `/arm-io` 基址换算（232-265 行），也读 `sheng-smp`/`sheng-smp-gic` | **丢掉本地覆盖**。我们的分叉相对上游 +99/−14 行，删掉的正是 cluster/cpufreq/idle/edge 这些能用的功能，加的是诊断 kprintf 和（已删的）SGI 回读。sheng 根本不带 ArmCpu，用核心的 |
| ArmDTPlatform | `drivers/ArmDTPlatform` | ❌ 核心版 210 行，**没有** `handlePlatformError` | **保留**。它给 `IOPlatformIO` 装了平台错误处理器，没有它每个 SError 都会 panic 成 "Unhandled implementation specific error"（上游缺这个功能）|
| ShengUfs | `drivers/NativeUfs` | ❌ 板级驱动 | 保留（这台机器的 UFS/ICE/MCQ 细节）|
| ShengNovatekTouch | `drivers/NativeNovatekTouch` | ❌ 板级驱动 | 保留 |
| XnuPrelink（kit） | `kits/XnuPrelink` | — | 保留 |

**关于 feeds 的解析顺序**（`feeds.toml:56-59` 的注释）：`local` 在第 4 步解析、`git` 在第 7 步，所以
`local` 总是赢。要让核心的 ArmCpu 生效，**把 ArmCpu 的 `local =` 和 `git =` 两项一起删掉**，
不要只删 git（那样还是用本地副本）。

## 2. ArmCpu 三方对比（关键证据）

| 版本 | 行数 | 启动路径上有 SGI 帧回读 | 读哪些 boot-arg |
|---|---|---|---|
| 核心上游 `UniXNU/ArmCpu` | 895 | **没有**（只在 `armcpu_gic_diag()` 显式诊断里） | `sheng-smp`、`sheng-smp-gic`、`armcpu-*` |
| Allenwdk `@8721288` | 1212 | **没有** | `smp`、`smp-gic`、`armcpu-*` |
| 我们 `TongHuaQWQ/ArmCpu@6288238` | 981 | **三处**（`prepareGic`/`enableGic`/`diag`） | 同核心 |

b262（`abe0af7`）已经把三处回读全部去掉；下一步是把整个本地覆盖删掉，改用核心版本。

## 3. kernel_args 对照：我们 52 条/982 B vs sheng(macOS) 47 条/871 B

**sheng 有我们没有**（这些是 macOS 侧子系统开关，但**不能盲抄**——它们要有对应的组件才有意义）：

```
native-source-storage=1  sheng-buttons=1   armcpu-self-ipi=1  sheng-adsp=2
sheng-audio=12  sheng-wifi=5  sheng-backlight=1  sheng-cpufreq=4  sheng-fb-bgra=1
sheng-dpu-scanout=2  sheng-no-lc=1  sheng-vsync=120  sheng-vsync-fixed=1
sheng-gpu-dcvs=1  sheng-disable-watchdogd=1
```

**我们有 sheng 没有**（多数是我们自己的诊断/实验开关，可回退）：

```
uke-gic=0  uke-aes-stub=1  -enable_kprintf_spam  -uketextconsole  offline-legacy-noconsole=1
uke-dpu-scanout=3  usb=1  nointr_consio=1  uke-platform-error-panic=1
native-ufs-allow-unverified=1  uke_usb=0  qcom-usb-debug=1  qcom-usb-qr=1  qcom-usb-panic=0
sheng-pmgr=2  amfi_get_out_of_my_way=1  ml-timeout-lock-panic-scale=40  boot-progress=2
-apfs_shared_datavolume  offline-apfs-load=1
```

值得单独提的两条：
- `armcpu-self-ipi=1`：sheng 有、我们没有。它和 `armcpu-spi-ipi=1` 一起决定 IPI 怎么走（Gunyah 下 SGI 不投递，
  所以用 SPI）。**8 核真正起来后，如果 IPI 不通，第一个要试的就是它。**
- `amfi_get_out_of_my_way=1`：我们用它在 nonce slot 读取全失败后继续启动；sheng 的 macOS 参数集里**没有**
  这一条，说明 macOS 侧不走这条绕行（走的是正常的 AMFI 启动），这是个值得对齐的信号。

## 4. 下一步动作（按顺序，每轮一个变量）

1. **b263（已推）**：dts 加 8 个 `unixnu,cluster-type`。验收 `ArmCpu: N timer interrupts` N>0。
2. **b264**：删掉 `feeds.toml` 里 ArmCpu 的 `local`+`git` 两行 → 用核心上游 ArmCpu。验收：行为不变
   （风暴仍不出现、N 仍 >0），但少了一个分叉。
3. **b265**：对齐 sheng 的参数集里**我们已有组件**的那几条（`armcpu-self-ipi=1` 优先），验收 IPI 计数。
4. 之后才是 USB / 显示 / Wi-Fi 这些增量。

---

## 5. 官方 macOS 参考：`Jelly10086/macOS-ForSheng`（本地 `work/ref-macos-sheng/`）

`unixnu.com/zh/devices/sheng-macos` 指向的不是 `UniXNU-sheng`（那是 iPadOS 的），而是这个：

| 项 | 内容 |
|---|---|
| 目标 | macOS **26.5 / 25F71**，配套 **XNU 12377.121.6 ARM64E** |
| 形态 | **UEFI + DarwinBoot + 自建 EL1 内核**（和我们一样），硬件适配参考 UniXNU |
| 已达成（实机）| 原版 **launchd** 与恢复服务已执行；UFS 读取 + **BaseSystem / ISC 挂载**；图形 Recovery 语言窗口已显示 |
| 未达成 | WindowServer 在 HID/显示初始化时反复退出；Adreno 加速未实现 |
| 文档 | `docs/{status,bringup,debugging,boot-progress,panic-qr,native-smp,native-early-exception,macos26-security,routes}.md` |

### 5.1 他们的日志体系（比我们的 userdata 尾环成熟一代）——**这是我们应该抄的**

`docs/debugging.md` 列了六个来源，各管一段，互不替代：

| 来源 | 保存内容 | 限制 |
|---|---|---|
| 滚动控制台 | 当前字符输出 | 会滚掉 |
| 底部进度 | 阶段/目标服务/停留时间/提交状态 | 不是百分比 |
| **首次 panic 面板** | 最多 **4095 字节**首次 panic 文本 | 受屏幕大小限制 |
| **panic 二维码** | 内核 UUID、调用地址、钩子寄存器、丢失计数、有界原因 | 仅摘要 |
| **RAM 保留日志** | 128 字节头 + 16 KiB 前缀 + 64 KiB 环形尾部 | writer 忙时丢字 |
| macoslog 文件 | 周期提交的 RAM 快照 | 双槽 + CRC + 保护域校验 |

**关键设计（`docs/panic-qr.md`）**：首次 panic 时把不超过 4095 字节的文本 + 一段结构化摘要**画成二维码**放在独立 panic 区，手机离线扫。编码与绘制路径**不申请内存、不等锁、不写盘**；重复/递归 panic 不覆盖首次记录。

→ 这正好解决我们今晚最痛的事：`brk` 断言的文本只能靠拍模糊照片猜（我还因此误读出一句内核里根本不存在的 `zone_allocate: failure`）。

### 5.2 我们应该照抄的三件事（按顺序）

1. **panic 文本 → 二维码**。我们已经有 QR 编码器（`package/qcom-usb-debug/kernel/QcomUsbQr.h`）和 panic 钩子（`patches/xnu/zzzzzzzzzzz-uke-panic-flush.patch`，目前只捕获 512 字节并经 `uke_panic_flush()` 写 pram）。把上限提到 4095 并在 panic 区画码，就能彻底摆脱拍屏。
2. **pram 尾部槽位**：固件在下次启动会打印 `pram slot 0x37F1xx`（可打印文本）并搜索 `UKEUSB`/`UKELASTLOG`/`QCOMUSB` 魔数。把 panic 文本同时写到那个尾部区，等于**零成本跨启动通道**（需先确认与 `UKE_RAMOOPS_OFFSET=0x200000` 及 log arena 不冲突）。
3. **磁盘日志双槽 + CRC**：我们的 32 MiB 窗口是单环；他们的 `macoslog` 是双槽提交 + CRC + 保护域校验，崩溃时能保住"最后一次成功提交"。等 b266 的 DMA 修好后，按这个格式改造我们的记录头（`UKELLOG1` 已在用 CRC，缺的是双槽）。

### 5.3 SMP：我们的配置与他们一致（`docs/native-smp.md`）

> `sheng-smp=1` 启动次级核心，`armcpu-spi-ipi=1` 使用分配给各核的 SPI 发送核间中断。
> 构建 40949503078 八核运行到达 `load_init_program`，七个副核均完成初始化，PSCI 返回值与失败计数均为 0，八核均收到定时器中断。

我们正是 `sheng-smp=1` + `armcpu-spi-ipi=1` + dts 里 8 个 `unixnu,cluster-type`，与他们逐条对应。**b263 之后 `ArmCpu: N timer interrupts` 是否为 0，是唯一还没拿到的证据**——下次拍屏请往上翻找这一行。
