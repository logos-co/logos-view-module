# logos-view-module

The **view-module backend**: everything needed to build a Logos *view* module
(`metadata.json` `type: ui_qml`, `interface: universal`), as a standalone repo
that `logos-module-builder` delegates to.

A view module is a different authoring flavour from a core module, not a
variation on one. A core module publishes a callable contract (`.lidl`) and is
reached over the protocol; a view module publishes a **`.rep`** — a QtRO
interface with slots, properties and signals — and is driven by a QML view. The
backend behind that view runs either as an ordinary core module
(`"backend": "module"`, `--backend ui-module`) or, deprecated, in a `ui-host`
process (`--backend ui`). This repo owns that flavour end to end, so changing
how views are built is a one-repo change.

It is deliberately a **leaf**: it depends on `logos-nix` for a pinned Qt and on
nothing else. Neither half needs to read a LIDL contract.

## What's in it

| Piece | What it does |
|---|---|
| `view-generator/` | `logos-view-generator` — emits the Qt plugin around a view's user-written `.rep` + `*Backend`: `<name>_ui_interface.h` and `<name>_ui_glue.{h,cpp}`. Qt Core only. This is the emitter `logos-module-builder` runs for every `type: ui_qml` module; it used to live in logos-qt-sdk as well, and that copy is gone. Two modes: `--backend ui-module` emits `<name>_ui_module_glue.{h,cpp}`, a core-module plugin around the `*Backend` whose only callable method is `view_endpoint()`; `--backend ui` (deprecated) emits the `ui-host` plugin. |
| `cpp/logos_view_channel.h` | `LogosViewChannel` — header-only; the private QtRO channel a `--backend ui-module` plugin publishes its `.rep` source and model properties on. Listens on a short `lv_<16 hex>` local socket (macOS caps socket paths at 104 bytes) and admits a connection only after it sends a one-time secret (valid 60 s) that `view_endpoint()` handed out. On start-up it removes `lv_*` sockets left by killed processes (older than a minute, nobody listening). |
| `cmake/LogosViewModule.cmake` | `logos_replica_factory()` — builds the typed QtRO replica factory a QML view loads, plus the per-module `LogosViewPlugin` base that lets `ui-host` drive the plugin through a plain `qobject_cast` instead of `QMetaObject` reflection. |
| `cmake/LogosView*.in` | The four templates that function configures — and the only copy of them anywhere. **Siblings of the `.cmake` by requirement** — it resolves them through `CMAKE_CURRENT_FUNCTION_LIST_DIR`. Also published flat as `packages.<sys>.logos-view-templates`, which is the shape `LOGOS_VIEW_TEMPLATE_DIR` wants; `logos-module-builder` reads that output for every `ui_qml` module and for its `view-interface-abi` check. See `cmake/README.md`. |
| `cpp/logos_ui_plugin_context.h` | `LogosUiPluginContext` — the narrow context a view's `*Backend` derives alongside its repc `SimpleSource`. It supplies `onContextReady()` and the typed `modules()` accessors to declared dependencies, and nothing else: a view is a view, not a module, so it gets no `modulePath`, no `instanceId`, no persistence, and no events of its own. It also carries the teardown hook (`aboutToUnload()` / `unloadFinished()`). Published as `packages.<sys>.include`; `logos-module-builder` puts it on the include path ahead of logos-qt-sdk's older copy. |

## One pin, one pair

`view-generator/lidl_gen_ui.cpp` and `cpp/logos_ui_plugin_context.h` are a
**matched pair**, and they live in one repo for that reason. The emitted glue
calls `_logos_codegen_::maybeUiPluginAboutToUnload(...)`; only that header
declares it. Ship them from two independently-pinned repos and every `ui_qml`
build silently depends on those two pins agreeing.

That is not hypothetical. Both files previously lived in logos-qt-sdk *and* a
copy of each lived here. logos-qt-sdk#38 added the module teardown hook to its
pair; the pair here never got it, and nothing failed — because a generated view
plugin missing the hook still builds, still loads and still runs. `ui-host`
reaches `aboutToUnload()` **by name** through the meta-object, so a plugin class
that does not declare it simply has no such meta-method:
`QMetaObject::invokeMethod` returns `false` and the host moves on, exactly as it
would for a view that answered "Synchronous, nothing to wait for". Every view
would have lost its chance to finish, permanently and quietly.

The `ui-plugin-metaobject` check exists to make that loud: it runs the
generator, compiles the plugin it emitted, loads it with `QPluginLoader` and
drives the teardown handshake through the meta-object the way the host does.

## The authoring split

As a core module (`"backend": "module"`; the builder names the module
`<name>_backend` and passes the view's name as `view_module`):

```
USER WRITES     src/<X>.rep            the view contract (SLOTs, PROPs, SIGNALs)
                src/<X>Backend.{h,cpp} deriving <RepClass>SimpleSource
                                       + LogosModuleContext

GENERATED       <name>_ui_module_glue.{h,cpp}
                                       the core-module plugin: owns the backend,
                                       wires modules() / the data folder / the
                                       aboutToUnload hook through the SDK's
                                       maybeSet* helpers, publishes the backend
                                       on a LogosViewChannel, and hands logos
                                       core a LogosProviderPlugin whose only
                                       method is view_endpoint()
```

In `ui-host` (deprecated, removed in the next breaking release):

```
USER WRITES     src/<X>.rep            the view contract (SLOTs, PROPs, SIGNALs)
                src/<X>Backend.{h,cpp} deriving <RepClass>SimpleSource
                                       + LogosUiPluginContext

GENERATED       <name>_ui_interface.h  PluginInterface + IID
                <name>_ui_glue.{h,cpp} the *Plugin: Q_PLUGIN_METADATA,
                                       name()/version(), ViewPluginBase, and the
                                       Q_INVOKABLE initLogos that builds
                                       LogosModules, wires it into the backend
                                       (firing onContextReady) and registers the
                                       backend as the QtRO source
                <name>_replica_factory the replica side of the same .rep, for QML
```

Declaring one in `metadata.json`:

```json
{
  "name": "chat_ui",
  "type": "ui_qml",
  "interface": "universal",
  "backend": "module",
  "codegen": {
    "rep": "src/ChatBackend.rep",
    "backend_class": "ChatBackend",
    "backend_header": "ChatBackend.h"
  },
  "dependencies": ["chat_module", "delivery_module"]
}
```

## Using it directly

```bash
logos-view-generator [--backend ui|ui-module] \
    --metadata metadata.json --rep src/ChatBackend.rep \
    [--backend-class ChatBackend] [--backend-header ChatBackend.h] \
    [--output-dir generated_code]
```

```cmake
include("${LOGOS_VIEW_MODULE_ROOT}/cmake/LogosViewModule.cmake")
logos_replica_factory(NAME chat_ui REP_FILE src/ChatBackend.rep)
```

## Tests

```bash
nix build .#checks.<system>.view-generator
nix build .#checks.<system>.view-generator-rejects-bad-rep
nix build .#checks.<system>.rep-file-plugin
nix build .#checks.<system>.view-channel
```

`view-channel` builds a `LogosViewChannel` and checks that it listens under a
macOS-length `$TMPDIR`, and that a connection with no secret, a wrong one or a
reused one is refused. The surface of a `--backend ui-module` plugin (only
`view_endpoint()` callable) is checked in `logos-module-builder`'s
`qml-module-backend` check, which builds a real one against the SDK.

The first drives the generator over a real `.rep` + `metadata.json` and asserts
the scraped rep class, the PascalCase plugin stem and the carried version. The
second asserts a `.rep` declaring no class **fails** rather than emitting half a
plugin — the failure mode that is otherwise silent.

The third (`tests/rep-file-plugin`) is the binary side of the plugin ABI in
`cmake/LogosView*.in`. It runs `repc` over a real `.rep`, instantiates the
factory templates through `configure_file` and compiles the result, then
asserts:

* the built plugin carries **exactly** `logos.view.replica_factory/1.0` and no
  other replica-factory IID — an exact-set match, because the substring match
  it replaced passed happily when `Q_PLUGIN_METADATA` was bumped to `/2.0`;
* `QPluginLoader::instance()` returns non-null and
  `qobject_cast<LogosViewReplicaFactory*>` succeeds — the `Q_INTERFACES`
  binding, which leaves every string in the binary untouched when deleted and
  only shows up as a blank view;
* `replicaMetaObject()` is wired to the `repc`-generated replica.

It is handed `cmake/` itself as `LOGOS_VIEW_TEMPLATE_DIR`, so it compiles the
same files a real `ui_qml` module does rather than a private duplicate, and it
does the load exactly as `logos-view-module-runtime`'s
`LogosQmlBridge::loadFactory` did (removed: hosts use dynamic replicas now, so
no current host loads a factory; the factory path is deprecated with ui-host). That complements `logos-module-builder`'s
`view-interface-abi` check, which compares the module and host *declarations*
as text and never builds either.
