cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# ---- configuration ----
project(kingtaker CXX)

option(KINGTAKER_STATIC "link all libs statically" ON)

set(KINGTAKER_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/include/generated")

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(KINGTAKER_CXX_FLAGS
  $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
    -Wall -Werror -pedantic-errors -Wextra -Wconversion -Wsign-conversion>
  $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:
    -Wno-overloaded-virtual>
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
    IMGUI_DEFINE_MATH_OPERATORS

    $<$<PLATFORM_ID:Linux>:BOOST_STACKTRACE_USE_ADDR2LINE>
    $<$<PLATFORM_ID:Linux>:_GNU_SOURCE>

    $<$<PLATFORM_ID:Darwin>:GL_SILENCE_DEPRECATION>
    $<$<PLATFORM_ID:Darwin>:_GNU_SOURCE>
)
target_sources(kingtaker
  PRIVATE
    kingtaker.hh

    gl.cc
    kingtaker.cc
    logic.cc
    luajit.cc
    main.cc
    node.cc
    system.cc
    value.cc

    iface/dir.hh
    iface/factory.hh
    iface/history.hh
    iface/node.hh

    util/format.hh
    util/gl.hh
    util/gui.hh
    util/gui.cc
    util/keymap.hh
    util/keymap.cc
    util/luajit.hh
    util/luajit.cc
    util/node.hh
    util/notify.hh
    util/notify.cc
    util/ptr_selector.hh
    util/queue.hh
    util/value.hh
    util/value.cc
)
target_link_libraries(kingtaker
  PRIVATE
    $<$<PLATFORM_ID:Linux,Darwin>:pthread>
    $<$<PLATFORM_ID:Linux>:dl>

    Boost::headers
    glew
    glfw
    imgui
    imnodes
    implot
    linalg.h
    luajit
    msgpackc-cxx
    source_location
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
