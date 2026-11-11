# SPDX-License-Identifier: Apache-2.0
"""游戏库共享 JSON 的编码（#514）：非中文 Windows（cp1252）/ 中文 Windows（cp936）上不靠 UTF-8 模式也要读写对。期望值都是夹具本身：
  ① core.atomic 在 ISO8859-1 / GBK 系统编码的子进程里（-I：PYTHONUTF8 不生效，同后台商店查询）写出的字节是 UTF-8，含 ™ / – / 中文
  ② 已被写坏的索引：cp1252 字节（™ –）、GBK 字节（中文 + –）——core.read 读出原对象，同时自愈：原字节备份到 <名>.<编码>.bak，
     文件改成 UTF-8、再读仍是原对象
  ③ 哪种编码都解不开 → 照样抛错（不返回空游戏库），文件原样不动
  ④ 后台商店查询子进程带 -X utf8
  ⑤ 真子进程、不打桩（同主作业 -I -X utf8，系统编码 GBK）：读 GBK 写坏的索引 → 原对象（v0.2.7 曾因 UTF-8 模式下取到的「系统编码」是 utf-8，
     落到 cp1252 写成乱码；中文 Windows 实机复现「Foo – 中文版」→「Foo ¨C ÖÐÎÄ°æ」）
  ⑥ 已被 v0.2.7 写成乱码的索引（catalog.json.cp1252.bak 是原 GBK 字节，之后库里又加了一款）：读一次 → 乱码串按备份逐串改回、后加的那款不动，
     备份改名为 <名>.<系统编码>.bak；再读不再改"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from jobcase import JOBS
import core

OBJ_1252 = {"games": [{"id": "0100000000010000", "name": "Foo™ – Bar"}]}
OBJ_GBK = {"games": [{"id": "0100000000010000", "name": "示例游戏 – 豪华版"}]}


class JsonEncodingCase(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp(prefix="s2a-json-enc "))
        self.addCleanup(lambda: __import__("shutil").rmtree(self.d, ignore_errors=True))

    def _atomic_in(self, lc, obj):
        out = self.d / f"{lc}.json"
        code = f"import sys; sys.path.insert(0, {str(JOBS)!r}); import core, json; core.atomic(sys.argv[1], json.loads(sys.argv[2]))"
        env = {k: v for k, v in os.environ.items() if not k.startswith(("LC_", "LANG", "PYTHON"))}
        env.update(LC_ALL=lc, LANG=lc)
        r = subprocess.run([sys.executable, "-I", "-B", "-c", code, str(out), json.dumps(obj)], env=env, capture_output=True, text=True, encoding="utf-8")
        return r, out

    def test_atomic_writes_utf8_whatever_locale(self):
        for lc, obj in (("en_US.ISO8859-1", OBJ_1252), ("zh_CN.GBK", OBJ_GBK)):
            with self.subTest(lc=lc):
                r, out = self._atomic_in(lc, obj)
                self.assertEqual(r.returncode, 0, r.stderr[-500:])
                self.assertEqual(json.loads(out.read_bytes().decode("utf-8")), obj)

    def _heal(self, raw, obj, system, enc):
        p = self.d / "catalog.json"
        p.write_bytes(raw)
        with patch.object(core, "system_encoding", lambda: system, ):
            self.assertEqual(core.read(p), obj)
        self.assertEqual(json.loads(p.read_bytes().decode("utf-8")), obj)    # 已改成 UTF-8
        self.assertEqual((self.d / f"catalog.json.{enc}.bak").read_bytes(), raw)   # 原字节留底
        self.assertEqual(core.read(p), obj)

    def test_read_heals_cp1252(self):
        self._heal(json.dumps(OBJ_1252, ensure_ascii=False).encode("cp1252"), OBJ_1252, "cp1252", "cp1252")

    def test_read_heals_cp1252_when_system_is_utf8(self):   # 系统编码读不了时再试 cp1252（#514 那份文件拿到别的机器上）
        self._heal(json.dumps(OBJ_1252, ensure_ascii=False).encode("cp1252"), OBJ_1252, "utf-8", "cp1252")

    def test_read_heals_gbk(self):
        self._heal(json.dumps(OBJ_GBK, ensure_ascii=False).encode("gbk"), OBJ_GBK, "cp936", "cp936")

    def test_undecodable_raises_and_keeps_file(self):
        p = self.d / "catalog.json"
        raw = b'{"name": "\x81\x81"}'   # UTF-8 / cp1252 都不认 0x81；GBK 的 0x81 0x81 是合法双字节但随后 json 照常——用 cp1252 当系统编码
        p.write_bytes(raw)
        with patch.object(core, "system_encoding", lambda: "cp1252", ), self.assertRaises(ValueError):
            core.read(p)
        self.assertEqual(p.read_bytes(), raw)
        self.assertFalse(list(self.d.glob("*.bak")))

    def _read_in_gbk_job(self, p):   # 同主作业：-I -X utf8，系统编码 GBK（中文 Windows 是 cp936）
        code = f"import sys, json; sys.path.insert(0, {str(JOBS)!r}); import core; print(json.dumps(core.read(sys.argv[1]), ensure_ascii=False))"
        env = {k: v for k, v in os.environ.items() if not k.startswith(("LC_", "LANG", "PYTHON"))}
        env.update(LC_ALL="zh_CN.GBK", LANG="zh_CN.GBK")
        r = subprocess.run([sys.executable, "-I", "-X", "utf8", "-B", "-c", code, str(p)], env=env, capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(r.returncode, 0, r.stderr[-500:])
        return json.loads(r.stdout)

    def test_gbk_heal_in_utf8_mode_job(self):
        p = self.d / "catalog.json"
        p.write_bytes(json.dumps(OBJ_GBK, ensure_ascii=False).encode("gbk"))
        self.assertEqual(self._read_in_gbk_job(p), OBJ_GBK)
        self.assertEqual(json.loads(p.read_bytes().decode("utf-8")), OBJ_GBK)

    def test_undo_v027_mojibake(self):
        p = self.d / "catalog.json"
        raw = json.dumps(OBJ_GBK, ensure_ascii=False).encode("gbk")
        (self.d / "catalog.json.cp1252.bak").write_bytes(raw)
        bad = json.loads(raw.decode("cp1252"))
        later = {"id": "0100000000020000", "name": "后来加的 Café"}
        p.write_text(json.dumps({"games": bad["games"] + [later]}, ensure_ascii=False), encoding="utf-8")
        want = {"games": OBJ_GBK["games"] + [later]}
        self.assertEqual(self._read_in_gbk_job(p), want)
        self.assertEqual(json.loads(p.read_bytes().decode("utf-8")), want)
        self.assertFalse((self.d / "catalog.json.cp1252.bak").exists())
        self.assertEqual([b.read_bytes() for b in self.d.glob("catalog.json.*.bak")], [raw])
        self.assertEqual(self._read_in_gbk_job(p), want)

    def test_store_meta_child_is_utf8_mode(self):
        import library
        src = Path(library.__file__).read_text(encoding="utf-8")
        self.assertIn('args = [sys.executable, "-I", "-X", "utf8", "-B", str(Path(__file__).with_name("store_meta_bg.py"))', src)


if __name__ == "__main__":
    unittest.main()
