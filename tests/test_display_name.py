#!/usr/bin/env python3
"""pack/display_name.py：装机 App 显示名只从游戏自带 control.nacp 取（11-04 维护者）。
期望值全部按夹具字节手写：每项标题 0x300（名字 0x200 + 发行商 0x100），14 简中、13 繁中、0 美式英语、1 英式英语、2 日语。"""
import contextlib
import importlib.util
import io
import os
import plistlib
import re
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('display_name', ROOT / 'pack/display_name.py')
dn = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dn)


def nacp(names, compressed=False):
    """{语言序号: 名字字节或字符串} → 0x4000 字节 control.nacp"""
    table = bytearray(0x6000 if compressed else 0x3000)
    for i, n in names.items():
        b = n.encode('utf-8') if isinstance(n, str) else n
        table[i * 0x300:i * 0x300 + len(b)] = b
    out = bytearray(0x4000)
    if compressed:
        c = zlib.compressobj(level=9, wbits=-15)
        payload = c.compress(bytes(table)) + c.flush()
        out[:2] = len(payload).to_bytes(2, 'little')
        out[2:2 + len(payload)] = payload
    else:
        out[:0x3000] = table
    return bytes(out)


class DisplayName(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory(prefix='dispname-')
        self.addCleanup(t.cleanup)
        self.base = Path(t.name)

    def put(self, data, rel='ctl/control.nacp'):
        p = self.base / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        return p

    def cli(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = dn.main([str(a) for a in args])
        return rc, out.getvalue().strip(), err.getvalue()

    def test_ascii_display_name(self):
        """侧载工具建 App ID 用的 ASCII 显示名（11-04 S0-5）：美式英语 → 英式英语 → 其余语言里第一个整个用拉丁字母写的标题；
        冒号、撇号等去掉、空白并一，重音字母去附加符；后缀里的中文去掉；没有拉丁标题 → 「S2Arm」（11-05：不拿中日韩标题删字凑名）"""
        cases = [  # (夹具各语言名字, 后缀, 手写期望)
            ({14: '集合啦！动物森友会', 0: 'Animal Crossing: New Horizons'}, '', 'Animal Crossing New Horizons S2Arm'),
            ({14: '泰拉瑞亚', 1: "Terraria's World"}, '实验', 'Terraria s World S2Arm'),
            ({14: '测试游戏'}, '', 'S2Arm'),
            ({14: '测试游戏', 2: 'ゼルダ BOTW'}, 'Exp 2', 'S2Arm Exp 2'),   # 日文标题夹英文：删字只剩残片，不用
            # 11-05 脑锻炼中文版真标题（只有韩 / 繁 / 简）：旧规则删字得「Nintendo Switch S2Arm」
            ({12: '매일매일 Nintendo Switch 두뇌 트레이닝', 13: '腦科學專家 川島隆太博士監修 大人的Nintendo Switch腦部鍛鍊',
              14: '脑科学专家 川岛隆太博士监修 大人的Nintendo Switch脑部锻炼'}, '', 'S2Arm'),
            ({14: '宝可梦', 3: 'Pokémon Épée'}, '', 'Pokemon Epee S2Arm'),   # 法语槽拉丁标题，重音去附加符
            ({14: '宝可梦', 2: 'ポケモン', 6: 'Pokémon'}, '', 'Pokemon S2Arm'),   # 非拉丁槽跳过，取后面的拉丁槽
        ]
        for names, suf, want in cases:
            with self.subTest(names=names):
                self.put(nacp(names))
                got = dn.ascii_display_name(self.base, suf)
                self.assertEqual(got, want); self.assertTrue(got.isascii())

    def test_localized_names(self):
        """各系统语言的桌面名（11-05 维护者「手机上 App 都叫游戏名 S2Arm」）：nacp 每个有标题的语言槽 → <lproj>：「<该语言标题> S2Arm[ 后缀]」；
        简 / 繁中文槽空时用中文优先选出的名字"""
        cases = [  # (夹具各语言名字, 后缀, 手写期望)
            ({12: '매일매일 Nintendo Switch 두뇌 트레이닝', 13: '腦科學專家 大人的Nintendo Switch腦部鍛鍊', 14: '脑科学专家 大人的Nintendo Switch脑部锻炼'}, '',
             {'ko': '매일매일 Nintendo Switch 두뇌 트레이닝 S2Arm', 'zh-Hant': '腦科學專家 大人的Nintendo Switch腦部鍛鍊 S2Arm',
              'zh-Hans': '脑科学专家 大人的Nintendo Switch脑部锻炼 S2Arm', 'en': '脑科学专家 大人的Nintendo Switch脑部锻炼 S2Arm'}),
            # DQ10（11-07）：只有日文标题 → 开发区域 en 也给日文名（英文系统桌面不再只显示「S2Arm」）
            ({2: 'ドラゴンクエストX　目覚めし五つの種族　オフライン'}, '',
             {'ja': 'ドラゴンクエストX　目覚めし五つの種族　オフライン S2Arm', 'en': 'ドラゴンクエストX　目覚めし五つの種族　オフライン S2Arm',
              'zh-Hans': 'ドラゴンクエストX　目覚めし五つの種族　オフライン S2Arm', 'zh-Hant': 'ドラゴンクエストX　目覚めし五つの種族　オフライン S2Arm'}),
            ({0: 'The Legend of Zelda', 2: 'ゼルダの伝説', 14: '塞尔达传说'}, '实验',
             {'en': 'The Legend of Zelda S2Arm 实验', 'ja': 'ゼルダの伝説 S2Arm 实验', 'zh-Hans': '塞尔达传说 S2Arm 实验', 'zh-Hant': '塞尔达传说 S2Arm 实验'}),
            ({0: 'Game', 1: 'Game UK', 15: 'Jogo'}, '',
             {'en': 'Game S2Arm', 'en-GB': 'Game UK S2Arm', 'pt-BR': 'Jogo S2Arm', 'zh-Hans': 'Game S2Arm', 'zh-Hant': 'Game S2Arm'}),
        ]
        for names, suf, want in cases:
            with self.subTest(names=names):
                self.put(nacp(names))
                self.assertEqual(dn.localized_names(self.base, suf), want)

    def test_language_priority(self):
        cases = [  # (夹具各语言名字, 手写期望)
            ({14: '集合啦！动物森友会', 13: '集合啦！動物森友會', 0: 'Animal Crossing: New Horizons'}, '集合啦！动物森友会 S2Arm'),
            ({13: '集合啦！動物森友會', 0: 'Animal Crossing: New Horizons'}, '集合啦！動物森友會 S2Arm'),
            ({0: 'Slay the Spire', 2: 'スレイ・ザ・スパイア'}, 'Slay the Spire S2Arm'),
            ({1: 'EA SPORTS FC 26'}, 'EA SPORTS FC 26 S2Arm'),
            ({2: 'ドラゴンクエストX'}, 'ドラゴンクエストX S2Arm'),
            ({12: '한국어', 3: 'Français'}, 'Français S2Arm'),   # 其余语言按序号：3 在 12 前
        ]
        for names, want in cases:
            with self.subTest(names=names):
                p = self.put(nacp(names))
                self.assertEqual(dn.display_name(p), want)

    def test_dir_forms_and_suffix(self):
        self.put(nacp({14: '空洞骑士 丝之歌', 0: 'Hollow Knight: Silksong'}))
        self.assertEqual(dn.display_name(self.base), '空洞骑士 丝之歌 S2Arm')                 # 解包目录 / 产物根：ctl/control.nacp
        self.assertEqual(dn.display_name(self.base, '实验'), '空洞骑士 丝之歌 S2Arm 实验')     # 后缀在 S2Arm 之后，游戏名在最前
        flat = self.base / 'flat'
        self.put(nacp({0: 'Raiders'}), 'flat/control.nacp')
        self.assertEqual(dn.display_name(flat), 'Raiders S2Arm')                              # 目录里直接放 control.nacp
        self.assertEqual(self.cli(self.base, '--suffix', '实验')[:2], (0, '空洞骑士 丝之歌 S2Arm 实验'))
        self.assertEqual(self.cli(self.base, '--title')[:2], (0, '空洞骑士 丝之歌'))

    def test_synth_nacp_from_ns2_dump(self):
        # ns2_dump 合成：16 项都写导出文件夹名（维护者 11-04「导不出来就用文件夹名」），照常读、不另判
        self.put(nacp({i: 'DonkeyKongBananza' for i in range(16)}))
        (self.base / 'ctl' / 'README.synth').write_text('合成', encoding='utf-8')
        self.assertEqual(dn.display_name(self.base), 'DonkeyKongBananza S2Arm')

    def test_compressed_32_languages(self):
        self.assertEqual(dn.display_name(self.put(nacp({14: '马力欧赛车8 豪华版', 20: 'x'}, compressed=True))), '马力欧赛车8 豪华版 S2Arm')
        self.assertEqual(dn.display_name(self.put(nacp({20: 'Only Late Slot'}, compressed=True))), 'Only Late Slot S2Arm')

    def test_bad_entry_skipped(self):
        self.assertEqual(dn.display_name(self.put(nacp({14: b'\xff\xfe', 0: 'Good Name'}))), 'Good Name S2Arm')
        self.assertEqual(dn.display_name(self.put(nacp({14: '  带空格  '}))), '带空格 S2Arm')

    def test_red_no_title(self):
        # 负对照：取不到 → 报红退出 1，不退回程序号 / 款名 / 对照表
        empty = self.base / 'empty'
        empty.mkdir()
        for path, why in ((empty, '没有 ctl/control.nacp'),
                          (self.put(nacp({}), 'z/ctl/control.nacp'), '全是空的'),
                          (self.put(b'nacp\n', 'short/ctl/control.nacp'), '只有 5 字节'),
                          (self.put(nacp({14: b'\xff', 0: b'\xc3'}), 'bad/ctl/control.nacp'), '不是合法 UTF-8'),
                          (self.base / 'nope.nacp', '不存在')):
            with self.subTest(path=path):
                with self.assertRaises(dn.NoTitle):
                    dn.display_name(path)
                rc, out, err = self.cli(path)
                self.assertEqual((rc, out), (1, ''))
                self.assertIn('【红】', err)
                self.assertIn(why, err)
        with self.assertRaises(dn.NoTitle):
            dn.compose('', '实验')


class Entrances(unittest.TestCase):
    """各出包入口都走 pack/display_name.py，不再查 pack/display_names.tsv、不再收任意显示名"""

    def test_ipa_sh(self):
        s = (ROOT / 'pack/ipa.sh').read_text(encoding='utf-8')
        self.assertNotIn('display_names.tsv', s)
        self.assertRegex(s, r'\nDN=\$\("\$\{S2A_PY:-python3\}" "\$H/display_name.py" "\$CUR" --suffix "\$DSUF"\) \|\|')
        self.assertIn(' dn=$DN', re.search(r'\nPK=\$\(ipa_pkg_key [^\n]*', s).group(0))   # 显示名进包键
        # 11-04 全外置：桌面显示名由组装 pack/ship_ext.py build 按同一后缀调 display_name() 写进 App（与上面 DN 同一份取名函数、同一后缀）；
        #   ipa.sh 把同一后缀与游戏名交给出包收尾 pack/ipa_tail.py（#1100 起收尾只有这一份实现），由它传给 ship_ext build、写 S2AGameTitle
        self.assertRegex(s, r'ipa_tail\.py" [^\n]*--suffix "\$DSUF"')
        self.assertRegex(s, r'--title "\$DT"')
        tail = (ROOT / 'pack/ipa_tail.py').read_text(encoding='utf-8')
        self.assertIn("'--suffix', a.suffix", tail)
        self.assertIn("('S2AGameTitle', a.title)", tail)
        self.assertIn('display_name(root, a.suffix)', (ROOT / 'pack/ship_ext.py').read_text(encoding='utf-8'))

    def test_oneclick(self):
        s = (ROOT / 'pack/oneclick/oneclick.sh').read_text(encoding='utf-8')
        self.assertNotIn('S2A_DISPLAY_NAME', s)
        self.assertIn('S2A_DISPLAY_SUFFIX= bash "$P/ipa.sh"', s)

    def test_android_label(self):
        s = (ROOT / 'android/tools/game_apk.py').read_text(encoding='utf-8')
        self.assertIn("label = display_name.display_name(os.path.join(pack, 'ctl', 'control.nacp'))", s)
        self.assertIn('<string name="game_name" translatable="false">{resource_string(label)}</string>', s)

if __name__ == '__main__':
    unittest.main(verbosity=2)
