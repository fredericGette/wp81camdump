#pragma once

#include <SDKDDKVer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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
}
