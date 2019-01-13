/*
  NoirVisor - Hardware-Accelerated Hypervisor solution

  Copyright 2018, Zero Tang. All rights reserved.

  This file is auxiliary to Hooking facility (MSR Hook, Inline Hook).

  This program is distributed in the hope that it will be useful, but 
  without any warranty (no matter implied warranty or merchantability
  or fitness for a particular purpose, etc.).

  File Location: ./xpf_core/windows/hooks.c
*/

#include <ntddk.h>
#include <windef.h>
#include "BeaEngine.h"
#include "hooks.h"

ULONG SizeOfCode(IN PVOID Code,IN ULONG Architecture)
{
	DISASM dis={0};
	dis.EIP=(ULONG_PTR)Code;
	dis.VirtualAddr=dis.EIP;
	dis.Archi=Architecture;
	dis.Options=MasmSyntax|NoTabulation|SuffixedNumeral|ShowSegmentRegs;
	return Disasm(&dis);
}

ULONG GetPatchSize(IN PVOID Code,IN ULONG Length)
{
	ULONG_PTR p=(ULONG_PTR)Code;
	ULONG s=0;
	while(s<Length)
	{
		ULONG const l=SizeOfCode((PVOID)p,sizeof(void*)*16-64);
		s+=l;p+=l;
	}
	return s;
}

ULONG64 static NoirGetPhysicalAddress(IN PVOID VirtualAddress)
{
	PHYSICAL_ADDRESS pa=MmGetPhysicalAddress(VirtualAddress);
	return pa.QuadPart;
}

NTSTATUS static fake_NtSetInformationFile(IN HANDLE FileHandle,OUT PIO_STATUS_BLOCK IoStatusBlock,IN PVOID FileInformation,IN ULONG Length,IN FILE_INFORMATION_CLASS FileInformationClass)
{
	if(FileInformationClass==FileDispositionInformation)
	{
		NTSTATUS st=STATUS_INSUFFICIENT_RESOURCES;
		PFILE_NAME_INFORMATION FileNameInfo=ExAllocatePool(NonPagedPool,PAGE_SIZE);
		if(FileNameInfo)
		{
			RtlZeroMemory(FileNameInfo,PAGE_SIZE);
			st=ZwQueryInformationFile(FileHandle,IoStatusBlock,FileNameInfo,PAGE_SIZE,FileNameInformation);
			if(NT_SUCCESS(st))
			{
				PWSTR const NameEnd=(PWSTR)((ULONG_PTR)FileNameInfo->FileName+FileNameInfo->FileNameLength);
				PWSTR const ShorterStart=(PWSTR)((ULONG_PTR)NameEnd-NoirProtectedFileNameCb);
				ULONG i=0;
				BOOLEAN Equal=TRUE;
				for(;i<NoirProtectedFileNameCch;i++)
				{
					if(ShorterStart[i]!=NoirProtectedFileName[i])
					{
						Equal=FALSE;
						break;
					}
				}
				if(Equal)
					st=STATUS_DEVICE_PAPER_EMPTY;
				else
					st=Old_NtSetInformationFile(FileHandle,IoStatusBlock,FileInformation,Length,FileInformationClass);
			}
			ExFreePool(FileNameInfo);
		}
		return st;
	}
	return Old_NtSetInformationFile(FileHandle,IoStatusBlock,FileInformation,Length,FileInformationClass);
}

void static NoirHookNtSetInformationFile(IN PVOID HookedAddress)
{
	//Note that Length-Disassembler is required.
	ULONG PatchSize=GetPatchSize(NtSetInformationFile,HookLength);
	Old_NtSetInformationFile=ExAllocatePool(NonPagedPool,PatchSize+DetourLength);
	if(Old_NtSetInformationFile)
	{
#if defined(_WIN64)
		//This shellcode can breach the 4GB-limit in AMD64 architecture.
		//No register would be destroyed.
		/*
		  ShellCode Overview:
		  push rax			-- 50
		  mov rax, proxy	-- 48 B8 XX XX XX XX XX XX XX XX
		  xchg [rsp],rax	-- 48 87 04 24
		  ret				-- C3
		  16 bytes in total.
		*/
		BYTE HookCode[16]={0x50,0x48,0xB8,0,0,0,0,0,0,0,0,0x48,0x87,0x04,0x24,0xC3};
		BYTE DetourCode[14]={0xFF,0x25,0x0,0x0,0x0,0x0,0,0,0,0,0,0,0,0};
		*(PULONG64)((ULONG64)HookCode+3)=(ULONG64)fake_NtSetInformationFile;
		*(PULONG64)((ULONG64)DetourCode+6)=(ULONG64)NtSetInformationFile+PatchSize;
#else
		BYTE HookCode[5]={0xE9,0,0,0,0};
		BYTE DetourCode[5]={0xE9,0,0,0,0};
		*(PULONG)((ULONG)HookCode+1)=(ULONG)fake_NtSetInformationFile-(ULONG)NtSetInformationFile-5;
		*(PULONG)((ULONG)DetourCode+1)=(ULONG)NtSetInformationFile-(ULONG)Old_NtSetInformationFile-5;
#endif
		RtlCopyMemory(Old_NtSetInformationFile,NtSetInformationFile,PatchSize);
		RtlCopyMemory((PVOID)((ULONG_PTR)Old_NtSetInformationFile+PatchSize),DetourCode,DetourLength);
		RtlCopyMemory(HookedAddress,HookCode,HookLength);
	}
}

void NoirBuildHookedPages()
{
	HookPages=ExAllocatePool(NonPagedPool,sizeof(NOIR_HOOK_PAGE));
	if(HookPages)
	{
		PHYSICAL_ADDRESS MaxAddr={0xFFFFFFFFFFFFFFFF};
		UNICODE_STRING uniFuncName=RTL_CONSTANT_STRING(L"NtSetInformationFile");
		NtSetInformationFile=MmGetSystemRoutineAddress(&uniFuncName);
		HookPages->OriginalPage.VirtualAddress=NoirGetPageBase(NtSetInformationFile);
		HookPages->OriginalPage.PhysicalAddress=NoirGetPhysicalAddress(HookPages->OriginalPage.VirtualAddress);
		HookPages->HookedPage.VirtualAddress=MmAllocateContiguousMemory(PAGE_SIZE,MaxAddr);
		if(HookPages->HookedPage.VirtualAddress)
		{
			USHORT PageOffset=(USHORT)((ULONG_PTR)NtSetInformationFile&0xFFF);
			HookPages->HookedPage.PhysicalAddress=NoirGetPhysicalAddress(HookPages->HookedPage.VirtualAddress);
			RtlCopyMemory(HookPages->HookedPage.VirtualAddress,HookPages->OriginalPage.VirtualAddress,PAGE_SIZE);
			NoirHookNtSetInformationFile((PVOID)((ULONG_PTR)HookPages->HookedPage.VirtualAddress+PageOffset));
		}
		HookPages->NextHook=NULL;
	}
}

void NoirGetNtOpenProcessIndex()
{
	UNICODE_STRING uniFuncName=RTL_CONSTANT_STRING(L"ZwOpenProcess");
	PVOID p=MmGetSystemRoutineAddress(&uniFuncName);
	if(p)IndexOf_NtOpenProcess=*(PULONG)((ULONG_PTR)p+INDEX_OFFSET);
}

void NoirSetProtectedPID(IN ULONG NewPID)
{
	ProtPID=NewPID;
	ProtPID&=0xFFFFFFFC;
}