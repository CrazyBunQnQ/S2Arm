# 按游戏自定义按键位置：布局按游戏目录存、外观按 App 存，拖动编辑生成的方案覆盖保存（vpad_layout.m）；
#   Mac 上另用 iOS SDK 对 vpad.m / vpad_settings.m 做语法与类型检查（编辑模式、描线配色都在 vpad.m，Foundation 单测编不到）
NATIVE_TEST_OWN += vpad_custom
.PHONY: native-test-vpad_custom
native-test-vpad_custom:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=gnu11 -fobjc-arc -framework Foundation -framework CoreGraphics -o $(BUILD)/tests/t_vpad_custom runtime/tests/t_vpad_custom.m ios/vpad_layout.m
	$(BUILD)/tests/t_vpad_custom
ifeq ($(shell uname -s),Darwin)
	@$(IOS_CC) -fsyntax-only -fobjc-arc $(NATIVE_INC) -Iruntime/src -Iruntime/ns2pro/include ios/vpad.m ios/vpad_settings.m ios/vpad_shell.m ios/settings.m ios/vpad_cheats.m
endif
TEST_TARGETS += native-test-vpad_custom

# 游戏库调整按键位置的宿主接口（vpad_shell.m，s2a_shell_vpad_*）
NATIVE_TEST_OWN += vpad_shell
.PHONY: native-test-vpad_shell
native-test-vpad_shell:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=gnu11 -fobjc-arc -framework Foundation -framework CoreGraphics -o $(BUILD)/tests/t_vpad_shell runtime/tests/t_vpad_shell.m ios/vpad_shell.m ios/vpad_layout.m
	$(BUILD)/tests/t_vpad_shell
TEST_TARGETS += native-test-vpad_shell

# 2D 布局（#268）：内置方案几何、0 根摇杆可存、八方向十字键判定
NATIVE_TEST_OWN += vpad_2d
.PHONY: native-test-vpad_2d
native-test-vpad_2d:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=gnu11 -fobjc-arc -framework Foundation -framework CoreGraphics -o $(BUILD)/tests/t_vpad_2d runtime/tests/t_vpad_2d.m ios/vpad_layout.m
	$(BUILD)/tests/t_vpad_2d
TEST_TARGETS += native-test-vpad_2d

# 金手指文本解析（ios/cheats_parse.m，#458）：只依赖 Foundation，Mac 上直接编直接跑
NATIVE_TEST_OWN += cheats_parse
.PHONY: native-test-cheats_parse
native-test-cheats_parse:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CFLAGS) -std=gnu11 -fobjc-arc -framework Foundation -o $(BUILD)/tests/t_cheats_parse runtime/tests/t_cheats_parse.m ios/cheats_parse.m
	$(BUILD)/tests/t_cheats_parse
TEST_TARGETS += native-test-cheats_parse
