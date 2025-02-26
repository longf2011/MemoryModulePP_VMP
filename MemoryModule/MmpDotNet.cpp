#include "stdafx.h"
#include <3rdparty/Detours/detours.h>

typedef HRESULT (WINAPI* GetFileVersion_T)(
    LPCWSTR szFilename,
    LPWSTR szBuffer,
    DWORD cchBuffer,
    DWORD* dwLength
);

typedef struct _MMP_FAKE_HANDLE_LIST_ENTRY {
    LIST_ENTRY InMmpFakeHandleList;
    HANDLE hObject;
    PVOID value;
    BOOL bImageMapping;
}MMP_FAKE_HANDLE_LIST_ENTRY, * PMMP_FAKE_HANDLE_LIST_ENTRY;

static decltype(&CreateFileW) OriginCreateFileW = CreateFileW;
static decltype(&GetFileInformationByHandle) OriginGetFileInformationByHandle = GetFileInformationByHandle;
static decltype(&GetFileAttributesExW) OriginGetFileAttributesExW = GetFileAttributesExW;
static decltype(&GetFileSize) OriginGetFileSize = GetFileSize;
static decltype(&GetFileSizeEx) OriginGetFileSizeEx = GetFileSizeEx;
static decltype(&CreateFileMappingW) OriginCreateFileMappingW = CreateFileMappingW;
static decltype(&MapViewOfFileEx) OriginMapViewOfFileEx = MapViewOfFileEx;
static decltype(&MapViewOfFile) OriginMapViewOfFile = MapViewOfFile;
static decltype(&UnmapViewOfFile)OriginUnmapViewOfFile = UnmapViewOfFile;
static decltype(&CloseHandle)OriginCloseHandle = CloseHandle;
static GetFileVersion_T OriginGetFileVersion1 = nullptr;
static GetFileVersion_T OriginGetFileVersion2 = nullptr;

FILETIME AssemblyTimes;

CRITICAL_SECTION MmpFakeHandleListLock;
LIST_ENTRY MmpFakeHandleListHead;

static BOOLEAN PreHooked = FALSE;
static BOOLEAN Initialized = FALSE;

BOOL MmpIsMemoryModuleFileName(
    _In_ LPCWSTR lpFileName,
    _Out_opt_ PLDR_DATA_TABLE_ENTRY *LdrEntry) {

    __try {
        if (LdrEntry)*LdrEntry = nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }

    BOOL result = FALSE;

    EnterCriticalSection(NtCurrentPeb()->LoaderLock);
    for (auto entry = NtCurrentPeb()->Ldr->InLoadOrderModuleList.Flink;
        entry != &NtCurrentPeb()->Ldr->InLoadOrderModuleList;
        entry = entry->Flink) {

        PLDR_DATA_TABLE_ENTRY CurEntry = CONTAINING_RECORD(entry, LDR_DATA_TABLE_ENTRY, LDR_DATA_TABLE_ENTRY::InLoadOrderLinks);
        if (!wcsncmp(CurEntry->FullDllName.Buffer, lpFileName, CurEntry->FullDllName.Length) &&
            wcslen(lpFileName) * 2 == CurEntry->FullDllName.Length) {
            result = IsValidMemoryModuleHandle((HMODULE)CurEntry->DllBase);
            if (result) {
                if (LdrEntry) {
                    __try {
                        *LdrEntry = CurEntry;
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {
                        ;
                    }
                }
            }

            break;
        }

    }
    LeaveCriticalSection(NtCurrentPeb()->LoaderLock);

    return result;
}

VOID MmpInsertHandleEntry(
    _In_ HANDLE hObject,
    _In_ PVOID value,
    _In_ BOOL bImageMapping = FALSE) {
    auto entry = (PMMP_FAKE_HANDLE_LIST_ENTRY)RtlAllocateHeap(RtlProcessHeap(), 0, sizeof(MMP_FAKE_HANDLE_LIST_ENTRY));
    entry->hObject = hObject;
    entry->value = value;
    entry->bImageMapping = bImageMapping;

    EnterCriticalSection(&MmpFakeHandleListLock);
    InsertTailList(&MmpFakeHandleListHead, &entry->InMmpFakeHandleList);
    LeaveCriticalSection(&MmpFakeHandleListLock);
}

PMMP_FAKE_HANDLE_LIST_ENTRY MmpFindHandleEntry(HANDLE hObject) {

    PMMP_FAKE_HANDLE_LIST_ENTRY result = nullptr;
    EnterCriticalSection(&MmpFakeHandleListLock);

    for (auto entry = MmpFakeHandleListHead.Flink; entry != &MmpFakeHandleListHead; entry = entry->Flink) {
        auto CurEntry = CONTAINING_RECORD(entry, MMP_FAKE_HANDLE_LIST_ENTRY, MMP_FAKE_HANDLE_LIST_ENTRY::InMmpFakeHandleList);

        if (CurEntry->hObject == hObject) {
            result = CurEntry;
            break;
        }

    }

    LeaveCriticalSection(&MmpFakeHandleListLock);
    return result;
}

VOID MmpFreeHandleEntry(PMMP_FAKE_HANDLE_LIST_ENTRY lpHandleEntry) {
    EnterCriticalSection(&MmpFakeHandleListLock);
    RemoveEntryList(&lpHandleEntry->InMmpFakeHandleList);
    RtlFreeHeap(RtlProcessHeap(), 0, lpHandleEntry);
    LeaveCriticalSection(&MmpFakeHandleListLock);
}

HANDLE WINAPI HookCreateFileW(
    _In_ LPCWSTR lpFileName,
    _In_ DWORD dwDesiredAccess,
    _In_ DWORD dwShareMode,
    _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    _In_ DWORD dwCreationDisposition,
    _In_ DWORD dwFlagsAndAttributes,
    _In_opt_ HANDLE hTemplateFile) {

    PLDR_DATA_TABLE_ENTRY entry;
    if (MmpIsMemoryModuleFileName(lpFileName, &entry)) {
        HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        MmpInsertHandleEntry(hEvent, entry);
        return hEvent;
    }

    return OriginCreateFileW(
        lpFileName,
        dwDesiredAccess,
        dwShareMode,
        lpSecurityAttributes,
        dwCreationDisposition,
        dwFlagsAndAttributes,
        hTemplateFile
    );
}

BOOL WINAPI HookGetFileInformationByHandle(
    _In_ HANDLE hFile,
    _Out_ LPBY_HANDLE_FILE_INFORMATION lpFileInformation) {
    auto iter = MmpFindHandleEntry(hFile);
    if (iter) {
        RtlZeroMemory(lpFileInformation, sizeof(BY_HANDLE_FILE_INFORMATION));

        auto entry = (PLDR_DATA_TABLE_ENTRY)iter->value;
        auto module = MapMemoryModuleHandle((HMEMORYMODULE)entry->DllBase);

        lpFileInformation->ftCreationTime = lpFileInformation->ftLastAccessTime = lpFileInformation->ftLastWriteTime = AssemblyTimes;
        lpFileInformation->nFileSizeLow = module->dwImageFileSize;

        return TRUE;
    }
    else {
        return OriginGetFileInformationByHandle(
            hFile,
            lpFileInformation
        );
    }
}

BOOL WINAPI HookGetFileAttributesExW(
    _In_ LPCWSTR lpFileName,
    _In_ GET_FILEEX_INFO_LEVELS fInfoLevelId,
    _Out_writes_bytes_(sizeof(WIN32_FILE_ATTRIBUTE_DATA)) LPVOID lpFileInformation) {

    PLDR_DATA_TABLE_ENTRY entry;
    if (MmpIsMemoryModuleFileName(lpFileName, &entry)) {
        __try {
            RtlZeroMemory(
                lpFileInformation,
                sizeof(WIN32_FILE_ATTRIBUTE_DATA)
            );

            LPWIN32_FILE_ATTRIBUTE_DATA data = (LPWIN32_FILE_ATTRIBUTE_DATA)lpFileInformation;
            auto module = MapMemoryModuleHandle((HMEMORYMODULE)entry->DllBase);

            data->ftCreationTime = data->ftLastAccessTime = data->ftLastWriteTime = AssemblyTimes;
            data->nFileSizeLow = module->dwImageFileSize;
            return TRUE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return FALSE;
        }
    }

    return OriginGetFileAttributesExW(
        lpFileName,
        fInfoLevelId,
        lpFileInformation
    );
}

DWORD WINAPI HookGetFileSize(
    _In_ HANDLE hFile,
    _Out_opt_ LPDWORD lpFileSizeHigh) {

    auto iter = MmpFindHandleEntry(hFile);
    if (iter) {
        if (lpFileSizeHigh)*lpFileSizeHigh = 0;

        auto entry = (PLDR_DATA_TABLE_ENTRY)iter->value;
        auto module = MapMemoryModuleHandle((HMEMORYMODULE)entry->DllBase);

        return module->dwImageFileSize;
    }
    else {
        return OriginGetFileSize(
            hFile,
            lpFileSizeHigh
        );
    }

}

BOOL WINAPI HookGetFileSizeEx(
    _In_ HANDLE hFile,
    _Out_ PLARGE_INTEGER lpFileSize) {

    auto iter = MmpFindHandleEntry(hFile);
    if (iter) {
        auto entry = (PLDR_DATA_TABLE_ENTRY)iter->value;
        auto module = MapMemoryModuleHandle((HMEMORYMODULE)entry->DllBase);

        lpFileSize->QuadPart = module->dwImageFileSize;
        return TRUE;
    }
    else {
        return OriginGetFileSizeEx(
            hFile,
            lpFileSize
        );
    }

}

HANDLE WINAPI HookCreateFileMappingW(
    _In_     HANDLE hFile,
    _In_opt_ LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
    _In_     DWORD flProtect,
    _In_     DWORD dwMaximumSizeHigh,
    _In_     DWORD dwMaximumSizeLow,
    _In_opt_ LPCWSTR lpName) {

    auto iter = MmpFindHandleEntry(hFile);
    if (iter) {
        HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        
        MmpInsertHandleEntry(hEvent, iter->value, !!(flProtect & SEC_IMAGE));
        return hEvent;
    }

    return OriginCreateFileMappingW(
        hFile,
        lpFileMappingAttributes,
        flProtect,
        dwMaximumSizeHigh,
        dwMaximumSizeLow,
        lpName
    );
}

LPVOID WINAPI HookMapViewOfFileEx(
    _In_ HANDLE hFileMappingObject,
    _In_ DWORD dwDesiredAccess,
    _In_ DWORD dwFileOffsetHigh,
    _In_ DWORD dwFileOffsetLow,
    _In_ SIZE_T dwNumberOfBytesToMap,
    _In_opt_ LPVOID lpBaseAddress) {

    auto iter = MmpFindHandleEntry(hFileMappingObject);
    if (iter) {
        HMEMORYMODULE hModule = nullptr;
        auto entry = (PLDR_DATA_TABLE_ENTRY)iter->value;
        auto pModule = MapMemoryModuleHandle((HMEMORYMODULE)entry->DllBase);
        if (pModule) {
            if (iter->bImageMapping) {
                MemoryLoadLibrary(&hModule, pModule->lpReserved, pModule->dwImageFileSize);
                if (hModule) MmpInsertHandleEntry(hModule, hModule);
            }
            else {
                return pModule->lpReserved;
            }
        }

        return hModule;
    }

    return OriginMapViewOfFileEx(
        hFileMappingObject,
        dwDesiredAccess,
        dwFileOffsetHigh,
        dwFileOffsetLow,
        dwNumberOfBytesToMap,
        lpBaseAddress
    );
}

LPVOID WINAPI HookMapViewOfFile(
    _In_ HANDLE hFileMappingObject,
    _In_ DWORD dwDesiredAccess,
    _In_ DWORD dwFileOffsetHigh,
    _In_ DWORD dwFileOffsetLow,
    _In_ SIZE_T dwNumberOfBytesToMap) {

    return HookMapViewOfFileEx(
        hFileMappingObject,
        dwDesiredAccess,
        dwFileOffsetHigh,
        dwFileOffsetLow,
        dwNumberOfBytesToMap,
        nullptr
    );

}

BOOL WINAPI HookUnmapViewOfFile(_In_ LPCVOID lpBaseAddress) {
    auto iter = MmpFindHandleEntry((HANDLE)lpBaseAddress);
    if (iter) {
        MemoryFreeLibrary((HMEMORYMODULE)lpBaseAddress);
        MmpFreeHandleEntry(iter);
        return TRUE;
    }

    return OriginUnmapViewOfFile(lpBaseAddress);
}

BOOL WINAPI HookCloseHandle(_In_ _Post_ptr_invalid_ HANDLE hObject) {
    auto iter = MmpFindHandleEntry(hObject);
    if (iter)MmpFreeHandleEntry(iter);

    return OriginCloseHandle(hObject);
}

HRESULT WINAPI HookGetFileVersion(
    LPCWSTR szFilename,
    LPWSTR szBuffer,
    DWORD cchBuffer,
    DWORD* dwLength) {

    typedef struct _COR20_METADATA {
        DWORD Signature;
        WORD MajorVersion;
        WORD MinorVersion;
        DWORD Reserved;
        DWORD VersionLength;
        CHAR VersionString[ANYSIZE_ARRAY];
    }COR20_METADATA, * PCOR20_METADATA;

    PLDR_DATA_TABLE_ENTRY entry = nullptr;

    if (MmpIsMemoryModuleFileName(szFilename, &entry)) {

        __try {
            PIMAGE_NT_HEADERS headers = RtlImageNtHeader(entry->DllBase);
            auto dir = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR];
            if (!dir.Size || !dir.VirtualAddress)__leave;

            PIMAGE_COR20_HEADER cor2 = PIMAGE_COR20_HEADER(LPBYTE(entry->DllBase) + dir.VirtualAddress);
            if (!cor2->MetaData.Size || !cor2->MetaData.VirtualAddress) __leave;

            PCOR20_METADATA meta = PCOR20_METADATA(LPBYTE(entry->DllBase) + cor2->MetaData.VirtualAddress);
            if (dwLength)*dwLength = meta->VersionLength;
            if (cchBuffer < meta->VersionLength)return 0x8007007A;
            
            MultiByteToWideChar(CP_ACP, 0, meta->VersionString, meta->VersionLength, szBuffer, cchBuffer);
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            ;
        }

    }

    return OriginGetFileVersion1(
        szFilename,
        szBuffer,
        cchBuffer,
        dwLength
    );
}

BOOL WINAPI MmpPreInitializeHooksForDotNet() {

    EnterCriticalSection(NtCurrentPeb()->FastPebLock);

    if (!PreHooked) {
        HMODULE hModule = LoadLibraryW(L"mscoree.dll");
        if (hModule) {
            OriginGetFileVersion2 = (GetFileVersion_T)GetProcAddress(hModule, "GetFileVersion");
            if (OriginGetFileVersion2) {

                GetSystemTimeAsFileTime(&AssemblyTimes);

                InitializeCriticalSection(&MmpFakeHandleListLock);
                InitializeListHead(&MmpFakeHandleListHead);

                DetourTransactionBegin();
                DetourUpdateThread(NtCurrentThread());

                DetourAttach((PVOID*)&OriginCreateFileW, HookCreateFileW);
                DetourAttach((PVOID*)&OriginGetFileInformationByHandle, HookGetFileInformationByHandle);
                DetourAttach((PVOID*)&OriginGetFileAttributesExW, HookGetFileAttributesExW);
                DetourAttach((PVOID*)&OriginGetFileSize, HookGetFileSize);
                DetourAttach((PVOID*)&OriginGetFileSizeEx, HookGetFileSizeEx);
                DetourAttach((PVOID*)&OriginCreateFileMappingW, HookCreateFileMappingW);
                DetourAttach((PVOID*)&OriginMapViewOfFileEx, HookMapViewOfFileEx);
                DetourAttach((PVOID*)&OriginMapViewOfFile, HookMapViewOfFile);
                DetourAttach((PVOID*)&OriginUnmapViewOfFile, HookUnmapViewOfFile);
                DetourAttach((PVOID*)&OriginCloseHandle, HookCloseHandle);
                DetourAttach((PVOID*)&OriginGetFileVersion2, HookGetFileVersion);

                DetourTransactionCommit();

                PreHooked = TRUE;
            }
        }
    }

    LeaveCriticalSection(NtCurrentPeb()->FastPebLock);

    return PreHooked;
}

BOOL WINAPI MmpInitializeHooksForDotNet() {
    HMODULE hModule = GetModuleHandleW(L"mscoreei.dll");
    if (hModule) {
        OriginGetFileVersion1 = (GetFileVersion_T)GetProcAddress(hModule, "GetFileVersion");
        if (OriginGetFileVersion1) {

            EnterCriticalSection(NtCurrentPeb()->FastPebLock);

            if (!PreHooked) {
                LeaveCriticalSection(NtCurrentPeb()->FastPebLock);
                return FALSE;
            }

            if (!Initialized) {
                DetourTransactionBegin();
                DetourUpdateThread(NtCurrentThread());
                DetourAttach((PVOID*)&OriginGetFileVersion1, HookGetFileVersion);
                DetourTransactionCommit();
                Initialized = TRUE;
            }

            LeaveCriticalSection(NtCurrentPeb()->FastPebLock);
            return TRUE;
        }
    }

    return FALSE;
}
