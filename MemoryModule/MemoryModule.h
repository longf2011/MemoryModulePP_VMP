#pragma once
#pragma warning(disable:4996)

#ifndef __MEMORY_MODULE_HEADER
#define __MEMORY_MODULE_HEADER

typedef HMODULE HMEMORYMODULE;

typedef struct _MEMORYMODULE {
	/*
		---------------------------
		|xxxxxxxx    BaseAddress  |
		|...                      |
		|...                      |
		|...                      | --> IMAGE_DOS_HEADER
		|...                      | --> IMAGE_NT_HEADERS
		|...                      |
		|...                      |
		--------------------------
		struct MEMORYMODULE;
		... (align)
		codes
	*/
	ULONG64 Signature;

	DWORD SizeofHeaders;
	union {
		struct {
			//Status Flags
			BYTE initialized : 1;
			BYTE loadFromNtLoadDllMemory : 1;
			BYTE underUnload : 1;
			BYTE reservedStatusFlags : 5;

			BYTE cbFlagsReserved;

			//Load Flags
			WORD MappedDll : 1;
			WORD InsertInvertedFunctionTableEntry : 1;
			WORD TlsHandled : 1;
			WORD UseReferenceCount : 1;
			WORD reservedLoadFlags : 12;

		};
		DWORD dwFlags;
	};

	LPBYTE codeBase;						//codeBase == ImageBase
	PVOID lpReserved;

	HMODULE* hModulesList;					//Import module handles
	DWORD dwModulesCount;					//number of module handles
	DWORD dwReferenceCount;

	DWORD dwImageFileSize;
	DWORD headers_align;				//headers_align == OptionalHeaders.BaseOfCode;

} MEMORYMODULE, * PMEMORYMODULE;


#define MEMORY_MODULE_SIGNATURE 0x00aabbcc11ffee00

#define AlignValueUpNew(value, alignment) ( size_t(value)%size_t(alignment)==0?size_t(size_t(value)/size_t(alignment))*size_t(alignment):(size_t(size_t(value)/size_t(alignment))+1)*size_t(alignment))


#ifdef __cplusplus
extern "C" {
#endif

	NTSTATUS MemoryLoadLibrary(
		_Out_ HMEMORYMODULE* MemoryModuleHandle,
		_In_ LPCVOID data,
		_In_ DWORD size
	);

	NTSTATUS MemoryResolveImportTable(
		_In_ LPBYTE base,
		_In_ PIMAGE_NT_HEADERS lpNtHeaders,
		_In_ PMEMORYMODULE hMemoryModule
	);

	NTSTATUS MemorySetSectionProtection(
		_In_ LPBYTE base,
		_In_ PIMAGE_NT_HEADERS lpNtHeaders
	);

    bool MemoryFreeLibrary(HMEMORYMODULE);

	bool WINAPI IsValidMemoryModuleHandle(HMEMORYMODULE hModule);

	PMEMORYMODULE WINAPI MapMemoryModuleHandle(HMEMORYMODULE hModule);

#ifdef __cplusplus
}
#endif

#endif  // __MEMORY_MODULE_HEADER
