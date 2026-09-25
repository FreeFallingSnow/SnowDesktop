#include "tray_protocol.h"
#include <commctrl.h>
#include <memory>
#include <new>

namespace snowdesktop::tray
{
namespace
{
constexpr UINT_PTR kSubclass = 0x53445452;
struct Pending : Notification
{
    // Do not copy a 16 KiB pixel array in the Explorer callback.
    HICON icon = nullptr;
};
struct Collector
{
    HWND window = nullptr;
    HANDLE mapping = nullptr, signal = nullptr, wake = nullptr, owner = nullptr;
    SharedState* shared = nullptr;
    SRWLOCK lock = SRWLOCK_INIT;
    std::array<Pending, 256> queue{};
    std::size_t head = 0, tail = 0;
    volatile LONG stopping = 0;
    ~Collector()
    {
        for (auto& event : queue) if (event.icon) DestroyIcon(event.icon);
        if (shared) UnmapViewOfFile(shared);
        for (HANDLE handle : {mapping, signal, wake, owner}) if (handle) CloseHandle(handle);
    }
    void Lost()
    { InterlockedIncrement(&shared->resync); SetEvent(signal); }
    void Push(Pending event)
    {
        if (!TryAcquireSRWLockExclusive(&lock))
        { if (event.icon) DestroyIcon(event.icon); Lost(); return; }
        const auto next = (head + 1) % queue.size();
        const bool full = next == tail;
        if (!full) { queue[head] = event; head = next; }
        ReleaseSRWLockExclusive(&lock);
        if (full) { if (event.icon) DestroyIcon(event.icon); Lost(); }
        else SetEvent(wake);
    }
    bool Pop(Pending& event)
    {
        AcquireSRWLockExclusive(&lock);
        const bool available = head != tail;
        if (available) { event = queue[tail]; queue[tail].icon = nullptr; tail = (tail + 1) % queue.size(); }
        ReleaseSRWLockExclusive(&lock);
        return available;
    }
};

bool CopyBytes(const void* source, void* destination, SIZE_T length)
{
    SIZE_T read = 0;
    return source && ReadProcessMemory(GetCurrentProcess(), source, destination, length, &read) && read == length;
}
void Pixels(HICON icon, Event& event)
{
    if (!icon) return;
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return;
    BITMAP bitmap{};
    const bool color = info.hbmColor != nullptr;
    const bool valid = GetObjectW(color ? info.hbmColor : info.hbmMask, sizeof(bitmap), &bitmap) != 0;
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    if (!valid || bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0) return;
    const int width = (std::min)(bitmap.bmWidth, static_cast<LONG>(kIconSize));
    const int height = (std::min)(bitmap.bmHeight / (color ? 1 : 2), static_cast<LONG>(kIconSize));
    if (height <= 0) return;
    BITMAPINFO dib{};
    dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dib.bmiHeader.biWidth = width; dib.bmiHeader.biHeight = -height;
    dib.bmiHeader.biPlanes = 1; dib.bmiHeader.biBitCount = 32; dib.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP image = CreateDIBSection(dc, &dib, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dc && image && bits)
    {
        HGDIOBJ old = SelectObject(dc, image);
        const auto count = static_cast<std::size_t>(width) * height;
        std::memset(bits, 0, count * 4);
        if (DrawIconEx(dc, 0, 0, icon, width, height, 0, nullptr, DI_NORMAL))
        {
            GdiFlush();
            std::copy_n(static_cast<std::uint32_t*>(bits), count, event.pixels.begin());
            const bool alpha = std::any_of(event.pixels.begin(), event.pixels.begin() + count,
                [](auto pixel) { return (pixel >> 24) != 0; });
            if (!alpha)
            {
                // Mask icons (including opaque black pixels) need the AND mask;
                // derive it by rendering against white as well as black.
                std::fill_n(static_cast<std::uint32_t*>(bits), count, 0x00ffffffu);
                DrawIconEx(dc, 0, 0, icon, width, height, 0, nullptr, DI_NORMAL);
                GdiFlush();
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto black = event.pixels[i] & 255, white = static_cast<std::uint32_t*>(bits)[i] & 255;
                    const auto a = 255 - (white >= black ? white - black : 0);
                    event.pixels[i] = (event.pixels[i] & 0xffffff) | (a << 24);
                }
            }
            event.width = static_cast<DWORD>(width); event.height = static_cast<DWORD>(height);
        }
        SelectObject(dc, old);
    }
    if (image) DeleteObject(image);
    if (dc) DeleteDC(dc);
}

void BootstrapClassic(Collector& collector)
{
    // Optional compatibility supplement. Read only Explorer-owned toolbars;
    // private pointers are copied with ReadProcessMemory, never dereferenced.
    EnumChildWindows(collector.window, [](HWND child, LPARAM parameter) -> BOOL {
        auto& self = *reinterpret_cast<Collector*>(parameter);
        wchar_t name[64]{}; GetClassNameW(child, name, 64);
        if (wcscmp(name, L"ToolbarWindow32") != 0) return TRUE;
        DWORD_PTR result = 0;
        if (!SendMessageTimeoutW(child, TB_BUTTONCOUNT, 0, 0, SMTO_ABORTIFHUNG, 200, &result)) return TRUE;
        const auto count = (std::min)(result, static_cast<DWORD_PTR>(512));
        for (DWORD_PTR i = 0; i < count && !Read(self.stopping) && !Read(self.shared->stop); ++i)
        {
            TBBUTTON button{};
            if (!SendMessageTimeoutW(child, TB_GETBUTTON, i, reinterpret_cast<LPARAM>(&button),
                    SMTO_ABORTIFHUNG, 200, &result) || !result || !button.dwData) continue;
            struct ClassicItem { HWND window; UINT id, callback, state, version; HICON icon; } item{};
            if (!CopyBytes(reinterpret_cast<void*>(button.dwData), &item, sizeof(item)) || !IsWindow(item.window)) continue;
            Event event;
            event.epoch = static_cast<std::uint64_t>(Read(self.shared->epoch));
            event.operation = NIM_ADD;
            event.flags = NIF_MESSAGE | NIF_ICON | NIF_STATE;
            event.identity.window = reinterpret_cast<std::uint64_t>(item.window); event.identity.id = item.id;
            GetWindowThreadProcessId(item.window, &event.identity.process);
            event.callback = item.callback; event.state = item.state; event.stateMask = NIS_HIDDEN;
            if (auto copy = CopyIcon(item.icon)) { Pixels(copy, event); DestroyIcon(copy); }
            Publish(*self.shared, event);
            event.operation = NIM_SETVERSION; event.version = item.version;
            Publish(*self.shared, event);
        }
        SetEvent(self.signal);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&collector));
}

DWORD WINAPI Worker(void* parameter)
{
    std::unique_ptr<std::shared_ptr<Collector>> holder(static_cast<std::shared_ptr<Collector>*>(parameter));
    auto self = *holder;
    InterlockedExchange(&self->shared->ready, 1);
    SetEvent(self->signal);
    BootstrapClassic(*self);
    LONG64 epoch = Read(self->shared->epoch);
    HANDLE handles[]{self->owner, self->wake};
    while (!Read(self->stopping) && !Read(self->shared->stop))
    {
        if (Read(self->shared->epoch) != epoch)
        {
            epoch = Read(self->shared->epoch);
            BootstrapClassic(*self);
        }
        Pending pending;
        while (self->Pop(pending))
        {
            if (pending.epoch == static_cast<std::uint64_t>(Read(self->shared->epoch)))
            {
                Event event;
                event.epoch = pending.epoch; event.operation = pending.operation;
                event.flags = pending.flags; event.callback = pending.callback;
                event.state = pending.state; event.stateMask = pending.stateMask;
                event.version = pending.version; event.identity = pending.identity;
                std::copy_n(pending.tip, std::size(event.tip), event.tip);
                Pixels(pending.icon, event);
                Publish(*self->shared, event);
                SetEvent(self->signal);
            }
            if (pending.icon) DestroyIcon(pending.icon);
        }
        if (WaitForMultipleObjects(2, handles, FALSE, 1000) == WAIT_OBJECT_0) break;
    }
    // The subclass owns another shared_ptr until its UI thread detaches. It
    // stays valid even if a hung Explorer delays this message past our exit.
    PostMessageW(self->window, RegisterWindowMessageW(kDetachMessage), self->shared->owner, 0);
    return 0;
}

LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
{
    auto* holder = reinterpret_cast<std::shared_ptr<Collector>*>(data);
    auto& self = **holder;
    if ((message == RegisterWindowMessageW(kDetachMessage) && wp == self.shared->owner) || message == WM_NCDESTROY)
    {
        InterlockedExchange(&self.stopping, 1); SetEvent(self.wake);
        RemoveWindowSubclass(window, Procedure, kSubclass);
        delete holder;
        return DefSubclassProc(window, message, wp, lp);
    }
    if (message == WM_COPYDATA && lp && !Read(self.stopping) && !Read(self.shared->stop))
    {
        const auto* copy = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (copy->dwData == 1)
        {
            InterlockedIncrement(&self.shared->received);
            InterlockedExchange(&self.shared->lastSize, static_cast<LONG>(copy->cbData));
            // Decode into a stack-local wire copy. Pixel conversion and all IPC
            // are on Worker; the UI callback only keeps an independent HICON.
            ShellTrayData wire{};
            Notification decoded;
            HICON icon = nullptr;
            if (copy->cbData <= sizeof(wire) && CopyBytes(copy->lpData, &wire, copy->cbData) &&
                Decode(&wire, copy->cbData, decoded, icon))
            {
                InterlockedIncrement(&self.shared->decoded);
                Pending pending;
                pending.epoch = static_cast<std::uint64_t>(Read(self.shared->epoch));
                pending.operation = decoded.operation; pending.flags = decoded.flags;
                pending.callback = decoded.callback; pending.state = decoded.state;
                pending.stateMask = decoded.stateMask; pending.version = decoded.version;
                pending.identity = decoded.identity;
                GetWindowThreadProcessId(reinterpret_cast<HWND>(pending.identity.window), &pending.identity.process);
                std::copy_n(decoded.tip, std::size(pending.tip), pending.tip);
                if (icon) pending.icon = CopyIcon(icon);
                self.Push(pending);
            }
            else if (copy->cbData >= sizeof(DWORD) * 2 && wire.operation != NIM_SETFOCUS)
            { InterlockedIncrement(&self.shared->rejected); self.Lost(); }
        }
        else if (copy->dwData == 3 && copy->cbData == sizeof(IconIdentifier32))
        {
            IconIdentifier32 wire{};
            if (CopyBytes(copy->lpData, &wire, sizeof(wire)) && (wire.message == 1 || wire.message == 2))
            {
                Identity identity{wire.guid, wire.window, wire.id, 0};
                GetWindowThreadProcessId(reinterpret_cast<HWND>(identity.window), &identity.process);
                RECT rect{};
                if (LookupGeometry(*self.shared, identity, rect))
                    return wire.message == 1 ? MAKELONG(rect.left, rect.top) : MAKELONG(rect.right, rect.bottom);
            }
        }
    }
    return DefSubclassProc(window, message, wp, lp);
}

void Attach(HWND window, DWORD owner)
{
    DWORD_PTR existing = 0;
    if (GetWindowSubclass(window, Procedure, kSubclass, &existing))
    {
        auto& current = **reinterpret_cast<std::shared_ptr<Collector>*>(existing);
        if (current.shared->owner != owner || Read(current.shared->stop))
            PostMessageW(window, RegisterWindowMessageW(kDetachMessage), current.shared->owner, 0);
        return;
    }
    auto self = std::make_shared<Collector>();
    self->window = window;
    self->mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, ObjectName(owner, L"State").c_str());
    if (!self->mapping) return;
    self->shared = static_cast<SharedState*>(MapViewOfFile(self->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!self->shared || self->shared->magic != kMagic || self->shared->version != kVersion ||
        self->shared->size != sizeof(SharedState) || self->shared->owner != owner || Read(self->shared->stop)) return;
    self->owner = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner);
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!self->owner || !GetProcessTimes(self->owner, &creation, &exit, &kernel, &user) ||
        (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32 | creation.dwLowDateTime) != self->shared->ownerCreation) return;
    self->signal = OpenEventW(EVENT_MODIFY_STATE, FALSE, ObjectName(owner, L"Signal").c_str());
    self->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!self->signal || !self->wake) return;
    // Keep code resident while subclass callbacks may still be in flight.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&Procedure), &pinned)) return;
    auto* subclassHolder = new(std::nothrow) std::shared_ptr<Collector>(self);
    if (!subclassHolder) return;
    if (!SetWindowSubclass(window, Procedure, kSubclass, reinterpret_cast<DWORD_PTR>(subclassHolder)))
    { delete subclassHolder; return; }
    self->shared->explorer = GetCurrentProcessId();
    auto* workerHolder = new(std::nothrow) std::shared_ptr<Collector>(self);
    HANDLE worker = workerHolder ? CreateThread(nullptr, 0, Worker, workerHolder, 0, nullptr) : nullptr;
    if (!worker)
    {
        delete workerHolder;
        RemoveWindowSubclass(window, Procedure, kSubclass); delete subclassHolder;
        return;
    }
    CloseHandle(worker);
}
}
}

extern "C" __declspec(dllexport) LRESULT CALLBACK
SnowDesktopTrayHookProc(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0 && lp)
    {
        const auto& message = *reinterpret_cast<const CWPSTRUCT*>(lp);
        if (message.message == RegisterWindowMessageW(snowdesktop::tray::kAttachMessage))
        {
            wchar_t name[64]{}; GetClassNameW(message.hwnd, name, 64);
            if (wcscmp(name, L"Shell_TrayWnd") == 0)
                try { snowdesktop::tray::Attach(message.hwnd, static_cast<DWORD>(message.wParam)); }
                catch (...) { /* A failed optional collector must not unwind through Explorer. */ }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
