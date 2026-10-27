# LogosViewModule.cmake — the view-plugin half of a Logos module's build.
#
# `logos_replica_factory()` builds the typed QtRO replica factory a QML view
# loads, plus the per-module LogosViewPlugin base that lets ui-host drive the
# plugin through a plain qobject_cast instead of QMetaObject reflection. It is
# the CMake counterpart of this repo's `logos-view-generator`: the generator
# emits the plugin glue around the user's .rep and *Backend, and this builds
# the replica side of the same .rep.
#
# It lives here, and not in the Qt plugin backend, for the same reason the
# generator does: a view module is a distinct authoring flavour from a core
# module, and keeping both halves in one repo means changing how views are
# built is a one-repo change.
#
# Usage — include it and call the public entry point:
#
#     include("${LOGOS_VIEW_MODULE_ROOT}/cmake/LogosViewModule.cmake")
#     logos_replica_factory(NAME my_view REP_FILE src/MyBackend.rep)
#
# The four .in templates are SIBLINGS of this file; the function resolves them
# through CMAKE_CURRENT_FUNCTION_LIST_DIR, with a CMAKE_CURRENT_LIST_DIR
# fallback for callers that `include()` it from a different scope. Move this
# file without the templates and configure_file() fails.

# Public entry point. NAME and REP_FILE are required; QML_URI and
# QML_TYPE_NAME default to Logos.<RepClass> and <RepClass>.
function(logos_replica_factory)
    message(DEPRECATION "logos_replica_factory: replica factories serve ui-host views, which are "
                        "deprecated; no current host loads them. Removed in the next breaking release.")
    cmake_parse_arguments(VIEW "" "NAME;REP_FILE;QML_URI;QML_TYPE_NAME" "" ${ARGN})
    if(NOT VIEW_NAME)
        message(FATAL_ERROR "logos_replica_factory: NAME is required")
    endif()
    if(NOT VIEW_REP_FILE)
        message(FATAL_ERROR "logos_replica_factory: REP_FILE is required")
    endif()
    _logos_module_add_replica_factory("${VIEW_NAME}" "${VIEW_REP_FILE}"
        "${VIEW_QML_URI}" "${VIEW_QML_TYPE_NAME}")
endfunction()

function(_logos_module_add_replica_factory MODULE_NAME REP_FILE QML_URI QML_TYPE_NAME)
    # Need repc replica generation + Qml for qmlRegisterUncreatableMetaObject
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core RemoteObjects Qml)

    # Also attach the source-side repc to the plugin target so the backend has
    # the generated SimpleSource base class available.
    if(QT_VERSION_MAJOR EQUAL 6)
        qt6_add_repc_sources(${MODULE_NAME}_module_plugin ${REP_FILE})
    else()
        qt5_add_repc_sources(${MODULE_NAME}_module_plugin ${REP_FILE})
    endif()

    # Parse class name out of the .rep (first `class Foo` line).
    set(_REP_FILE_ABS "${REP_FILE}")
    if(NOT IS_ABSOLUTE "${_REP_FILE_ABS}")
        set(_REP_FILE_ABS "${CMAKE_CURRENT_SOURCE_DIR}/${REP_FILE}")
    endif()
    file(READ "${_REP_FILE_ABS}" _REP_CONTENTS)
    string(REGEX MATCH "class[ \t]+([A-Za-z_][A-Za-z0-9_]*)" _ "${_REP_CONTENTS}")
    set(LOGOS_REP_CLASS "${CMAKE_MATCH_1}")
    if(NOT LOGOS_REP_CLASS)
        message(FATAL_ERROR "logos_module: could not parse class name from ${REP_FILE}")
    endif()

    get_filename_component(LOGOS_REP_BASE "${REP_FILE}" NAME_WE)
    set(LOGOS_FACTORY_CLASS "${LOGOS_REP_CLASS}ReplicaFactoryPlugin")

    if(NOT QML_URI)
        set(QML_URI "Logos.${LOGOS_REP_CLASS}")
    endif()
    if(NOT QML_TYPE_NAME)
        set(QML_TYPE_NAME "${LOGOS_REP_CLASS}")
    endif()
    set(LOGOS_QML_URI "${QML_URI}")
    set(LOGOS_QML_TYPE_NAME "${QML_TYPE_NAME}")

    # Locate the factory h/cpp templates (sibling of this .cmake file).
    set(_TEMPLATE_DIR "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    if(NOT EXISTS "${_TEMPLATE_DIR}/LogosViewReplicaFactory.h.in")
        set(_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    endif()

    set(_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/replica_factory_${MODULE_NAME}")
    file(MAKE_DIRECTORY "${_GEN_DIR}")
    configure_file("${_TEMPLATE_DIR}/LogosViewReplicaFactory.h.in"
                   "${_GEN_DIR}/LogosViewReplicaFactory.h" @ONLY)
    configure_file("${_TEMPLATE_DIR}/LogosViewReplicaFactory.cpp.in"
                   "${_GEN_DIR}/LogosViewReplicaFactory.cpp" @ONLY)

    # Generate the per-module LogosViewPlugin base that plugins inherit
    # from. It implements viewObject() + enableRemoting() so ui-host can
    # drive the plugin via a plain qobject_cast<LogosViewPlugin*> instead
    # of QMetaObject::invokeMethod reflection.
    set(_VIEW_PLUGIN_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/view_plugin_base_${MODULE_NAME}")
    file(MAKE_DIRECTORY "${_VIEW_PLUGIN_GEN_DIR}")
    configure_file("${_TEMPLATE_DIR}/LogosViewPluginBase.h.in"
                   "${_VIEW_PLUGIN_GEN_DIR}/LogosViewPluginBase.h" @ONLY)
    configure_file("${_TEMPLATE_DIR}/LogosViewPluginBase.cpp.in"
                   "${_VIEW_PLUGIN_GEN_DIR}/LogosViewPluginBase.cpp" @ONLY)
    target_sources(${MODULE_NAME}_module_plugin PRIVATE
        "${_VIEW_PLUGIN_GEN_DIR}/LogosViewPluginBase.h"
        "${_VIEW_PLUGIN_GEN_DIR}/LogosViewPluginBase.cpp"
    )
    target_include_directories(${MODULE_NAME}_module_plugin PRIVATE
        "${_VIEW_PLUGIN_GEN_DIR}"
    )
    target_link_libraries(${MODULE_NAME}_module_plugin PRIVATE
        Qt${QT_VERSION_MAJOR}::RemoteObjects
    )

    set(_FACTORY_TARGET ${MODULE_NAME}_replica_factory)
    add_library(${_FACTORY_TARGET} SHARED
        "${_GEN_DIR}/LogosViewReplicaFactory.h"
        "${_GEN_DIR}/LogosViewReplicaFactory.cpp"
    )

    set_target_properties(${_FACTORY_TARGET} PROPERTIES
        AUTOMOC ON
        PREFIX ""
        OUTPUT_NAME "${MODULE_NAME}_replica_factory"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules"
        BUILD_WITH_INSTALL_RPATH TRUE
        SKIP_BUILD_RPATH FALSE
        INSTALL_NAME_DIR "@rpath"
    )
    if(APPLE)
        target_link_options(${_FACTORY_TARGET} PRIVATE "-Wl,-headerpad_max_install_names")
    endif()

    target_include_directories(${_FACTORY_TARGET} PRIVATE
        "${_GEN_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
    )
    if(QT_VERSION_MAJOR EQUAL 6)
        qt6_add_repc_replicas(${_FACTORY_TARGET} ${REP_FILE})
    else()
        qt5_add_repc_replicas(${_FACTORY_TARGET} ${REP_FILE})
    endif()

    target_link_libraries(${_FACTORY_TARGET} PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::RemoteObjects
        Qt${QT_VERSION_MAJOR}::Qml
    )

    if(APPLE)
        set_target_properties(${_FACTORY_TARGET} PROPERTIES
            INSTALL_RPATH "@loader_path"
            INSTALL_NAME_DIR "@rpath"
            BUILD_WITH_INSTALL_NAME_DIR TRUE
        )
        add_custom_command(TARGET ${_FACTORY_TARGET} POST_BUILD
            COMMAND install_name_tool -id "@rpath/${MODULE_NAME}_replica_factory.dylib"
                    $<TARGET_FILE:${_FACTORY_TARGET}>
            COMMENT "Updating library paths for macOS"
        )
    else()
        set_target_properties(${_FACTORY_TARGET} PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            INSTALL_RPATH_USE_LINK_PATH FALSE
        )
    endif()

    install(TARGETS ${_FACTORY_TARGET}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/logos/modules
        RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/logos/modules
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/logos/modules
    )

    message(STATUS "Logos module ${MODULE_NAME}: replica factory plugin from ${REP_FILE} "
                   "(class ${LOGOS_REP_CLASS}, QML ${LOGOS_QML_URI}.${LOGOS_QML_TYPE_NAME})")
endfunction()
