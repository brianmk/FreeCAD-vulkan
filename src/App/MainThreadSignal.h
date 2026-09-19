// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2025 Joao Matos
// SPDX-FileNotice: Part of the FreeCAD project.

/******************************************************************************
 *                                                                            *
 *   FreeCAD is free software: you can redistribute it and/or modify          *
 *   it under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1            *
 *   of the License, or (at your option) any later version.                   *
 *                                                                            *
 *   FreeCAD is distributed in the hope that it will be useful,               *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty              *
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                  *
 *   See the GNU Lesser General Public License for more details.              *
 *                                                                            *
 *   You should have received a copy of the GNU Lesser General Public         *
 *   License along with FreeCAD. If not, see https://www.gnu.org/licenses     *
 *                                                                            *
 ******************************************************************************/

#ifndef APP_MAINTHREADSIGNAL_H
#define APP_MAINTHREADSIGNAL_H

#include <Base/Interpreter.h>
#include <fastsignals/signal.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace App
{

// Forward declarations: deferred delivery only needs pointers to these types in
// this header. The concrete encode/decode logic lives in Application.cpp where
// the types are complete.
class Document;
class DocumentObject;
class Property;

// Per-signal delivery policy. The choice is made at the signal declaration so
// that the blocking/queueing decision is explicit and reviewed, instead of
// being implicit in every emit() call.
//
// See src/Doc/AsyncRecompute.dox for the policy applied to each Document signal.
enum class DeliveryPolicy
{
    // Hop to the main thread and wait for the slot to finish. This is the
    // historical behaviour. It is required for non-void signals and for signals
    // whose observers must run before the emitter continues (object deletion,
    // recompute barriers, file I/O, transactions).
    Blocking,
    // Hop to the main thread without waiting. Arguments are captured as stable
    // identity handles and resolved back to live objects on the main thread, so
    // deferred delivery cannot dereference a destroyed object.
    Queued,
    // Like Queued, but repeated emissions that resolve to the same argument
    // identity collapse into a single main-thread delivery. Intended for
    // high-frequency state notifications where observers only need the latest
    // state (change/touch during a worker recompute).
    Coalesced,
};

class MainThreadSignalConfig
{
public:
    using IsMainThreadFn = bool (*)();  // true iff currently on GUI/main thread
    using InvokeFn = void (*)(std::function<void()>&& fn, bool blocking);

    // Stable identity handles. Captured on the emitting thread and resolved on
    // the main thread; never dereferenced directly.
    struct DocumentHandle
    {
        std::string name;
    };
    struct ObjectHandle
    {
        std::string document;
        std::string object;
    };
    struct PropertyHandle
    {
        std::string document;
        std::string object;
        std::string property;
    };

    using EncodeDocumentFn = DocumentHandle (*)(const Document*);
    using EncodeObjectFn = ObjectHandle (*)(const DocumentObject*);
    using EncodePropertyFn = PropertyHandle (*)(const Property*);
    using ResolveDocumentFn = const Document* (*)(const DocumentHandle&);
    using ResolveObjectFn = const DocumentObject* (*)(const ObjectHandle&);
    using ResolvePropertyFn = const Property* (*)(const PropertyHandle&);

    static void setHooks(IsMainThreadFn isMainThread, InvokeFn invoke)
    {
        isMainThreadSlot() = isMainThread;
        invokeSlot() = invoke;
    }

    // Set by the main thread while it is blocked waiting for the recompute
    // worker (document close, application shutdown). A cross-thread delivery
    // that would otherwise park the worker on the main thread is abandoned
    // instead, so the two threads cannot deadlock.
    static inline void setMainThreadWaiting(bool waiting)
    {
        mainThreadWaitingFlag().store(waiting, std::memory_order_release);
    }

    static inline bool mainThreadWaiting()
    {
        return mainThreadWaitingFlag().load(std::memory_order_acquire);
    }

    // Installed once by App, where Document/DocumentObject/Property are complete.
    static void setDeferredHooks(
        EncodeDocumentFn encodeDocument,
        EncodeObjectFn encodeObject,
        EncodePropertyFn encodeProperty,
        ResolveDocumentFn resolveDocument,
        ResolveObjectFn resolveObject,
        ResolvePropertyFn resolveProperty
    )
    {
        encodeDocumentSlot() = encodeDocument;
        encodeObjectSlot() = encodeObject;
        encodePropertySlot() = encodeProperty;
        resolveDocumentSlot() = resolveDocument;
        resolveObjectSlot() = resolveObject;
        resolvePropertySlot() = resolveProperty;
    }

    static inline bool isMainThread()
    {
        auto* f = isMainThreadSlot();
        return f ? f() : true;  // no hooks, treat current thread as "main"
    }

    static inline bool hasHooks()
    {
        return isMainThreadSlot() && invokeSlot();
    }

    static inline void invoke(std::function<void()>&& fn, bool blocking)
    {
        auto* f = invokeSlot();
        if (f) {
            f(std::move(fn), blocking);
        }
        else {
            fn();  // no hooks, run inline
        }
    }

    static inline DocumentHandle encodeDocument(const Document* document)
    {
        auto* f = encodeDocumentSlot();
        return f ? f(document) : DocumentHandle {};
    }

    static inline ObjectHandle encodeObject(const DocumentObject* object)
    {
        auto* f = encodeObjectSlot();
        return f ? f(object) : ObjectHandle {};
    }

    static inline PropertyHandle encodeProperty(const Property* property)
    {
        auto* f = encodePropertySlot();
        return f ? f(property) : PropertyHandle {};
    }

    static inline const Document* resolveDocument(const DocumentHandle& handle)
    {
        auto* f = resolveDocumentSlot();
        return f ? f(handle) : nullptr;
    }

    static inline const DocumentObject* resolveObject(const ObjectHandle& handle)
    {
        auto* f = resolveObjectSlot();
        return f ? f(handle) : nullptr;
    }

    static inline const Property* resolveProperty(const PropertyHandle& handle)
    {
        auto* f = resolvePropertySlot();
        return f ? f(handle) : nullptr;
    }

private:
    static IsMainThreadFn& isMainThreadSlot()
    {
        static IsMainThreadFn fn = nullptr;
        return fn;
    }
    static InvokeFn& invokeSlot()
    {
        static InvokeFn fn = nullptr;
        return fn;
    }
    static std::atomic<bool>& mainThreadWaitingFlag()
    {
        static std::atomic<bool> flag {false};
        return flag;
    }
    static EncodeDocumentFn& encodeDocumentSlot()
    {
        static EncodeDocumentFn fn = nullptr;
        return fn;
    }
    static EncodeObjectFn& encodeObjectSlot()
    {
        static EncodeObjectFn fn = nullptr;
        return fn;
    }
    static EncodePropertyFn& encodePropertySlot()
    {
        static EncodePropertyFn fn = nullptr;
        return fn;
    }
    static ResolveDocumentFn& resolveDocumentSlot()
    {
        static ResolveDocumentFn fn = nullptr;
        return fn;
    }
    static ResolveObjectFn& resolveObjectSlot()
    {
        static ResolveObjectFn fn = nullptr;
        return fn;
    }
    static ResolvePropertyFn& resolvePropertySlot()
    {
        static ResolvePropertyFn fn = nullptr;
        return fn;
    }
};

namespace detail
{
// Holds a value (for by-value params)
template<class T>
struct SignalArgValue
{
    T v;
    constexpr decltype(auto) get() noexcept
    {
        return (v);
    }
    constexpr decltype(auto) get() const noexcept
    {
        return (v);
    }
};

// Holds a pointer (for by-reference params, preserves cv-qualifiers)
template<class T>
struct SignalArgRef
{
    using Raw = std::remove_reference_t<T>;  // keeps const if present
    Raw* p {};                               // pointer preserves cv-ness
    constexpr decltype(auto) get() const noexcept
    {
        return *p;
    }  // Raw& or const Raw&
};

// Explicitly choose storage kind from the declared parameter type PDecl
template<class PDecl>
auto captureSignalArg(PDecl&& x)
{
    if constexpr (std::is_lvalue_reference_v<PDecl>) {
        using Raw = std::remove_reference_t<PDecl>;
        return SignalArgRef<Raw> {std::addressof(x)};  // &param → pointer
    }
    else {
        using V = std::decay_t<PDecl>;
        return SignalArgValue<V> {std::forward<PDecl>(x)};  // value param → by value
    }
}

template<class T>
using non_void_t = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

template<class T>
struct always_false: std::false_type
{
};

// Liveness token shared by every deferred callback of one signal. Marked dead
// when the signal (and therefore its owning Document) is destroyed, so a queued
// callback never dereferences a dangling signal.
struct DeferredLiveness
{
    std::atomic<bool> alive {true};
};

// Pending coalesced emissions, keyed by the identity of the captured arguments.
struct CoalesceState
{
    std::mutex mutex;
    std::map<std::string, std::function<void()>> pending;
    bool scheduled {false};
};

// ---- deferred argument captures -------------------------------------------
// A deferred capture must outlive the emitting stack frame. Value arguments are
// copied. Document/DocumentObject/Property references are replaced by stable
// identity handles and resolved again on the main thread, which makes deferred
// delivery safe even if the object is destroyed before the callback runs.

struct DeferredDocumentArg
{
    MainThreadSignalConfig::DocumentHandle handle;
    const Document* ptr {nullptr};

    bool resolve()
    {
        ptr = MainThreadSignalConfig::resolveDocument(handle);
        return ptr != nullptr;
    }
    const Document& get() const
    {
        return *ptr;
    }
    void appendKey(std::string& out) const
    {
        out += handle.name;
        out.push_back('\0');
    }
};

struct DeferredObjectArg
{
    MainThreadSignalConfig::ObjectHandle handle;
    const DocumentObject* ptr {nullptr};

    bool resolve()
    {
        ptr = MainThreadSignalConfig::resolveObject(handle);
        return ptr != nullptr;
    }
    const DocumentObject& get() const
    {
        return *ptr;
    }
    void appendKey(std::string& out) const
    {
        out += handle.document;
        out.push_back('\0');
        out += handle.object;
        out.push_back('\0');
    }
};

struct DeferredPropertyArg
{
    MainThreadSignalConfig::PropertyHandle handle;
    const Property* ptr {nullptr};

    bool resolve()
    {
        ptr = MainThreadSignalConfig::resolveProperty(handle);
        return ptr != nullptr;
    }
    const Property& get() const
    {
        return *ptr;
    }
    void appendKey(std::string& out) const
    {
        out += handle.document;
        out.push_back('\0');
        out += handle.object;
        out.push_back('\0');
        out += handle.property;
        out.push_back('\0');
    }
};

template<class T>
struct DeferredValueArg
{
    T value;
    bool resolve() const
    {
        return true;
    }
    T& get()
    {
        return value;
    }
    const T& get() const
    {
        return value;
    }
    void appendKey(std::string& out) const
    {
        if constexpr (std::is_same_v<T, std::string>) {
            out += value;
        }
        else {
            static_assert(
                always_false<T>::value,
                "this value type cannot be used with DeliveryPolicy::Coalesced"
            );
        }
        out.push_back('\0');
    }
};

// Choose the deferred capture from the *declared* parameter type. Using the
// declared type (not fastsignals::signal_arg_t) preserves whether the parameter
// was written by value or by reference.
template<class Decl, class T>
auto makeDeferredArg(T&& x)
{
    using D = std::decay_t<Decl>;
    if constexpr (std::is_same_v<D, Document>) {
        return DeferredDocumentArg {MainThreadSignalConfig::encodeDocument(std::addressof(x))};
    }
    else if constexpr (std::is_same_v<D, DocumentObject>) {
        return DeferredObjectArg {MainThreadSignalConfig::encodeObject(std::addressof(x))};
    }
    else if constexpr (std::is_same_v<D, Property>) {
        return DeferredPropertyArg {MainThreadSignalConfig::encodeProperty(std::addressof(x))};
    }
    else if constexpr (!std::is_reference_v<Decl>) {
        return DeferredValueArg<D> {std::forward<T>(x)};
    }
    else {
        static_assert(
            always_false<D>::value,
            "this argument type cannot be deferred to the main thread; "
            "keep the signal on DeliveryPolicy::Blocking"
        );
    }
}

template<class Tuple>
bool resolveAll(Tuple& caps)
{
    return std::apply([](auto&... c) { return (c.resolve() && ...); }, caps);
}

template<class Tuple>
std::string coalesceKey(const Tuple& caps)
{
    std::string key;
    std::apply([&key](const auto&... c) { (c.appendKey(key), ...); }, caps);
    return key;
}
}  // namespace detail

// Wrapper that mirrors fastsignals::signal but executes slots on GUI thread.
template<
    class Signature,
    DeliveryPolicy Policy = DeliveryPolicy::Blocking,
    template<class T> class Combiner = ::fastsignals::optional_last_value>
class MainThreadSignal;

template<class Return, class... Arguments, DeliveryPolicy Policy, template<class T> class Combiner>
class MainThreadSignal<Return(Arguments...), Policy, Combiner>
{
    using base_sig = ::fastsignals::signal<Return(Arguments...), Combiner>;

public:
    using signature_type = typename base_sig::signature_type;
    using slot_type = typename base_sig::slot_type;
    using combiner_type = typename base_sig::combiner_type;
    using result_type = typename base_sig::result_type;

    MainThreadSignal()
    {
        liveness_ = std::make_shared<detail::DeferredLiveness>();
        if constexpr (Policy == DeliveryPolicy::Coalesced) {
            coalesce_ = std::make_shared<detail::CoalesceState>();
        }
    }

    MainThreadSignal(const MainThreadSignal&) = delete;
    MainThreadSignal& operator=(const MainThreadSignal&) = delete;

    MainThreadSignal(MainThreadSignal&& other)
        : sig_(std::move(other.sig_))
        , liveness_(std::move(other.liveness_))
        , coalesce_(std::move(other.coalesce_))
    {
        other.liveness_ = nullptr;
    }

    MainThreadSignal& operator=(MainThreadSignal&& other)
    {
        if (this != &other) {
            if (liveness_) {
                liveness_->alive.store(false, std::memory_order_release);
            }
            sig_ = std::move(other.sig_);
            liveness_ = std::move(other.liveness_);
            coalesce_ = std::move(other.coalesce_);
            other.liveness_ = nullptr;
        }
        return *this;
    }

    ~MainThreadSignal()
    {
        if (liveness_) {
            liveness_->alive.store(false, std::memory_order_release);
        }
    }

    // connections
    ::fastsignals::connection connect(slot_type slot)
    {
        return sig_.connect(std::move(slot));
    }
    ::fastsignals::advanced_connection connect(slot_type slot, ::fastsignals::advanced_tag tag)
    {
        return sig_.connect(std::move(slot), tag);
    }

    void disconnect_all_slots() noexcept
    {
        sig_.disconnect_all_slots();
    }
    std::size_t num_slots() const noexcept
    {
        return sig_.num_slots();
    }
    bool empty() const noexcept
    {
        return sig_.empty();
    }

    // ---- emission APIs ------------------------------------------------------

    result_type emit(typename ::fastsignals::signal_arg_t<Arguments>... args)
    {
        return emitImpl(this, std::forward<typename ::fastsignals::signal_arg_t<Arguments>>(args)...);
    }

    result_type emit(typename ::fastsignals::signal_arg_t<Arguments>... args) const
    {
        return emitImpl(this, std::forward<typename ::fastsignals::signal_arg_t<Arguments>>(args)...);
    }

    result_type operator()(typename ::fastsignals::signal_arg_t<Arguments>... args)
    {
        return emit(std::forward<typename ::fastsignals::signal_arg_t<Arguments>>(args)...);
    }

    result_type operator()(typename ::fastsignals::signal_arg_t<Arguments>... args) const
    {
        return emit(std::forward<typename ::fastsignals::signal_arg_t<Arguments>>(args)...);
    }

    // Plug a MainThreadSignal into a plain fastsignal as a slot:
    operator slot_type() const noexcept
    {
        return [this](typename ::fastsignals::signal_arg_t<Arguments>... args) {
            emit(std::forward<typename ::fastsignals::signal_arg_t<Arguments>>(args)...);
            if constexpr (!std::is_void_v<Return>) {
                return Return();
            }
        };
    }

    // escape hatch
    base_sig& underlying()
    {
        return sig_;
    }
    const base_sig& underlying() const
    {
        return sig_;
    }

private:
    template<class Self>
    static result_type emitImpl(Self* self, typename ::fastsignals::signal_arg_t<Arguments>... args)
    {
        if (MainThreadSignalConfig::isMainThread()) {
            return self->sig_(std::forward<typename ::fastsignals::signal_arg_t<Arguments>>(args)...);
        }

        if constexpr (Policy == DeliveryPolicy::Blocking) {
            return emitBlocking(
                self,
                std::forward<typename ::fastsignals::signal_arg_t<Arguments>>(args)...
            );
        }
        else {
            static_assert(
                std::is_void_v<result_type>,
                "DeliveryPolicy::Queued/Coalesced requires a void signal"
            );
            emitDeferred(self, std::forward<typename ::fastsignals::signal_arg_t<Arguments>>(args)...);
        }
    }

    template<class Self>
    static result_type emitBlocking(Self* self, typename ::fastsignals::signal_arg_t<Arguments>... args)
    {
        Base::PyGILStateRelease release;

        auto caps = std::make_tuple(
            detail::captureSignalArg<typename ::fastsignals::signal_arg_t<Arguments>>(args)...
        );

        if constexpr (std::is_void_v<result_type>) {
            MainThreadSignalConfig::invoke(
                [self, caps = std::move(caps)]() mutable {
                    std::apply([self](auto&... c) { self->sig_(c.get()...); }, caps);
                },
                /*blocking=*/true
            );
        }
        else {
            std::optional<detail::non_void_t<result_type>> result;
            MainThreadSignalConfig::invoke(
                [self, caps = std::move(caps), &result]() mutable {
                    result.emplace(
                        std::apply([self](auto&... c) { return self->sig_(c.get()...); }, caps)
                    );
                },
                /*blocking=*/true
            );
            return std::move(*result);
        }
    }

    template<class Self>
    static void emitDeferred(Self* self, typename ::fastsignals::signal_arg_t<Arguments>... args)
    {
        // No GIL release here: unlike the blocking path, nothing waits on the
        // main thread, and the capture/schedule below is pure C++.
        auto caps = std::make_tuple(detail::makeDeferredArg<Arguments>(args)...);

        auto liveness = self->liveness_;

        if constexpr (Policy == DeliveryPolicy::Coalesced) {
            // Build the key first so a duplicate key can be dropped before
            // allocating a new callback.
            auto key = detail::coalesceKey(caps);
            if (self->coalesceHas(key)) {
                return;
            }
            self->scheduleCoalesced(makeDeferredTask(self, liveness, std::move(caps)), std::move(key));
        }
        else {
            MainThreadSignalConfig::invoke(
                makeDeferredTask(self, liveness, std::move(caps)),
                /*blocking=*/false
            );
        }
    }

    template<class Self, class Tuple>
    static std::function<void()> makeDeferredTask(
        Self* self,
        std::shared_ptr<detail::DeferredLiveness> liveness,
        Tuple caps
    )
    {
        return std::function<void()>([self, liveness, caps = std::move(caps)]() mutable {
            if (!liveness || !liveness->alive.load(std::memory_order_acquire)) {
                return;
            }
            if (!detail::resolveAll(caps)) {
                return;
            }
            std::apply([self](auto&... c) { self->sig_(c.get()...); }, caps);
        });
    }

    bool coalesceHas(const std::string& key) const
    {
        if (!coalesce_) {
            return false;
        }
        std::lock_guard<std::mutex> lock(coalesce_->mutex);
        return coalesce_->pending.find(key) != coalesce_->pending.end();
    }

    void scheduleCoalesced(std::function<void()> task, std::string key) const
    {
        if (!coalesce_) {
            return;
        }

        bool schedule = false;
        {
            std::lock_guard<std::mutex> lock(coalesce_->mutex);
            if (coalesce_->pending.find(key) != coalesce_->pending.end()) {
                return;  // an equivalent emission is already pending
            }
            coalesce_->pending.emplace(std::move(key), std::move(task));
            if (!coalesce_->scheduled) {
                coalesce_->scheduled = true;
                schedule = true;
            }
        }

        if (!schedule) {
            return;
        }

        auto liveness = liveness_;
        auto state = coalesce_;
        MainThreadSignalConfig::invoke(
            [liveness, state]() {
                if (!liveness || !liveness->alive.load(std::memory_order_acquire)) {
                    return;
                }
                std::map<std::string, std::function<void()>> pending;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    pending.swap(state->pending);
                    state->scheduled = false;
                }
                for (auto& entry : pending) {
                    entry.second();
                }
            },
            /*blocking=*/false
        );
    }

    mutable base_sig sig_;
    mutable std::shared_ptr<detail::DeferredLiveness> liveness_;
    mutable std::shared_ptr<detail::CoalesceState> coalesce_;
};

}  // namespace App

#endif  // APP_MAINTHREADSIGNAL_H
