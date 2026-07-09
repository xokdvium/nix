#pragma once
///@file

#include "nix/store/store-api.hh"
#include "nix/store/build-result.hh"
#include "nix/util/async.hh"

namespace nix {

class TimedOut final : public CloneableError<TimedOut, BuildError>
{
    void anchor() override;

public:
    time_t maxDuration;

    TimedOut(time_t maxDuration);
};

/**
 * Forward definition.
 */
struct Goal;
class Worker;

/**
 * A pointer to a goal.
 */
typedef std::shared_ptr<Goal> GoalPtr;
typedef std::weak_ptr<Goal> WeakGoalPtr;

struct CompareGoalPtrs
{
    bool operator()(const GoalPtr & a, const GoalPtr & b) const;
};

/**
 * Set of goals.
 */
typedef std::set<GoalPtr, CompareGoalPtrs> Goals;
typedef std::set<WeakGoalPtr, std::owner_less<WeakGoalPtr>> WeakGoals;

/**
 * A map of paths to goals (and the other way around).
 */
typedef std::map<StorePath, WeakGoalPtr> WeakGoalMap;

/**
 * Used as a hint to the worker on how to schedule a particular goal. For example,
 * builds are typically CPU- and memory-bound, while substitutions are I/O bound.
 * Using this information, the worker might decide to schedule more or fewer goals
 * of each category in parallel.
 */
enum struct JobCategory {
    /**
     * A build of a derivation; it will use CPU and disk resources.
     */
    Build,
    /**
     * A substitution an arbitrary store object; it will use network resources.
     */
    Substitution,
    /**
     * A goal that does no "real" work by itself, and just exists to depend on
     * other goals which *do* do real work. These goals therefore are not
     * limited.
     *
     * These goals cannot infinitely create themselves, so there is no risk of
     * a "fork bomb" type situation (which would be a problem even though the
     * goal do no real work) either.
     */
    Administration,
};

struct Goal : public std::enable_shared_from_this<Goal>
{
private:
    /* VTable anchor to avoid weak linkage of the vtable - it breaks
       dynamic_cast across shared libraries on Darwin. */
    virtual void anchor();

    /**
     * Memoised result of key().
     */
    std::optional<std::string> cachedKey;

    /**
     * Cancellation signal connected to the cancellation slot of the top-level
     * coroutine of this goal.
     */
    asio::cancellation_signal cancelSignal;

    /**
     * Stores the exception that the completion handler has received.
     */
    std::exception_ptr error;

    /**
     * A list of completion handlers (or rather functions invoking those
     * completions handlers) awaiting for this goal to complete.
     */
    std::list<std::move_only_function<void(boost::system::error_code)>> waiters;

    std::size_t numWaiters = 0;

    /**
     * Whether the top-level coroutine has been "co_spawned".
     */
    bool spawned = false;

    /* TODO: Not really used but seems useful? */
    bool hasResult() const noexcept
    {
        return exitCode != ExitCode::ecBusy;
    }

    /**
     * Whether the goal has run to completion.
     */
    bool isDone() const noexcept
    {
        return exitCode != ExitCode::ecBusy || error;
    }

    /**
     * Obtain the top-level coroutine that, when driven to completion, will
     * produce a BuildResult representing the result of "finishing" this goal.
     *
     * Must be called only once.
     *
     * @todo Ideally this would be an awaitable<BuildResult> and it wasn't
     * smuggled through a member.
     */
    virtual asio::awaitable<void> run() = 0;

    /**
     * Await for the goal to complete.
     */
    static asio::awaitable<void> join(GoalPtr goal);

    /**
     * Await for a set of goals to complete.
     */
    static asio::awaitable<void> join(Goals goals, bool keepGoing);

    /**
     * Lazily spawn a coroutine produced by Goal::run().
     */
    void maybeSpawnLazily();

    /**
     * "Finish" the goal by invoking the completion handler that also notifies
     * all the waiters.
     */
    void finish(std::exception_ptr ex);


public:
    typedef enum { ecBusy, ecSuccess, ecFailed, ecNoSubstituters } ExitCode;

    /**
     * Backlink to the worker.
     */
    Worker & worker;

    /**
     * Number of goals we are/were waiting for that have failed.
     */
    size_t nrFailed = 0;

    /**
     * Number of substitution goals we are/were waiting for that
     * failed because there are no substituters.
     */
    size_t nrNoSubstituters = 0;

    /**
     * Name of this goal for debugging purposes.
     */
    std::string name;

    /**
     * Whether the goal is finished.
     */
    ExitCode exitCode = ecBusy;

    /**
     * Build result.
     */
    BuildResult buildResult;

protected:
    /**
     * Signals that the goal is done.
     * `co_return` the result. If you're not inside a coroutine, you can ignore
     * the return value safely.
     *
     * Prefer using `doneSuccess` or `doneFailure` instead, which ensure
     * `buildResult` is set correctly.
     */
    void amDone(ExitCode result);

    /**
     * Signals successful completion of the goal.
     * Sets `buildResult` and calls `amDone`.
     */
    void doneSuccess(BuildResult::Success success);

    /**
     * Signals failed completion of the goal.
     * Sets `buildResult` and calls `amDone`.
     *
     * @param result The exit code (ecFailed or ecNoSubstituters)
     * @param failure The failure details including status and error message
     */
    void doneFailure(ExitCode result, BuildResult::Failure failure);

    /**
     * @brief Wait for a set of Goals.
     *
     * Awaiting on the resulting coroutine will suspend the caller until:
     *
     * - Without --keep-going, until all waitees complete successfully, or any one of them fails.
     *   In the latter case, other waitees will be cancelled.
     * - Otherwise until goals complete (successfully or with failures).
     *
     * If any goal throws an exception, we'll receive it and are expected to propagate it up
     * the awaitable chain to the top level.
     */
    asio::awaitable<void> await(Goals waitees);

public:
    /**
     * Hack to say that this goal should not log the failure, but instead keep
     * it around. Set by a waitee which sees itself as the designated
     * continuation of this goal, responsible for reporting its
     * successes or failures.
     *
     * @todo this is yet another not-nice hack in the goal system that
     * we ought to get rid of. See #11927
     */
    bool preserveFailure = false;

    Goal(Worker & worker)
        : worker(worker)
    {
    }

    Goal(Goal &&) = delete;
    Goal(const Goal &) = delete;
    Goal & operator=(Goal &&) = delete;
    Goal & operator=(const Goal &) = delete;

    virtual ~Goal()
    {
        trace("goal destroyed");
    }

    void trace(std::string_view s);

    std::string getName() const
    {
        return name;
    }

    /**
     * Used for comparisons. The order matters a bit for scheduling. We
     * want:
     *
     * 1. Substitution
     * 2. Derivation administrativia
     * 3. Actual building
     *
     * Also, ensure that derivations get processed in order of their
     * name, i.e. a derivation named "aardvark" always comes before
     * "baboon".
     */
    virtual std::string key() = 0;

    /**
     * Memoising variant of key(). We really don't want to pay the overhead of
     * allocating strings just to compare Goals.
     */
    std::string_view keyCached() &
    {
        if (cachedKey)
            return *cachedKey;
        return *(cachedKey = key());
    }

    /**
     * @brief Hint for the scheduler, which concurrency limit applies.
     * @see JobCategory
     */
    virtual JobCategory jobCategory() const = 0;
};

void addToWeakGoals(WeakGoals & goals, GoalPtr p);

} // namespace nix
