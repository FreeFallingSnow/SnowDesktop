// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace snowdesktop::steam_bridge
{
enum class PublishLifecycleState
{
    Idle,
    Preparing,
    ItemCreated,
    Submitting,
    Succeeded,
    Failed,
};

class PublishLifecycle
{
public:
    bool Begin(bool creating)
    {
        if (state_ != PublishLifecycleState::Idle) return false;
        creating_ = creating;
        state_ = PublishLifecycleState::Preparing;
        return true;
    }

    bool ItemCreated(std::uint64_t publishedFileId)
    {
        if (!creating_ || state_ != PublishLifecycleState::Preparing ||
            publishedFileId == 0)
            return false;
        publishedFileId_ = publishedFileId;
        state_ = PublishLifecycleState::ItemCreated;
        return true;
    }

    bool BindExisting(std::uint64_t publishedFileId)
    {
        if (creating_ || state_ != PublishLifecycleState::Preparing ||
            publishedFileId == 0)
            return false;
        publishedFileId_ = publishedFileId;
        return true;
    }

    bool SubmitStarted()
    {
        if (state_ != PublishLifecycleState::Preparing &&
            state_ != PublishLifecycleState::ItemCreated)
            return false;
        if (publishedFileId_ == 0) return false;
        state_ = PublishLifecycleState::Submitting;
        return true;
    }

    bool Succeed()
    {
        if (state_ != PublishLifecycleState::Submitting) return false;
        state_ = PublishLifecycleState::Succeeded;
        return true;
    }

    bool Fail()
    {
        if (state_ == PublishLifecycleState::Idle ||
            state_ == PublishLifecycleState::Succeeded ||
            state_ == PublishLifecycleState::Failed)
            return false;
        state_ = PublishLifecycleState::Failed;
        return true;
    }

    PublishLifecycleState State() const { return state_; }
    std::uint64_t PublishedFileId() const { return publishedFileId_; }
    bool CanCancel() const
    {
        return state_ == PublishLifecycleState::Preparing ||
            state_ == PublishLifecycleState::ItemCreated;
    }
    bool MustPersistCreatedItem() const
    {
        return creating_ && publishedFileId_ != 0;
    }

private:
    PublishLifecycleState state_ = PublishLifecycleState::Idle;
    bool creating_ = false;
    std::uint64_t publishedFileId_ = 0;
};
}
