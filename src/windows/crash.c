#include "crash.h"

#include <tlhelp32.h>

#include <stddef.h>

#if !defined(__i386__) && !defined(_M_IX86)
#error "The GTA IV client crash logger is intentionally 32-bit x86 only"
#endif

#define CRASH_LOG_CAPACITY 32768u
#define CRASH_TRACE_CAPACITY 64u
#define CRASH_MODULE_CAPACITY 128u
#define CRASH_STACK_SCAN_BYTES 8192u
#define CRASH_REPORT_LIMIT 32u
#define CRASH_STATUS_HEAP_CORRUPTION 0xC0000374ul
#define CRASH_STATUS_STACK_BUFFER_OVERRUN 0xC0000409ul
#define CRASH_STATUS_INVALID_CRUNTIME_PARAMETER 0xC0000417ul
#define CRASH_STATUS_ASSERTION_FAILURE 0xC0000420ul

struct crash_module {
	DWORD base;
	DWORD size;
	char path[MAX_PATH];
};

static PVOID vectored_handler;
static HANDLE crash_log = INVALID_HANDLE_VALUE;
static volatile LONG crash_handler_busy;
static char crash_text[CRASH_LOG_CAPACITY];
static DWORD crash_trace[CRASH_TRACE_CAPACITY];
static size_t crash_trace_count;
static struct crash_module crash_modules[CRASH_MODULE_CAPACITY];
static size_t crash_module_count;
static DWORD crash_reported_addresses[CRASH_REPORT_LIMIT];
static size_t crash_report_count;

static char *append_char(char *position, char value)
{
	size_t index = (size_t)(position - crash_text);

	if (index >= CRASH_LOG_CAPACITY - 1u) {
		crash_text[CRASH_LOG_CAPACITY - 1u] = '\0';
		return position;
	}
	crash_text[index++] = value;
	crash_text[index] = '\0';
	return &crash_text[index];
}

static char *append_text(char *position, const char *text)
{
	while (*text != '\0') {
		position = append_char(position, *text++);
	}
	return position;
}

static char *append_unsigned_decimal(char *position, DWORD value)
{
	char digits[10];
	size_t count = 0u;

	if (value == 0u) {
		return append_text(position, "0");
	}
	while (value != 0u && count < sizeof(digits)) {
		digits[count++] = (char)('0' + (value % 10u));
		value /= 10u;
	}
	while (count != 0u) {
		position = append_char(position, digits[--count]);
	}
	return position;
}

static char *append_fixed_decimal(char *position, DWORD value, size_t width)
{
	char digits[10];
	size_t count = 0u;

	if (value == 0u) {
		digits[count++] = '0';
	}
	while (value != 0u && count < sizeof(digits)) {
		digits[count++] = (char)('0' + (value % 10u));
		value /= 10u;
	}
	while (count < width && count < sizeof(digits)) {
		digits[count++] = '0';
	}
	while (count != 0u) {
		position = append_char(position, digits[--count]);
	}
	return position;
}

static char *append_hex(char *position, DWORD value)
{
	static const char hex[] = "0123456789ABCDEF";
	int shift;

	for (shift = 28; shift >= 0; shift -= 4) {
		position = append_char(position, hex[(value >> shift) & 0x0fu]);
	}
	return position;
}

static int readable_dword(DWORD address, DWORD *value)
{
	MEMORY_BASIC_INFORMATION information;
	DWORD base;
	DWORD region_size;

	if (address < 0x10000u) {
		return 0;
	}
	base = address & ~3u;
	if (VirtualQuery((const void *)(ULONG_PTR)base, &information,
		    sizeof(information)) == 0u) {
		return 0;
	}
	if (information.State != MEM_COMMIT ||
		(information.Protect & PAGE_GUARD) != 0u ||
		(information.Protect & PAGE_NOACCESS) != 0u ||
		information.RegionSize < sizeof(DWORD)) {
		return 0;
	}
	region_size = (DWORD)information.RegionSize;
	if (address < base || address - base > region_size - sizeof(DWORD)) {
		return 0;
	}
	*value = *(const DWORD *)(ULONG_PTR)address;
	return 1;
}

static void refresh_modules(void)
{
	HANDLE snapshot;
	MODULEENTRY32 entry;
	DWORD process_id = GetCurrentProcessId();
	DWORD index;

	crash_module_count = 0u;
	snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process_id);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return;
	}
	entry.dwSize = (DWORD)sizeof(entry);
	if (!Module32First(snapshot, &entry)) {
		(void)CloseHandle(snapshot);
		return;
	}
	do {
		if (crash_module_count >= CRASH_MODULE_CAPACITY) {
			break;
		}
		index = (DWORD)crash_module_count++;
		crash_modules[index].base = (DWORD)(ULONG_PTR)entry.modBaseAddr;
		crash_modules[index].size = (DWORD)entry.modBaseSize;
		(void)lstrcpynA(crash_modules[index].path, entry.szExePath, MAX_PATH);
		crash_modules[index].path[MAX_PATH - 1] = '\0';
	} while (Module32Next(snapshot, &entry));
	(void)CloseHandle(snapshot);
}

static int find_module(DWORD address, DWORD *base, const char **path)
{
	size_t index;

	for (index = 0u; index < crash_module_count; ++index) {
		if (address >= crash_modules[index].base &&
			address - crash_modules[index].base < crash_modules[index].size) {
			if (base != NULL) {
				*base = crash_modules[index].base;
			}
			if (path != NULL) {
				*path = crash_modules[index].path;
			}
			return 1;
		}
	}
	return 0;
}

static int trace_contains(DWORD address)
{
	size_t index;

	for (index = 0u; index < crash_trace_count; ++index) {
		if (crash_trace[index] == address) {
			return 1;
		}
	}
	return 0;
}

static char *append_trace(char *position, DWORD address)
{
	DWORD base;
	const char *path;

	if (crash_trace_count >= CRASH_TRACE_CAPACITY || trace_contains(address)) {
		return position;
	}
	crash_trace[crash_trace_count++] = address;
	position = append_text(position, "  #");
	position = append_unsigned_decimal(position,
		(DWORD)crash_trace_count - 1u);
	position = append_text(position, " 0x");
	position = append_hex(position, address);
	if (find_module(address, &base, &path)) {
		position = append_text(position, " ");
		position = append_text(position, path);
		position = append_text(position, "+0x");
		position = append_hex(position, address - base);
	} else {
		position = append_text(position, " [module unknown]");
	}
	return append_text(position, "\r\n");
}

static char *append_context(char *position, const CONTEXT *context)
{
	position = append_text(position, "context:\r\n");
	position = append_text(position, "  Eax=0x");
	position = append_hex(position, (DWORD)context->Eax);
	position = append_text(position, " Ebx=0x");
	position = append_hex(position, (DWORD)context->Ebx);
	position = append_text(position, " Ecx=0x");
	position = append_hex(position, (DWORD)context->Ecx);
	position = append_text(position, " Edx=0x");
	position = append_hex(position, (DWORD)context->Edx);
	position = append_text(position, "\r\n  Esi=0x");
	position = append_hex(position, (DWORD)context->Esi);
	position = append_text(position, " Edi=0x");
	position = append_hex(position, (DWORD)context->Edi);
	position = append_text(position, "\r\n  Esp=0x");
	position = append_hex(position, (DWORD)context->Esp);
	position = append_text(position, " Ebp=0x");
	position = append_hex(position, (DWORD)context->Ebp);
	position = append_text(position, " Eip=0x");
	position = append_hex(position, (DWORD)context->Eip);
	return append_text(position, "\r\n");
}

static char *append_exception_record(char *position,
	const EXCEPTION_RECORD *record)
{
	DWORD information0;
	DWORD information1;

	position = append_text(position, "exception:\r\n  code=0x");
	position = append_hex(position, (DWORD)record->ExceptionCode);
	position = append_text(position, " flags=0x");
	position = append_hex(position, (DWORD)record->ExceptionFlags);
	position = append_text(position, " address=0x");
	position = append_hex(position,
		(DWORD)(ULONG_PTR)record->ExceptionAddress);
	position = append_text(position, "\r\n");
	if (record->NumberParameters >= 2u) {
		information0 = (DWORD)(ULONG_PTR)record->ExceptionInformation[0];
		information1 = (DWORD)(ULONG_PTR)record->ExceptionInformation[1];
		position = append_text(position, "  information0=0x");
		position = append_hex(position, information0);
		position = append_text(position, " information1=0x");
		position = append_hex(position, information1);
		position = append_text(position, "\r\n");
	}
	return position;
}

static char *append_backtrace(char *position, const CONTEXT *context,
	DWORD exception_address)
{
	DWORD stack_address;
	DWORD frame_pointer;
	DWORD next_frame;
	DWORD return_address;
	DWORD offset;

	refresh_modules();
	position = append_text(position, "backtrace:\r\n");
	position = append_trace(position, exception_address);

	frame_pointer = (DWORD)context->Ebp;
	while (crash_trace_count < CRASH_TRACE_CAPACITY) {
		if (frame_pointer < 0x10000u ||
			frame_pointer < (DWORD)context->Esp ||
			frame_pointer - (DWORD)context->Esp > 0x00100000u ||
			!readable_dword(frame_pointer, &next_frame) ||
			!readable_dword(frame_pointer + 4u, &return_address)) {
			break;
		}
		if (return_address >= 0x10000u) {
			position = append_trace(position, return_address);
		}
		if (next_frame <= frame_pointer ||
			next_frame - frame_pointer > 0x00100000u) {
			break;
		}
		frame_pointer = next_frame;
	}

	stack_address = (DWORD)context->Esp;
	for (offset = 0u; offset < CRASH_STACK_SCAN_BYTES &&
		crash_trace_count < CRASH_TRACE_CAPACITY;
		offset += 4u) {
		if (!readable_dword(stack_address + offset, &return_address)) {
			break;
		}
		if (return_address >= 0x10000u &&
			find_module(return_address, NULL, NULL) &&
			!trace_contains(return_address)) {
			position = append_trace(position, return_address);
		}
	}
	if (crash_trace_count == 1u) {
		position = append_text(position,
			"  no additional frame-pointer or module "
			"stack candidates\r\n");
	}
	return position;
}

static char *build_crash_report(const EXCEPTION_POINTERS *exception_pointers)
{
	char *position = crash_text;
	SYSTEMTIME time;
	const EXCEPTION_RECORD *record = exception_pointers->ExceptionRecord;
	DWORD code = (DWORD)record->ExceptionCode;

	GetLocalTime(&time);
	position = append_text(position, "IVMP crash report\r\ntime=");
	position = append_fixed_decimal(position, (DWORD)time.wYear, 4u);
	position = append_text(position, "-");
	position = append_fixed_decimal(position, (DWORD)time.wMonth, 2u);
	position = append_text(position, "-");
	position = append_fixed_decimal(position, (DWORD)time.wDay, 2u);
	position = append_text(position, "T");
	position = append_fixed_decimal(position, (DWORD)time.wHour, 2u);
	position = append_text(position, ":");
	position = append_fixed_decimal(position, (DWORD)time.wMinute, 2u);
	position = append_text(position, ":");
	position = append_fixed_decimal(position, (DWORD)time.wSecond, 2u);
	position = append_text(position, ".");
	position = append_fixed_decimal(position, (DWORD)time.wMilliseconds, 3u);
	position = append_text(position, "\r\nprocess=0x");
	position = append_hex(position, GetCurrentProcessId());
	position = append_text(position, " thread=0x");
	position = append_hex(position, GetCurrentThreadId());
	position = append_text(position, "\r\n\r\n");
	position = append_exception_record(position, record);
	position = append_text(position, "\r\n");
	if (exception_pointers->ContextRecord != NULL) {
		position = append_context(position, exception_pointers->ContextRecord);
	}
	position = append_text(position, "\r\n");
	if (exception_pointers->ContextRecord != NULL) {
		position = append_backtrace(position, exception_pointers->ContextRecord,
			(DWORD)(ULONG_PTR)record->ExceptionAddress);
	}
	if (code == (DWORD)EXCEPTION_STACK_OVERFLOW) {
		position = append_text(position,
			"\r\nwarning: stack overflow; backtrace may be "
			"partial\r\n");
	}
	return append_text(position, "\r\n");
}

static int is_fatal_exception_code(DWORD code)
{
	switch (code) {
	case (DWORD)EXCEPTION_ACCESS_VIOLATION:
	case (DWORD)EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
	case (DWORD)EXCEPTION_DATATYPE_MISALIGNMENT:
	case (DWORD)EXCEPTION_FLT_DENORMAL_OPERAND:
	case (DWORD)EXCEPTION_FLT_DIVIDE_BY_ZERO:
	case (DWORD)EXCEPTION_FLT_INEXACT_RESULT:
	case (DWORD)EXCEPTION_FLT_INVALID_OPERATION:
	case (DWORD)EXCEPTION_FLT_OVERFLOW:
	case (DWORD)EXCEPTION_FLT_STACK_CHECK:
	case (DWORD)EXCEPTION_FLT_UNDERFLOW:
	case (DWORD)EXCEPTION_ILLEGAL_INSTRUCTION:
	case (DWORD)EXCEPTION_IN_PAGE_ERROR:
	case (DWORD)EXCEPTION_INT_DIVIDE_BY_ZERO:
	case (DWORD)EXCEPTION_INT_OVERFLOW:
	case (DWORD)EXCEPTION_INVALID_DISPOSITION:
	case (DWORD)EXCEPTION_NONCONTINUABLE_EXCEPTION:
	case (DWORD)EXCEPTION_PRIV_INSTRUCTION:
	case (DWORD)EXCEPTION_STACK_OVERFLOW:
	case CRASH_STATUS_HEAP_CORRUPTION:
	case CRASH_STATUS_STACK_BUFFER_OVERRUN:
	case CRASH_STATUS_INVALID_CRUNTIME_PARAMETER:
	case CRASH_STATUS_ASSERTION_FAILURE:
		return 1;
	default:
		return 0;
	}
}

static LONG WINAPI vectored_exception_handler(
	EXCEPTION_POINTERS *exception_pointers)
{
	char *position;
	DWORD written;
	DWORD code;

	if (exception_pointers == NULL ||
		exception_pointers->ExceptionRecord == NULL) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	code = (DWORD)exception_pointers->ExceptionRecord->ExceptionCode;
	if (!is_fatal_exception_code(code)) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	if (InterlockedCompareExchange(&crash_handler_busy, 1L, 0L) != 0L) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	{
		DWORD address = (DWORD)(ULONG_PTR)
					exception_pointers->ExceptionRecord->ExceptionAddress;
		size_t index;

		for (index = 0u; index < crash_report_count; ++index) {
			if (crash_reported_addresses[index] == address) {
				break;
			}
		}
		if (index < crash_report_count ||
			crash_report_count >= CRASH_REPORT_LIMIT) {
			(void)InterlockedExchange(&crash_handler_busy, 0L);
			return EXCEPTION_CONTINUE_SEARCH;
		}
		crash_reported_addresses[crash_report_count++] = address;
	}

	crash_trace_count = 0u;
	position = build_crash_report(exception_pointers);
	if (crash_log != INVALID_HANDLE_VALUE) {
		if (!WriteFile(crash_log, crash_text,
			    (DWORD)(position - crash_text), &written, NULL)) {
			OutputDebugStringA(crash_text);
		}
	} else {
		OutputDebugStringA(crash_text);
	}
	(void)InterlockedExchange(&crash_handler_busy, 0L);
	return EXCEPTION_CONTINUE_SEARCH;
}

static void truncate_file(const char *path)
{
	HANDLE file = CreateFileA(path, GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (file != INVALID_HANDLE_VALUE) {
		(void)CloseHandle(file);
	}
}

static void open_crash_log(HINSTANCE module)
{
	char path[MAX_PATH * 2];
	char temporary_path[MAX_PATH * 2];
	char *last_separator = NULL;
	DWORD length;
	DWORD index;

	if (crash_log != INVALID_HANDLE_VALUE) {
		return;
	}
	length = GetModuleFileNameA(module, path, (DWORD)sizeof(path));
	if (length == 0u || length >= sizeof(path)) {
		return;
	}
	for (index = 0u; index < length; ++index) {
		if (path[index] == '\\') {
			last_separator = &path[index];
		}
	}
	if (last_separator != NULL) {
		last_separator[1] = '\0';
		(void)lstrcatA(path, "ivmp-crash.log");
	} else {
		(void)lstrcpyA(path, "ivmp-crash.log");
	}
	truncate_file(path);
	crash_log = CreateFileA(path, FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (crash_log != INVALID_HANDLE_VALUE) {
		return;
	}
	if (GetTempPathA((DWORD)sizeof(temporary_path), temporary_path) != 0u) {
		(void)lstrcatA(temporary_path, "ivmp-crash.log");
		truncate_file(temporary_path);
		crash_log = CreateFileA(temporary_path, FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	}
}

void ivmp_crash_install(HINSTANCE module)
{
	char setting[8];

	if (vectored_handler != NULL) {
		return;
	}
	if (GetEnvironmentVariableA("IVMP_CRASH_LOG", setting,
		    (DWORD)sizeof(setting)) != 0u &&
		lstrcmpA(setting, "0") == 0) {
		return;
	}
	open_crash_log(module);
	vectored_handler = AddVectoredExceptionHandler(1u,
		vectored_exception_handler);
	if (vectored_handler == NULL && crash_log != INVALID_HANDLE_VALUE) {
		(void)CloseHandle(crash_log);
		crash_log = INVALID_HANDLE_VALUE;
	}
}

void ivmp_crash_uninstall(void)
{
	if (vectored_handler != NULL) {
		(void)RemoveVectoredExceptionHandler(vectored_handler);
		vectored_handler = NULL;
	}
	if (crash_log != INVALID_HANDLE_VALUE) {
		(void)CloseHandle(crash_log);
		crash_log = INVALID_HANDLE_VALUE;
	}
}
