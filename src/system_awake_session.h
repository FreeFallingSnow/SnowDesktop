#pragma once
#include "system_controls.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace snowdesktop::system_control
{
// Host-only session policy. A timer owns the expiry independently of panel
// visibility or data subscriptions; the platform boundary owns power handles.
enum class AwakeMode { Plan, Indefinite, Timed };
struct AwakeState
{
    AwakeMode mode = AwakeMode::Plan;
    bool keepScreenOn = false;
    std::chrono::seconds duration{0}, remaining{0};
};
class AwakeSession
{
    using Clock = std::chrono::steady_clock;
    std::function<Result(bool, const std::string&)> acquire_;
    std::function<void()> release_;
    std::mutex mutex_;
    std::condition_variable changed_;
    AwakeState state_;
    Clock::time_point deadline_{};
    std::string reason_;
    bool stopping_ = false;
    std::uint64_t revision_ = 0;
    std::thread timer_;
    void RestorePlan()
    {
        if(state_.mode!=AwakeMode::Plan)release_();
        state_.mode=AwakeMode::Plan;state_.remaining=std::chrono::seconds{0};++revision_;
    }
    void Expire()
    {if(state_.mode==AwakeMode::Timed&&Clock::now()>=deadline_)RestorePlan();}
    void Run()
    {
        std::unique_lock guard(mutex_);
        while(!stopping_)
        {
            if(state_.mode!=AwakeMode::Timed)
            {changed_.wait(guard,[this]{return stopping_||state_.mode==AwakeMode::Timed;});continue;}
            const auto revision=revision_;
            changed_.wait_until(guard,deadline_,[this,revision]{return stopping_||revision_!=revision;});
            if(!stopping_)Expire();
        }
    }
public:
    AwakeSession(std::function<Result(bool,const std::string&)> acquire,std::function<void()> release)
        :acquire_(std::move(acquire)),release_(std::move(release)),timer_([this]{Run();}){}
    AwakeSession(const AwakeSession&)=delete;
    AwakeSession& operator=(const AwakeSession&)=delete;
    ~AwakeSession()
    {
        {std::lock_guard guard(mutex_);stopping_=true;changed_.notify_all();}
        timer_.join();std::lock_guard guard(mutex_);RestorePlan();
    }
    AwakeState Read()
    {
        std::lock_guard guard(mutex_);Expire();auto value=state_;
        if(value.mode==AwakeMode::Timed)value.remaining=std::chrono::ceil<std::chrono::seconds>(deadline_-Clock::now());
        return value;
    }
    Result Set(AwakeMode mode,std::chrono::seconds duration,bool keepScreenOn,std::string reason)
    {
        if(mode==AwakeMode::Timed&&(duration<=std::chrono::seconds{0}||duration>std::chrono::hours{24}))return {false,"invalidArguments",0};
        std::lock_guard guard(mutex_);Expire();
        if(mode==AwakeMode::Plan){RestorePlan();changed_.notify_all();return {true,{},0};}
        const auto result=acquire_(keepScreenOn,reason);if(!result.ok)return result;
        state_.mode=mode;state_.keepScreenOn=keepScreenOn;state_.duration=duration;reason_=std::move(reason);
        deadline_=Clock::now()+duration;++revision_;changed_.notify_all();return result;
    }
    Result SetScreen(bool keepScreenOn)
    {
        std::lock_guard guard(mutex_);Expire();
        if(state_.mode==AwakeMode::Plan)return {false,"actionUnsupported",0};
        const auto result=acquire_(keepScreenOn,reason_);if(result.ok)state_.keepScreenOn=keepScreenOn;
        // Changing screen policy does not restart a timed session.
        return result;
    }
};
}
