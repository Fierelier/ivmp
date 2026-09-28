#include <windows.h>
#include <string.h>
#include "entry_hook.h"

static unsigned char entry_orig_bytes[5];
static unsigned char *entry_point_addr = NULL;
static ivmp_deferred_init_fn entry_deferred_init = NULL;

static void ivmp_entry_trampoline(void)
{
	DWORD old_protect;

	VirtualProtect(entry_point_addr, sizeof(entry_orig_bytes), PAGE_EXECUTE_READWRITE, &old_protect);
	memcpy(entry_point_addr, entry_orig_bytes, sizeof(entry_orig_bytes));
	VirtualProtect(entry_point_addr, sizeof(entry_orig_bytes), old_protect, &old_protect);
	FlushInstructionCache(GetCurrentProcess(), entry_point_addr, sizeof(entry_orig_bytes));

	if (entry_deferred_init != NULL) {
		entry_deferred_init();
	}

	((void (*)(void))entry_point_addr)();
}

void ivmp_entry_hook_install(ivmp_deferred_init_fn deferred_init)
{
	unsigned char *base;
	IMAGE_DOS_HEADER *dos;
	IMAGE_NT_HEADERS *nt;
	unsigned char patch[5];
	DWORD old_protect;
	DWORD rel;

	if (entry_point_addr != NULL) {
		return;
	}

	base = (unsigned char *)GetModuleHandle(NULL);
	if (base == NULL) {
		return;
	}

	dos = (IMAGE_DOS_HEADER *)base;
	nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	entry_point_addr = base + nt->OptionalHeader.AddressOfEntryPoint;
	entry_deferred_init = deferred_init;

	memcpy(entry_orig_bytes, entry_point_addr, sizeof(entry_orig_bytes));

	rel = (DWORD)((unsigned char *)ivmp_entry_trampoline - (entry_point_addr + sizeof(patch)));
	patch[0] = 0xe9;
	memcpy(patch + 1, &rel, sizeof(rel));

	VirtualProtect(entry_point_addr, sizeof(patch), PAGE_EXECUTE_READWRITE, &old_protect);
	memcpy(entry_point_addr, patch, sizeof(patch));
	VirtualProtect(entry_point_addr, sizeof(patch), old_protect, &old_protect);
	FlushInstructionCache(GetCurrentProcess(), entry_point_addr, sizeof(patch));
}
