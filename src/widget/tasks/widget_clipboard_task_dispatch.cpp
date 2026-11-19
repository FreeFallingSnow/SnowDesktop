#include "widget_clipboard_task_dispatch.h"

#include "common/utils.h"
#include "widget/data/widget_resource_lua.h"
#include "widget_app_task_executor.h"
#include "widget_task_broker.h"

#include <string_view>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
void MarkResultUnavailable(TaskCompletion& completion)
{
    completion.ok = false;
    completion.error = "taskResultUnavailable";
}

void SetString(lua_State* state, const char* key, std::string_view value)
{
    lua_pushlstring(state, value.data(), value.size());
    lua_setfield(state, -2, key);
}
} // namespace

const char* PrepareClipboardTaskRequest(const TaskBrokerAction& action,
    const ClipboardWriteSources& sources, WidgetClipboardTaskRequest& request)
{
    request.action = action.name;
    if (const auto format = action.arguments.find("format"); format != action.arguments.end())
        request.format = format->second;

    if (action.name == "clipboard.write")
    {
        if (request.format == "text" || request.format == "html")
        {
            const auto value = action.arguments.find(request.format);
            if (value == action.arguments.end()) return "invalidArguments";
            request.text = value->second;
        }
        else if (request.format == "image")
        {
            if (const auto image = action.arguments.find("image"); image != action.arguments.end())
                sources.resolveImage(image->second, request);
            if (!request.image && request.imagePath.empty()) return "clipboardImageUnavailable";
        }
        else if (request.format == "file-reference")
        {
            for (std::size_t index = 1; index <= WidgetClipboardTaskExecutor::MaximumFileReferences; ++index)
            {
                const auto item = action.arguments.find("item" + std::to_string(index));
                if (item == action.arguments.end()) break;
                std::filesystem::path path;
                if (const char* error = sources.resolveFile(item->second, path)) return error;
                request.files.push_back(std::move(path));
            }
        }
    }
    return WidgetClipboardTaskExecutor::ValidateRequest(request) ? nullptr : "invalidArguments";
}

bool ClipboardTaskPayload::HasValue() const noexcept
{
    return result_.has_value();
}

void ClipboardTaskPayload::PrepareValue(TaskCompletion& completion,
    const ImageRegistrar& registerImage, const FilePublisher& publishFiles)
{
    if (!completion.ok || !result_) return;
    if (result_->format == "image")
    {
        imageToken_ = registerImage(result_->resourceToken, result_->image);
        if (imageToken_.empty()) MarkResultUnavailable(completion);
        return;
    }
    if (result_->format != "file-reference") return;

    const auto count = result_->files.size();
    files_.reserve(count);
    const std::string scope = "clipboard.read:" + std::to_string(completion.ownerToken);
    for (const auto& file : result_->files)
    {
        auto target = WideToUtf8(file.path.wstring());
        auto displayName = file.path.filename().wstring();
        if (displayName.empty()) displayName = file.path.root_name().wstring();
        auto name = WideToUtf8(displayName);
        auto reference = MakeWidgetItemReference(scope, target);
        if (target.empty() || name.empty() || reference.empty())
        {
            MarkResultUnavailable(completion);
            files_.clear();
            break;
        }
        files_.push_back({std::move(reference), std::move(target), std::move(name), file.folder});
    }
    publishFiles(count, files_);
}

void ClipboardTaskPayload::PushValue(lua_State* state) const
{
    const auto& result = *result_;
    lua_createtable(state, 0, 5);
    SetString(state, "format", result.format);
    if (result.format == "text" || result.format == "html")
        SetString(state, result.format.c_str(), result.text);
    else if (result.format == "image")
    {
        PushResourceHandle(state, LuaResourceType::Image, imageToken_);
        lua_setfield(state, -2, "image");
        lua_pushinteger(state, result.image->width);
        lua_setfield(state, -2, "width");
        lua_pushinteger(state, result.image->height);
        lua_setfield(state, -2, "height");
    }
    else
    {
        lua_createtable(state, static_cast<int>(files_.size()), 0);
        int index = 1;
        for (const auto& file : files_)
        {
            lua_createtable(state, 0, 3);
            SetString(state, "ref", file.reference);
            SetString(state, "name", file.name);
            SetString(state, "type", file.folder ? "folder" : "file");
            lua_rawseti(state, -2, index++);
        }
        lua_setfield(state, -2, "items");
    }
}

void WidgetClipboardTaskDispatch::Consume(std::vector<WidgetClipboardTaskCompletion> messages,
    WidgetTaskBroker& broker)
{
    for (auto& message : messages)
    {
        const auto id = message.id;
        const auto snapshot = broker.Snapshot(id);
        if (!snapshot) continue;
        const bool ok = message.ok;
        std::string error = message.error;
        if (ok && message.action == "clipboard.read" && !snapshot->cancelRequested)
            results_.insert_or_assign(id, std::move(message));
        else
            results_.erase(id);
        (void)broker.Complete(id, ok, std::move(error));
    }
}

void WidgetClipboardTaskDispatch::StorePreviewRead(std::uint64_t id, const std::string& format)
{
    WidgetClipboardTaskCompletion result;
    result.id = id;
    result.action = "clipboard.read";
    result.ok = true;
    result.format = format;
    if (format == "text")
        result.text = "Preview clipboard";
    else if (format == "html")
        result.text = "<strong>Preview clipboard</strong>";
    else if (format == "image")
    {
        auto pixels = std::make_shared<WidgetRuntimeImagePixels>();
        pixels->width = 64;
        pixels->height = 64;
        pixels->stride = pixels->width * 4;
        pixels->bgraPremultiplied.resize(pixels->stride * pixels->height);
        for (std::uint32_t y = 0; y < pixels->height; ++y)
        {
            for (std::uint32_t x = 0; x < pixels->width; ++x)
            {
                const std::size_t offset = static_cast<std::size_t>(y * pixels->stride + x * 4);
                pixels->bgraPremultiplied[offset] = static_cast<std::uint8_t>(48 + x * 3);
                pixels->bgraPremultiplied[offset + 1] = static_cast<std::uint8_t>(80 + y * 2);
                pixels->bgraPremultiplied[offset + 2] = 210;
                pixels->bgraPremultiplied[offset + 3] = 255;
            }
        }
        result.resourceToken = MakeWidgetRuntimeImageToken("clipboard", *pixels);
        result.image = std::move(pixels);
    }
    else
    {
        result.files.push_back({std::filesystem::path(L"C:\\Users\\Maya\\Documents\\Preview.txt"), false});
        result.files.push_back({std::filesystem::path(L"C:\\Users\\Maya\\Pictures"), true});
    }
    results_.insert_or_assign(id, std::move(result));
}

ClipboardTaskPayload WidgetClipboardTaskDispatch::TakeCompletion(TaskCompletion& completion)
{
    auto result = results_.extract(completion.id);
    ClipboardTaskPayload payload;
    if (!completion.ok || completion.name != "clipboard.read") return payload;
    if (result)
        payload.result_.emplace(std::move(result.mapped()));
    else
        MarkResultUnavailable(completion);
    return payload;
}

void WidgetClipboardTaskDispatch::DiscardCompletion(std::uint64_t id)
{
    results_.erase(id);
}

void WidgetClipboardTaskDispatch::Clear()
{
    results_.clear();
}
}
