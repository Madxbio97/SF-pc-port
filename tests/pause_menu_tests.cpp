#include "sf/game/pause_menu.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <source_location>
#include <stdexcept>
#include <string>
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
           command.color == sf::game::PauseColorRole::map_highlight;
  }));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::map_marker &&
           command.id ==
               static_cast<std::uint32_t>(sf::game::MapMarkerKind::player) &&
           command.color == sf::game::PauseColorRole::map_highlight;
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

void testOptionsPreviewIsImmediatelyComplete() {
  auto menu = makeMenu();
  moveNext(menu, 5);
  require(menu.screen() == PauseScreen::root);
  constexpr std::array labels{
      "Restart Mission",
      "Restart At Last Checkpoint",
      "Quit Game",
      "Select Mission",
      "Sound",
      "Game Brightness",
      "Screen Centering",
      "Controller",
  };
  const auto commands = menu.buildRenderCommands();
  for (std::size_t index = 0; index < labels.size(); ++index) {
    require(
        std::ranges::any_of(commands, [index, &labels](const auto &command) {
          return command.kind == PauseRenderKind::menu_item &&
                 command.panel == PausePanelRole::left_content &&
                 command.id == index && command.text == labels[index] &&
                 !command.selected;
        }));
  }
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
  };
  const auto options = menu.buildRenderCommands();
  for (std::size_t index = 0; index < option_labels.size(); ++index) {
    require(std::ranges::any_of(
        options, [index, &option_labels](const auto &command) {
          return command.kind == PauseRenderKind::menu_item &&
                 command.panel == PausePanelRole::left_content &&
                 command.id == index && command.text == option_labels[index];
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

  moveNext(menu);
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
    return command.kind == PauseRenderKind::asset && command.bounds.x >= 236;
  }));
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text && command.bounds.x < 220 &&
           command.text.find("PK-102") != std::string::npos;
  }));
  require(std::ranges::none_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.text.find("Submachine gun") != std::string::npos;
  }));
  require(std::ranges::none_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.panel == PausePanelRole::right_information;
  }));
  require(!menu.update({.confirm = true}));
  require(menu.expanded());
  commands = menu.buildRenderCommands();
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::panel &&
           sameRect(command.bounds,
                    PauseAcdLayout::expanded_weapon_information_panel);
  }));
  require(std::ranges::count_if(commands, [](const auto &command) {
            return command.kind == PauseRenderKind::slider;
          }) == 3);
  for (const auto label : {"Fire Rate", "Power", "Accuracy"}) {
    require(std::ranges::any_of(commands, [label](const auto &command) {
      return command.kind == PauseRenderKind::slider && command.text == label;
    }));
  }
  require(std::ranges::none_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.text.find("Submachine gun") != std::string::npos;
  }));
  require(!menu.update({.right = true}));
  commands = menu.buildRenderCommands();
  require(std::ranges::any_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.text.find("Submachine gun") != std::string::npos &&
           command.bounds.x > 190;
  }));
  require(std::ranges::none_of(commands, [](const auto &command) {
    return command.kind == PauseRenderKind::slider;
  }));
  require(!menu.update({.cancel = true}));
  require(!menu.expanded());
  require(!menu.update({.confirm = true}));
  commands = menu.buildRenderCommands();
  require(std::ranges::count_if(commands, [](const auto &command) {
            return command.kind == PauseRenderKind::slider;
          }) == 3);
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
}

void testWeaponLabelsAndControllerRecovery() {
  auto menu = makeMenu();
  openRootSection(menu, 4);
  const auto weapon_commands = menu.buildRenderCommands();
  require(std::ranges::any_of(weapon_commands, [](const auto &command) {
    return command.kind == PauseRenderKind::text &&
           command.panel == PausePanelRole::left_content &&
           command.text.find("Rate: 5/5") != std::string::npos &&
           command.text.find("Damage: 3/5") != std::string::npos;
  }));

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

} // namespace

int main() {
  try {
    testRetailLayoutContract();
    testOriginalRootComposition();
    testRetailMapInformationWrapsIntoTheWindow();
    testOptionsPreviewIsImmediatelyComplete();
    testRetailControllerBindingsAreVisible();
    testRetailOptionsAndControllerOrder();
    testEverySectionKeepsAcdComposition();
    testNestedScreensStayInsideLeftPanel();
    testAdjustmentScreensStayInsideLeftPanel();
    testRetailTransitions();
    testRetailMissionSelectCheat();
    testRootAndBriefing();
    testWeaponEquipAndGuard();
    testMapAndWeaponExpandedViews();
    testOptionsConfirmationAndBinding();
    testDenseObjectivesStayInsidePanel();
    testWeaponLabelsAndControllerRecovery();
    testRetailControllerPresetsApplyBindings();
    testRetailControllerTransaction();
    testExactGuestMissionEntries();
    testNoReconnaissanceRootPreview();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
