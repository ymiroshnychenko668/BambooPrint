# GUI/DeviceWeb
# usage -- GUI about device web for BambuStudio
# date -- 2025.01.01
# status -- Building

# ==================== Node.js/pnpm 环境准备（仅用于 deviceweb 构建）====================

include("${CMAKE_CURRENT_LIST_DIR}/PrepareNodeEnv.cmake")

# 执行环境准备（在 config 阶段执行）
prepare_node_env("${CMAKE_SOURCE_DIR}/../node-cache")

# ==================== 前端工程构建 ====================

set(DEVICE_PAGE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/device_page")
set(DEVICE_PAGE_DIST_DIR "${DEVICE_PAGE_SRC_DIR}/dist")
set(DEVICE_PAGE_TARGET_DIR "${CMAKE_SOURCE_DIR}/resources/web/device_page/dist")

get_filename_component(NODE_BIN_PATH "${NODE_EXECUTABLE}" DIRECTORY)

if(WIN32)
    # 原本PATH末尾的\会影响后续的分隔符，需要处理
    set(DEVICE_PAGE_PATH_UNESCAPED "${NODE_BIN_PATH};$ENV{PATH}")
    string(REPLACE "\;" ";" DEVICE_PAGE_PATH "${DEVICE_PAGE_PATH_UNESCAPED}")
else()
    # Unix PATH 分隔符是 :
    set(DEVICE_PAGE_PATH "${NODE_BIN_PATH}:$ENV{PATH}")
endif()

set(PNPM_STORE_DIR "${CMAKE_CURRENT_BINARY_DIR}/.pnpm-store")
file(MAKE_DIRECTORY "${PNPM_STORE_DIR}")

set(DEVICE_PAGE_BUILD_STAMP "${CMAKE_CURRENT_BINARY_DIR}/device_page.stamp")

file(GLOB_RECURSE DEVICE_PAGE_SRC_FILES CONFIGURE_DEPENDS
    "${DEVICE_PAGE_SRC_DIR}/src/*"
)
file(GLOB_RECURSE DEVICE_PAGE_LOCALE_FILES CONFIGURE_DEPENDS
    "${DEVICE_PAGE_SRC_DIR}/locales/*"
)
file(GLOB_RECURSE DEVICE_PAGE_PUBLIC_FILES CONFIGURE_DEPENDS
    "${DEVICE_PAGE_SRC_DIR}/public/*"
)

set(DEVICE_PAGE_BUILD_INPUTS
    ${DEVICE_PAGE_SRC_FILES}
    ${DEVICE_PAGE_LOCALE_FILES}
    ${DEVICE_PAGE_PUBLIC_FILES}
    "${DEVICE_PAGE_SRC_DIR}/index.html"
    "${DEVICE_PAGE_SRC_DIR}/package.json"
    "${DEVICE_PAGE_SRC_DIR}/pnpm-lock.yaml"
    "${DEVICE_PAGE_SRC_DIR}/vite.config.ts"
    "${DEVICE_PAGE_SRC_DIR}/tsconfig.json"
    "${DEVICE_PAGE_SRC_DIR}/tsconfig.app.json"
    "${DEVICE_PAGE_SRC_DIR}/tsconfig.node.json"
    "${DEVICE_PAGE_SRC_DIR}/eslint.config.js"
    "${DEVICE_PAGE_SRC_DIR}/i18next-parser.config.js"
)

add_custom_command(
    OUTPUT "${DEVICE_PAGE_BUILD_STAMP}"
    COMMAND echo "Running PNPM install"
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${DEVICE_PAGE_PATH}" ${PNPM_EXECUTABLE} install --store-dir ${PNPM_STORE_DIR}
    COMMAND echo "Running PNPM run build"
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${DEVICE_PAGE_PATH}" ${PNPM_EXECUTABLE} run build
    COMMAND echo "Removing page target directory"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${DEVICE_PAGE_TARGET_DIR}"
    COMMAND echo "Creating page target directory"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DEVICE_PAGE_TARGET_DIR}"
    COMMAND echo "Copying directory to target"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${DEVICE_PAGE_DIST_DIR}" "${DEVICE_PAGE_TARGET_DIR}"
    COMMAND echo "Create device page stamp file"
    COMMAND ${CMAKE_COMMAND} -E touch "${DEVICE_PAGE_BUILD_STAMP}"
    DEPENDS ${DEVICE_PAGE_BUILD_INPUTS}
    WORKING_DIRECTORY ${DEVICE_PAGE_SRC_DIR}
    COMMENT "Building device_page web bundle when sources change..."
    VERBATIM
)

# Not ALL: rebuilt only when a dependent target (libslic3r_gui / BambuStudio) is built.
add_custom_target(device_page_build DEPENDS "${DEVICE_PAGE_BUILD_STAMP}")

list(APPEND SLIC3R_GUI_SOURCES
    GUI/DeviceWeb/DeviceHttpServer.cpp
    GUI/DeviceWeb/DeviceHttpServer.hpp
    GUI/DeviceWeb/DeviceWebPage.cpp
    GUI/DeviceWeb/DeviceWebPage.hpp
    GUI/DeviceWeb/DeviceWebHost.cpp
    GUI/DeviceWeb/DeviceWebHost.hpp
    GUI/DeviceWeb/DeviceWebBridge.cpp
    GUI/DeviceWeb/DeviceWebBridge.hpp
    GUI/DeviceWeb/DeviceWebModel.hpp
    GUI/DeviceWeb/IViewModel.hpp
    GUI/DeviceWeb/DeviceWebManager.cpp
    GUI/DeviceWeb/DeviceWebManager.hpp
    GUI/DeviceWeb/ViewModels/FilamentManager/FilamentManagerVM.cpp
    GUI/DeviceWeb/ViewModels/FilamentManager/FilamentManagerVM.hpp
    GUI/DeviceWeb/ViewModels/DevicePage/DevicePageDialogHelpers.cpp
    GUI/DeviceWeb/ViewModels/DevicePage/DevicePageDialogHelpers.h
    GUI/DeviceWeb/ViewModels/DevicePage/AmsControlWeb/ViewModel.cpp
    GUI/DeviceWeb/ViewModels/DevicePage/AmsControlWeb/ViewModel.hpp
    GUI/DeviceWeb/ViewModels/DevicePage/AmsControlWeb/ViewModelActions.cpp
    GUI/DeviceWeb/ViewModels/DevicePage/AmsControlWeb/ViewModelActions.hpp
    GUI/DeviceWeb/ViewModels/DevicePage/AmsControlWeb/ViewModelDataBuilder.cpp
    GUI/DeviceWeb/ViewModels/DevicePage/AmsControlWeb/ViewModelDataBuilder.hpp
    GUI/DeviceWeb/ViewModels/DevicePage/AmsControlWeb/ViewModelDisplayBuilder.cpp
    GUI/DeviceWeb/ViewModels/DevicePage/AmsControlWeb/ViewModelDisplayBuilder.hpp
    GUI/DeviceWeb/ViewModels/DevicePage/AmsControlWeb/ViewModelLayoutBuilder.cpp
    GUI/DeviceWeb/ViewModels/DevicePage/AmsControlWeb/ViewModelLayoutBuilder.hpp
    GUI/DeviceWeb/ViewModels/DevicePage/AmsControlWeb/Schema.hpp
    GUI/DeviceWeb/ViewModels/DevicePage/AmsControlWeb/SchemaData.hpp
    GUI/DeviceWeb/ViewModels/DevicePage/AmsControlWeb/SchemaDisplay.hpp
)
set(SLIC3R_GUI_SOURCES ${SLIC3R_GUI_SOURCES} PARENT_SCOPE)