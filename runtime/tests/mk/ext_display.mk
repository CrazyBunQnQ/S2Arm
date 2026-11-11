# native-test-ext_display：外接显示器原生全屏的接线（ios/extdisplay.m 登记场景配件、ios/app.m S2AGameVC、graphics/nvn/host_adapt.m 出图视图）
NATIVE_TEST_OWN += ext_display
ifeq ($(shell uname -s),Darwin)
.PHONY: native-test-ext_display
native-test-ext_display:
	@bash runtime/tests/t_ext_display.sh
	@$(IOS_CC) -fsyntax-only -fobjc-arc -Wunguarded-availability $(NATIVE_INC) -Iruntime/src -Iruntime/ns2pro/include ios/extdisplay.m ios/app.m ios/lan_permission.m ios/textinput.m ios/phone_motion.m
endif
