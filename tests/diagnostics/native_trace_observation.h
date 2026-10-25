#pragma once

#include "diagnostics/perfetto_categories.h"
#include "diagnostics/performance_trace_control.h"
#include <windows.h>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace snowdesktop::test
{
// Test-only synchronous observation at the SDK's serialized packet boundary.
// No product callback, timing stack, aggregation or alternate capture backend.
struct NativeTraceEvent
{
    enum Kind { Begin, Counter, Instant } kind = Instant;
    std::string module, name;
    std::wstring owner;
    std::uint64_t operation = 0;
    double value = 0;
};
class NativeTraceInterceptor : public perfetto::Interceptor<NativeTraceInterceptor>
{
public:
    struct ThreadLocalState : perfetto::InterceptorBase::ThreadLocalState
    {
        explicit ThreadLocalState(ThreadLocalStateArgs&) {}
        std::map<std::uint64_t, std::string> names, annotations;
    };
    inline static std::mutex mutex;
    inline static std::function<void(const NativeTraceEvent&)> observe;
    inline static std::map<std::uint64_t, std::string> tracks;
    static std::string Text(protozero::ConstChars value) { return {value.data, value.size}; }
    static void OnTracePacket(InterceptorContext context)
    {
        namespace pb = perfetto::protos::pbzero;
        pb::TracePacket::Decoder packet(context.packet_data.data, context.packet_data.size);
        auto& tls = context.GetThreadLocalState();
        if (packet.sequence_flags() & pb::TracePacket::SEQ_INCREMENTAL_STATE_CLEARED)
        { tls.names.clear(); tls.annotations.clear(); }
        if (packet.has_interned_data())
        {
            pb::InternedData::Decoder data(packet.interned_data());
            for (auto it = data.event_names(); it; ++it)
            { pb::EventName::Decoder name(*it); tls.names[name.iid()] = Text(name.name()); }
            for (auto it = data.debug_annotation_names(); it; ++it)
            { pb::DebugAnnotationName::Decoder name(*it); tls.annotations[name.iid()] = Text(name.name()); }
        }
        std::lock_guard lock(mutex);
        if (packet.has_track_descriptor())
        { pb::TrackDescriptor::Decoder track(packet.track_descriptor()); tracks[track.uuid()] = Text(track.has_name() ? track.name() : track.static_name()); }
        if (!packet.has_track_event() || !observe) return;
        pb::TrackEvent::Decoder event(packet.track_event());
        NativeTraceEvent result;
        result.name = event.has_name() ? Text(event.name()) : tls.names[event.name_iid()];
        std::uint64_t owner = 0;
        for (auto it = event.debug_annotations(); it; ++it)
        {
            pb::DebugAnnotation::Decoder arg(*it);
            const auto key = arg.has_name() ? Text(arg.name()) : tls.annotations[arg.name_iid()];
            if (key == "module") result.module = Text(arg.string_value());
            if (key == "owner") owner = arg.uint_value();
            if (key == "operation") result.operation = arg.uint_value();
        }
        if (event.type() == pb::TrackEvent::TYPE_SLICE_BEGIN) result.kind = NativeTraceEvent::Begin;
        else if (event.type() == pb::TrackEvent::TYPE_COUNTER)
        {
            result.kind = NativeTraceEvent::Counter;
            const auto label = tracks[event.track_uuid()];
            const auto first = label.find('|'), last = label.rfind('|');
            if (first == label.npos || first == last) return;
            result.module = label.substr(0, first);
            result.name = label.substr(first + 1, last - first - 1);
            owner = std::stoull(label.substr(last + 1), nullptr, 16);
            result.value = event.has_double_counter_value() ? event.double_counter_value() : static_cast<double>(event.counter_value());
        }
        else if (event.type() != pb::TrackEvent::TYPE_INSTANT) return;
        for (const auto& item : performance::DescribeTraceOwners())
            if (item.opaqueId == owner) { result.owner = item.instanceId; break; }
        observe(result);
    }
};
class NativeTraceObservation
{
    std::unique_ptr<perfetto::TracingSession> session_;
public:
    explicit NativeTraceObservation(std::function<void(const NativeTraceEvent&)> callback)
    {
        static std::once_flag once;
        std::call_once(once, [] {
            perfetto::TracingInitArgs args; args.backends = perfetto::kInProcessBackend;
            perfetto::Tracing::Initialize(args);
            performance::native_trace::TrackEvent::Register();
            // v58.2 deliberately admits only its upstream test interceptor
            // name (plus console/etwexport), even though Register accepts a string.
            perfetto::InterceptorDescriptor descriptor; descriptor.set_name("test_interceptor");
            NativeTraceInterceptor::Register(descriptor);
        });
        { std::lock_guard lock(NativeTraceInterceptor::mutex);
          if (NativeTraceInterceptor::observe) throw std::runtime_error("nested native trace observations");
          NativeTraceInterceptor::tracks.clear(); NativeTraceInterceptor::observe = std::move(callback); }
        perfetto::TraceConfig config;
        config.add_buffers()->set_size_kb(1024);
        auto* source = config.add_data_sources()->mutable_config();
        source->set_name("track_event");
        source->mutable_interceptor_config()->set_name("test_interceptor");
        session_ = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
        session_->Setup(config); session_->StartBlocking();
        static std::uint64_t generation = 0;
        performance::ActivateTrace(++generation, 738129);
    }
    ~NativeTraceObservation()
    {
        performance::DeactivateTrace();
        session_->StopBlocking();
        std::lock_guard lock(NativeTraceInterceptor::mutex);
        NativeTraceInterceptor::observe = {};
    }
};
}
