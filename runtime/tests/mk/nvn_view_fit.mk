# native-test-nvn_view_fit：本单测的构建规则（runtime/build.mk 末尾按 runtime/tests/mk/*.mk 收进来；新单测只新增一个分片，不改 build.mk）
NATIVE_TEST_OWN += nvn_view_fit
ifeq ($(shell uname -s),Darwin)
# #199 Metal 建不出的贴图视图（1 层 2D 上的立方体视图等）按空槽处理，不让 Metal 断言；Metal 4 / 3 两路
$(BUILD)/tests/t_nvn_view_fit: graphics/nvn/tests/t_nvn_view_fit.m graphics/nvn/gfx/nvn/nvn_mtl.m $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN)
	$(CC) $(CFLAGS) -std=gnu11 -fobjc-arc -Wno-unused-function -Wno-unguarded-availability-new -Wno-unguarded-availability \
	  $(NATIVE_INC) -Igraphics/nvn/host -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/third_party/vulkan-headers/include \
	  -o $@ graphics/nvn/tests/t_nvn_view_fit.m $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN) \
	  -Wl,-rpath,@executable_path/nvn/widen $(NVN_TEST_WEAK) \
	  -framework Metal -framework Foundation -framework QuartzCore -framework CoreGraphics \
	  -framework ImageIO -framework UniformTypeIdentifiers -lc++
.PHONY: native-test-nvn_view_fit
native-test-nvn_view_fit: $(BUILD)/tests/t_nvn_view_fit
	NXGAL_BE=mtl4 $(BUILD)/tests/t_nvn_view_fit
	NXGAL_BE=mtl3 $(BUILD)/tests/t_nvn_view_fit
TEST_TARGETS += native-test-nvn_view_fit
endif
