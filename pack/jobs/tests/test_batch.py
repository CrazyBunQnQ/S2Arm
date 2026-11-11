import io
import contextlib
import json
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import batch  # noqa: E402

A, B, C = "0100000000010000", "0100000000020000", "0100000000030000"


class BatchRules(unittest.TestCase):
    """批量出包的批次规则（v0.2.4）：期望按规则手写，不跑出包"""

    def make(self):
        return batch.create([{"gameId": A, "name": "甲", "platforms": ["iphone", "mac"]},   # 勾的顺序不影响：先 Mac 再 iPhone（iPhone 收尾会清解包，放最后）
                             {"gameId": B, "name": "乙", "platforms": ["iphone"]},
                             {"gameId": C, "name": "丙", "platforms": []},                 # 没勾平台：不进批次
                             {"gameId": A, "name": "甲又一次", "platforms": ["iphone"]}],  # 同一款放进来两次只算一次
                            location="/tmp/批量包")

    def test_create_keeps_order_and_drops_unchecked(self):
        b = self.make()
        self.assertEqual([(i["gameId"], i["platforms"]) for i in b["items"]], [(A, ["mac", "iphone"]), (B, ["iphone"])])
        self.assertEqual(b["location"], "/tmp/批量包")
        with self.assertRaises(ValueError):
            batch.create([{"gameId": C, "platforms": []}])

    def test_platforms_of_one_game_run_back_to_back_then_next_game(self):
        b = self.make()
        steps = []
        while (n := batch.next_step(b)) is not None:
            steps.append((n["gameId"], n["platform"]))
            batch.mark(b, n["gameId"], n["platform"], "running")
            self.assertIsNone(batch.next_step(b))   # 正在出一个时不给下一个
            batch.mark(b, n["gameId"], n["platform"], "completed")
        self.assertEqual(steps, [(A, "mac"), (A, "iphone"), (B, "iphone")])
        self.assertEqual(batch.summary(b), {"total": 2, "completed": 2, "failed": 0, "cancelled": 0, "pending": 0, "done": True})

    def test_failure_does_not_stop_the_rest(self):
        b = self.make()
        batch.mark(b, A, "iphone", "failed", "签名失败", "signing_failed")
        self.assertEqual(batch.next_step(b), {"gameId": A, "platform": "mac"})
        batch.mark(b, A, "mac", "completed"); batch.mark(b, B, "iphone", "completed")
        s = batch.summary(b)
        self.assertEqual((s["completed"], s["failed"], s["done"]), (1, 1, True))   # 甲有平台失败 = 失败那一款
        batch.retry(b)
        self.assertEqual(batch.next_step(b), {"gameId": A, "platform": "iphone"})   # 重试只重出失败的那个平台

    def test_disk_full_pauses_until_retry(self):
        b = self.make()
        batch.mark(b, A, "mac", "failed", "磁盘空间不足", "disk_full")
        self.assertEqual(b["paused"], "disk_full")
        self.assertIsNone(batch.next_step(b))   # 后面的不接着出
        self.assertFalse(batch.summary(b)["done"])
        batch.retry(b, A)
        self.assertIsNone(b["paused"])
        self.assertEqual(batch.next_step(b), {"gameId": A, "platform": "mac"})

    def test_disk_full_holds_until_retry(self):   # 审计 P1-164：甲 Mac 磁盘满 → 甲 iPhone 不许再开；「继续」不解除暂停；只有重试解除
        b = self.make()
        batch.mark(b, A, "mac", "running"); batch.mark(b, A, "mac", "failed", "磁盘空间不足", "disk_full")
        with self.assertRaises(ValueError):
            batch.mark(b, A, "iphone", "running")
        self.assertEqual(b["items"][0]["results"]["iphone"], {"state": "pending"})
        batch.resume(b)
        self.assertEqual((b["paused"], batch.next_step(b)), ("disk_full", None))
        batch.retry(b)
        self.assertEqual((b["paused"], batch.next_step(b)), (None, {"gameId": A, "platform": "mac"}))

    def test_game_with_platform_left_is_not_finished(self):   # 审计 P1-168：甲 Mac 签名失败、iPhone 还没出 → 甲算没出完，整批没完
        b = self.make()
        batch.mark(b, A, "mac", "failed", "签名失败", "signing_failed")
        self.assertEqual(batch.next_step(b), {"gameId": A, "platform": "iphone"})
        self.assertEqual(batch.summary(b), {"total": 2, "completed": 0, "failed": 0, "cancelled": 0, "pending": 2, "done": False})
        batch.mark(b, A, "iphone", "running")
        self.assertEqual(batch.summary(b)["pending"], 2)   # 正在出的也算没出完
        batch.mark(b, A, "iphone", "completed"); batch.mark(b, B, "iphone", "completed")
        self.assertEqual(batch.summary(b), {"total": 2, "completed": 1, "failed": 1, "cancelled": 0, "pending": 0, "done": True})

    def test_cancel_keeps_finished_and_resume_continues(self):
        b = self.make()
        batch.mark(b, A, "iphone", "completed"); batch.mark(b, A, "mac", "running")
        batch.cancel(b)
        self.assertEqual([b["items"][0]["results"]["iphone"]["state"], b["items"][0]["results"]["mac"]["state"], b["items"][1]["results"]["iphone"]["state"]],
                         ["completed", "cancelled", "cancelled"])
        self.assertIsNone(batch.next_step(b))
        self.assertEqual(batch.summary(b)["cancelled"], 2)
        batch.resume(b)
        self.assertEqual(batch.next_step(b), {"gameId": A, "platform": "mac"})

    def test_recover_requeues_what_was_running_when_the_launcher_closed(self):
        b = self.make()
        batch.mark(b, A, "iphone", "completed"); batch.mark(b, A, "mac", "running")
        self.assertIsNone(batch.next_step(b))
        batch.recover(b)
        self.assertEqual(b["items"][0]["results"]["mac"], {"state": "pending", "interrupted": True})
        self.assertEqual(batch.next_step(b), {"gameId": A, "platform": "mac"})

    def test_bad_marks_are_rejected(self):
        b = self.make()
        for args in ((C, "iphone", "completed"), (B, "mac", "completed"), (A, "iphone", "done")):
            with self.subTest(args=args), self.assertRaises(ValueError):
                batch.mark(b, *args)


class BatchCli(unittest.TestCase):
    """命令行（两端启动器调的出口）：存档落在数据目录 batch.json，关掉再开读得回；show 不改存档"""

    def run_cli(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = batch.main(list(args) + ["--private-root", self.root])
        return rc, json.loads(out.getvalue())

    def setUp(self):
        self.root = tempfile.mkdtemp()

    def test_round_trip(self):
        self.assertEqual(self.run_cli("show"), (0, {}))
        rc, got = self.run_cli("create", "--json", json.dumps({"items": [{"gameId": A, "platforms": ["iphone"]}], "location": None}))
        self.assertEqual((rc, got["next"]), (0, {"gameId": A, "platform": "iphone"}))
        self.run_cli("mark", "--game", A, "--platform", "iphone", "--state", "running")
        before = Path(self.root, "batch.json").read_text(encoding="utf-8")
        rc, got = self.run_cli("show")
        self.assertIsNone(got["next"])   # 正在出：show 不把它当成没出
        self.assertEqual(Path(self.root, "batch.json").read_text(encoding="utf-8"), before)
        rc, got = self.run_cli("recover")
        self.assertEqual(got["next"], {"gameId": A, "platform": "iphone"})
        rc, got = self.run_cli("mark", "--game", A, "--platform", "iphone", "--state", "completed", "--output", "/x/S2Arm.ipa")
        self.assertEqual((got["summary"]["done"], got["batch"]["items"][0]["results"]["iphone"]["output"]), (True, "/x/S2Arm.ipa"))
        self.assertEqual(self.run_cli("clear"), (0, {}))
        self.assertFalse(Path(self.root, "batch.json").exists())

    def test_broken_file_is_no_batch(self):   # 审计 P2-280：某款缺 results / 状态不对 → 当没有批次，一行 JSON 报错，不抛异常
        for item in ({"gameId": A, "platforms": ["iphone"]}, {"gameId": A, "platforms": ["iphone"], "results": {"iphone": {"state": "done"}}}):
            with self.subTest(item=item):
                Path(self.root, "batch.json").write_text(json.dumps({"schema": 1, "items": [item]}), encoding="utf-8")
                self.assertEqual(self.run_cli("show"), (0, {}))
                rc, got = self.run_cli("mark", "--game", A, "--platform", "iphone", "--state", "running")
                self.assertEqual(rc, 1); self.assertIn("没有进行中的批次", got["error"])

    def test_create_does_not_overwrite_unfinished(self):   # 审计 P2-270：暂停 / 没出完的批次不被下一次 create 盖掉；出完了才能新建
        one = json.dumps({"items": [{"gameId": A, "platforms": ["iphone"]}]}); two = json.dumps({"items": [{"gameId": B, "platforms": ["iphone"]}]})
        self.run_cli("create", "--json", one)
        self.run_cli("mark", "--game", A, "--platform", "iphone", "--state", "failed", "--code", "disk_full")
        rc, got = self.run_cli("create", "--json", two)
        self.assertEqual(rc, 1); self.assertIn("上一批还没出完", got["error"])
        self.assertEqual(json.loads(Path(self.root, "batch.json").read_text(encoding="utf-8"))["items"][0]["gameId"], A)
        self.run_cli("retry"); self.run_cli("mark", "--game", A, "--platform", "iphone", "--state", "completed")
        rc, got = self.run_cli("create", "--json", two)
        self.assertEqual((rc, got["batch"]["items"][0]["gameId"]), (0, B))

    def test_held_lock_is_an_error_not_a_silent_write(self):   # 审计 P2-270：另一个进程正改着 batch.json → 等不到锁报错，不写
        self.run_cli("create", "--json", json.dumps({"items": [{"gameId": A, "platforms": ["iphone"]}]}))
        before = Path(self.root, "batch.json").read_text(encoding="utf-8")
        import filelock
        with open(Path(self.root, "batch.json.lock"), "a+") as f, mock.patch.object(batch.time, "sleep", lambda s: None):
            filelock.try_lock(f)
            rc, got = self.run_cli("mark", "--game", A, "--platform", "iphone", "--state", "running")
            filelock.unlock(f)
        self.assertEqual(rc, 1); self.assertIn("另一个进程", got["error"])
        self.assertEqual(Path(self.root, "batch.json").read_text(encoding="utf-8"), before)

    def test_errors_are_one_json_line(self):
        rc, got = self.run_cli("mark", "--game", A, "--platform", "iphone", "--state", "completed")
        self.assertEqual(rc, 1); self.assertIn("没有进行中的批次", got["error"])


if __name__ == "__main__":
    unittest.main()
