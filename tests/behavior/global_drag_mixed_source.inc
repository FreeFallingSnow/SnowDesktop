#pragma once

// Include after global_drag_ole_driver.inc. This composes actual Shell-backed
// file input with an independent content provider; no product format decision,
// target operation or layout change is performed here.
namespace drag_global::ole_driver { class V4MixedGraphExecutor; class V4ConfiguredContentGraphExecutor; }
namespace drag_global::mixed_fixture
{
class MixedBinding;
class MixedOwner;
enum class CallPhase { entered, returned, unwound };
struct Call
{
    bool query = false, enumeration = false, formatPresent = false, targetDevicePresent = false;
    CallPhase phase = CallPhase::entered;
    std::uint64_t call = 0, parent = 0;
    std::size_t depth = 0;
    CLIPFORMAT format = 0;
    DWORD aspect = 0, tymed = 0, thread = 0, direction = 0;
    LONG index = 0;
    HRESULT result = E_PENDING;
};

class Source final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject, IDataObjectAsyncCapability>
{
    friend class MixedOwner;
    friend class MixedBinding;
    friend MixedBinding PrepareMixedSource(const V4FamilyValidatedContext&, const event_graph::V4FamilyIdentityContext&,
        const std::shared_ptr<V4IndependentFilePayload>&, const V4IndependentShellSource&,
        const std::shared_ptr<V4IndependentShellOwner>&, const std::shared_ptr<MixedOwner>&, std::size_t, std::size_t);
public:
    DWORD OwnerThread() const noexcept { return ownerThread_; }
    bool ObservationFailed() const noexcept { return failed_.load(std::memory_order_acquire); }
    std::uintptr_t ContentProviderIdentity() const noexcept
    {
        const DWORD saved = GetLastError();
        const auto result = GetCurrentThreadId() == ownerThread_ && content_
            ? reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(content_.Get())) : 0;
        SetLastError(saved); return result;
    }
    bool CopyCalls(std::vector<Call>& output) const noexcept
    {
        const DWORD saved = GetLastError(); bool ok = false;
        try
        {
            if (GetCurrentThreadId() == ownerThread_)
            { std::lock_guard lock(callsMutex_); output = calls_; ok = !ObservationFailed() && depth_ == 0; }
        }
        catch (...) { ok = false; }
        SetLastError(saved); return ok;
    }
    // Local provider records only; the caller must still associate them with
    // the same production receipt and graph. This never invokes a provider.
    bool CopyProviderEvidence(std::vector<ole_file_source_fixture::FileDropRead>& fileReads,
        std::vector<ole_content_source_fixture::Call>& contentCalls) const noexcept
    {
        const DWORD saved = GetLastError(); bool ok = false;
        try
        {
            ole_file_source_fixture::FileDropCallState state;
            if (GetCurrentThreadId() == ownerThread_ && !ObservationFailed() && depth_ == 0 && files_ && content_ &&
                !files_->wrongApartment && !files_->fileDropObservationFailed && files_->fileReads >= 0 &&
                static_cast<std::size_t>(files_->fileReads) == files_->fileDropReads.size() &&
                files_->ReadFileDropCallState(state) && !state.failed && !state.depth && state.owner == ownerThread_ &&
                !content_->wrongApartment.load() && !content_->observationFailed.load())
            {
                std::lock_guard lock(content_->observationMutex);
                fileReads = files_->fileDropReads; contentCalls = content_->calls; ok = true;
            }
        }
        catch (...) { ok = false; }
        SetLastError(saved); return ok;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        CallScope scope(*this, true, format); return scope.Return(Route(format, nullptr, true));
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        CallScope scope(*this, false, format); return scope.Return(Route(format, medium, false));
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override
    { if (!output) return E_POINTER; output->ptd = nullptr; return DATA_S_SAMEFORMATETC; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** output) override
    {
        CallScope scope(*this, false, nullptr, true, direction);
        if (!OnOwner()) return scope.Return(RPC_E_WRONG_THREAD);
        if (!output) return scope.Return(E_POINTER);
        *output = nullptr;
        if (direction != DATADIR_GET) return scope.Return(E_NOTIMPL);
        try
        {
            std::vector<FORMATETC> formats;
            if (offerFiles_) formats.push_back(FORMATETC{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
            if (offerShellId_) formats.push_back(FORMATETC{shellIdFormat_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
            for (const auto& entry : content_->entries)
                formats.push_back(FORMATETC{entry.format, nullptr, DVASPECT_CONTENT, entry.index, entry.tymed});
            if (formats.size() > (std::numeric_limits<UINT>::max)()) return scope.Return(E_OUTOFMEMORY);
            return scope.Return(SHCreateStdEnumFmtEtc(static_cast<UINT>(formats.size()), formats.data(), output));
        }
        catch (...) { return scope.Return(E_OUTOFMEMORY); }
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL enabled) override
    { return OnOwner() ? files_->SetAsyncMode(enabled) : RPC_E_WRONG_THREAD; }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* enabled) override
    { return OnOwner() ? files_->GetAsyncMode(enabled) : RPC_E_WRONG_THREAD; }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx* context) override
    { return OnOwner() ? files_->StartOperation(context) : RPC_E_WRONG_THREAD; }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* active) override
    { return OnOwner() ? files_->InOperation(active) : RPC_E_WRONG_THREAD; }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx* context, DWORD effect) override
    { return OnOwner() ? files_->EndOperation(result, context, effect) : RPC_E_WRONG_THREAD; }
private:
    bool OnOwner() noexcept
    {
        if (GetCurrentThreadId() == ownerThread_) return true;
        failed_.store(true, std::memory_order_release); return false;
    }
    HRESULT Route(FORMATETC* format, STGMEDIUM* medium, bool query)
    {
        if (!OnOwner()) return RPC_E_WRONG_THREAD;
        if (!format || (!query && !medium)) return E_POINTER;
        if ((format->cfFormat == CF_HDROP && offerFiles_) || (format->cfFormat == shellIdFormat_ && offerShellId_))
        {
            // The candidate exposes only HGLOBAL for these formats. Preserve
            // the original request in our log but never ask Shell for an
            // unmodelled alternate medium when several bits were requested.
            if (!(format->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
            FORMATETC restricted = *format; restricted.tymed = TYMED_HGLOBAL;
            return query ? files_->QueryGetData(&restricted) : files_->GetData(&restricted, medium);
        }
        if (std::any_of(content_->entries.begin(), content_->entries.end(),
            [&](const auto& entry) { return entry.format == format->cfFormat; }))
            return query ? content_->QueryGetData(format) : content_->GetData(format, medium);
        return DV_E_FORMATETC;
    }
    void Append(const Call& call) noexcept
    {
        const DWORD saved = GetLastError();
        try
        {
            std::lock_guard lock(callsMutex_);
            if (calls_.size() >= 4096) failed_.store(true, std::memory_order_release);
            else calls_.push_back(call);
        }
        catch (...) { failed_.store(true, std::memory_order_release); }
        SetLastError(saved);
    }
    class CallScope final
    {
    public:
        CallScope(Source& source, bool query, const FORMATETC* format, bool enumeration = false, DWORD direction = 0) noexcept : source_(source)
        {
            const DWORD saved = GetLastError();
            if (!source_.OnOwner() || source_.nextCall_ == (std::numeric_limits<std::uint64_t>::max)() ||
                source_.depth_ == (std::numeric_limits<std::size_t>::max)())
            { source_.failed_.store(true, std::memory_order_release); SetLastError(saved); return; }
            active_ = true; event_.query = query; event_.enumeration = enumeration; event_.direction = direction; event_.call = ++source_.nextCall_;
            event_.parent = source_.currentCall_; event_.depth = source_.depth_++; event_.thread = source_.ownerThread_;
            if (format)
            { event_.formatPresent = true; event_.format = format->cfFormat; event_.aspect = format->dwAspect;
              event_.tymed = format->tymed; event_.index = format->lindex; event_.targetDevicePresent = format->ptd != nullptr; }
            source_.currentCall_ = event_.call; source_.Append(event_);
            if (!query && !enumeration && snowdesktop::external_transfer_observation::Enabled())
            { trace_ = true; Shared(snowdesktop::external_transfer_observation::Phase::mixed_provider_getdata_enter,
                event_.formatPresent ? static_cast<std::int64_t>(event_.format) : -1); }
            SetLastError(saved);
        }
        HRESULT Return(HRESULT result) noexcept
        {
            if (active_)
            {
                event_.phase = CallPhase::returned; event_.result = result; source_.Append(event_);
                if (trace_) Shared(snowdesktop::external_transfer_observation::Phase::mixed_provider_getdata_return,
                    static_cast<std::int64_t>(result));
                returned_ = true;
            }
            return result;
        }
        ~CallScope()
        {
            if (!active_) return;
            const DWORD saved = GetLastError();
            if (!returned_)
            {
                event_.phase = CallPhase::unwound; source_.Append(event_);
                if (trace_) Shared(snowdesktop::external_transfer_observation::Phase::mixed_provider_getdata_unwound,
                    static_cast<std::int64_t>(E_PENDING));
            }
            if (!source_.depth_ || source_.currentCall_ != event_.call) source_.failed_.store(true, std::memory_order_release);
            else { --source_.depth_; source_.currentCall_ = event_.parent; }
            SetLastError(saved);
        }
    private:
        void Shared(snowdesktop::external_transfer_observation::Phase phase, std::int64_t value) const noexcept
        {
            const DWORD saved = GetLastError();
            snowdesktop::external_transfer_observation::AdapterScopeIdentity scope{};
            snowdesktop::ole_adapter_observation::Event current;
            if (snowdesktop::ole_adapter_observation::Current(current))
                scope = {current.session, current.call, current.parent, current.adapter, current.thread,
                    current.depth, static_cast<unsigned>(current.kind), true};
            snowdesktop::external_transfer_observation::RecordAny(0, phase,
                reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(&source_)), value, event_.call, scope);
            SetLastError(saved);
        }
        Source& source_; Call event_; bool active_ = false, returned_ = false, trace_ = false;
    };
    DWORD ownerThread_ = GetCurrentThreadId();
    bool offerFiles_ = true, offerShellId_ = false;
    CLIPFORMAT shellIdFormat_ = 0;
    Microsoft::WRL::ComPtr<ole_file_source_fixture::Source> files_;
    Microsoft::WRL::ComPtr<ole_content_source_fixture::Source> content_;
    mutable std::mutex callsMutex_;
    std::vector<Call> calls_;
    std::atomic_bool failed_{false};
    std::uint64_t nextCall_ = 0, currentCall_ = 0;
    std::size_t depth_ = 0;
};

class MixedOwner final
{
    friend struct ::NativeDragIntegrationAccess;
    friend class MixedBinding;
    friend class ::drag_global::ole_driver::V4MixedGraphExecutor;
    friend MixedBinding PrepareMixedSource(const V4FamilyValidatedContext&, const event_graph::V4FamilyIdentityContext&,
        const std::shared_ptr<V4IndependentFilePayload>&, const V4IndependentShellSource&,
        const std::shared_ptr<V4IndependentShellOwner>&, const std::shared_ptr<MixedOwner>&, std::size_t, std::size_t);
public:
    MixedOwner(const MixedOwner&) = delete;
    MixedOwner& operator=(const MixedOwner&) = delete;
    ~MixedOwner()
    { if (source_) { (void)source_.Detach(); (void)retainedFileOwner_.release(); } }
private:
    MixedOwner() noexcept : thread_(GetCurrentThreadId()) {}
    bool Live() const noexcept
    {
        if (!active_.load(std::memory_order_acquire) || GetCurrentThreadId() != thread_) return false;
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        return SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) &&
            (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA);
    }
    bool Begin(const std::shared_ptr<V4IndependentShellOwner>& fileOwner, bool preparing) noexcept
    {
        if (!Live() || !fileOwner || (preparing ? static_cast<bool>(source_) :
            (!source_ || !retainedFileOwner_ || *retainedFileOwner_ != fileOwner))) return false;
        bool expected = false;
        if (!executing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
        if (!fileOwner->TryBeginExecution()) { executing_.store(false, std::memory_order_release); return false; }
        activeFileOwner_ = fileOwner; return true;
    }
    void End() noexcept
    {
        if (activeFileOwner_) activeFileOwner_->EndExecution();
        activeFileOwner_.reset(); executing_.store(false, std::memory_order_release);
    }
    bool CanInvalidate() const noexcept
    { return GetCurrentThreadId() == thread_ && !executing_.load(std::memory_order_acquire); }
    bool FileOwnerLive() const noexcept
    { return retainedFileOwner_ && *retainedFileOwner_ && (*retainedFileOwner_)->Live(); }
    void Invalidate() noexcept
    {
        const DWORD saved = GetLastError();
        if (CanInvalidate()) { active_.store(false, std::memory_order_release); source_.Reset(); retainedFileOwner_.reset(); }
        SetLastError(saved);
    }
    bool Owns(const std::shared_ptr<V4IndependentShellOwner>& owner,
        const std::shared_ptr<V4IndependentFilePayload>& payload, const ole_file_source_fixture::Source* source) const noexcept
    { return owner && owner->payload_ == payload && owner->source_.Get() == source; }
    DWORD thread_ = 0;
    std::atomic_bool active_{true}, executing_{false};
    std::shared_ptr<V4IndependentShellOwner> activeFileOwner_;
    Microsoft::WRL::ComPtr<Source> source_;
    std::unique_ptr<std::shared_ptr<V4IndependentShellOwner>> retainedFileOwner_;
};

class MixedBinding final
{
    friend class ::drag_global::ole_driver::V4MixedGraphExecutor;
    friend MixedBinding PrepareMixedSource(const V4FamilyValidatedContext&, const event_graph::V4FamilyIdentityContext&,
        const std::shared_ptr<V4IndependentFilePayload>&, const V4IndependentShellSource&,
        const std::shared_ptr<V4IndependentShellOwner>&, const std::shared_ptr<MixedOwner>&, std::size_t, std::size_t);
public:
    bool inputPrepared = false, resourceUnavailable = false;
    std::string candidateId;
    std::vector<std::string> unresolved;
    Source* SourceObject() const noexcept
    {
        const auto owner = owner_.lock();
        return owner && owner->Live() && owner->FileOwnerLive() && owner->source_.Get() == source_ ? source_ : nullptr;
    }
private:
    std::weak_ptr<MixedOwner> owner_;
    Source* source_ = nullptr;
};

struct ContentConfiguration
{
    bool ready = false, resourceUnavailable = false;
    std::string_view reason;
    std::vector<ole_content_source_fixture::Entry> entries;
};

// Configure bytes and per-request outcomes only. No provider is called and no
// image validity, virtual-file consumption or product decision is inferred.
inline ContentConfiguration ConfigureContent(const V4FamilyValidatedContext& verified,
    std::size_t byteBudget, std::size_t virtualBudget)
{
    ContentConfiguration out;
    try
    {
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        struct Format { std::string name, medium; CLIPFORMAT id; bool offered; };
        std::vector<Format> formats;
        const auto add = [&](const char* name, const char* medium, CLIPFORMAT fixed, const wchar_t* registered) {
            const auto offer = value(std::string("V4.format.") + name + ".offer");
            if (offer != "yes" && offer != "no") return false;
            CLIPFORMAT id = fixed;
            if (offer == "yes" && !id && registered) id = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(registered));
            if (offer == "yes" && !id) return false;
            formats.push_back({name, medium, id, offer == "yes"}); return true;
        };
        if (!add("text_unicode", "hglobal", CF_UNICODETEXT, nullptr) ||
            !add("dib", "hglobal", CF_DIB, nullptr) || !add("dibv5", "hglobal", CF_DIBV5, nullptr) ||
            !add("png", "hglobal", 0, L"PNG") || !add("image_png", "hglobal", 0, L"image/png") ||
            !add("file_descriptor_w", "hglobal", 0, L"FileGroupDescriptorW") ||
            !add("file_contents", "istream", 0, L"FileContents"))
        { out.reason = "mixed_content_format_unavailable"; return out; }
        const auto offered = [&](std::string_view name) {
            const auto it = std::find_if(formats.begin(), formats.end(), [&](const auto& f) { return f.name == name; });
            return it != formats.end() && it->offered;
        };
        const bool text = offered("text_unicode"), image = offered("dib") || offered("dibv5") || offered("png") || offered("image_png");
        const bool descriptor = offered("file_descriptor_w"), contents = offered("file_contents");
        if ((!text && !image && !descriptor && !contents) || value("T02.text") != (text ? "yes" : "no") ||
            value("T02.image") != (image ? "yes" : "no") || value("T02.virtual_files") != (descriptor ? "yes" : "no") ||
            value("T02.url") != "no" || (value("T02.invalid_format") != "no" && value("T02.invalid_format") != "yes"))
        { out.reason = "mixed_content_summary_unimplemented"; return out; }
        const auto virtualCountValue = value("V4.source.virtual_count");
        std::size_t virtualCount = 0;
        if (descriptor)
        {
            std::string decimal;
            if (virtualCountValue == "zero") decimal = "0";
            else if (virtualCountValue == "one") decimal = "1";
            else if (virtualCountValue == "two") decimal = "2";
            else if (virtualCountValue.starts_with("count_")) decimal = virtualCountValue.substr(6);
            else { out.reason = "mixed_virtual_count_unimplemented"; return out; }
            const auto count = V4MaterializeCount({V3CountKind::exact, decimal}, virtualBudget, false);
            if (count.status == V4AxisStatus::resource_unavailable)
            { out.resourceUnavailable = true; out.reason = "mixed_virtual_count_budget"; return out; }
            if (count.status != V4AxisStatus::schema_only || count.value > static_cast<std::size_t>((std::numeric_limits<LONG>::max)()))
            { out.reason = "mixed_virtual_count_unrepresentable"; return out; }
            virtualCount = count.value;
            if (contents && !virtualCount) { out.reason = "mixed_empty_virtual_contents_offer_unimplemented"; return out; }
        }
        else if (contents || (virtualCountValue != "na" && virtualCountValue != "zero"))
        { out.reason = "mixed_virtual_descriptor_unimplemented"; return out; }
        const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        const auto decode = [&](std::string_view textValue, std::vector<std::byte>& bytes, std::size_t& total) {
            if (!textValue.starts_with("hex_") || (textValue.size() - 4) % 2) return false;
            const auto length = (textValue.size() - 4) / 2;
            if (length > byteBudget - total) { out.resourceUnavailable = true; out.reason = "mixed_content_byte_budget"; return false; }
            bytes.reserve(length);
            for (std::size_t i = 4; i < textValue.size(); i += 2)
            { const int a = digit(textValue[i]), b = digit(textValue[i + 1]); if (a < 0 || b < 0) return false;
              bytes.push_back(static_cast<std::byte>((a << 4) | b)); }
            total += length; return true;
        };
        struct EntryKey { std::string format, medium, index; };
        std::vector<EntryKey> keys; std::size_t total = 0;
        const auto append = [&](const Format& format, std::string index, LONG nativeIndex) {
            const auto id = "V4.payload." + format.name + "." + format.medium + "." + index + ".bytes";
            std::vector<std::byte> bytes;
            if (!decode(value(id), bytes, total)) return false;
            // A raw provider can offer any bytes.  Until the wider format
            // semantics are observable, construct the explicit valid-Unicode
            // subset conservatively instead of treating malformed bytes as a
            // proven T02.invalid_format=no input.
            if (format.name == "text_unicode" && value("T02.invalid_format") == "no")
            {
                if (bytes.size() < 2 || bytes.size() % 2 ||
                    bytes[bytes.size() - 2] != std::byte{} || bytes.back() != std::byte{})
                { out.reason = "mixed_valid_unicode_unimplemented"; return false; }
                const auto unit = [&](std::size_t offset) {
                    return static_cast<unsigned>(std::to_integer<unsigned char>(bytes[offset])) |
                        (static_cast<unsigned>(std::to_integer<unsigned char>(bytes[offset + 1])) << 8);
                };
                for (std::size_t offset = 0; offset + 2 < bytes.size(); offset += 2)
                {
                    const auto c = unit(offset);
                    if (!c || (c >= 0xdc00 && c <= 0xdfff))
                    { out.reason = "mixed_valid_unicode_unimplemented"; return false; }
                    if (c >= 0xd800 && c <= 0xdbff)
                    {
                        if (offset + 4 >= bytes.size())
                        { out.reason = "mixed_valid_unicode_unimplemented"; return false; }
                        const auto low = unit(offset + 2);
                        if (low < 0xdc00 || low > 0xdfff)
                        { out.reason = "mixed_valid_unicode_unimplemented"; return false; }
                        offset += 2;
                    }
                }
            }
            keys.push_back({format.name, format.medium, std::move(index)});
            out.entries.push_back({format.id, nativeIndex, format.medium == "istream" ? TYMED_ISTREAM : TYMED_HGLOBAL,
                std::move(bytes), false, S_OK});
            return true;
        };
        for (const auto& format : formats)
        {
            if (!format.offered) continue;
            if (format.name == "file_contents")
            {
                for (std::size_t i = 0; i < virtualCount; ++i)
                    if (!append(format, "index_" + std::to_string(i), static_cast<LONG>(i)))
                    { if (out.reason.empty()) out.reason = "mixed_virtual_content_bytes_unavailable"; return out; }
            }
            else if (!append(format, "default", -1))
            { if (out.reason.empty()) out.reason = "mixed_content_payload_unavailable"; return out; }
        }
        std::set<std::string> expectedPayloads, expectedNames;
        for (const auto& key : keys) expectedPayloads.insert("V4.payload." + key.format + "." + key.medium + "." + key.index + ".bytes");
        if (descriptor)
        {
            const auto d = std::find_if(keys.begin(), keys.end(), [](const auto& key) { return key.format == "file_descriptor_w"; });
            if (d == keys.end()) { out.reason = "mixed_virtual_descriptor_missing"; return out; }
            const auto& bytes = out.entries[static_cast<std::size_t>(std::distance(keys.begin(), d))].bytes;
            if (virtualCount > ((std::numeric_limits<std::size_t>::max)() - offsetof(FILEGROUPDESCRIPTORW, fgd)) / sizeof(FILEDESCRIPTORW) ||
                bytes.size() != offsetof(FILEGROUPDESCRIPTORW, fgd) + virtualCount * sizeof(FILEDESCRIPTORW))
            { out.reason = "mixed_virtual_descriptor_shape_unimplemented"; return out; }
            UINT actualCount = 0; std::memcpy(&actualCount, bytes.data(), sizeof(actualCount));
            if (actualCount != virtualCount) { out.reason = "mixed_virtual_descriptor_count_mismatch"; return out; }
            for (std::size_t i = 0; i < virtualCount; ++i)
            {
                FILEDESCRIPTORW item{};
                std::memcpy(&item, bytes.data() + offsetof(FILEGROUPDESCRIPTORW, fgd) + i * sizeof(item), sizeof(item));
                const auto* last = std::find(std::begin(item.cFileName), std::end(item.cFileName), L'\0');
                if (last == std::end(item.cFileName)) { out.reason = "mixed_virtual_name_unterminated"; return out; }
                constexpr char hex[] = "0123456789abcdef"; std::string encoded = "utf16le_hex_";
                for (const auto* p = std::begin(item.cFileName); p != last; ++p)
                { const auto unit = static_cast<unsigned short>(*p); for (const unsigned byte : {unit & 255u, static_cast<unsigned>(unit) >> 8})
                  { encoded.push_back(hex[byte >> 4]); encoded.push_back(hex[byte & 15]); } }
                const auto id = "V4.virtual_name.index_" + std::to_string(i) + ".utf16le";
                expectedNames.insert(id);
                if (value(id) != encoded) { out.reason = "mixed_virtual_name_mismatch"; return out; }
            }
        }
        const auto idOffer = value("V4.format.shell_id_list.offer");
        const bool fileOffered = value("V4.source.kind") == "file_and_content";
        if (!fileOffered && value("V4.source.kind") != "content_only")
        { out.reason = "configured_content_source_kind_unimplemented"; return out; }
        for (std::size_t i = 0; i < verified.Axes().size(); ++i)
        {
            const auto& axis = verified.Axes()[i]; const auto& chosen = verified.Coordinate().values[i];
            const std::string_view id = axis.id;
            if (id.starts_with("task_b.")) continue;
            if (id.starts_with("V4.format.") && (id.ends_with(".offer") || id.find(".medium.") != std::string_view::npos))
            {
                bool expected = (fileOffered && (id == "V4.format.cf_hdrop.offer" || id == "V4.format.cf_hdrop.medium.hglobal")) ||
                    ((id == "V4.format.shell_id_list.offer" || id == "V4.format.shell_id_list.medium.hglobal") && idOffer == "yes");
                for (const auto& f : formats)
                    if (f.offered && (id == "V4.format." + f.name + ".offer" || id == "V4.format." + f.name + ".medium." + f.medium)) expected = true;
                if (chosen != (expected ? "yes" : "no")) { out.reason = "mixed_format_or_medium_unimplemented"; return out; }
            }
            else if (id.starts_with("V4.payload.") && id.ends_with(".bytes") && !expectedPayloads.contains(axis.id) && chosen != "na")
            { out.reason = "mixed_extra_payload_unimplemented"; return out; }
            else if (id.starts_with("V4.virtual_name.") && id.ends_with(".utf16le") && !expectedNames.contains(axis.id) && chosen != "na")
            { out.reason = "mixed_extra_virtual_name_unimplemented"; return out; }
            else if (id.starts_with("V4.request.") && id.ends_with(".hresult") && chosen != "na")
            {
                std::size_t match = keys.size(); std::string phase;
                for (std::size_t j = 0; j < keys.size(); ++j)
                    for (const auto* candidatePhase : {"query", "get", "read"})
                        if (id == "V4.request." + keys[j].format + "." + keys[j].medium + "." + keys[j].index + "." + candidatePhase + ".hresult")
                        { match = j; phase = candidatePhase; }
                if (match == keys.size() || !std::string_view(chosen).starts_with("hresult_") || chosen.size() != 16)
                { out.reason = "mixed_request_unimplemented"; return out; }
                std::uint32_t bits = 0;
                for (const char c : std::string_view(chosen).substr(8))
                { const int n = digit(c); if (n < 0) { out.reason = "mixed_hresult_invalid"; return out; } bits = (bits << 4) | static_cast<std::uint32_t>(n); }
                const HRESULT requested = static_cast<HRESULT>(bits);
                if (phase == "query" && requested != S_OK) { out.reason = "mixed_query_fault_unimplemented"; return out; }
                if (phase == "get")
                { if (requested != S_OK && SUCCEEDED(requested)) { out.reason = "mixed_get_success_code_unimplemented"; return out; }
                  out.entries[match].getDataFailure = requested; }
                if (phase == "read")
                { if (keys[match].medium != "istream" || (requested != S_OK && requested != STG_E_READFAULT))
                    { out.reason = "mixed_stream_read_result_unimplemented"; return out; }
                  out.entries[match].readFailure = requested == STG_E_READFAULT; }
            }
        }
        out.ready = true; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "mixed_content_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "mixed_content_resource_unavailable"; return out; }
    catch (...) { out.reason = "mixed_content_observation_unavailable"; return out; }
}

// This subset configures exact text/image HGLOBAL and virtual descriptor/stream
// bytes and supported per-request faults. Other representations remain unavailable.
// Actual Shell offers, data consumption and selected format require observation.
inline MixedBinding PrepareMixedSource(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& fileBinding, const std::shared_ptr<V4IndependentShellOwner>& fileOwner,
    const std::shared_ptr<MixedOwner>& owner, std::size_t byteBudget, std::size_t virtualBudget = 128)
{
    MixedBinding out;
    const auto pinnedOwner = owner; const auto pinnedFile = fileOwner; const auto pinnedPayload = payload;
    try
    {
        if (!pinnedOwner || !pinnedFile || !pinnedPayload || !fileBinding.SameOwner(pinnedFile) ||
            !pinnedOwner->Begin(pinnedFile, true))
        { out.unresolved.emplace_back("mixed_owner_or_file_lease_unavailable"); return out; }
        const auto end = [](MixedOwner* value) noexcept { value->End(); };
        const std::unique_ptr<MixedOwner, decltype(end)> guard(pinnedOwner.get(), end);
        auto* fileSource = fileBinding.SourceObject();
        if (!fileSource || !pinnedOwner->Owns(pinnedFile, pinnedPayload, fileSource) ||
            !fileBinding.inputPrepared || fileBinding.resourceUnavailable ||
            verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || verified.FamilyBinding() != identity.FamilyBinding() ||
            verified.Axes().size() != identity.Axes().size())
        { out.unresolved.emplace_back("mixed_candidate_or_file_source_unavailable"); return out; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { out.unresolved.emplace_back("mixed_schema_mismatch"); return out; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) out.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != fileBinding.candidateId)
        { out.unresolved.emplace_back("mixed_candidate_identity_unavailable"); return out; }
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (value("V4.source.kind") != "file_and_content" || value("T01") != "external_ingress" ||
            value("T02.file_paths") != "yes" || value("T04.capability") != "enabled" ||
            value("V4.format.cf_hdrop.offer") != "yes")
        { out.unresolved.emplace_back("mixed_source_format_or_transport_unimplemented"); return out; }
        const auto initial = ole_driver::InspectV4IndependentFileInputs(verified, identity, *pinnedPayload, *fileSource, true);
        if (!initial.captured || initial.resourceUnavailable || initial.candidateId != raw.id)
        { out.resourceUnavailable = initial.resourceUnavailable; out.unresolved.emplace_back("mixed_file_snapshot_unavailable"); return out; }
        const auto idOffer = value("V4.format.shell_id_list.offer");
        if ((idOffer != "yes" && idOffer != "no") || value("T02.shell_object") != idOffer)
        { out.unresolved.emplace_back("mixed_shell_id_offer_unimplemented"); return out; }
        auto configured = ConfigureContent(verified, byteBudget, virtualBudget);
        if (!configured.ready)
        { out.resourceUnavailable = configured.resourceUnavailable; out.unresolved.emplace_back(configured.reason); return out; }
        auto content = Microsoft::WRL::Make<ole_content_source_fixture::Source>();
        if (!content) { out.resourceUnavailable = true; return out; }
        content->entries = std::move(configured.entries);
        auto combined = Microsoft::WRL::Make<Source>();
        if (!combined) { out.resourceUnavailable = true; return out; }
        combined->shellIdFormat_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
        if (!combined->shellIdFormat_) { out.unresolved.emplace_back("mixed_shell_id_format_unavailable"); return out; }
        combined->offerShellId_ = idOffer == "yes";
        combined->files_ = fileSource; combined->content_ = content;
        auto retained = std::make_unique<std::shared_ptr<V4IndependentShellOwner>>(pinnedFile);
        auto* const sourceIdentity = combined.Get();
        pinnedOwner->source_ = std::move(combined); pinnedOwner->retainedFileOwner_ = std::move(retained);
        out.owner_ = pinnedOwner; out.source_ = sourceIdentity; out.candidateId = raw.id; out.inputPrepared = true;
        out.unresolved = {"mixed_other_axes_and_source_formats_unimplemented", "mixed_content_semantic_validity_and_virtual_identity_unverified",
            "mixed_shell_advertisement_and_actual_requests_unobserved",
            "mixed_wrapper_to_provider_call_identity_unverified",
            "mixed_production_format_selection_completion_and_business_unverified"};
        return out;
    }
    catch (const std::bad_alloc&) { out.inputPrepared = false; out.resourceUnavailable = true; return out; }
    catch (const std::length_error&) { out.inputPrepared = false; out.resourceUnavailable = true; return out; }
    catch (...) { out.inputPrepared = false; return out; }
}

class ConfiguredContentBinding;
class ConfiguredContentOwner final
{
    friend struct ::NativeDragIntegrationAccess;
    friend class ConfiguredContentBinding;
    friend class ::drag_global::ole_driver::V4ConfiguredContentGraphExecutor;
    friend ConfiguredContentBinding PrepareConfiguredContentSource(const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, const std::shared_ptr<ConfiguredContentOwner>&,
        std::size_t, std::size_t);
public:
    ConfiguredContentOwner(const ConfiguredContentOwner&) = delete;
    ConfiguredContentOwner& operator=(const ConfiguredContentOwner&) = delete;
    ~ConfiguredContentOwner() { (void)source_.Detach(); }
private:
    ConfiguredContentOwner() noexcept : ownerThread_(GetCurrentThreadId()) {}
    bool Live() const noexcept
    {
        if (!active_.load(std::memory_order_acquire) || GetCurrentThreadId() != ownerThread_) return false;
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        return SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) &&
            (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA);
    }
    bool Begin(bool preparation) const noexcept
    {
        if (!Live() || (preparation ? static_cast<bool>(source_) : !source_)) return false;
        bool expected = false; return executing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
    void End() const noexcept { executing_.store(false, std::memory_order_release); }
    bool CanInvalidate() const noexcept
    { return GetCurrentThreadId() == ownerThread_ && !executing_.load(std::memory_order_acquire); }
    void Invalidate() noexcept
    {
        const DWORD saved = GetLastError();
        if (CanInvalidate()) { active_.store(false, std::memory_order_release); source_.Reset(); }
        SetLastError(saved);
    }
    DWORD ownerThread_ = 0;
    std::atomic_bool active_{true};
    mutable std::atomic_bool executing_{false};
    Microsoft::WRL::ComPtr<ole_content_source_fixture::Source> source_;
};

class ConfiguredContentBinding final
{
    friend class ::drag_global::ole_driver::V4ConfiguredContentGraphExecutor;
    friend ConfiguredContentBinding PrepareConfiguredContentSource(const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, const std::shared_ptr<ConfiguredContentOwner>&,
        std::size_t, std::size_t);
public:
    bool inputPrepared = false, resourceUnavailable = false;
    std::string candidateId;
    std::vector<std::string> unresolved;
    ole_content_source_fixture::Source* SourceObject() const noexcept
    {
        const auto owner = owner_.lock();
        return owner && owner->Live() && owner->source_.Get() == source_ ? source_ : nullptr;
    }
    const std::vector<ole_content_source_fixture::Entry>& ConfiguredEntries() const noexcept { return entries_; }
private:
    std::weak_ptr<ConfiguredContentOwner> owner_;
    ole_content_source_fixture::Source* source_ = nullptr;
    std::vector<ole_content_source_fixture::Entry> entries_;
};

// Content-only provider, sharing exactly the same model-to-entry configuration
// as the mixed source. No GetData, materialization or host decision occurs here.
inline ConfiguredContentBinding PrepareConfiguredContentSource(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::shared_ptr<ConfiguredContentOwner>& owner,
    std::size_t byteBudget, std::size_t virtualBudget)
{
    ConfiguredContentBinding out; const auto pinnedOwner = owner;
    try
    {
        if (!pinnedOwner || !pinnedOwner->Begin(true))
        { out.unresolved.emplace_back("content_owner_preparation_unavailable"); return out; }
        const auto end = [](const ConfiguredContentOwner* value) noexcept { value->End(); };
        const std::unique_ptr<const ConfiguredContentOwner, decltype(end)> guard(pinnedOwner.get(), end);
        if (verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || verified.FamilyBinding() != identity.FamilyBinding() ||
            verified.Axes().size() != identity.Axes().size())
        { out.unresolved.emplace_back("content_authoritative_context_unavailable"); return out; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { out.unresolved.emplace_back("content_schema_mismatch"); return out; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) out.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
        { out.unresolved.emplace_back("content_candidate_unavailable"); return out; }
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (value("V4.source.kind") != "content_only" || value("T01") != "external_ingress" ||
            (value("V4.source.file_count") != "na" && value("V4.source.file_count") != "zero") ||
            value("T02.file_paths") != "no" || value("T02.shell_object") != "no" ||
            value("V4.format.shell_id_list.offer") != "no" || value("T04.capability") != "absent" ||
            value("T04.availability") != "immediate")
        { out.unresolved.emplace_back("content_transport_or_file_source_unimplemented"); return out; }
        auto configured = ConfigureContent(verified, byteBudget, virtualBudget);
        if (!configured.ready)
        { out.resourceUnavailable = configured.resourceUnavailable; out.unresolved.emplace_back(configured.reason); return out; }
        auto source = Microsoft::WRL::Make<ole_content_source_fixture::Source>();
        if (!source) { out.resourceUnavailable = true; return out; }
        // Keep a separate exact candidate-derived input baseline for later
        // comparison; the live provider's public entries are not trusted.
        out.entries_ = configured.entries;
        source->entries = std::move(configured.entries);
        if (source->ownerThread != GetCurrentThreadId() || source->wrongApartment.load() ||
            source->observationFailed.load() || !source->calls.empty())
        { out.unresolved.emplace_back("content_provider_initial_state_unavailable"); return out; }
        auto* const sourceIdentity = source.Get();
        pinnedOwner->source_ = std::move(source);
        out.owner_ = pinnedOwner; out.source_ = sourceIdentity; out.candidateId = raw.id; out.inputPrepared = true;
        out.unresolved = {"content_unlisted_formats_requests_and_other_axes_unimplemented",
            "content_semantic_validity_and_virtual_identity_unverified",
            "content_host_selection_materialization_completion_and_business_unverified"};
        return out;
    }
    catch (const std::bad_alloc&) { out.inputPrepared = false; out.resourceUnavailable = true; return out; }
    catch (const std::length_error&) { out.inputPrepared = false; out.resourceUnavailable = true; return out; }
    catch (...) { out.inputPrepared = false; return out; }
}

struct VirtualExpectedMember
{
    std::size_t descriptorIndex = 0;
    std::wstring name;
    std::vector<std::byte> bytes;
};
struct VirtualExpectedManifest
{
    bool ready = false, resourceUnavailable = false;
    std::string candidateId;
    std::vector<VirtualExpectedMember> members;
    std::vector<std::string> unresolved;
};

// Candidate-derived expectations only. Restrict names and successful stream
// inputs so no test-side filename sanitization, collision resolution or
// materialization policy substitutes for the production implementation.
// A caller must independently bind the selected production reads, completed
// operation and isolated output before using these as business assertions.
inline VirtualExpectedManifest BuildVirtualExpectedManifest(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, std::size_t byteBudget, std::size_t virtualBudget)
{
    VirtualExpectedManifest out;
    try
    {
        if (verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || verified.FamilyBinding() != identity.FamilyBinding() ||
            verified.Axes().size() != identity.Axes().size())
        { out.unresolved.emplace_back("virtual_manifest_context_unavailable"); return out; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { out.unresolved.emplace_back("virtual_manifest_schema_mismatch"); return out; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) out.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
        { out.unresolved.emplace_back("virtual_manifest_candidate_unavailable"); return out; }
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (value("V4.source.kind") != "content_only" || value("T02.file_paths") != "no" ||
            value("T04.capability") != "absent" || value("T04.availability") != "immediate" ||
            value("T02.virtual_files") != "yes" || value("T02.virtual_completeness") != "complete" ||
            value("T02.text") != "no" || value("T02.image") != "no" || value("T02.url") != "no" ||
            value("T02.invalid_format") != "no" || value("V4.format.file_descriptor_w.offer") != "yes" ||
            value("V4.format.file_contents.offer") != "yes")
        { out.unresolved.emplace_back("virtual_manifest_source_or_selection_unimplemented"); return out; }
        auto configured = ConfigureContent(verified, byteBudget, virtualBudget);
        if (!configured.ready)
        { out.resourceUnavailable = configured.resourceUnavailable; out.unresolved.emplace_back(configured.reason); return out; }
        const auto descriptorId = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
        const auto contentsId = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
        if (!descriptorId || !contentsId) { out.unresolved.emplace_back("virtual_manifest_format_unavailable"); return out; }
        const auto descriptor = std::find_if(configured.entries.begin(), configured.entries.end(), [&](const auto& entry) {
            return entry.format == descriptorId && entry.tymed == TYMED_HGLOBAL && entry.index == -1; });
        if (descriptor == configured.entries.end() || descriptor->getDataFailure != S_OK ||
            descriptor->bytes.size() < offsetof(FILEGROUPDESCRIPTORW, fgd))
        { out.unresolved.emplace_back("virtual_manifest_descriptor_or_fault_unimplemented"); return out; }
        UINT count = 0; std::memcpy(&count, descriptor->bytes.data(), sizeof(count));
        if (!count || static_cast<std::size_t>(count) > virtualBudget ||
            static_cast<std::size_t>(count) > ((std::numeric_limits<std::size_t>::max)() - offsetof(FILEGROUPDESCRIPTORW, fgd)) / sizeof(FILEDESCRIPTORW) ||
            descriptor->bytes.size() != offsetof(FILEGROUPDESCRIPTORW, fgd) + static_cast<std::size_t>(count) * sizeof(FILEDESCRIPTORW))
        { out.unresolved.emplace_back("virtual_manifest_descriptor_shape_unavailable"); return out; }
        // This expectation covers the synchronous production materializer's
        // currently bounded successful subset, not the model's entire domain.
        constexpr std::size_t maxSynchronousFiles = 256;
        constexpr std::size_t maxSynchronousBytes = 64u * 1024u * 1024u;
        if (count > maxSynchronousFiles)
        { out.unresolved.emplace_back("virtual_manifest_synchronous_count_unimplemented"); return out; }
        std::size_t remainingBytes = maxSynchronousBytes;
        out.members.reserve(count);
        for (UINT i = 0; i < count; ++i)
        {
            FILEDESCRIPTORW item{};
            std::memcpy(&item, descriptor->bytes.data() + offsetof(FILEGROUPDESCRIPTORW, fgd) +
                static_cast<std::size_t>(i) * sizeof(item), sizeof(item));
            const auto* first = std::cbegin(item.cFileName);
            const auto* end = std::find(first, std::cend(item.cFileName), L'\0');
            if (end == first || end == std::cend(item.cFileName) ||
                ((item.dwFlags & FD_ATTRIBUTES) &&
                    (item.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))))
            { out.unresolved.emplace_back("virtual_manifest_name_or_directory_unimplemented"); return out; }
            std::wstring name(first, end), upper;
            // Production may truncate longer suggested names. This manifest
            // only predicts exact names when no such transformation is needed.
            if (name.size() > 180)
            { out.unresolved.emplace_back("virtual_manifest_name_length_unimplemented"); return out; }
            for (const auto ch : name)
            {
                if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
                    (ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'_' || ch == L'-'))
                { out.unresolved.emplace_back("virtual_manifest_name_spelling_unimplemented"); return out; }
                upper.push_back(ch >= L'a' && ch <= L'z' ? static_cast<wchar_t>(ch - L'a' + L'A') : ch);
            }
            const auto dot = upper.find(L'.'); const auto base = upper.substr(0, dot);
            const bool device = base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
                (base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT")) && base[3] >= L'1' && base[3] <= L'9');
            if (name == L"." || name == L".." || name.back() == L'.' || device)
            { out.unresolved.emplace_back("virtual_manifest_name_unimplemented"); return out; }
            for (const auto& previous : out.members)
            {
                const int comparison = CompareStringOrdinal(previous.name.c_str(), -1, name.c_str(), -1, TRUE);
                if (!comparison || comparison == CSTR_EQUAL)
                { out.unresolved.emplace_back("virtual_manifest_name_collision_unimplemented"); return out; }
            }
            const auto stream = std::find_if(configured.entries.begin(), configured.entries.end(), [&](const auto& entry) {
                return entry.format == contentsId && entry.tymed == TYMED_ISTREAM && entry.index == static_cast<LONG>(i); });
            if (stream == configured.entries.end() || stream->getDataFailure != S_OK || stream->readFailure)
            { out.unresolved.emplace_back("virtual_manifest_stream_or_fault_unimplemented"); return out; }
            if (stream->bytes.size() > remainingBytes ||
                (stream->bytes.size() == remainingBytes && i + 1 < count))
            { out.unresolved.emplace_back("virtual_manifest_synchronous_bytes_unimplemented"); return out; }
            remainingBytes -= stream->bytes.size();
            if (item.dwFlags & FD_FILESIZE)
            {
                const auto declared = (static_cast<std::uint64_t>(item.nFileSizeHigh) << 32) | item.nFileSizeLow;
                if (declared != stream->bytes.size())
                { out.unresolved.emplace_back("virtual_manifest_declared_size_difference_unimplemented"); return out; }
            }
            out.members.push_back({static_cast<std::size_t>(i), std::move(name), stream->bytes});
        }
        out.candidateId = raw.id; out.ready = true;
        out.unresolved = {"virtual_manifest_target_collision_and_actual_selected_reads_unverified",
            "virtual_manifest_production_materialization_completion_and_layout_unverified"};
        return out;
    }
    catch (const std::bad_alloc&) { out.ready = false; out.resourceUnavailable = true; return out; }
    catch (const std::length_error&) { out.ready = false; out.resourceUnavailable = true; return out; }
    catch (...) { out.ready = false; return out; }
}
} // namespace drag_global::mixed_fixture
