#include "Private.h"
#include "Routines.h"
#include "Loader.h"
#include <Ntstrsafe.h>

#define CALL_COMPLETE   0xC0371E7E

typedef struct _INJECT_BUFFER
{
    UCHAR code[0x200];
    union
    {
        UNICODE_STRING path;
        UNICODE_STRING32 path32;
    };

    wchar_t buffer[488];
    PVOID module;
    ULONG complete;
} INJECT_BUFFER, *PINJECT_BUFFER;

extern DYNAMIC_DATA dynData;

PINJECT_BUFFER BBGetWow64Code( IN PVOID LdrLoadDll, IN PUNICODE_STRING pPath );
PINJECT_BUFFER BBGetNativeCode( IN PVOID LdrLoadDll, IN PUNICODE_STRING pPath );

NTSTATUS BBApcInject( IN PINJECT_BUFFER pUserBuf, IN HANDLE pid, IN ULONG initRVA, IN PCWCHAR InitArg );

#pragma alloc_text(PAGE, BBInjectDll)
#pragma alloc_text(PAGE, BBGetWow64Code)
#pragma alloc_text(PAGE, BBGetNativeCode)
#pragma alloc_text(PAGE, BBExecuteInNewThread)
#pragma alloc_text(PAGE, BBApcInject)
#pragma alloc_text(PAGE, BBQueueUserApc)
#pragma alloc_text(PAGE, BBLookupProcessThread)

/// <summary>
/// Inject dll into process
/// </summary>
/// <param name="pid">Target PID</param>
/// <param name="pPath">TFull-qualified dll path</param>
/// <returns>Status code</returns>
NTSTATUS BBInjectDll( IN PINJECT_DLL pData )
{
    NTSTATUS status = STATUS_SUCCESS;
    NTSTATUS threadStatus = STATUS_SUCCESS;
    PEPROCESS pProcess = NULL;

    status = PsLookupProcessByProcessId( (HANDLE)pData->pid, &pProcess );
    if (NT_SUCCESS( status ))
    {
        KAPC_STATE apc;
        UNICODE_STRING ustrPath, ustrNtdll;
        SET_PROC_PROTECTION prot = { 0 };
        PVOID pNtdll = NULL;
        PVOID LdrLoadDll = NULL;
        BOOLEAN isWow64 = (PsGetProcessWow64Process( pProcess ) != NULL) ? TRUE : FALSE;
        LARGE_INTEGER procTimeout = { 0 };

        // Process in signaled state, abort any operations
        if (KeWaitForSingleObject( pProcess, Executive, KernelMode, FALSE, &procTimeout ) == STATUS_WAIT_0)
        {
            DPRINT( "BlackBone: %s: Process %u is terminating. Abort\n", __FUNCTION__, pData->pid );

            if (pProcess)
                ObDereferenceObject( pProcess );
            return STATUS_PROCESS_IS_TERMINATING;
        }

        KeStackAttachProcess( pProcess, &apc );

        RtlInitUnicodeString( &ustrPath, pData->FullDllPath );
        RtlInitUnicodeString( &ustrNtdll, L"Ntdll.dll" );

        // Get ntdll base
        pNtdll = BBGetUserModule( pProcess, &ustrNtdll, isWow64 );

        if (!pNtdll)
        {
            DPRINT( "BlackBone: %s: Failed to get Ntdll base\n", __FUNCTION__ );
            status = STATUS_NOT_FOUND;
        }

        // Get LdrLoadDll address
        if (NT_SUCCESS( status ))
        {
            LdrLoadDll = BBGetModuleExport( pNtdll, "LdrLoadDll", pProcess, NULL );
            if (!LdrLoadDll)
            {
                DPRINT( "BlackBone: %s: Failed to get LdrLoadDll address\n", __FUNCTION__ );
                status = STATUS_NOT_FOUND;
            }
        }

        // If process is protected - temporarily disable protection
        if (PsIsProtectedProcess( pProcess ))
        {
            prot.pid = pData->pid;
            prot.enableState = FALSE;
            BBSetProtection( &prot );
        }

        // Call LdrLoadDll
        if (NT_SUCCESS( status ))
        {
            SIZE_T size = 0;
            PINJECT_BUFFER pUserBuf = isWow64 ? BBGetWow64Code( LdrLoadDll, &ustrPath ) : BBGetNativeCode( LdrLoadDll, &ustrPath );

            if (pData->type == IT_Thread)
            {
                status = BBExecuteInNewThread( pUserBuf, NULL, THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER, pData->wait, &threadStatus );

                // Injection failed
                if (!NT_SUCCESS( threadStatus ))
                {
                    status = threadStatus;
                    DPRINT( "BlackBone: %s: User thread failed with status - 0x%X\n", __FUNCTION__, status );
                }
                // Call Init routine
                else
                {
                    if (pUserBuf->module != 0 && pData->initRVA != 0)
                    {
                        RtlCopyMemory( pUserBuf->buffer, pData->initArg, sizeof( pUserBuf->buffer ) );
                        BBExecuteInNewThread( (PUCHAR)pUserBuf->module + pData->initRVA, pUserBuf->buffer,
                                              THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER, TRUE, &threadStatus );
                    }
                    else if (pUserBuf->module == 0)
                        DPRINT( "BlackBone: %s: Module base = 0. Aborting\n", __FUNCTION__ );
                }
            }
            else if (pData->type == IT_Apc)
            {
                status = BBApcInject( pUserBuf, (HANDLE)pData->pid, pData->initRVA, pData->initArg );
            }
            else if (pData->type == IT_MMap)
            {
                MODULE_DATA mod = { 0 };
                status = BBMapUserImage( pProcess, &ustrPath, NULL, 0, FALSE, KRebaseProcess | KManualImports, &mod );
            }
            else
            {
                DPRINT( "BlackBone: %s: Invalid injection type specified - %d\n", __FUNCTION__, pData->type );
                status = STATUS_INVALID_PARAMETER;
            }

            // Post-inject stuff
            if (NT_SUCCESS( status ))
            {
                // Unlink module
                if (pData->unlink)
                    BBUnlinkFromLoader( pProcess, pUserBuf->module, isWow64 );

                // Erase header
                if (pData->erasePE)
                {
                    __try
                    {
                        PIMAGE_NT_HEADERS64 pHdr = RtlImageNtHeader( pUserBuf->module );
                        if (pHdr)
                        {
                            ULONG oldProt = 0;
                            SIZE_T size = (pHdr->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) ? 
                                            ((PIMAGE_NT_HEADERS32)pHdr)->OptionalHeader.SizeOfHeaders :
                                            pHdr->OptionalHeader.SizeOfHeaders;

                            if (NT_SUCCESS( ZwProtectVirtualMemory( ZwCurrentProcess(), &pUserBuf->module, &size, PAGE_EXECUTE_READWRITE, &oldProt ) ))
                            {
                                RtlZeroMemory( pUserBuf->module, size );
                                ZwProtectVirtualMemory( ZwCurrentProcess(), &pUserBuf->module, &size, oldProt, &oldProt );

                                DPRINT( "BlackBone: %s: PE headers erased. \n", __FUNCTION__ );
                            }
                        }
                        else
                            DPRINT( "BlackBone: %s: Failed to retrieve PE headers for image\n", __FUNCTION__ );
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        DPRINT( "BlackBone: %s: Exception during PE header erease: 0x%X\n", __FUNCTION__, GetExceptionCode() );
                    }
                }
            }

            ZwFreeVirtualMemory( ZwCurrentProcess(), &pUserBuf, &size, MEM_RELEASE );
        }

        // Restore protection
        if (prot.pid != 0)
        {
            prot.enableState = TRUE;
            BBSetProtection( &prot );
        }

        KeUnstackDetachProcess( &apc );
    }
    else
        DPRINT( "BlackBone: %s: PsLookupProcessByProcessId failed with status 0x%X\n", __FUNCTION__, status );

    if (pProcess)
        ObDereferenceObject( pProcess );

    return status;
}

/// <summary>
/// Create new thread in the target process
/// Must be running in target process context
/// </summary>
/// <param name="pBaseAddress">Thread start address</param>
/// <param name="pParam">Thread argument</param>
/// <param name="flags">Thread creation flags</param>
/// <param name="wait">If set to TRUE - wait for thread completion</param>
/// <param name="pExitStatus">Thread exit status</param>
/// <returns>Status code</returns>
NTSTATUS BBExecuteInNewThread(
    IN PVOID pBaseAddress,
    IN PVOID pParam,
    IN ULONG flags,
    IN BOOLEAN wait,
    OUT PNTSTATUS pExitStatus 
    )
{
    HANDLE hThread = NULL;
    OBJECT_ATTRIBUTES ob = { 0 };
    
    InitializeObjectAttributes( &ob, NULL, OBJ_KERNEL_HANDLE, NULL, NULL );

    // If PreviousMode == KernelMode, handle granted access is ignored.
    // So it's safe to create handle without any DesiredAccess
    NTSTATUS status = ZwCreateThreadEx(
        &hThread, THREAD_QUERY_LIMITED_INFORMATION, &ob,
        ZwCurrentProcess(), pBaseAddress, pParam, flags,
        0, 0x1000, 0x100000, NULL
        );

    // Wait for completion
    if (NT_SUCCESS( status ) && wait != FALSE)
    {
        // Force 60 sec timeout
        LARGE_INTEGER timeout = { 0 };
        timeout.QuadPart = -(60ll * 10 * 1000 * 1000);

        status = ZwWaitForSingleObject( hThread, TRUE, &timeout );
        if (NT_SUCCESS(status))
        {
            THREAD_BASIC_INFORMATION info = { 0 };
            ULONG bytes = 0;

            status = ZwQueryInformationThread( hThread, ThreadBasicInformation, &info, sizeof( info ), &bytes );
            if (NT_SUCCESS( status ) && pExitStatus)
            {
                *pExitStatus = info.ExitStatus;
            }
            else if (!NT_SUCCESS( status ))
            {
                DPRINT( "BlackBone: %s: ZwQueryInformationThread failed with status 0x%X\n", __FUNCTION__, status );
            }
        }
        else
            DPRINT( "BlackBone: %s: ZwWaitForSingleObject failed with status 0x%X\n", __FUNCTION__, status ); 
    }
    else
        DPRINT( "BlackBone: %s: ZwCreateThreadEx failed with status 0x%X\n", __FUNCTION__, status );

    if (hThread)
        ZwClose( hThread );

    return status;
}

/// <summary>
/// Build injection code for wow64 process
/// Must be running in target process context
/// </summary>
/// <param name="LdrLoadDll">LdrLoadDll address</param>
/// <param name="pPath">Path to the dll</param>
/// <returns>Code pointer. When not needed, it should be freed with ZwFreeVirtualMemory</returns>
PINJECT_BUFFER BBGetWow64Code( IN PVOID LdrLoadDll, IN PUNICODE_STRING pPath )
{
    NTSTATUS status = STATUS_SUCCESS;
    PINJECT_BUFFER pBuffer = NULL;
    SIZE_T size = PAGE_SIZE;

    // Code
    UCHAR code[] = 
    { 
        0x68, 0, 0, 0, 0,                       // push ModuleHandle            offset +1 
        0x68, 0, 0, 0, 0,                       // push ModuleFileName          offset +6
        0x6A, 0,                                // push Flags  
        0x6A, 0,                                // push PathToFile
        0xE8, 0, 0, 0, 0,                       // call LdrLoadDll              offset +15
        0xBA, 0, 0, 0, 0,                       // mov edx, COMPLETE_OFFSET     offset +20
        0xC7, 0x02, 0x7E, 0x1E, 0x37, 0xC0,     // mov [edx], CALL_COMPLETE     
        0xC2, 0x04, 0x00                        // ret 4
    };

    status = ZwAllocateVirtualMemory( ZwCurrentProcess(), &pBuffer, 0, &size, MEM_COMMIT, PAGE_EXECUTE_READWRITE );
    if (NT_SUCCESS( status ))
    { 
        // Copy path
        PUNICODE_STRING32 pUserPath = &pBuffer->path32;
        pUserPath->Length = pPath->Length;
        pUserPath->MaximumLength = pPath->MaximumLength;
        pUserPath->Buffer = (ULONG)pBuffer->buffer;

        // Copy path
        memcpy( (PVOID)pUserPath->Buffer, pPath->Buffer, pPath->Length );

        // Copy code
        memcpy( pBuffer, code, sizeof( code ) );

        // Fill stubs
        *(ULONG*)((PUCHAR)pBuffer + 1)  = (ULONG)&pBuffer->module;
        *(ULONG*)((PUCHAR)pBuffer + 6)  = (ULONG)pUserPath;
        *(ULONG*)((PUCHAR)pBuffer + 15) = (ULONG)((ULONG_PTR)LdrLoadDll - ((ULONG_PTR)pBuffer + 15) - 5 + 1);
        *(ULONG*)((PUCHAR)pBuffer + 20) = (ULONG)&pBuffer->complete;

        return pBuffer;
    }

    UNREFERENCED_PARAMETER( pPath );
    return NULL;
}

/// <summary>
/// Build injection code for native x64 process
/// Must be running in target process context
/// </summary>
/// <param name="LdrLoadDll">LdrLoadDll address</param>
/// <param name="pPath">Path to the dll</param>
/// <returns>Code pointer. When not needed it should be freed with ZwFreeVirtualMemory</returns>
PINJECT_BUFFER BBGetNativeCode( IN PVOID LdrLoadDll, IN PUNICODE_STRING pPath )
{
    NTSTATUS status = STATUS_SUCCESS;
    PINJECT_BUFFER pBuffer = NULL;
    SIZE_T size = PAGE_SIZE;

    // Code
    UCHAR code[] =
    {
        0x48, 0x83, 0xEC, 0x28,                 // sub rsp, 0x28
        0x48, 0x31, 0xC9,                       // xor rcx, rcx
        0x48, 0x31, 0xD2,                       // xor rdx, rdx
        0x49, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,     // mov r8, ModuleFileName   offset +12
        0x49, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,     // mov r9, ModuleHandle     offset +28
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,     // mov rax, LdrLoadDll      offset +32
        0xFF, 0xD0,                             // call rax
        0x48, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,     // mov rdx, COMPLETE_OFFSET offset +44
        0xC7, 0x02, 0x7E, 0x1E, 0x37, 0xC0,     // mov [rdx], CALL_COMPLETE 
        0x48, 0x83, 0xC4, 0x28,                 // add rsp, 0x28
        0xC3                                    // ret
    };

    status = ZwAllocateVirtualMemory( ZwCurrentProcess(), &pBuffer, 0, &size, MEM_COMMIT, PAGE_EXECUTE_READWRITE );
    if (NT_SUCCESS( status ))
    {
        // Copy path
        PUNICODE_STRING pUserPath = &pBuffer->path;
        pUserPath->Length = 0;
        pUserPath->MaximumLength = sizeof(pBuffer->buffer);
        pUserPath->Buffer = pBuffer->buffer;

        RtlUnicodeStringCopy( pUserPath, pPath );

        // Copy code
        memcpy( pBuffer, code, sizeof( code ) );

        // Fill stubs
        *(ULONGLONG*)((PUCHAR)pBuffer + 12) = (ULONGLONG)pUserPath;
        *(ULONGLONG*)((PUCHAR)pBuffer + 22) = (ULONGLONG)&pBuffer->module;
        *(ULONGLONG*)((PUCHAR)pBuffer + 32) = (ULONGLONG)LdrLoadDll;
        *(ULONGLONG*)((PUCHAR)pBuffer + 44) = (ULONGLONG)&pBuffer->complete;

        return pBuffer;
    }

    UNREFERENCED_PARAMETER( pPath );
    return NULL;
}

/// <summary>
/// Inject dll using APC
/// Must be running in target process context
/// </summary>
/// <param name="pUserBuf">Injcetion code</param>
/// <param name="pid">Target process ID</param>
/// <param name="initRVA">Init routine RVA</param>
/// <param name="InitArg">Init routine argument</param>
/// <returns>Status code</returns>
NTSTATUS BBApcInject( IN PINJECT_BUFFER pUserBuf, IN HANDLE pid, IN ULONG initRVA, IN PCWCHAR InitArg )
{
    NTSTATUS status = STATUS_SUCCESS;
    PETHREAD pThread = NULL;

    // Get suitable thread
    status = BBLookupProcessThread( pid, &pThread );

    if (NT_SUCCESS( status ))
    {
        status = BBQueueUserApc( pThread, pUserBuf, NULL, NULL, NULL, TRUE );

        // Wait for completion
        if (NT_SUCCESS( status ))
        {
            LARGE_INTEGER interval = { 0 };
            interval.QuadPart = -(5LL * 10 * 1000);

            // Protect from UserMode AV
            __try
            {
                for (ULONG i = 0; pUserBuf->complete != CALL_COMPLETE && i < 10000; i++)
                    KeDelayExecutionThread( KernelMode, FALSE, &interval );

                // Call init routine
                if (pUserBuf->module != 0 && initRVA != 0)
                {
                    RtlCopyMemory( (PUCHAR)pUserBuf->buffer, InitArg, sizeof( pUserBuf->buffer ) );
                    BBQueueUserApc( pThread, (PUCHAR)pUserBuf->module + initRVA, pUserBuf->buffer, NULL, NULL, TRUE );

                    // Wait some time for routine to finish
                    interval.QuadPart = -(100LL * 10 * 1000);
                    KeDelayExecutionThread( KernelMode, FALSE, &interval );
                }
                else if (pUserBuf->module == 0)
                    DPRINT( "BlackBone: %s: Module base = 0. Aborting\n", __FUNCTION__ );
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                DPRINT( "BlackBone: %s: Exception\n", __FUNCTION__ );
                status = STATUS_ACCESS_VIOLATION;
            }            
        }
    }
    else
        DPRINT( "BlackBone: %s: Failed to locate thread\n", __FUNCTION__ );

    if (pThread)
        ObDereferenceObject( pThread );

    return status;
}