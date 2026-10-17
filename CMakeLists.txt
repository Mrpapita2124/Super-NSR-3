cmake_minimum_required(VERSION 3.20)
project(SuperMarioBros3 C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NESRECOMP_ROOT "C:/Users/josec/.gemini/antigravity/scratch/nesrecomp" CACHE PATH "Root of NESRecomp checkout")
set(RECOMP_UI_ROOT "C:/Users/josec/.gemini/antigravity/scratch/recomp-ui" CACHE PATH "Root of recomp-ui checkout")

include("${RECOMP_UI_ROOT}/recomp_ui.cmake")
include("${NESRECOMP_ROOT}/runner/cyc/cyc.cmake")

file(GLOB NESRECOMP_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/Super-Mario-Bros-3-USA_cyc*.c"
)

add_executable(SuperMarioBros3
    ${NESRECOMP_CYC_SOURCES}
    ${NESRECOMP_GENERATED_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/smb3_widescreen.c"
)

target_include_directories(SuperMarioBros3 PRIVATE
    ${NESRECOMP_CYC_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/generated"
    "${NESRECOMP_ROOT}/runner/cyc"
    "${RECOMP_UI_ROOT}/src"
)

target_link_libraries(SuperMarioBros3 PRIVATE
    ${NESRECOMP_CYC_LIBRARIES}
)

nesrecomp_cyc_enable_sdl(SuperMarioBros3)
nesrecomp_cyc_enable_recomp_ui(SuperMarioBros3
    ROM_SHA256 "d77d17d34af24871d7ce1160ccd3330555835c8e940b7100e095ac38973d927a"
)
