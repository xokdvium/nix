#pragma once
///@file

#include "nix/store/store-api.hh"
#include "nix/util/ref.hh"

namespace nix {

class WritebackStore : public virtual Store
{
    void anchor() override;

protected:
    explicit WritebackStore(const Config & config);

public:
    virtual void sync(const StorePath & path) = 0;

    virtual void sync() = 0;

    virtual void sync(const StorePathSet & paths);

    virtual ref<Store> getInnerStore() = 0;

    void sync(const DerivedPath & paths);

    void sync(std::span<const DerivedPath> paths);

    static ref<WritebackStore> make(ref<Store> next);
};

} // namespace nix
