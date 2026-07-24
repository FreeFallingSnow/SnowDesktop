#pragma once

#include <functional>
#include <memory>

class SearchVisibilityDetector
{
public:
    explicit SearchVisibilityDetector(std::function<void()> stateChanged);
    ~SearchVisibilityDetector();

    SearchVisibilityDetector(const SearchVisibilityDetector&) = delete;
    SearchVisibilityDetector& operator=(const SearchVisibilityDetector&) =
        delete;

    bool IsAvailable() const noexcept;
    bool IsVisible() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
