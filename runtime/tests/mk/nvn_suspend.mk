# native-test-nvn_suspend：iOS 后台通知只同步关图形入口，不在 UIKit 主线程等在途提交。
NATIVE_TEST_OWN += nvn_suspend
ifeq ($(shell uname -s),Darwin)
$(BUILD)/tests/t_nvn_suspend: graphics/nvn/tests/t_nvn_suspend.c graphics/nvn/host/dm.h $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN)
	$(CC) $(CFLAGS) $(NATIVE_INC) -Igraphics/nvn -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include \
	  -o $@ graphics/nvn/tests/t_nvn_suspend.c $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN) \
	  -Wl,-rpath,@executable_path/nvn/widen $(NVN_TEST_WEAK) \
	  -framework Metal -framework Foundation -framework QuartzCore -framework CoreGraphics \
	  -framework ImageIO -framework UniformTypeIdentifiers -lc++
.PHONY: native-test-nvn_suspend
native-test-nvn_suspend: $(BUILD)/tests/t_nvn_suspend
	$(BUILD)/tests/t_nvn_suspend
TEST_TARGETS += native-test-nvn_suspend
endif
