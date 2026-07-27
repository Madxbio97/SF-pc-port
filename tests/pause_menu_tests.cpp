#include "sf/game/localization.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/pause_menu.hpp"
#include "sf/game/pause_menu_data.hpp"
#include "sf/game/retail_cheats.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using sf::game::PauseAcdLayout;
using sf::game::PauseMenu;
using sf::game::PausePanelRole;
using sf::game::PauseRect;
using sf::game::PauseRenderKind;
using sf::game::PauseScreen;

void require(bool condition, const std::source_location location =
                                 std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error{"Pause menu test failed at line " +
                             std::to_string(location.line())};
  }
}

constexpr bool sameRect(const PauseRect &left, const PauseRect &right) {
  return left.x == right.x && left.y == right.y && left.width == right.width &&
         left.height == right.height;
}

constexpr bool contains(const PauseRect &outer, const PauseRect &inner) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.width >= 0 &&
         inner.height >= 0 && inner.x + inner.width <= outer.x + outer.width &&
         inner.y + inner.height <= outer.y + outer.height;
}

PauseMenu makeMenu() {
  sf::game::PauseMenuData data;
  data.mission.mission_name = "Georgia Street";
  data.mission.date_time = "08/23 22:45";
  data.mission.location = "Washington DC";
  data.mission.briefing_pages = {"Page one", "Page two"};
  data.mission.objectives.push_back({1, "First objective"});
  data.mission.objectives.push_back(
      {2, "Completed objective", sf::game::MissionEntryState::completed});
  data.mission.parameters.push_back({3, "Do not fail"});
  data.mission.map.layer_assets = {"MAP1.TIM", "MAP2.TIM"};
  data.mission.map.current_location = "Georgia Street";
  data.mission.map.current_layer = 0U;
  data.mission.map.markers.push_back(
      {sf::game::MapMarkerKind::player, 0.5F, 0.5F, 0.0F, true, 0U, {}, 0U});
  data.mission.map.markers.push_back({sf::game::MapMarkerKind::objective, 0.25F,
                                      0.75F, 0.0F, true, 1U, "OBJ 1", 0U});
  data.weapons.push_back({
      7,
      "SILENCED 9MM",
      "GLOKSIL.TIM",
      "Side-arm",
      15,
      90,
      3,
      2,
      5,
      true,
      false,
      true,
      "15",
      "90",
  });
  data.weapons.push_back({
      8,
      "PK-102",
      "PK102.TIM",
      "Submachine gun",
      30,
      150,
      5,
      3,
      4,
      true,
      true,
      true,
      "30",
      "150",
  });
  data.current_mission = 0U;
  data.missions = {
      {0U, "1. Georgia Street"},
      {1U, "2. Destroyed Subway"},
  };
  return PauseMenu{std::move(data)};
}

void settleTransition(PauseMenu &menu) {
  while (menu.transition().input_delay > 0) {
    require(!menu.update({}));
  }
}

void openRootSection(PauseMenu &menu, std::size_t section) {
  while (menu.sectionSelection() != section) {
    require(!menu.update({.next = true}));
    settleTransition(menu);
  }
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
}

void moveNext(PauseMenu &menu, std::size_t count = 1) {
  for (std::size_t index = 0; index < count; ++index) {
    require(!menu.update({.next = true}));
    settleTransition(menu);
  }
}

void requireRetailComposition(const PauseMenu &menu) {
  const auto commands = menu.buildRenderCommands();
  std::array<bool, 4> panels{};
  std::array<bool, 6> sections{};
  std::size_t selected_sections{};
  bool has_information{};
  bool has_hint{};

  for (const auto &command : commands) {
    require(contains(PauseAcdLayout::canvas, command.bounds));
    if (command.kind != PauseRenderKind::dim_background) {
      require(command.bounds.width <= PauseAcdLayout::left_grid.width);
    }

    switch (command.panel) {
    case PausePanelRole::none:
      require(command.kind == PauseRenderKind::dim_background);
      require(sameRect(command.bounds, PauseAcdLayout::canvas));
      break;
    case PausePanelRole::left_content:
      if (command.kind == PauseRenderKind::panel) {
        require(sameRect(command.bounds, PauseAcdLayout::left_grid));
        panels[0] = true;
      } else {
        require(contains(PauseAcdLayout::left_grid, command.bounds));
      }
      break;
    case PausePanelRole::right_information:
      if (command.kind == PauseRenderKind::panel) {
        require(sameRect(command.bounds, PauseAcdLayout::information_grid));
        panels[1] = true;
      } else {
        require(contains(PauseAcdLayout::information_content, command.bounds));
        has_information = true;
      }
      break;
    case PausePanelRole::right_sections:
      if (command.kind == PauseRenderKind::panel) {
        require(sameRect(command.bounds, PauseAcdLayout::section_menu));
        panels[2] = true;
        break;
      }
      require(command.kind == PauseRenderKind::menu_item);
      require(command.id < sections.size());
      require(sameRect(command.bounds,
                       PauseAcdLayout::sectionSelection(command.id)));
      require(command.alignment == sf::game::PauseTextAlignment::center);
      sections[command.id] = true;
      selected_sections += command.selected ? 1U : 0U;
      break;
    case PausePanelRole::hint:
      require(sameRect(command.bounds, PauseAcdLayout::hint));
      if (command.kind == PauseRenderKind::panel) {
        panels[3] = true;
      } else {
        require(command.kind == PauseRenderKind::button_hint);
        require(command.alignment == sf::game::PauseTextAlignment::center);
        has_hint = true;
      }
      break;
    }
  }

  require(std::ranges::all_of(panels, [](bool present) { return present; }));
  require(std::ranges::all_of(sections, [](bool present) { return present; }));
  require(selected_sections == 1);
  require(has_information && has_hint);
}

void testRetailLayoutContract() {
  require(sameRect(PauseAcdLayout::canvas, {0, 0, 384, 240}));
  require(sameRect(PauseAcdLayout::left_grid, {36, 25, 190, 184}));
  require(sameRect(PauseAcdLayout::left_content, {52, 32, 165, 155}));
  require(sameRect(PauseAcdLayout::information_grid, {233, 35, 116, 81}));
  require(sameRect(PauseAcdLayout::information_content, {236, 40, 109, 70}));
  require(sameRect(PauseAcdLayout::section_menu, {252, 134, 100, 100}));
  require(sameRect(PauseAcdLayout::hint, {52, 200, 165, 10}));
  require(sameRect(PauseAcdLayout::map_image, {76, 28, 118, 174}));
  require(sameRect(PauseAcdLayout::expanded_content, {12, 8, 360, 210}));
  require(sameRect(PauseAcdLayout::expanded_map_panel, {8, 8, 256, 210}));
  require(sameRect(PauseAcdLayout::expanded_map_image, {14, 14, 244, 198}));
  require(
      sameRect(PauseAcdLayout::expanded_information_panel, {268, 8, 108, 210}));
  require(sameRect(PauseAcdLayout::expanded_information_content,
                   {274, 16, 96, 194}));
  require(sameRect(PauseAcdLayout::expanded_weapon_image_panel,
                   {18, 34, 168, 170}));
  require(sameRect(PauseAcdLayout::expanded_weapon_information_panel,
                   {192, 34, 174, 170}));
  require(sameRect(PauseAcdLayout::expanded_hint, {20, 224, 344, 10}));
  require(sameRect(PauseAcdLayout::sectionSelection(5), {252, 212, 100, 11}));
}

void testOriginalRootComposition() {
  auto menu = makeMenu();
  requireRetailComposition(menu);
  const auto commands = menu.buildRenderCommands();
  const auto map = std::ranges::find_if(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::asset &&
           command.asset == "MAP1.TIM";
  });
  require(map != commands.end());
  require(sameRect(map->bounds, PauseAcdLayout::map_image));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::button_hint &&
           command.text == "%x select   %t resume";
  }));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::map_marker &&
           command.id ==
               static_cast<std::uint32_t>(sf::game::MapMarkerKind::objective) &&
           command.color == sf::game::PauseColorRole::map_highlight &&
           command.selected;
  }));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::map_marker &&
           command.id ==
               static_cast<std::uint32_t>(sf::game::MapMarkerKind::player) &&
           command.color == sf::game::PauseColorRole::map_highlight &&
           command.selected;
  }));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text && command.id == 1U &&
           command.text == "- First objective" && command.selected &&
           command.color == sf::game::PauseColorRole::map_highlight;
  }));

  moveNext(menu);
  auto list_commands = menu.buildRenderCommands();
  require(std::ranges::any_of(list_commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.text.find("- First objective") != std::string::npos;
  }));
  moveNext(menu);
  list_commands = menu.buildRenderCommands();
  require(std::ranges::any_of(list_commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.text.find("- Do not fail") != std::string::npos;
  }));
}

void testRetailMapInformationWrapsIntoTheWindow() {
  auto menu = makeMenu();
  auto data = menu.data();
  data.mission.objectives = {
      {1U, "Eliminate Kravitch and destroy comm. array"},
      {2U, "Turn off power to terminal security doors"},
      {3U, "Eliminate Rhoemer"},
  };
  data.mission.map.markers = {
      {sf::game::MapMarkerKind::player, 0.5F, 0.5F, 0.0F, true, 0U, {}, 0U},
      {sf::game::MapMarkerKind::objective, 0.7F, 0.8F, 0.0F, true, 1U, {}, 0U},
      {sf::game::MapMarkerKind::objective, 0.4F, 0.2F, 0.0F, true, 2U, {}, 0U},
      {sf::game::MapMarkerKind::objective, 0.2F, 0.6F, 0.0F, true, 3U, {}, 0U},
  };
  menu.reset(std::move(data));

  const auto commands = menu.buildRenderCommands();
  std::vector<const sf::game::PauseRenderCommand *> information;
  for (const auto &command : commands) {
    if (command.kind == PauseRenderKind::text &&
        command.panel == PausePanelRole::right_information) {
      information.push_back(&command);
      require(contains(PauseAcdLayout::information_content, command.bounds));
      require(command.selected &&
              command.color == sf::game::PauseColorRole::map_highlight);
    }
  }
  require(information.size() == 10U);
  require(information.front()->text == "- Current Location");
  require(std::ranges::none_of(information, [](const auto *command) {
    return command->text.find("Georgia Street") != std::string::npos;
  }));
  require(information.back()->text == "Rhoemer");
  require(information.back()->bounds.y + information.back()->bounds.height ==
          PauseAcdLayout::information_content.y +
              PauseAcdLayout::information_content.height);
}

void testRussianMapObjectivesFitInsideInformationPanel() {
  sf::game::setGameLanguage(sf::game::GameLanguage::russian_vit);
  auto menu = makeMenu();
  auto data = menu.data();
  data.mission.objectives = {
      {1U, "Avoid damaging viral delivery systems or explosive bombs"},
      {2U, "Protect CBDC bomb squad"},
      {3U, "Eliminate Kravitch and destroy comm. array"},
      {4U, "Turn off power to terminal security doors"},
  };
  data.mission.map.markers = {
      {sf::game::MapMarkerKind::player, 0.5F, 0.5F, 0.0F, true, 0U, {}, 0U},
      {sf::game::MapMarkerKind::objective, 0.7F, 0.8F, 0.0F, true, 1U, {}, 0U},
      {sf::game::MapMarkerKind::objective, 0.4F, 0.2F, 0.0F, true, 2U, {}, 0U},
      {sf::game::MapMarkerKind::objective, 0.2F, 0.6F, 0.0F, true, 3U, {}, 0U},
      {sf::game::MapMarkerKind::objective, 0.8F, 0.4F, 0.0F, true, 4U, {}, 0U},
  };
  menu.reset(std::move(data));

  std::array<bool, 5U> visible_entries{};
  auto rendered_lines = std::size_t{};
  for (const auto &command : menu.buildRenderCommands()) {
    if (command.kind != PauseRenderKind::text ||
        command.panel != PausePanelRole::right_information) {
      continue;
    }
    require(contains(PauseAcdLayout::information_content, command.bounds));
    require(command.id < visible_entries.size());
    require(command.text.find('?') == std::string::npos);
    visible_entries[command.id] = true;
    ++rendered_lines;
  }
  require(rendered_lines <= 10U);
  require(std::ranges::all_of(visible_entries,
                              [](bool visible) { return visible; }));
  sf::game::setGameLanguage(sf::game::GameLanguage::english);
}

void testOptionsPreviewIsImmediatelyComplete() {
  auto menu = makeMenu();
  moveNext(menu, 5);
  require(menu.screen() == PauseScreen::root);
  const auto commands = menu.buildRenderCommands();
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.panel == PausePanelRole::left_content &&
           command.text ==
               "Sound\nController\nGame Brightness\nScreen Centering";
  }));
  require(std::ranges::none_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::menu_item &&
           command.panel == PausePanelRole::left_content;
  }));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.panel == PausePanelRole::right_information &&
           command.text.starts_with("Configuration\nBrightness:");
  }));
}

void testRetailControllerBindingsAreVisible() {
  auto menu = makeMenu();
  openRootSection(menu, 5);
  moveNext(menu, 7);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  moveNext(menu, 1);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);

  constexpr std::array labels{
      "Change Weapon: SELECT", "Shoot: SQUARE",         "Kneel: CROSS",
      "Roll/Zoom Out: CIRCLE", "Step Right: R2",        "Step Left: L2",
      "Target Lock: R1",       "Use/Zoom In: TRIANGLE", "Aim: L1",
  };
  const auto commands = menu.buildRenderCommands();
  for (const auto label : labels) {
    require(std::ranges::any_of(commands, [label](const auto &command) {
      return command.kind == PauseRenderKind::menu_item &&
             command.panel == PausePanelRole::left_content &&
             command.text == label;
    }));
  }
}

void testRetailOptionsAndControllerOrder() {
  auto menu = makeMenu();
  openRootSection(menu, 5);
  constexpr std::array option_labels{
      "Restart Mission",
      "Restart At Last Checkpoint",
      "Quit Game",
      "Select Mission",
      "Sound",
      "Game Brightness",
      "Screen Centering",
      "Controller",
      "Cheats",
  };
  const auto options = menu.buildRenderCommands();
  for (std::size_t index = 0; index < option_labels.size(); ++index) {
    require(std::ranges::any_of(
        options, [index, &option_labels](const auto &command) {
          return command.kind == PauseRenderKind::menu_item &&
                 command.panel == PausePanelRole::left_content &&
                 command.id == index && command.text == option_labels[index] &&
                 contains(PauseAcdLayout::left_content, command.bounds);
        }));
  }

  moveNext(menu, 7);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  constexpr std::array controller_prefixes{
      "Preset config: ", "Controller Configuration:",
      "Invert Aim: ",    "Vibration: ",
      "Reset",           "Accept",
      "Cancel",
  };
  const auto controller = menu.buildRenderCommands();
  for (std::size_t index = 0; index < controller_prefixes.size(); ++index) {
    require(std::ranges::any_of(
        controller, [index, &controller_prefixes](const auto &command) {
          return command.kind == PauseRenderKind::menu_item &&
                 command.panel == PausePanelRole::left_content &&
                 command.id == index &&
                 command.text.starts_with(controller_prefixes[index]);
        }));
  }
}

void testRetailCheatsMenu() {
  auto menu = makeMenu();
  openRootSection(menu, 5);
  moveNext(menu, 8);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  require(menu.screen() == PauseScreen::cheats);
  requireRetailComposition(menu);

  constexpr std::array labels{
      "All Weapons + Infinite Ammo",
      "Hard Mode",
      "One-Shot Kills",
      "Stage Select",
      "Weak Enemies",
      "Movie Theater",
  };
  const auto commands = menu.buildRenderCommands();
  for (std::size_t index = 0; index < labels.size(); ++index) {
    require(
        std::ranges::any_of(commands, [index, &labels](const auto &command) {
          return command.kind == PauseRenderKind::menu_item &&
                 command.panel == PausePanelRole::left_content &&
                 command.id == index && command.text == labels[index] &&
                 contains(PauseAcdLayout::left_content, command.bounds);
        }));
  }

  const auto enabled = menu.update({.confirm = true});
  require(enabled.type == sf::game::PauseCommandType::set_retail_cheat);
  require(enabled.subject ==
              static_cast<std::uint32_t>(sf::game::RetailCheat::all_weapons) &&
          enabled.value == 1 &&
          menu.data().cheats.enabled(sf::game::RetailCheat::all_weapons));
  const auto disabled = menu.update({.left = true});
  require(disabled.type == sf::game::PauseCommandType::set_retail_cheat &&
          disabled.value == 0 &&
          !menu.data().cheats.enabled(sf::game::RetailCheat::all_weapons));
}

void testEverySectionKeepsAcdComposition() {
  constexpr std::array expected{
      PauseScreen::map,      PauseScreen::root,    PauseScreen::root,
      PauseScreen::briefing, PauseScreen::weapons, PauseScreen::options,
  };
  for (std::size_t section = 0; section < expected.size(); ++section) {
    auto menu = makeMenu();
    openRootSection(menu, section);
    require(menu.screen() == expected[section]);
    require(menu.sectionSelection() == section);
    requireRetailComposition(menu);
  }
}

void testNestedScreensStayInsideLeftPanel() {
  auto menu = makeMenu();
  openRootSection(menu, 5);
  require(menu.screen() == PauseScreen::options);
  requireRetailComposition(menu);

  moveNext(menu, 4);
  require(!menu.update({.confirm = true}));
  require(menu.screen() == PauseScreen::sound);
  requireRetailComposition(menu);
  settleTransition(menu);
  require(menu.update({.cancel = true}).type ==
          sf::game::PauseCommandType::commit_settings);
  settleTransition(menu);

  moveNext(menu, 3);
  require(!menu.update({.confirm = true}));
  require(menu.screen() == PauseScreen::controller);
  requireRetailComposition(menu);
  settleTransition(menu);
  moveNext(menu, 1);
  require(!menu.update({.confirm = true}));
  require(menu.screen() == PauseScreen::controller_bindings);
  requireRetailComposition(menu);
}

void testAdjustmentScreensStayInsideLeftPanel() {
  auto menu = makeMenu();
  openRootSection(menu, 5);
  moveNext(menu, 5);
  require(!menu.update({.confirm = true}));
  require(menu.screen() == PauseScreen::brightness);
  requireRetailComposition(menu);

  menu = makeMenu();
  openRootSection(menu, 5);
  moveNext(menu, 6);
  require(!menu.update({.confirm = true}));
  require(menu.screen() == PauseScreen::screen_centering);
  requireRetailComposition(menu);
}

void testRetailTransitions() {
  auto menu = makeMenu();
  require(!menu.transition().active());
  require(!menu.update({.next = true}));
  const auto selection = menu.transition();
  require(selection.kind == sf::game::PauseTransitionKind::section_selection);
  require(selection.from_selection == 0 && selection.to_selection == 1);
  require(selection.frame == 0 && selection.duration == 4 &&
          selection.input_delay == 10);

  for (std::uint8_t frame = 1; frame <= 4; ++frame) {
    require(!menu.update({}));
    require(menu.transition().frame == frame);
  }
  require(!menu.transition().active());
  require(menu.transition().input_delay == 6);

  settleTransition(menu);
  require(!menu.update({.confirm = true}));
  require(menu.screen() == PauseScreen::root &&
          menu.transition().kind == sf::game::PauseTransitionKind::none);

  menu = makeMenu();
  openRootSection(menu, 5);
  require(menu.screen() == PauseScreen::options);
  require(!menu.update({.next = true}));
  const auto item = menu.transition();
  require(item.kind == sf::game::PauseTransitionKind::item_selection);
  require(item.from_selection == 0 && item.to_selection == 1);
  require(item.frame == 0 && item.duration == 4 && item.input_delay == 10);
}

void testRetailMissionSelectCheat() {
  auto progress = makeMenu().data();
  progress.current_mission = 1U;
  progress.missions.push_back({2U, "3. Main Subway Line"});

  auto menu = PauseMenu{progress};
  openRootSection(menu, 5);
  moveNext(menu, 3);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  require(menu.screen() == PauseScreen::mission_select);
  require(menu.selection() == 1U);
  require(!menu.update({.previous = true}));
  settleTransition(menu);
  const auto previous = menu.update({.confirm = true});
  require(previous.type == sf::game::PauseCommandType::select_mission);
  require(previous.subject == 0U);

  menu = PauseMenu{progress};
  openRootSection(menu, 5);
  moveNext(menu, 3);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  moveNext(menu);
  require(!menu.update({.confirm = true}));
  require(menu.screen() == PauseScreen::notification);
  require(!menu.missionSelectUnlocked());

  menu = PauseMenu{progress};
  menu.unlockMissionSelect();
  require(menu.missionSelectUnlocked());
  openRootSection(menu, 5);
  moveNext(menu, 3);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  moveNext(menu);
  const auto selected = menu.update({.confirm = true});
  require(selected.type == sf::game::PauseCommandType::select_mission);
  require(selected.subject == 2U);

  progress.current_mission = 0U;
  progress.maximum_unlocked_mission = 2U;
  menu = PauseMenu{progress};
  openRootSection(menu, 5);
  moveNext(menu, 3);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  require(menu.selection() == 0U);
  moveNext(menu, 2);
  const auto replay = menu.update({.confirm = true});
  require(replay.type == sf::game::PauseCommandType::select_mission);
  require(replay.subject == 2U);
}

void testRootAndBriefing() {
  auto menu = makeMenu();
  openRootSection(menu, 3);
  require(menu.screen() == PauseScreen::briefing);
  const auto initial_commands = menu.buildRenderCommands();
  require(std::ranges::any_of(initial_commands, [](const auto &command) {
    return command.text == "08/23 22:45\nWashington DC" &&
           command.panel == PausePanelRole::left_content &&
           command.bounds.height >= 20;
  }));
  require(std::ranges::any_of(initial_commands, [](const auto &command) {
    return command.text == "Briefing\nGeorgia Street\nWashington DC" &&
           command.panel == PausePanelRole::right_information;
  }));
  require(!menu.update({.right = true}));
  settleTransition(menu);
  const auto commands = menu.buildRenderCommands();
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.text == "Page two" &&
           command.panel == PausePanelRole::left_content;
  }));
  require(!menu.update({.cancel = true}));
  require(menu.screen() == PauseScreen::root);
  settleTransition(menu);
  require(menu.update({.cancel = true}).type ==
          sf::game::PauseCommandType::resume);
}

void testBriefingPagination() {
  const auto pages = sf::game::paginatePauseBriefing(
      "AGENCY DIRECTIVE:",
      "Your targets are Erich Rhoemer, Pavel Kravitch, Mara Aramov and Anton "
      "Girdeux. Locate the communications array and protect the CBDC team. "
      "Avoid civilian casualties and review the objectives for updates.");
  require(!pages.empty());
  require(pages.front().starts_with("AGENCY DIRECTIVE:\nYour targets"));
  require(std::ranges::all_of(pages, [](const auto &page) {
    return static_cast<std::size_t>(
               std::count(page.begin(), page.end(), '\n')) < 9U;
  }));

  std::string long_text;
  for (auto index = 0; index < 80; ++index) {
    long_text += "complete objective ";
  }
  const auto multiple_pages =
      sf::game::paginatePauseBriefing("FIELD REPORT:", long_text);
  require(multiple_pages.size() > 1U);
  require(multiple_pages.front().find("FIELD REPORT:") != std::string::npos);
  require(multiple_pages[1].find("complete") != std::string::npos);
}

void testWeaponEquipAndGuard() {
  auto menu = makeMenu();
  openRootSection(menu, 4);
  moveNext(menu);
  require(!menu.update({.confirm = true}));
  require(menu.expanded());
  const auto equip = menu.update({.confirm = true});
  require(equip.type == sf::game::PauseCommandType::equip_weapon);
  require(equip.subject == 7);
  require(!menu.data().weapons[0].equipped);
  require(menu.data().weapons[1].equipped);
  menu.resolveWeaponEquip(equip.subject, true);
  require(menu.data().weapons[0].equipped);
  require(!menu.data().weapons[1].equipped);

  require(!menu.update({.cancel = true}));
  require(!menu.expanded());
  moveNext(menu);
  require(!menu.update({.confirm = true}));
  require(menu.expanded());
  const auto rejected = menu.update({.confirm = true});
  require(rejected.type == sf::game::PauseCommandType::equip_weapon);
  require(rejected.subject == 8);
  menu.resolveWeaponEquip(rejected.subject, false);
  require(menu.data().weapons[0].equipped);
  require(!menu.data().weapons[1].equipped);
  require(menu.screen() == PauseScreen::notification);

  auto blocked_data = menu.data();
  blocked_data.weapons[0].equip_allowed = false;
  menu.reset(std::move(blocked_data));
  openRootSection(menu, 4);
  require(!menu.update({.confirm = true}));
  require(menu.expanded());
  require(!menu.update({.confirm = true}));
  require(menu.screen() == PauseScreen::notification);
  requireRetailComposition(menu);
}

void testMapAndWeaponExpandedViews() {
  auto menu = makeMenu();
  openRootSection(menu, 0);
  require(menu.screen() == PauseScreen::map && !menu.expanded());
  require(!menu.update({.confirm = true}));
  require(menu.expanded());
  auto commands = menu.buildRenderCommands();
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::asset &&
           sameRect(command.bounds, PauseAcdLayout::expanded_map_image);
  }));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::map_marker &&
           command.id ==
               static_cast<std::uint32_t>(sf::game::MapMarkerKind::objective) &&
           command.maximum == 1 &&
           command.color == sf::game::PauseColorRole::map_highlight;
  }));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::panel &&
           sameRect(command.bounds, PauseAcdLayout::expanded_map_panel);
  }));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           contains(PauseAcdLayout::expanded_information_panel, command.bounds);
  }));
  require(std::ranges::none_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::menu_item &&
           command.panel == PausePanelRole::right_sections;
  }));
  require(!menu.update({.right = true}));
  commands = menu.buildRenderCommands();
  require(std::ranges::none_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::map_marker;
  }));
  require(!menu.update({.cancel = true}));
  require(menu.screen() == PauseScreen::map && !menu.expanded());

  menu = makeMenu();
  openRootSection(menu, 4);
  commands = menu.buildRenderCommands();
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::weapon_icon &&
           command.panel == PausePanelRole::right_information &&
           contains(PauseAcdLayout::information_content, command.bounds) &&
           command.id == 8U;
  }));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::menu_item &&
           command.panel == PausePanelRole::left_content &&
           command.text == "PK-102" && command.selected;
  }));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.panel == PausePanelRole::right_information &&
           command.text == "Ammo: 30/150";
  }));
  require(std::ranges::none_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.text.find("Submachine gun") != std::string::npos;
  }));
  require(!menu.update({.confirm = true}));
  require(menu.expanded());
  commands = menu.buildRenderCommands();
  requireRetailComposition(menu);
  require(std::ranges::none_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::panel &&
           (sameRect(command.bounds,
                     PauseAcdLayout::expanded_weapon_image_panel) ||
            sameRect(command.bounds,
                     PauseAcdLayout::expanded_weapon_information_panel));
  }));
  for (const auto label : {"Fire Rate", "Damage", "Clip Size", "Max Rounds"}) {
    require(std::ranges::any_of(commands, [label](const auto &command) {
      return command.kind == PauseRenderKind::text &&
             command.panel == PausePanelRole::left_content &&
             command.text == label;
    }));
  }
  for (const auto value : {"30", "150"}) {
    require(std::ranges::any_of(commands, [value](const auto &command) {
      return command.kind == PauseRenderKind::text &&
             command.panel == PausePanelRole::left_content &&
             command.text == value &&
             command.alignment == sf::game::PauseTextAlignment::right;
    }));
  }
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.text.find("Submachine gun") != std::string::npos &&
           command.panel == PausePanelRole::left_content;
  }));
  require(std::ranges::none_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::slider;
  }));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::menu_item &&
           command.panel == PausePanelRole::right_sections;
  }));
  require(!menu.update({.right = true}));
  require(menu.expanded() && menu.selection() == 1U);
  require(!menu.update({.cancel = true}));
  require(!menu.expanded());
  require(!menu.update({.confirm = true}));
  commands = menu.buildRenderCommands();
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.text.find("Submachine gun") != std::string::npos;
  }));
}

void testOptionsConfirmationAndBinding() {
  auto menu = makeMenu();
  openRootSection(menu, 5);
  require(!menu.update({.confirm = true}));
  require(menu.screen() == PauseScreen::confirmation);
  requireRetailComposition(menu);
  settleTransition(menu);
  require(!menu.update({.previous = true}));
  settleTransition(menu);
  require(menu.update({.confirm = true}).type ==
          sf::game::PauseCommandType::restart_mission);

  menu = makeMenu();
  openRootSection(menu, 5);
  moveNext(menu, 4);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  const auto volume = menu.update({.left = true});
  require(volume.type == sf::game::PauseCommandType::preview_setting);
  require(volume.subject == static_cast<std::uint32_t>(
                                sf::game::PauseSetting::sound_effects_volume) &&
          menu.settings().sound_effects_volume == 95 &&
          menu.settings().voice_volume == 100);
  require(menu.update({.cancel = true}).type ==
          sf::game::PauseCommandType::commit_settings);
  settleTransition(menu);
  moveNext(menu, 3);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  moveNext(menu, 1);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  const auto request = menu.update({.confirm = true});
  require(request.type == sf::game::PauseCommandType::begin_controller_binding);
  const auto binding = menu.completeControllerBinding(42);
  require(binding.type == sf::game::PauseCommandType::preview_setting);
  require(menu.settings().controller_preset ==
          sf::game::ControllerPreset::custom);
}

void testDenseObjectivesStayInsidePanel() {
  auto menu = makeMenu();
  auto data = menu.data();
  data.mission.objectives.clear();
  for (std::uint32_t index = 0; index < 12; ++index) {
    data.mission.objectives.push_back({
        index,
        "Objective " + std::to_string(index),
        index % 2 == 0 ? sf::game::MissionEntryState::active
                       : sf::game::MissionEntryState::completed,
    });
  }
  menu.reset(std::move(data));
  openRootSection(menu, 1);
  requireRetailComposition(menu);

  auto commands = menu.buildRenderCommands();
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.panel == PausePanelRole::left_content && command.id == 0U;
  }));
  require(!menu.update({.right = true}));
  commands = menu.buildRenderCommands();
  require(std::ranges::none_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.panel == PausePanelRole::left_content && command.id == 0U;
  }));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.panel == PausePanelRole::left_content && command.id == 1U;
  }));
  require(!menu.update({.left = true}));
  require(menu.page() == 0U);
}

void testWeaponLabelsAndControllerRecovery() {
  auto menu = makeMenu();
  openRootSection(menu, 4);
  auto weapon_commands = menu.buildRenderCommands();
  require(std::ranges::any_of(weapon_commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.panel == PausePanelRole::right_information &&
           command.text == "Ammo: 30/150";
  }));
  require(!menu.update({.confirm = true}));
  weapon_commands = menu.buildRenderCommands();
  for (const auto label : {"Fire Rate", "Damage", "Clip Size", "Max Rounds"}) {
    require(std::ranges::any_of(weapon_commands, [label](const auto &command) {
      return command.kind == PauseRenderKind::text &&
             command.panel == PausePanelRole::left_content &&
             command.text == label;
    }));
  }

  menu = makeMenu();
  openRootSection(menu, 5);
  moveNext(menu, 7);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  moveNext(menu, 1);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  require(menu.update({.confirm = true}).type ==
          sf::game::PauseCommandType::begin_controller_binding);
  menu.showControllerMissing();
  require(menu.screen() == PauseScreen::notification);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  require(menu.screen() == PauseScreen::controller_bindings);
  moveNext(menu);
  require(menu.selection() == 1);
}

void testRetailControllerPresetsApplyBindings() {
  sf::game::PauseSettings settings;
  sf::game::applyControllerPreset(settings,
                                  sf::game::ControllerPreset::alternate);
  require(settings.controller_preset == sf::game::ControllerPreset::alternate);
  constexpr std::array expected{
      std::pair{sf::game::ControllerAction::change_weapon, 0x0800U},
      std::pair{sf::game::ControllerAction::shoot, 0x2000U},
      std::pair{sf::game::ControllerAction::kneel, 0x0200U},
      std::pair{sf::game::ControllerAction::roll_zoom_out, 0x1000U},
      std::pair{sf::game::ControllerAction::step_right, 0x0100U},
      std::pair{sf::game::ControllerAction::step_left, 0x8000U},
      std::pair{sf::game::ControllerAction::target_lock, 0x4000U},
      std::pair{sf::game::ControllerAction::use_zoom_in, 0x0001U},
      std::pair{sf::game::ControllerAction::aim, 0x0400U},
  };
  for (const auto [action, button] : expected) {
    require(sf::game::controllerButtonForAction(settings, action) == button);
  }

  auto menu = makeMenu();
  openRootSection(menu, 5);
  moveNext(menu, 7);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  const auto preset = menu.update({.right = true});
  require(preset.type == sf::game::PauseCommandType::preview_setting &&
          menu.settings().controller_preset ==
              sf::game::ControllerPreset::alternate &&
          sf::game::controllerButtonForAction(
              menu.settings(), sf::game::ControllerAction::shoot) == 0x2000U);
}

void testRetailControllerTransaction() {
  auto menu = makeMenu();
  openRootSection(menu, 5);
  moveNext(menu, 7);
  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  require(menu.screen() == PauseScreen::controller);

  const auto alternate = menu.update({.right = true});
  require(alternate.type == sf::game::PauseCommandType::preview_setting);
  moveNext(menu, 2);
  require(menu.update({.confirm = true}).type ==
          sf::game::PauseCommandType::preview_setting);
  moveNext(menu);
  require(menu.update({.confirm = true}).type ==
          sf::game::PauseCommandType::preview_setting);
  require(menu.settings().controller_preset ==
          sf::game::ControllerPreset::alternate);
  require(menu.settings().invert_aim && !menu.settings().vibration);

  moveNext(menu);
  const auto reset = menu.update({.confirm = true});
  require(reset.type == sf::game::PauseCommandType::preview_setting);
  require(menu.settings().controller_preset ==
          sf::game::ControllerPreset::standard);
  require(!menu.settings().invert_aim && menu.settings().vibration);
  require(sf::game::controllerButtonForAction(
              menu.settings(), sf::game::ControllerAction::shoot) == 0x8000U);

  moveNext(menu);
  const auto accept = menu.update({.confirm = true});
  require(accept.type == sf::game::PauseCommandType::commit_settings);
  require(menu.screen() == PauseScreen::options);
  settleTransition(menu);

  require(!menu.update({.confirm = true}));
  settleTransition(menu);
  require(menu.screen() == PauseScreen::controller);
  require(menu.update({.right = true}).type ==
          sf::game::PauseCommandType::preview_setting);
  require(menu.settings().controller_preset ==
          sf::game::ControllerPreset::alternate);
  moveNext(menu, 6);
  const auto cancel = menu.update({.confirm = true});
  require(cancel.type == sf::game::PauseCommandType::revert_settings);
  require(menu.screen() == PauseScreen::options);
  require(menu.settings().controller_preset ==
          sf::game::ControllerPreset::standard);
  require(!menu.settings().invert_aim && menu.settings().vibration);
  require(sf::game::controllerButtonForAction(
              menu.settings(), sf::game::ControllerAction::shoot) == 0x8000U);
}

void testExactGuestMissionEntries() {
  const std::vector<std::string> objective_texts{
      "Eliminate Kravitch", "Protect CBDC", "Turn off power", "Tag bomb", ".",
  };
  const auto objectives = sf::game::makeRetailMissionMenuEntries(
      objective_texts, 5U, 0x11U, 0x08U, 0x04U);
  require(objectives.size() == 4U);
  require(objectives[0].id == 4U && objectives[0].text == "Tag bomb" &&
          objectives[0].state == sf::game::MissionEntryState::completed &&
          objectives[0].visible);
  require(objectives[1].id == 3U && objectives[1].text == "Turn off power" &&
          objectives[1].state == sf::game::MissionEntryState::failed &&
          objectives[1].visible);
  require(objectives[2].id == 2U && objectives[2].text == "Protect CBDC" &&
          objectives[2].state == sf::game::MissionEntryState::active &&
          !objectives[2].visible);
  require(objectives[3].id == 1U &&
          objectives[3].text == "Eliminate Kravitch" &&
          objectives[3].state == sf::game::MissionEntryState::active &&
          objectives[3].visible);

  const std::vector<std::string> parameter_texts{
      "Do not eliminate CBDC",
      "Avoid damaging bombs",
  };
  const auto parameters = sf::game::makeRetailMissionMenuEntries(
      parameter_texts, 2U, 0x03U, 0U, 0x01U);
  require(parameters.size() == 2U &&
          parameters[0].text == "Avoid damaging bombs" &&
          parameters[0].state == sf::game::MissionEntryState::active &&
          parameters[1].text == "Do not eliminate CBDC" &&
          parameters[1].state == sf::game::MissionEntryState::failed);

  require(
      sf::game::makeRetailMissionMenuEntries(objective_texts, 6U, 0xffffffffU)
          .empty());
  require(
      sf::game::makeRetailMissionMenuEntries(objective_texts, 33U, 0xffffffffU)
          .empty());
  const std::vector<std::string> invalid_texts{"", "Valid"};
  require(
      sf::game::makeRetailMissionMenuEntries(invalid_texts, 2U, 0x03U).empty());
}

void testNoReconnaissanceRootPreview() {
  auto menu = makeMenu();
  auto data = menu.data();
  data.mission.map.layer_assets.clear();
  data.mission.map.reconnaissance_available = false;
  data.mission.map.markers.push_back(
      {sf::game::MapMarkerKind::hostile, 0.25F, 0.75F});
  menu.reset(std::move(data));

  const auto commands = menu.buildRenderCommands();
  require(std::ranges::none_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::asset ||
           command.kind == PauseRenderKind::map_marker;
  }));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.text == "No Reconnaissance";
  }));
  require(!menu.update({.confirm = true}) &&
          menu.screen() == PauseScreen::root);
}

void testCompoundRussianMenuLocalization() {
  sf::game::setGameLanguage(sf::game::GameLanguage::russian_vit);
  const auto slots = sf::game::localizeTextCopy("Slot 2  Empty");
  require(slots.find("Slot") == std::string::npos &&
          slots.find("Empty") == std::string::npos);
  const auto status =
      sf::game::localizeTextCopy("Mission Objectives\nActive: 4\nCompleted: 0");
  require(status.find("Mission") == std::string::npos &&
          status.find("Active") == std::string::npos &&
          status.find("Completed") == std::string::npos);
  const auto hint = sf::game::localizeTextCopy("%x select   %t back");
  require(hint.find("select") == std::string::npos &&
          hint.find("back") == std::string::npos);
  require(sf::game::localizeTextCopy("Sound") == "zbyk");
  for (const auto label : {"ARMOR", "HEALTH", "DANGER", "TARGET", "HEAD SHOT",
                           "HEADSHOT", "BOMB"}) {
    const auto translated = sf::game::localizeTextCopy(label);
    require(translated != label && translated.find('?') == std::string::npos);
  }
  const auto action = sf::game::localizeTextCopy("Change Weapon: R1");
  require(action.find("Change Weapon") == std::string::npos &&
          action.find('?') == std::string::npos && action.ends_with("R1"));
  const auto ammo = sf::game::localizeTextCopy("Ammo: 15/90");
  require(ammo.find("Ammo") == std::string::npos &&
          ammo.find("15/90") != std::string::npos);
  require(sf::game::localizeTextCopy("Gas Granade") != "Gas Granade");
  for (const auto label : {"Grenades", "GRANADES", "Gas Grenades",
                           "GAS GRANADES"}) {
    const auto translated = sf::game::localizeTextCopy(label);
    require(translated != label && translated.find('?') == std::string::npos);
  }
  require(sf::game::localizeTextCopy("N/A") != "N/A");
  require(sf::game::localizeTextCopy("Infinite") != "Infinite");
  for (const auto message :
       {"Campaign Complete", "CAMPAIGN COMPLETED", "CAMPAING COMPLETED"}) {
    const auto translated = sf::game::localizeTextCopy(message);
    require(translated != message &&
            translated.find("Campaign") == std::string::npos &&
            translated.find("CAMPAIGN") == std::string::npos &&
            translated.find("CAMPAING") == std::string::npos);
  }
  const auto completed_slot =
      sf::game::localizeTextCopy("Slot 2  Campaign Complete");
  require(completed_slot.find("Slot") == std::string::npos &&
          completed_slot.find("Campaign") == std::string::npos);
  require(sf::game::completeGameplayTextSource("Scope Pwr O") ==
          std::optional<std::string_view>{"Scope Pwr On"});
  require(sf::game::completeGameplayTextSource("No Target Avail") ==
          std::optional<std::string_view>{"No Target Available"});
  require(!sf::game::completeGameplayTextSource("No"));
  for (
      const auto description : {
          "The 9mm handgun is the standard issue side-arm for NATO and all "
          "five branches of the US armed forces since passing the 1979 MRBF "
          "(Mean Rounds Before operational Failure) performance test, "
          "expending 35,000 rounds, six times the pistol's service life.",
          "Primarily used as a stealth weapon against multiple targets, this "
          "grenade releases trace amounts of Soman nerve agent into the air. "
          "The gas quickly dissipates, but not before rendering victims "
          "unconscious. If no antidote is administered, death follows within "
          "15 minutes.",
          "These incendiary blocks are made of a putty-like material which "
          "can be molded to the user's liking. The C4 explosive putty is then "
          "wired to a fuse and a friction igniter, allowing the user to "
          "detonate the explosive from a distant or protected position.",
          "The overly heavy recoil of this 12 gauge shotgun is more than "
          "compensated for by it's unparalleled stopping power and its "
          "recoil-inertia operation which is significantly faster than the "
          "gas operated system found in most autoloading shotguns.",
          "The 12-gauge modified choke shotgun is standard issue for the DEA, "
          "FBI and USSS. In firing tests using tactical 00 shot with nine lead "
          "on an ISCP regulation target at 25 yards, the payload was delivered "
          "into the \"A\" kill zone with limited collateral damage.",
      }) {
    const auto translated = sf::game::localizeTextCopy(description);
    require(translated != description &&
            translated.find('?') == std::string::npos);
  }
  const auto confirmation =
      sf::game::localizeTextCopy("Do you really want to restart this mission?");
  require(confirmation.find("restart") == std::string::npos &&
          confirmation.find('?') == confirmation.size() - 1U);
  for (const auto message :
       {"9mm taken", "9 mm taken", "Sniper Rifle bullet taken",
        "Nightvision Rifle shell taken", "Grenade taken", "Gas Grenade taken",
        "Gas Granade taken", "GAS GRENADE taken", "Objective Updated",
        "Objective Complete", "OBJECTIVE COMPLETE"}) {
    const auto translated = sf::game::localizeTextCopy(message);
    require(translated != message && translated.find('?') == std::string::npos);
  }
  const auto gas_grenade_pickup =
      sf::game::localizeTextCopy("Gas Grenade taken");
  for (const auto variant :
       {"GAS GRENADE TAKEN", "GAS GRANADE TAKEN", "Gas Grenades taken",
        "Gas Granades Taken", "Gas Grenade  taken"}) {
    require(sf::game::localizeTextCopy(variant) == gas_grenade_pickup);
  }
  for (const auto prefix :
       {"Gas G", "Gas Gren", "Gas Grana", "GAS GRENADES TAK"}) {
    const auto completed = sf::game::completeGameplayTextSource(prefix);
    require(completed == std::optional<std::string_view>{"Gas Grenade taken"});
    require(sf::game::localizeTextCopy(*completed) == gas_grenade_pickup);
  }
  for (const auto message : {
           "Press Triangle to contact Lian Xing",
           "Press X to Contact Lian Xing",
           "Press BUTTON to Contact Lian Xing",
           "Press \x1f"
           " to Contact Lian Xing",
           "Flak Jacket Undamaged",
           "Antigen administered\n6 subjects remaining",
           "Missile indexed.\n4 remaining",
           "Eliminate hostile",
           "Find security cardkey",
       }) {
    const auto translated = sf::game::localizeTextCopy(message);
    require(translated != message &&
            translated.find("Press") == std::string::npos &&
            translated.find("Flak") == std::string::npos &&
            translated.find("Antigen") == std::string::npos &&
            translated.find("remaining") == std::string::npos &&
            translated.find('?') == std::string::npos);
  }
  const auto contact_prompt =
      sf::game::localizeTextCopy("Press BUTTON to Contact Lian Xing");
  require(contact_prompt.find("%x") != std::string::npos &&
          contact_prompt.find("%s") == std::string::npos &&
          contact_prompt.find("BUTTON") == std::string::npos &&
          contact_prompt.find(sf::game::localizeTextCopy("Lian Xing")) ==
              std::string::npos);
  for (const auto objective : {
           "Turn off power to terminal security doors",
           "Locate explosives cache",
           "LOCATE EXPLOSIVES CACHE.",
           "Eliminate Kravitch and destroy comm array",
           "Do not allow yourself to be spotted until you reach the meeting.  "
           "Do not shoot Phagan.",
           "Do not kill Aramov",
       }) {
    const auto translated = sf::game::localizeTextCopy(objective);
    require(translated != objective &&
            translated.find('?') == std::string::npos);
  }
  sf::game::setGameLanguage(sf::game::GameLanguage::english);
}

void testProofreadRussianCampaignTextIsBuiltIn() {
  sf::game::setLocalizationRoot({});
  sf::game::setGameLanguage(sf::game::GameLanguage::russian_vit);

  const auto briefing = sf::game::localizedMissionBriefing(0U);
  require(briefing &&
          briefing->location.find("Washington") == std::string::npos &&
          briefing->mission_title.find("Georgia") == std::string::npos &&
          briefing->additional_directive.find("Rhoemer") == std::string::npos &&
          briefing->additional_directive.find('?') == std::string::npos);

  constexpr std::array expected_dates{
      std::string_view{"08/23 22:45"}, std::string_view{"08/23 23:45"},
      std::string_view{"08/24 00:30"}, std::string_view{"08/24 00:45"},
      std::string_view{"08/24 01:15"}, std::string_view{"08/25 19:00"},
      std::string_view{"08/25 19:15"}, std::string_view{"09/01 21:00"},
      std::string_view{"09/01 21:30"}, std::string_view{"09/01 21:40"},
      std::string_view{"09/01 21:45"}, std::string_view{"09/07 06:30"},
      std::string_view{"09/07 07:15"}, std::string_view{"09/07 08:00"},
      std::string_view{"09/08 03:00"}, std::string_view{"09/08 03:25"},
      std::string_view{"09/08 04:00"}, std::string_view{"09/08 04:15"},
      std::string_view{"09/08 04:45"}, std::string_view{"09/08 05:00"},
  };
  std::vector<std::string> full_directives;
  for (std::uint32_t mission = 0U; mission < expected_dates.size(); ++mission) {
    const auto localized = sf::game::localizedMissionBriefing(mission);
    const auto &definition = sf::game::missionDefinition(mission);
    require(localized && localized->date_time == expected_dates[mission] &&
            localized->mission_title ==
                sf::game::localizeText(definition.title) &&
            localized->directive.find("\n\n") != std::string::npos &&
            localized->directive.size() > 100U &&
            localized->additional_directive.size() > 40U &&
            std::ranges::find(full_directives, localized->directive) ==
                full_directives.end());
    full_directives.push_back(localized->directive);
  }

  const std::vector<std::string> objectives{
      "Eliminate Kravitch and destroy comm. array",
      "Protect CBDC bomb squad",
      "Plant C4 charges at 4 fuel tanks",
  };
  const std::vector<std::string> parameters{
      "Do not kill Aramov",
      "Do not kill any CBDC agent",
  };
  const auto localized =
      sf::game::localizedMissionMenuTexts(0U, objectives, parameters);
  require(localized && localized->objectives.size() == objectives.size() &&
          localized->parameters.size() == parameters.size());
  for (std::size_t index = 0U; index < objectives.size(); ++index) {
    require(localized->objectives[index] != objectives[index] &&
            localized->objectives[index].find('?') == std::string::npos);
  }
  for (std::size_t index = 0U; index < parameters.size(); ++index) {
    require(localized->parameters[index] != parameters[index] &&
            localized->parameters[index].find('?') == std::string::npos);
  }

  for (const auto name :
       {"Mara Aramov", "Anton Girdeux", "Gabriel Logan", "Lian Xing",
        "Jonathan Phagan", "Erich Rhoemer", "Jorge Marcos", "Pavel Kravitch",
        "Thomas Markinson", "Edward Benton"}) {
    const auto translated = sf::game::localizeTextCopy(name);
    require(translated != name && translated.find('?') == std::string::npos);
  }
  const std::vector<std::string> transient_objectives{
      "transient overlay table contents"};
  const std::vector<std::string> no_parameters;
  const auto safe_fallback = sf::game::localizedMissionMenuTexts(
      0U, transient_objectives, no_parameters);
  require(safe_fallback && safe_fallback->objectives.size() == 1U &&
          safe_fallback->objectives.front() != transient_objectives.front() &&
          safe_fallback->objectives.front().find("transient") ==
              std::string::npos);
  sf::game::setGameLanguage(sf::game::GameLanguage::english);
}

void testFormattedMissionLocalization() {
  sf::game::setLocalizationRoot({});
  sf::game::setGameLanguage(sf::game::GameLanguage::russian_vit);
  const auto authored_briefing = sf::game::localizedMissionBriefing(0U);
  require(authored_briefing.has_value());

  const auto root = std::filesystem::current_path() / "sf-localization-test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  std::vector<std::byte> bytes{
      std::byte{'S'}, std::byte{'F'}, std::byte{'L'}, std::byte{'M'},
      std::byte{'N'}, std::byte{'U'}, std::byte{'2'}, std::byte{0},
  };
  const auto append_u32 = [&](std::uint32_t value) {
    for (auto shift = 0U; shift < 32U; shift += 8U) {
      bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
  };
  const auto append_string = [&](std::string_view value) {
    append_u32(static_cast<std::uint32_t>(value.size()));
    for (const auto character : value) {
      bytes.push_back(static_cast<std::byte>(character));
    }
  };
  append_u32(2U);
  append_u32(1U);
  append_string("Eliminate %d%s scientist%s");
  append_string("Eliminate translated: %d");
  append_u32(1U);
  append_string("Locate explosives cache");
  append_string("Localized explosives cache");
  {
    std::ofstream output{root / "mission_menu.dat", std::ios::binary};
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  std::vector<std::byte> briefing_bytes{
      std::byte{'S'}, std::byte{'F'}, std::byte{'L'}, std::byte{'B'},
      std::byte{'R'}, std::byte{'F'}, std::byte{'1'}, std::byte{0},
  };
  const auto append_briefing_u32 = [&](std::uint32_t value) {
    for (auto shift = 0U; shift < 32U; shift += 8U) {
      briefing_bytes.push_back(
          static_cast<std::byte>((value >> shift) & 0xffU));
    }
  };
  const auto append_briefing_string = [&](std::string_view value) {
    append_briefing_u32(static_cast<std::uint32_t>(value.size()));
    for (const auto character : value) {
      briefing_bytes.push_back(static_cast<std::byte>(character));
    }
  };
  append_briefing_u32(1U);
  append_briefing_string("PACK LOCATION");
  append_briefing_string("PACK TITLE");
  append_briefing_string("PACK DATE");
  append_briefing_string("PACK DIRECTIVE");
  append_briefing_string("PACK DETAILS");
  {
    std::ofstream output{root / "briefings.dat", std::ios::binary};
    output.write(reinterpret_cast<const char *>(briefing_bytes.data()),
                 static_cast<std::streamsize>(briefing_bytes.size()));
  }

  sf::game::setLocalizationRoot(root);
  sf::game::setGameLanguage(sf::game::GameLanguage::russian_vit);
  const auto packed_briefing = sf::game::localizedMissionBriefing(0U);
  require(packed_briefing &&
          packed_briefing->location == authored_briefing->location &&
          packed_briefing->mission_title == authored_briefing->mission_title &&
          packed_briefing->date_time == authored_briefing->date_time &&
          packed_briefing->directive == authored_briefing->directive &&
          packed_briefing->additional_directive ==
              authored_briefing->additional_directive);
  const std::vector<std::string> objectives{
      "Eliminate 6 scientists",
      "Locate explosives cache.",
  };
  const std::vector<std::string> parameters;
  const auto localized =
      sf::game::localizedMissionMenuTexts(0U, objectives, parameters);
  require(localized && localized->objectives.size() == 2U &&
          localized->objectives.front() == "Eliminate translated: 6" &&
          localized->objectives.back() == "Localized explosives cache");
  require(sf::game::localizeTextCopy("Locate explosives cache.") ==
          "Localized explosives cache");
  sf::game::setLocalizationRoot({});
  sf::game::setGameLanguage(sf::game::GameLanguage::english);
  std::filesystem::remove_all(root);
}

void testMissionMenuLocalizationDoesNotLeakAcrossLanguages() {
  const std::vector<std::string> objectives{
      "Eliminate Kravitch and destroy comm. array",
      "Protect CBDC bomb squad",
  };
  const std::vector<std::string> parameters{
      "Do not kill Aramov",
  };

  sf::game::setGameLanguage(sf::game::GameLanguage::english);
  require(!sf::game::localizedMissionMenuTexts(0U, objectives, parameters));

  sf::game::setGameLanguage(sf::game::GameLanguage::russian_vit);
  const auto localized =
      sf::game::localizedMissionMenuTexts(0U, objectives, parameters);
  require(localized && localized->objectives.size() == objectives.size() &&
          localized->parameters.size() == parameters.size() &&
          localized->objectives.front() != objectives.front() &&
          localized->parameters.front() != parameters.front());

  sf::game::setGameLanguage(sf::game::GameLanguage::english);
}

void testRetailCheatChordsAndContexts() {
  using sf::game::RetailCheat;
  using sf::game::RetailPauseCheatContext;
  sf::game::RetailCheatState state;
  state.enableAll();
  for (std::size_t index = 0U; index < sf::game::retail_cheat_count; ++index) {
    require(state.enabled(sf::game::retailCheatAt(index)));
  }
  state.set(RetailCheat::hard_mode, false);
  require(!state.hard_mode);

  const auto hard = sf::game::detectRetailTitleCheat(0xe681U, true);
  require(hard && *hard == RetailCheat::hard_mode);
  require(!sf::game::detectRetailTitleCheat(0xe681U, false));

  const auto all_weapons = sf::game::detectRetailPauseCheat(
      0xe320U, RetailPauseCheatContext::weapons_section);
  require(all_weapons && *all_weapons == RetailCheat::all_weapons);
  require(
      !sf::game::detectRetailPauseCheat(0xe320U, RetailPauseCheatContext::map));

  const auto stage_select = sf::game::detectRetailPauseCheat(
      0xed00U, RetailPauseCheatContext::select_mission);
  require(stage_select && *stage_select == RetailCheat::stage_select);
  require(!sf::game::detectRetailPauseCheat(
      0xfd00U, RetailPauseCheatContext::select_mission));

  const auto weak =
      sf::game::detectRetailPauseCheat(0x4c20U, RetailPauseCheatContext::map);
  const auto theater =
      sf::game::detectRetailPauseCheat(0x4920U, RetailPauseCheatContext::map);
  require(weak && *weak == RetailCheat::weak_enemies);
  require(theater && *theater == RetailCheat::movie_theater);
}

} // namespace

int main() {
  try {
    testRetailLayoutContract();
    testOriginalRootComposition();
    testRetailMapInformationWrapsIntoTheWindow();
    testRussianMapObjectivesFitInsideInformationPanel();
    testOptionsPreviewIsImmediatelyComplete();
    testRetailControllerBindingsAreVisible();
    testRetailOptionsAndControllerOrder();
    testRetailCheatsMenu();
    testEverySectionKeepsAcdComposition();
    testNestedScreensStayInsideLeftPanel();
    testAdjustmentScreensStayInsideLeftPanel();
    testRetailTransitions();
    testRetailMissionSelectCheat();
    testRootAndBriefing();
    testBriefingPagination();
    testWeaponEquipAndGuard();
    testMapAndWeaponExpandedViews();
    testOptionsConfirmationAndBinding();
    testDenseObjectivesStayInsidePanel();
    testWeaponLabelsAndControllerRecovery();
    testRetailControllerPresetsApplyBindings();
    testRetailControllerTransaction();
    testExactGuestMissionEntries();
    testNoReconnaissanceRootPreview();
    testCompoundRussianMenuLocalization();
    testProofreadRussianCampaignTextIsBuiltIn();
    testFormattedMissionLocalization();
    testMissionMenuLocalizationDoesNotLeakAcrossLanguages();
    testRetailCheatChordsAndContexts();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
