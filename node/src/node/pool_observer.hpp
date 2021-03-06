#pragma once

#include <string>

namespace cocaine {
namespace service {
namespace node {

class pool_observer {
public:
    virtual ~pool_observer() = default;

    /// Called when a slave was spawned.
    virtual
    auto
    spawned(const std::string& id) -> void = 0;

    virtual
    auto
    despawned(const std::string& id) -> void = 0;

    virtual
    auto
    forced_unpublish() -> void {}

    virtual
    auto
    maybe_publish() -> void {}
};

} // namespace node
} // namespace service
} // namespace cocaine
