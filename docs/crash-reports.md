# Crash reports

`xlive.dll` installs a process-local vectored exception
handler. The handler records first-chance exceptions other than breakpoint and
single-step exceptions, including exceptions that lead to termination. Some
handled game exceptions may therefore also appear in the log.

## Log location

The handler tries to open:

```text
ivmp-crash.log
```

beside `xlive.dll`. If that directory is not writable,
the handler falls back to the current user's temporary directory.

The file is emptied when the game starts, so it only holds reports from the
latest run. Each report contains:

- local timestamp, process ID, and thread ID;
- exception code, flags, address, and available exception information;
- x86 register context (`Eax`, `Ebx`, `Ecx`, `Edx`, `Esi`, `Edi`, `Esp`,
  `Ebp`, `Eip`);
- a backtrace containing the exception address, frame-pointer return addresses,
  and additional executable-looking stack addresses;
- module path and relative offset for addresses that can be resolved.

The stack scan is best effort. A release build may not have complete frame
pointers, and a stack-overflow report may only contain a partial trace. The
project builds the Windows client with frame pointers enabled to improve the
normal case.

## Collecting a report

Send the complete `ivmp-crash.log` together with:

1. The exact GTA IV executable version/build.
2. Whether the game was vanilla or modified.
3. The launcher and relay versions.
4. The server address with any credentials removed.
5. The steps immediately before the crash.
6. The corresponding `ivmp.log`, if tracing was enabled.

The log may contain local file paths and memory addresses. Review it before
posting publicly. No minidump or memory dump is currently generated.

The handler writes at most 32 reports per run and only one per faulting
address. Each report lists the modules loaded at the time of the exception.
