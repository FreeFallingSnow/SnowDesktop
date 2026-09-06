#pragma once

#include <windows.h>

#include <algorithm>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
// UI-thread event transactions. Keep the existing synchronous host callback,
// but invoke it only after the outer event has restored its Lua/render context.
// No timer or message is introduced: pointer feedback still finishes in the
// current input message. Surface names are resolved by the engine at request
// time; an empty surface retains its existing desktop + auxiliary meaning.
class WidgetInvalidationBatch
{
public:
    using Callback = std::function<void(const std::wstring&,
        const std::optional<RECT>&, std::string_view)>;

    class Scope
    {
    public:
        Scope(WidgetInvalidationBatch& batch, const Callback& callback)
            : batch_(batch), callback_(callback),
              exceptions_(std::uncaught_exceptions())
        {
            ++batch_.depth_;
        }
        ~Scope() noexcept(false)
        {
            if (--batch_.depth_ != 0) return;
            auto pending = std::exchange(batch_.pending_, {});
            // Native exception unwinding must not call back into rendering.
            // Lua errors are protected-call results and take the normal path.
            if (std::uncaught_exceptions() != exceptions_) return;
            for (const auto& request : pending)
                if (callback_)
                    callback_(request.widgetId, request.dirty, request.surface);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        WidgetInvalidationBatch& batch_;
        const Callback& callback_;
        int exceptions_;
    };

    void Invalidate(const Callback& callback, const std::wstring& widgetId,
        const std::optional<RECT>& dirty, std::string_view surface)
    {
        if (depth_ == 0)
        {
            if (callback) callback(widgetId, dirty, surface);
            return;
        }
        for (auto& request : pending_)
        {
            if (request.widgetId != widgetId || request.surface != surface)
                continue;
            if (!request.dirty || !dirty)
                request.dirty.reset();
            else
            {
                auto& rect = *request.dirty;
                rect.left = (std::min)(rect.left, dirty->left);
                rect.top = (std::min)(rect.top, dirty->top);
                rect.right = (std::max)(rect.right, dirty->right);
                rect.bottom = (std::max)(rect.bottom, dirty->bottom);
            }
            return;
        }
        pending_.push_back({ widgetId, dirty, std::string(surface) });
    }

private:
    struct Request
    {
        std::wstring widgetId;
        std::optional<RECT> dirty;
        std::string surface;
    };
    unsigned depth_ = 0;
    std::vector<Request> pending_;
};
}
