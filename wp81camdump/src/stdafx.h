#pragma once

#include <SDKDDKVer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <wrl/client.h>
#include <shcore.h>
#include <mfobjects.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <windows.foundation.h>
#include <windows.storage.streams.h>
#include <windows.media.h>
#include <windows.media.capture.h>
#include <windows.media.devices.h>
#include <windows.media.mediaproperties.h>

#include "Win32Api.h"

// Functions hidden by WINAPI_FAMILY_PHONE_APP but exported by the phone (mincore.lib)
extern "C" {
	ROAPI HRESULT WINAPI RoInitialize(RO_INIT_TYPE initType);
	WINBASEAPI VOID WINAPI Sleep(DWORD dwMilliseconds);
	WINBASEAPI BOOL WINAPI VirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect);
	WINBASEAPI PVOID WINAPI AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
	WINBASEAPI BOOL WINAPI GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE *phModule);
	WINBASEAPI DWORD WINAPI GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize);

	// Registry (winreg.h isn't in the Windows Phone 8.1 SDK; exported by api-ms-win-core-registry, in mincore.lib)
	typedef ACCESS_MASK REGSAM;
	DECLSPEC_IMPORT LONG WINAPI RegCreateKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD Reserved, LPWSTR lpClass, DWORD dwOptions,
		REGSAM samDesired, LPSECURITY_ATTRIBUTES lpSecurityAttributes, HKEY *phkResult, LPDWORD lpdwDisposition);
	DECLSPEC_IMPORT LONG WINAPI RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, HKEY *phkResult);
	DECLSPEC_IMPORT LONG WINAPI RegSetValueExW(HKEY hKey, LPCWSTR lpValueName, DWORD Reserved, DWORD dwType,
		const BYTE *lpData, DWORD cbData);
	DECLSPEC_IMPORT LONG WINAPI RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType,
		LPBYTE lpData, LPDWORD lpcbData);
	DECLSPEC_IMPORT LONG WINAPI RegDeleteValueW(HKEY hKey, LPCWSTR lpValueName);
	DECLSPEC_IMPORT LONG WINAPI RegDeleteKeyExW(HKEY hKey, LPCWSTR lpSubKey, REGSAM samDesired, DWORD Reserved);
	DECLSPEC_IMPORT LONG WINAPI RegCloseKey(HKEY hKey);
}
#ifndef HKEY_LOCAL_MACHINE
#define HKEY_LOCAL_MACHINE ((HKEY)(ULONG_PTR)((LONG)0x80000002))
#endif
#ifndef REG_CREATED_NEW_KEY
#define REG_CREATED_NEW_KEY 0x00000001L
#endif
extern "C" {
}
