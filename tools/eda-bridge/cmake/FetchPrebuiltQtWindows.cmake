##
## FetchPrebuiltQtWindows.cmake
##
## Copyright (C) 2026, Charles Chiou
##
## Automatically retrieves official prebuilt Qt 6.8.3 Windows binaries via
## aqtinstall into ${AIMON_ROOT_DIR}/build/qt-prebuilt without ever compiling
## Qt from source (.agents/skills/cross-platform-build/SKILL.md).
##

set(AIMON_QT_VERSION "6.8.3" CACHE STRING "Official prebuilt Qt6 version for Windows")

if(MINGW)
    set(AIMON_QT_ARCH "win64_mingw")
    set(AIMON_QT_SUBDIR "mingw_64")
elseif(MSVC)
    set(AIMON_QT_ARCH "win64_msvc2022_64")
    set(AIMON_QT_SUBDIR "msvc2022_64")
else()
    message(FATAL_ERROR "FetchPrebuiltQtWindows.cmake requires MINGW or MSVC target toolchain")
endif()

set(AIMON_QT_PREBUILT_ROOT "${AIMON_ROOT_DIR}/build/qt-prebuilt")
set(AIMON_QT_PREFIX "${AIMON_QT_PREBUILT_ROOT}/${AIMON_QT_VERSION}/${AIMON_QT_SUBDIR}")

if(NOT EXISTS "${AIMON_QT_PREFIX}/include/QtWidgets/QApplication")
    message(STATUS "[aimon] Official prebuilt Qt ${AIMON_QT_VERSION} (${AIMON_QT_ARCH}) not found at ${AIMON_QT_PREFIX}; fetching via aqtinstall...")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    set(AIMON_AQT_VENV "${AIMON_ROOT_DIR}/build/.venv-aqt")
    if(CMAKE_HOST_WIN32)
        set(AIMON_AQT_PYTHON "${AIMON_AQT_VENV}/Scripts/python.exe")
    else()
        set(AIMON_AQT_PYTHON "${AIMON_AQT_VENV}/bin/python3")
    endif()

    if(NOT EXISTS "${AIMON_AQT_PYTHON}")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -m venv "${AIMON_AQT_VENV}"
            RESULT_VARIABLE _venv_res
        )
        if(NOT _venv_res EQUAL 0)
            message(FATAL_ERROR "Failed to create Python venv at ${AIMON_AQT_VENV}")
        endif()

        execute_process(
            COMMAND "${AIMON_AQT_PYTHON}" -m pip install --quiet --upgrade pip aqtinstall
            RESULT_VARIABLE _pip_res
        )
        if(NOT _pip_res EQUAL 0)
            message(FATAL_ERROR "Failed to install aqtinstall into ${AIMON_AQT_VENV}")
        endif()
    endif()

    file(MAKE_DIRECTORY "${AIMON_QT_PREBUILT_ROOT}")
    execute_process(
        COMMAND "${AIMON_AQT_PYTHON}" -m aqt install-qt windows desktop "${AIMON_QT_VERSION}" "${AIMON_QT_ARCH}" -O "${AIMON_QT_PREBUILT_ROOT}"
        RESULT_VARIABLE _aqt_res
    )
    if(NOT _aqt_res EQUAL 0)
        message(FATAL_ERROR "aqtinstall failed to download Qt ${AIMON_QT_VERSION} (${AIMON_QT_ARCH})")
    endif()
endif()

if(NOT EXISTS "${AIMON_QT_PREFIX}/include/QtWidgets/QApplication")
    message(FATAL_ERROR "Prebuilt Qt headers missing at ${AIMON_QT_PREFIX}/include/QtWidgets/QApplication")
endif()

set(AIMON_QT_INCLUDE_DIRS
    "${AIMON_QT_PREFIX}/include"
    "${AIMON_QT_PREFIX}/include/QtCore"
    "${AIMON_QT_PREFIX}/include/QtGui"
    "${AIMON_QT_PREFIX}/include/QtWidgets"
)

if(MINGW)
    set(AIMON_QT_LIBRARIES
        "${AIMON_QT_PREFIX}/lib/libQt6Widgets.a"
        "${AIMON_QT_PREFIX}/lib/libQt6Gui.a"
        "${AIMON_QT_PREFIX}/lib/libQt6Core.a"
    )
else()
    set(AIMON_QT_LIBRARIES
        "${AIMON_QT_PREFIX}/lib/Qt6Widgets.lib"
        "${AIMON_QT_PREFIX}/lib/Qt6Gui.lib"
        "${AIMON_QT_PREFIX}/lib/Qt6Core.lib"
    )
endif()

message(STATUS "[aimon] Using official prebuilt Qt6 at ${AIMON_QT_PREFIX}")
