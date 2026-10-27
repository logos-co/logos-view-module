# Builds a REAL view plugin out of the generator's output and checks the
# meta-object surface ui-host reaches it through. See main.cpp for what is
# pinned and why it is pinned through strings rather than through the class.
cmake_minimum_required(VERSION 3.25)  # try_compile(SOURCE_FROM_CONTENT)
project(UiPluginMetaObjectCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core RemoteObjects)

# ── Inputs ────────────────────────────────────────────────────────────────
# GENERATED_DIR holds what logos-view-generator emitted; the harness runs the
# generator and passes the directory in. The fixture never commits generated
# files -- the whole point is to compile what the generator produces TODAY.
if(NOT LOGOS_GENERATED_DIR)
    message(FATAL_ERROR "ui-plugin-metaobject: LOGOS_GENERATED_DIR is not set.")
endif()

# The templates, straight from this repo's cmake/ -- the same directory
# logos_module(REP_FILE ...) is handed in a real build, so this check compiles
# the file every ui_qml module compiles rather than a private copy of it.
if(NOT LOGOS_VIEW_TEMPLATE_DIR AND DEFINED ENV{LOGOS_VIEW_TEMPLATE_DIR})
    set(LOGOS_VIEW_TEMPLATE_DIR "$ENV{LOGOS_VIEW_TEMPLATE_DIR}")
endif()
if(NOT LOGOS_VIEW_TEMPLATE_DIR)
    message(FATAL_ERROR "ui-plugin-metaobject: LOGOS_VIEW_TEMPLATE_DIR is not set.")
endif()

# The REAL context header, not a stub: compiling against it is what proves the
# emitter's maybeUiPluginAboutToUnload() call and the header's declaration of
# it still agree. Those two are the pair that rotted apart across repos.
if(NOT LOGOS_VIEW_INCLUDE_DIR)
    message(FATAL_ERROR "ui-plugin-metaobject: LOGOS_VIEW_INCLUDE_DIR is not set.")
endif()

# A backend that derives LogosUiPluginContext must be told it is deprecated;
# the header's own helpers must not be (they only dispatch on the class).
try_compile(_ui_ctx_deprecation_silent
    SOURCE_FROM_CONTENT check.cpp
        "#include \"logos_ui_plugin_context.h\"\nstruct B : LogosUiPluginContext {};\nint main() { B b; return 0; }\n"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${LOGOS_VIEW_INCLUDE_DIR}"
    COMPILE_DEFINITIONS -Werror=deprecated-declarations
    OUTPUT_VARIABLE _ui_ctx_deprecation_log)
if(_ui_ctx_deprecation_silent)
    message(FATAL_ERROR "LogosUiPluginContext is not marked deprecated for its backends")
elseif(NOT _ui_ctx_deprecation_log MATCHES "LogosUiPluginContext \\(ui-host\\) is deprecated")
    message(FATAL_ERROR "the deprecation check failed for another reason:\n${_ui_ctx_deprecation_log}")
endif()
try_compile(_ui_ctx_helpers_clean
    SOURCE_FROM_CONTENT helpers.cpp
        "#include \"logos_ui_plugin_context.h\"\nint main() { return 0; }\n"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${LOGOS_VIEW_INCLUDE_DIR}"
    COMPILE_DEFINITIONS -Werror=deprecated-declarations
    OUTPUT_VARIABLE _ui_ctx_helpers_log)
if(NOT _ui_ctx_helpers_clean)
    message(FATAL_ERROR "including logos_ui_plugin_context.h alone warns:\n${_ui_ctx_helpers_log}")
endif()
# This fixture IS a ui-host backend, on purpose.
add_compile_definitions(LOGOS_UI_HOST_ALLOWED)

set(REP_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Ticker.rep")

# Mirrors LogosModule.cmake's extraction, and the generator's own.
file(READ "${REP_FILE}" _REP_CONTENTS)
string(REGEX MATCH "class[ \t]+([A-Za-z_][A-Za-z0-9_]*)" _ "${_REP_CONTENTS}")
set(LOGOS_REP_CLASS "${CMAKE_MATCH_1}")                       # TickerBackend
get_filename_component(LOGOS_REP_BASE "${REP_FILE}" NAME_WE)  # Ticker

# ── Instantiate the view-plugin base from this repo's templates ───────────
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/view_plugin_base_gen")
file(MAKE_DIRECTORY "${GEN_DIR}")
configure_file("${LOGOS_VIEW_TEMPLATE_DIR}/LogosViewPluginBase.h.in"
               "${GEN_DIR}/LogosViewPluginBase.h" @ONLY)
configure_file("${LOGOS_VIEW_TEMPLATE_DIR}/LogosViewPluginBase.cpp.in"
               "${GEN_DIR}/LogosViewPluginBase.cpp" @ONLY)

# ── The plugin: generator output + user-written backend + the base ────────
add_library(ticker_panel_plugin MODULE
    "${LOGOS_GENERATED_DIR}/ticker_panel_ui_glue.cpp"
    "${LOGOS_GENERATED_DIR}/ticker_panel_ui_glue.h"
    "${LOGOS_GENERATED_DIR}/ticker_panel_ui_interface.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ticker_panel_backend.h"
    "${GEN_DIR}/LogosViewPluginBase.h"
    "${GEN_DIR}/LogosViewPluginBase.cpp"
)

set_target_properties(ticker_panel_plugin PROPERTIES
    AUTOMOC ON
    PREFIX ""
    OUTPUT_NAME "ticker_panel_plugin"
)

target_include_directories(ticker_panel_plugin PRIVATE
    "${LOGOS_GENERATED_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${LOGOS_VIEW_INCLUDE_DIR}"
    "${GEN_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

# Gives LogosViewPluginBase.cpp its rep_Ticker_source.h and the backend its
# TickerBackendSimpleSource -- the real repc pipeline, so the fixture is a
# faithful view plugin rather than a QObject wearing its name.
qt6_add_repc_sources(ticker_panel_plugin "${REP_FILE}")

target_link_libraries(ticker_panel_plugin PRIVATE Qt6::Core Qt6::RemoteObjects)

# ── The checker ───────────────────────────────────────────────────────────
# Knows nothing about the plugin's headers, exactly like the host: it loads the
# .so and resolves every member by string.
add_executable(ui_plugin_metaobject_check
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
)
target_link_libraries(ui_plugin_metaobject_check PRIVATE Qt6::Core)
