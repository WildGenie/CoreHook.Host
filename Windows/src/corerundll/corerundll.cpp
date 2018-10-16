#include "corerundll.h"
#include "logger.h"
#include "mscoree.h"
#include <psapi.h>
#include <memory>

// Utility macro for testing whether or not a flag is set.
#define HAS_FLAG(value, flag) (((value) & (flag)) == (flag))

// Environment variable for setting whether or not to use Server GC.
// Off by default.
static const WCHAR *serverGcVar = W("COMPlus_gcServer");

// Environment variable for setting whether or not to use Concurrent GC.
// On by default.
static const WCHAR *concurrentGcVar = W("COMPlus_gcConcurrent");

// The name of the .NET Core runtime native runtime DLL.
static const WCHAR *coreCLRDll = W("CoreCLR.dll");

// The location where CoreCLR is expected to be installed. If CoreCLR.dll isn't
//  found in the same directory as the host, it will be looked for here.
static const WCHAR *coreCLRInstallDirectory = W("%windir%\\system32\\");

// Handle to the CoreCLR hosting interface
ICLRRuntimeHost4 *g_Host;

// Handle to a logger which writes to the standard output
std::shared_ptr<Logger> g_Log;

// The AppDomain ID in  which .NET assemblies will be executed in
DWORD g_domainId;

// Encapsulates the environment that CoreCLR will run in, including the TPALIST
class HostEnvironment
{
    // The path to this module
    std::wstring m_hostPath;

    // The path to the directory containing this module
    std::wstring m_hostDirectoryPath;

    // The name of this module, without the path
    std::wstring m_hostExeName;

    // The list of paths to the assemblies that will be trusted by CoreCLR
    std::wstring m_tpaList;

    ICLRRuntimeHost4 *m_CLRRuntimeHost;

    HMODULE m_coreCLRModule;

    Logger *m_log;

    DWORD GetModuleFileNameWrapper(
        IN HMODULE hModule,
        std::wstring& buffer
       )
    {
        WCHAR buf[MAX_PATH];
        DWORD size = MAX_PATH;
        DWORD ret = GetModuleFileNameW(
            hModule,
            buf,
            size
        );
        if (ret == 0)
        {
            return GetLastError();
        }

        buffer.assign(buf);
        return ret;
    }

    // Attempts to load CoreCLR.dll from the given directory.
    // On success pins the dll, sets m_coreCLRDirectoryPath and returns the HMODULE.
    // On failure returns nullptr.
    HMODULE TryLoadCoreCLR(const std::wstring& directoryPath) {

        std::wstring coreCLRPath(directoryPath);

        coreCLRPath.append(coreCLRDll);

        *m_log << W("Attempting to load: ") << coreCLRPath << Logger::endl;

        HMODULE result = LoadLibraryExW(coreCLRPath.c_str(), NULL, 0);
        if (!result) {
            *m_log << W("Failed to load: ") << coreCLRPath << Logger::endl;
            *m_log << W("Error code: ") << GetLastError() << Logger::endl;
            return nullptr;
        }

        // Pin the module - CoreCLR.dll does not support being unloaded.
        HMODULE dummy_coreCLRModule;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, coreCLRPath.c_str(), &dummy_coreCLRModule)) {
            *m_log << W("Failed to pin: ") << coreCLRPath << Logger::endl;
            return nullptr;
        }

        std::wstring coreCLRLoadedPath;
        GetModuleFileNameWrapper(result, coreCLRLoadedPath);

        *m_log << W("Loaded: ") << coreCLRLoadedPath << Logger::endl;

        return result;
    }

public:
    // The path to the directory that CoreCLR is in
    std::wstring m_coreCLRDirectoryPath;

    HostEnvironment(Logger *logger, const WCHAR *coreRootPath)
        : m_log(logger), m_CLRRuntimeHost(nullptr) {

        // Discover the path to this exe's module. All other files are expected to be in the same directory.
        GetModuleFileNameWrapper(::GetModuleHandleW(nullptr), m_hostPath);

        // Search for the last backslash in the host path.
        std::size_t lastBackslash = m_hostPath.find_last_of(W('\\')) + 1;

        // Copy the directory path
        m_hostDirectoryPath = m_hostPath.substr(0, lastBackslash);

        // Save the exe name
        m_hostExeName = m_hostPath.substr(lastBackslash);

        *m_log << W("Host directory: ") << m_hostDirectoryPath << Logger::endl;
        *m_log << W("Host Exe: ") << m_hostExeName << Logger::endl;

        // Check for %CORE_ROOT% and try to load CoreCLR.dll from it if it is set
        std::wstring coreRoot;

        m_coreCLRModule = NULL; // Initialize this here since we don't call TryLoadCoreCLR if CORE_ROOT is unset.

        if (coreRootPath)
        {
            coreRoot.append(coreRootPath);
            coreRoot.append(W("\\"));
            m_coreCLRModule = TryLoadCoreCLR(coreRoot);
        }
        else
        {
            *m_log << W("CORE_ROOT path was not set; skipping") << Logger::endl;
        }

        // Try to load CoreCLR from the host directory
        if (!m_coreCLRModule) {
            m_coreCLRModule = TryLoadCoreCLR(m_hostDirectoryPath);
        }

        if (!m_coreCLRModule) {

            // Failed to load. Try to load from the well-known location.
            WCHAR coreCLRInstallPath[MAX_PATH];
            ::ExpandEnvironmentStringsW(coreCLRInstallDirectory, coreCLRInstallPath, MAX_PATH);
            m_coreCLRModule = TryLoadCoreCLR(coreCLRInstallPath);

        }

        if (m_coreCLRModule) {

            // Save the directory that CoreCLR was found in
            GetModuleFileNameWrapper(m_coreCLRModule, m_coreCLRDirectoryPath);

            // Search for the last backslash and terminate it there to keep just the directory path with trailing slash

            std::size_t lastBackslash = m_coreCLRDirectoryPath.find_last_of(W('\\'));
            m_coreCLRDirectoryPath.resize(lastBackslash);
        }
        else {
            *m_log << W("Unable to load ") << coreCLRDll << Logger::endl;
        }
    }

    ~HostEnvironment() {
        if (m_coreCLRModule) {
            // Free the module. This is done for completeness, but in fact CoreCLR.dll 
            // was pinned earlier so this call won't actually free it. The pinning is
            // done because CoreCLR does not support unloading.
            ::FreeLibrary(m_coreCLRModule);
        }
    }

    bool TPAListContainsFile(
        _In_z_ WCHAR *fileNameWithoutExtension,
        _In_reads_(countExtensions) const WCHAR **rgTPAExtensions,
        int countExtensions)
    {
        if (m_tpaList.empty()) return false;

        for (int iExtension = 0; iExtension < countExtensions; iExtension++)
        {
            std::wstring fileName;
            fileName.append(W("\\")); // So that we don't match other files that end with the current file name
            fileName.append(fileNameWithoutExtension);
            fileName.append(rgTPAExtensions[iExtension] + 1);
            fileName.append(W(";")); // So that we don't match other files that begin with the current file name

            if (m_tpaList.find(fileName) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    void
    RemoveExtensionAndNi(
        _In_z_ WCHAR *fileName
        )
    {
        // Remove extension, if it exists
        WCHAR* extension = wcsrchr(fileName, W('.'));
        if (extension != NULL)
        {
            extension[0] = W('\0');

            // Check for .ni
            const size_t len = wcslen(fileName);
            if (len > 3 &&
                fileName[len - 1] == W('i') &&
                fileName[len - 2] == W('n') &&
                fileName[len - 3] == W('.'))
            {
                fileName[len - 3] = W('\0');
            }
        }
    }

    void AddFilesFromDirectoryToTPAList(
        _In_z_ const std::wstring& targetPath,
        _In_reads_(countExtensions) const WCHAR **rgTPAExtensions,
        int countExtensions)
    {
        *m_log << W("Adding assemblies from ") << targetPath << W(" to the TPA list") << Logger::endl;
        std::wstring assemblyPath;
        const size_t dirLength = targetPath.size();

        for (int iExtension = 0; iExtension < countExtensions; iExtension++)
        {
            assemblyPath.assign(targetPath);
            assemblyPath.append(rgTPAExtensions[iExtension]);

            WIN32_FIND_DATA data;
            HANDLE findHandle = FindFirstFileW(assemblyPath.c_str(), &data);

            if (findHandle != INVALID_HANDLE_VALUE) {
                do {
                    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        // It seems that CoreCLR doesn't always use the first instance of an assembly on the TPA list (ni's may be preferred
                        // over il, even if they appear later). So, only include the first instance of a simple assembly name to allow
                        // users the opportunity to override Framework assemblies by placing dlls in %CORE_LIBRARIES%

                        // ToLower for case-insensitive comparisons
                        WCHAR* fileNameChar = data.cFileName;
                        while (*fileNameChar)
                        {
                            *fileNameChar = towlower(*fileNameChar);
                            fileNameChar++;
                        }

                        // Remove extension
                        WCHAR fileNameWithoutExtension[MAX_PATH];
                        wcscpy_s(fileNameWithoutExtension, MAX_PATH, data.cFileName);

                        RemoveExtensionAndNi(fileNameWithoutExtension);

                        // Add to the list if not already on it
                        if (!TPAListContainsFile(fileNameWithoutExtension, rgTPAExtensions, countExtensions))
                        {
                            assemblyPath.resize(dirLength);
                            assemblyPath.append(data.cFileName);
                            m_tpaList.append(assemblyPath);
                            m_tpaList.append(W(";"));
                        }
                        else
                        {
                            *m_log << W("Not adding ")
                                << targetPath 
                                << data.cFileName
                                << W(" to the TPA list because another file with the same name is already present on the list")
                                << Logger::endl;
                        }
                    }
                } while (0 != FindNextFileW(findHandle, &data));

                FindClose(findHandle);
            }
        }
    }

    // Returns the semicolon-separated list of paths to runtime dlls that are considered trusted.
    // On first call, scans the coreclr directory for dlls and adds them all to the list.
    const WCHAR * GetTpaList(const WCHAR *coreLibsPath) {
        const WCHAR *rgTPAExtensions[] = {
            // Probe for .ni.dll first so that it's preferred
            // if ni and il coexist in the same dir
            W("*.ni.dll"),
            W("*.dll"),
            W("*.ni.exe"),
            W("*.exe"),
            W("*.ni.winmd"),
            W("*.winmd")
        };
        if (m_tpaList.empty()) {
            std::wstring coreCLRDirectoryPath = m_coreCLRDirectoryPath;
            coreCLRDirectoryPath.append(W("\\"));
            AddFilesFromDirectoryToTPAList(coreCLRDirectoryPath, rgTPAExtensions, _countof(rgTPAExtensions));
        }
        // Add files from from coreLibsPath if it is a different path than our initial current root 
        std::wstring coreLibraries;
        coreLibraries.append(coreLibsPath);
        if (coreLibraries.compare(m_coreCLRDirectoryPath) != 0) {
            coreLibraries.append(W("\\"));
            AddFilesFromDirectoryToTPAList(coreLibraries, rgTPAExtensions, _countof(rgTPAExtensions));
        }
        
        return m_tpaList.c_str();
    }

    // Returns the path to the host module
    const WCHAR* GetHostPath() {
        return m_hostPath.c_str();
    }

    // Returns the path to the host module
    const WCHAR* GetHostExeName() {
        return m_hostExeName.c_str();
    }

    // Returns the ICLRRuntimeHost4 instance, loading it from CoreCLR.dll if necessary, or nullptr on failure.
    ICLRRuntimeHost4* GetCLRRuntimeHost() {
        if (!m_CLRRuntimeHost) {

            if (!m_coreCLRModule) {
                *m_log << W("Unable to load ") << coreCLRDll << Logger::endl;
                return nullptr;
            }

            *m_log << W("Finding GetCLRRuntimeHost(...)") << Logger::endl;

            FnGetCLRRuntimeHost pfnGetCLRRuntimeHost =
                (FnGetCLRRuntimeHost)::GetProcAddress(m_coreCLRModule, "GetCLRRuntimeHost");

            if (!pfnGetCLRRuntimeHost) {
                *m_log << W("Failed to find function GetCLRRuntimeHost in ") << coreCLRDll << Logger::endl;
                return nullptr;
            }

            *m_log << W("Calling GetCLRRuntimeHost(...)") << Logger::endl;

            HRESULT hr = pfnGetCLRRuntimeHost(IID_ICLRRuntimeHost4, (IUnknown**)&m_CLRRuntimeHost);
            if (FAILED(hr)) {
                *m_log 
                    << W("Failed to get ICLRRuntimeHost4 interface. ERRORCODE: ")
                    << Logger::hresult
                    << hr
                    << Logger::endl;
                return nullptr;
            }
        }

        return m_CLRRuntimeHost;
    }
};

VOID
SetGlobalHost (
    ICLRRuntimeHost4 *host
    )
{
    g_Host = host;
}

ICLRRuntimeHost4*
GetGlobalHost (
    VOID
    )
{
    return g_Host;
}

VOID
SetDomainId (
    IN CONST DWORD domainId
    )
{
    g_domainId = domainId;
}

DWORD
GetDomainId (
    VOID
    )
{
    return g_domainId;
}

std::shared_ptr<Logger>
GetLogger (
    VOID
    )
{
    if (g_Log == nullptr) {
        g_Log = std::make_shared<Logger>();
    }
    return g_Log;
}

INT32
PrintModules (
    VOID
    ) 
{
    HMODULE hMods[1024];
    HANDLE hProcess;
    DWORD cbNeeded;
    unsigned int i;
    DWORD processID = GetCurrentProcessId();

    // Print the process identifier.
    printf("\nProcess ID: %u\n", processID);

    // Get a handle to the process.
    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION |
        PROCESS_VM_READ,
        FALSE, processID);
    if (NULL == hProcess)
        return 1;

    // Get a list of all the modules in this process.
    if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded))
    {
        for (i = 0; i < (cbNeeded / sizeof(HMODULE)); i++)
        {
            TCHAR szModName[MAX_PATH];

            // Get the full path to the module's file.
            if (GetModuleFileNameEx(hProcess, hMods[i], szModName,
                sizeof(szModName) / sizeof(TCHAR)))
            {
                // Print the module name and handle value.
                wprintf(TEXT("\t%s (0x%llX)\n"), szModName, (UINT64)hMods[i]);
            }
        }
    }

    // Release the handle to the process.

    CloseHandle(hProcess);

    return 0;
}
// Creates the startup flags for the runtime, starting with the default startup
// flags and adding or removing from them based on environment variables. Only
// two environment variables are respected right now: serverGcVar, controlling
// Server GC, and concurrentGcVar, controlling Concurrent GC.
STARTUP_FLAGS
CreateStartupFlags (
    VOID
    )
{
    auto initialFlags =
        static_cast<STARTUP_FLAGS>(
            STARTUP_FLAGS::STARTUP_LOADER_OPTIMIZATION_SINGLE_DOMAIN |
            STARTUP_FLAGS::STARTUP_SINGLE_APPDOMAIN |
            STARTUP_FLAGS::STARTUP_CONCURRENT_GC);

    // server GC is off by default, concurrent GC is on by default.
    auto checkVariable = [&](STARTUP_FLAGS flag, const WCHAR *var) {
        WCHAR result[25];
        size_t outsize;
        if (_wgetenv_s(&outsize, result, 25, var) == 0 && outsize > 0) {
            // set the flag if the var is present and set to 1,
            // clear the flag if the var is present and set to 0.
            // Otherwise, ignore it.
            if (_wcsicmp(result, W("1")) == 0) {
                initialFlags = static_cast<STARTUP_FLAGS>(initialFlags | flag);
            }
            else if (_wcsicmp(result, W("0")) == 0) {
                initialFlags = static_cast<STARTUP_FLAGS>(initialFlags & ~flag);
            }
        }
    };

    checkVariable(STARTUP_FLAGS::STARTUP_SERVER_GC, serverGcVar);
    checkVariable(STARTUP_FLAGS::STARTUP_CONCURRENT_GC, concurrentGcVar);

    return initialFlags;
}

// Convert an integer value to it's hex string representation
template <typename I>
std::string
ConvertToHexString (
    I input, 
    size_t hex_len = sizeof(I) << 1
    )
{
    static const char* digits = "0123456789ABCDEF";
    std::string hex_string(hex_len, '0');
    for (size_t i = 0, j = (hex_len - 1) * 4; i < hex_len; ++i, j -= 4) {
        hex_string[i] = digits[(input >> j) & 0x0f];
    }
    return hex_string;
}

// Create a native function delegate for a function inside a .NET assembly
HRESULT
CreateAssemblyDelegate(
    IN CONST WCHAR  *assembly,
    IN CONST WCHAR  *type,
    IN CONST WCHAR  *entry,
    _Inout_  PVOID  *pfnDelegate
)
{
    HRESULT hr = E_HANDLE;

    auto host = GetGlobalHost();

    if (host != nullptr) {

        hr = host->CreateDelegate(
            GetDomainId(),
            assembly, // Target managed assembly
            type, // Target managed type
            entry, // Target entry point (static method)
            reinterpret_cast<INT_PTR*>(pfnDelegate));

        if (FAILED(hr) || *pfnDelegate == NULL)
        {
            *GetLogger() << W("Failed call to CreateDelegate. ERRORCODE: ") << Logger::hresult << hr << Logger::endl;
        }
    }

    return hr;
}

// Execute a method from a class located inside a .NET Core Library Assembly
HRESULT 
ExecuteAssemblyClassFunction (
    IN       Logger &log,
    IN CONST WCHAR  *assembly,
    IN CONST WCHAR  *type,
    IN CONST WCHAR  *entry,
    IN CONST BYTE   *arguments
    )
{
    HRESULT hr = E_HANDLE;
    RemoteEntryInfo EntryInfo;
    typedef void (STDMETHODCALLTYPE MainMethodFp)(const VOID* args);
    MainMethodFp *pfnDelegate = NULL;

    auto host = GetGlobalHost();

    if (host != nullptr) {

        EntryInfo.HostPID = GetCurrentProcessId();

        hr = host->CreateDelegate(
            GetDomainId(),
            assembly, // Target managed assembly
            type, // Target managed type
            entry, // Target entry point (static method)
            reinterpret_cast<INT_PTR*>(&pfnDelegate));

        if (FAILED(hr) || pfnDelegate == NULL)
        {
            log << W("Failed call to CreateDelegate. ERRORCODE: ") << Logger::hresult << hr << Logger::endl;
            return hr;
        }
        
        auto remoteArgs = reinterpret_cast<const RemoteFunctionArgs*>(arguments);
        if (remoteArgs != NULL) {

            // construct a hex string for the address of the EntryInfo parameter
            // which is passed to the .NET delegate function and execute the delegate
            EntryInfo.Args.UserData = remoteArgs->UserData;
            EntryInfo.Args.UserDataSize = remoteArgs->UserDataSize;

            auto paramString = ConvertToHexString(reinterpret_cast<LONGLONG>(&EntryInfo), 16);

            pfnDelegate(paramString.c_str());
        }
        else {
            // No arguments were supplied to pass to the delegate function so pass an
            // empty string
            pfnDelegate(NULL);
        }
    }
    return hr;
}

HRESULT
UnloadStopHost (
    IN Logger &log
    )
{
    HRESULT hr = E_HANDLE;
    int exitCode = -1;

    //-------------------------------------------------------------

    // Unload the AppDomain

    auto host = GetGlobalHost();
    if (host != nullptr) {

        log << W("Unloading the AppDomain") << Logger::endl;

        hr = host->UnloadAppDomain2(
            GetDomainId(),
            true,
            (int *)&exitCode);                          // Wait until done

        if (FAILED(hr)) {
            log << W("Failed to unload the AppDomain. ERRORCODE: ") << Logger::hresult << hr << Logger::endl;
            return hr;
        }

        log << W("App domain unloaded exit value = ") << exitCode << Logger::endl;

        //-------------------------------------------------------------
        // Stop the host

        log << W("Stopping the host") << Logger::endl;

        hr = host->Stop();

        if (FAILED(hr)) {
            log << W("Failed to stop the host. ERRORCODE: ") << Logger::hresult << hr << Logger::endl;
            return hr;
        }

        //-------------------------------------------------------------

        // Release the reference to the host

        log << W("Releasing ICLRRuntimeHost4") << Logger::endl;

        host->Release();
    }
    SetGlobalHost(nullptr);

    SetDomainId((DWORD)-1);

    return hr;
}

BOOLEAN
StartHost(
    IN CONST WCHAR   *dllPath,
    IN       Logger  &log,
    IN CONST BOOLEAN waitForDebugger,
    _Inout_  HRESULT &exitCode,
    IN CONST WCHAR   *coreRoot,
    IN CONST WCHAR   *coreLibraries
    )
{
    if (GetGlobalHost() != nullptr) {
        log << W(".NET Core runtime has already been started.") << Logger::endl;
        return false;
    }

    if (waitForDebugger)
    {
        if (!IsDebuggerPresent())
        {
            log << W("Waiting for the debugger to attach. Press any key to continue ...") << Logger::endl;
            getchar();
            if (IsDebuggerPresent())
            {
                log << "Debugger is attached." << Logger::endl;
            }
            else
            {
                log << "Debugger failed to attach." << Logger::endl;
            }
        }
    }

    // Assume failure
    exitCode = E_FAIL;

    HostEnvironment hostEnvironment(&log, coreRoot);

    //-------------------------------------------------------------

    // Find the specified dll. This is done using LoadLibrary so that
    // the OS library search semantics are used to find it.

    const WCHAR* dotnetAppName = dllPath;
    if (dotnetAppName == nullptr)
    {
        log << W("No assembly name specified.") << Logger::endl;
        exitCode = E_INVALIDARG;
        return false;
    }

    WCHAR targetAssembly[MAX_PATH];
    GetFullPathNameW(dllPath, MAX_PATH, targetAssembly, NULL);

    // Also note the directory the target library is in, as it will be referenced later.
    // The directory is determined by simply truncating the target app's full path
    // at the last path delimiter (\)
    WCHAR targetAssemblyPath[MAX_PATH];
    wcscpy_s(targetAssemblyPath, targetAssembly);
    size_t i = wcslen(targetAssemblyPath) - 1;
    while (i >= 0 && targetAssemblyPath[i] != L'\\') {
        i--;
    }
    targetAssemblyPath[i] = L'\0';

    std::wstring appPath = targetAssemblyPath;
    std::wstring appNiPath;
    std::wstring managedAssemblyFullName;
    std::wstring appLocalWinmetadata;

    managedAssemblyFullName.assign(targetAssembly);

    log << W("Loading: ") << managedAssemblyFullName << Logger::endl;

    appNiPath.assign(appPath);
    appNiPath.append(W("NI"));
    appNiPath.append(W(";"));
    appNiPath.append(appPath);

    // APP_PATHS
    // App paths are directories to probe in for assemblies which are not one of the well-known Framework assemblies
    // included in the TPA list.
    //
    // For this simple sample, we just include the directory the target application is in.
    // More complex hosts may want to also check the current working directory or other
    // locations known to contain application assets.
    WCHAR appPaths[MAX_PATH * 50];

    // Just use the targetAssembly provided by the user and remove the file name
    wcscpy_s(appPaths, targetAssemblyPath);

    std::wstring managedAssemblyDirectory = managedAssemblyFullName;

    // Search for the last backslash and terminate it there to keep just the directory path with trailing slash

    auto lastBackslash = managedAssemblyDirectory.find_last_of(W("\\"));
    managedAssemblyDirectory.resize(lastBackslash + 1);

    // NATIVE_DLL_SEARCH_DIRECTORIES
    // Native dll search directories are paths that the runtime will probe for native DLLs called via PInvoke

    std::wstring nativeDllSearchDirs(appPath);

    nativeDllSearchDirs.append(W(";"));
    nativeDllSearchDirs.append(managedAssemblyDirectory);

    if (wcslen(coreLibraries) != 0) {
        nativeDllSearchDirs.append(W(";"));
        nativeDllSearchDirs.append(coreLibraries);
    }
    nativeDllSearchDirs.append(W(";"));
    nativeDllSearchDirs.append(hostEnvironment.m_coreCLRDirectoryPath);

    // Start the .NET Core runtime

    ICLRRuntimeHost4 *host = GetGlobalHost() == nullptr ? 
        hostEnvironment.GetCLRRuntimeHost() : GetGlobalHost();
    if (!host) {
        log << W("Unable to get ICLRRuntimeHost4 handle") << Logger::endl;
        exitCode = E_HANDLE;
        return false;
    }

    SetGlobalHost(host);

    HRESULT hr;

    STARTUP_FLAGS flags = CreateStartupFlags();
    log << W("Setting ICLRRuntimeHost4 startup flags") << Logger::endl;
    log << W("Server GC enabled: ") << HAS_FLAG(flags, STARTUP_FLAGS::STARTUP_SERVER_GC) << Logger::endl;
    log << W("Concurrent GC enabled: ") << HAS_FLAG(flags, STARTUP_FLAGS::STARTUP_CONCURRENT_GC) << Logger::endl;

    // Default startup flags
 
    hr = host->SetStartupFlags(flags);
    if (FAILED(hr)) {
        log << W("Failed to set startup flags. ERRORCODE: ") << Logger::hresult << hr << Logger::endl;
        exitCode = hr;
        return false;
    }
    log << W("Starting ICLRRuntimeHost4") << Logger::endl;

    hr = host->Start();
    if (FAILED(hr)) {
        log << W("Failed to start CoreCLR. ERRORCODE: ") << Logger::hresult << hr << Logger::endl;
        exitCode = hr;
        return false;
    }    

    std::wstring tpaList;
    if (!managedAssemblyFullName.empty())
    {
        // Target assembly should be added to the tpa list. 
        // Otherwise the wrong assembly could be executed.
        // Details can be found at https://github.com/dotnet/coreclr/issues/5631
        tpaList = managedAssemblyFullName;
        tpaList.append(W(";"));
    }
    if (wcslen(coreLibraries) != 0) {
        tpaList.append(hostEnvironment.GetTpaList(coreLibraries));
    }
    tpaList.append(hostEnvironment.GetTpaList(coreRoot));

    //-------------------------------------------------------------

    // Create an AppDomain

    // Allowed property names:
    // APPBASE
    // - The base path of the application from which the application and other assemblies will be loaded
    //
    // TRUSTED_PLATFORM_ASSEMBLIES
    // - The list of complete paths to each of the fully trusted assemblies
    //
    // APP_PATHS
    // - The list of paths which will be probed by the assembly loader
    //
    // APP_NI_PATHS
    // - The list of additional paths that the assembly loader will probe for ngen images
    //
    // NATIVE_DLL_SEARCH_DIRECTORIES
    // - The list of paths that will be probed for native DLLs called by PInvoke
    //
    const WCHAR *property_keys[] = {
        W("TRUSTED_PLATFORM_ASSEMBLIES"),
        W("APP_PATHS"),
        W("APP_NI_PATHS"),
        W("NATIVE_DLL_SEARCH_DIRECTORIES")
    };
    const WCHAR *property_values[] = {
        // TRUSTED_PLATFORM_ASSEMBLIES
        tpaList.c_str(),
        // APP_PATHS
        appPath.c_str(),
        // APP_NI_PATHS
        appNiPath.c_str(),
        // NATIVE_DLL_SEARCH_DIRECTORIES
        nativeDllSearchDirs.c_str()
    };

    log << W("Creating an AppDomain") << Logger::endl;
    for (int idx = 0; idx < sizeof(property_keys) / sizeof(WCHAR*); idx++)
    {
        log << property_keys[idx] << W("=") << property_values[idx] << Logger::endl;
    }

    DWORD domainId;

    hr = host->CreateAppDomainWithManager(
        hostEnvironment.GetHostExeName(),   // The friendly name of the AppDomain
        // Flags:
        // APPDOMAIN_ENABLE_PLATFORM_SPECIFIC_APPS
        // - By default CoreCLR only allows platform neutral assembly to be run. To allow
        //   assemblies marked as platform specific, include this flag
        //
        // APPDOMAIN_ENABLE_PINVOKE_AND_CLASSIC_COMINTEROP
        // - Allows sandboxed applications to make P/Invoke calls and use COM interop
        //
        // APPDOMAIN_SECURITY_SANDBOXED
        // - Enables sandboxing. If not set, the app is considered full trust
        //
        // APPDOMAIN_IGNORE_UNHANDLED_EXCEPTION
        // - Prevents the application from being torn down if a managed exception is unhandled
        //
        APPDOMAIN_ENABLE_PLATFORM_SPECIFIC_APPS |
        APPDOMAIN_ENABLE_PINVOKE_AND_CLASSIC_COMINTEROP |
        APPDOMAIN_DISABLE_TRANSPARENCY_ENFORCEMENT,
        NULL,                // Name of the assembly that contains the AppDomainManager implementation
        NULL,                    // The AppDomainManager implementation type name
        sizeof(property_keys) / sizeof(WCHAR*),  // The number of properties
        property_keys,
        property_values,
        &domainId);

    if (FAILED(hr)) {

        log << W("Failed call to CreateAppDomainWithManager. ERRORCODE: ") << Logger::hresult << hr << Logger::endl;
        exitCode = hr;
        return false;
    }

    exitCode = NOERROR;
    SetDomainId(domainId);

    return TRUE;
}

HRESULT
ValidateArgument (
    IN CONST WCHAR *argument,
    IN CONST DWORD maxSize
    )
{
    if (argument != nullptr) {
        const size_t dirLength = wcslen(argument);
        if (dirLength >= maxSize) {
            return E_INVALIDARG;
        }
    }
    else {
        return E_INVALIDARG;
    }
    return S_OK;
}

HRESULT
ValidateAssemblyFunctionCallArgs (
    IN CONST AssemblyFunctionCall *args
    ) 
{
    if (args != nullptr) {
        if (SUCCEEDED(ValidateArgument(args->Assembly, FunctionNameSize))
            && SUCCEEDED(ValidateArgument(args->Class, FunctionNameSize))
            && SUCCEEDED(ValidateArgument(args->Function, FunctionNameSize))) {
            return S_OK;
        }
    }
    return E_INVALIDARG;
}

HRESULT 
ValidateBinaryLoaderArgs (
    IN CONST BinaryLoaderArgs *args
    )
{
    if (args != nullptr) {
        if (SUCCEEDED(ValidateArgument(args->BinaryFilePath, MAX_PATH))
            && SUCCEEDED(ValidateArgument(args->CoreRootPath, MAX_PATH))
            && SUCCEEDED(ValidateArgument(args->CoreLibrariesPath, MAX_PATH))) {
            return S_OK;
        }
    }

    return E_INVALIDARG;
}

// Start the .NET Core runtime in the current application
HRESULT
StartCoreCLRInternal (
    IN CONST WCHAR   *dllPath,
    IN CONST BOOLEAN verbose,
    IN CONST BOOLEAN waitForDebugger,
    IN CONST WCHAR   *coreRoot,
    IN CONST WCHAR   *coreLibraries
    )
{
    HRESULT exitCode = -1;

    auto log = GetLogger();
    if (verbose) {
        log->Enable();
    }
    else {
        log->Disable();
    }

    const BOOLEAN success =
        StartHost(
            dllPath,
            *log,
            waitForDebugger,
            exitCode, 
            coreRoot,
            coreLibraries);

    *log << W("Execution ") << (success ? W("succeeded") : W("failed")) << Logger::endl;
    
    return exitCode;
}
// Host the .NET Core runtime in the current application
DllApi
HRESULT
StartCoreCLR(
    IN CONST BinaryLoaderArgs *args
    )
{
    if (SUCCEEDED(ValidateBinaryLoaderArgs(args))) {
        return StartCoreCLRInternal(
            args->BinaryFilePath,
            args->Verbose,
            args->WaitForDebugger,
            args->CoreRootPath,
            args->CoreLibrariesPath);
    }
    return E_INVALIDARG;
}

// Execute a function located in a .NET assembly by creating a native delegate
DllApi
HRESULT
ExecuteAssemblyFunction (
    IN CONST AssemblyFunctionCall *args
    )
{
    if (SUCCEEDED(ValidateAssemblyFunctionCallArgs(args))) {

        return ExecuteAssemblyClassFunction(*GetLogger(),
            args->Assembly,
            args->Class,
            args->Function,
            args->Arguments);
    }
    return E_INVALIDARG;
}

// Shutdown the .NET Core runtime
DllApi
HRESULT
UnloadRunTime(
    VOID
    ) 
{
    return UnloadStopHost(*GetLogger());
}

BOOLEAN
WINAPI
DllMain(
    VOID
    )
{
    return TRUE;
}
