// SPDX-License-Identifier: BSD-2-Clause-Patent
/*
 * Fuxi HID link stubs.
 *
 * The full NativeHidInput archive still needs the original v9
 * IOHIDFamily prelink candidate, which fuxi does not carry yet. This keeps
 * the kernel linkable while the real offline HID source remains disabled.
 * The display path is independent of these symbols.
 */

void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");

void
native_hid_static_init(void)
{
}

void *
native_hid_input_allocate(void)
{
	return 0;
}

void *
native_hid_touch_allocate(void)
{
	return 0;
}
