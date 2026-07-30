#include "nix/expr/writeback-store.hh"
#include "nix/store/build-result.hh"
#include "nix/store/build.hh"
#include "nix/store/derivations.hh"
#include "nix/store/gc-store.hh"
#include "nix/store/log-store.hh"
#include "nix/util/callback.hh"
#include "nix/util/memory-source-accessor.hh"

#include <boost/unordered/concurrent_flat_map.hpp>

namespace nix {

void WritebackStore::sync(const DerivedPath & dp)
{
    sync(dp.getBaseStorePath());
}

void WritebackStore::sync(std::span<const DerivedPath> paths)
{
    for (const auto & dp : paths)
        sync(dp);
}

void WritebackStore::sync(const StorePathSet & paths)
{
    for (const auto & p : paths)
        sync(p);
}

void WritebackStore::anchor() {}

namespace {

class WritebackStoreBuilder : public Builder
{
    ref<Builder> next;
    ref<WritebackStore> wb;

public:
    WritebackStoreBuilder(ref<Builder> next, ref<WritebackStore> wb)
        : next(std::move(next))
        , wb(std::move(wb))
    {
    }

    void buildPaths(const std::vector<DerivedPath> & reqs, BuildMode buildMode) override
    {
        wb->sync(reqs);
        return next->buildPaths(reqs, buildMode);
    }

    std::vector<KeyedBuildResult>
    buildPathsWithResults(const std::vector<DerivedPath> & reqs, BuildMode buildMode) override
    {
        wb->sync(reqs);
        return next->buildPathsWithResults(reqs, buildMode);
    }

    BuildResult buildDerivation(const StorePath & drvPath, const BasicDerivation & drv, BuildMode buildMode) override
    {
        /* The derivation is not supposed to be read from the store here, so don't bother synching. drvPath
           is just an opaque string that's mostly used for locking and nothing else. */
        return next->buildDerivation(drvPath, drv, buildMode);
    }

    void ensurePath(const StorePath & path) override
    {
        /* TODO: If we'd write the path ourselves, we can return early since it's available in the union store. */
        wb->sync(path), next->ensurePath(path);
    }

    void repairPath(const StorePath & path) override
    {
        wb->sync(path), next->repairPath(path);
    }
};

} // namespace

WritebackStore::WritebackStore(const Config & config)
    : Store(config)
{
}

namespace {

/**
 * @brief Asynchronous writeback store.
 */
class WritebackStoreImpl : public WritebackStore
{
    ref<Store> next;

    struct Item
    {
        StorePath storePath;
        /* For now this must point to a source tree with a regular file at the root. */
        ref<MemorySourceAccessor> contents;
        ContentAddressMethod method;
        HashAlgorithm hashAlgo;
        StorePathSet references;
        RepairFlag repair;
        std::promise<void> promise;
    };

    struct State
    {
        std::vector<std::shared_ptr<Item>> writeBackQueue;
        bool quit = false;
    };

    boost::concurrent_flat_map<StorePath, std::weak_ptr<Item>> pathToItem;

    boost::concurrent_flat_map<StorePath, std::shared_future<void>> pathToFuture;

    Sync<State> state_;

    std::condition_variable wakeupCV;

    std::thread workerThread;

    void anchor() override;

public:
    WritebackStoreImpl(ref<Store> next)
        /* We pretend to be the inner store, so just pass through the inner
           store's config. */
        : Store(next->config)
        , WritebackStore(next->config)
        , next(std::move(next))
    {
        /* We won't be doing any caching whatsoever - that's up to the inner store. */
        pathInfoCache = nullptr;
        diskCache = nullptr;
        workerThread = std::thread([&]() { workerThreadMain(); });
    }

    WritebackStoreImpl(WritebackStoreImpl &&) = delete;
    WritebackStoreImpl(const WritebackStoreImpl &) = delete;
    WritebackStoreImpl & operator=(WritebackStoreImpl &&) = delete;
    WritebackStoreImpl & operator=(const WritebackStoreImpl &) = delete;

    ~WritebackStoreImpl()
    {
        {
            auto state(state_.lock());
            state->writeBackQueue.clear();
            state->quit = true;
            wakeupCV.notify_one();
        }

        if (workerThread.joinable())
            workerThread.join();
    }

    void processItem(std::shared_ptr<Item> item)
    {
        /* TODO: Also handle in-memory NARs. For now we are just writing back
           derivations and flat non-executable files. */

        assert(item->method == ContentAddressMethod::Raw::Flat || item->method == ContentAddressMethod::Raw::Text);
        const auto * rootFile = item->contents->open(CanonPath::root, std::nullopt);
        assert(rootFile);
        const auto * contents = std::get_if<MemorySourceAccessor::File::Regular>(&rootFile->raw);
        assert(contents && !contents->executable);

        /* In case the store object is already valid, we bail out early since that's
           faster. But we do have to make sure to temproot it before doing the validity
           check. */
        next->addTempRoot(item->storePath);

        if (next->isValidPath(item->storePath) && !item->repair)
            return;

        StringSource source(contents->contents);
        auto resPath = next->addToStoreFromDump(
            source,
            item->storePath.name(),
            item->method.getFileSerialisationMethod(),
            item->method,
            item->hashAlgo,
            item->references,
            item->repair);

        /* If this breaks - it's a bug. */
        assert(resPath == item->storePath);
    }

    void workerThreadMain()
    {
        while (true) {
            std::vector<std::shared_ptr<Item>> queue;

            {
                auto state(state_.lock());
                while (!state->quit && state->writeBackQueue.empty())
                    state.wait(wakeupCV);
                if (state->writeBackQueue.empty() && state->quit)
                    return;
                std::swap(queue, state->writeBackQueue);
            }

            for (auto & item : queue) {
                try {
                    processItem(item);
                    item->promise.set_value();
                } catch (...) {
                    item->promise.set_exception(std::current_exception());
                }

                /* We won't need it anymore, so run the destructor too. Though
                   we might not be the last to release the refcount - and that's
                   ok. */
                item.reset();
            }
        }
    }

    void sync(const StorePath & path) override
    {
        std::shared_future<void> future;
        pathToFuture.visit(path, [&future](const auto & kv) { future = kv.second; });
        if (future.valid())
            future.get();
        pathToFuture.erase(path);
    }

    void sync() override
    {
        /* TODO: Implement. */
    }

    ref<Store> getInnerStore() override
    {
        return sync(), next;
    }

    using WritebackStore::sync;

    void init() override
    {
        /* Nothing to do, since we already have an opened inner store. */
    }

    ref<Builder> getBuilder(std::shared_ptr<Store> evalStore) override
    {
        return make_ref<WritebackStoreBuilder>(
            next->getBuilder(std::move(evalStore)), ref{std::dynamic_pointer_cast<WritebackStore>(shared_from_this())});
    }

    bool isValidPathUncached(const StorePath & path) override
    {
        /* If an item was written. */
        return pathToItem.contains(path) || next->isValidPath(path);
    }

    StorePathSet queryValidPaths(const StorePathSet & paths, SubstituteFlag maybeSubstitute) override
    {
        return sync(), next->queryValidPaths(paths, maybeSubstitute);
    }

    StorePathSet queryAllValidPaths() override
    {
        /* Not really used in the evaluator, but sane semantics would dictate that we
           see all paths that we have previously enqueued. */
        return sync(), next->queryAllValidPaths();
    }

    bool pathInfoIsUntrusted(const ValidPathInfo & info) override
    {
        return next->pathInfoIsUntrusted(info);
    }

    bool realisationIsUntrusted(const Realisation & realisation) override
    {
        return next->realisationIsUntrusted(realisation);
    }

    void queryPathInfoUncached(
        const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override
    {
        sync(path);
        /* Our uncached access translates into cached lookup in the inner store. We are not doing any caching
           anyway. */
        return next->queryPathInfo(
            path,
            /* TODO: std::move_only_function in Callback. Not yet available on macOS. */
            Callback<ref<const ValidPathInfo>>{[callback = make_ref<decltype(callback)>(std::move(callback))](
                                                   std::future<ref<const ValidPathInfo>> info) mutable {
                try {
                    (*callback)(info.get());
                } catch (InvalidPath &) {
                    (*callback)(nullptr);
                } catch (...) {
                    (*callback).rethrow();
                }
            }});
    }

    void queryRealisationUncached(
        const DrvOutput & output, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override
    {
        return next->queryRealisation(output, std::move(callback));
    }

    void queryReferrers(const StorePath & path, StorePathSet & referrers) override
    {
        /* Not really used by the evaluator. */
        return sync(), next->queryReferrers(path, referrers);
    }

    StorePathSet queryValidDerivers(const StorePath & path) override
    {
        /* Not really used by the evaluator. */
        return sync(path), next->queryValidDerivers(path);
    }

    StorePathSet queryDerivationOutputs(const StorePath & path) override
    {
        return sync(path), next->queryDerivationOutputs(path);
    }

    std::map<std::string, std::optional<StorePath>>
    queryPartialDerivationOutputMap(const StorePath & path, Store * evalStore) override
    {
        return sync(path), next->queryPartialDerivationOutputMap(path, evalStore);
    }

    std::map<std::string, std::optional<StorePath>>
    queryStaticPartialDerivationOutputMap(const StorePath & path) override
    {
        return sync(path), next->queryStaticPartialDerivationOutputMap(path);
    }

    std::optional<StorePath>
    queryStaticPartialDerivationOutput(const StorePath & path, const std::string & outputName) override
    {
        return sync(path), next->queryStaticPartialDerivationOutput(path, outputName);
    }

    std::optional<StorePath> queryPathFromHashPart(const std::string & hashPart) override
    {
        /* Not really used much in the evaluator. */
        return sync(), next->queryPathFromHashPart(hashPart);
    }

    StorePathSet querySubstitutablePaths(const StorePathSet & paths) override
    {
        /* Does not care about path validity, so don't sync(). */
        return next->querySubstitutablePaths(paths);
    }

    void querySubstitutablePathInfos(const StorePathCAMap & paths, SubstitutablePathInfos & infos) override
    {
        /* Does not care about path validity, so don't sync(). */
        return next->querySubstitutablePathInfos(paths, infos);
    }

    void addToStore(const ValidPathInfo & info, Source & narSource, RepairFlag repair, CheckSigsFlag checkSigs) override
    {
        /* Not really used much in the evaluator. */
        return sync(info.references), next->addToStore(info, narSource, repair, checkSigs);
    }

    void
    addMultipleToStore(PathsSource && pathsToCopy, Activity & act, RepairFlag repair, CheckSigsFlag checkSigs) override
    {
        /* Not really used much in the evaluator. */
        return sync(), next->addMultipleToStore(std::move(pathsToCopy), act, repair, checkSigs);
    }

    StorePath addToStore(
        std::string_view name,
        const SourcePath & path,
        ContentAddressMethod method,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        PathFilter & filter,
        RepairFlag repair) override
    {
        /* TODO: Add small files to the writeback queue. Hard to do without resorting to sourceToSink,
           since evaluation should happen on the callers stack. */

        /* There can't be a self-reference, because the path is
           content-addressed without modulus. */
        return sync(references), next->addToStore(name, path, method, hashAlgo, references, filter, repair);
    }

    StorePath addToStoreFromDump(
        Source & dump,
        std::string_view name,
        FileSerialisationMethod dumpMethod,
        ContentAddressMethod hashMethod,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        RepairFlag repair) override
    {
        /* There can't be a self-reference, because the path is
           content-addressed without modulus. */
        return sync(references),
               next->addToStoreFromDump(dump, name, dumpMethod, hashMethod, hashAlgo, references, repair);
    }

    void registerDrvOutput(const Realisation & output) override
    {
        /* Not really used by the evaluator or anything at all really. */
        sync(output.id.drvPath), next->registerDrvOutput(output);
    }

    void registerDrvOutput(const Realisation & output, CheckSigsFlag checkSigs) override
    {
        /* Not really used by the evaluator or anything at all really. */
        sync(output.id.drvPath), next->registerDrvOutput(output, checkSigs);
    }

    void narFromPath(const StorePath & path, Sink & sink) override
    {
        /* TODO: Read from the writeback queue if possible. */
        sync(path), next->narFromPath(path, sink);
    }

    void addTempRoot(const StorePath & path) override
    {
        next->addTempRoot(path);
    }

    bool verifyStore(bool checkContents, RepairFlag repair = NoRepair) override
    {
        return sync(), next->verifyStore(checkContents, repair);
    };

    ref<SourceAccessor> getFSAccessor(bool requireValidPath = true) override
    {
        return makeUnionSourceAccessor({next->getFSAccessor(requireValidPath), make_ref<MemorySourceAccessor>()});
    }

    std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath & path, bool requireValidPath = true) override
    {
        return sync(path), next->getFSAccessor(path, requireValidPath);
    }

    void addSignatures(const StorePath & storePath, const std::set<Signature> & sigs) override
    {
        return sync(storePath), next->addSignatures(storePath, sigs);
    }

    Derivation readDerivation(const StorePath & drvPath) override
    {
        return sync(drvPath), next->readDerivation(drvPath);
    }

    Derivation readInvalidDerivation(const StorePath & drvPath) override
    {
        return sync(drvPath), next->readInvalidDerivation(drvPath);
    }

    StorePath writeDerivation(const Derivation & drv, RepairFlag repair) override
    {
        auto [suffix, contents, references, path] = infoForDerivation(*this, drv);
        auto state(state_.lock());
        std::promise<void> promise;
        std::shared_future<void> future = promise.get_future();
        state->writeBackQueue.push_back(
            make_ref<Item>(Item{
                .storePath = path,
                .contents =
                    [&]() {
                        auto accessor = make_ref<MemorySourceAccessor>();
                        accessor->addFile(CanonPath::root, std::move(contents));
                        return accessor;
                    }(),
                .method = ContentAddressMethod::Raw::Text,
                .hashAlgo = HashAlgorithm::SHA256,
                .references = std::move(references),
                .repair = repair,
                .promise = std::move(promise),
            }));
        return path;
    }

    void computeFSClosure(
        const StorePathSet & paths,
        StorePathSet & out,
        bool flipDirection = false,
        bool includeOutputs = false,
        bool includeDerivers = false) override
    {
        return sync(paths), next->computeFSClosure(paths, out, flipDirection, includeOutputs, includeDerivers);
    }

    MissingPaths queryMissing(const std::vector<DerivedPath> & targets) override
    {
        return sync(targets), next->queryMissing(targets);
    }

    StorePaths topoSortPaths(const StorePathSet & paths) override
    {
        return sync(paths), next->topoSortPaths(paths);
    }

    void connect() override
    {
        return next->connect();
    }

    unsigned int getProtocol() override
    {
        return next->getProtocol();
    };

    std::optional<TrustedFlag> isTrustedClient() override
    {
        return next->isTrustedClient();
    }

    void setOptions() override
    {
        return next->setOptions();
    }

    std::optional<std::string> getVersion() override
    {
        return next->getVersion();
    }
};

/* This is somewhat load-bearing to preserve the behavior of dynamic_cast that's
   used to conditionally downcast to mixin store types (nix::require). It's
   unavoidable that we have to also implement wrapper mixins here of course as
   that's also a conditionally implemented part of the Store interface. Since
   there's only a few mixin interfaces it's not that big of a burden to handle
   the tiny combinatorial explosion here for now. */

class WritebackLogStoreImpl : public LogStore, virtual public WritebackStoreImpl
{
    void anchor() override;

    ref<LogStore> nextLogStore;

public:
    WritebackLogStoreImpl(ref<LogStore> next)
        : Store(next->config)
        , WritebackStoreImpl(next.cast<Store>())
        , nextLogStore(std::move(next))
    {
    }

    std::optional<std::string> getBuildLogExact(const StorePath & path) override
    {
        return nextLogStore->getBuildLogExact(path);
    }

    void addBuildLog(const StorePath & path, std::string_view log) override
    {
        return nextLogStore->addBuildLog(path, log);
    }
};

void WritebackLogStoreImpl::anchor() {}

class WritebackGcStoreImpl : public GcStore, virtual public WritebackStoreImpl
{
    void anchor() override;

    ref<GcStore> nextGcStore;

public:
    /* We don't have to bother sync-ing anything because everything that was
       written and would be written is effectively rooted anyway. */

    Roots findRoots(bool censor) override
    {
        return nextGcStore->findRoots(censor);
    }

    void collectGarbage(const GCOptions & options, GCResults & results) override
    {
        return nextGcStore->collectGarbage(options, results);
    }

    void deleteBuildTraces(const std::set<DrvOutput> & keys) override
    {
        return nextGcStore->deleteBuildTraces(keys);
    }

    WritebackGcStoreImpl(ref<GcStore> next)
        : Store(next->config)
        , WritebackStoreImpl(next.cast<Store>())
        , nextGcStore(std::move(next))
    {
    }
};

void WritebackGcStoreImpl::anchor() {}

class WritebackGcLogStoreImpl : public WritebackGcStoreImpl, public WritebackLogStoreImpl
{
    void anchor() override;

public:
    WritebackGcLogStoreImpl(ref<Store> next)
        : Store(next->config)
        , WritebackStoreImpl(next)
        , WritebackGcStoreImpl(ref{next.dynamic_pointer_cast<GcStore>()})
        , WritebackLogStoreImpl(ref{next.dynamic_pointer_cast<LogStore>()})
    {
    }
};

void WritebackGcLogStoreImpl::anchor() {}

} // namespace

ref<WritebackStore> WritebackStore::make(ref<Store> next)
{
    auto maybeGcStore = std::dynamic_pointer_cast<GcStore>(next.get_ptr());
    auto maybeLogStore = std::dynamic_pointer_cast<LogStore>(next.get_ptr());

    if (maybeGcStore) {
        if (maybeLogStore)
            return make_ref<WritebackGcLogStoreImpl>(std::move(next)).cast<WritebackStore>();
        return make_ref<WritebackGcStoreImpl>(ref{maybeGcStore}).cast<WritebackStore>();
    }

    if (maybeLogStore)
        return make_ref<WritebackLogStoreImpl>(ref{maybeLogStore}).cast<WritebackStore>();

    return make_ref<WritebackStoreImpl>(std::move(next)).cast<WritebackStore>();
}

void WritebackStoreImpl::anchor() {}

} // namespace nix
