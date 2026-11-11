# native-test-nvn_alias_orphan：本单测的构建规则（runtime/build.mk 末尾按 runtime/tests/mk/*.mk 收进来；新单测只新增一个分片，不改 build.mk）
NATIVE_TEST_OWN += nvn_alias_orphan
ifeq ($(shell uname -s),Darwin)
# #357 无人引用的共享贴图：没被 GPU 写过的即放；GPU 写过的等存储整页被 CPU 写过再放；Metal 4 / 3 两路
$(BUILD)/tests/t_nvn_alias_orphan: graphics/nvn/tests/t_nvn_alias_orphan.m graphics/nvn/gfx/nvn/nvn_mtl.m $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN)
	$(CC) $(CFLAGS) -std=gnu11 -fobjc-arc -Wno-unused-function -Wno-unguarded-availability-new -Wno-unguarded-availability \
	  $(NATIVE_INC) -Igraphics/nvn/host -Igraphics/nvn/gfx/nvn -Igraphics/nvn/gfx/nvn/include -Igraphics/nvn/third_party/vulkan-headers/include \
	  -o $@ graphics/nvn/tests/t_nvn_alias_orphan.m $(NVN_TEST_HOST_OBJS) $(NVN_TEST_LIB) $(NVN_TEST_WIDEN) \
	  -Wl,-rpath,@executable_path/nvn/widen $(NVN_TEST_WEAK) \
	  -framework Metal -framework Foundation -framework QuartzCore -framework CoreGraphics \
	  -framework ImageIO -framework UniformTypeIdentifiers -lc++
.PHONY: native-test-nvn_alias_orphan
native-test-nvn_alias_orphan: $(BUILD)/tests/t_nvn_alias_orphan
	NXGAL_BE=mtl4 $(BUILD)/tests/t_nvn_alias_orphan
	NXGAL_BE=mtl3 $(BUILD)/tests/t_nvn_alias_orphan
TEST_TARGETS += native-test-nvn_alias_orphan
endif
