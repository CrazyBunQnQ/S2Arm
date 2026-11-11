#!/usr/bin/env python3
# 出包阶段输入闭包（维护者 10-26 20:3x：直连库 / 抽取键收窄，键 = 声明的输入文件内容 + 游戏输入，不再取整棵树对象号）
#   python3 tools/stage_inputs.py [--table <声明表>] <入口文件>…   打印入口经 bash / source / python 调用、python import、clang 编译与 #include 能走到的仓库跟踪文件（一行一个）
# 认的写法：脚本里的 $R / $ROOT / $H / $NX / $TOOLS 路径变量与 $(dirname "$0")；bash|source|. <路径>；python <路径>；clang … <源> 与 -I <目录>；
#   #include / #import "x"；python 的 import / from（本目录 + 它 sys.path.insert 的目录）；字面「graphics/… pack/… tools/…」相对路径。
# --table：表里记「外」的文件（排队、记账、进度等基础设施）照样打印、但不往里扫——它们的文档串 / 帮助文字提到的脚本不是本阶段依赖（10-26 一个排队脚本的文档串提到 pack/refresh.sh，闭包涨到 393 个）
# 只是静态扫描，动态拼出来的路径扫不到：门禁单测（tests/stage_keys_test.sh）拿它核「阶段声明的输入清单」漏没漏
import ast, io, os, re, subprocess, sys, tokenize

_ENV = os.environ.copy(); _ENV.pop('DEVELOPER_DIR', None)   # 账本 git 调用不吃工具链 env：坏 / 假 DEVELOPER_DIR 会让 /usr/bin/git 垫片 xcrun 报红
R = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True, text=True, env=_ENV).stdout.strip()
TR = set(subprocess.run(['git', '-C', R, 'ls-files'], capture_output=True, text=True, env=_ENV).stdout.split('\n'))
VARS = {'R': '', 'ROOT': '', 'H': 'pack', 'NX': 'graphics/nvn', 'TOOLS': 'tools'}
STOP = set()  # --table 里的「外」文件：收进闭包、不展开
INC = set()   # 各脚本 clang -I 给的目录：被编的 .m / .c 的 #include 按它们找（-I 写在调用方脚本里、不在源文件里）
EXT = r'(?:sh|py|m|mm|h|c|inc|tsv|json|lock|cs|csproj)'
NXGAL = None  # libnxgal 导出名 → 定义它的源码（见 nxgal_defs）


def nxgal_defs():   # 出库经 libnxgal 调的导出（C# DllImport / Python ctypes 写成名字、跨 .m 用 extern 声明）不是路径，#include 扫不到定义它的实现文件（审计 #1565 P1-115：gal_tessxc.m 漏登记）
    global NXGAL   # graphics/metal/gal_*.m / gal_*.c 里 NXGAL_API 定义（带函数体）的导出名 → 该文件；.c 再连上 #include 它的 gal_*.m（Mac / iOS 经它编进库，Makefile 只通配 gal_*.m）
    if NXGAL is None:
        NXGAL = {}
        src = [f for f in TR if re.fullmatch(r'graphics/(?:metal|dsx)/\w+\.(?:m|c)', f)]   # 11-08 直连翻译段搬到 graphics/dsx：导出定义在两边都找
        txt = {f: strip(f, open(os.path.join(R, f), errors='replace').read()) for f in src}
        for f, t in txt.items():
            own = {f} | {g for g, u in txt.items() if g.endswith('.m') and re.search(r'#\s*include\s+"' + re.escape(os.path.basename(f)) + '"', u)} if f.endswith('.c') else {f}
            for m in re.finditer(r'NXGAL_API\b[^;{}]*?\b(nxgal_\w+)\s*\([^;{}]*\)\s*\{', t):
                NXGAL.setdefault(m.group(1), set()).update(own)
    return NXGAL


def norm(p):
    p = os.path.normpath(p)
    return p[2:] if p.startswith('./') else p.lstrip('/')


def subst(expr, f):
    d = os.path.dirname(f)
    expr = re.sub(r'\$\(\s*cd\s+"?\$\(dirname\s+"?\$0"?\)"?[^)]*\)|\$\(\s*dirname\s+"?\$0"?\s*\)', d, expr)
    bad = []
    def var(m):
        v = m.group(1)
        if v in VARS: return VARS[v]
        if v in ('HERE', 'GFX', 'D'): return d
        bad.append(v); return ''
    expr = re.sub(r'\$\{?(\w+)\}?', var, expr)
    return None if bad else norm(expr)


def strip(f, t):   # 去注释再扫：注释里随手提到的路径不是依赖（10-26 首版不去注释，闭包 583 个文件、混进 tests / platforms）
    if f.endswith('.py'):   # Python 用 tokenize 精确去注释（正则会把字符串里的 # 当注释剥，剥坏了 ast 解析失败、依赖静默漏掉：10-26 link_gen.py 的 toolchain.env）
        try:
            toks, prev = [], tokenize.NEWLINE
            for x in tokenize.generate_tokens(io.StringIO(t).readline):
                if x.type == tokenize.COMMENT: continue
                if x.type == tokenize.STRING and prev in (tokenize.NEWLINE, tokenize.INDENT, tokenize.DEDENT, tokenize.NL):
                    x = x._replace(string='""')   # 文档串（单独成句的字符串）：说明文字里提到的文件不是依赖（10-26 dmsh_build.py 文档串提到 SpirvEdgePush.cs）
                toks.append(x)
                if x.type not in (tokenize.NL, tokenize.COMMENT): prev = x.type
            t = tokenize.untokenize(toks)
        except (tokenize.TokenError, SyntaxError, IndentationError): pass
    elif f.endswith(('.sh', '.env')):   # .env 是 shell 语法（10-26 toolchain.env 注释里提到 refpso.py）
        t = re.sub(r'^\s*#.*$', '', t, flags=re.M)                     # 整行注释
        t = re.sub(r'(?<=\s)#(?![!{]).*$', '', t, flags=re.M)          # 行尾「  # …」（不碰 ${#x}、#!）
    elif f.endswith(('.m', '.mm', '.h', '.c', '.inc', '.cs')):
        t = re.sub(r'(?s)/\*.*?\*/', '', t); t = re.sub(r'//.*$', '', t, flags=re.M)
    return t


def pyeval(f, src):   # Python 里按 __file__ 拼的路径：X = os.path.dirname(os.path.abspath(__file__))、os.path.join(X, '..')、sys.path.insert(0, X) → (拼出的路径, sys.path 目录)
    try: tree = ast.parse(src)
    except SyntaxError: return set(), [], set()
    env = {'__file__': f}; paths, dirs = set(), []
    def ev(n):
        if isinstance(n, ast.Constant) and isinstance(n.value, str): return n.value
        if isinstance(n, ast.Name): return env.get(n.id)
        if isinstance(n, ast.Call):
            fn = ast.unparse(n.func); a = [ev(x) for x in n.args]
            if any(x is None for x in a): return None
            if fn in ('os.path.dirname',) and len(a) == 1: return os.path.dirname(a[0])
            if fn in ('os.path.abspath', 'os.path.realpath', 'os.path.normpath') and len(a) == 1: return norm(a[0])
            if fn == 'os.path.join' and a: return norm(os.path.join(*a))
        return None
    for n in ast.walk(tree):
        if isinstance(n, ast.Assign) and len(n.targets) == 1 and isinstance(n.targets[0], ast.Name):
            v = ev(n.value)
            if v is not None: env[n.targets[0].id] = v
        elif isinstance(n, ast.Assign) and isinstance(n.targets[0], ast.Tuple) and isinstance(n.value, ast.Tuple):
            for k, x in zip(n.targets[0].elts, n.value.elts):
                v = ev(x)
                if isinstance(k, ast.Name) and v is not None: env[k.id] = v
    for n in ast.walk(tree):
        if isinstance(n, ast.Call):
            fn = ast.unparse(n.func)
            if fn == 'os.path.join':
                v = ev(n)
                if v: paths.add(v)
            elif fn == 'sys.path.insert' and len(n.args) == 2:
                v = ev(n.args[1])
                if v: dirs.append(v)
    mods = set()
    for n in ast.walk(tree):
        if isinstance(n, ast.Import): mods.update(x.name.split('.')[0] for x in n.names)
        elif isinstance(n, ast.ImportFrom) and n.module and not n.level: mods.add(n.module.split('.')[0])
    return paths, dirs, mods


def refs(f, t):
    out, d = set(), os.path.dirname(f); inc = [d]
    for m in re.finditer(r'([\w$\{\}./-]*/[\w./-]+\.' + EXT + r')\b', t):
        s = m.group(1)
        p = subst(s, f) if '$' in s else None
        if p: out.add(p)
        if '$' not in s:
            out.add(norm(s)); out.add(norm(os.path.join(d, s)))
    for m in re.finditer(r'-I\s*"?([^\s"]+)"?', t):
        p = subst(m.group(1), f) if '$' in m.group(1) else norm(m.group(1))
        if p: inc.append(p); INC.add(p)
    if f.endswith(('.m', '.mm', '.h', '.c', '.inc')):
        for m in re.finditer(r'#\s*(?:include|import)\s+"([^"]+)"', t):
            for x in inc + sorted(INC): out.add(norm(os.path.join(x, m.group(1))))
    if f.endswith(('.cs', '.py')):   # 跨语言调 libnxgal：名字即依赖
        for m in set(re.findall(r'\bnxgal_\w+', t)): out |= nxgal_defs().get(m, set())
    elif f.endswith(('.m', '.mm', '.c')):   # 跨文件 extern 声明的 libnxgal 导出（gal_tessxc.m → gal_vpull.m 的 nxgal_test_vpull_msl）
        for m in set(re.findall(r'\bextern\b[^;{}]*?\b(nxgal_\w+)\s*\(', t)): out |= nxgal_defs().get(m, set())
    if f.endswith('.py'):
        pp, pd, mods = pyeval(f, t); out |= pp
        ds = [d] + pd
        for mod in mods:
            for x in ds: out.add(norm(os.path.join(x, mod + '.py')))
        if re.search(r"sys\.path\.insert\([^)]*'\.\./games'", t) or re.search(r'sys\.path\.insert\([^)]*\.\./games', t):
            ds.append(norm(os.path.join(os.path.dirname(d), '../games')))
        for m in re.finditer(r'^\s*(?:from\s+([\w.]+)\s+import|import\s+([\w.]+(?:\s*,\s*[\w.]+)*))', t, re.M):
            for mod in (m.group(1) or m.group(2)).replace(' ', '').split(','):
                for x in ds: out.add(norm(os.path.join(x, mod.split('.')[0] + '.py')))
    if f.endswith('.sh'):
        for m in re.finditer(r'(?:^|[;&|(]\s*)(?:\.|source)\s+"?([^\s";)]+)', t, re.M):
            s = m.group(1)
            p = subst(s, f) if '$' in s else norm(os.path.join(d, s))
            if p: out.add(p)
    return {p for p in out if p in TR}


def closure(entries):
    for _ in range(2):   # 两遍：第一遍收齐各脚本的 -I，第二遍 #include 按全部 -I 找
        seen = once(entries)
    return seen


def once(entries):
    seen, todo = set(), [norm(e) for e in entries]
    while todo:
        f = todo.pop()
        if f in seen or f not in TR: continue
        seen.add(f)
        if f in STOP: continue
        try: t = strip(f, open(os.path.join(R, f), errors='replace').read())
        except OSError: continue
        todo += sorted(refs(f, t) - seen)
    return sorted(seen)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['--table']:
        STOP.update(l.split('\t')[1] for l in open(a[1]).read().split('\n') if l.startswith('外\t'))
        a = a[2:]
    for p in closure(a): print(p)
