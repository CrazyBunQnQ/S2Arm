# 共享出包阶段（J07 / #183 Phase 1）

这里放**与目标平台无关**的出包阶段：输入是游戏原始数据 / 共享宿主数据，输出是产物根里与目标无关的相对路径。
目标插件（`pack/targets/**`、`pack/stage_*.sh` 的入口、Android `android/tools/make_pack.py`）只保留
「目标相关的阶段」：改写补丁、直连库、界面布局、目标屏幕尺寸、打包/签名。

本片抽六个阶段（第一批四个 + 第二批两个）：

| 共享阶段 | 入口 | 产物（相对路径） | 目标名进路径？ |
|---|---|---|---|
| 选包 / 解包 | `pack/stages/unpack.sh` | 调用方给的解包目录：`exefs/ romfs/ ctl/ aoc/ unpack.tsv direct-profile-input.json .stage_unpack` | 否 |
| 系统数据 | `pack/stages/sysdata.sh` | `systemdata/`（字体 / 时区 / 所有款共用的系统数据）、`tz.log` | 否 |
| RomFS 索引 | `pack/stages/romfsidx.sh` | `romfs.s2aidx`、`romfs_index.log`、`.stage_romfsidx` | 否 |
| 元数据（第二批） | `pack/stages/nacp.sh` | `ctl/control.nacp`、`nacp.log`（无阶段键） | 否 |
| 追加内容（第二批） | `pack/stages/aoc.sh` | `aoc/`、`aoc.tsv`、`aoc.log`、`.stage_aoc` | 否 |

`pack/stage_sysdata.sh`、`pack/stage_romfsidx.sh`、`pack/stage_nacp.sh`、`pack/stage_aoc.sh`
的**对外命令行不变**，文件内容只剩一层转发：旧调用方（`pack/refresh.sh`、`pack/stage_keys.sh`、`tools/app_only.sh`、
`pack/ipa.sh`、`pack/seed_root.sh`、`tools/auto_formal.sh`）不需要改。

## 契约

六个入口只认参数 / 环境变量，不读「当前是哪个目标」：

| 阶段 | 输入 | 产物 | 退出码 |
|---|---|---|---|
| `unpack.sh inspect --src <目录>` | `--src`、可选 `--ids/--nx-unpack/--keys`；环境 `S2A_GAME_SRC`（= `--src`）、`S2A_NX_UNPACK`、`S2A_KEYS`、`S2A_SRC_IDS` | stdout 每行「路径 TAB 字节 TAB mtime TAB 程序号」（不写产物根） | 0 列成功；1 工具 / 目录失败 |
| `unpack.sh extract --src <目录> --out <目录>` | 同上 + `--title-id`（缺省按内容推出唯一本体）、`--log`、`--force` | `<out>/.stage_unpack` 记键；先写 `<out>.tmp.<pid>`，成功才原子改名；失败清 tmp、`<out>` 不动 | 0 成功 / 命中复用；非 0 没写成 |
| `sysdata.sh <款>` | 现行产物根、`S2A_FONTS`、`S2A_SYSDATA`（需求表仅作统计，不参与出包判断） | 新版本产物根里的 `systemdata/`；走 `root_begin` / `root_commit`（`ROOT_REPLAY='systemdata tz.log'`） | 0 成功 / 已齐；1 红（缺数据 / 生成失败），现行不动 |
| `romfsidx.sh <款>` | 现行产物根 `romfs/`、本树 `HEAD` 的索引写法对象号 | 新版本 `romfs.s2aidx` + `.stage_romfsidx`；走 `root_begin` / `root_commit` | 0 成功 / 已做；1 红，现行不动 |
| `nacp.sh <款> <xci / nsp…>` | 现行产物根 `exefs/main` 的构建号、原始包、`S2A_KEYS`、`S2A_NX_UNPACK` | 新版本 `ctl/control.nacp` + `nacp.log`；走 `root_begin` / `root_commit`（无 `ROOT_REPLAY`） | 0 成功 / 已有 `ctl/control.nacp` 跳过；1 红（缺工具 / 抽不出），现行不动 |
| `aoc.sh <款>` | 现行产物根 `exefs/main` / `unpack.tsv`、`S2A_GAME_SRC` 内容清单（`pack/srcs.sh`）、`S2A_AOC_CACHE` | 新版本 `aoc/` + `aoc.tsv` + `aoc.log` + `.stage_aoc`；走 `root_begin` / `root_commit`（`ROOT_REPLAY='aoc aoc.tsv aoc.log .stage_aoc'`） | 0 成功 / 已做 / 本机没原始包（黄、跳过）；1 红（没现行 / 没工具），现行不动 |

`sysdata / romfsidx / nacp / aoc` 的事务语义完全沿用 `pack/roots.sh`：本阶段**没有**改并发锁、提交、重放规则。
`unpack` 不在版本化产物根里（它是「原始输入」这一步），用 `<out>.tmp → <out>` 原子改名；已存在且键不匹配时
必须显式 `--force`，避免误删别人的解包目录。
`aoc` 的「本机没原始包」和「解不出」都退 0（黄、现行不动）：DLC 是附加内容，拦住会让整款出不了包（原实现如此，本片不改）。
`nacp` 没有阶段键（原实现如此）：已有 `ctl/control.nacp` 就跳过，标签是 `nacp-<main 构建号>`。

## 缓存键公式

### 选包 / 解包（`.stage_unpack`）

```
selected = { 程序号 == --title-id 的包 } ∪ { 程序号 = "-" 的包 }   # 后者是读不出控制分区的更新 / DLC
K_unpack = sha256(
             逐行 "绝对路径 TAB 字节数 TAB mtime TAB 程序号"
           + sha256(nx_unpack) + sha256(nx_unpack.dll)
          )[:16]
```

- **为什么够**：解包字节只由「原始包字节」和「nx_unpack 程序」决定。包只读、不会被原地改，所以用
  `字节数 + mtime` 代替整包哈希（16 GB 的包不必每次全读）；工具换了由工具内容哈希兜住。
- **为什么不看文件名**：10-20 维护者定「按内容认包」，程序号由 `pack/srcs.sh` 的 `src_ids` 调
  `nx_unpack --nacp-only` 读出，缓存按「路径 + 字节 + mtime」记在 `S2A_SRC_IDS`。
- **目标 / ABI / 工具链为什么不进键**：这一步只把游戏包换成文件树，跟后面怎么改写、怎么打包无关。
- 与 `oneclick` 的 `.oneclick_key` 是两套独立键（那边含 `--aoc-index` / `--title-id` 与 macOS `stat` 输出）；
  本片不动 `oneclick`，两套键互不冒充。

### 系统数据（无 `.stage_*`，沿用原实现）

```
已齐 = 字体目录逐文件一致（diff -rq） ∧ systemdata/010000000000080E 存在（目录名大写） ∧ COPY/SYNTH 都为空
TAG  = "sys-" + sha256(排序后的各 .bfttf 的 sha256 清单)[:8] + "-tzup" + ("-sd" if 有拷入) + ("-syn" if 有合成)
```

- **为什么够**：产物就是「字体原文件 + 时区数据 + 每款共用的系统数据目录（优先已有真实数据，其次玩家目录，否则合成 0802 / 0823；需求表不参与判断）」，全部由文件内容 / 目录存在性决定。
- **为什么没有阶段键**：原实现就没有（`pack/stage_keys.sh` 注释也写明「系统数据没有阶段键、不算」），本片不改语义。
- **目标 / ABI / 工具链为什么不进键**：这里只有文件拷贝与宿主侧数据生成，没有目标编译器参与。
- 已知缺口（原实现如此，不在本片修）：`sysdata_synth.py` 自身改版不会让已齐的根重做。

### RomFS 索引（`.stage_romfsidx`）

```
KR = sha256( romfs/ 下逐文件 "相对路径 TAB 字节数"（目录名排序） )[:16]
KT = sha256( git HEAD:services/S2A.Services/RomFsIndex.cs
             git HEAD:services/romfs_index
             git HEAD:services/romfs_index.sh
             git HEAD:compat/tree/Directory.Packages.props )[:16]
K  = sha256("KR KT")[:16]
```

- **为什么够**：索引只记路径 / 字节数 / 偏移，同一份清单生成的索引字节相同；**文件内容不进键**（改内容不改清单
  不需要重出索引）。写法只取真正决定索引字节的四处（格式与读方同一份、索引生成器、LibHac 版本）。
- **目标 / ABI / 工具链为什么不进键**：索引是纯数据，各端读同一份 `romfs.s2aidx`；生成逻辑由 `KT` 固定。
- 注意：`KT` 取的是 **git HEAD** 的对象号，不是工作树；未提交的索引写法改动不会改变键（原实现如此）。

### 着色器（已删）

11-07 拆通用模式：只服务通用模式的着色器阶段（`pack/stages/shaders.sh`、`pack/stage_shaders.sh`）与客体代码表（`pack/stage_direct_gk.sh`）一并删掉；
出包只有直连库（`pack/stage_direct.sh`，键规则见下表）。

### 元数据（`.stage_nacp`：没有）

```
已做 = 现行产物根里有 ctl/control.nacp
TAG  = "nacp-" + exefs/main 0x40 起 4 字节十六进制（od 读）   # main 构建号，与程序同版本
```

- **为什么没有阶段键**：原实现就没有（`pack/stage_keys.sh` 注释写明「元数据不算」）。判断只有「产物在不在」，
  因为 `ctl/control.nacp` 一旦进了产物根就跟 exefs 同版本；换版本时 `pack/refresh.sh` 会随新程序重新解包、
  不再带旧的 `ctl/`，所以「在」就等于「对应这一版程序」。
- **为什么用 main 构建号做标签**：用户看得懂「这一版元数据对应哪个构建」，也从标签看得出没有阶段键、不进
  `stage_keys.sh` 的记分板。
- **目标 / ABI / 工具链为什么不进键**：产物是游戏自带的元数据字节，各端读同一份。

### 追加内容（`.stage_aoc`）

```
selected = { 程序号 == 现行产物根程序号 的包 } ∪ { 程序号 = "-" 的包 }   # 后者是读不出控制分区的更新 / DLC
K_aoc = sha256(
           逐行 "绝对路径 TAB 字节数 TAB mtime TAB 程序号"     # 即 pack/srcs.sh 的 src_ids 输出
         + sha256(nx_unpack.dll) + sha256(nx_unpack)
         + exefs/main 0x40 起 16 字节十六进制（od 读）      # main 构建号（16 字节）
        )[:16]
C = $S2A_AOC_CACHE/<款>-<K_aoc>/                             # 解好的 DLC 全机共享，各产物根克隆
```

- **为什么够**：解出的 DLC 只由「给了哪些原始包」和「nx_unpack 程序」决定；包只读，用 `字节数 + mtime` 代替整包哈希。
- **为什么带 main 构建号**：`nx_unpack --aoc-only` 要按产物根的程序版本挑 DLC 版本，程序换了必须重解。
- **为什么带 `src_ids` 的顺序**：清单里少一个包（比如单独 DLC 包被挪走）就少一份 DLC，键必须变。
- **目标 / ABI / 工具链为什么不进键**：DLC 是游戏内容，与出到哪个平台无关；Android 宿主换 `CC` 不会重解 5 GB DLC。
- **缓存与事务分开**：`C` 是「解一次、全机复用」，产物根里的 `aoc/` 是从 `C` 克隆的副本；`ROOT_REPLAY` 只声明
  `aoc aoc.tsv aoc.log .stage_aoc`，所以能和改写 / 系统数据 / 界面布局并行提交。
- **解不出不拦出包，但点名**（审计 P2-30）：先整份解；整份失败时逐份兜底——每次给「上下文（程序号对上的包 + 内容清单里有本体 / 更新的包）+ 一份纯 DLC 包」，
  能解的进产物根，解不出的每份在 `aoc.log` 写一行固定格式 `追加内容缺<TAB>文件名<TAB>原因`（运行区出包队列按它登记），出包完成行 / 一键出包 result
  的 `aoc_missing` / 登记说明都带「追加内容 <文件名> 未能解出，包内不含」（`pack/roots.sh` root_aoc_note）。有缺的结果不进共享缓存 `C`；
  一份都没解出时 `aoc/`、`aoc.tsv`、`.stage_aoc` 沿用现行（下次刷新再试），只换 `aoc.log`。

## 阶段 I/O 对照表（全量）

「事务」=`root_begin` / `root_commit`（版本化产物根，见 `pack/roots.sh`）；「键」= 本阶段 `.stage_*` 的算法。

| 阶段（入口） | 输入 | 输出（相对产物根） | 事务 | 现有键 | 判定 | 一句话理由 |
|---|---|---|---|---|---|---|
| `pack/stage_keys.sh` | 指定产物根 + 本树代码 | 无（stdout：「阶段 TAB 键 TAB 根里记的键」） | 无（只读） | 各阶段自己的键（`S2A_ROOT_CUR` / `S2A_STAGE_KEYS`） | **与目标无关**（工具，留在 `pack/`） | 只算键、不出产物 |
| `pack/stages/nacp.sh`（旧入口 `pack/stage_nacp.sh` 转发） | 现行根 `exefs/main`（构建号）、原始包、`S2A_KEYS` / `S2A_NX_UNPACK` | `ctl/control.nacp`、`nacp.log` | begin/commit（无 REPLAY） | 无（已有 `ctl/control.nacp` 就跳过；标签 `nacp-<构建号>`） | **共享（第二批已抽）** | 元数据由游戏包决定，与出哪个平台无关 |
| `pack/stages/aoc.sh`（旧入口 `pack/stage_aoc.sh` 转发） | 现行根 `exefs/main` / `unpack.tsv`、`S2A_GAME_SRC` 内容清单、`S2A_AOC_CACHE` | `aoc/`、`aoc.tsv`、`aoc.log`、`.stage_aoc` | begin/commit + `ROOT_REPLAY='aoc aoc.tsv aoc.log .stage_aoc'` | `sha256( 候选包「路径 大小 mtime」 + nx_unpack / .dll + main 构建号 )[:16]` | **共享（第二批已抽）** | DLC 数据是游戏内容，与目标无关 |
| `pack/stage_sysdata.sh` | 现行根、`S2A_FONTS`、`S2A_SYSDATA`、`sysdata_need.tsv` | `systemdata/`、`tz.log` | begin/commit + `ROOT_REPLAY='systemdata tz.log'` | 无阶段键；`TAG=sys-<字体清单 sha8>-tzup[-sd][-syn][-dr]` | **共享（本片已抽）** | 纯数据文件拷贝 / 生成 |
| `pack/stage_romfsidx.sh` | 现行根 `romfs/`、`HEAD` 的索引写法 | `romfs.s2aidx`、`romfs_index.log`、`.stage_romfsidx` | begin/commit | `sha256( romfs 清单哈希 + 写法对象号哈希 )[:16]` | **共享（本片已抽）** | 纯数据索引，各端读同一份 |
| `pack/stage_rewrite.sh` | 现行根 `exefs/`（`addr_need.tsv` 只由 `tools/addr_need.py` 静态识别 `exefs/main` 得出，不读逐款表、不读跑局日志）、`runtime/rewriter`、`graphics/widen/scan`、启用表、`S2A_TARGET`、`S2A_REWRITE_SPIN` | `*.patched.bin`、`*.fixups.tsv`、`*.gates.tsv`、`*.sites.tsv`、`*.gaps.tsv`、`*.npdm`、`widen_sites.tsv`、`widen_enable*.tsv`、`addr_need.tsv`、`nro_exports.tsv`、`.stage_rewrite` | begin/commit + 大 `ROOT_REPLAY` | 改写器 / 扫描器对象号 + 启用表内容 + **`S2A_TARGET`** + 自旋开关 + 地址 / NRO 表 | **目标相关（留插件）** | 补丁代码按目标不同（`S2A_TARGET`），Android 走 `make_pack.py` 自己的改写 |
| `pack/stage_extract.sh` | 现行根 `romfs/` / `exefs/main` / `aoc/`、所选配置档、抽取代码（`KF` 列的文件）、Python 与解包库版本 | `extract/`（`ue_shaders/`、`pkg/`、`split/`、`s2x/`、`ok`）、`extract.log`、`.stage_extract` | begin/commit | `sha256( 输入清单哈希 + 抽取代码对象号 + 库版本 + 配置档键 )[:16]`（10-25「抽着色器只做一次」，直连库阶段只读） | **共享** |
| `pack/stage_direct.sh` | 现行根 `romfs/` / `exefs/main` / `aoc/`、`graphics/nvn`、`graphics/tools/direct`、Metal 编译器版本、配置档、`.stage_shaders`、`.stage_extract`、`extract/` | `shaders/direct/`、`direct_lib.log`、`.stage_direct` | begin/commit + `ROOT_REPLAY='shaders/direct direct_lib.log ue_shaders ue_pak.log .stage_direct'` | 游戏输入 + 出库代码对象号 + **iOS Metal 版本** + 着色器键 + 配置档键 | **目标相关（留插件）** | NVN → Metal 直连库，产物只对 iOS 有意义 |
| `pack/stage_uilayout.sh` | 原版 `romfs`（`seed.txt` 的 `src=`）、`ui_lyt.py` + `yaz0enc.c` 版本、屏幕宽 `W`（缺省 2346） | 覆盖后的 `romfs/`、`ui_overlay.tsv`、`.stage_uilayout`；删 `.stage_romfsidx` / `romfs.s2aidx` | begin/commit + `ROOT_REPLAY='romfs ui_overlay.tsv .stage_uilayout .stage_romfsidx romfs.s2aidx'` | `W` + 工具版本 + 覆盖文件内容哈希 | **目标相关（留插件）** | 覆盖按目标屏幕宽与画幅策略生成 |
| `pack/stage_direct_profile.sh` | 现行根 `exefs/` / `romfs/`、配置档工具 | `direct-profile/`、`direct_profile.json`、`.stage_direct_profile` | 仅生成临时档时 begin/commit | `direct_profile_stage.py --key` | **目标相关（留插件）** | NVN 直连配置（iOS / Metal 路线），Vulkan 款跳过 |
| `android/tools/make_pack.py` | **已解包**的产物根：`exefs/main.npdm`、`romfs/`、`ctl/control.nacp`（可选 `romfs.s2aidx` / `systemdata` / `aoc*` / `shaders/`）；`S2A_TARGET=android` 的改写 | 包目录：`manifest.json` / `index.tsv` / 文件树 + `modules/` + `game-build-artifact.json` | 无 `roots.sh`；`<out>` 只允许是包目录或空（否则要 `--force`） | `contentRoot = sha256(index.tsv)`；模块 `REWRITE_ABI=android-sp-safe-v1` | **目标相关（留 Android 插件）** | Android 改写 + APK 资源布局；**它不做选包 / 解包**，输入就是共享解包阶段的产物 |
| `pack/oneclick/oneclick.sh` | 源包文件夹、`--keys/--fonts/--sysdata` | `<out>/extract`、私有产物根、`.ipa`、资源包 | 私有根（`S2A_ROOTS=<out>/roots`） | `<out>/extract/.oneclick_key` | **目标相关（iOS 交付入口）** | 选包 / 解包段下一批改为调 `pack/stages/unpack.sh`，出口是 IPA / 资源包 |
| `pack/srcs.sh` | `S2A_GAME_SRC`、`S2A_SRC_IDS`、`S2A_NX_UNPACK` | 无（函数：`prog_id` / `src_ids` / `nxu_path`；`src_ids` 写 ids 缓存） | 无 | 按「路径 + 字节 + mtime」缓存程序号 | **与目标无关**（共享依赖，保留） | 按内容认包，与平台无关 |
| `pack/nx_unpack/` | `prod.keys`、输出目录、`xci/nsp` | `exefs/ romfs/ ctl/ aoc/ icon.jpg unpack.tsv` | 无（写调用方给的目录） | 无 | **与目标无关**（工具） | 解包字节由游戏包决定 |

## 目标隔离规则

1. 共享阶段的产物**相对路径里不许出现目标名**（`ios` / `android` / `ohos`）。本片六个阶段的产物只有
   `exefs/ romfs/ ctl/ aoc/ systemdata/ romfs.s2aidx shaders/` 与阶段键文件。
2. 目标名、ABI、宿主工具链环境变量**不进共享阶段的键**。目标插件要区分产物时，自己追加后缀
   （现有例子：`stage_rewrite.sh` 的 `S2A_TARGET` 后缀）。
3. 目标相关的东西一律留 `pack/targets/**` 或目标自己的入口：改写补丁、直连库、界面布局、打包 / 签名。
4. `tests/pack_stages_test.sh` 用合成夹具强制检查第 1、2 条（换 `S2A_TARGET` / `S2A_REWRITE_ABI` / `CC`
   键不变；产物相对路径不含目标名）——六个阶段各有一组。

## 本片没做（明确边界）

- **没抽**：`keys`（`pack/stage_keys.sh` 只读算键、不出产物，保留在 `pack/`）/ `rewrite` / `direct` / `uilayout` /
  `direct_profile`（后四个目标相关，留在插件，不需要抽）。
- **没改** `pack/oneclick/oneclick.sh`、`android/tools/make_pack.py`：它们不在本片允许改的目录里。
  Android 的 `make_pack.py` 本来就不做选包 / 解包；共享 `unpack.sh` 的产物（`exefs/main.npdm`、`romfs/`、
  `ctl/control.nacp`）正好是它 `--root` 的必需输入，本片只做夹具级一致，不声称已重接。
  `nacp.sh` / `aoc.sh` 同理：Android 宿主今天仍由 `make_pack.py` 用自己的路径取 DLC 数据，本片只把 iOS 侧入口
  改成转发，Android 重接留给维护方定。
- **没跑真机整包**：本机没有游戏源、没有 `~/.s2arm/run/s2arm/roots`、没有 dotnet，`nx_unpack` / 服务层索引 /
  Metal 烘焙都用假工具验证接线；「iOS 产物逐字节不变」只用「入口转发后与 `pack/stages/*.sh` 直接调用一致」证明，
  真机整包对拍留给维护者第二层。
- **宿主可移植性**：`pack/srcs.sh` 的 `src_ids`（共享 `unpack.sh` 与 `aoc.sh` 复用）10-28 起 GNU `stat -c` / BSD `stat -f` 都认、读不出就停
  （审计 #824，`tests/linux_port_test.sh`）；Windows 宿主（J05 / #179）仍未覆盖。
