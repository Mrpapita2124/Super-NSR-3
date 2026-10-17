cmake_minimum_required(VERSION 3.20)
project(Super_Mario_Bros_3_USA_runner C)

# Cambia esta ruta a tu checkout real de NESRecomp.
set(NESRECOMP_ROOT "C:/path/to/nesrecomp" CACHE PATH "Root of the NESRecomp checkout")
set(NESRECOMP_BACKEND "cycle" CACHE STRING "CPU backend: cycle or legacy")
set_property(CACHE NESRECOMP_BACKEND PROPERTY STRINGS cycle legacy)

# Si quieres usar el backend cycle, usa el runner oficial de NESRecomp.
if(NESRECOMP_BACKEND STREQUAL "cycle")
    include("${NESRECOMP_ROOT}/runner/cyc/project.cmake")

    nesrecomp_add_cycle_game(SMB3
        ROM "${CMAKE_SOURCE_DIR}/Super Mario Bros. 3 (USA).nes"
        GAME_CONFIG "${CMAKE_SOURCE_DIR}/game.toml"
        HEADLESS OFF
    )
    return()
endif()

if(NOT NESRECOMP_BACKEND STREQUAL "legacy")
    message(FATAL_ERROR "Unknown NESRECOMP_BACKEND: ${NESRECOMP_BACKEND}")
endif()

# Si necesitas compatibilidad antigua, aquí iría tu configuración legacy.
# Pero para un proyecto nuevo, la ruta recomendada es cycle.
