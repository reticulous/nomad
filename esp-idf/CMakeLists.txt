include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
# Conditional sources: esp-idf/conditional/<name>/ is compiled only when <name>
# is staged. The on-device Nomad browser program lives in conditional/spangap-lcd/
# and registers via the when:-gated nomadLcdRegister hook — no #if anywhere.
idf_component_register(
    SRCS
        "src/nomad.cpp"
        ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    REQUIRES
        ${SPANGAP_REQUIRES}
        json
)
