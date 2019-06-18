# Based on CMakeDependentOption.cmake
#=============================================================================
# Copyright 2006-2009 Kitware, Inc.
# Copyright 2013 Pali Rohár <pali.rohar@gmail.com>
#
# Distributed under the OSI-approved BSD License (the "License");
# see accompanying file Copyright.txt for details.
#
# This software is distributed WITHOUT ANY WARRANTY; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the License for more information.
#=============================================================================
# (To distribute this file outside of CMake, substitute the full
#  License text for the above reference.)

include(FeatureSummary)

# trojita_string_option(option description default depends force [options...])
macro(trojita_string_option option description default depends force)
    if(${option}_ISSET MATCHES "^${option}_ISSET$")
        set(${option}_AVAILABLE 1)
        foreach(d ${depends})
            string(REGEX REPLACE " +" ";" CMAKE_DEPENDENT_OPTION_DEP "${d}")
            if(NOT (${CMAKE_DEPENDENT_OPTION_DEP}))
                set(${option}_AVAILABLE 0)
                message(STATUS "Disabling ${option} because of ${CMAKE_DEPENDENT_OPTION_DEP}")
            endif()
        endforeach()
        if(${option}_AVAILABLE)
            if(${option} MATCHES "^${option}$")
                set(${option} "${default}" CACHE STRING "${description}" FORCE)
            else()
                set(${option} "${${option}}" CACHE STRING "${description}" FORCE)
            endif()
            set_property(CACHE ${option} PROPERTY STRINGS ${ARGN})
        else()
            if(NOT ${option} MATCHES "^${option}$")
                set(${option} "${${option}}" CACHE INTERNAL "${description}")
            endif()
            set(${option} ${force})
        endif()
    else()
        set(${option} "${${option}_ISSET}")
    endif()
endmacro()

# trojita_option(option description default [depends])
macro(trojita_option option description default)
    set(depends ${ARGN})
    trojita_string_option(${option} "${description}" ${default} "${depends}" OFF AUTO ON OFF)
    add_feature_info(${option} ${option} "${description}")
endmacro()

# trojita_plugin_option(option description [depends] [STATIC])
macro(trojita_plugin_option option description)
    set(depends)
    set(default "AUTO")
    foreach(arg ${ARGN})
        if("${arg}" STREQUAL "STATIC")
            set(default "STATIC")
        else()
            list(APPEND depends "${arg}")
        endif()
    endforeach()
    trojita_string_option(${option} "${description}" ${default} "${depends}" OFF AUTO STATIC ON OFF)
    if(NOT WITH_SHARED_PLUGINS)
        if("${${option}}" STREQUAL "AUTO")
            if(NOT ${option} MATCHES "^${option}$")
                set(${option} "${${option}}" CACHE INTERNAL "${description}")
            endif()
            set(${option} "STATIC")
        elseif(NOT "${${option}}" STREQUAL "STATIC")
            message(STATUS "Disabling ${option} because of NOT WITH_SHARED_PLUGINS")
            if("${${option}}" STREQUAL "ON")
                message(FATAL_ERROR "Plugin ${option} is disabled")
            endif()
            if(NOT ${option} MATCHES "^${option}$")
                set(${option} "${${option}}" CACHE INTERNAL "${description}")
            endif()
            set(${option} OFF)
        endif()
    endif()
    add_feature_info(${option} ${option} "${description}")
endmacro()

# trojita_add_plugin(target type [sources...])
macro(trojita_add_plugin target type)
    if("${${type}}" STREQUAL "STATIC")
        message(STATUS "Building static plugin ${target}")
        add_library(${target} STATIC ${ARGN})
        set_property(TARGET ${target} APPEND PROPERTY COMPILE_DEFINITIONS QT_STATICPLUGIN)
        set_property(GLOBAL APPEND PROPERTY TROJITA_STATIC_PLUGINS ${target})
    else()
        message(STATUS "Building shared plugin ${target}")
        add_library(${target} MODULE ${ARGN})
        install(TARGETS ${target} DESTINATION ${CMAKE_INSTALL_PLUGIN_DIR})
        set_property(GLOBAL APPEND PROPERTY TROJITA_SHARED_PLUGINS ${target})
    endif()
    set_target_properties(${target} PROPERTIES PREFIX "")
    set_property(TARGET ${target} APPEND PROPERTY COMPILE_DEFINITIONS BUILD_PLUGIN)
    target_link_libraries(${target} Plugins)
    if (WITH_QT5)
        qt5_use_modules(${target} Core)
    else()
        target_link_libraries(${target} ${QT_QTCORE_LIBRARY})
    endif()
endmacro()

# trojita_find_package(package version url description purpose [options...])
macro(trojita_find_package package version url description purpose)
    set(type OPTIONAL)
    foreach(arg ${ARGN})
        if("${${arg}}" STREQUAL "ON" OR "${arg}" STREQUAL REQUIRED)
            message(STATUS "Package ${package} is required because of ${arg}")
            set(type REQUIRED)
        endif()
    endforeach()
    if ("${type}" STREQUAL REQUIRED)
        find_package(${package} ${version} REQUIRED)
    else()
        find_package(${package} ${version})
    endif()
    set_package_properties(${package} PROPERTIES URL "${url}" DESCRIPTION "${description}" TYPE ${type} PURPOSE "${purpose}")
    if(NOT ${package}_FOUND)
        foreach(arg ${ARGN})
            if("${${arg}}" STREQUAL "AUTO")
                message(STATUS "Disabling ${arg} because package ${package} was not found")
                if(NOT ${arg} MATCHES "^${arg}$")
                    set(${arg} "${${arg}}" CACHE INTERNAL "")
                endif()
                set(${arg} OFF)
                get_property(features GLOBAL PROPERTY ENABLED_FEATURES)
                list(REMOVE_ITEM features ${arg})
                set_property(GLOBAL PROPERTY ENABLED_FEATURES "${features}")
                get_property(features GLOBAL PROPERTY DISABLED_FEATURES)
                list(FIND features ${arg} id)
                if(id EQUAL -1)
                    set_property(GLOBAL APPEND PROPERTY DISABLED_FEATURES "${arg}")
                endif()
            endif()
        endforeach()
    endif()
endmacro()
