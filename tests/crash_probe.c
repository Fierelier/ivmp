#include <windows.h>

#include "crash.h"

int main(void)
{
	ivmp_crash_install(GetModuleHandleA(NULL));
	RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0u,
		NULL);
	ivmp_crash_uninstall();
	return 1;
}
