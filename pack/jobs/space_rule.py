#!/usr/bin/env python3
"""直连库开工前复核要多少盘（graphics/tools/direct/direct_lib.sh 在抽取之后、翻译编译之前调；Mac / Windows 两条链都过这里，#265：预检放过、编到一半写满白编十几分钟）。
规则与出包作业预检同一份：pack/jobs/library.py 的 _pack_direct_bytes（按原件大小分两档：小于 3 GB 记 5 GB、其余记 15 GB），这里从 library.py 源码里取，
  不另抄常量（library 导入 Mac 的 core / fcntl，Windows 链导不进，同 winenv._budget_rules 的取法）。原件大小在这一步不知道，用产物根里解出的游戏内容
  （romfs/ exefs/ aoc/）代替——即预检 _pack_input_bytes 取的 contentBytes 那一项。已在盘上的同款直连缓存（重出包复用）从中扣掉。
11-10 去掉原先「按着色器抽取量 × 系数」（缓存 9× + 库 3×）：抽取字节数不是直连占用的驱动量，抽取产物有的是压缩过的着色器档案、有的是原文——
  0100559011740000 抽取 0.48 GiB、01008FC00D964000 抽取 1.66 GiB，两款直连缓存峰值却差不多（6.3 / 7.0 GiB），乘系数注定一款多估一款少估。
CLI：space_rule.py direct-check <产物根> <直连缓存目录> <输出目录> → 够：退 0；不够：打「磁盘空间不足」原话、退 1（S2A_SPACE_OVERRIDE=1 时只打黄、退 0）；复核自己没跑成退 2"""
import ast, os, shutil, sys

CONTENT_DIRS = ("romfs", "exefs", "aoc")


def tree_bytes(path):
    """目录实际分配的字节（不跟软链，按 inode 去重）；不在 = 0"""
    seen, total, stack = set(), 0, [os.fspath(path)]
    while stack:
        p = stack.pop()
        try: st = os.lstat(p)
        except OSError: continue
        if (st.st_dev, st.st_ino) in seen or os.path.islink(p): continue
        seen.add((st.st_dev, st.st_ino))
        total += st.st_blocks * 512 if hasattr(st, "st_blocks") else st.st_size
        if os.path.isdir(p):
            try: stack.extend(os.path.join(p, n) for n in os.listdir(p))
            except OSError: pass
    return total


def direct_bytes(size):
    """原件（游戏内容）size 字节 → 直连段要的字节；就是 library._pack_direct_bytes，从 library.py 源码里取那一个函数与它的三个常量"""
    lib = os.path.join(os.path.dirname(os.path.abspath(__file__)), "library.py")
    with open(lib, encoding="utf-8") as f: tree = ast.parse(f.read())
    want = {"PACK_SMALL_BYTES", "DIRECT_SMALL_BYTES", "DIRECT_LARGE_BYTES"}
    body = [n for n in tree.body if isinstance(n, ast.Assign) and want & {t.id for x in n.targets for t in ast.walk(x) if isinstance(t, ast.Name)}]
    body += [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "_pack_direct_bytes"]
    if len(body) != 2: raise ValueError(f"{lib} 里找不到唯一的 _pack_direct_bytes 与它的常量")
    ns = {}; exec(compile(ast.Module(body=body, type_ignores=[]), lib, "exec"), ns)
    return ns["_pack_direct_bytes"](size)


def _existing(path):
    path = os.fspath(path)
    while path and not os.path.exists(path): path = os.path.dirname(path)
    return path or "/"


def _free(vol):
    """盘余量；S2A_DISK_FREE_G（GB，单测注入，与 ipa.sh / refresh 同名口径）给了就用它，单测结果不随本机磁盘余量变（11-09 门禁机余量紧时
    直接真跑 direct_lib.sh 的单测被复核拦下、桩工具没调起）"""
    g = os.environ.get("S2A_DISK_FREE_G")
    return shutil.disk_usage(vol) if not g else shutil._ntuple_diskusage(0, 0, int(float(g) * 1e9))


def direct_check(root, cache_dir, out_dir, free=_free):
    """→ None（够）或给玩家看的一句（不够）。缓存目录与输出目录各在哪块盘都核这一个数（同一块盘只核一次）"""
    size = sum(tree_bytes(os.path.join(root, d)) for d in CONTENT_DIRS)
    rule = direct_bytes(size); need = max(0, rule - tree_bytes(cache_dir))
    vols = {}
    for d in (cache_dir, out_dir): v = _existing(d); vols.setdefault(os.stat(v).st_dev, v)
    for vol in vols.values():
        have = free(vol).free
        if have < need:
            return f"磁盘空间不足：直连库预计还要 {need / 1e9:.1f} GB（游戏内容 {size / 1e9:.2f} GB，按出包预检同一规则记 {rule / 1e9:.0f} GB），这块盘只剩 {have / 1e9:.1f} GB"
    return None


if __name__ == "__main__":
    if len(sys.argv) != 5 or sys.argv[1] != "direct-check": sys.exit("用法：space_rule.py direct-check <产物根> <直连缓存目录> <输出目录>")
    try: why = direct_check(*sys.argv[2:])
    except Exception as e:   # 复核自己没跑成（如缺 library.py）退 2：Python 未接住的异常退 1，会被 direct_lib.sh 当成「空间不足」
        print(f"【红】直连库开工前空间复核没跑成：{type(e).__name__}: {e}"); sys.exit(2)
    if why and os.environ.get("S2A_SPACE_OVERRIDE") == "1": print(f"【黄】{why}；已按你的选择仍然继续"); sys.exit(0)
    if why: print(f"【红】{why}"); sys.exit(1)
