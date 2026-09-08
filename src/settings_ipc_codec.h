#pragma once

// Private, same-executable protocol. Only explicitly described value types
// cross the process boundary; pointers, handles and object padding never do.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace snowdesktop::settings_ipc
{
inline constexpr std::size_t MaximumFrameBytes = 16 * 1024 * 1024;
inline constexpr std::size_t MaximumElements = 65536;
using Bytes = std::vector<std::byte>;

class ProtocolError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

template<class T> struct Fields;
template<class T> struct IsVector : std::false_type {};
template<class T, class A> struct IsVector<std::vector<T, A>> : std::true_type {};
template<class T> struct IsOptional : std::false_type {};
template<class T> struct IsOptional<std::optional<T>> : std::true_type {};
template<class T> struct IsShared : std::false_type {};
template<class T> struct IsShared<std::shared_ptr<T>> : std::true_type {};
template<class T> struct IsString : std::false_type {};
template<class C, class T, class A>
struct IsString<std::basic_string<C, T, A>> : std::true_type {};
template<class T> struct IsMap : std::false_type {};
template<class K, class V, class C, class A>
struct IsMap<std::map<K, V, C, A>> : std::true_type {};
template<class K, class V, class H, class E, class A>
struct IsMap<std::unordered_map<K, V, H, E, A>> : std::true_type {};

class Writer
{
public:
    template<class... T> void operator()(const T&... value)
    {
        (Write(value), ...);
    }
    Bytes Take() { return std::move(bytes_); }

private:
    void Append(const void* data, std::size_t size)
    {
        if (size > MaximumFrameBytes - bytes_.size())
            throw ProtocolError("settings IPC frame exceeds limit");
        const auto first = static_cast<const std::byte*>(data);
        bytes_.insert(bytes_.end(), first, first + size);
    }
    template<class T> void Write(const T& value)
    {
        if (++depth_ > 64) throw ProtocolError("settings IPC nesting limit");
        if constexpr (std::is_same_v<T, bool>)
        {
            Write(static_cast<std::uint8_t>(value ? 1 : 0));
        }
        else if constexpr (std::is_arithmetic_v<T>)
        {
            if constexpr (std::is_floating_point_v<T>)
                if (!std::isfinite(value)) throw ProtocolError("nonfinite value");
            Append(&value, sizeof(value));
        }
        else if constexpr (std::is_enum_v<T>)
            Write(static_cast<std::underlying_type_t<T>>(value));
        else if constexpr (IsString<T>::value)
        {
            if (value.size() > MaximumFrameBytes / sizeof(typename T::value_type))
                throw ProtocolError("settings IPC string exceeds limit");
            Write(static_cast<std::uint32_t>(value.size()));
            Append(value.data(), value.size() * sizeof(typename T::value_type));
        }
        else if constexpr (std::is_same_v<T, std::filesystem::path>)
            Write(value.native());
        else if constexpr (IsOptional<T>::value || IsShared<T>::value)
        {
            Write(static_cast<bool>(value));
            if (value) Write(*value);
        }
        else if constexpr (IsVector<T>::value || IsMap<T>::value)
        {
            if (value.size() > MaximumElements)
                throw ProtocolError("settings IPC collection exceeds limit");
            Write(static_cast<std::uint32_t>(value.size()));
            for (const auto& element : value) Write(element);
        }
        else if constexpr (std::is_array_v<T>)
            for (const auto& element : value) Write(element);
        else if constexpr (requires { std::tuple_size<T>::value; })
            std::apply([this](const auto&... element) { (*this)(element...); }, value);
        else
            Write(Fields<T>::Tie(value));
        --depth_;
    }
    Bytes bytes_;
    unsigned depth_ = 0;
};

class Reader
{
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes)
    {
        if (bytes.size() > MaximumFrameBytes) throw ProtocolError("oversized frame");
    }
    template<class... T> void operator()(T&... value) { (Read(value), ...); }
    void Finish() const
    {
        if (position_ != bytes_.size()) throw ProtocolError("trailing IPC data");
    }

private:
    void Extract(void* target, std::size_t size)
    {
        if (size > bytes_.size() - position_) throw ProtocolError("truncated IPC data");
        if (size) std::memcpy(target, bytes_.data() + position_, size);
        position_ += size;
    }
    void Allocate(std::size_t size)
    {
        if (size > MaximumFrameBytes * 4 - allocation_)
            throw ProtocolError("settings IPC allocation limit");
        allocation_ += size;
    }
    template<class T> void Read(T& value)
    {
        if (++depth_ > 64) throw ProtocolError("settings IPC nesting limit");
        if constexpr (std::is_same_v<T, bool>)
        {
            std::uint8_t byte = 0;
            Read(byte);
            if (byte > 1) throw ProtocolError("invalid IPC boolean");
            value = byte != 0;
        }
        else if constexpr (std::is_arithmetic_v<T>)
        {
            Extract(&value, sizeof(value));
            if constexpr (std::is_floating_point_v<T>)
                if (!std::isfinite(value)) throw ProtocolError("nonfinite value");
        }
        else if constexpr (std::is_enum_v<T>)
        {
            std::underlying_type_t<T> underlying{};
            Read(underlying);
            value = static_cast<T>(underlying);
        }
        else if constexpr (IsString<T>::value)
        {
            std::uint32_t count = 0;
            Read(count);
            if (count > (bytes_.size() - position_) / sizeof(typename T::value_type))
                throw ProtocolError("invalid IPC string length");
            Allocate(static_cast<std::size_t>(count) * sizeof(typename T::value_type));
            value.resize(count);
            Extract(value.data(), value.size() * sizeof(typename T::value_type));
        }
        else if constexpr (std::is_same_v<T, std::filesystem::path>)
        {
            std::filesystem::path::string_type path;
            Read(path);
            if (path.find(L'\0') != path.npos) throw ProtocolError("invalid IPC path");
            value = std::move(path);
        }
        else if constexpr (IsOptional<T>::value)
        {
            bool present = false;
            Read(present);
            value.reset();
            if (present) { Allocate(sizeof(typename T::value_type)); Read(value.emplace()); }
        }
        else if constexpr (IsShared<T>::value)
        {
            bool present = false;
            Read(present);
            value.reset();
            if (present)
            {
                using Element = std::remove_const_t<typename T::element_type>;
                Allocate(sizeof(Element));
                auto element = std::make_shared<Element>();
                Read(*element);
                value = std::move(element);
            }
        }
        else if constexpr (IsVector<T>::value || IsMap<T>::value)
        {
            std::uint32_t count = 0;
            Read(count);
            if (count > MaximumElements) throw ProtocolError("invalid IPC collection length");
            Allocate(static_cast<std::size_t>(count) * sizeof(typename T::value_type));
            value.clear();
            if constexpr (IsVector<T>::value)
            {
                value.resize(count);
                for (auto& element : value) Read(element);
            }
            else for (std::uint32_t i = 0; i < count; ++i)
            {
                typename T::key_type key{};
                typename T::mapped_type mapped{};
                Read(key); Read(mapped);
                if (!value.emplace(std::move(key), std::move(mapped)).second)
                    throw ProtocolError("duplicate IPC map key");
            }
        }
        else if constexpr (std::is_array_v<T>)
            for (auto& element : value) Read(element);
        else if constexpr (requires { std::tuple_size<T>::value; })
            std::apply([this](auto&... element) { (*this)(element...); }, value);
        else
        {
            auto fields = Fields<T>::Tie(value);
            Read(fields);
        }
        --depth_;
    }
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
    std::size_t allocation_ = 0;
    unsigned depth_ = 0;
};

template<class... T> Bytes Pack(const T&... value)
{
    Writer writer;
    writer(value...);
    return writer.Take();
}
template<class T> T Unpack(std::span<const std::byte> bytes)
{
    T result{};
    Reader reader(bytes);
    reader(result);
    reader.Finish();
    return result;
}
} // namespace snowdesktop::settings_ipc
