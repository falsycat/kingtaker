cmake_minimum_required(VERSION 3.20)
project(kingtaker CXX)

# ---- configuration ----
option(KINGTAKER_STATIC "link all libs statically" ON)
option(KINGTAKER_TEST   "compiles tests"           OFF)

set(KINGTAKER_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/include/generated")

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(KINGTAKER_CXX_FLAGS
  $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
    -Wall -Werror -pedantic-errors -Wextra -Wconversion -Wsign-conversion>
  $<$<CXX_COMPILER_ID:MSVC>:
    /W4 /WX>
)


# ---- thirdparty ----
add_subdirectory(thirdparty EXCLUDE_FROM_ALL)
find_package(Boost REQUIRED)


# ---- subdirs ----
add_subdirectory(tool)


# ---- KINGTAKER application ----
add_executable(kingtaker)
target_compile_options(kingtaker PRIVATE ${KINGTAKER_CXX_FLAGS})
target_include_directories(kingtaker SYSTEM BEFORE PRIVATE thirdparty)
target_include_directories(kingtaker PRIVATE . "${PROJECT_BINARY_DIR}/include")

target_compile_definitions(kingtaker
  PRIVATE
    $<$<BOOL:${KINGTAKER_TEST}>:KINGTAKER_TEST>
    $<$<PLATFORM_ID:Linux>:BOOST_STACKTRACE_USE_ADDR2LINE>
)
target_sources(kingtaker
  PRIVATE
    generic.cc
    kingtaker.cc
    kingtaker.hh
    main.cc
    node.cc
    system.cc
    test.hh
    value.cc

    iface/dir.hh
    iface/gui.hh
    iface/history.hh
    iface/node.hh
    iface/queue.hh
)
target_link_libraries(kingtaker
  PRIVATE
    $<$<PLATFORM_ID:Linux,Darwin>:pthread>
    $<$<PLATFORM_ID:Linux>:dl>

    Boost::boost
    glew
    glfw
    imgui
    imnodes
    linalg.h
    msgpackc-cxx
)


# ---- resource compilation ----
function(add_resource dst src conv)
  set(dst_full "${KINGTAKER_GENERATED_INCLUDE_DIR}/${dst}")
  set(src_full "${PROJECT_SOURCE_DIR}/${src}")

  get_filename_component(dst_dir "${dst_full}" DIRECTORY)
  file(MAKE_DIRECTORY "${dst_dir}")

  add_custom_command(
    OUTPUT  "${dst_full}"
    DEPENDS "${src_full}"
    COMMAND ${conv} < "${src_full}" > "${dst_full}"
    COMMENT "generating ${dst} from ${src}"
    VERBATIM
  )
  target_sources(kingtaker PRIVATE "${dst_full}")
endfunction()

add_resource(kingtaker.inc kingtaker.json $<TARGET_FILE:json2mpk>)
