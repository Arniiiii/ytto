cmake_minimum_required(VERSION 3.22...3.29)

project(
    YoutubeToOllama
  VERSION 0.0.0.1
  LANGUAGES CXX)

option(${PROJECT_NAME}_ENABLE_WARNINGS "Enable warnings" YES)
# option(${PROJECT_NAME}_BUILD_FUZZER "Build fuzzer" YES)
# option(BUILD_SHARED_LIBS "yes/no" YES)

# ---- Include guards ----

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(
    FATAL_ERROR
      "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
  )
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/tools.cmake)

# ---- Add source files ----

file(GLOB_RECURSE headers CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp")

add_executable(${PROJECT_NAME} ${headers} ${sources}
                               ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

target_include_directories(
  ${PROJECT_NAME}
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include/${PROJECT_NAME}>)

# https://youtu.be/Nm9-xKsZoNI?t=912
target_compile_options(${PROJECT_NAME}
                       PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/permissive->")


if(${${PROJECT_NAME}_ENABLE_WARNINGS})
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/warnings.cmake)
    set_project_warnings(${PROJECT_NAME})
endif()

# target_compile_options(${PROJECT_NAME} PUBLIC -fsanitize=address,leak)
# target_link_options(${PROJECT_NAME} PUBLIC -fsanitize=address,leak)
# set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 26)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/packages/addfmt.cmake)
find_package(Boost REQUIRED COMPONENTS stacktrace_from_exception process url)
# find_package(GTest REQUIRED)
find_package(corral REQUIRED)
find_package(args-parser REQUIRED)
find_package(magic_enum REQUIRED)
find_package(inja REQUIRED)
find_package(quill)
find_package(glaze REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CLI11 REQUIRED)

target_link_libraries(${PROJECT_NAME} PRIVATE Boost::boost Boost::process)
target_link_libraries(${PROJECT_NAME} PRIVATE Boost::stacktrace_from_exception)
target_link_libraries(${PROJECT_NAME} PRIVATE Boost::url)
target_link_libraries(${PROJECT_NAME} PRIVATE magic_enum::magic_enum)
# target_link_libraries(${PROJECT_NAME} PRIVATE GTest::gtest_main)
target_link_libraries(${PROJECT_NAME} PRIVATE corral::corral)
target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::fmt)
target_link_libraries(${PROJECT_NAME} PRIVATE CLI11::CLI11)
target_link_libraries(${PROJECT_NAME} PRIVATE pantor::inja)
target_link_libraries(${PROJECT_NAME} PRIVATE glaze::glaze)
target_link_libraries(${PROJECT_NAME} PRIVATE openssl::openssl)
if(TARGET quill::quill)
    target_link_libraries(${PROJECT_NAME} PRIVATE quill::quill)
endif()

# find_package(constexpr-to-string REQUIRED)
# target_link_libraries(${PROJECT_NAME}
#                       PRIVATE constexpr-to-string::constexpr-to-string)

# just add -lbacktrace
target_link_libraries(${PROJECT_NAME} PRIVATE backtrace)

# enable_testing()
# # add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})
# include(GoogleTest)
# gtest_discover_tests(${PROJECT_NAME})

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${PROJECT_NAME}
        PUBLIC -fconcepts-diagnostics-depth=100)
endif()

if(ENABLE_TEST_COVERAGE)
    target_compile_options(${PROJECT_NAME} PUBLIC -O0 -g -fprofile-arcs
                                                -ftest-coverage)
    target_link_options(${PROJECT_NAME} PUBLIC -fprofile-arcs -ftest-coverage)
endif()

# ---- Create an installable target ----

string(TOLOWER ${PROJECT_NAME}/version.h VERSION_HEADER_LOCATION)

set_property(TARGET ${PROJECT_NAME} PROPERTY VERSION ${PROJECT_VERSION})
set_property(TARGET ${PROJECT_NAME} PROPERTY SOVERSION 1)

install(TARGETS ${PROJECT_NAME})
