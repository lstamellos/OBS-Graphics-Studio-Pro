cmake_minimum_required(VERSION 3.16)
project(obs-graphics-studio-pro VERSION 1.0.0)

# ── C++ standard ────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── OBS plugin packaging layout ─────────────────────────────────────
# OBS' modern manual-install layout is:
#   <plugins-root>/<plugin-name>/bin/<arch>/<plugin binary>
#   <plugins-root>/<plugin-name>/data/locale/*.ini
# Keeping build artifacts in this shape makes the build tree directly copyable
# into the OBS plugins directory and matches the Windows ProgramData layout.
set(OBS_TITLES_PLUGIN_NAME "obs-graphics-studio-pro")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(OBS_TITLES_ARCH_DIR "64bit")
  set(OBS_TITLES_VCPKG_DEFAULT_TRIPLET "x64-windows")
else()
  set(OBS_TITLES_ARCH_DIR "32bit")
  set(OBS_TITLES_VCPKG_DEFAULT_TRIPLET "x86-windows")
endif()
set(OBS_TITLES_PACKAGE_DIR
    "${CMAKE_BINARY_DIR}/${OBS_TITLES_PLUGIN_NAME}"
    CACHE PATH "Staging directory for the OBS user-plugin folder layout")
set(OBS_TITLES_PACKAGE_BIN_DIR
    "${OBS_TITLES_PACKAGE_DIR}/bin/${OBS_TITLES_ARCH_DIR}")

# ── Qt auto-tools ───────────────────────────────────────────────────
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ── Find OBS ────────────────────────────────────────────────────────
# Try to find standard CMake config package first (e.g. from CMAKE_PREFIX_PATH)
find_package(libobs QUIET)
find_package(obs-frontend-api QUIET)

if(NOT libobs_FOUND OR NOT obs-frontend-api_FOUND)
  # Allow callers to point at an OBS SDK/install tree without baking in
  # machine-specific paths. CMake cache values take precedence over
  # environment variables so CI and developers can override discovery.
  set(OBS_SEARCH_ROOTS)
  if(OBS_SDK_DIR)
    list(APPEND OBS_SEARCH_ROOTS "${OBS_SDK_DIR}")
  endif()
  if(DEFINED ENV{OBS_SDK_DIR})
    list(APPEND OBS_SEARCH_ROOTS "$ENV{OBS_SDK_DIR}")
  endif()
  if(DEFINED ENV{OBS_STUDIO_DIR})
    list(APPEND OBS_SEARCH_ROOTS "$ENV{OBS_STUDIO_DIR}")
  endif()

  if(WIN32)
    foreach(base_dir
        "$ENV{USERPROFILE}/Desktop/obs-build-dependencies"
        "$ENV{USERPROFILE}/Downloads/obs-build-dependencies"
        "${CMAKE_SOURCE_DIR}/obs-build-dependencies")
      if(IS_DIRECTORY "${base_dir}")
        file(GLOB plugin_deps "${base_dir}/plugin-deps-*")
        list(SORT plugin_deps ORDER DESCENDING)
        list(APPEND OBS_SEARCH_ROOTS ${plugin_deps})
      endif()
    endforeach()

    if(DEFINED ENV{ProgramFiles})
      list(APPEND OBS_SEARCH_ROOTS "$ENV{ProgramFiles}/obs-studio")
    endif()
    if(DEFINED ENV{ProgramW6432})
      list(APPEND OBS_SEARCH_ROOTS "$ENV{ProgramW6432}/obs-studio")
    endif()
  endif()

  list(REMOVE_DUPLICATES OBS_SEARCH_ROOTS)

  foreach(candidate IN LISTS OBS_SEARCH_ROOTS)
    if(NOT IS_DIRECTORY "${candidate}")
      continue()
    endif()

    unset(OBS_LIB CACHE)
    unset(OBS_LIB)
    unset(OBS_FRONTEND_API_LIB CACHE)
    unset(OBS_FRONTEND_API_LIB)
    unset(OBS_INCLUDE_DIR CACHE)
    unset(OBS_INCLUDE_DIR)
    unset(OBS_DLL CACHE)
    unset(OBS_DLL)
    unset(OBS_FRONTEND_API_DLL CACHE)
    unset(OBS_FRONTEND_API_DLL)

    find_library(OBS_LIB NAMES obs
      HINTS "${candidate}/lib" "${candidate}/lib/obs"
      NO_DEFAULT_PATH)
    find_library(OBS_FRONTEND_API_LIB NAMES obs-frontend-api
      HINTS "${candidate}/lib" "${candidate}/lib/obs"
      NO_DEFAULT_PATH)
    find_path(OBS_INCLUDE_DIR NAMES obs.h
      HINTS "${candidate}/include" "${candidate}/include/obs"
      NO_DEFAULT_PATH)

    if(OBS_LIB AND OBS_FRONTEND_API_LIB AND OBS_INCLUDE_DIR)
      set(OBS_SDK_DIR "${candidate}" CACHE PATH "Path to OBS Studio SDK" FORCE)
      message(STATUS "Found OBS SDK at: ${OBS_SDK_DIR}")
      list(APPEND CMAKE_PREFIX_PATH "${OBS_SDK_DIR}")

      # Find DLLs to specify IMPORTED_LOCATION and prevent CMP0111 warnings.
      find_file(OBS_DLL NAMES obs.dll
        HINTS "${OBS_SDK_DIR}/bin/64bit" "${OBS_SDK_DIR}/bin"
        NO_DEFAULT_PATH)
      find_file(OBS_FRONTEND_API_DLL NAMES obs-frontend-api.dll
        HINTS "${OBS_SDK_DIR}/bin/64bit" "${OBS_SDK_DIR}/bin"
        NO_DEFAULT_PATH)

      if(NOT TARGET OBS::libobs)
        add_library(OBS::libobs SHARED IMPORTED GLOBAL)
        set_target_properties(OBS::libobs PROPERTIES
          INTERFACE_INCLUDE_DIRECTORIES "${OBS_INCLUDE_DIR}"
        )
        if(WIN32)
          set_target_properties(OBS::libobs PROPERTIES IMPORTED_IMPLIB "${OBS_LIB}")
          if(OBS_DLL)
            set_target_properties(OBS::libobs PROPERTIES IMPORTED_LOCATION "${OBS_DLL}")
          endif()
        else()
          set_target_properties(OBS::libobs PROPERTIES IMPORTED_LOCATION "${OBS_LIB}")
        endif()
      endif()
      if(NOT TARGET OBS::obs-frontend-api)
        add_library(OBS::obs-frontend-api SHARED IMPORTED GLOBAL)
        set_target_properties(OBS::obs-frontend-api PROPERTIES
          INTERFACE_INCLUDE_DIRECTORIES "${OBS_INCLUDE_DIR}"
        )
        if(WIN32)
          set_target_properties(OBS::obs-frontend-api PROPERTIES IMPORTED_IMPLIB "${OBS_FRONTEND_API_LIB}")
          if(OBS_FRONTEND_API_DLL)
            set_target_properties(OBS::obs-frontend-api PROPERTIES IMPORTED_LOCATION "${OBS_FRONTEND_API_DLL}")
          endif()
        else()
          set_target_properties(OBS::obs-frontend-api PROPERTIES IMPORTED_LOCATION "${OBS_FRONTEND_API_LIB}")
        endif()
      endif()
      if(EXISTS "${OBS_SDK_DIR}/cmake/Qt6")
        set(Qt6_DIR "${OBS_SDK_DIR}/cmake/Qt6" CACHE PATH "Qt6 cmake dir from OBS" FORCE)
      endif()
      set(libobs_FOUND TRUE)
      set(obs-frontend-api_FOUND TRUE)
      break()
    endif()

    unset(OBS_LIB CACHE)
    unset(OBS_LIB)
    unset(OBS_FRONTEND_API_LIB CACHE)
    unset(OBS_FRONTEND_API_LIB)
    unset(OBS_INCLUDE_DIR CACHE)
    unset(OBS_INCLUDE_DIR)
  endforeach()
endif()

if(NOT libobs_FOUND OR NOT obs-frontend-api_FOUND)
  # Fallback to finding them normally if we didn't resolve them manually
  find_package(libobs REQUIRED)
  find_package(obs-frontend-api REQUIRED)
endif()

# ── Find Qt ─────────────────────────────────────────────────────────
find_package(Qt6 COMPONENTS Core Widgets Svg OpenGLWidgets QUIET)
if(NOT Qt6_FOUND)
  find_package(Qt5 5.15 COMPONENTS Core Widgets Svg OpenGL REQUIRED)
  set(QT_VERSION_MAJOR 5)
else()
  set(QT_VERSION_MAJOR 6)
endif()

set(QT_LIBS Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Svg)
if(QT_VERSION_MAJOR EQUAL 6)
  list(APPEND QT_LIBS Qt6::OpenGLWidgets)
elseif(TARGET Qt5::OpenGL)
  list(APPEND QT_LIBS Qt5::OpenGL)
endif()

# ── nlohmann/json ────────────────────────────────────────────────────
# Either install via package manager:  apt install nlohmann-json3-dev
# or rely on FetchContent:
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

# ── Sources ─────────────────────────────────────────────────────────
set(PLUGIN_SOURCES
    src/plugin-main.cpp
    src/title-data.cpp
    src/title-source.cpp
    src/title-renderer-gpu.cpp
    src/title-hotkeys.cpp
    src/title-dock.cpp
    src/title-editor.cpp
)

set(PLUGIN_HEADERS
    src/plugin-main.h
    src/title-data.h
    src/title-source.h
    src/title-renderer-gpu.h
    src/title-hotkeys.h
    src/title-dock.h
    src/title-editor.h
)

# ── Target ──────────────────────────────────────────────────────────
add_library(obs-graphics-studio-pro MODULE
    ${PLUGIN_SOURCES}
    ${PLUGIN_HEADERS}
)

target_include_directories(obs-graphics-studio-pro PRIVATE
    src
)

target_link_libraries(obs-graphics-studio-pro PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    ${QT_LIBS}
    nlohmann_json::nlohmann_json
)

# ── Locale / data files ──────────────────────────────────────────────
target_compile_definitions(obs-graphics-studio-pro PRIVATE
    PLUGIN_NAME="${OBS_TITLES_PLUGIN_NAME}"
    PLUGIN_VERSION="${PROJECT_VERSION}"
)

# ── OBS module helper (sets correct output name & install path) ──────
# This macro is provided by the OBS cmake helpers:
#   set_target_obs_bundle(obs-graphics-studio-pro)
# If building standalone, stage artifacts in OBS' user-plugin folder layout.
set_target_properties(obs-graphics-studio-pro PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${OBS_TITLES_PACKAGE_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${OBS_TITLES_PACKAGE_BIN_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
  string(TOUPPER "${config}" config_upper)
  set_target_properties(obs-graphics-studio-pro PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${OBS_TITLES_PACKAGE_BIN_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${OBS_TITLES_PACKAGE_BIN_DIR}"
      ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/lib/${config}")
endforeach()

add_custom_command(TARGET obs-graphics-studio-pro POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${OBS_TITLES_PACKAGE_DIR}/data"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/data/locale"
            "${OBS_TITLES_PACKAGE_DIR}/data/locale"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/data/icons"
            "${OBS_TITLES_PACKAGE_DIR}/data/icons"
    COMMENT "Staging OBS plugin data files")

if(WIN32)
  # OBS loads third-party DLL dependencies from the plugin binary directory.
  # Copy vcpkg-provided runtime DLLs into the staged folder so the plugin does
  # not compile successfully but fail LoadLibrary at OBS startup.
  set(OBS_TITLES_RUNTIME_DLL_DIRS)
  if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND OBS_TITLES_RUNTIME_DLL_DIRS
         "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
  endif()
  if(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND OBS_TITLES_RUNTIME_DLL_DIRS
         "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
  endif()
  if(DEFINED ENV{VCPKG_ROOT})
    if(DEFINED VCPKG_TARGET_TRIPLET)
      list(APPEND OBS_TITLES_RUNTIME_DLL_DIRS
           "$ENV{VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/bin")
    endif()
    list(APPEND OBS_TITLES_RUNTIME_DLL_DIRS
         "$ENV{VCPKG_ROOT}/installed/${OBS_TITLES_VCPKG_DEFAULT_TRIPLET}/bin")
  endif()

  set(OBS_TITLES_RUNTIME_DLLS)
  list(REMOVE_DUPLICATES OBS_TITLES_RUNTIME_DLL_DIRS)
  foreach(runtime_dir IN LISTS OBS_TITLES_RUNTIME_DLL_DIRS)
    if(IS_DIRECTORY "${runtime_dir}")
      file(GLOB runtime_dlls CONFIGURE_DEPENDS "${runtime_dir}/*.dll")
      list(APPEND OBS_TITLES_RUNTIME_DLLS ${runtime_dlls})
    endif()
  endforeach()

  if(OBS_TITLES_RUNTIME_DLLS)
    list(REMOVE_DUPLICATES OBS_TITLES_RUNTIME_DLLS)
    add_custom_command(TARGET obs-graphics-studio-pro POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${OBS_TITLES_RUNTIME_DLLS}
                "$<TARGET_FILE_DIR:obs-graphics-studio-pro>"
        COMMAND_EXPAND_LISTS
        COMMENT "Copying Windows runtime DLL dependencies")
    install(FILES ${OBS_TITLES_RUNTIME_DLLS}
            DESTINATION ${OBS_TITLES_PLUGIN_NAME}/bin/${OBS_TITLES_ARCH_DIR})
  else()
    message(WARNING
      "No vcpkg runtime DLLs were found to bundle with obs-graphics-studio-pro. "
      "If OBS reports that obs-graphics-studio-pro failed to load, ensure Qt "
      "runtime DLLs are next to the plugin binary or use build-windows.ps1.")
  endif()
endif()

# ── Install ──────────────────────────────────────────────────────────
# Install into the same user-plugin layout expected below the chosen prefix.
# Example Windows prefix: C:/ProgramData/obs-studio/plugins
# Example Linux prefix:   ~/.config/obs-studio/plugins
install(TARGETS obs-graphics-studio-pro
        RUNTIME DESTINATION ${OBS_TITLES_PLUGIN_NAME}/bin/${OBS_TITLES_ARCH_DIR}
        LIBRARY DESTINATION ${OBS_TITLES_PLUGIN_NAME}/bin/${OBS_TITLES_ARCH_DIR})

# ── Data files ──────────────────────────────────────────────────────
install(DIRECTORY data/locale
        DESTINATION ${OBS_TITLES_PLUGIN_NAME}/data)
install(DIRECTORY data/icons
        DESTINATION ${OBS_TITLES_PLUGIN_NAME}/data)

# ── Print summary ────────────────────────────────────────────────────
message(STATUS "OBS Graphics Studio Pro: Qt${QT_VERSION_MAJOR}, OBS-native GPU renderer")
