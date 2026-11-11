#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""预编壳的实际输入闭包（维护者 11-04：壳编号 = 壳构建输入的指纹，取 make ios 出壳实际读到的文件，不手挑）。
make ios 由 clang / dotnet / Python / cp 等多种进程组成，阶段键的 Python 读入钩子（tools/stage_deps_hook）只看得到 Python；这里改用文件访问时间：
出壳前把仓库里算输入的文件（git 跟踪 + 未忽略，同 tools/stage_deps.py）的访问时间设成固定旧值，出壳后访问时间变新的就是这次被读过的——
任何进程读都算（APFS / ReFS 读文件会更新访问时间：访问时间早于 24 小时前时必更新）。

  closure.py mark    [<仓库根>]           出壳前：访问时间设旧（只改访问时间，不改内容与修改时间）
  closure.py observed [<仓库根>]          出壳后：打印被读过的文件（仓库相对路径，一行一个，已排序）
  closure.py check   [<仓库根>]           出壳后：被读过、但输入表 pack/stage_inputs/shell.tsv 没覆盖（不是「入」/「外」的文件，也不在其列的目录下）→ 逐个点名、退出 1
  closure.py pyimports [<仓库根>]          不出壳的静态核对（11-06 维护者：列车没有出壳这一步，#1586 让 pack/ship_ext.py import 新增的 tools/module_order.py、
                                         没登记就合进 main，全线出壳红）：表里「入」的 .py 逐个解析 import，顺着解析到的仓库模块递归，没被表覆盖的报红；几秒内跑完
  closure.py csiblings [<仓库根>]          不出壳的静态核对（11-10 v0.2.3 出壳红：单只 Joy-Con 2 新增 runtime/ns2pro/NS2JoyCon.c 没登记，列车没有出壳这一步）：
                                         表里逐文件登记过的目录下，git 跟踪的 C 系文件（.c .m .mm .cpp .h .hpp .inc）必须是「入」「外」或「不」（不编进壳）之一；
                                         没登记的点名、退 1。「不」行只给本核对用，出壳闭包与壳编号都不认它
  closure.py suggest [<仓库根>]           冷出壳后：把被读过的文件收成表行（目录里跟踪文件被读 ≥ 80% 的整目录收成一行，多收只会多重出壳、不会漏）

新依赖只能经由已登记文件的改动引入（加 #include、加源文件都要改已登记的源码 / 构建脚本），而改动必然让那部分重编、新文件被读到，
所以热出壳（有缓存）时 check 也能抓到漏登记；本机制只管仓库内文件，仓库外的工具链版本另记（壳编号含 iOS 最低版本；编译器版本见壳清单 details）。
"""
import hashlib
import os
import subprocess
import sys
import tempfile

MARK = 978307200   # 2001-01-01 00:00:00 UTC：访问时间设成它（早于 24 小时前，任何读都会把它更新到现在）
H = os.path.dirname(os.path.abspath(__file__)); R0 = os.path.dirname(os.path.dirname(H))
TABLE = 'pack/stage_inputs/shell.tsv'


def tracked(repo):   # 仓库里算输入的文件：git 跟踪 + 未跟踪但没被忽略（构建产物 / 缓存被 .gitignore 挡掉）
    out = subprocess.run(['git', '-C', repo, 'ls-files', '-co', '--exclude-standard', '-z'], capture_output=True, check=True).stdout
    return sorted(f for f in out.decode('utf-8', 'surrogateescape').split('\0') if f)


def listing(repo):   # mark 时存下的文件清单：observed 不再调 git（git ls-files 会读 .gitignore，自己把它们记成「被读过」）
    return os.path.join(tempfile.gettempdir(), 's2a-shell-closure-' + hashlib.sha1(os.path.realpath(repo).encode()).hexdigest()[:12] + '.txt')


def settle_index(repo):
    """mark 前把 git 索引里「刚写过」的条目刷成干净（11-07 定因：新检出 / 刚合并的树，最后一秒写出的文件与索引同一秒 = 「racily clean」，
    之后任何全树 git status / diff 都要重读它们的内容——即使 core.trustctime=false；冷出壳（换工具链清了中间产物后）路径里有这样的 git 调用，
    闭包把这些重读当成「出壳读到表外文件」：全新工作树 1168–1295 个、刚合并的增量树 23 个，壳随即被删）。
    等过一秒再全树刷新索引：索引写入时刻晚于所有文件修改时刻，条目不再 racy，之后的 git 命令只比 stat、不读内容"""
    import time
    time.sleep(1)
    subprocess.run(['git', '-C', repo, 'update-index', '-q', '--refresh'], capture_output=True)   # 有改动的文件退出非 0，不影响（改动由 shell_build.sh 前面那步拦）


def mark(repo):
    settle_index(repo)
    n = 0; files = tracked(repo)
    with open(listing(repo), 'w', encoding='utf-8', newline='\n') as f: f.write(''.join(x + '\n' for x in files))
    for f in files:
        p = os.path.join(repo, f)
        try:
            st = os.lstat(p)
            if os.path.islink(p): continue
            os.utime(p, ns=(MARK * 10**9, st.st_mtime_ns)); n += 1
        except OSError: pass
    return n


def countable(f):   # 本工具自己、输入表本身（核对时自己读；表里的「入」集合已决定壳编号）、.gitignore / .gitattributes（git 命令自己读的忽略 / 属性规则，不影响壳内容）不计
    #   11-07：出壳期间的 git 调用会读 .gitattributes（属性规则：换行 / 过滤器，只管检出），被当成「表外读入」把壳删掉
    return f not in ('pack/shell/closure.py', TABLE) and os.path.basename(f) not in ('.gitignore', '.gitattributes')


def observed(repo):
    try:
        with open(listing(repo), encoding='utf-8') as f: files = f.read().splitlines()
    except OSError: raise SystemExit('【红】没有出壳前的文件清单（先 closure.py mark）')
    out = []
    for f in files:
        if not countable(f): continue

        p = os.path.join(repo, f)
        try:
            if not os.path.islink(p) and os.lstat(p).st_atime > MARK + 86400: out.append(f)
        except OSError: pass
    return out


def table(repo):   # → (入 / 外 的文件与目录集合)
    rows = []
    with open(os.path.join(repo, TABLE), encoding='utf-8') as t:
        for line in t:
            parts = line.rstrip('\n').split('\t')
            if len(parts) >= 2 and parts[0] in ('入', '外'): rows.append((parts[0], parts[1].rstrip('/')))
    return rows


def covered(f, rows):
    return any(f == p or f.startswith(p + '/') for _, p in rows)


C_FAMILY = ('.c', '.m', '.mm', '.cpp', '.h', '.hpp', '.inc')


def c_siblings(repo):
    """→ 表里逐文件登记过的目录下、没登记的 git 跟踪 C 系文件（排序）。「不」行（不编进壳）算已登记；整目录登记的目录下本来就全覆盖，不用看"""
    rows = table(repo)
    no = set()
    with open(os.path.join(repo, TABLE), encoding='utf-8') as t:
        for line in t:
            parts = line.rstrip('\n').split('\t')
            if len(parts) >= 2 and parts[0] == '不': no.add(parts[1].rstrip('/'))
    files = tracked(repo); fs = set(files)
    dirs = {os.path.dirname(p) for _, p in rows if p in fs}
    return sorted(f for f in files if f.endswith(C_FAMILY) and os.path.dirname(f) in dirs and f not in no and not covered(f, rows))


def py_imports(repo, rows):
    """静态 Python import 闭包：起点 = 表里「入」的 .py（含「入」目录下的 .py）；模块搜索根 = 本文件目录、仓库根、
    本文件里以字符串字面量出现的仓库一级目录（覆盖 sys.path.insert(0, str(REPO / 'tools')) 这类写法）。只认解析得到仓库文件的模块，标准库 / 三方包自然跳过。
    只看模块顶层的 import（导入即执行）；搜索顺序 仓库根 → 字面量一级目录 → 本文件目录（同 sys.path.insert(0, …) 后的实际顺序）。
    → 被 import 到、但表里没覆盖的仓库 .py（排序）"""
    import ast
    def toplevel(tree):   # 模块顶层（含顶层 if / try 里）的 import：导入模块时必然执行；函数 / 类里的延迟 import 出壳不一定走到，不算（避免误报）
        st = list(tree.body)
        while st:
            n = st.pop()
            if isinstance(n, (ast.Import, ast.ImportFrom)): yield n
            elif isinstance(n, (ast.If, ast.Try)): st += n.body + n.orelse + getattr(n, 'finalbody', []) + [x for h in getattr(n, 'handlers', []) for x in h.body]
    files = set(tracked(repo)); tops = {f.split('/', 1)[0] for f in files if '/' in f}
    todo = sorted(f for f in files if f.endswith('.py') and any(k == '入' and (f == p or f.startswith(p + '/')) for k, p in rows))
    seen, miss = set(todo), set()
    dirs = {os.path.dirname(f) for f in files}; dirs |= {d2 for d in list(dirs) for d2 in [d.rsplit('/', i)[0] for i in range(d.count('/') + 1)]}
    def resolve(base, mod):   # → 仓库文件；'' = 命中目录包（无 __init__ 的命名空间包，到此为止、不再往后面的搜索根找）；None = 这一根下没有
        rel = mod.replace('.', '/')
        for c in (rel + '.py', rel + '/__init__.py'):
            c = os.path.normpath(os.path.join(base, c)) if base else c
            if c in files: return c
        d = os.path.normpath(os.path.join(base, rel)) if base else rel
        return '' if d in dirs else None
    while todo:
        f = todo.pop()
        try: tree = ast.parse(open(os.path.join(repo, f), encoding='utf-8').read())
        except (OSError, SyntaxError, ValueError): continue
        here = os.path.dirname(f)
        roots = [''] + sorted({n.value for n in ast.walk(tree) if isinstance(n, ast.Constant) and isinstance(n.value, str) and n.value in tops}) + [here]
        mods = []
        for n in toplevel(tree):
            if isinstance(n, ast.Import): mods += [a.name for a in n.names]
            elif isinstance(n, ast.ImportFrom):
                if n.level:   # 相对导入：从本文件所在包往上 level-1 层
                    b = here
                    for _ in range(n.level - 1): b = os.path.dirname(b)
                    for name in ([n.module] if n.module else []) + [(n.module + '.' if n.module else '') + a.name for a in n.names]:
                        r = resolve(b, name)
                        if r and r not in seen: seen.add(r); todo.append(r)
                    continue
                mods += [n.module + '.' + a.name for a in n.names] + [n.module]   # 先认 from 包 import 子模块；包本身（__init__）其次
        for m in mods:
            for b in roots:
                r = resolve(b, m)
                if r is None: continue
                if r and r not in seen: seen.add(r); todo.append(r)
                break
    for f in seen:
        if not covered(f, rows): miss.add(f)
    return sorted(miss)


def suggest(repo, seen, frac=0.8):
    files = tracked(repo); seen = set(seen)
    total, hit = {}, {}
    for f in files:
        d = os.path.dirname(f)
        while d:
            total[d] = total.get(d, 0) + 1
            if f in seen: hit[d] = hit.get(d, 0) + 1
            d = os.path.dirname(d)
    out, done = [], set()
    for f in sorted(seen):   # 从最浅的目录起：够比例就整目录一行
        parts = f.split('/'); row = f
        for k in range(1, len(parts)):
            d = '/'.join(parts[:k])
            if total.get(d, 0) >= 2 and hit.get(d, 0) >= frac * total[d]: row = d; break
        if row not in done: done.add(row); out.append(row)
    return sorted(set(out))


def main(a):
    if not a or a[0] not in ('mark', 'observed', 'check', 'suggest', 'pyimports', 'csiblings'): print(__doc__, file=sys.stderr); return 2
    repo = os.path.abspath(a[1]) if len(a) > 1 else R0
    if a[0] == 'csiblings':
        repo = a[1] if len(a) > 1 else '.'
        miss = c_siblings(repo)
        for f in miss: print(f'【红】{f}：和表里逐文件登记的源码同目录，却没登记（编进壳记「入」，只读不影响产物记「外」，不编进壳记「不」）')
        print(f"壳输入表同目录 C 系核对：{'漏登记 ' + str(len(miss)) + ' 个' if miss else '0 个漏登记'}")
        return 1 if miss else 0
    if a[0] == 'pyimports':
        miss = py_imports(repo, table(repo))
        if miss:
            print(f'【红】壳输入表 {TABLE} 里「入」的 Python 会 import 到 {len(miss)} 个没登记的仓库文件（出壳会读，壳编号算不到）：' + ' '.join(miss), file=sys.stderr)
            print('  影响壳内容的记「入」，只读不影响产物的记「外」；和 import 它的改动同一笔登记', file=sys.stderr)
            return 1
        print('壳输入表 Python import 闭包：全在表里'); return 0
    if a[0] == 'mark': print(f'壳输入闭包：{mark(repo)} 个仓库文件访问时间已设旧'); return 0
    seen = observed(repo)
    if a[0] == 'observed': sys.stdout.write(''.join(f + '\n' for f in seen)); return 0
    if a[0] == 'suggest': sys.stdout.write(''.join(f'入\t{p}\n' for p in suggest(repo, seen))); return 0
    try:
        with open(listing(repo), encoding='utf-8') as f: total = sum(1 for x in f.read().splitlines() if countable(x))
    except OSError: total = 0
    if total and len(seen) >= 0.9 * total:   # 外部进程全量重读（多半是 git status：设旧访问时间改了文件状态时间，git 会把全部文件重读核对）
        print(f'【黄】壳输入闭包核不了：出壳期间仓库 {len(seen)}/{total} 个文件都被读过（有别的进程全量读了这棵树，多半是 git status / 编辑器监视），本次不判；'
              '出壳请在没有别的进程盯着的树里跑', file=sys.stderr)
        return 0
    rows = table(repo); miss = [f for f in seen if not covered(f, rows)]
    if miss:
        print(f'【红】出壳读到了输入表 {TABLE} 没登记的 {len(miss)} 个文件（壳编号算不到它们，改了会用错旧壳）：' + ' '.join(miss[:30]) + (' …' if len(miss) > 30 else ''), file=sys.stderr)
        print('  按用途归类加进表里：影响壳内容的记「入」，只读不影响产物的记「外」', file=sys.stderr)
        return 1
    print(f'壳输入闭包：出壳读到 {len(seen)} 个仓库文件，全在输入表里')
    return 0


if __name__ == '__main__':
    if hasattr(sys.stdout, 'reconfigure'): sys.stdout.reconfigure(encoding='utf-8', newline='\n')
    sys.exit(main(sys.argv[1:]))
