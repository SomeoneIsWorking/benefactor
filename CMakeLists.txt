cmake_minimum_required(VERSION 3.20)
project(benefactor-port LANGUAGES NONE)

set(BENEFACTOR_AMIGAPORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shared/amigaport"
    CACHE PATH "Path to the shared amigaport runtime")
get_filename_component(BENEFACTOR_AMIGAPORT_DIR "${BENEFACTOR_AMIGAPORT_DIR}"
    ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

if(IS_DIRECTORY "${BENEFACTOR_AMIGAPORT_DIR}")
    set(BENEFACTOR_PRODUCT_BLOCKER
        "the Benefactor runtime adapter to shared/amigaport is not implemented")
else()
    set(BENEFACTOR_PRODUCT_BLOCKER
        "shared/amigaport is missing at ${BENEFACTOR_AMIGAPORT_DIR}")
endif()

add_custom_target(benefactor_product
    COMMAND "${CMAKE_COMMAND}" -E echo
            "Benefactor gameplay product unavailable: ${BENEFACTOR_PRODUCT_BLOCKER}"
    COMMAND "${CMAKE_COMMAND}" -E false
    VERBATIM
)

enable_testing()
find_program(BENEFACTOR_UV_EXECUTABLE uv)
if(BENEFACTOR_UV_EXECUTABLE)
    add_test(
        NAME benefactor_source_policy
        COMMAND "${BENEFACTOR_UV_EXECUTABLE}" run --frozen python -m tools.verify
    )
    set_tests_properties(benefactor_source_policy PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
