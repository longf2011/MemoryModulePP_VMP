#include "stdafx.h"
#include <tchar.h>
#include <algorithm>

#if _MSC_VER
#pragma warning(disable:4055)
#pragma warning(error: 4244)
#pragma warning(error: 4267)
#pragma warning(disable:4996)
#define inline __inline
#endif

#ifdef _WIN64
#define HOST_MACHINE IMAGE_FILE_MACHINE_AMD64
#else
#define HOST_MACHINE IMAGE_FILE_MACHINE_I386
#endif

#define GET_HEADER_DICTIONARY(headers, idx)  &headers->OptionalHeader.DataDirectory[idx]

PMEMORYMODULE WINAPI MapMemoryModuleHandle(HMEMORYMODULE hModule) {
	__try {
		PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hModule;
		if (!dos)return nullptr;
		PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((LPBYTE)hModule + dos->e_lfanew);
		if (!nt)return nullptr;
		PMEMORYMODULE pModule = (PMEMORYMODULE)((LPBYTE)hModule + nt->OptionalHeader.SizeOfHeaders);
		if (!_ProbeForRead(pModule, sizeof(MEMORYMODULE)))return nullptr;
		if (pModule->Signature != MEMORY_MODULE_SIGNATURE || (size_t)pModule->codeBase != nt->OptionalHeader.ImageBase)return nullptr;
		return pModule;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

bool WINAPI IsValidMemoryModuleHandle(HMEMORYMODULE hModule) {
	return MapMemoryModuleHandle(hModule) != nullptr;
}

//AlignValueUp ->0x1000 -> 0x2000, last section error
//#define AlignValueUp(value, alignment) ((size_t(value) + size_t(alignment) + 1) & ~(size_t(alignment) - 1))

#define OffsetPointer(data, offset) LPVOID(LPBYTE(data) + ptrdiff_t(offset))


// Protection flags for memory pages (Executable, Readable, Writeable)
static int ProtectionFlags[2][2][2] = {
	{
		// not executable
		{PAGE_NOACCESS, PAGE_WRITECOPY},
		{PAGE_READONLY, PAGE_READWRITE},
	}, {
		// executable
		{PAGE_EXECUTE, PAGE_EXECUTE_WRITECOPY},
		{PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE},
	},
};

static SYSTEM_INFO sysInfo = []()->SYSTEM_INFO {
	SYSTEM_INFO tmp;
	GetNativeSystemInfo(&tmp);
	return tmp;
}();

NTSTATUS MemoryResolveImportTable(
	_In_ LPBYTE base,
	_In_ PIMAGE_NT_HEADERS lpNtHeaders,
	_In_ PMEMORYMODULE hMemoryModule) {
	NTSTATUS status = STATUS_SUCCESS;
	PIMAGE_IMPORT_DESCRIPTOR importDesc = nullptr;
	DWORD count = 0;

	do {
		__try {
			PIMAGE_DATA_DIRECTORY dir = GET_HEADER_DICTIONARY(lpNtHeaders, IMAGE_DIRECTORY_ENTRY_IMPORT);
			PIMAGE_IMPORT_DESCRIPTOR iat = nullptr;

			if (dir && dir->Size) {
				iat = importDesc = PIMAGE_IMPORT_DESCRIPTOR(lpNtHeaders->OptionalHeader.ImageBase + dir->VirtualAddress);
			}

			if (iat) {
				while (iat->Name) {
					++count;
					++iat;
				}
			}

			if (importDesc && count) {
				if (!(hMemoryModule->hModulesList = new HMODULE[count])) {
					status = STATUS_NO_MEMORY;
					break;
				}

				RtlZeroMemory(
					hMemoryModule->hModulesList,
					sizeof(HMODULE) * count
				);

				for (DWORD i = 0; i < count; ++i, ++importDesc) {
					uintptr_t* thunkRef;
					FARPROC* funcRef;
					HMODULE handle = LoadLibraryA((LPCSTR)(base + importDesc->Name));

					if (!handle) {
						status = STATUS_DLL_NOT_FOUND;
						break;
					}

					hMemoryModule->hModulesList[hMemoryModule->dwModulesCount++] = handle;
					thunkRef = (uintptr_t*)(base + (importDesc->OriginalFirstThunk ? importDesc->OriginalFirstThunk : importDesc->FirstThunk));
					funcRef = (FARPROC*)(base + importDesc->FirstThunk);
					while (*thunkRef) {
						*funcRef = GetProcAddress(
							handle,
							IMAGE_SNAP_BY_ORDINAL(*thunkRef) ? (LPCSTR)IMAGE_ORDINAL(*thunkRef) : (LPCSTR)PIMAGE_IMPORT_BY_NAME(base + (*thunkRef))->Name
						);
						if (!*funcRef) {
							status = STATUS_ENTRYPOINT_NOT_FOUND;
							break;
						}
						++thunkRef;
						++funcRef;
					}

					if (!NT_SUCCESS(status))break;
				}

			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			status = GetExceptionCode();
		}
	} while (false);

	if (!NT_SUCCESS(status)) {
		for (DWORD i = 0; i < hMemoryModule->dwModulesCount; ++i)
			FreeLibrary(hMemoryModule->hModulesList[i]);

		delete[]hMemoryModule->hModulesList;
		hMemoryModule->hModulesList = nullptr;
		hMemoryModule->dwModulesCount = 0;
	}

	return status;
}

NTSTATUS MemorySetSectionProtection(
	_In_ LPBYTE base,
	_In_ PIMAGE_NT_HEADERS lpNtHeaders) {
	NTSTATUS status = STATUS_SUCCESS;
	PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(lpNtHeaders);

	//
	// Determine whether it is a .NET assembly
	//
	auto& com = lpNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR];
	bool CorImage = com.Size && com.VirtualAddress;

	for (DWORD i = 0; i < lpNtHeaders->FileHeader.NumberOfSections; ++i, ++section) {
		LPVOID address = LPBYTE(base) + section->VirtualAddress;
		SIZE_T size = AlignValueUpNew(section->Misc.VirtualSize, lpNtHeaders->OptionalHeader.SectionAlignment);

		if (section->Characteristics & IMAGE_SCN_MEM_DISCARDABLE && !CorImage) {
			//
			// If it is a .NET assembly, we cannot release this memory block
			//
#pragma warning(disable:6250)
			VirtualFree(address, size, MEM_DECOMMIT);
#pragma warning(default:6250)
		}
		else {
			BOOL executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0,
				readable = (section->Characteristics & IMAGE_SCN_MEM_READ) != 0,
				writeable = (section->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
			DWORD protect = ProtectionFlags[executable][readable][writeable], oldProtect;

			if (section->Characteristics & IMAGE_SCN_MEM_NOT_CACHED) protect |= PAGE_NOCACHE;

			status = NtProtectVirtualMemory(NtCurrentProcess(), &address, &size, protect, &oldProtect);
			if (!NT_SUCCESS(status))break;
		}
	}

	return status;
}

NTSTATUS MemoryLoadLibrary(
	_Out_ HMEMORYMODULE* MemoryModuleHandle,
	_In_ LPCVOID data,
	_In_ DWORD size) {

	PIMAGE_DOS_HEADER dos_header = nullptr;
	PIMAGE_NT_HEADERS old_header = nullptr;
	BOOLEAN CorImage = FALSE;
	NTSTATUS status = STATUS_SUCCESS;

	//
	// Check parameters
	//
	__try {

		*MemoryModuleHandle = nullptr;

		//
		// Check dos magic
		//
		dos_header = (PIMAGE_DOS_HEADER)data;
		if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
			status = STATUS_INVALID_IMAGE_FORMAT;
			__leave;
		}

		//
		// Check nt headers
		//
		old_header = (PIMAGE_NT_HEADERS)((size_t)data + dos_header->e_lfanew);
		if (old_header->Signature != IMAGE_NT_SIGNATURE ||
			old_header->OptionalHeader.SectionAlignment & 1) {
			status = STATUS_INVALID_IMAGE_FORMAT;
			__leave;
		}

		//
		// Match machine type
		//
		if (old_header->FileHeader.Machine != HOST_MACHINE) {
			status = STATUS_IMAGE_MACHINE_TYPE_MISMATCH;
			__leave;
		}

		//
		// Only dll image support
		//
		if (!(old_header->FileHeader.Characteristics & IMAGE_FILE_DLL)) {
			status = STATUS_NOT_SUPPORTED;
			__leave;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		status = GetExceptionCode();
	}
	if (!NT_SUCCESS(status) || status == STATUS_IMAGE_MACHINE_TYPE_MISMATCH)return status;

	//
	// Reserve the address range of image
	//
	LPBYTE base = (LPBYTE)VirtualAlloc(
		LPVOID(old_header->OptionalHeader.ImageBase),
		old_header->OptionalHeader.SizeOfImage,
		MEM_RESERVE,
		PAGE_READWRITE
	);
	if (!base) {
		if (old_header->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) {

			base = (LPBYTE)VirtualAlloc(
				nullptr,
				old_header->OptionalHeader.SizeOfImage,
				MEM_RESERVE,
				PAGE_READWRITE
			);
			if (!base) status = STATUS_NO_MEMORY;
		}

		if (!NT_SUCCESS(status)) {
			return status;
		}
	}

	//
	// Allocate memory for image headers
	//
	size_t alignedHeadersSize = (DWORD)AlignValueUpNew(old_header->OptionalHeader.SizeOfHeaders + sizeof(MEMORYMODULE), sysInfo.dwPageSize);
	if (!VirtualAlloc(base, alignedHeadersSize, MEM_COMMIT, PAGE_READWRITE)) {
		VirtualFree(base, 0, MEM_RELEASE);
		status = STATUS_NO_MEMORY;
		return status;
	}

	//
	// Copy headers
	//
	PIMAGE_DOS_HEADER new_dos_header = (PIMAGE_DOS_HEADER)base;
	PIMAGE_NT_HEADERS new_header = (PIMAGE_NT_HEADERS)(base + dos_header->e_lfanew);
	RtlCopyMemory(
		new_dos_header,
		dos_header,
		old_header->OptionalHeader.SizeOfHeaders
	);
	new_header->OptionalHeader.ImageBase = (size_t)base;

	//
	// Setup MemoryModule structure.
	//
	PMEMORYMODULE hMemoryModule = (PMEMORYMODULE)(base + old_header->OptionalHeader.SizeOfHeaders);
	RtlZeroMemory(hMemoryModule, sizeof(MEMORYMODULE));
	hMemoryModule->codeBase = base;
	hMemoryModule->dwImageFileSize = size;
	hMemoryModule->Signature = MEMORY_MODULE_SIGNATURE;
	hMemoryModule->SizeofHeaders = old_header->OptionalHeader.SizeOfHeaders;
	hMemoryModule->lpReserved = (LPVOID)data;
	hMemoryModule->dwReferenceCount = 1;

	do {
		//
		// Allocate and copy sections
		//
		PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(new_header);
		for (DWORD i = 0; i < new_header->FileHeader.NumberOfSections; ++i, ++section) {

			DWORD size = AlignValueUpNew(
				section->Misc.VirtualSize,
				new_header->OptionalHeader.SectionAlignment
			);
			if (size < section->SizeOfRawData) {
				status = STATUS_INVALID_IMAGE_FORMAT;
				break;
			}

			LPVOID dest = VirtualAlloc(
				(LPSTR)new_header->OptionalHeader.ImageBase + section->VirtualAddress,
				size,
				MEM_COMMIT,
				PAGE_READWRITE
			);
			if (!dest) {
				status = STATUS_NO_MEMORY;
				break;
			}

			if (section->SizeOfRawData) {
				RtlCopyMemory(
					dest,
					LPBYTE(data) + section->PointerToRawData,
					section->SizeOfRawData
				);
			}

		}
		if (!NT_SUCCESS(status))break;

		//
		// Rebase image
		//
		auto locationDelta = new_header->OptionalHeader.ImageBase - old_header->OptionalHeader.ImageBase;
		if (locationDelta) {
			typedef struct _REBASE_INFO {
				USHORT Offset : 12;
				USHORT Type : 4;
			}REBASE_INFO, * PREBASE_INFO;
			typedef struct _IMAGE_BASE_RELOCATION_HEADER {
				DWORD VirtualAddress;
				DWORD SizeOfBlock;
				REBASE_INFO TypeOffset[ANYSIZE_ARRAY];

				DWORD TypeOffsetCount()const {
					return (this->SizeOfBlock - 8) / sizeof(_REBASE_INFO);
				}
			}IMAGE_BASE_RELOCATION_HEADER, * PIMAGE_BASE_RELOCATION_HEADER;

			PIMAGE_DATA_DIRECTORY dir = GET_HEADER_DICTIONARY(new_header, IMAGE_DIRECTORY_ENTRY_BASERELOC);
			PIMAGE_BASE_RELOCATION_HEADER relocation = (PIMAGE_BASE_RELOCATION_HEADER)(LPBYTE(base) + dir->VirtualAddress);

			if (dir->Size && dir->VirtualAddress) {
				while (relocation->VirtualAddress > 0) {
					auto relInfo = (_REBASE_INFO*)&relocation->TypeOffset;
					for (DWORD i = 0; i < relocation->TypeOffsetCount(); ++i, ++relInfo) {
						switch (relInfo->Type) {
						case IMAGE_REL_BASED_HIGHLOW: *(DWORD*)(base + relocation->VirtualAddress + relInfo->Offset) += (DWORD)locationDelta; break;
#ifdef _WIN64
						case IMAGE_REL_BASED_DIR64: *(ULONGLONG*)(base + relocation->VirtualAddress + relInfo->Offset) += (ULONGLONG)locationDelta; break;
#endif
						case IMAGE_REL_BASED_ABSOLUTE:
						default: break;
						}
					}

					// advance to next relocation block
					//relocation->VirtualAddress += module->headers_align;
					relocation = decltype(relocation)(OffsetPointer(relocation, relocation->SizeOfBlock));
				}
			}

		}
		if (!NT_SUCCESS(status))break;

		__try {
			*MemoryModuleHandle = (HMEMORYMODULE)base;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			status = GetExceptionCode();
			break;
		}

		return status;
	} while (false);

	MemoryFreeLibrary((HMEMORYMODULE)base);
	return status;
}

bool MemoryFreeLibrary(HMEMORYMODULE mod) {
	PMEMORYMODULE module = MapMemoryModuleHandle(mod);
	PIMAGE_NT_HEADERS headers = RtlImageNtHeader(mod);

	if (!module) return false;
	if (module->loadFromNtLoadDllMemory && !module->underUnload)return false;
	if (module->hModulesList) {
		for (DWORD i = 0; i < module->dwModulesCount; ++i) {
			if (module->hModulesList[i]) {
				FreeLibrary(module->hModulesList[i]);
			}
		}
		delete[] module->hModulesList;
	}

	if (module->codeBase) VirtualFree(mod, 0, MEM_RELEASE);
	return true;
}
