#pragma once

#include <OpenHome/Private/Thread.h>
#include <OpenHome/Private/Standard.h>
#include <OpenHome/Private/Printer.h>

#include <vector>
#include <algorithm>
#include <functional>

namespace OpenHome {

template<typename TObserver>
class IObservable
{
public:
    virtual ~IObservable() { }

    virtual void AddObserver(TObserver& aObserver, const TChar* aId) = 0;
    virtual void RemoveObserver(TObserver& aObserver) = 0;
};

/* Helper class to aid in implementing the observable pattern with multiple observers.
 * Class is designed to be used either as an owned component or can be inherited from
 * directly.
 *
 * This class is not thread safe. See 'ThreadSafeObservable' for a thread-safe version.
 * Otherwise, it's up to the caller to implement a suitable thread-safe approach.
 *
 * This class does no additional checking for duplicate watchers on add, or non-existant
 * watchers on removal.
 *
 * It's expected that the notify function should be used alongside a lambda function call.
 * E.g: (This assumes Klass has inherited from Observable<TObserver>)
 *  void Klass::Foo() {
 *      DoSomeWork();
 *
 *      NotifyAll([] (TObserver& o) {
 *          o.NotifyOfCoolThing();
 *      });
 *  }
 *
 * If you need to pass parameters to the observer, you can do so by capturing the correct
 * context in your lamba.
 * E.g:
 *  void Klass::Bar() {
 *      int baz1 = DoSomeWork();
 *      int baz2 = DoSomeMoreWork();
 *
 *      NotifyAll([&] (TObserver& o) {
 *          o.ValuesComputed(baz1, baz2);
 *      });
 *  }
 */
template<typename TObserver>
class Observable : public IObservable<TObserver>
{
    public:
        Observable() { }
        ~Observable()
        {
            if (iObservers.size() > 0) {
                Log::Print("ERROR: %u Observable observers leaked:\n", (TUint)iObservers.size());
                for (auto p : iObservers) {
                    Log::Print("\t%s\n", p.second);
                }
                ASSERTS();
            }
        }

    public:
        void AddObserver(TObserver& aObserver, const TChar* aId) override
        {
            iObservers.push_back(std::pair<std::reference_wrapper<TObserver>, const TChar*>(aObserver, aId));
        }

        void RemoveObserver(TObserver& aObserver) override
        {
            for (auto it = iObservers.begin(); it != iObservers.end(); ++it) {
                auto& o = (*it).first.get();
                if (&o == &aObserver) {
                    iObservers.erase(it);
                    return;
                }
            }
        }

        void NotifyAll(std::function<void (TObserver&)> aNotifyFunc)
        {
            for (auto it = iObservers.cbegin(); it != iObservers.cend(); ++it) {
                aNotifyFunc(it->first.get());
            }
            //std::for_each(iObservers.cbegin(), iObservers.cend(), aNotifyFunc);
        }

        void NotifyAll(FunctorGeneric<TObserver&>& aNotifyFunc)
        {
            for(auto it = iObservers.cbegin(); it != iObservers.cend(); ++it) {
                aNotifyFunc(it->first.get());
            }
        }

    protected:
        std::vector<std::pair<std::reference_wrapper<TObserver>, const TChar*>> iObservers;
};

/* Class that provides a mutex lock around Observable class methods.
 * See Observable<TObserver> for intended usage. */
template<typename TObserver>
class ThreadSafeObservable : public Observable<TObserver>
{
    public:
        ThreadSafeObservable()
            : iLock("TSOB")
        { }
        ~ThreadSafeObservable() { }

    public: // NOTE: These methods 'hide' the base Observable<TObserver> methods.
        void AddObserver(TObserver& aObserver, const TChar* aId) override
        {
            AutoMutex m(iLock);
            Observable<TObserver>::AddObserver(aObserver, aId);
        }

        void RemoveObserver(TObserver& aObserver) override
        {
            AutoMutex m(iLock);
            Observable<TObserver>::RemoveObserver(aObserver);
        }

        void NotifyAll(std::function<void (TObserver&)> aNotifyFunc)
        {
            AutoMutex m(iLock);
            Observable<TObserver>::NotifyAll(aNotifyFunc);
        }

        void NotifyAll(FunctorGeneric<TObserver&>& aNotifyFunc)
        {
            AutoMutex m(iLock);
            Observable<TObserver>::NotifyAll(aNotifyFunc);
        }

    private:
        Mutex iLock;
};

} // namespace OpenHome
