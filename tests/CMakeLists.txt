# TODO Add individual tests as executables in separate directories
# add_subirectory(<testcase>)

set(TEST_DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
file(TO_NATIVE_PATH "${TEST_DATA_DIR}" TEST_DATA_DIR)

add_library(Catch2 INTERFACE)
list (APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/modules/Catch2)
target_include_directories(Catch2 INTERFACE ${CMAKE_CURRENT_LIST_DIR})
add_library(Catch2::Catch2 ALIAS Catch2)
if (APPLE)
	# OSX builds targeting OSX 10.9 do not support new std::uncought_exception()
	# see https://github.com/catchorg/Catch2/issues/1218
	target_compile_definitions(Catch2 INTERFACE -DCATCH_CONFIG_NO_CPP17_UNCAUGHT_EXCEPTIONS)
endif()
include(Catch)

set(CATCH_EXTRA_ARGS "" CACHE STRING "Extra arguments for catch2 test suites.")

add_library(test_common INTERFACE)
target_compile_definitions(test_common INTERFACE TEST_DATA_DIR=R"\(${TEST_DATA_DIR}\)" CATCH_CONFIG_FAST_COMPILE)
target_link_libraries(test_common INTERFACE Catch2::Catch2)

if (APPLE)
    target_link_libraries(test_common INTERFACE "-liconv -framework IOKit" "-framework CoreFoundation" -lc++)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The desktop executable supplies these implementations for libslic3r.
    add_library(native_core_test_support OBJECT
        fff_print/native_nanosvg.cpp
        ${PROJECT_SOURCE_DIR}/src/slic3r/Utils/Http.cpp
        ${PROJECT_SOURCE_DIR}/src/slic3r/Utils/BBLUtil.cpp
    )
    target_link_libraries(native_core_test_support PUBLIC libslic3r libcurl OpenSSL::SSL OpenSSL::Crypto)
    foreach(test_name IN ITEMS native_object_witness_test native_emission_map_test native_processor_export_callbacks_test)
        add_executable(${test_name} ${test_name}.cpp)
        target_include_directories(${test_name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_compile_options(${test_name} PRIVATE -UNDEBUG)
        target_link_libraries(${test_name} PRIVATE native_core_test_support)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

add_subdirectory(libnest2d)
add_subdirectory(libslic3r)
add_subdirectory(slic3rutils)
add_subdirectory(fff_print)
add_subdirectory(sla_print)
add_subdirectory(fila_manager)
add_subdirectory(filament_group)
add_subdirectory(cpp17 EXCLUDE_FROM_ALL)    # does not have to be built all the time
# add_subdirectory(example)

if(TARGET native_core_test_support)
    # These standalone targets also link libslic3r without its desktop executable.
    foreach(target IN ITEMS
        libnest2d_tests libslic3r_tests fff_print_tests sla_print_tests
        fila_manager_tests filament_group_tests fg_test_data_generator
        support_material_on_wipe_tower_tests periodic_modifier_tests
    )
        target_link_libraries(${target} native_core_test_support)
    endforeach()
endif()
