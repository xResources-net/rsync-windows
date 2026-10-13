/* Test helper standing in for "ssh HOST CMD...": ignores HOST (and any
 * leading -x style options), joins CMD the way ssh does and runs it under
 * MSYS2 bash, so the "remote" end is MSYS2's own rsync on this machine.
 * Usage: rsync -e /path/to/fakessh.exe ... localhost:/cygdrive/... */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
	static wchar_t cmd[32768], joined[30000];
	const wchar_t *bash = _wgetenv(L"FAKESSH_BASH");
	STARTUPINFOW si = { sizeof si };
	PROCESS_INFORMATION pi;
	DWORD code = 255;
	int i = 1;

	while (i < argc && argv[i][0] == L'-') {
		if (!wcscmp(argv[i], L"-l") || !wcscmp(argv[i], L"-p"))
			i++;
		i++;
	}
	i++; /* host */
	for (; i < argc; i++) {
		if (joined[0])
			wcscat(joined, L" ");
		wcscat(joined, argv[i]);
	}
	if (!bash)
		bash = L"C:/PortableApps/msys64/usr/bin/bash.exe";
	swprintf(cmd, 32768, L"\"%ls\" -c \"export PATH=/usr/bin:$PATH; exec %ls\"", bash, joined);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		fwprintf(stderr, L"fakessh: CreateProcess failed %lu\n", GetLastError());
		return 255;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, &code);
	return (int)code;
}
