// UI-backend glue generation: type=ui_qml + interface=universal modules.
//
// USER-WRITTEN: the .rep (the view contract) and the *Backend.{h,cpp}
// deriving <RepClass>SimpleSource + LogosUiPluginContext (this repo's
// logos_ui_plugin_context.h — NOT logos-cpp-sdk's LogosModuleContext, which is
// the std-typed context a core/cdylib module derives).
// GENERATED: <name>_ui_interface.h (PluginInterface + IID) and
// <name>_ui_glue.{h,cpp} (the *Plugin: Q_PLUGIN_METADATA + initLogos wiring
// LogosModules/context into the backend + setBackend).
#pragma once

#include <QString>

struct UiGlueSpec {
    QString moduleName;     // metadata.json name (e.g. ticker_panel)
    QString moduleVersion;  // metadata.json version
    QString pluginBase;     // PascalCase of moduleName (class name stem)
    QString repClass;       // class declared in the user's .rep
    QString backendClass;   // user's backend class (default <pluginBase>Backend)
    QString backendHeader;  // include name (default <name>_backend.h)
    QString viewName;       // ui-module: the view's module name (models are "<viewName>/<prop>")
};

bool lidlUiParseRepClass(const QString& repPath, QString* repClass, QString* whyNot);
QString lidlMakeUiInterfaceHeader(const UiGlueSpec& spec);
QString lidlMakeUiGlueHeader(const UiGlueSpec& spec);
QString lidlMakeUiGlueSource(const UiGlueSpec& spec);

// --backend ui-module: the backend runs as a core module, not in ui-host.
QString lidlMakeUiModuleGlueHeader(const UiGlueSpec& spec);
QString lidlMakeUiModuleGlueSource(const UiGlueSpec& spec);
