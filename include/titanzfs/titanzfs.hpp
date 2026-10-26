#pragma once

#include "titanzfs/types.hpp"
#include "titanzfs/fletcher4.hpp"
#include "titanzfs/vdev.hpp"
#include "titanzfs/dmu.hpp"
#include "titanzfs/spa.hpp"

namespace titanzfs {

// ============================================================================
// Global Pool Registry (Subsystem Singleton)
// ============================================================================

class TitanZfsSubsystem {
public:
    static TitanZfsSubsystem& Get() noexcept {
        static TitanZfsSubsystem instance;
        return instance;
    }

    Status registerPool(std::shared_ptr<ZfsPool> pool) {
        if (!pool) return Status::InvalidParameter;
        if (pools_.contains(pool->getName())) return Status::ObjectNameCollision;
        pools_[pool->getName()] = std::move(pool);
        return Status::Success;
    }

    [[nodiscard]] std::shared_ptr<ZfsPool> getPool(const std::string& name) const {
        auto it = pools_.find(name);
        return (it != pools_.end()) ? it->second : nullptr;
    }

    [[nodiscard]] const std::map<std::string, std::shared_ptr<ZfsPool>>& getPools() const noexcept {
        return pools_;
    }

    Status destroyPool(const std::string& name) {
        auto it = pools_.find(name);
        if (it == pools_.end()) return Status::ObjectNameNotFound;
        pools_.erase(it);
        return Status::Success;
    }

private:
    TitanZfsSubsystem() = default;
    std::map<std::string, std::shared_ptr<ZfsPool>> pools_;
};

} // namespace titanzfs
