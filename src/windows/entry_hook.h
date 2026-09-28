#ifndef IVMP_ENTRY_HOOK_H
#define IVMP_ENTRY_HOOK_H

typedef void (*ivmp_deferred_init_fn)(void);

void ivmp_entry_hook_install(ivmp_deferred_init_fn deferred_init);

#endif
