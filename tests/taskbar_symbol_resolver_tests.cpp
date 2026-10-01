#include "taskbar_hook/taskbar_symbol_resolver.h"
#include "taskbar_hook/taskbar_hook_protocol.h"
#include "taskbar_hook/taskbar_autohide_rules.h"
#include <shellapi.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
using namespace snowdesktop::taskbar_hook;
struct ImageFixture
{
    std::vector<BYTE> bytes = std::vector<BYTE>(0x2000);
    AutoHideAdapter adapter;
    template<class T> void Put(std::size_t at, const T& value) { std::memcpy(bytes.data() + at, &value, sizeof(value)); }
    ImageFixture(DWORD shift = 0)
    {
        IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 0x80; Put(0, dos);
        IMAGE_NT_HEADERS64 nt{};
        nt.Signature = IMAGE_NT_SIGNATURE; nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt.FileHeader.NumberOfSections = 1; nt.FileHeader.TimeDateStamp = 0xe8e4b3d1 + shift;
        nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt.OptionalHeader.SizeOfImage = static_cast<DWORD>(bytes.size());
        nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG] = {0x300, sizeof(IMAGE_DEBUG_DIRECTORY)};
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION] = {0x500, 8 * sizeof(RUNTIME_FUNCTION)};
        Put(0x80, nt);
        IMAGE_SECTION_HEADER code{}; code.VirtualAddress = 0x800; code.Misc.VirtualSize = 0x1000;
        code.Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE; Put(0x80 + sizeof(nt), code);
        IMAGE_DEBUG_DIRECTORY debug{}; debug.Type = IMAGE_DEBUG_TYPE_CODEVIEW;
        debug.AddressOfRawData = 0x340; debug.SizeOfData = 24; Put(0x300, debug);
        const DWORD signature = 0x53445352, age = 1;
        GUID guid{0xa5dd95b + shift, 0xef53, 0x208b, {0x28, 0xb2, 0xf3, 0xd8, 0x58, 0x6e, 0x64, 0x26}};
        Put(0x340, signature); Put(0x344, guid); Put(0x354, age);
        adapter.image = {nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage, guid, age};
        for (std::size_t i = 0; i < adapter.functions.size(); ++i)
        {
            const DWORD begin = 0x800 + static_cast<DWORD>(i) * 0x80 + shift;
            RUNTIME_FUNCTION function{begin, begin + 0x40, 0x600}; Put(0x500 + i * sizeof(function), function);
            AutoHideFunction expected{begin, function.EndAddress};
            expected.entry.fill(static_cast<BYTE>(0x40 + i)); Put(begin, expected.entry);
            adapter.functions[i] = expected;
        }
    }
    AutoHideImageView View() const { return AutoHideImageView(bytes); }
};
}

// Only the symbol helper process is substituted here. The production parent
// still creates an inherited mapping, restricted handle list, job and wait.
// No desktop host, Explorer injection or live taskbar interaction is started.
std::optional<int> TryRunTaskbarSymbolTestHelper()
{
    int count = 0;
    auto** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return {};
    struct Free { wchar_t** args; ~Free() { LocalFree(args); } } free{args};
    if (count < 2 || std::wcscmp(args[1], kSymbolHelperCommand)) return {};
    if (count != 4) return ERROR_INVALID_PARAMETER;
    const std::filesystem::path root(args[3]);
    const auto mode = root.filename().wstring();
    if (mode == L"crash") return ERROR_GEN_FAILURE;
    if (mode == L"timeout" || mode == L"cancel")
    {
        { std::ofstream pid(root / L"pid.txt"); pid << GetCurrentProcessId(); }
        const auto name = L"Local\\SnowDesktop.SymbolTest." + root.parent_path().filename().wstring();
        const HANDLE started = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
        if (started) { SetEvent(started); CloseHandle(started); }
        WaitForSingleObject(GetCurrentProcess(), INFINITE); // Parent's bounded job owns termination.
        return ERROR_GEN_FAILURE;
    }
    const auto handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::wcstoull(args[2], nullptr, 10)));
    auto* result = static_cast<AutoHideResolution*>(MapViewOfFile(handle, FILE_MAP_WRITE, 0, 0, sizeof(AutoHideResolution)));
    if (!result) return ERROR_INVALID_HANDLE;
    *result = AutoHideResolution{.error = ERROR_SUCCESS, .adapter = ImageFixture(0x30).adapter};
    if (mode == L"corrupt") result->magic = 0;
    UnmapViewOfFile(result);
    return ERROR_SUCCESS;
}

int RunTaskbarSymbolResolverTests()
{
    int failures = 0;
    auto check = [&](bool ok, const char* text) { if (!ok) { ++failures; std::cerr << "FAIL: " << text << '\n'; } };
    ImageFixture old, updated(0x30);
    check(old.View().Validate(old.adapter) && updated.View().Validate(updated.adapter),
        "matching symbol adapters accept changed timestamps, PDB GUIDs and function addresses");
    check(!updated.View().Validate(old.adapter), "an old adapter must not hook an updated or differently loaded image");
    auto invalid = updated.adapter;
    invalid.image.age++;
    check(!updated.View().Validate(invalid), "a different PDB age cannot provide hook addresses");
    invalid = updated.adapter; invalid.functions[1] = {};
    check(!updated.View().Validate(invalid), "a missing secondary reveal symbol must disable all native filtering");
    invalid = updated.adapter; invalid.functions[1].entry[3] ^= 1;
    check(!updated.View().Validate(invalid), "stale prologues or another hook cannot receive an unsafe trampoline");
    invalid = updated.adapter; invalid.functions[1] = invalid.functions[0];
    check(!updated.View().Validate(invalid), "two symbols cannot silently resolve to the same function");
    invalid = updated.adapter; invalid.functions[6].end += 0x80;
    check(!updated.View().Validate(invalid), "an invented activation range must not suppress unrelated callers");
    ImageFixture nonExecutable(0x30);
    IMAGE_SECTION_HEADER section{};
    section.VirtualAddress = 0x800; section.Misc.VirtualSize = 0x1000; section.Characteristics = IMAGE_SCN_MEM_READ;
    nonExecutable.Put(0x80 + sizeof(IMAGE_NT_HEADERS64), section);
    check(!nonExecutable.View().Validate(updated.adapter), "data and non-executable pages cannot become hook targets");
    check(!AutoHideImageView(std::span<const BYTE>(updated.bytes.data(), 64)).Identity(),
        "truncated image headers must fail safely before any address lookup");
    ImageFixture badDebug;
    IMAGE_DEBUG_DIRECTORY debug{}; debug.Type = IMAGE_DEBUG_TYPE_CODEVIEW; debug.SizeOfData = 24;
    debug.AddressOfRawData = 0xfffffff0; badDebug.Put(0x300, debug);
    check(!badDebug.View().Identity(), "overflowing CodeView locations cannot establish image compatibility");

    for (bool secondary : {false, true})
    {
        const auto& procedure = updated.adapter.Get(secondary ? AutoHideSymbol::SecondaryWndProc : AutoHideSymbol::PrimaryWndProc);
        ActivationRevealContext context;
        context.protectedTaskbar = true; context.geometryValid = true; context.activation = WA_ACTIVE;
        context.monitor = {0, 0, 2560, 1440}; context.taskbar = {0, 1438, 2560, 1498}; context.cursor = {100, 100};
        context.secondary = secondary;
        context.callerIsActivationHandler = updated.adapter.IsActivationCaller(secondary, procedure.begin + 0x20);
        int calls = 0;
        check(DispatchActivationReveal(context, 0, 8, [&](int, int) { ++calls; }) && calls == 0,
            "a moved native WM_ACTIVATE caller still suppresses passive expansion on either screen");
        context.callerIsActivationHandler = updated.adapter.IsActivationCaller(secondary, procedure.end);
        check(!DispatchActivationReveal(context, 0, 8, [&](int, int) { ++calls; }) && calls == 1,
            "the end of the native procedure is excluded and unrelated callers fail open");
    }
    SharedState state;
    state.autoHideAdapter = updated.adapter; state.autoHideResolutionError = ERROR_SUCCESS;
    Snapshot snapshot;
    check(ReadSharedSnapshot(&state, snapshot) && updated.View().Validate(snapshot.autoHideAdapter) &&
        snapshot.autoHideResolutionError == ERROR_SUCCESS, "a coherent private snapshot carries the resolved image and all focus hooks");
    state.version--;
    check(!ReadSharedSnapshot(&state, snapshot), "an older host/Explorer protocol cannot supply an incomplete symbol adapter");

    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    const auto root = std::filesystem::temp_directory_path() /
        (L"SnowSymbols-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); } } cleanup{root};
    for (const auto* mode : {L"ok", L"crash", L"corrupt", L"timeout", L"cancel"})
        std::filesystem::create_directories(root / mode);
    auto resolved = RunTaskbarSymbolHelper(exe, root / L"ok", nullptr, 5000);
    check(resolved.error == ERROR_SUCCESS && updated.View().Validate(resolved.adapter),
        "production helper transport accepts a complete result without blocking a taskbar UI thread");
    check(RunTaskbarSymbolHelper(exe, root / L"crash", nullptr, 5000).error == ERROR_INVALID_DATA,
        "a helper exit without publishing cannot masquerade as a successful resolve");
    check(RunTaskbarSymbolHelper(exe, root / L"corrupt", nullptr, 5000).error == ERROR_INVALID_DATA,
        "a corrupt helper response must fail open");
    const auto eventName = L"Local\\SnowDesktop.SymbolTest." + root.filename().wstring();
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto stopped = [&](const std::filesystem::path& directory) {
        DWORD pid = 0; std::ifstream file(directory / L"pid.txt"); file >> pid;
        HANDLE child = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
        const bool exited = pid && (!child || WaitForSingleObject(child, 0) == WAIT_OBJECT_0);
        if (child) CloseHandle(child);
        return exited;
    };
    check(RunTaskbarSymbolHelper(exe, root / L"timeout", nullptr, 1000).error == ERROR_TIMEOUT && stopped(root / L"timeout"),
        "a stuck symbol download has a bounded deadline and leaves no helper process");
    ResetEvent(started);
    std::thread worker([&] { resolved = RunTaskbarSymbolHelper(exe, root / L"cancel", cancel, 5000); });
    const bool childStarted = WaitForSingleObject(started, 3000) == WAIT_OBJECT_0;
    SetEvent(cancel); worker.join();
    check(childStarted && resolved.error == ERROR_CANCELLED && stopped(root / L"cancel"),
        "host cancellation stops an in-flight helper and reaps it before worker destruction");
    check(RunTaskbarSymbolHelper(exe, root / L"ok", cancel, 5000).error == ERROR_CANCELLED,
        "an already cancelled host must not start a new helper");
    CloseHandle(started); CloseHandle(cancel);
    return failures;
}
