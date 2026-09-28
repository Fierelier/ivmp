#ifndef IVMP_GAME_PATCH_H
#define IVMP_GAME_PATCH_H

enum ivmp_game_patch_result {
	IVMP_GAME_PATCH_APPLIED,
	IVMP_GAME_PATCH_UNKNOWN_EXECUTABLE,
	IVMP_GAME_PATCH_PARTIAL,
	IVMP_GAME_PATCH_FAILED
};

enum ivmp_game_patch_result ivmp_game_patch_apply(void);

#endif
