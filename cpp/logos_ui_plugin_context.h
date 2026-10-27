#ifndef LOGOS_UI_PLUGIN_CONTEXT_H
#define LOGOS_UI_PLUGIN_CONTEXT_H

#include <functional>
#include <type_traits>

// Deprecated with ui-host: a backend that runs as a core module ("backend":
// "module") derives LogosModuleContext instead. Removed in the next breaking
// release; LOGOS_UI_HOST_ALLOWED silences the warning until then.
#if defined(LOGOS_UI_HOST_ALLOWED)
#define LOGOS_UI_PLUGIN_CONTEXT_DEPRECATED
#else
#define LOGOS_UI_PLUGIN_CONTEXT_DEPRECATED                                            \
    [[deprecated("LogosUiPluginContext (ui-host) is deprecated: add \"backend\": \"module\" "\
                 "to metadata.json and derive LogosModuleContext")]]
#endif

// ---------------------------------------------------------------------------
// LogosUiPluginContext — opt-in mixin for codegen-generated UI plugins
//
// A UI plugin (type: ui_qml, interface: universal) is NOT a module: it has no
// host-provisioned identity (no modulePath / instanceId / instancePersistencePath),
// it emits no events of its own, and it exposes no methods to other modules — it
// is a *view* whose backend derives a QtRO `<RepClass>SimpleSource` and pushes
// state to the QML replica through that. The one thing it legitimately needs from
// the framework is typed access to the modules/interfaces it declares as
// `dependencies`. This mixin confines the context to exactly that: `modules()`
// plus the `onContextReady()` lifecycle hook.
//
// This is the Qt-layer counterpart to logos-cpp-sdk's `LogosModuleContext`
// (which serves real, std-typed core/cdylib modules). A UI plugin lives entirely
// in Qt-land — its `.rep` slots are Qt-typed — so its `modules().<dep>` wrappers
// are generated Qt-typed too (api-style `qt`); no std<->Qt conversions at the
// view boundary.
//
// Usage from a UI backend:
//
//     class MyBackend : public MyRepSimpleSource,
//                       public LogosUiPluginContext {
//     public:
//         // ... .rep SLOT overrides ...
//     protected:
//         void onContextReady() override {
//             // modules() is now live — make typed calls or subscribe to a
//             // dependency's typed events here.
//             modules().some_dep.onSomething([this](int v) { /* ... */ });
//         }
//     };
//
// Backends that don't inherit from LogosUiPluginContext are unaffected — the
// generated glue's `maybeSetUiPluginModules` tag-dispatches to a no-op.
// ---------------------------------------------------------------------------

// Per-module aggregate of dependency wrappers. Each module's codegen emits
// `struct LogosModules { ... };` at global scope in its own
// `generated_code/logos_sdk.h` (one accessor per `metadata.json#dependencies`
// entry). Forward-declared here so this header stays decoupled from per-module
// codegen — the backend's translation unit makes the type complete via its own
// `#include "logos_sdk.h"`, at which point the inline `modules()` body compiles.
struct LogosModules;

// How a view answers aboutToUnload(). Deliberately mirrors logos-cpp-sdk's
// `LogosShutdown` (same name, same order, same meaning) so a Logos author meets
// ONE teardown vocabulary whether they are writing a module or a view.
//
// It is redeclared here rather than included because this repo's UI layer takes
// no dependency on logos-cpp-sdk's module headers -- and it does not have to:
// a UI plugin is a view, not a module, so `logos_module_context.h` and this
// header are never in one translation unit. If that ever stops being true the
// compiler says so at the redefinition, which is the loud failure, not a silent
// one.
//
// Synchronous  — the view is already quiescent; the host may tear it down as
//                soon as the call returns.
// Asynchronous — the view has work to finish first. The host waits, up to a
//                bounded grace period, until the view calls unloadFinished().
//                A grace period is not a veto: the host proceeds either way.
enum class LogosShutdown {
    Synchronous,
    Asynchronous,
};

class LogosUiPluginContext {
public:
    // On the constructor, not the class: GCC does not warn for a deprecated
    // base class, but every backend that derives this one calls it.
    LOGOS_UI_PLUGIN_CONTEXT_DEPRECATED LogosUiPluginContext() = default;
    virtual ~LogosUiPluginContext() = default;

    // Typed access to this plugin's per-build `LogosModules` aggregate, which
    // the codegen emits in `generated_code/logos_sdk.h`. It owns one
    // strongly-typed (Qt-typed) client wrapper per entry in `metadata.json`'s
    // `dependencies` list — nothing else. The backend can call those declared
    // deps' methods, and subscribe to their typed events, without ever touching
    // the raw `LogosAPI`:
    //
    //     #include "logos_sdk.h"           // generated at build time
    //
    //     QString MyBackend::libVersion() {
    //         return modules().some_dep.version();
    //     }
    //
    // The pointer is set by the codegen-generated UI plugin's `initLogos`, which
    // constructs the `LogosModules` from the `LogosAPI`. Calling before that
    // (e.g. from a unit test bypassing the generated glue) is undefined.
    LogosModules& modules() const {
        return *static_cast<LogosModules*>(m_logosModulesPtr);
    }

    // True once the framework has wired `modules()`. Stays false when the
    // backend is constructed outside the generated glue (e.g. unit tests),
    // matching the null-pointer fallback above. Read this before using
    // `modules()` from helpers that may run earlier in the backend's life.
    bool isContextReady() const { return m_logosModulesPtr != nullptr; }

    // Framework-only entry point — invoked by the generated UI plugin's
    // `initLogos`. Sets the typed-deps pointer and fires `onContextReady()`
    // (by which point `modules()` is live). The leading/trailing underscores
    // signal "do not call from user code".
    void _logosCoreSetLogosModulesPtr_(void* ptr) {
        m_logosModulesPtr = ptr;
        onContextReady();
    }

    // Framework-only — installs the trampoline `unloadFinished()` fires down.
    // The generated plugin sets this to a queued emission of its own
    // `unloadFinished()` signal, which is what ui-host is waiting on. It stays
    // empty outside a framework context, which is what makes unloadFinished()
    // a no-op there rather than a crash.
    void _logosCoreSetUnloadFinished_(std::function<void()> cb) {
        m_unloadFinishedCallback = std::move(cb);
    }

    // Framework-only — drives the hook. Named apart from aboutToUnload() so the
    // protected override stays the only thing an author sees, and so the plugin
    // has an entry point without making the hook itself public.
    LogosShutdown _logosCoreAboutToUnload_() { return aboutToUnload(); }

protected:
    // Hook for derived backends. Fires exactly once, after `modules()` becomes
    // usable, before the view's first call. The default is a no-op; override to
    // make typed dependency calls or arm typed event subscriptions. Do NOT do
    // this work in the constructor — it runs before the framework hands the
    // dependencies over.
    virtual void onContextReady() {}

    // Hook for derived backends, fired when the host is about to tear this view
    // down — after the view's event loop has returned, before the plugin is
    // destroyed. The default answers Synchronous, so a view that does not
    // override this costs its teardown nothing at all.
    //
    // Override and return Asynchronous to buy a bounded grace period for work
    // that cannot finish inline (flushing a draft, closing a session). Having
    // done so, you MUST call unloadFinished() when the work completes:
    // forgetting costs every teardown of this view the full grace period.
    // Returning Synchronous while work is still in flight is the other bug, and
    // the quieter one.
    //
    // NOT part of the view's .rep contract: this is framework plumbing, so it
    // is never remoted and no QML caller can reach it.
    virtual LogosShutdown aboutToUnload() { return LogosShutdown::Synchronous; }

    // Signal that the Asynchronous teardown begun in aboutToUnload() has
    // finished. Safe from any thread — the generated plugin's trampoline
    // marshals the emission back to the plugin's thread — and safe to call when
    // the host is not listening (outside a framework context, or after the
    // grace period elapsed): a no-op then rather than an error, so a view needs
    // no special case for being torn down under a deadline it missed.
    //
    // Calling it more than once is harmless; the host acts on the first.
    void unloadFinished() const {
        if (m_unloadFinishedCallback)
            m_unloadFinishedCallback();
    }

private:
    // Type-erased so this header doesn't need the per-module LogosModules
    // definition. Reinterpreted via the typed `modules()` accessor above.
    // Stays null when the backend is constructed outside the generated glue.
    void* m_logosModulesPtr = nullptr;
    // Installed by the generated plugin; see _logosCoreSetUnloadFinished_.
    // Stays empty for a backend constructed outside the generated glue, which
    // is what makes unloadFinished() a no-op in unit tests.
    std::function<void()> m_unloadFinishedCallback;
};

// ---------------------------------------------------------------------------
// _logos_codegen_::maybeSetUiPluginModules — codegen helper, do not call
// directly. The generated UI plugin's `initLogos` always wants to "wire the
// LogosModules aggregate if the backend inherits LogosUiPluginContext,
// otherwise do nothing." Tag-dispatching through two function templates makes
// the unused branch invisible to the compiler for non-inheriting backends
// (the discarded `static_cast` is never type-checked). Mirrors the
// `maybeSet*` helpers in logos-cpp-sdk's `logos_module_context.h`.
// ---------------------------------------------------------------------------
// The helpers below name the class only to dispatch on it; the warning is for
// the backend author's own `: public LogosUiPluginContext`, not for these.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
namespace _logos_codegen_ {

template<class T>
inline auto maybeSetUiPluginModules(T& backend, void* ptr)
    -> std::enable_if_t<std::is_base_of_v<LogosUiPluginContext, T>>
{
    static_cast<LogosUiPluginContext&>(backend)._logosCoreSetLogosModulesPtr_(ptr);
}

template<class T>
inline auto maybeSetUiPluginModules(T&, void*)
    -> std::enable_if_t<!std::is_base_of_v<LogosUiPluginContext, T>>
{
    // Backend didn't opt into LogosUiPluginContext; nothing to do.
}

// Same tag dispatch for the teardown hook. Returns the LogosShutdown as an int
// because the generated plugin hands it straight back to a host that resolves
// this by signature string and must not need this enum to do it.
//
// `cb` is installed BEFORE the backend is asked, so a backend that finishes its
// work inline still finds a live trampoline; installing it afterwards would
// drop that completion and cost the view its full grace period.
template<class T, class Cb>
inline auto maybeUiPluginAboutToUnload(T& backend, Cb cb)
    -> std::enable_if_t<std::is_base_of_v<LogosUiPluginContext, T>, int>
{
    auto& ctx = static_cast<LogosUiPluginContext&>(backend);
    ctx._logosCoreSetUnloadFinished_(std::move(cb));
    return static_cast<int>(ctx._logosCoreAboutToUnload_());
}

template<class T, class Cb>
inline auto maybeUiPluginAboutToUnload(T&, Cb)
    -> std::enable_if_t<!std::is_base_of_v<LogosUiPluginContext, T>, int>
{
    // Backend didn't opt into LogosUiPluginContext, so it has nothing to
    // finish: Synchronous.
    return 0;
}

} // namespace _logos_codegen_
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif // LOGOS_UI_PLUGIN_CONTEXT_H
