// logos-view-generator — the plugin glue for a Logos VIEW module.
//
// A view module (metadata `type: ui_qml`, `interface: universal`) is authored
// as two user-written pieces: the `.rep` that declares the view contract (full
// QtRO: SLOTs, PROPs, SIGNALs) and a `*Backend` class deriving the repc
// `<RepClass>SimpleSource` plus `LogosUiPluginContext`. This tool emits the
// third piece — the Qt plugin around them:
//
//   <name>_ui_interface.h   PluginInterface + the IID
//   <name>_ui_glue.{h,cpp}  the *Plugin: Q_PLUGIN_METADATA, name()/version(),
//                           ViewPluginBase, and the Q_INVOKABLE initLogos that
//                           builds LogosModules, wires it into the backend
//                           (firing onContextReady) and registers the backend
//                           as the QtRO source.
//
// It deliberately does NOT read LIDL. A view's contract is its `.rep`, not a
// `.lidl` — the class name is scraped out of the .rep and everything else comes
// from metadata.json — so this binary needs Qt Core and nothing else. That is
// what keeps this repo a leaf.
//
// Usage:
//   logos-view-generator [--backend ui] --metadata <metadata.json> --rep <view.rep>
//                        [--backend-class <C>] [--backend-header <h>]
//                        [--output-dir <dir>]
//
// `--backend ui` is the only backend this binary has, and it is the default, so
// it may be omitted. It is ACCEPTED rather than ignored because the caller that
// matters -- logos-module-builder's ui_qml codegen step -- spells it explicitly,
// having previously invoked `logos-qt-generator --backend ui`. An unrecognised
// value is REFUSED: silently treating `--backend qml-dep` as ui would emit the
// wrong artifact set under a green build, which is the failure mode that put
// this generator in two repos in the first place.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include "lidl_gen_ui.h"

namespace {

struct Out { QString file; QString content; };

int writeAll(const QList<Out>& outs, const QString& dir,
             QTextStream& out, QTextStream& err)
{
    for (const Out& o : outs) {
        const QString abs = QDir(dir).filePath(o.file);
        QFile f(abs);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            err << "Failed to write: " << abs << "\n";
            return 1;
        }
        f.write(o.content.toUtf8());
        f.close();
        out << "Generated: " << abs << "\n";
    }
    return 0;
}

QString argValue(const QStringList& args, const QString& flag)
{
    const int i = args.indexOf(flag);
    if (i < 0 || i + 1 >= args.size()) return QString();
    return args.at(i + 1);
}

// PascalCase of a module name, for the plugin class stem. Same rule the other
// Logos generators use; inlined here rather than shared so this binary keeps
// no dependency on a generator frontend it otherwise has no use for.
QString toPascalCase(const QString& name)
{
    QString out;
    bool cap = true;
    for (QChar c : name) {
        if (!c.isLetterOrNumber()) { cap = true; continue; }
        if (cap) { out.append(c.toUpper()); cap = false; }
        else { out.append(c.toLower()); }
    }
    if (out.isEmpty()) return QString("Module");
    return out;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    const QStringList args = QCoreApplication::arguments();

    // Backend selection. Defaults to the only backend there is; anything else
    // is an error rather than a silent fallback. See the usage note above.
    const QString backend = argValue(args, "--backend");
    const bool asModule = backend == QStringLiteral("ui-module");
    if (!backend.isEmpty() && backend != QStringLiteral("ui") && !asModule) {
        err << "Unknown --backend: " << backend << " (expected ui or ui-module)\n";
        return 2;
    }
    if (!asModule) {
        err << "logos-view-generator: DEPRECATED: --backend ui emits a ui-host plugin. Run the "
               "backend as a core module instead (--backend ui-module, \"backend\": \"module\" "
               "in metadata.json); ui-host is removed in the next breaking release.\n";
        err.flush();
    }

    const QString metadata = argValue(args, "--metadata");
    const QString repPath  = argValue(args, "--rep");
    QString outputDir      = argValue(args, "--output-dir");

    if (metadata.isEmpty() || repPath.isEmpty()) {
        err << "Usage: logos-view-generator [--backend ui|ui-module] --metadata <metadata.json> --rep <view.rep>\n"
            << "         [--backend-class <C>] [--backend-header <h>] [--output-dir <dir>]\n";
        return 2;
    }

    QFile mf(metadata);
    if (!mf.open(QIODevice::ReadOnly)) {
        err << "Failed to read metadata: " << metadata << "\n";
        return 3;
    }
    const QJsonObject meta = QJsonDocument::fromJson(mf.readAll()).object();

    UiGlueSpec spec;
    spec.moduleName = meta.value(QStringLiteral("name")).toString();
    spec.moduleVersion = meta.value(QStringLiteral("version")).toString(QStringLiteral("1.0.0"));
    if (spec.moduleName.isEmpty()) {
        err << "metadata.json has no name\n";
        return 3;
    }
    spec.pluginBase = toPascalCase(spec.moduleName);
    // ui-module: metadata.json is the backend module's (`<view>_backend`); the
    // view it serves names the models and the user's Backend class/header.
    spec.viewName = meta.value(QStringLiteral("view_module")).toString(spec.moduleName);
    const QString viewBase = toPascalCase(spec.viewName);

    QString repErr;
    if (!lidlUiParseRepClass(repPath, &spec.repClass, &repErr)) {
        err << "Error: " << repErr << "\n";
        return 4;
    }

    spec.backendClass = argValue(args, "--backend-class");
    if (spec.backendClass.isEmpty())
        spec.backendClass = viewBase + QStringLiteral("Backend");
    spec.backendHeader = argValue(args, "--backend-header");
    if (spec.backendHeader.isEmpty())
        spec.backendHeader = spec.viewName + QStringLiteral("_backend.h");

    if (outputDir.isEmpty())
        outputDir = QDir::current().filePath("generated");
    QDir().mkpath(outputDir);

    QList<Out> outs;
    if (asModule) {
        outs.append({spec.moduleName + "_ui_module_glue.h",   lidlMakeUiModuleGlueHeader(spec)});
        outs.append({spec.moduleName + "_ui_module_glue.cpp", lidlMakeUiModuleGlueSource(spec)});
        const int rc = writeAll(outs, outputDir, out, err);
        out.flush();
        return rc;
    }
    outs.append({spec.moduleName + "_ui_interface.h", lidlMakeUiInterfaceHeader(spec)});
    outs.append({spec.moduleName + "_ui_glue.h",      lidlMakeUiGlueHeader(spec)});
    outs.append({spec.moduleName + "_ui_glue.cpp",    lidlMakeUiGlueSource(spec)});

    const int rc = writeAll(outs, outputDir, out, err);
    out.flush();
    return rc;
}
