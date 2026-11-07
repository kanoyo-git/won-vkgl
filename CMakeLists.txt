cmake_minimum_required(VERSION 3.20)
project(vkgl C ASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4 OR NOT WIN32)
  message(FATAL_ERROR "GoldSrc is a 32-bit Windows game: configure with "
                      "-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wno-unused-parameter)
add_compile_definitions(_WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN)

set(GEN ${CMAKE_BINARY_DIR}/gen)
set(VKGL_VULKAN_INCLUDE /usr/include CACHE PATH
    "Directory with vulkan/ and vk_video/ headers (any OS: the API is the same)")

set(VKINC ${GEN}/vkinc)
file(MAKE_DIRECTORY ${VKINC})
foreach(d vulkan vk_video)
  if(NOT EXISTS ${VKINC}/${d})
    file(CREATE_LINK ${VKGL_VULKAN_INCLUDE}/${d} ${VKINC}/${d} SYMBOLIC)
  endif()
endforeach()

set(VKGL_SOURCES src/vkgl/log.c src/vkgl/select.c src/vkgl/profile.c src/vkgl/vk.c src/vkgl/wgl.c src/vkgl/gl.c
    src/vkgl/state.c src/vkgl/geometry.c src/vkgl/texture.c src/vkgl/render.c
    src/vkgl/engine.c src/vkgl/physics.c src/vkgl/capture.c src/vkgl/mouse.c src/vkgl/menu.c)

find_program(GLSLC glslc REQUIRED)
set(VKGL_SHADERS)
foreach(shader gl.vert gl.frag)
  set(out ${GEN}/${shader}.inc)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${GLSLC} -O -mfmt=c -o ${out} ${CMAKE_SOURCE_DIR}/src/vkgl/shaders/${shader}
    DEPENDS src/vkgl/shaders/${shader}
    COMMENT "Compiling ${shader}")
  list(APPEND VKGL_SHADERS ${out})
endforeach()
set(VKGL_GEN ${GEN}/vkgl_ids.h ${GEN}/vkgl_names.c ${GEN}/vkgl_stubs.S ${GEN}/vkgl.def)
add_custom_command(
  OUTPUT ${VKGL_GEN}
  COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen_stubs.py
          ${CMAKE_SOURCE_DIR}/exports/opengl32.txt ${CMAKE_NM}
          ${CMAKE_FIND_ROOT_PATH}/lib/libopengl32.a ${GEN}
          ${VKGL_SOURCES}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS tools/gen_stubs.py exports/opengl32.txt ${VKGL_SOURCES}
  COMMENT "Generating vkgl export stubs")

add_library(vkgl SHARED ${VKGL_SOURCES} ${VKGL_GEN} ${VKGL_SHADERS})
set_target_properties(vkgl PROPERTIES PREFIX "")
target_include_directories(vkgl PRIVATE src/common src/vkgl ${GEN} ${VKINC})

target_compile_options(vkgl PRIVATE -msse2 -mfpmath=sse -mstackrealign)
target_link_options(vkgl PRIVATE -static -static-libgcc)
target_link_libraries(vkgl PRIVATE user32 advapi32 kernel32 winmm)
