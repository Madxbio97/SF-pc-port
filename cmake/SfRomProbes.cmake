function(sf_add_game_probe target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE sf::game)
    sf_set_project_warnings(${target})
    set_target_properties(${target} PROPERTIES FOLDER "Diagnostics/ROM")
endfunction()

if(SF_BUILD_ROM_PROBES)
    sf_add_game_probe(sf_h3_audio_probe apps/sf_h3_audio_probe.cpp)
    sf_add_game_probe(sf_h4_probe apps/sf_h4_probe.cpp)
    sf_add_game_probe(sf_h5_catalog_probe apps/sf_h5_catalog_probe.cpp)
    sf_add_game_probe(sf_h5_campaign_probe apps/sf_h5_campaign_probe.cpp)
    sf_add_game_probe(sf_h5_bootability_probe apps/sf_h5_bootability_probe.cpp)
    sf_add_game_probe(sf_g2_active_probe apps/sf_g2_active_probe.cpp)
    sf_add_game_probe(sf_g2_script_probe apps/sf_g2_script_probe.cpp)
    sf_add_game_probe(sf_g3_gameplay_probe apps/sf_g3_gameplay_probe.cpp)
    sf_add_game_probe(sf_g3_retail_spawn_probe apps/sf_g3_retail_spawn_probe.cpp)
    sf_add_game_probe(sf_agent_mission_weapon_probe
        apps/sf_agent_mission_weapon_probe.cpp)
    sf_add_game_probe(sf_agent_kravitch_controller_probe
        apps/sf_agent_kravitch_controller_probe.cpp)
    sf_add_game_probe(sf_retail_grenade_probe apps/sf_retail_grenade_probe.cpp)
    sf_add_game_probe(sf_g3_special_actor_probe apps/sf_g3_special_actor_probe.cpp)
    sf_add_game_probe(sf_g3_ai_combat_probe apps/sf_g3_ai_combat_probe.cpp)
    sf_add_game_probe(sf_g3_stealth_probe apps/sf_g3_stealth_probe.cpp)
    sf_add_game_probe(sf_g3_overlay_outcome_probe apps/sf_g3_overlay_outcome_probe.cpp)
    sf_add_game_probe(sf_g4_campaign_transition_probe
        apps/sf_g4_campaign_transition_probe.cpp)
    sf_add_game_probe(sf_retail_environment_probe
        apps/sf_retail_environment_probe.cpp)
    sf_add_game_probe(sf_retail_prop_state_probe
        apps/sf_retail_prop_state_probe.cpp)
endif()

function(sf_add_supported_rom_test name target labels timeout)
    add_test(NAME ${name} COMMAND ${target} "${SF_SUPPORTED_ROM_CUE}")
    set_tests_properties(${name} PROPERTIES
        LABELS "${labels}"
        TIMEOUT ${timeout})
endfunction()

function(sf_register_supported_rom_tests)
    if(NOT SF_BUILD_ROM_PROBES OR NOT SF_SUPPORTED_ROM_CUE)
        return()
    endif()

    sf_add_supported_rom_test(sf_h5_bootability_rom sf_h5_bootability_probe
        "rom;h5;g1;g2.1" 300)
    sf_add_supported_rom_test(sf_h5_catalog_rom sf_h5_catalog_probe
        "rom;h5;g4;g4.3;catalog" 300)
    sf_add_supported_rom_test(sf_h5_campaign_rom sf_h5_campaign_probe
        "rom;h5;g4;g4.3;campaign" 600)
    sf_add_supported_rom_test(sf_g2_active_rom sf_g2_active_probe
        "rom;g2;g2.2" 600)
    sf_add_supported_rom_test(sf_g2_script_rom sf_g2_script_probe
        "rom;g2;g2.3" 600)
    sf_add_supported_rom_test(sf_g3_gameplay_rom sf_g3_gameplay_probe
        "rom;g3;g3.1" 900)
    sf_add_supported_rom_test(sf_g3_retail_spawn_rom sf_g3_retail_spawn_probe
        "rom;g3;g3.2" 300)
    sf_add_supported_rom_test(sf_agent_mission_weapon_rom
        sf_agent_mission_weapon_probe "rom;agent;gameplay;weapon" 300)
    sf_add_supported_rom_test(sf_agent_kravitch_controller_rom
        sf_agent_kravitch_controller_probe
        "rom;agent;gameplay;weapon;controller;ai;movement;shotgun" 300)
    sf_add_supported_rom_test(sf_g3_special_actor_rom sf_g3_special_actor_probe
        "rom;g3;g3.3" 300)
    sf_add_supported_rom_test(sf_g3_ai_combat_rom sf_g3_ai_combat_probe
        "rom;g3;g3.4" 900)
    sf_add_supported_rom_test(sf_g3_stealth_rom sf_g3_stealth_probe
        "rom;g3;g3.4;stealth" 900)
    sf_add_supported_rom_test(sf_g3_overlay_outcome_rom
        sf_g3_overlay_outcome_probe "rom;g3;g3.5" 300)
    sf_add_supported_rom_test(sf_g4_campaign_transition_rom
        sf_g4_campaign_transition_probe "rom;g4;g4.3;campaign;checkpoint" 900)
    sf_add_supported_rom_test(sf_retail_environment_rom
        sf_retail_environment_probe "rom;rendering;environment;effects" 900)
    sf_add_supported_rom_test(sf_retail_grenade_rom sf_retail_grenade_probe
        "rom;gameplay;rendering;grenade" 300)
endfunction()
