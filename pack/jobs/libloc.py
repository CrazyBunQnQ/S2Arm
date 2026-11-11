"""游戏库位置检查（纯函数，Mac / Windows 共用；11-07 启动器：预检加云盘目录与卷格式，拦下时给「换游戏库」）。

游戏库是出包的工作数据（解包、产物根、缓存，几十 GB、上万个小文件），要放在本机磁盘、能原子改名的格式上：
  - 云盘同步目录（iCloud Drive、OneDrive、Dropbox、Google Drive …）：同步客户端会把文件换成占位、上传几十 GB 中间产物、
    改名 / 删除时和同步冲突，出包中途读不到文件或变慢；
  - 卷格式：Mac 要 APFS（播种、发布要原子的「不覆盖改名」和克隆，同 library.LIBRARY_FORMAT_MESSAGE）；
    Windows 要 NTFS / ReFS（FAT32 单文件 4 GB 上限，exFAT 没有硬链接 / 符号链接与可靠的原子替换）。
不导入 core / fcntl：winenv.py 在 Windows 上直接 import 本模块。判断只看路径字符串与调用方给的环境，便于两边同一套单测。
"""
from __future__ import annotations
import ntpath
import posixpath

# 每项：(云盘名, 相对家目录的路径前缀)。Mac：iCloud Drive 在 ~/Library/Mobile Documents；第三方云盘（文件提供程序）在 ~/Library/CloudStorage
MAC_CLOUD = (("iCloud 云盘", "Library/Mobile Documents"), ("云盘（文件提供程序）", "Library/CloudStorage"))
# 「桌面与文稿」开了 iCloud 同步时，~/Desktop、~/Documents 的内容在 iCloud Drive 里有同名目录
MAC_ICLOUD_DESKTOP_DOCS = ("Desktop", "Documents")
# Windows：OneDrive 客户端设的环境变量（个人 / 工作学校）；iCloud for Windows、Dropbox 的缺省目录
WIN_CLOUD_ENV = (("OneDrive", "OneDrive"), ("OneDrive", "OneDriveConsumer"), ("OneDrive", "OneDriveCommercial"))
WIN_CLOUD_HOME = (("iCloud 云盘", "iCloudDrive"), ("Dropbox", "Dropbox"), ("Google 云端硬盘", "Google Drive"))

MAC_OK_FORMATS = ("apfs",)
WIN_OK_FORMATS = ("ntfs", "refs")


def _under(path, root, mod):
    p, r = mod.normcase(mod.normpath(path)), mod.normcase(mod.normpath(root))
    return p == r or p.startswith(r.rstrip(mod.sep) + mod.sep)


def cloud_folder(path, *, system, home, env=None, exists=None):
    """path 在云盘同步目录里 → 云盘名；不在 → None。system 'Darwin' / 'Windows'；home 家目录；env 环境变量（Windows 用）；
    exists(p) 判断路径存在（Mac 判「桌面与文稿」是否开了 iCloud 同步用；不给就不判这一条）"""
    if not path or not home:
        return None
    if system == "Windows":
        env = env or {}
        for name, var in WIN_CLOUD_ENV:
            root = env.get(var)
            if root and _under(path, root, ntpath):
                return name
        for name, rel in WIN_CLOUD_HOME:
            if _under(path, ntpath.join(home, rel), ntpath):
                return name
        return None
    for name, rel in MAC_CLOUD:
        if _under(path, posixpath.join(home, rel), posixpath):
            return name
    if exists is not None and exists(posixpath.join(home, "Library/Mobile Documents/com~apple~CloudDocs/Desktop")):
        for rel in MAC_ICLOUD_DESKTOP_DOCS:
            if _under(path, posixpath.join(home, rel), posixpath):
                return "iCloud 云盘（桌面与文稿同步）"
    return None


def format_ok(fmt, *, system):
    """卷格式能不能放游戏库；读不到格式（空串）不拦（同 library.require_apfs）"""
    fmt = (fmt or "").lower()
    if not fmt:
        return True
    return fmt in (WIN_OK_FORMATS if system == "Windows" else MAC_OK_FORMATS)



SLACK_CLUSTER = 64 << 10   # 分配单元（簇）≥ 这么大就提示：解包有大量小文件，每个至少占满一个簇（#444 玩家 512 KB 簇的外置盘实占膨胀 5 倍以上）


def slack_reason(fmt, cluster, *, system):
    """存放位置所在卷会让出包多占空间时给原因（只提示不拦），没问题给 None。原因不同、多占的量不同，提示要说对：
    "cluster" = 分配单元 ≥ 64 KB：解包的大量小文件每个至少占满一个簇，多占数倍（先判这条，它占得最多）；
    "format" = 格式不是这一端的推荐格式（Mac APFS 才能克隆 / Windows NTFS、ReFS 才有硬链接）：解包和产物要多拷一份。
    格式和分配单元都读不到 → None（不凭空吓人）"""
    fmt = (fmt or "").lower()
    if (cluster or 0) >= SLACK_CLUSTER:
        return "cluster"
    if fmt and not format_ok(fmt, system=system):
        return "format"
    return None

LIBRARY_FORMAT_MESSAGE = ("这个位置所在的磁盘格式是 {fmt}。游戏原件放在这块盘上没问题（S2Arm 只读、不改不删），"
                          "但导出时的工作数据（游戏库）要放在 APFS 格式的磁盘上：请改用这台 Mac 上的默认位置（内置盘），"
                          "或选一块 APFS 格式的外接盘；外接盘可以在「磁盘工具」里另分出一个 APFS 分区（盘上要有空闲空间，原有数据不动）。")
CLOUD_MESSAGE = ("游戏库在「{cloud}」的同步目录里。出包时游戏库里会写几十 GB 的中间文件，云盘会把它们上传、换成占位文件，"
                 "出包中途读不到文件或变得很慢。请把游戏库换到本机磁盘上不同步的位置（例如默认位置）。游戏原件放在哪都行，不受影响。")
WIN_FORMAT_MESSAGE = ("这个位置所在的磁盘格式是 {fmt}。游戏原件放在这块盘上没问题（S2Arm 只读、不改不删），"
                      "但导出时的工作数据（游戏库）要放在 NTFS 格式的磁盘上：请改用这台电脑上的默认位置，或选一块 NTFS 格式的盘。")


def problems(path, *, system, home, fmt, env=None, exists=None):
    """游戏库位置的问题清单：[{code, message}]，没问题是空表。code 对应 pack/jobs/errors.py RECOVERY 与 experience.v1.json errors，
    启动器据此显示「换游戏库」动作"""
    out = []
    cloud = cloud_folder(path, system=system, home=home, env=env, exists=exists)
    if cloud:
        out.append(dict(code="library_in_cloud", message=CLOUD_MESSAGE.format(cloud=cloud)))
    if not format_ok(fmt, system=system):
        if system == "Windows":
            out.append(dict(code="library_bad_format", message=WIN_FORMAT_MESSAGE.format(fmt=fmt.upper())))
        else:
            out.append(dict(code="library_not_apfs", message=LIBRARY_FORMAT_MESSAGE.format(fmt=fmt.upper())))
    return out
