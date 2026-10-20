#include "native_module.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

namespace {
#if defined(_WIN32)
void* pl_dlopen(const std::filesystem::path& p) {
    return LoadLibraryExW(p.wstring().c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}
void* pl_dlsym(void* h, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h), name));
}
void pl_dlclose(void* h) {
    FreeLibrary(static_cast<HMODULE>(h));
}
std::string pl_dlerror() {
    char buf[256]{};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        GetLastError(), 0, buf, sizeof(buf), nullptr);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
}
#else
#include <dlfcn.h>
void* pl_dlopen(const std::filesystem::path& p) {
    return dlopen(p.c_str(), RTLD_LAZY | RTLD_LOCAL);
}
void* pl_dlsym(void* h, const char* name) {
    return dlsym(h, name);
}
void pl_dlclose(void* h) {
    dlclose(h);
}
std::string pl_dlerror() {
    const char* e = dlerror();
    return e ? e : "(unknown error)";
}
#endif
}  // namespace

namespace dusk::mods::loader {
NativeModule::NativeModule() noexcept : handle(nullptr) {}

NativeModule::NativeModule(NativeModule&& other) noexcept {
    handle = other.handle;
    other.handle = nullptr;
}

NativeModule& NativeModule::operator=(NativeModule&& other) noexcept {
    handle = other.handle;
    other.handle = nullptr;
    return *this;
}

NativeModule::NativeModule(const std::filesystem::path& path) {
    handle = pl_dlopen(path);
    if (!handle) {
        throw std::runtime_error(pl_dlerror());
    }
}

NativeModule::~NativeModule() {
    if (handle) {
        pl_dlclose(handle);
    }
}

void* NativeModule::LookupSymbol(const char* name) const {
    return pl_dlsym(handle, name);
}

unsigned long current_process_id() {
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

bool process_running(unsigned long pid) {
#if defined(_WIN32)
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}
}  // namespace dusk::mods::loader
