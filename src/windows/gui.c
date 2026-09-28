#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <string.h>

#define GUI_TITLE "IVMP GUI"
#define GUI_CLASS "ivmp_gui"
#define GUI_TEXT_CAPACITY 1024
#define GUI_MARGIN 10
#define GUI_LABEL_WIDTH 80
#define GUI_EDIT_WIDTH 320
#define GUI_BUTTON_WIDTH 75
#define GUI_ROW_HEIGHT 22
#define GUI_HINT_HEIGHT 16
#define GUI_CLIENT_WIDTH (GUI_MARGIN * 3 + GUI_LABEL_WIDTH + GUI_EDIT_WIDTH + GUI_MARGIN + GUI_BUTTON_WIDTH)

enum gui_control {
	GUI_EXECUTABLE = 100,
	GUI_BROWSE,
	GUI_NICKNAME,
	GUI_ADDRESS,
	GUI_DEBUG_LOGS,
	GUI_DISABLE_HOOKS
};

static HFONT gui_font;
static char settings_path[MAX_PATH];

static HWND add_control(HWND parent, const char *class_name, const char *text, DWORD style, int x, int y, int width, int height, int id)
{
	HWND control = CreateWindowExA(strcmp(class_name, "EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, class_name, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);

	SendMessageA(control, WM_SETFONT, (WPARAM)gui_font, TRUE);
	return control;
}

static int text_height(HWND window, const char *text, int width)
{
	HDC context = GetDC(window);
	HGDIOBJ previous = SelectObject(context, gui_font);
	RECT bounds;

	SetRect(&bounds, 0, 0, width, 0);
	DrawTextA(context, text, -1, &bounds, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
	SelectObject(context, previous);
	ReleaseDC(window, context);
	return bounds.bottom > GUI_HINT_HEIGHT ? bounds.bottom : GUI_HINT_HEIGHT;
}

static int add_field(HWND window, int y, const char *label, int id, const char *hint)
{
	int edit_x = GUI_MARGIN * 2 + GUI_LABEL_WIDTH;

	add_control(window, "STATIC", label, SS_LEFT, GUI_MARGIN, y + 3, GUI_LABEL_WIDTH, GUI_HINT_HEIGHT, -1);
	add_control(window, "EDIT", "", WS_TABSTOP | ES_AUTOHSCROLL, edit_x, y, GUI_EDIT_WIDTH, GUI_ROW_HEIGHT, id);
	y += GUI_ROW_HEIGHT + 2;
	add_control(window, "STATIC", hint, SS_LEFT, GUI_MARGIN * 2, y, GUI_CLIENT_WIDTH - GUI_MARGIN * 3, text_height(window, hint, GUI_CLIENT_WIDTH - GUI_MARGIN * 3), -1);
	return y + text_height(window, hint, GUI_CLIENT_WIDTH - GUI_MARGIN * 3) + GUI_MARGIN;
}

static void load_setting(HWND window, int id, const char *key, const char *fallback)
{
	char value[GUI_TEXT_CAPACITY];

	GetPrivateProfileStringA("ivmp", key, fallback, value, sizeof(value), settings_path);
	SetDlgItemTextA(window, id, value);
}

static void load_settings(HWND window)
{
	char default_executable[MAX_PATH + 16];
	char *separator;

	lstrcpyA(default_executable, settings_path);
	separator = strrchr(default_executable, '\\');
	lstrcpyA(separator != NULL ? separator + 1 : default_executable, "LaunchGTAIV.exe");
	if (GetFileAttributesA(default_executable) == INVALID_FILE_ATTRIBUTES) {
		default_executable[0] = '\0';
	}
	load_setting(window, GUI_EXECUTABLE, "executable", default_executable);
	load_setting(window, GUI_NICKNAME, "nickname", "Player");
	load_setting(window, GUI_ADDRESS, "address", "127.0.0.1:42123");
	CheckDlgButton(window, GUI_DEBUG_LOGS, GetPrivateProfileIntA("ivmp", "debug_logs", 0, settings_path) ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(window, GUI_DISABLE_HOOKS, GetPrivateProfileIntA("ivmp", "disable_game_hooks", 0, settings_path) ? BST_CHECKED : BST_UNCHECKED);
}

static void save_settings(const char *executable, const char *nickname, const char *address, int debug_logs, int disable_hooks)
{
	WritePrivateProfileStringA("ivmp", "executable", executable, settings_path);
	WritePrivateProfileStringA("ivmp", "nickname", nickname, settings_path);
	WritePrivateProfileStringA("ivmp", "address", address, settings_path);
	WritePrivateProfileStringA("ivmp", "debug_logs", debug_logs ? "1" : "0", settings_path);
	WritePrivateProfileStringA("ivmp", "disable_game_hooks", disable_hooks ? "1" : "0", settings_path);
}

static void browse(HWND window)
{
	char path[MAX_PATH];
	OPENFILENAMEA dialog;

	GetDlgItemTextA(window, GUI_EXECUTABLE, path, sizeof(path));
	memset(&dialog, 0, sizeof(dialog));
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = window;
	dialog.lpstrFilter = "Programs (*.exe;*.bat;*.cmd;*.lnk)\0*.exe;*.bat;*.cmd;*.lnk\0All files (*.*)\0*.*\0";
	dialog.lpstrFile = path;
	dialog.nMaxFile = sizeof(path);
	dialog.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
	if (GetOpenFileNameA(&dialog)) {
		SetDlgItemTextA(window, GUI_EXECUTABLE, path);
	}
}

static int fail(HWND window, int id, const char *message)
{
	MessageBoxA(window, message, GUI_TITLE, MB_OK | MB_ICONWARNING);
	SetFocus(GetDlgItem(window, id));
	return 0;
}

static int connect_and_launch(HWND window)
{
	char executable[MAX_PATH];
	char directory[MAX_PATH];
	char nickname[GUI_TEXT_CAPACITY];
	char address[GUI_TEXT_CAPACITY];
	char *separator;
	int debug_logs = IsDlgButtonChecked(window, GUI_DEBUG_LOGS) == BST_CHECKED;
	int disable_hooks = IsDlgButtonChecked(window, GUI_DISABLE_HOOKS) == BST_CHECKED;
	SHELLEXECUTEINFOA launch;

	GetDlgItemTextA(window, GUI_EXECUTABLE, executable, sizeof(executable));
	GetDlgItemTextA(window, GUI_NICKNAME, nickname, sizeof(nickname));
	GetDlgItemTextA(window, GUI_ADDRESS, address, sizeof(address));
	if (GetFileAttributesA(executable) == INVALID_FILE_ATTRIBUTES) {
		return fail(window, GUI_EXECUTABLE, "Pick the game executable to launch.");
	}
	if (nickname[0] == '\0') {
		return fail(window, GUI_NICKNAME, "Enter a nickname.");
	}
	separator = strrchr(address, ':');
	if (separator == NULL || separator == address || separator[1] == '\0') {
		return fail(window, GUI_ADDRESS, "Enter the server address as host:port, for example 127.0.0.1:42123.");
	}
	save_settings(executable, nickname, address, debug_logs, disable_hooks);

	SetEnvironmentVariableA("IVMP_SERVER", address);
	SetEnvironmentVariableA("IVMP_NICKNAME", nickname);
	SetEnvironmentVariableA("IVMP_REDIRECT_ALL", "1");
	SetEnvironmentVariableA("IVMP_TRACE", debug_logs ? "1" : NULL);
	SetEnvironmentVariableA("IVMP_GAME_PATCHES", disable_hooks ? "0" : NULL);

	if (GetFullPathNameA(executable, sizeof(directory), directory, &separator) == 0 || separator == NULL) {
		return fail(window, GUI_EXECUTABLE, "Pick the game executable to launch.");
	}
	lstrcpynA(executable, directory, sizeof(executable));
	*separator = '\0';
	if (!SetCurrentDirectoryA(directory)) {
		return fail(window, GUI_EXECUTABLE, "The executable's folder could not be opened.");
	}
	memset(&launch, 0, sizeof(launch));
	launch.cbSize = sizeof(launch);
	launch.fMask = SEE_MASK_NOASYNC;
	launch.hwnd = window;
	launch.lpVerb = "open";
	launch.lpFile = executable;
	launch.lpDirectory = directory;
	launch.nShow = SW_SHOWNORMAL;
	if (!ShellExecuteExA(&launch)) {
		return fail(window, GUI_EXECUTABLE, "The executable could not be started.");
	}
	return 1;
}

static void create_controls(HWND window)
{
	int y = GUI_MARGIN;
	int browse_x = GUI_MARGIN * 3 + GUI_LABEL_WIDTH + GUI_EDIT_WIDTH;

	add_control(window, "BUTTON", "Browse", WS_TABSTOP | BS_PUSHBUTTON, browse_x, y, GUI_BUTTON_WIDTH, GUI_ROW_HEIGHT, GUI_BROWSE);
	y = add_field(window, y, "Executable", GUI_EXECUTABLE, "Your game executable. Usually LaunchGTAIV.exe, but if you prefer a different one, you may use it.");
	y = add_field(window, y, "Nickname", GUI_NICKNAME, "This name will show ingame.");
	y = add_field(window, y, "Address", GUI_ADDRESS, "The IP address and port of the server. Example: 127.0.0.1:42123");
	add_control(window, "BUTTON", "Enable debug logs", WS_TABSTOP | BS_AUTOCHECKBOX, GUI_MARGIN, y, 200, GUI_ROW_HEIGHT - 4, GUI_DEBUG_LOGS);
	y += GUI_ROW_HEIGHT;
	add_control(window, "BUTTON", "Disable game hooks", WS_TABSTOP | BS_AUTOCHECKBOX, GUI_MARGIN, y, 200, GUI_ROW_HEIGHT - 4, GUI_DISABLE_HOOKS);
	y += GUI_ROW_HEIGHT + GUI_MARGIN;
	add_control(window, "BUTTON", "Launch", WS_TABSTOP | BS_DEFPUSHBUTTON, (GUI_CLIENT_WIDTH - GUI_BUTTON_WIDTH) / 2, y, GUI_BUTTON_WIDTH, GUI_ROW_HEIGHT + 2, IDOK);
	SetWindowLongA(window, GWL_USERDATA, y + GUI_ROW_HEIGHT + 2 + GUI_MARGIN);
	load_settings(window);
	SetFocus(GetDlgItem(window, GUI_NICKNAME));
}

static LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
	switch (message) {
	case WM_CREATE:
		create_controls(window);
		return 0;
	case WM_COMMAND:
		if (LOWORD(w_param) == GUI_BROWSE) {
			browse(window);
		} else if (LOWORD(w_param) == IDOK && connect_and_launch(window)) {
			DestroyWindow(window);
		} else if (LOWORD(w_param) == IDCANCEL) {
			DestroyWindow(window);
		}
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	default:
		return DefWindowProcA(window, message, w_param, l_param);
	}
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show)
{
	WNDCLASSA window_class;
	NONCLIENTMETRICSA metrics;
	RECT bounds;
	HWND window;
	MSG message;
	char *extension;
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

	(void)previous;
	(void)command_line;
	GetModuleFileNameA(NULL, settings_path, sizeof(settings_path) - 4);
	extension = strrchr(settings_path, '.');
	lstrcpyA(extension != NULL ? extension : settings_path + lstrlenA(settings_path), ".ini");

	metrics.cbSize = sizeof(metrics);
	gui_font = SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) ? CreateFontIndirectA(&metrics.lfMessageFont) : (HFONT)GetStockObject(DEFAULT_GUI_FONT);

	memset(&window_class, 0, sizeof(window_class));
	window_class.lpfnWndProc = window_procedure;
	window_class.hInstance = instance;
	window_class.hIcon = LoadIconA(NULL, (LPCSTR)IDI_APPLICATION);
	window_class.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
	window_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	window_class.lpszClassName = GUI_CLASS;
	RegisterClassA(&window_class);

	window = CreateWindowExA(0, GUI_CLASS, GUI_TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT, GUI_CLIENT_WIDTH, 100, NULL, NULL, instance, NULL);
	if (window == NULL) {
		return 1;
	}
	SetRect(&bounds, 0, 0, GUI_CLIENT_WIDTH, (int)GetWindowLongA(window, GWL_USERDATA));
	AdjustWindowRect(&bounds, style, FALSE);
	SetWindowPos(window, NULL, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOMOVE | SWP_NOZORDER);
	ShowWindow(window, show);

	while (GetMessageA(&message, NULL, 0, 0) > 0) {
		if (!IsDialogMessageA(window, &message)) {
			TranslateMessage(&message);
			DispatchMessageA(&message);
		}
	}
	return (int)message.wParam;
}
