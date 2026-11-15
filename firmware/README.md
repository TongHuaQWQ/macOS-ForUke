# uke 固件（uefi-uke）的补丁集

这里放的是**固件层**的全部自有改动，以补丁形式跟设备仓库一起备份。

## 为什么放在这里

固件是另一层东西，不要和本仓库混为一谈：

| | **uefi-uke**（本目录所描述的） | **本仓库（设备仓库）** |
|---|---|---|
| 是什么 | UEFI 固件实现 = `Project-Silicium/Mu-Silicium` 的 fork | UniXNU 的**设备描述**（board.toml / configs / package / drivers） |
| 语言、体积 | Assembly + C（EDK2）/ 上游 125 MB | Python（构建描述）/ 2 MB |
| 产出 | **`boot.img`** → 刷 `boot_a` | 参与产出 **`vendor_boot`** → 刷 `vendor_boot_a` |
| 启动时的位置 | **最先跑**：DXE、菜单、GOP、内存图 → 交给 `UniXNU.efi` | 不参与运行，只在构建期描述硬件 |

上游那份 125 MB 的仓库 fork 起来太重，而我们真正的自有改动只有 27 个文件。
所以把改动固化成补丁放在这里：**不需要新仓库，跟着本仓库走，任何人 `git apply` 就能重建。**

## 内容

| 文件 | 是什么 | 规模 |
|---|---|---|
| `uke-firmware.patch` | 相对上游 Mu-Silicium 的**已提交**改动 | 23 文件 / 2912 行 / 105 KB |
| `uke-firmware-worktree-wip.patch` | 在上一份之上的**未提交**改动（`UkeUsbLogDxe` 那一大批） | 7 文件 / 1163 行 / 45 KB |
| `files/` | 补丁覆盖不到的**未跟踪文件 + 二进制**（见下） | 6 文件 / 2.0 MB |
| `2026-11-12-panic-header-offsets.patch` | panic 头按 XNU 的 packed 布局解析（0x14/0x18），b147 用 | 1 文件 / 3 KB |
| `2026-11-12-uke-log-record.patch` | 上一轮内核日志：`UKELOG:` 记录读出来打进固件日志，b150 用 | 2 文件 / 12 KB |

`files/` 四类，缺一不可：

```
Binaries/uke/RawFiles/Panel_O82_42_02_0b_dualdsi_dsc_vid.xml   ← 面板 XML，只存在于 Binaries
                                                                  子模块的未跟踪目录里，全系统仅此一份
Platforms/Xiaomi/ukePkg/Drivers/UkeProgressDxe/{.c,.inf}       ← 未跟踪的驱动
Platforms/Xiaomi/ukePkg/Include/UkePersist.h                   ← 未跟踪的头文件
Platforms/Xiaomi/ukePkg/Include/UkePramLog.h                   ← 未跟踪的头文件（b150：pram 日志记录的格式与窗口几何，唯一一份）
Resources/DTBs/uke.dtb                                         ← 1.9 MB 二进制；uke.toml 要 append_dtb，
                                                                  缺它 build_uefi.py 会 exit 1（已实测）
```

## 重建步骤

```bash
# 1. 拿一份上游，并回到我们合并的那个基线
git clone --recursive https://github.com/Project-Silicium/Mu-Silicium.git uefi-uke
cd uefi-uke
git checkout 20b82927                      # ← 本补丁集的 base

# 2. 打补丁（顺序不能反）
git apply /path/to/macOS-ForUke/firmware/uke-firmware.patch
git apply /path/to/macOS-ForUke/firmware/uke-firmware-worktree-wip.patch

# 3. 铺开补丁覆盖不到的文件
cp -r /path/to/macOS-ForUke/firmware/files/* .

# 4. 子模块：用我们自己的指针，**不要**跟上游（见下）
git submodule update --init --recursive

# 5. 构建（本机约 10 秒）
./.venv-edk2/bin/python3 build_uefi.py -d uke -r DEBUG ; echo $?
# 期望 exit=0 且 PROGRESS - Success，产出 Mu-uke.img ≈ 2.5 MB
```

**验证补丁集本身**（我在 `workspace/uefi-uke` 里做过，结论：27/27 文件逐字节一致）：

```bash
git apply --check ../macOS-ForUke/firmware/uke-firmware.patch          # 干净上游上应通过
# 打完两份补丁并铺开 files/ 后，逐个 cmp 与真实树比对
```

## 子模块指针：故意与上游不同

上游 `feat/macos`/`main` 那批 gitlink 里有**三个指向远端已经不存在的提交**
（`Adreno*` 与我们无关，但同一批里 `Adreno740 765738cf`、`AdrenoMetal 3efd3ec3`、
`ArmCpu 4dde97f6` 都返回 `No commit found`）。所以本补丁集不含 gitlink 改动，重建时用我们这套：

| 子模块 | 指针 |
|---|---|
| `Binaries` | `a1501e0c2e642d6ac0d8f52c85af2a67b75a7799` |
| `Common/Mu` | `b8d46c71610318e36a661819e1e3c0b8bdda413a` |
| `Mu_Basecore` | `cfb93724a06ff86ff5ba971413368a7809ee42a5` |
| `Silicium-ACPI` | `2d465ad77c25de908f0ec7bdfcb57f865b61233c` |

## 这 27 个文件里有什么

- `Platforms/Xiaomi/ukePkg/**` —— 这台设备的整个固件包：`uke.dsc`/`.fdf`/`.dec`、内存图
  （含 5 GiB 高区、GMU pool、GFX 完整 MMIO 区）、`APRIORI.inc`/`DXE.inc` 的模块取舍、
  面板 RAW 声明、`UkeUsbLogDxe`（USB 大容量存储日志通道）、`UkeBootMemoryDxe`（含
  从 `iOSNJBrepo/macOS-uke` 借来的"live DT 未描述的高区页 fence"）、`UkeProgressDxe`
- `Silicon/Silicium/SiliciumPkg/Sec/**` + `SiliciumPkg.dec` —— SEC 阶段入口
- `BootShim/BootShim.S`、`Resources/Configs/uke.toml`、`Resources/MuPatches/Gic-Spurious.patch`

## 对应产物

打完这些补丁构建出的固件，打成了 `work/flash/boot-round-b107-newfw.img`
（sha256 `f02e1a97856783dd…`），与 `vendor_boot_round-b107-cdres.img` 配对使用。

## 来源与许可

绝大部分是本项目自己的改动。其中 `UkeBootMemoryDxe.c` 的 fence 逻辑取自
`iOSNJBrepo/macOS-uke` 的 `patches/uefi/worktree.diff`（该仓库 UEFI 衍生的源码标
`BSD-2-Clause-Patent`），提交信息 `fd4ef909` 里写明了来源。上游 Mu-Silicium 部分沿用 EDK2
的 BSD-2-Clause-Patent（见上游 `LICENSE`）。

## 2026-11-15：b223 的只读 PHY 转储（现场说明，未并进上面两份补丁）

> 上面两份补丁是 **11-12 的快照**，之后的 b208–b223 改动（`UkeUsbLogDxe` 里那一批 `UKE-*`
> 探针、`UkeBootMemoryDxe` 的 fence、`uke.dec`/`uke.dsc` 的 PCD 行）都还只在
> `workspace/uefi-uke` 的工作树里，没有回写。这是已知欠账，别当成"固件没改过"。

b223 在 `UkeUsbLogDxe` 里加了一段只读转储（新 PCD `PcdUkeUsbPhyDump`，`uke.dec` 0x21、
`uke.dsc` =1、`.inf` 里声明）。它一个问题都不猜，只打印四组事实：

| 日志前缀 | 读的是什么 | 出处 |
|---|---|---|
| `UKE-PHYD:` | eUSB2 PHY `0x088e3000..+0x154` 全窗口（85 字），10 行 × 8 + 尾行 5 | 节点 reg `live.dts:16039`；期望值 `work/re/data/eusb2-phy-init-19mhz.json` |
| `UKE-QSCFG:` | QSCRATCH `GENERAL_CFG` = `0xa6f8808` 及三个命名位 | `drivers/usb/dwc3/dwc3-qcom.c:33-36`（`PIPE_UTMI_CLK_SEL` bit0 / `PIPE3_PHYSTATUS_SW` bit3 / `PIPE_UTMI_CLK_DIS` bit8） |
| `UKE-PIPE:` | `gcc_usb3_prim_phy_pipe_clk`(0x39068) 的 enable/halt，以及 pipe/aux/两位 mock_utmi 源字 | `work/re/data/gcc-cliffs-register-map.json` |
| `UKE-QMP:` | `0x088e8000` 上 `qcom,qmp-phy-reg-offset` 列出的 13 个偏移（0xffff 跳过的那个照实写 absent），按索引打印、不给名字 | `live.dts:16069`；索引→名字的映射**未定**，不下结论 |

判读用 `python3 work/analysis/check-eusb2-phy-dump.py <日志文件>`：它把 `UKE-PHYD` 的字与厂商
29 条无条件 `rmw` 逐条比对，并单独报 EUD/`GENERAL_CFG`/pipe clock/QMP 四个结论。
注意 EUD：`eud_enable` bit0 = 1 时厂商 PHY 驱动**一个 PHY 寄存器都不写**，那种情况下表比对无效。

构建（本机，venv 在 `work/tmp/uefi-venv`）：

```bash
cd workspace/uefi-uke
work/tmp/uefi-venv/bin/python build_uefi.py -d uke -r DEBUG   # 增量约 7 s，产出 Mu-uke.img
```

依赖没装时先 `python3 -m venv work/tmp/uefi-venv && work/tmp/uefi-venv/bin/pip install -r workspace/uefi-uke/pip-requirements.txt`
（本机没有 `gcc-aarch64-linux-gnu`，`CLANGPDB` 工具链只用 clang+lld，够用）。
