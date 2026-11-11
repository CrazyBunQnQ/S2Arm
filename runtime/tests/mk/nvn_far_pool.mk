# native-test-nvn_far_pool：本单测的构建规则（runtime/build.mk 末尾按 runtime/tests/mk/*.mk 收进来；新单测只新增一个分片，不改 build.mk）
NATIVE_TEST_OWN += nvn_far_pool
ifeq ($(shell uname -s),Darwin)
# #450 远端别名区内存池影子不连续时建一段连续视图（vm_remap，同一批物理页），不再 FATAL；Metal 4 / 3 两路
$(BUILD)/tests/t_nvn_far_pool: graphics/nvn/tests/t_nvn_far_pool.m graphics/nvn/gfx/nvn/nvn_mtl.m $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN)
	$(CC) $(CFLAGS) -std=gnu11 -fobjc-arc -Wno-unused-function -Wno-unguarded-availability-new -Wno-unguarded-availability \
	  $(NATIVE_INC) -Igraphics/nvn/host -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/third_party/vulkan-headers/include \
	  -o $@ graphics/nvn/tests/t_nvn_far_pool.m $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN) \
	  -Wl,-rpath,@executable_path/nvn/widen $(NVN_TEST_WEAK) \
	  -framework Metal -framework Foundation -framework QuartzCore -framework CoreGraphics \
	  -framework ImageIO -framework UniformTypeIdentifiers -lc++
.PHONY: native-test-nvn_far_pool
native-test-nvn_far_pool: $(BUILD)/tests/t_nvn_far_pool
	NXGAL_BE=mtl4 $(BUILD)/tests/t_nvn_far_pool
	NXGAL_BE=mtl3 $(BUILD)/tests/t_nvn_far_pool
TEST_TARGETS += native-test-nvn_far_pool
endif
