#pragma once

namespace snowdesktop
{
// Arm only the setter's next save, never all saves while a pointer gesture is
// present. Consuming the token before scheduling also keeps nested work and
// subsequent settings/exit boundaries on their ordinary synchronous path.
class InteractiveLayoutSave final
{
public:
    explicit InteractiveLayoutSave(const void* owner) : previous_(owner_) { owner_ = owner; }
    ~InteractiveLayoutSave() { owner_ = previous_; }
    InteractiveLayoutSave(const InteractiveLayoutSave&) = delete;
    InteractiveLayoutSave& operator=(const InteractiveLayoutSave&) = delete;

    static bool Consume(const void* owner)
    {
        if (!owner || owner_ != owner) return false;
        owner_ = nullptr;
        return true;
    }
private:
    const void* previous_;
    static inline thread_local const void* owner_ = nullptr;
};
}
