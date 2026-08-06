function(sf_add_unit_test target source)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "LIBRARIES")

    add_executable(${target} ${source})
    target_include_directories(${target} PRIVATE include)
    target_compile_features(${target} PRIVATE cxx_std_20)
    if(arg_LIBRARIES)
        target_link_libraries(${target} PRIVATE ${arg_LIBRARIES})
    endif()
    sf_set_project_warnings(${target})
    set_target_properties(${target} PROPERTIES FOLDER "Tests")
    add_test(NAME ${target} COMMAND ${target})
endfunction()

if(SF_BUILD_TESTS)
    enable_testing()

    sf_add_unit_test(sf_tests tests/test_main.cpp LIBRARIES sf::game)
    sf_add_unit_test(sf_pause_menu_tests tests/pause_menu_tests.cpp
        LIBRARIES sf::game)
    sf_add_unit_test(sf_campaign_tests tests/campaign_tests.cpp
        LIBRARIES sf::game)
    sf_add_unit_test(sf_combat_ai_tests tests/combat_ai_tests.cpp
        LIBRARIES sf::game)
    sf_add_unit_test(sf_agent_mission_hud_tests tests/agent_mission_hud_tests.cpp
        LIBRARIES sf::game)
    sf_add_unit_test(sf_agent_late_mission_rules_tests
        tests/agent_late_mission_rules_tests.cpp)
    sf_add_unit_test(sf_dynamic_lighting_tests tests/dynamic_lighting_tests.cpp
        LIBRARIES sf::game)
    sf_add_unit_test(sf_player_input_tests tests/player_input_tests.cpp
        LIBRARIES sf::platform_input)
    sf_add_unit_test(sf_audio_output_policy_tests
        tests/audio_output_policy_tests.cpp)
    sf_add_unit_test(sf_r3000_runtime_tests tests/r3000_runtime_tests.cpp
        LIBRARIES sf::game)
    sf_add_unit_test(sf_legacy_presentation_bridge_tests
        tests/legacy_presentation_bridge_tests.cpp LIBRARIES sf::game)
    sf_add_unit_test(sf_raw_sector_source_tests
        tests/raw_sector_source_tests.cpp LIBRARIES sf::disc)
    sf_add_unit_test(sf_xa_decoder_tests tests/xa_decoder_tests.cpp
        LIBRARIES sf::psx)
    sf_add_unit_test(sf_spu_tests tests/spu_tests.cpp LIBRARIES sf::psx)
    sf_add_unit_test(sf_spu_machine_tests tests/spu_machine_tests.cpp
        LIBRARIES sf::psx)
    sf_add_unit_test(sf_cd_xa_routing_tests tests/cd_xa_routing_tests.cpp
        LIBRARIES sf::psx)
    sf_add_unit_test(sf_stable_frame_vector_tests
        tests/stable_frame_vector_tests.cpp)
    sf_add_unit_test(sf_file_io_tests tests/file_io_tests.cpp
        LIBRARIES sf::core)
    sf_add_unit_test(sf_retail_pause_map_tests
        tests/retail_pause_map_tests.cpp LIBRARIES sf::game)

    add_test(NAME sf_architecture_check
        COMMAND "${CMAKE_COMMAND}"
            "-DSF_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_SOURCE_DIR}/cmake/CheckArchitecture.cmake")
    set_tests_properties(sf_architecture_check PROPERTIES
        LABELS "architecture;static")

    sf_register_supported_rom_tests()

    if(SF_ENABLE_PSYCROSS)
        sf_add_unit_test(sf_aspect_ratio_tests tests/aspect_ratio_tests.cpp
            LIBRARIES psycross_static)
        sf_add_unit_test(sf_pgxp_precision_tests tests/pgxp_precision_tests.cpp
            LIBRARIES psycross_static)
        sf_add_unit_test(sf_psycross_vram_tests
            tests/psycross_vram_tests.cpp LIBRARIES sf::psycross_backend)
        target_include_directories(sf_psycross_vram_tests
            PRIVATE "${CMAKE_SOURCE_DIR}/src/platform")
        if(SF_SUPPORTED_ROM_CUE)
            add_test(NAME sf_g4_fmv_rom
                COMMAND sf_movie_probe "${SF_SUPPORTED_ROM_CUE}")
            set_tests_properties(sf_g4_fmv_rom PROPERTIES
                LABELS "rom;g4;g4.3;fmv"
                TIMEOUT 300)
        endif()
    endif()
endif()
