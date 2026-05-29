set(NOMAD_SRCS
    "src/nomad.cpp"
    # LCD slice — see reticulous-lxmf/CMakeLists.txt for the same pattern.
    "lcd/src/nomad_lcd.cpp"
)

include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
idf_component_register(
    SRCS ${NOMAD_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        ${SPANGAP_REQUIRES}
        json
)
