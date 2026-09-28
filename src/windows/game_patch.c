#include <windows.h>
#include <string.h>

#include "game_patch.h"

#define GTAIV_104_TIME_DATE_STAMP 0x499dc616u
#define GTAIV_107_TIME_DATE_STAMP 0x4bd9efbeu
#define PATCH_MAX_LENGTH 5u

typedef unsigned char(__cdecl *social_club_name_fn)(char *name);

struct code_patch {
	DWORD time_date_stamp;
	DWORD rva;
	unsigned char expected[14];
	unsigned char expected_mask[14];
	unsigned char replacement[3];
	social_club_name_fn target;
};

static unsigned char __cdecl social_club_signed_in(char *name)
{
	char nickname[64];
	DWORD length;

	if (name == NULL) {
		return 1u;
	}
	length = GetEnvironmentVariableA("IVMP_NICKNAME", nickname, (DWORD)sizeof(nickname));
	if (length == 0u || length >= sizeof(nickname)) {
		(void)lstrcpynA(nickname, "Player", (int)sizeof(nickname));
	}
	(void)lstrcpynA(name, nickname, (int)sizeof(nickname));
	return 1u;
}

#define SOCIAL_CLUB_PROLOGUE \
	{ \
		0x83, 0xec, 0x44, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x33, 0xc4, 0x89, 0x44, 0x24, 0x40 \
	}
#define ABSOLUTE_ADDRESS_MASK \
	{ \
		1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1 \
	}

static const struct code_patch game_patches[] = {
	{ GTAIV_107_TIME_DATE_STAMP, 0x00003870u,
		{ 0x83, 0xec, 0x24, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x8b, 0x48, 0x14, 0x53, 0x55, 0x56 },
		ABSOLUTE_ADDRESS_MASK,
		{ 0x32, 0xc0, 0xc3 },
		NULL },
	{ GTAIV_107_TIME_DATE_STAMP, 0x00060050u,
		{ 0x83, 0xec, 0x7c, 0x32, 0xc0, 0x38, 0x05, 0x00, 0x00, 0x00, 0x00, 0x56, 0x57, 0x74 },
		{ 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1 },
		{ 0xb0, 0x01, 0xc3 },
		NULL },
	{ GTAIV_107_TIME_DATE_STAMP, 0x0006043cu,
		{ 0x72, 0x5f, 0x0f, 0xb7, 0x05, 0x00, 0x00, 0x00, 0x00, 0xf3, 0x0f, 0x10, 0x0d, 0x00 },
		{ 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0 },
		{ 0xeb, 0x5f, 0x0f },
		NULL },
	{ GTAIV_104_TIME_DATE_STAMP, 0x002b234cu,
		{ 0x72, 0x5a, 0x0f, 0xb7, 0x05, 0x00, 0x00, 0x00, 0x00, 0xf3, 0x0f, 0x10, 0x0d, 0x00 },
		{ 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0 },
		{ 0xeb, 0x5a, 0x0f },
		NULL },
	{ GTAIV_107_TIME_DATE_STAMP, 0x003b05c0u,
		SOCIAL_CLUB_PROLOGUE,
		ABSOLUTE_ADDRESS_MASK,
		{ 0 },
		social_club_signed_in },
	{ GTAIV_104_TIME_DATE_STAMP, 0x00570ee0u,
		SOCIAL_CLUB_PROLOGUE,
		ABSOLUTE_ADDRESS_MASK,
		{ 0 },
		social_club_signed_in }
};

static IMAGE_NT_HEADERS *game_nt_headers(unsigned char *base)
{
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
	IMAGE_NT_HEADERS *nt;

	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
		return NULL;
	}
	nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	return nt->Signature == IMAGE_NT_SIGNATURE ? nt : NULL;
}

static int code_matches(const unsigned char *code, const struct code_patch *patch)
{
	size_t index;

	for (index = 0u; index < sizeof(patch->expected); ++index) {
		if (patch->expected_mask[index] && code[index] != patch->expected[index]) {
			return 0;
		}
	}
	return 1;
}

static size_t build_replacement(unsigned char *bytes, const unsigned char *code, const struct code_patch *patch)
{
	DWORD relative;

	if (patch->target == NULL) {
		memcpy(bytes, patch->replacement, sizeof(patch->replacement));
		return sizeof(patch->replacement);
	}
	relative = (DWORD)((const unsigned char *)patch->target - (code + 5));
	bytes[0] = 0xe9u;
	memcpy(bytes + 1, &relative, sizeof(relative));
	return 5u;
}

static int write_code(unsigned char *code, const unsigned char *bytes, size_t length)
{
	DWORD old_protect;
	DWORD ignored;

	if (!VirtualProtect(code, length, PAGE_EXECUTE_READWRITE, &old_protect)) {
		return 0;
	}
	memcpy(code, bytes, length);
	(void)VirtualProtect(code, length, old_protect, &ignored);
	(void)FlushInstructionCache(GetCurrentProcess(), code, length);
	return 1;
}

enum ivmp_game_patch_result ivmp_game_patch_apply(void)
{
	unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS *nt;
	size_t index;
	size_t known = 0u;
	size_t applied = 0u;

	if (base == NULL) {
		return IVMP_GAME_PATCH_FAILED;
	}
	nt = game_nt_headers(base);
	if (nt == NULL) {
		return IVMP_GAME_PATCH_UNKNOWN_EXECUTABLE;
	}
	for (index = 0u; index < sizeof(game_patches) / sizeof(game_patches[0]); ++index) {
		const struct code_patch *patch = &game_patches[index];
		unsigned char bytes[PATCH_MAX_LENGTH];
		unsigned char *code;
		size_t length;

		if (nt->FileHeader.TimeDateStamp != patch->time_date_stamp || patch->rva + sizeof(patch->expected) > nt->OptionalHeader.SizeOfImage) {
			continue;
		}
		++known;
		code = base + patch->rva;
		length = build_replacement(bytes, code, patch);
		if (memcmp(code, bytes, length) == 0 || (code_matches(code, patch) && write_code(code, bytes, length))) {
			++applied;
		}
	}
	if (known == 0u) {
		return IVMP_GAME_PATCH_UNKNOWN_EXECUTABLE;
	}
	if (applied == known) {
		return IVMP_GAME_PATCH_APPLIED;
	}
	return applied == 0u ? IVMP_GAME_PATCH_FAILED : IVMP_GAME_PATCH_PARTIAL;
}
