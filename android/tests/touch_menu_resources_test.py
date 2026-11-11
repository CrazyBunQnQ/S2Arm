#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Device-free touch menu resource/consumer contract, not an Android UI test."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
RES = ROOT / 'app/src/main/res'
LABELS = {
    'touch_menu_title': ('Touch controls', '触屏控制'),
    'touch_menu_edit': ('Edit button positions', '编辑按键位置'),
    'touch_menu_reset': ('Restore default layout', '恢复默认布局'),
    'touch_passthrough_on': ('Touch passthrough: on (tap to turn off)', '触摸透传：已开启（点按关闭）'),
    'touch_passthrough_off': ('Touch passthrough: off (tap to turn on)', '触摸透传：已关闭（点按开启）'),
    'touch_edit_save': ('Save', '保存'),
    'touch_edit_default': ('Default', '默认'),
    'touch_edit_cancel': ('Cancel', '取消'),
}


def strings(directory):
    nodes = ET.parse(RES / directory / 'strings.xml').getroot().findall('string')
    names = [node.attrib['name'] for node in nodes]
    if len(names) != len(set(names)):
        raise AssertionError('duplicate resource name')
    return {node.attrib['name']: ''.join(node.itertext()) for node in nodes}


class TouchMenuResourcesTest(unittest.TestCase):
    def test_default_labels_remain_english(self):
        values = strings('values')
        for key, (english, _) in LABELS.items():
            with self.subTest(key=key):
                self.assertEqual(values.get(key), english)

    def test_simplified_chinese_labels(self):
        values = strings('values-b+zh+Hans')
        self.assertLessEqual(set(LABELS), set(values))   # other screens may add their own translations
        for key, (_, chinese) in LABELS.items():
            with self.subTest(key=key):
                self.assertEqual(values[key], chinese)

    def test_menu_and_editor_use_resources(self):
        code = (ROOT / 'app/src/main/java/org/s2arm/android/MainActivity.java').read_text()
        menu = code.split('private void showTouchMenu() {', 1)[1].split('private void placeTouchEditBar()', 1)[0]
        self.assertEqual(set(re.findall(r'(?<!android\.)R\.string\.(\w+)', menu)), set(LABELS))
        self.assertIn('.setTitle(R.string.touch_menu_title)', menu)
        self.assertIn('labels.add(getString(R.string.touch_menu_edit))', menu)
        self.assertIn('labels.add(getString(R.string.touch_menu_reset))', menu)
        self.assertIn('on ? R.string.touch_passthrough_on : R.string.touch_passthrough_off', menu)
        self.assertIn('int[] names = {R.string.touch_edit_save, R.string.touch_edit_default, R.string.touch_edit_cancel}', menu)
        self.assertIn('button.setText(names[i])', menu)
        # Keep the platform-localized dialog cancel; no app preference or locale override.
        self.assertIn('.setNegativeButton(android.R.string.cancel, null)', menu)
        for english, _ in LABELS.values():
            self.assertNotIn('"' + english + '"', menu)


if __name__ == '__main__':
    unittest.main()
