#include "targetver.h"
#include "coreload.h"

// Function export macro
#ifdef _USRDLL  
#define DllExport    __declspec(dllexport)
#else
#define DllExport    __declspec(dllimport)
#endif

// Export functions with their plain name
#define DllApi extern "C" DllExport

// The max length of a function to be executed in a .NET class
#define FunctionNameSize               256

// The max length of arguments to be parsed and passed to a .NET function
#define AssemblyFunCallArgsSize        12

using namespace coreload;

// Arguments for hosting the .NET Core runtime and loading an assembly into the
struct CoreLoadArgs
{
    unsigned char    Verbose;
    unsigned char    Reserved[7];
    pal::char_t      BinaryFilePath[MAX_PATH];
    pal::char_t      CoreRootPath[MAX_PATH];
    pal::char_t      CoreLibrariesPath[MAX_PATH];
};

// Arguments for executing a function located in a .NET assembly,
// with optional arguments passed to the function call
struct AssemblyFunctionCall
{
    char           Assembly[FunctionNameSize];
    char           Class[FunctionNameSize];
    char           Function[FunctionNameSize];
    unsigned char  Arguments[AssemblyFunCallArgsSize];
};

struct RemoteFunctionArgs
{
    const unsigned char *UserData;
    unsigned long       UserDataSize;
};

struct RemoteEntryInfo
{
    unsigned long      HostPID;
    RemoteFunctionArgs Args;
};

// DLL exports used for starting, executing in, and stopping the .NET Core runtime

// Create a native function delegate for a function inside a .NET assembly
DllApi
int
CreateAssemblyDelegate(
    const char *assembly_name,
    const char *type_name,
    const char *method_name,
    void       **pfnDelegate
);

// Execute a function located in a .NET assembly by creating a native delegate
DllApi
int
ExecuteAssemblyFunction(
    const AssemblyFunctionCall *args
);

// Host the .NET Core runtime in the current application
DllApi
int
StartCoreCLR(
    const CoreLoadArgs *args
);

// Stop the .NET Core host in the current application
DllApi
int
UnloadRuntime(
    void
);

// Start the .NET Core runtime in the current application
int
StartCoreCLRInternal(
    const pal::char_t   *dllPath,
    const unsigned char verbose,
    const pal::char_t   *coreRoot,
    const pal::char_t   *coreLibraries) {

    return StatusCode::Success;
}

// Host the .NET Core runtime in the current application
DllApi
int
StartCoreCLR(
    const CoreLoadArgs *args) {
    host_startup_info_t startup_info;
    arguments_t arguments;

    // Used to the find dotnet dependencies
    startup_info.dotnet_root = args->CoreRootPath;

    arguments.managed_application = args->BinaryFilePath;
    arguments.app_root = get_directory(arguments.managed_application);

    return fx_muxer_t::initialize_clr(
        arguments,
        startup_info,
        host_mode_t::muxer);
}

// Create a native function delegate for a function inside a .NET assembly
int
CreateAssemblyDelegate(
    const char *assembly_name,
    const char *type_name,
    const char *method_name,
    void **pfnDelegate) {
    return fx_muxer_t::create_delegate(
        assembly_name,
        type_name,
        method_name,
        pfnDelegate
    );
}

// Execute a method from a class located inside a .NET assembly
int
ExecuteAssemblyClassFunction(
    const char *assembly,
    const char *type,
    const char *entry,
    const unsigned char *arguments) {

    int exit_code = StatusCode::HostApiFailed;
    typedef void (STDMETHODCALLTYPE MainMethodFp)(const VOID* args);
    MainMethodFp *pfnDelegate = nullptr;

    if (SUCCEEDED((exit_code = CreateAssemblyDelegate(assembly, type, entry, reinterpret_cast<PVOID*>(&pfnDelegate))))) {
        RemoteEntryInfo entryInfo = { 0 };
        entryInfo.HostPID = GetCurrentProcessId();

        const auto remoteArgs = reinterpret_cast<const RemoteFunctionArgs*>(arguments);
        if (remoteArgs != nullptr) {
            // construct a hex string for the address of the entryInfo parameter
            // which is passed to the .NET delegate function and execute the delegate
            entryInfo.Args.UserData = remoteArgs->UserData;
            entryInfo.Args.UserDataSize = remoteArgs->UserDataSize;

            pfnDelegate(&entryInfo);
        }
        else {
            // No arguments were supplied to pass to the delegate function so pass an
            // empty string
            pfnDelegate(nullptr);
        }
    }
    else {
        exit_code = StatusCode::InvalidArgFailure;
    }
    return exit_code;
}

// Execute a function located in a .NET assembly by creating a native delegate
DllApi
int
ExecuteAssemblyFunction(
    const AssemblyFunctionCall *args) {

    return ExecuteAssemblyClassFunction(args->Assembly, args->Class, args->Function, args->Arguments);
}

// Shutdown the .NET Core runtime
DllApi
int
UnloadRuntime(void) {
    return fx_muxer_t::unload_runtime();
}

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
) {
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

