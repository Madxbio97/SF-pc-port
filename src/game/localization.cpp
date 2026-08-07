#include "sf/game/localization.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

namespace sf::game {
namespace {

std::atomic language{GameLanguage::english};
std::filesystem::path pack_root;

using Utf8Translation = std::pair<std::string_view, std::u8string_view>;

// Human-readable source of truth for the common UI. The old ViT executable
// contains numerous spelling errors and look-alike Latin letters; encode the
// proofread UTF-8 strings into its one-byte glyph map only at runtime.
constexpr std::array base_utf8_translations{
    Utf8Translation{
        "AGENT MODE",
        u8"\u0420\u0415\u0416\u0418\u041c \u0410\u0413\u0415\u041d\u0422"},
    Utf8Translation{"DIFFICULTY",
                    u8"\u0421\u041b\u041e\u0416\u041d\u041e\u0421\u0422\u042c"},
    Utf8Translation{
        "A special PC-version mode not present in the original game. Enemies "
        "are more accurate and aggressive, and some missions have stricter "
        "conditions.",
        u8"\u0421\u041f\u0415\u0426\u0418\u0410\u041b\u042c\u041d\u042b\u0419 "
        u8"\u0420\u0415\u0416\u0418\u041c "
        u8"\u041f\u041a-\u0412\u0415\u0420\u0421\u0418\u0418, "
        u8"\u041a\u041e\u0422\u041e\u0420\u041e\u0413\u041e \u041d\u0415 "
        u8"\u0411\u042b\u041b\u041e \u0412 "
        u8"\u041e\u0420\u0418\u0413\u0418\u041d\u0410\u041b\u042c\u041d\u041e"
        u8"\u0419 \u0418\u0413\u0420\u0415. \u0412\u0420\u0410\u0413\u0418 "
        u8"\u0422\u041e\u0427\u041d\u0415\u0415 \u0418 "
        u8"\u0410\u0413\u0420\u0415\u0421\u0421\u0418\u0412\u041d\u0415\u0415, "
        u8"\u0410 \u0412 "
        u8"\u041d\u0415\u041a\u041e\u0422\u041e\u0420\u042b\u0425 "
        u8"\u041c\u0418\u0421\u0421\u0418\u042f\u0425 "
        u8"\u0414\u0415\u0419\u0421\u0422\u0412\u0423\u042e\u0422 "
        u8"\u0411\u041e\u041b\u0415\u0415 "
        u8"\u0421\u0422\u0420\u041e\u0413\u0418\u0415 "
        u8"\u0423\u0421\u041b\u041e\u0412\u0418\u042f."},
    Utf8Translation{
        "%x continue   %t back",
        u8"%x \u041f\u0420\u041e\u0414\u041e\u041b\u0416\u0418\u0422\u042c   "
        u8"%t \u041d\u0410\u0417\u0410\u0414"},
    Utf8Translation{
        "%x - continue; %t - back",
        u8"%x - \u041f\u0420\u041e\u0414\u041e\u041b\u0416\u0418\u0422\u042c; "
        u8"%t - \u041d\u0410\u0417\u0410\u0414"},
    Utf8Translation{"Select Difficulty",
                    u8"\u0412\u042b\u0411\u0415\u0420\u0418\u0422\u0415 "
                    u8"\u0421\u041b\u041e\u0416\u041d\u041e\u0421\u0422\u042c"},
    Utf8Translation{"Normal",
                    u8"\u041e\u0420\u0418\u0413\u0418\u041d\u0410\u041b"},
    Utf8Translation{"Agent", u8"\u0410\u0413\u0415\u041d\u0422"},
    Utf8Translation{"Playing Agent mode",
                    u8"\u0412\u042b\u0411\u0420\u0410\u041d\u041d\u0410\u042f "
                    u8"\u0421\u041b\u041e\u0416\u041d\u041e\u0421\u0422\u042c: "
                    u8"\u0410\u0413\u0415\u041d\u0422"},
    Utf8Translation{"Map", u8"КАРТА"},
    Utf8Translation{"Objectives", u8"ЦЕЛИ"},
    Utf8Translation{"Parameters", u8"УСЛОВИЯ"},
    Utf8Translation{"Briefing", u8"БРИФИНГ"},
    Utf8Translation{"Weapons", u8"ОРУЖИЕ"},
    Utf8Translation{"Options", u8"НАСТРОЙКИ"},
    Utf8Translation{"Cheats", u8"ЧИТЫ"},
    Utf8Translation{"All Weapons + Infinite Ammo",
                    u8"ВСЁ ОРУЖИЕ + БЕСК. ПАТРОНЫ"},
    Utf8Translation{"Hard Mode", u8"ВЫСОКАЯ СЛОЖНОСТЬ"},
    Utf8Translation{"One-Shot Kills", u8"УБИЙСТВО С ОДНОГО ВЫСТРЕЛА"},
    Utf8Translation{"Stage Select", u8"ВЫБОР МИССИЙ"},
    Utf8Translation{"Weak Enemies", u8"ОСЛАБЛЕННЫЕ ВРАГИ"},
    Utf8Translation{"Movie Theater", u8"КИНОТЕАТР"},
    Utf8Translation{"Retail Codes", u8"КОДЫ ОРИГИНАЛА"},
    Utf8Translation{"On", u8"ВКЛ"},
    Utf8Translation{"Off", u8"ВЫКЛ"},
    Utf8Translation{"%x toggle", u8"%x ПЕРЕКЛЮЧИТЬ"},
    Utf8Translation{"Restart Mission", u8"НАЧАТЬ ЗАНОВО"},
    Utf8Translation{"Restart At Last Checkpoint", u8"С КОНТРОЛЬНОЙ ТОЧКИ"},
    Utf8Translation{"Quit Game", u8"ВЫЙТИ ИЗ ИГРЫ"},
    Utf8Translation{"Select Mission", u8"ВЫБОР МИССИИ"},
    Utf8Translation{"Sound", u8"ЗВУК"},
    Utf8Translation{"Game Brightness", u8"ЯРКОСТЬ ИГРЫ"},
    Utf8Translation{"Screen Centering", u8"ПОЛОЖЕНИЕ ЭКРАНА"},
    Utf8Translation{"Controller", u8"УПРАВЛЕНИЕ"},
    Utf8Translation{"Controller Configuration:", u8"НАСТРОЙКА УПРАВЛЕНИЯ:"},
    Utf8Translation{"Controller Configuration", u8"НАСТРОЙКА УПРАВЛЕНИЯ"},
    Utf8Translation{"Stick Layout", u8"РАСКЛАДКА СТИКОВ"},
    Utf8Translation{"Character Left / Camera Right",
                    u8"ПЕРСОНАЖ: ЛЕВЫЙ / КАМЕРА: ПРАВЫЙ"},
    Utf8Translation{"Character Right / Camera Left",
                    u8"ПЕРСОНАЖ: ПРАВЫЙ / КАМЕРА: ЛЕВЫЙ"},
    Utf8Translation{
        "Original (One Stick)",
        u8"\u041e\u0420\u0418\u0413\u0418\u041d\u0410\u041b\u042c\u041d\u0410"
        u8"\u042f (\u041e\u0414\u0418\u041d \u0421\u0422\u0418\u041a)"},
    Utf8Translation{"Reset", u8"СБРОСИТЬ"},
    Utf8Translation{"Accept", u8"ПРИНЯТЬ"},
    Utf8Translation{"Cancel", u8"ОТМЕНИТЬ"},
    Utf8Translation{"Load Game", u8"ЗАГРУЗИТЬ ИГРУ"},
    Utf8Translation{"Slot", u8"ЯЧЕЙКА"},
    Utf8Translation{"Empty", u8"ПУСТО"},
    Utf8Translation{"Back", u8"НАЗАД"},
    Utf8Translation{"Mission Complete", u8"МИССИЯ ВЫПОЛНЕНА"},
    Utf8Translation{"MISSION COMPLETE", u8"МИССИЯ ВЫПОЛНЕНА"},
    Utf8Translation{"Campaign Complete", u8"КАМПАНИЯ ЗАВЕРШЕНА"},
    Utf8Translation{"CAMPAIGN COMPLETE", u8"КАМПАНИЯ ЗАВЕРШЕНА"},
    Utf8Translation{"Campaign Completed", u8"КАМПАНИЯ ЗАВЕРШЕНА"},
    Utf8Translation{"CAMPAIGN COMPLETED", u8"КАМПАНИЯ ЗАВЕРШЕНА"},
    // Keep the misspelling accepted as well: it appeared in early PC UI
    // builds and old save-slot labels can still reach the renderer.
    Utf8Translation{"Campaing Complete", u8"КАМПАНИЯ ЗАВЕРШЕНА"},
    Utf8Translation{"CAMPAING COMPLETE", u8"КАМПАНИЯ ЗАВЕРШЕНА"},
    Utf8Translation{"Campaing Completed", u8"КАМПАНИЯ ЗАВЕРШЕНА"},
    Utf8Translation{"CAMPAING COMPLETED", u8"КАМПАНИЯ ЗАВЕРШЕНА"},
    Utf8Translation{"Save Mission?", u8"СОХРАНИТЬ ПРОГРЕСС?"},
    Utf8Translation{"Yes", u8"ДА"},
    Utf8Translation{"No", u8"НЕТ"},
    Utf8Translation{"Memory Card", u8"КАРТА ПАМЯТИ"},
    Utf8Translation{"Choose a save slot", u8"ВЫБЕРИТЕ ЯЧЕЙКУ СОХРАНЕНИЯ"},
    Utf8Translation{"Choose slot to use", u8"ВЫБЕРИТЕ ЯЧЕЙКУ"},
    Utf8Translation{"--- Empty Slot ---", u8"--- ПУСТАЯ ЯЧЕЙКА ---"},
    Utf8Translation{"Reading save data", u8"ЧТЕНИЕ СОХРАНЕНИЙ"},
    Utf8Translation{"Invalid", u8"НЕДОСТУПНО"},
    Utf8Translation{"Current Location", u8"МЕСТОПОЛОЖЕНИЕ"},
    Utf8Translation{"Press %x to continue", u8"НАЖМИТЕ %x, ЧТОБЫ ПРОДОЛЖИТЬ"},
    Utf8Translation{"%t exit menu", u8"%t ВЫЙТИ ИЗ МЕНЮ"},
    Utf8Translation{"%t cancel", u8"%t ОТМЕНИТЬ"},
    Utf8Translation{"%t back", u8"%t НАЗАД"},
    Utf8Translation{"%t resume", u8"%t ПРОДОЛЖИТЬ"},
    Utf8Translation{"%t close", u8"%t ЗАКРЫТЬ"},
    Utf8Translation{"%x load", u8"%x ЗАГРУЗИТЬ"},
    Utf8Translation{"%x save", u8"%x СОХРАНИТЬ"},
    Utf8Translation{"%x full", u8"%x НА ВЕСЬ ЭКРАН"},
    Utf8Translation{"%x equip", u8"%x ВЫБРАТЬ"},
    Utf8Translation{"%x accept", u8"%x ПРИНЯТЬ"},
    Utf8Translation{"%x next", u8"%x ДАЛЕЕ"},
    Utf8Translation{"%x select", u8"%x ВЫБРАТЬ"},
    Utf8Translation{"none", u8"НЕТ"},
    Utf8Translation{"Mission Objectives:", u8"ЦЕЛИ МИССИИ:"},
    Utf8Translation{"Mission Objectives", u8"ЦЕЛИ МИССИИ"},
    Utf8Translation{"Completed Objectives:", u8"ВЫПОЛНЕННЫЕ ЦЕЛИ:"},
    Utf8Translation{"Failed Objectives:", u8"ПРОВАЛЕННЫЕ ЦЕЛИ:"},
    Utf8Translation{"Mission Parameters:", u8"УСЛОВИЯ МИССИИ:"},
    Utf8Translation{"Mission Parameters", u8"УСЛОВИЯ МИССИИ"},
    Utf8Translation{"Failed Parameters:", u8"НАРУШЕННЫЕ УСЛОВИЯ:"},
    Utf8Translation{"Data Available", u8"ДОСТУПНЫ ДАННЫЕ"},
    Utf8Translation{"Location Unknown", u8"МЕСТОПОЛОЖЕНИЕ НЕИЗВЕСТНО"},
    Utf8Translation{"Custom", u8"ПОЛЬЗОВАТЕЛЬСКАЯ"},
    Utf8Translation{"Alternate", u8"АЛЬТЕРНАТИВНАЯ"},
    Utf8Translation{"Standard", u8"СТАНДАРТНАЯ"},
    Utf8Translation{"Configuration", u8"НАСТРОЙКИ"},
    Utf8Translation{"Select Mission - All", u8"ВСЕ МИССИИ"},
    Utf8Translation{"No Reconnaissance", u8"НЕТ ДАННЫХ КАРТЫ"},
    Utf8Translation{"No briefing data available", u8"НЕТ ДАННЫХ БРИФИНГА"},
    Utf8Translation{"No weapon equipped", u8"ОРУЖИЕ НЕ ВЫБРАНО"},
    Utf8Translation{"Press new button for action", u8"НАЖМИТЕ НОВУЮ КНОПКУ"},
    Utf8Translation{"Adjust game lighting levels by moving bar up or down",
                    u8"ИЗМЕНИТЕ ЯРКОСТЬ КНОПКАМИ ВВЕРХ И ВНИЗ"},
    Utf8Translation{"Use directional buttons to center the image.",
                    u8"ПЕРЕМЕЩАЙТЕ ИЗОБРАЖЕНИЕ КНОПКАМИ НАПРАВЛЕНИЯ."},
    Utf8Translation{"Brightness", u8"ЯРКОСТЬ"},
    Utf8Translation{"Active", u8"АКТИВНЫЕ"},
    Utf8Translation{"Completed", u8"ВЫПОЛНЕНО"},
    Utf8Translation{"Sound FX", u8"ЗВУКОВЫЕ ЭФФЕКТЫ"},
    Utf8Translation{"Music", u8"МУЗЫКА"},
    Utf8Translation{"Voice-over", u8"РЕЧЬ"},
    Utf8Translation{"Preset config", u8"СХЕМА"},
    Utf8Translation{"Invert Aim", u8"ИНВЕРСИЯ ПРИЦЕЛА"},
    Utf8Translation{"Vibration", u8"ВИБРАЦИЯ"},
    Utf8Translation{"yes", u8"ДА"},
    Utf8Translation{"no", u8"НЕТ"},
    Utf8Translation{"Equipped", u8"ВЫБРАНО"},
    Utf8Translation{"Selected", u8"ВЫБРАНО"},
    Utf8Translation{"Power", u8"МОЩНОСТЬ"},
    Utf8Translation{"Accuracy", u8"ТОЧНОСТЬ"},
    Utf8Translation{"Fire Rate", u8"ТЕМП ОГНЯ"},
    Utf8Translation{"Rate", u8"ТЕМП ОГНЯ"},
    Utf8Translation{"Damage", u8"УРОН"},
    Utf8Translation{"Ammo", u8"ПАТРОНЫ"},
    Utf8Translation{"Ammo: ", u8"ПАТРОНЫ: "},
    Utf8Translation{"Clip Size", u8"МАГАЗИН"},
    Utf8Translation{"Max Rounds", u8"БОЕЗАПАС"},
    Utf8Translation{"N/A", u8"Н/Д"},
    Utf8Translation{"Infinite", u8"БЕСКОНЕЧНО"},
    Utf8Translation{"Mission Failed", u8"МИССИЯ ПРОВАЛЕНА"},
    Utf8Translation{"Mission Parameter Failed", u8"УСЛОВИЕ МИССИИ НАРУШЕНО"},
    Utf8Translation{"Mission Objective Failed", u8"ЦЕЛЬ МИССИИ НЕ ВЫПОЛНЕНА"},
    Utf8Translation{"MISSION FAILED", u8"МИССИЯ ПРОВАЛЕНА"},
    Utf8Translation{"FIRE OR ACTION TO RETRY", u8"НАЖМИТЕ ОГОНЬ ИЛИ ДЕЙСТВИЕ"},
    Utf8Translation{"Objective", u8"ЦЕЛЬ"},
    Utf8Translation{"Parameter", u8"УСЛОВИЕ"},
    Utf8Translation{"Complete", u8"ВЫПОЛНЕНО"},
    Utf8Translation{"Press START to see objectives",
                    u8"НАЖМИТЕ %t ДЛЯ ПРОСМОТРА ЦЕЛЕЙ"},
    Utf8Translation{"Playing on HARD difficulty", u8"СЛОЖНОСТЬ: ВЫСОКАЯ"},
    Utf8Translation{"Target Lock", u8"ЗАХВАТ ЦЕЛИ"},
    Utf8Translation{"Head Shot", u8"ВЫСТРЕЛ В ГОЛОВУ"},
    Utf8Translation{"Checkpoint", u8"КОНТРОЛЬНАЯ ТОЧКА"},
    Utf8Translation{"Briefing Updated", u8"БРИФИНГ ОБНОВЛЁН"},
    Utf8Translation{"Objective Added", u8"ДОБАВЛЕНА ЦЕЛЬ"},
    Utf8Translation{"Objective Complete", u8"ЦЕЛЬ ВЫПОЛНЕНА"},
    Utf8Translation{"Objectives Incomplete", u8"ЦЕЛИ НЕ ВЫПОЛНЕНЫ"},
    Utf8Translation{"Level alerted", u8"ПОДНЯТА ТРЕВОГА"},
    Utf8Translation{"Electronically Locked", u8"ЭЛЕКТРОННЫЙ ЗАМОК"},
    Utf8Translation{"Lock Code Required", u8"ТРЕБУЕТСЯ КОД ДОСТУПА"},
    Utf8Translation{"Elevator Call Switch", u8"КНОПКА ВЫЗОВА ЛИФТА"},
    Utf8Translation{"Power Switch", u8"ВЫКЛЮЧАТЕЛЬ ПИТАНИЯ"},
    Utf8Translation{"No Target Available", u8"НЕТ ДОСТУПНЫХ ЦЕЛЕЙ"},
    Utf8Translation{"Body Armor", u8"БРОНЕЖИЛЕТ"},
    Utf8Translation{"Georgia Street", u8"ДЖОРДЖИЯ-СТРИТ"},
    Utf8Translation{"Destroyed subway", u8"РАЗРУШЕННОЕ МЕТРО"},
    Utf8Translation{"Main subway line", u8"ГЛАВНАЯ ЛИНИЯ МЕТРО"},
    Utf8Translation{"Washington Park", u8"ВАШИНГТОН-ПАРК"},
    Utf8Translation{"Freedom Memorial", u8"МЕМОРИАЛ СВОБОДЫ"},
    Utf8Translation{"Expo Center Reception", u8"ПРИЁМНАЯ ЭКСПОЦЕНТРА"},
    Utf8Translation{"Expo Center Dinorama", u8"ДИНОРАМА ЭКСПОЦЕНТРА"},
    Utf8Translation{"Rhoemer's Base", u8"БАЗА РОМЕРА"},
    Utf8Translation{"Base Bunker", u8"БУНКЕР БАЗЫ"},
    Utf8Translation{"Base Tower", u8"БАШНЯ БАЗЫ"},
    Utf8Translation{"Base Escape", u8"ПОБЕГ С БАЗЫ"},
    Utf8Translation{"Rhoemer's Stronghold", u8"КРЕПОСТЬ РОМЕРА"},
    Utf8Translation{"Stronghold lower level", u8"НИЖНИЙ УРОВЕНЬ КРЕПОСТИ"},
    Utf8Translation{"Stronghold catacombs", u8"КАТАКОМБЫ КРЕПОСТИ"},
    Utf8Translation{"PHARCOM warehouses", u8"СКЛАДЫ ФАРКОМ"},
    Utf8Translation{"PHARCOM elite guards", u8"ЭЛИТНАЯ ОХРАНА ФАРКОМ"},
    Utf8Translation{"Warehouse 76", u8"СКЛАД 76"},
    Utf8Translation{"Silo access tunnels", u8"ТОННЕЛИ К РАКЕТНОЙ ШАХТЕ"},
    Utf8Translation{"Tunnel blackout", u8"ОБЕСТОЧЕННЫЕ ТОННЕЛИ"},
    Utf8Translation{"Missile Silo", u8"РАКЕТНАЯ ШАХТА"},
    Utf8Translation{"C4 Explosives", u8"ВЗРЫВЧАТКА С4"},
    Utf8Translation{"Flak Jacket", u8"БРОНЕЖИЛЕТ"},
    Utf8Translation{"Viral Scanner", u8"ДЕТЕКТОР ВИРУСА"},
    Utf8Translation{"Flashlight", u8"ФОНАРЬ"},
    Utf8Translation{"Gas Grenade", u8"ГАЗОВАЯ ГРАНАТА"},
    Utf8Translation{"Sniper rifle", u8"СНАЙПЕРСКАЯ ВИНТОВКА"},
    Utf8Translation{"Nightvision rifle", u8"ВИНТОВКА С НОЧНЫМ ПРИЦЕЛОМ"},
    Utf8Translation{"Combat Shotgun", u8"БОЕВОЙ ДРОБОВИК"},
    Utf8Translation{"Silenced 9mm", u8"9-ММ ПИСТОЛЕТ С ГЛУШИТЕЛЕМ"},
};

// Native-authored strings are encoded to the original ViT single-byte glyph
// map at the boundary. Keep the source literals as real UTF-8: pre-encoding
// them (or saving mojibake here) silently selects unrelated Cyrillic glyphs.
constexpr std::array utf8_translations{
    Utf8Translation{
        "BOMB DETONATION",
        u8"\u0414\u0415\u0422\u041e\u041d\u0410\u0426\u0418\u042f \u0411\u041e\u041c\u0411\u042b"},
    Utf8Translation{"BOMB TECH",
                    u8"\u0421\u0410\u041f\u0401\u0420"},
    Utf8Translation{
        "ARAMOV ESCAPE",
        u8"\u041f\u041e\u0411\u0415\u0413 \u0410\u0420\u0410\u041c\u041e\u0412\u041e\u0419"},
    Utf8Translation{
        "SUSPICION",
        u8"\u041f\u041e\u0414\u041e\u0417\u0420\u0415\u041d\u0418\u0415"},
    Utf8Translation{"PHAGAN",
                    u8"\u0424\u042d\u0419\u0413\u0410\u041d"},
    Utf8Translation{"ARAMOV",
                    u8"\u0410\u0420\u0410\u041c\u041e\u0412\u0410"},
    Utf8Translation{"ARMOR", u8"БРОНЯ"},
    Utf8Translation{"HEALTH", u8"ЗДОРОВЬЕ"},
    Utf8Translation{"DANGER", u8"ОПАСНОСТЬ"},
    Utf8Translation{"TARGET", u8"ЦЕЛЬ"},
    Utf8Translation{"HEAD SHOT", u8"В ГОЛОВУ"},
    Utf8Translation{"HEADSHOT", u8"В ГОЛОВУ"},
    Utf8Translation{"BOMB", u8"БОМБА"},
    Utf8Translation{"health", u8"ЗДОРОВЬЕ"},
    Utf8Translation{"armor", u8"БРОНЯ"},
    Utf8Translation{"danger", u8"ОПАСНОСТЬ"},
    Utf8Translation{"target", u8"ЦЕЛЬ"},
    Utf8Translation{"head", u8"ГОЛОВА"},
    Utf8Translation{"weapon", u8"ОРУЖИЕ"},
    Utf8Translation{"hand", u8"РУКА"},
    Utf8Translation{"body", u8"ТЕЛО"},
    Utf8Translation{"limb", u8"КОНЕЧНОСТЬ"},
    Utf8Translation{"Switch", u8"ПЕРЕКЛЮЧАТЕЛЬ"},
    Utf8Translation{"Bomb", u8"БОМБА"},
    Utf8Translation{"Locked", u8"ЗАКРЫТО"},
    Utf8Translation{"No Weapon", u8"БЕЗ ОРУЖИЯ"},
    Utf8Translation{"Silenced 9mm", u8"9-ММ ПИСТОЛЕТ С ГЛУШИТЕЛЕМ"},
    Utf8Translation{"9mm", u8"9 ММ"},
    Utf8Translation{"9 mm", u8"9 ММ"},
    Utf8Translation{".45", u8".45"},
    Utf8Translation{"G-18", u8"G-18"},
    Utf8Translation{"Shotgun", u8"ДРОБОВИК"},
    Utf8Translation{"Combat Shotgun", u8"БОЕВОЙ ДРОБОВИК"},
    Utf8Translation{"PK-102", u8"PK-102"},
    Utf8Translation{"M-16", u8"M-16"},
    Utf8Translation{"BIZ-2", u8"BIZ-2"},
    Utf8Translation{"HK-5", u8"HK-5"},
    Utf8Translation{"Nightvision Rifle", u8"ВИНТОВКА С НОЧНЫМ ПРИЦЕЛОМ"},
    Utf8Translation{"Sniper Rifle", u8"СНАЙПЕРСКАЯ ВИНТОВКА"},
    Utf8Translation{"Taser", u8"ШОКЕР"},
    Utf8Translation{"Flamethrower", u8"ОГНЕМЁТ"},
    Utf8Translation{"M-79", u8"М-79"},
    Utf8Translation{"K3G4", u8"K3G4"},
    Utf8Translation{"Virus Scanner", u8"ДЕТЕКТОР ВИРУСА"},
    Utf8Translation{"Grenade", u8"ГРАНАТА"},
    Utf8Translation{"Grenades", u8"ГРАНАТЫ"},
    Utf8Translation{"Granade", u8"ГРАНАТА"},
    Utf8Translation{"Granades", u8"ГРАНАТЫ"},
    Utf8Translation{"Gas Grenade", u8"ГАЗОВАЯ ГРАНАТА"},
    Utf8Translation{"Gas Grenades", u8"ГАЗОВЫЕ ГРАНАТЫ"},
    // Some retail call sites use the misspelled item label "Granade".
    Utf8Translation{"Gas Granade", u8"ГАЗОВАЯ ГРАНАТА"},
    Utf8Translation{"Gas Granades", u8"ГАЗОВЫЕ ГРАНАТЫ"},
    Utf8Translation{"Flashlight", u8"ФОНАРЬ"},
    Utf8Translation{"Chopper Gun", u8"БОРТОВОЙ ПУЛЕМЁТ"},
    Utf8Translation{"Keycard", u8"КЛЮЧ-КАРТА"},
    Utf8Translation{"Cardkey", u8"КЛЮЧ-КАРТА"},
    Utf8Translation{"C4 Explosives", u8"ВЗРЫВЧАТКА С4"},
    Utf8Translation{"Viral Antigen", u8"АНТИГЕН"},
    Utf8Translation{"Antigen", u8"АНТИГЕН"},
    Utf8Translation{"Objective Updated", u8"ЦЕЛЬ ОБНОВЛЕНА"},
    Utf8Translation{"OBJECTIVE UPDATED", u8"ЦЕЛЬ ОБНОВЛЕНА"},
    Utf8Translation{"OBJECTIVE ADDED", u8"ДОБАВЛЕНА ЦЕЛЬ"},
    Utf8Translation{"BRIEFING UPDATED", u8"БРИФИНГ ОБНОВЛЁН"},
    Utf8Translation{"Mission locked", u8"МИССИЯ ЗАКРЫТА"},
    Utf8Translation{"Cannot change weapons while performing current action",
                    u8"СЕЙЧАС НЕЛЬЗЯ СМЕНИТЬ ОРУЖИЕ"},
    Utf8Translation{
        "Controller missing. Please reinsert controller into controller port 1 "
        "and press the %t button to continue",
        u8"КОНТРОЛЛЕР НЕ ПОДКЛЮЧЁН. ПОДКЛЮЧИТЕ ЕГО И НАЖМИТЕ %t"},
    Utf8Translation{"Weapons - Ratings", u8"ОРУЖИЕ - ХАРАКТЕРИСТИКИ"},
    Utf8Translation{"Weapons - Description", u8"ОРУЖИЕ - ОПИСАНИЕ"},
    Utf8Translation{"No description available", u8"НЕТ ОПИСАНИЯ"},
    Utf8Translation{"No weapons available", u8"НЕТ ОРУЖИЯ"},
    Utf8Translation{"A/D page", u8"A/D СТРАНИЦА"},
    Utf8Translation{"W/S weapon", u8"W/S ОРУЖИЕ"},
    Utf8Translation{"A/D map", u8"A/D КАРТА"},
    Utf8Translation{"%x info", u8"%x ИНФО"},
    Utf8Translation{"%x Save", u8"%x СОХРАНИТЬ"},
    Utf8Translation{"%t Cancel", u8"%t ОТМЕНА"},
    Utf8Translation{"Do you really want to restart at the last checkpoint?",
                    u8"НАЧАТЬ С ПОСЛЕДНЕЙ КОНТРОЛЬНОЙ ТОЧКИ?"},
    Utf8Translation{"Do you really want to restart this mission?",
                    u8"НАЧАТЬ МИССИЮ ЗАНОВО?"},
    Utf8Translation{"Do you really want to abort this game?",
                    u8"ВЫЙТИ ИЗ ТЕКУЩЕЙ ИГРЫ?"},
    Utf8Translation{"Pause", u8"ПАУЗА"},
    Utf8Translation{"Confirmation", u8"ПОДТВЕРЖДЕНИЕ"},
    Utf8Translation{"Notification", u8"УВЕДОМЛЕНИЕ"},
    Utf8Translation{"Unknown", u8"НЕИЗВЕСТНО"},
    Utf8Translation{"Change Weapon", u8"СМЕНА ОРУЖИЯ"},
    Utf8Translation{"Shoot", u8"ОГОНЬ"},
    Utf8Translation{"Kneel", u8"ПРИСЕСТЬ"},
    Utf8Translation{"Roll/Zoom Out", u8"КУВЫРОК/ОТДАЛИТЬ"},
    Utf8Translation{"Step Right", u8"ШАГ ВПРАВО"},
    Utf8Translation{"Step Left", u8"ШАГ ВЛЕВО"},
    Utf8Translation{"Use/Zoom In", u8"ДЕЙСТВИЕ/ПРИБЛИЗИТЬ"},
    Utf8Translation{"Aim", u8"ПРИЦЕЛ"},
    Utf8Translation{"Acknowledge", u8"ПОДТВЕРДИТЬ"},
    Utf8Translation{"Mara Aramov", u8"МАРА АРАМОВА"},
    Utf8Translation{"Aramov", u8"АРАМОВА"},
    Utf8Translation{"Anton Girdeux", u8"АНТОН ГИРДО"},
    Utf8Translation{"Girdeux", u8"ГИРДО"},
    Utf8Translation{"Gabriel Logan", u8"ГАБРИЭЛЬ ГЕЙБ ЛОГАН"},
    Utf8Translation{"Gabe Logan", u8"ГЕЙБ ЛОГАН"},
    Utf8Translation{"Gabe", u8"ГЕЙБ"},
    Utf8Translation{"Lian Xing", u8"ЛИАН СИН"},
    Utf8Translation{"Lian", u8"ЛИАН СИН"},
    Utf8Translation{"Jonathan Phagan", u8"ДЖОНАТАН ФЭЙГАН"},
    Utf8Translation{"Phagan", u8"ФЭЙГАН"},
    Utf8Translation{"Erich Rhoemer", u8"ЭРИХ РОМЕР"},
    Utf8Translation{"Rhoemer", u8"РОМЕР"},
    Utf8Translation{"Jorge Marcos", u8"ХОРХЕ МАРКОС"},
    Utf8Translation{"Pavel Kravitch", u8"ПАВЕЛ КРАВИЧ"},
    Utf8Translation{"Thomas Markinson", u8"ТОМАС МАРКИНСОН"},
    Utf8Translation{"Markinson", u8"ТОМАС МАРКИНСОН"},
    Utf8Translation{"Edward Benton", u8"ЭДВАРД БЕНТОН"},
    Utf8Translation{"CBDC Agent", u8"АГЕНТ ХИМЗАЩИТЫ"},
    Utf8Translation{"Communications array", u8"АНТЕННА СВЯЗИ"},
    Utf8Translation{"Computer", u8"КОМПЬЮТЕР"},
    Utf8Translation{"Elevator", u8"ЛИФТ"},
    Utf8Translation{"Message", u8"СООБЩЕНИЕ"},
    Utf8Translation{"Plant beacon", u8"УСТАНОВИТЬ МАЯК"},
    Utf8Translation{"Viral detector", u8"ДЕТЕКТОР ВИРУСА"},
    Utf8Translation{"remaining", u8"ОСТАЛОСЬ"},
    // The first retail %s is a controller glyph, not prose. Keep it as the
    // native %x action token so presentation can substitute the current PC
    // interact binding after localization. The second argument is always
    // Lian Xing in retail, so use the concise Russian call-sign here instead
    // of copying the guest's English name back into the translated line.
    Utf8Translation{"Press %s to Contact %s",
                    u8"НАЖМИТЕ %x, ЧТОБЫ СВЯЗАТЬСЯ С ЛИАН"},
    Utf8Translation{"%s undamaged", u8"БРОНЕЖИЛЕТ НЕ ПОВРЕЖДЁН"},
    Utf8Translation{"Complete Objectives", u8"ВЫПОЛНИТЬ ЦЕЛИ"},
    Utf8Translation{"Debug Display", u8"ОТЛАДОЧНЫЙ ЭКРАН"},
    Utf8Translation{"End Level", u8"ЗАВЕРШИТЬ УРОВЕНЬ"},
    Utf8Translation{"NPCs on Radar", u8"ПЕРСОНАЖИ НА РАДАРЕ"},
    Utf8Translation{"Poly Debug", u8"ОТЛАДКА ПОЛИГОНОВ"},
    Utf8Translation{"Resurrect Gabriel", u8"ВОСКРЕСИТЬ ГЕЙБА"},
    Utf8Translation{"Select Level", u8"ВЫБОР УРОВНЯ"},
    Utf8Translation{"To first zoom point", u8"К ПЕРВОЙ ТОЧКЕ МАСШТАБА"},
    Utf8Translation{"To second zoom point", u8"КО ВТОРОЙ ТОЧКЕ МАСШТАБА"},
    Utf8Translation{"Toggle CObj drawing", u8"ПЕРЕКЛЮЧИТЬ ОТРИСОВКУ ОБЪЕКТОВ"},
    Utf8Translation{"New York City", u8"НЬЮ-ЙОРК"},
    Utf8Translation{"Washington DC", u8"ВАШИНГТОН, ОКРУГ КОЛУМБИЯ"},
    Utf8Translation{"Almaty, Kazakhstan", u8"АЛМАТЫ, КАЗАХСТАН"},
    Utf8Translation{"Rozovka, Kazakhstan", u8"РОЗОВКА, КАЗАХСТАН"},
    Utf8Translation{"Uzhhorod, Ukraine", u8"УЖГОРОД, УКРАИНА"},
    Utf8Translation{"Overwrite existing files?",
                    u8"ПЕРЕЗАПИСАТЬ СУЩЕСТВУЮЩИЕ ФАЙЛЫ?"},
    Utf8Translation{"This MEMORY CARD is full.  Syphon Filter requires one "
                    "block on the MEMORY CARD.  You will not be able to save "
                    "your game to this MEMORY CARD.  Continue anyway?",
                    u8"КАРТА ПАМЯТИ ЗАПОЛНЕНА. ДЛЯ СОХРАНЕНИЯ НУЖЕН ОДИН "
                    u8"СВОБОДНЫЙ БЛОК. ПРОДОЛЖИТЬ БЕЗ СОХРАНЕНИЯ?"},
    Utf8Translation{"Lots o' Bullets", u8"МНОГО ПАТРОНОВ"},
    Utf8Translation{"stereo", u8"СТЕРЕО"},
    Utf8Translation{"diagnostic: ok", u8"ДИАГНОСТИКА: НОРМА"},
    Utf8Translation{"power: ok", u8"ПИТАНИЕ: НОРМА"},
    Utf8Translation{"Status: ok", u8"СОСТОЯНИЕ: НОРМА"},
    Utf8Translation{"Scope Pwr On", u8"ПРИЦЕЛ ВКЛЮЧЁН"},
    Utf8Translation{"on body", u8"НА ТЕЛЕ"},
    Utf8Translation{"Paul is dead.", u8"ПОЛ МЁРТВ."},
    Utf8Translation{"Turn off power to terminal security doors",
                    u8"ОТКЛЮЧИТЬ ПИТАНИЕ ДВЕРЕЙ ТЕРМИНАЛА"},
    Utf8Translation{"Locate explosives cache", u8"НАЙТИ СКЛАД ВЗРЫВЧАТКИ"},
    Utf8Translation{"Do not kill Aramov", u8"НЕ УБИВАТЬ МАРУ АРАМОВУ"},
    Utf8Translation{"Tag bomb in terminal", u8"ПОМЕТИТЬ БОМБУ В ТЕРМИНАЛЕ"},
    Utf8Translation{"Protect CBDC bomb squad", u8"ЗАЩИТИТЬ САПЁРОВ ХИМЗАЩИТЫ"},
    Utf8Translation{"Eliminate Kravitch and destroy comm. array",
                    u8"ЛИКВИДИРОВАТЬ ПАВЛА КРАВИЧА И УНИЧТОЖИТЬ АНТЕННУ СВЯЗИ"},
    Utf8Translation{
        "Avoid damaging viral delivery systems or explosive bombs",
        u8"НЕ ПОВРЕДИТЬ СИСТЕМЫ ДОСТАВКИ ВИРУСА И ВЗРЫВНЫЕ УСТРОЙСТВА"},
    Utf8Translation{"Do not eliminate any CBDC agent",
                    u8"НЕ УБИВАТЬ АГЕНТОВ ХИМЗАЩИТЫ"},
    Utf8Translation{"Kravitch", u8"ПАВЕЛ КРАВИЧ"},
    Utf8Translation{"Blow open passage to street and protect CBDC agent",
                    u8"ВЗОРВАТЬ ПРОХОД НА УЛИЦУ И ЗАЩИТИТЬ АГЕНТА ХИМЗАЩИТЫ"},
    Utf8Translation{"Do not damage any bomb", u8"НЕ ПОВРЕДИТЬ БОМБЫ"},
    Utf8Translation{"Do not kill any CBDC agent",
                    u8"НЕ УБИВАТЬ АГЕНТОВ ХИМЗАЩИТЫ"},
    Utf8Translation{"C4 taken", u8"ВЗРЫВЧАТКА С4 ВЗЯТА"},
    Utf8Translation{"Need Explosives", u8"НУЖНА ВЗРЫВЧАТКА"},
    Utf8Translation{"Blocked Passage", u8"ПРОХОД ЗАВАЛЕН"},
    Utf8Translation{"Gas Mains", u8"ГАЗОВАЯ МАГИСТРАЛЬ"},
    Utf8Translation{"Gas Mains Closed", u8"ГАЗ ПЕРЕКРЫТ"},
    Utf8Translation{"Bomb Detonated", u8"БОМБА ВЗОРВАНА"},
    Utf8Translation{"Eliminate Aramov", u8"ЛИКВИДИРОВАТЬ МАРУ АРАМОВУ"},
    Utf8Translation{"Do not use grenades or train lines may be damaged",
                    u8"НЕ ИСПОЛЬЗОВАТЬ ГРАНАТЫ: МОЖНО ПОВРЕДИТЬ ПУТИ"},
    Utf8Translation{"Reach Freedom Memorial",
                    u8"ДОБРАТЬСЯ ДО МЕМОРИАЛА СВОБОДЫ"},
    Utf8Translation{"Eliminate trigger man Marcos",
                    u8"ЛИКВИДИРОВАТЬ ПОДРЫВНИКА ХОРХЕ МАРКОСА"},
    Utf8Translation{"Secure terrorist comm. array",
                    u8"ВЗЯТЬ ПОД КОНТРОЛЬ АНТЕННУ СВЯЗИ ТЕРРОРИСТОВ"},
    Utf8Translation{"Rescue CBDC hostages",
                    u8"ОСВОБОДИТЬ ЗАЛОЖНИКОВ ХИМЗАЩИТЫ"},
    Utf8Translation{"Do not damage the comm. array",
                    u8"НЕ ПОВРЕДИТЬ АНТЕННУ СВЯЗИ"},
    Utf8Translation{
        "All bombs must be defused in under 15 minutes",
        u8"\u041e\u0411\u0415\u0417\u0412\u0420\u0415\u0414\u0418\u0422\u042c \u0412\u0421\u0415 \u0411\u041e\u041c\u0411\u042b \u041c\u0415\u041d\u0415\u0415 \u0427\u0415\u041c \u0417\u0410 15 \u041c\u0418\u041d\u0423\u0422"},
    Utf8Translation{"All bombs must be defused in under 20 minutes",
                    u8"ОБЕЗВРЕДИТЬ ВСЕ БОМБЫ МЕНЕЕ ЧЕМ ЗА 20 МИНУТ"},
    Utf8Translation{"Do not kill any member of the strike team",
                    u8"НЕ УБИВАТЬ БОЙЦОВ ШТУРМОВОЙ ГРУППЫ"},
    Utf8Translation{"Locate and disarm %d%s viral bomb%s",
                    u8"НАЙТИ И ОБЕЗВРЕДИТЬ ВИРУСНЫЕ БОМБЫ: %d"},
    Utf8Translation{"them", u8"ИХ"},
    Utf8Translation{"%d Bomb%s Remaining", u8"ОСТАЛОСЬ БОМБ: %d"},
    Utf8Translation{"Communications array damaged",
                    u8"АНТЕННА СВЯЗИ ПОВРЕЖДЕНА"},
    Utf8Translation{"Marcos", u8"ХОРХЕ МАРКОС"},
    Utf8Translation{"Kill Girdeux", u8"ЛИКВИДИРОВАТЬ АНТОНА ГИРДО"},
    Utf8Translation{
        "Do not use grenades or Girdeux's bomb may be damaged",
        u8"НЕ ИСПОЛЬЗОВАТЬ ГРАНАТЫ: МОЖНО ПОВРЕДИТЬ БОМБУ АНТОНА ГИРДО"},
    Utf8Translation{"Capture Phagan alive",
                    u8"ЗАХВАТИТЬ ДЖОНАТАНА ФЭЙГАНА ЖИВЫМ"},
    Utf8Translation{"Shadow Phagan to secret meeting",
                    u8"ПРОСЛЕДИТЬ ЗА ДЖОНАТАНОМ ФЭЙГАНОМ ДО ТАЙНОЙ ВСТРЕЧИ"},
    Utf8Translation{"Do not allow yourself to be spotted until you reach the "
                    "meeting.  Do not shoot Phagan.",
                    u8"НЕ ПОПАДАТЬСЯ НА ГЛАЗА ДО МЕСТА ВСТРЕЧИ. НЕ СТРЕЛЯТЬ В "
                    u8"ДЖОНАТАНА ФЭЙГАНА"},
    Utf8Translation{"Benton", u8"ЭДВАРД БЕНТОН"},
    Utf8Translation{"Gate Control Panel", u8"ПУЛЬТ УПРАВЛЕНИЯ ВОРОТАМИ"},
    Utf8Translation{"Capture Aramov and Phagan alive",
                    u8"ЗАХВАТИТЬ МАРУ АРАМОВУ И ДЖОНАТАНА ФЭЙГАНА ЖИВЫМИ"},
    Utf8Translation{"Do not allow Phagan to die",
                    u8"НЕ ДАТЬ ДЖОНАТАНУ ФЭЙГАНУ ПОГИБНУТЬ"},
    Utf8Translation{"Escape through main gate", u8"ВЫЙТИ ЧЕРЕЗ ГЛАВНЫЕ ВОРОТА"},
    Utf8Translation{"Reach missile bunker", u8"ДОБРАТЬСЯ ДО РАКЕТНОГО БУНКЕРА"},
    Utf8Translation{"Disable power to motion sensors",
                    u8"ОТКЛЮЧИТЬ ПИТАНИЕ ДАТЧИКОВ ДВИЖЕНИЯ"},
    Utf8Translation{"Eliminate Gabrek and collect cardkey",
                    u8"ЛИКВИДИРОВАТЬ ГАБРЕКА И ЗАБРАТЬ КЛЮЧ-КАРТУ"},
    Utf8Translation{"Get out of base before timer runs out on explosives",
                    u8"ПОКИНУТЬ БАЗУ ДО ВЗРЫВА ЗАРЯДОВ"},
    Utf8Translation{"Do not damage the missiles", u8"НЕ ПОВРЕДИТЬ РАКЕТЫ"},
    Utf8Translation{"Do not damage the explosive charges",
                    u8"НЕ ПОВРЕДИТЬ ВЗРЫВНЫЕ ЗАРЯДЫ"},
    Utf8Translation{"Reach comm. building roof",
                    u8"ДОБРАТЬСЯ ДО КРЫШИ ЦЕНТРА СВЯЗИ"},
    Utf8Translation{"Plant C4 charges at %d%s fuel tank%s",
                    u8"УСТАНОВИТЬ С4 НА ТОПЛИВНЫЕ ЦИСТЕРНЫ: %d"},
    Utf8Translation{"Catalog %d%s enemy missile%s",
                    u8"ЗАРЕГИСТРИРОВАТЬ РАКЕТЫ: %d"},
    Utf8Translation{" more", u8" ЕЩЁ"},
    Utf8Translation{"Gabrek", u8"ГАБРЕК"},
    Utf8Translation{"Place explosive here", u8"УСТАНОВИТЬ ЗАРЯД ЗДЕСЬ"},
    Utf8Translation{"%d more explosive%s to plant",
                    u8"ОСТАЛОСЬ УСТАНОВИТЬ ЗАРЯДОВ: %d"},
    Utf8Translation{"Motion Sensor Power", u8"ПИТАНИЕ ДАТЧИКОВ ДВИЖЕНИЯ"},
    Utf8Translation{"Missile Access Control", u8"КОНТРОЛЬ ДОСТУПА К РАКЕТАМ"},
    Utf8Translation{"Missile Access Initiated", u8"ДОСТУП К РАКЕТАМ ОТКРЫТ"},
    Utf8Translation{"SS-23 Missile", u8"РАКЕТА SS-23"},
    Utf8Translation{"Missile indexed.\n%d remaining",
                    u8"РАКЕТА ЗАРЕГИСТРИРОВАНА\nОСТАЛОСЬ: %d"},
    Utf8Translation{"Shoot down attack helicopter", u8"СБИТЬ БОЕВОЙ ВЕРТОЛЁТ"},
    Utf8Translation{"Disable radar tracking",
                    u8"ОТКЛЮЧИТЬ РАДАР СОПРОВОЖДЕНИЯ"},
    Utf8Translation{"Radar Tracking Override.",
                    u8"УПРАВЛЕНИЕ РАДАРОМ ПЕРЕХВАЧЕНО"},
    Utf8Translation{"Find entrance to catacombs", u8"НАЙТИ ВХОД В КАТАКОМБЫ"},
    Utf8Translation{"Do not kill any human test subjects",
                    u8"НЕ УБИВАТЬ ПОДОПЫТНЫХ"},
    Utf8Translation{"Eliminate Rhoemer's %d%s scientist%s",
                    u8"ЛИКВИДИРОВАТЬ УЧЁНЫХ ЭРИХА РОМЕРА: %d"},
    Utf8Translation{"Administer antigen to %d%s test subject%s",
                    u8"ВВЕСТИ АНТИГЕН ПОДОПЫТНЫМ: %d"},
    Utf8Translation{"%d scientist%s left to kill", u8"ОСТАЛОСЬ УЧЁНЫХ: %d"},
    Utf8Translation{"Antigen administered\n%d subject%s remaining",
                    u8"АНТИГЕН ВВЕДЁН\nОСТАЛОСЬ ПОДОПЫТНЫХ: %d"},
    Utf8Translation{"Test Subject", u8"ПОДОПЫТНЫЙ"},
    Utf8Translation{"Door lock", u8"ЗАМОК ДВЕРИ"},
    Utf8Translation{"Door unlocked", u8"ДВЕРЬ ОТКРЫТА"},
    Utf8Translation{"Get Lian Xing out of the catacombs",
                    u8"ВЫВЕСТИ ЛИАН СИН ИЗ КАТАКОМБ"},
    Utf8Translation{"Follow Phagan to Lian Xing's cell",
                    u8"ПРОСЛЕДИТЬ ЗА ДЖОНАТАНОМ ФЭЙГАНОМ ДО КАМЕРЫ ЛИАН СИН"},
    Utf8Translation{"Find Phagan", u8"НАЙТИ ДЖОНАТАНА ФЭЙГАНА"},
    Utf8Translation{"Do not allow Lian Xing to die", u8"ЗАЩИТИТЬ ЛИАН СИН"},
    Utf8Translation{
        "Do not leave Phagan unguarded or allow him to die",
        u8"НЕ ОСТАВЛЯТЬ ДЖОНАТАНА ФЭЙГАНА БЕЗ ОХРАНЫ И НЕ ДАТЬ ЕМУ ПОГИБНУТЬ"},
    Utf8Translation{
        "Do not be spotted until the scientist has opened Phagan's cell",
        u8"НЕ ПОПАДАТЬСЯ НА ГЛАЗА, ПОКА УЧЁНЫЙ НЕ ОТКРОЕТ КАМЕРУ ДЖОНАТАНА "
        u8"ФЭЙГАНА"},
    Utf8Translation{"Get to warehouse 76", u8"ДОБРАТЬСЯ ДО СКЛАДА 76"},
    Utf8Translation{"Turn off power to electric fences",
                    u8"ОТКЛЮЧИТЬ ПИТАНИЕ ЭЛЕКТРИЧЕСКИХ ОГРАЖДЕНИЙ"},
    Utf8Translation{"Find and interrogate Erikson",
                    u8"НАЙТИ И ДОПРОСИТЬ ЭРИКСОНА"},
    Utf8Translation{
        "Do not kill Erikson before you've gotten the computer codes",
        u8"НЕ УБИВАТЬ ЭРИКСОНА ДО ПОЛУЧЕНИЯ КОДОВ"},
    Utf8Translation{"Get to freight elevator",
                    u8"ДОБРАТЬСЯ ДО ГРУЗОВОГО ЛИФТА"},
    Utf8Translation{"Get out before the building collapses in 15 minutes",
                    u8"ПОКИНУТЬ ЗДАНИЕ ЗА 15 МИНУТ, ДО ЕГО ОБРУШЕНИЯ"},
    Utf8Translation{
        "Get out before the building collapses in 12 minutes",
        u8"\u041f\u041e\u041a\u0418\u041d\u0423\u0422\u042c "
        u8"\u0417\u0414\u0410\u041d\u0418\u0415 \u0417\u0410 12 "
        u8"\u041c\u0418\u041d\u0423\u0422, \u0414\u041e \u0415\u0413\u041e "
        u8"\u041e\u0411\u0420\u0423\u0428\u0415\u041d\u0418\u042f"},
    Utf8Translation{"Locate and tag %d%s viral carrier%s",
                    u8"НАЙТИ И ПОМЕТИТЬ НОСИТЕЛЕЙ ВИРУСА: %d"},
    Utf8Translation{"Viral carrier tagged. %d remaining",
                    u8"НОСИТЕЛЬ ПОМЕЧЕН. ОСТАЛОСЬ: %d"},
    Utf8Translation{"Erikson", u8"ЭРИКСОН"},
    Utf8Translation{"Shut down power room", u8"ОТКЛЮЧИТЬ ЭНЕРГОБЛОК"},
    Utf8Translation{"Reroute power to elevator",
                    u8"ПЕРЕНАПРАВИТЬ ПИТАНИЕ НА ЛИФТ"},
    Utf8Translation{"Access missile command computer",
                    u8"ПОЛУЧИТЬ ДОСТУП К КОМПЬЮТЕРУ УПРАВЛЕНИЯ РАКЕТОЙ"},
    Utf8Translation{"Retrieve missile destruct codes",
                    u8"ПОЛУЧИТЬ КОДЫ САМОУНИЧТОЖЕНИЯ РАКЕТЫ"},
    Utf8Translation{
        "Command computer must be accessed within 3 minutes after launch",
        u8"ПОЛУЧИТЬ ДОСТУП К КОМПЬЮТЕРУ В ТЕЧЕНИЕ 3 МИНУТ ПОСЛЕ ЗАПУСКА"},
    Utf8Translation{
        "Destruct codes must be retrieved before the missile launches",
        u8"ПОЛУЧИТЬ КОДЫ ДО ЗАПУСКА РАКЕТЫ"},
    Utf8Translation{"Do not shoot at the missile", u8"НЕ СТРЕЛЯТЬ ПО РАКЕТЕ"},
    Utf8Translation{"Find missile silo", u8"НАЙТИ РАКЕТНУЮ ШАХТУ"},
    Utf8Translation{"No Power", u8"НЕТ ПИТАНИЯ"},
    Utf8Translation{"Power Relay", u8"СИЛОВОЕ РЕЛЕ"},
    Utf8Translation{"Elevator Control Panel", u8"ПУЛЬТ УПРАВЛЕНИЯ ЛИФТОМ"},
    Utf8Translation{"Elevator activated", u8"ЛИФТ ВКЛЮЧЁН"},
    Utf8Translation{"Control Panel", u8"ПУЛЬТ УПРАВЛЕНИЯ"},
    Utf8Translation{"Safety Released", u8"БЛОКИРОВКА СНЯТА"},
    Utf8Translation{"Need missile codes",
                    u8"НУЖНЫ КОДЫ САМОУНИЧТОЖЕНИЯ РАКЕТЫ"},
    Utf8Translation{"R-9 Devyatka Missile", u8"РАКЕТА R-9 ДЕВЯТКА"},
    Utf8Translation{"Command Computer", u8"КОМПЬЮТЕР УПРАВЛЕНИЯ"},
    Utf8Translation{
        "The 9mm handgun is the standard issue side-arm for NATO and all five "
        "branches of the US armed forces since passing the 1979 MRBF (Mean "
        "Rounds Before operational Failure) performance test, expending 35,000 "
        "rounds, six times the pistol's service life.",
        u8"9-ММ ПИСТОЛЕТ - ШТАТНОЕ ЛИЧНОЕ ОРУЖИЕ НАТО И ВСЕХ ПЯТИ РОДОВ ВОЙСК "
        u8"США. В 1979 ГОДУ ОН ПРОШЁЛ ИСПЫТАНИЕ НА НАДЁЖНОСТЬ, ВЫДЕРЖАВ 35 000 "
        u8"ВЫСТРЕЛОВ - ВШЕСТЕРО БОЛЬШЕ РАСЧЁТНОГО РЕСУРСА."},
    Utf8Translation{
        "This tough, durable pistol has been in production for almost a "
        "century. It has tremendous stopping power, and in spite of it's "
        "strong recoil and heavy slide and bolt, in the hands of a seasoned "
        "professional, it is a deadly weapon.",
        u8"ЭТОТ ПРОЧНЫЙ И НАДЁЖНЫЙ ПИСТОЛЕТ ВЫПУСКАЕТСЯ ПОЧТИ СТО ЛЕТ. ОН "
        u8"ОБЛАДАЕТ ОГРОМНОЙ ОСТАНАВЛИВАЮЩЕЙ СИЛОЙ. НЕСМОТРЯ НА СИЛЬНУЮ ОТДАЧУ "
        u8"И ТЯЖЁЛЫЙ ЗАТВОР, В РУКАХ ОПЫТНОГО СТРЕЛКА ЭТО СМЕРТОНОСНОЕ "
        u8"ОРУЖИЕ."},
    Utf8Translation{
        "With a rate of fire topping 60 rounds per second, the G-18 is perhaps "
        "the most deadly pistol-machinegun in the world. It's only weakness is "
        "it's tendency to expend ammunition faster than most shooters are "
        "prepared for, leaving them defenseless during a reload.",
        u8"СКОРОСТРЕЛЬНОСТЬ G-18 ПРЕВЫШАЕТ 60 ВЫСТРЕЛОВ В СЕКУНДУ, ЧТО ДЕЛАЕТ "
        u8"ЕГО ОДНИМ ИЗ САМЫХ ОПАСНЫХ АВТОМАТИЧЕСКИХ ПИСТОЛЕТОВ В МИРЕ. ЕГО "
        u8"ЕДИНСТВЕННЫЙ НЕДОСТАТОК - БОЕПРИПАСЫ КОНЧАЮТСЯ ТАК БЫСТРО, ЧТО "
        u8"СТРЕЛОК ОСТАЁТСЯ БЕЗЗАЩИТНЫМ ВО ВРЕМЯ ПЕРЕЗАРЯДКИ."},
    Utf8Translation{
        "The overly heavy recoil of this 12 gauge shotgun is more than "
        "compensated for by it's unparalleled stopping power and its "
        "recoil-inertia operation which is significantly faster than the gas "
        "operated system found in most autoloading shotguns.",
        u8"ЧРЕЗМЕРНАЯ ОТДАЧА ЭТОГО РУЖЬЯ 12-ГО КАЛИБРА КОМПЕНСИРУЕТСЯ "
        u8"НЕПРЕВЗОЙДЁННОЙ ОСТАНАВЛИВАЮЩЕЙ СИЛОЙ. ИНЕРЦИОННАЯ АВТОМАТИКА "
        u8"РАБОТАЕТ ЗНАЧИТЕЛЬНО БЫСТРЕЕ ГАЗООТВОДНОЙ СИСТЕМЫ БОЛЬШИНСТВА "
        u8"САМОЗАРЯДНЫХ РУЖЕЙ."},
    Utf8Translation{
        "The 12-gauge modified choke shotgun is standard issue for the DEA, "
        "FBI and USSS. In firing tests using tactical 00 shot with nine lead "
        "on an ISCP regulation target at 25 yards, the payload was delivered "
        "into the \"A\" kill zone with limited collateral damage.",
        u8"РУЖЬЁ 12-ГО КАЛИБРА СО СМЕННЫМ ДУЛЬНЫМ СУЖЕНИЕМ СОСТОИТ НА "
        u8"ВООРУЖЕНИИ АНТИНАРКОТИЧЕСКОГО УПРАВЛЕНИЯ, ФБР И СЕКРЕТНОЙ СЛУЖБЫ "
        u8"США. НА ИСПЫТАНИЯХ КАРТЕЧЬ УВЕРЕННО ПОРАЖАЛА УБОЙНУЮ ЗОНУ МИШЕНИ С "
        u8"25 ЯРДОВ, НЕ НАНОСЯ ЛИШНЕГО УЩЕРБА."},
    Utf8Translation{
        "A variant of the popular Vokinhsilak system (one of the most widely "
        "used and modified designs in the world) the PK102 is a compact, "
        "lightweight, full assault rifle that is easy to conceal, making it a "
        "popular choice for terrorists.",
        u8"PK-102 - КОМПАКТНЫЙ ВАРИАНТ СИСТЕМЫ КАЛАШНИКОВА, ОДНОЙ ИЗ САМЫХ "
        u8"РАСПРОСТРАНЁННЫХ И ЧАСТО МОДИФИЦИРУЕМЫХ В МИРЕ. ЭТА КОМПАКТНАЯ И "
        u8"ЛЁГКАЯ ШТУРМОВАЯ ВИНТОВКА, КОТОРУЮ ЛЕГКО СПРЯТАТЬ, ПОЭТОМУ ОНА "
        u8"ПОЛЬЗУЕТСЯ СПРОСОМ У ТЕРРОРИСТОВ."},
    Utf8Translation{
        "This weapon is lightweight, accurate, and has very low recoil. The "
        "preeminent assault rifle in the world, it was developed by the US "
        "Army in 1965, and has since become a mainstay for armed forces, "
        "police, and personal defense enthusiasts.",
        u8"ЭТА ВИНТОВКА ЛЕГКАЯ, ТОЧНАЯ И ПОЧТИ НЕ ИМЕЕТ ОТДАЧИ. M-16 БЫЛА "
        u8"РАЗРАБОТАНА ДЛЯ АРМИИ США В 1965 ГОДУ И СТАЛА ОДНОЙ ИЗ ГЛАВНЫХ "
        u8"ШТУРМОВЫХ ВИНТОВОК ВООРУЖЁННЫХ СИЛ, ПОЛИЦИИ И ГРАЖДАНСКИХ "
        u8"СТРЕЛКОВ."},
    Utf8Translation{
        "This pistol-machine gun is designed to deliver sustained firepower in "
        "tight quarters. The unconventional design of its large capacity "
        "magazine keeps the weapon compact but still provides a "
        "near-bottomless source of ammunition.",
        u8"ЭТОТ ПИСТОЛЕТ-ПУЛЕМЁТ СОЗДАН ДЛЯ ДЛИТЕЛЬНОГО ОГНЯ В ТЕСНЫХ "
        u8"ПОМЕЩЕНИЯХ. НЕОБЫЧНАЯ КОНСТРУКЦИЯ ВМЕСТИТЕЛЬНОГО МАГАЗИНА СОХРАНЯЕТ "
        u8"КОМПАКТНОСТЬ ОРУЖИЯ И ОБЕСПЕЧИВАЕТ ПОЧТИ НЕИСЧЕРПАЕМЫЙ БОЕЗАПАС."},
    Utf8Translation{
        "The HK5's modular design and small size makes it very popular with "
        "both military special forces and terrorists. With over 23 officially "
        "recognized variants, it is fast becoming the most widely used "
        "pistol-machine gun in the world.",
        u8"МОДУЛЬНАЯ КОНСТРУКЦИЯ И НЕБОЛЬШИЕ РАЗМЕРЫ HK-5 СДЕЛАЛИ ЕГО "
        u8"ПОПУЛЯРНЫМ КАК У ВОЕННОГО СПЕЦНАЗА, ТАК И У ТЕРРОРИСТОВ. СУЩЕСТВУЕТ "
        u8"БОЛЕЕ 23 ОФИЦИАЛЬНЫХ ВАРИАНТОВ, И ЭТОТ ПИСТОЛЕТ-ПУЛЕМЁТ БЫСТРО "
        u8"СТАНОВИТСЯ ОДНИМ ИЗ САМЫХ РАСПРОСТРАНЁННЫХ В МИРЕ."},
    Utf8Translation{
        "A Russian rifle capable of high accuracy, it is often used by Russian "
        "Army snipers. It excels in engaging fleeting, moving, open and masked "
        "single targets. This model comes standard equipped with an SVDN2 "
        "night sight and silencer.",
        u8"ЭТА ВЫСОКОТОЧНАЯ РОССИЙСКАЯ ВИНТОВКА ЧАСТО ИСПОЛЬЗУЕТСЯ СНАЙПЕРАМИ. "
        u8"ОНА ЭФФЕКТИВНА ПРОТИВ БЫСТРО ПОЯВЛЯЮЩИХСЯ, ДВИЖУЩИХСЯ, ОТКРЫТЫХ И "
        u8"ЗАМАСКИРОВАННЫХ ОДИНОЧНЫХ ЦЕЛЕЙ. ШТАТНО ОСНАЩЕНА НОЧНЫМ ПРИЦЕЛОМ "
        u8"СВДН-2 И ГЛУШИТЕЛЕМ."},
    Utf8Translation{
        "This high-caliber, silenced rifle comes equipped with a classified "
        "digital scope with basic optical character recognition, making it a "
        "highly accurate weapon capable of identifying and classifying human "
        "targets and impact points prior to firing.",
        u8"ЭТА КРУПНОКАЛИБЕРНАЯ ВИНТОВКА С ГЛУШИТЕЛЕМ ОСНАЩЕНА СЕКРЕТНЫМ "
        u8"ЦИФРОВЫМ ПРИЦЕЛОМ С БАЗОВЫМ ОПТИЧЕСКИМ РАСПОЗНАВАНИЕМ. ПРИЦЕЛ "
        u8"ОПРЕДЕЛЯЕТ ТИП ЦЕЛИ И РАСЧЁТНУЮ ТОЧКУ ПОПАДАНИЯ ДО ВЫСТРЕЛА, "
        u8"ОБЕСПЕЧИВАЯ ВЫСОКУЮ ТОЧНОСТЬ."},
    Utf8Translation{
        "Using CO2 cartridges, this weapon fires a probe that lodges one inch "
        "deep in the victim's body. Then a charge of 500,000 volts is passed "
        "along a wire connecting the weapon to the probe. This charge can be "
        "sustained indefinitely.",
        u8"С ПОМОЩЬЮ БАЛЛОНА С УГЛЕКИСЛЫМ ГАЗОМ ШОКЕР ВЫСТРЕЛИВАЕТ ЗОНД, "
        u8"КОТОРЫЙ ВХОДИТ В ТЕЛО ЖЕРТВЫ НА ГЛУБИНУ ОКОЛО ДЮЙМА. ПО ПРОВОДУ К "
        u8"ЗОНДУ ПОДАЁТСЯ НАПРЯЖЕНИЕ 500 000 ВОЛЬТ. РАЗРЯД МОЖНО ПОДДЕРЖИВАТЬ "
        u8"НЕОГРАНИЧЕННО ДОЛГО."},
    Utf8Translation{
        "This single-barreled, break-action grenade launcher was developed "
        "during the Vietnam war. Commonly referred to as the \"Blooper\", it "
        "fires 40mm HE grenades that contain enough explosives to produce over "
        "300 fragments with a lethal radius of up to five meters.",
        u8"ЭТОТ ОДНОСТВОЛЬНЫЙ ПЕРЕЛАМЫВАЮЩИЙСЯ ГРАНАТОМЁТ БЫЛ РАЗРАБОТАН ВО "
        u8"ВРЕМЯ ВОЙНЫ ВО ВЬЕТНАМЕ. М-79 СТРЕЛЯЕТ 40-ММ ОСКОЛОЧНЫМИ ГРАНАТАМИ, "
        u8"КАЖДАЯ ИЗ КОТОРЫХ ОБРАЗУЕТ БОЛЕЕ 300 ОСКОЛКОВ С РАДИУСОМ ПОРАЖЕНИЯ "
        u8"ДО ПЯТИ МЕТРОВ."},
    Utf8Translation{"A popular assault rifle, the K3G4 is commonly armed with "
                    "Teflon-coated bullets, making it a deadly weapon capable "
                    "of cutting through most standard-issue flak jackets like "
                    "a hot knife through butter.",
                    u8"K3G4 - ПОПУЛЯРНАЯ ШТУРМОВАЯ ВИНТОВКА. ДЛЯ НЕЁ ЧАСТО "
                    u8"ИСПОЛЬЗУЮТ ПУЛИ С ТЕФЛОНОВЫМ ПОКРЫТИЕМ, КОТОРЫЕ ЛЕГКО "
                    u8"ПРОБИВАЮТ БОЛЬШИНСТВО ШТАТНЫХ БРОНЕЖИЛЕТОВ."},
    Utf8Translation{
        "Developed in secret by the viral research branch of PHARCOM Inc., "
        "this device is capable of detecting trace particles of the Syphon "
        "Filter virus from up to 50 meters away. It can also scan through some "
        "solid objects and provide visual feedback of their contents.",
        u8"ЭТО УСТРОЙСТВО БЫЛО ТАЙНО РАЗРАБОТАНО ВИРУСОЛОГИЧЕСКИМ ОТДЕЛОМ "
        u8"КОРПОРАЦИИ ФАРКОМ. ОНО ОБНАРУЖИВАЕТ СЛЕДЫ ВИРУСА СИФОН ФИЛЬТР НА "
        u8"РАССТОЯНИИ ДО 50 МЕТРОВ, А ТАКЖЕ СКАНИРУЕТ НЕКОТОРЫЕ ТВЁРДЫЕ "
        u8"ОБЪЕКТЫ И ПОКАЗЫВАЕТ ИХ СОДЕРЖИМОЕ."},
    Utf8Translation{
        "Upon detonation, this incendiary weapon spreads ammonium perchlorate "
        "three meters outwards from the blast point. It is instantly ignited "
        "by the explosion and quickly burns out, fatally burning anyone "
        "nearby, but leaving little collateral damage in the terrain.",
        u8"ПРИ ВЗРЫВЕ ЭТА ЗАЖИГАТЕЛЬНАЯ ГРАНАТА РАЗБРАСЫВАЕТ ПЕРХЛОРАТ АММОНИЯ "
        u8"В РАДИУСЕ ТРЁХ МЕТРОВ. ВЕЩЕСТВО МГНОВЕННО ВОСПЛАМЕНЯЕТСЯ И БЫСТРО "
        u8"ВЫГОРАЕТ, СМЕРТЕЛЬНО ОБЖИГАЯ ВСЕХ ПОБЛИЗОСТИ, НО ПОЧТИ НЕ ПОВРЕЖДАЯ "
        u8"ОКРУЖЕНИЕ."},
    Utf8Translation{
        "Primarily used as a stealth weapon against multiple targets, this "
        "grenade releases trace amounts of Soman nerve agent into the air. The "
        "gas quickly dissipates, but not before rendering victims unconscious. "
        "If no antidote is administered, death follows within 15 minutes.",
        u8"ЭТА ГРАНАТА ПРЕДНАЗНАЧЕНА ДЛЯ СКРЫТНОГО ПОРАЖЕНИЯ НЕСКОЛЬКИХ ЦЕЛЕЙ "
        u8"И ВЫДЕЛЯЕТ В ВОЗДУХ НЕРВНО-ПАРАЛИТИЧЕСКИЙ ГАЗ ЗОМАН. ГАЗ БЫСТРО "
        u8"РАССЕИВАЕТСЯ, НО УСПЕВАЕТ ЛИШИТЬ ЖЕРТВ СОЗНАНИЯ. БЕЗ АНТИДОТА "
        u8"СМЕРТЬ НАСТУПАЕТ В ТЕЧЕНИЕ 15 МИНУТ."},
    Utf8Translation{
        "Standard equipment for all agency operatives, this flashlight is "
        "shockproof and charged by a 300 hour battery.",
        u8"СТАНДАРТНОЕ СНАРЯЖЕНИЕ ВСЕХ ОПЕРАТИВНИКОВ АГЕНТСТВА. УДАРОПРОЧНЫЙ "
        u8"ФОНАРЬ РАБОТАЕТ ОТ БАТАРЕИ, РАССЧИТАННОЙ НА 300 ЧАСОВ."},
    Utf8Translation{"A standard magnetic-strip card key.",
                    u8"СТАНДАРТНАЯ КЛЮЧ-КАРТА С МАГНИТНОЙ ПОЛОСОЙ."},
    Utf8Translation{
        "These incendiary blocks are made of a putty-like material which can "
        "be molded to the user's liking. The C4 explosive putty is then wired "
        "to a fuse and a friction igniter, allowing the user to detonate the "
        "explosive from a distant or protected position.",
        u8"ПЛАСТИЧНЫЕ ЗАРЯДЫ С4 МОЖНО ФОРМОВАТЬ ПО МЕСТУ УСТАНОВКИ. К "
        u8"ВЗРЫВЧАТКЕ ПОДКЛЮЧАЮТСЯ ДЕТОНАТОР И ЗАПАЛ, ЧТО ПОЗВОЛЯЕТ ПОДОРВАТЬ "
        u8"ЗАРЯД С БЕЗОПАСНОГО РАССТОЯНИЯ ИЛИ ИЗ УКРЫТИЯ."},
    Utf8Translation{
        "This device is used to subcutanelously inject a fine stream of fluid "
        "under high pressure without puncturing the skin, and is loaded with "
        "an experimental serum capable of counteracting the effects of the "
        "Syphon Filter virus.",
        u8"ЭТО БЕЗЫГОЛЬНЫЙ ИНЪЕКТОР, КОТОРЫЙ ВВОДИТ ПОД КОЖУ ТОНКУЮ СТРУЮ "
        u8"ЖИДКОСТИ ПОД ВЫСОКИМ ДАВЛЕНИЕМ. ОН ЗАРЯЖЕН ЭКСПЕРИМЕНТАЛЬНОЙ "
        u8"СЫВОРОТКОЙ, СПОСОБНОЙ ПРОТИВОДЕЙСТВОВАТЬ ВИРУСУ СИФОН ФИЛЬТР."},
    Utf8Translation{"Standard Agency side-arm with a sound suppressor.",
                    u8"ШТАТНЫЙ ПИСТОЛЕТ АГЕНТСТВА С ГЛУШИТЕЛЕМ."},
    Utf8Translation{
        "Fires a probe and delivers a sustained electrical charge.",
        u8"ВЫСТРЕЛИВАЕТ ЗОНД И ПОДАЁТ ДЛИТЕЛЬНЫЙ ЭЛЕКТРИЧЕСКИЙ РАЗРЯД."},
    Utf8Translation{"Shockproof Agency flashlight with a long-life battery.",
                    u8"УДАРОПРОЧНЫЙ ФОНАРЬ АГЕНТСТВА С ДОЛГОВЕЧНОЙ БАТАРЕЕЙ."},
    Utf8Translation{"Detects trace particles of the Syphon Filter virus.",
                    u8"ОБНАРУЖИВАЕТ СЛЕДЫ ВИРУСА СИФОН ФИЛЬТР."},
    Utf8Translation{"Incendiary grenade with a short lethal blast radius.",
                    u8"ЗАЖИГАТЕЛЬНАЯ ГРАНАТА С НЕБОЛЬШИМ РАДИУСОМ ПОРАЖЕНИЯ."},
    Utf8Translation{
        "Releases a rapidly dissipating incapacitating nerve agent.",
        u8"ВЫДЕЛЯЕТ БЫСТРО РАССЕИВАЮЩИЙСЯ НЕРВНО-ПАРАЛИТИЧЕСКИЙ ГАЗ."},
    Utf8Translation{"Agency field weapon.", u8"ПОЛЕВОЕ ОРУЖИЕ АГЕНТСТВА."},
};

struct Utf8MissionBriefing {
  std::u8string_view location;
  std::string_view mission_title;
  std::string_view date_time;
  std::u8string_view directive;
  std::u8string_view additional_directive;
};

// Faithful, proofread translations of the English SCUS-94240 briefing data.
// Do not inherit prose from the old fan translation: its character table and
// several names, places and mission details are incorrect.
constexpr std::array localized_briefings{
    Utf8MissionBriefing{
        u8"ВАШИНГТОН, ОКРУГ КОЛУМБИЯ", "Georgia Street", "08/23 22:45",
        u8"ДИРЕКТИВА АГЕНТСТВА:\n\n"
        u8"НАШ ИНФОРМАТОР В ФБР УСТАНОВИЛ МЕСТО ПЛАНИРУЕМОЙ "
        u8"ТЕРРОРИСТИЧЕСКОЙ АТАКИ С ПРИМЕНЕНИЕМ ВИРУСА - МЕТРО ВАШИНГТОНА. "
        u8"ИСТОЧНИКИ В МИНИСТЕРСТВЕ ОБОРОНЫ США И ИНТЕРПОЛЕ "
        u8"ПОДТВЕРДИЛИ ЛИЧНОСТИ ТЕРРОРИСТОВ. ВЫ БУДЕТЕ ВЫСАЖЕНЫ ПОСЛЕ "
        u8"НАЧАЛА ОПЕРАЦИИ КОМАНДОВАНИЯ ХИМИЧЕСКОЙ И БИОЛОГИЧЕСКОЙ ЗАЩИТЫ "
        u8"АРМИИ США (CBDC).",
        u8"ВАШИ ЦЕЛИ - ЭРИХ РОМЕР, ПАВЕЛ КРАВИЧ, МАРА АРАМОВА И АНТОН ГИРДО. "
        u8"СПУТНИКОВАЯ СЛУЖБА ПЕРЕХВАТИЛА КОДИРОВАННЫЙ РАДИООБМЕН. В ЭТОМ "
        u8"РАЙОНЕ ДОЛЖНА НАХОДИТЬСЯ СТАНЦИЯ СВЯЗИ РОМЕРА. ПРИ НЕОБХОДИМОСТИ "
        u8"ПОМОГИТЕ СЛУЖБЕ CBDC. ПО ВОЗМОЖНОСТИ ИЗБЕГАЙТЕ ЖЕРТВ СРЕДИ "
        u8"МИРНЫХ ЖИТЕЛЕЙ. ДОПОЛНИТЕЛЬНЫЕ СВЕДЕНИЯ СМОТРИТЕ В СПИСКЕ ЦЕЛЕЙ."},
    Utf8MissionBriefing{
        u8"ВАШИНГТОН, ОКРУГ КОЛУМБИЯ", "Destroyed subway", "08/23 23:45",
        u8"СООБЩЕНИЕ ОТ ЭДВАРДА БЕНТОНА:\n\n"
        u8"ТЕБЕ ПОВЕЗЛО ОСТАТЬСЯ В ЖИВЫХ. ДЕСЯТЬ КВАРТАЛОВ В ЦЕНТРЕ "
        u8"ВАШИНГТОНА ТОЛЬКО ЧТО ПРОВАЛИЛИСЬ НА ДВАДЦАТЬ ФУТОВ. ПОХОЖЕ, "
        u8"ЭРИХ РОМЕР ЗАМИНИРОВАЛ ВСЮ ПОДЗЕМКУ КАК ЧАСТЬ ПЛАНА "
        u8"ОТСТУПЛЕНИЯ.",
        u8"СКОРЕЕ ВСЕГО, ТЫ СОРВАЛ ЕГО ГРАФИК, И ОН ВСЁ ЕЩЁ НАХОДИТСЯ РЯДОМ. "
        u8"МАРА АРАМОВА И АНТОН ГИРДО ТАКЖЕ НЕ НАЙДЕНЫ. НЕ ДАЙ ЭРИХУ "
        u8"РОМЕРУ ПОКИНУТЬ ТЕРМИНАЛ. УЧТИ: ТЫ ПОЛНОСТЬЮ ОТРЕЗАН ОТ "
        u8"ПОДКРЕПЛЕНИЙ."},
    Utf8MissionBriefing{
        u8"ВАШИНГТОН, ОКРУГ КОЛУМБИЯ", "Main subway line", "08/24 00:30",
        u8"СООБЩЕНИЕ ОТ ЭДВАРДА БЕНТОНА:\n\n"
        u8"ЛОГАН, ПО ДАННЫМ РАЗВЕДКИ, РОМЕР ПОКИНУЛ МЕСТО ОПЕРАЦИИ. ГРУППА "
        u8"ДЖЕНКИНСА СТОЛКНУЛАСЬ С СЕРЬЁЗНЫМ СОПРОТИВЛЕНИЕМ В РАЙОНЕ "
        u8"ВАШИНГТОН-ПАРКА, А ПОЛИЦИЯ ОКРУГА КОЛУМБИЯ СООБЩАЕТ О ПЯТНАДЦАТИ "
        u8"ПОГИБШИХ СОТРУДНИКАХ. МЫ ОПАСАЕМСЯ, ЧТО УТЕЧКА В АГЕНТСТВЕ "
        u8"ВЫДАЛА ЕГО ПОЗИЦИЮ.",
        u8"ИНТЕРПОЛ ПОДТВЕРДИЛ ЛИЧНОСТЬ МАРЫ АРАМОВОЙ. НЕ ДАЙ ЕЙ СКРЫТЬСЯ. "
        u8"ВНИМАНИЕ: ФЕДЕРАЛЬНОЕ АГЕНТСТВО ПО ЧРЕЗВЫЧАЙНЫМ СИТУАЦИЯМ "
        u8"ИСПОЛЬЗУЕТ ПУТИ ВОСТОЧНОГО НАПРАВЛЕНИЯ ДЛЯ ПЕРЕВОЗКИ СПАСАТЕЛЕЙ. "
        u8"НЕ ПРИМЕНЯЙ В ТОННЕЛЕ ВЗРЫВЧАТКУ, ИНАЧЕ ПОЕЗД СОЙДЁТ С РЕЛЬСОВ."},
    Utf8MissionBriefing{
        u8"ВАШИНГТОН, ОКРУГ КОЛУМБИЯ", "Washington Park", "08/24 00:45",
        u8"СООБЩЕНИЕ ОТ ЭДВАРДА БЕНТОНА:\n\n"
        u8"ИЗМЕНЕНИЕ ЗАДАНИЯ: КОМАНДОВАНИЕ ХИМИЧЕСКОЙ И БИОЛОГИЧЕСКОЙ "
        u8"ЗАЩИТЫ ЗАПРОСИЛО ПРЯМОЕ ВМЕШАТЕЛЬСТВО И ПОМОЩЬ. НОВЫЕ ПРИКАЗЫ: "
        u8"НАЙТИ ВИРУСНЫЕ БОМБЫ, УСТАНОВИТЬ МАЯКИ, ДОЖДАТЬСЯ ПРИБЫТИЯ "
        u8"СЛУЖБЫ CBDC И ОБЕСПЕЧИТЬ ОГНЕВОЕ ПРИКРЫТИЕ. БОМБЫ ВЗОРВУТСЯ "
        u8"В ТЕЧЕНИЕ ЧАСА.",
        u8"СОПРОТИВЛЕНИЕ ТЕРРОРИСТОВ ОЧЕНЬ СИЛЬНОЕ. ПО ДАННЫМ АГЕНТСТВА, "
        u8"ОПЕРАЦИЕЙ В ПАРКЕ РУКОВОДИТ АНТОН ГИРДО. БУДЬ ОСТОРОЖЕН. СВЯЗЬ С "
        u8"ГРУППОЙ ДЖЕНКИНСА ПОТЕРЯНА."},
    Utf8MissionBriefing{
        u8"ВАШИНГТОН, ОКРУГ КОЛУМБИЯ", "Freedom Memorial", "08/24 01:15",
        u8"ПЕРЕДАНО РАЗВЕДКОЙ АГЕНТСТВА:\n\n"
        u8"СПУТНИК-ШПИОН ЗАВЕРШИЛ ПОДРОБНЫЙ АНАЛИЗ БРОНИ АНТОНА ГИРДО: ОНА "
        u8"НЕУЯЗВИМА ДЛЯ ОБЫЧНОГО ОРУЖИЯ. НАШИ СПЕЦИАЛИСТЫ ИЩУТ РЕШЕНИЕ, "
        u8"НО ВАШЕМУ АГЕНТУ, ВОЗМОЖНО, ПРИДЁТСЯ ИМПРОВИЗИРОВАТЬ И ИСКАТЬ "
        u8"СЛАБОЕ "
        u8"МЕСТО.",
        u8"НАШ ДЕМИНЕР УСТАНОВИЛ: ЛЮБОЙ ВЗРЫВ ПРИВЕДЁТ В "
        u8"ДЕЙСТВИЕ ВИРУСНУЮ БОМБУ."},
    Utf8MissionBriefing{
        u8"НЬЮ-ЙОРК", "Expo Center Reception", "08/25 19:00",
        u8"СООБЩЕНИЕ ОТ ТОМАСА МАРКИНСОНА:\n\n"
        u8"Я ОДОБРИЛ ТВОЙ ЗАПРОС НА ДОСТУП К ДОСЬЕ ФБР НА ДЖОНАТАНА ФЭЙГАНА. "
        u8"ВОЗМОЖНО, ТВОИ ПОДОЗРЕНИЯ ВЕРНЫ. ВЕЧЕРНИЙ ПРИЕМ ФАРКОМ МОЖЕТ БЫТЬ "
        u8"ПРИКРЫТИЕМ ДЛЯ ВСТРЕЧИ ФЭЙГАНА С РОМЕРОМ. НЕ ВЫПУСКАЙ ФЭЙГАНА "
        u8"ИЗ ВИДУ.",
        u8"ТЕБЯ НЕ ДОЛЖНЫ ЗАМЕТИТЬ ИЛИ ЗАХВАТИТЬ. ОХРАНУ ФЭЙГАНА "
        u8"РАЗРЕШЕНО УСТРАНЯТЬ, НО ТОЛЬКО ИЗ ОРУЖИЯ С ГЛУШИТЕЛЕМ. ПОСЛЕ "
        u8"НАБЛЮДЕНИЯ ЗА ВСТРЕЧЕЙ ЗАХВАТИ ФЭЙГАНА ЖИВЫМ ЛЮБОЙ ЦЕНОЙ. "
        u8"ОФИЦИАЛЬНО АГЕНТСТВО НИЧЕГО ОБ ЭТОМ НЕ ЗНАЕТ."},
    Utf8MissionBriefing{
        u8"НЬЮ-ЙОРК", "Expo Center Dinorama", "08/25 19:15",
        u8"СООБЩЕНИЕ ОТ ЛИАН СИН:\n\n"
        u8"АРАМОВА ДОПРАШИВАЕТ ФЭЙГАНА У ЭКСПОЗИЦИИ ДИНОЗАВРОВ. АУДИОКАНАЛЫ "
        u8"ПРОПАДАЮТ, НО ОНА ПОСТОЯННО УПОМИНАЕТ СИФОН ФИЛЬТР.",
        u8"У МАРЫ АРАМОВОЙ И ДЖОНАТАНА ФЭЙГАНА ЕСТЬ НУЖНЫЕ НАМ СВЕДЕНИЯ. ТЫ "
        u8"ДОЛЖЕН ПОМЕШАТЬ АРАМОВОЙ УБИТЬ ФЭЙГАНА, НО АРАМОВА ТАКЖЕ ДОЛЖНА "
        u8"УЦЕЛЕТЬ."},
    Utf8MissionBriefing{
        u8"РОЗОВКА, КАЗАХСТАН", "Rhoemer's Base", "09/01 21:00",
        u8"СООБЩЕНИЕ ОТ ТОМАСА МАРКИНСОНА:\n\n"
        u8"Я ЗНАЮ, ЧТО ПОДОБНЫЕ ВОЕННЫЕ ОПЕРАЦИИ ОБЫЧНО ПРОВОДЯТ НАШИ "
        u8"АГЕНТЫ ИЗ АРМЕЙСКИХ РЕЙНДЖЕРОВ, НО ПО ОЧЕВИДНЫМ ПРИЧИНАМ Я "
        u8"ПОРУЧАЮ ЭТО ТЕБЕ: НАЙДИ И СОСТАВЬ ОПИСЬ РАКЕТНОГО АРСЕНАЛА "
        u8"БАЗЫ, УСТАНОВИ ЗАРЯДЫ C4 В КЛЮЧЕВЫХ ТОЧКАХ И УСТРАНИ "
        u8"ГАБРЕКА, КОМАНДИРА БАЗЫ.",
        u8"ОТКЛЮЧИ РАДАР ПРОТИВОВОЗДУШНОЙ ОБОРОНЫ БАЗЫ, ЧТОБЫ ЛИАН МОГЛА "
        u8"НАЧАТЬ ЭВАКУАЦИЮ. ПОСЛЕ УСТАНОВКИ ВЗРЫВЧАТКИ У ТЕБЯ ОСТАНЕТСЯ МАЛО "
        u8"ВРЕМЕНИ, ЧТОБЫ ДОБРАТЬСЯ ДО ТОЧКИ ЭВАКУАЦИИ."},
    Utf8MissionBriefing{
        u8"РОЗОВКА, КАЗАХСТАН", "Base Bunker", "09/01 21:30",
        u8"СООБЩЕНИЕ ОТ ЛИАН СИН:\n\n"
        u8"ГЕЙБ, ВСЕ УСТАНОВЛЕННЫЕ ТОБОЙ ЗАРЯДЫ ВЗВЕДЕНЫ И ГОТОВЫ К ПОДРЫВУ. "
        u8"ТЕПЕРЬ НУЖНО ТОЛЬКО ЗАРЕГИСТРИРОВАТЬ РАКЕТЫ SS-23 И ДОБРАТЬСЯ ДО "
        u8"КРЫШИ. ПО МЕРЕ ПРОВЕРКИ КАЖДОЙ РАКЕТЫ Я БУДУ ПЕРЕДАВАТЬ КОДЫ "
        u8"ЗАПУСКА В ШТАБ. НЕ ЗАКРЫВАЙ КАНАЛ СВЯЗИ.",
        u8"ПРИ ВХОДЕ В БУНКЕР СРАБОТАЛ ДАТЧИК ДВИЖЕНИЯ. БАЗА ПОДНЯТА ПО "
        u8"ТРЕВОГЕ, ПОЭТОМУ ВЕРНУТЬСЯ НА ПОВЕРХНОСТЬ ПРЕЖНИМ ПУТЁМ НЕ УДАСТСЯ. "
        u8"НА СПУТНИКОВЫХ СНИМКАХ ВИДЕН ГРУЗОВОЙ ЛИФТ НА КРЫШЕ. ВЕРОЯТНО, ОН "
        u8"СВЯЗАН С БУНКЕРОМ. ЭТО ТВОЙ ПУТЬ НАРУЖУ."},
    Utf8MissionBriefing{
        u8"РОЗОВКА, КАЗАХСТАН", "Base Tower", "09/01 21:40",
        u8"СООБЩЕНИЕ ОТ ЛИАН СИН:\n\n"
        u8"ПАТРУЛИ РОМЕРА ОБНАРУЖИЛИ МЕНЯ И ОТКРЫЛИ ПЛОТНЫЙ ОГОНЬ. ТЕБЕ "
        u8"ЛУЧШЕ СКОРЕЕ ОТКЛЮЧИТЬ РАДАР, ИНАЧЕ Я НЕ СМОГУ ВЗЛЕТЕТЬ. ПАНЕЛЬ "
        u8"УПРАВЛЕНИЯ ДОЛЖНА НАХОДИТЬСЯ У ЕГО ОСНОВАНИЯ.",
        u8"ГРУППА ТОМАСА МАРКИНСОНА БУДЕТ НАГОТОВЕ НА СЛУЧАЙ НЕПРЕДВИДЕННОГО. "
        u8"ПОТОРОПИСЬ, ГЕЙБ."},
    Utf8MissionBriefing{
        u8"РОЗОВКА, КАЗАХСТАН", "Base Escape", "09/01 21:45",
        u8"СООБЩЕНИЕ ОТ ТОМАСА МАРКИНСОНА:\n\n"
        u8"ТАЙМЕРЫ НА УСТАНОВЛЕННОЙ ТОБОЙ ВЗРЫВЧАТКЕ ЗАПУЩЕНЫ. КОГДА ЗАРЯДЫ "
        u8"СРАБОТАЮТ, ОСТАНЕТСЯ ВОРОНКА ШИРИНОЙ В МИЛЮ. НАШ ВЕРТОЛЁТ ЗАБЕРЁТ "
        u8"ТЕБЯ У ГЛАВНЫХ ВОРОТ, КАК ТОЛЬКО ТЫ ДОБЕРЕШСЯ ДО НИХ. "
        u8"ПОТОРОПИСЬ, У НАС МАЛО ВРЕМЕНИ!",
        u8"Я ОТПРАВИЛ ГРУППУ К ТОЧКЕ ВСТРЕЧИ С ЛИАН СИН. ОНИ НАШЛИ ТОЛЬКО "
        u8"ГОРЯЩИЕ ОБЛОМКИ. ЭРИХ РОМЕР ДОРОГО ЗА ЭТО ЗАПЛАТИТ И ОЧЕНЬ СКОРО."},
    Utf8MissionBriefing{
        u8"УЖГОРОД, УКРАИНА", "Rhoemer's Stronghold", "09/07 06:30",
        u8"СООБЩЕНИЕ ОТ ТОМАСА МАРКИНСОНА:\n\n"
        u8"ТЕБЕ НУЖНО СПУСТИТЬСЯ В КАТАКОМБЫ, ГДЕ ДЕРЖАТ ФЭЙГАНА. ПО ПУТИ "
        u8"УСТРАНИ ВСЕХ УЧЁНЫХ ФАРКОМ: Я ХОЧУ ПОЛНОСТЬЮ ПРЕКРАТИТЬ ИХ "
        u8"ДЕЯТЕЛЬНОСТЬ ЗДЕСЬ.",
        u8"НАШИ ЛАБОРАТОРИИ ПОДГОТОВИЛИ АНТИГЕН, КОТОРЫЙ СДЕРЖИВАЕТ ДЕЙСТВИЕ "
        u8"ВИРУСА. ВВЕДИ ЕГО КАЖДОМУ НАЙДЕННОМУ ПОДОПЫТНОМУ. АГЕНТСТВУ НУЖНЫ "
        u8"ЭТИ ЛЮДИ ЖИВЫМИ, ПОЭТОМУ СДЕЛАЙ ВСЁ ВОЗМОЖНОЕ ДЛЯ ИХ СПАСЕНИЯ."},
    Utf8MissionBriefing{
        u8"УЖГОРОД, УКРАИНА", "Stronghold lower level", "09/07 07:15",
        u8"СООБЩЕНИЕ ОТ ТОМАСА МАРКИНСОНА:\n\n"
        u8"ИНЦИДЕНТ В КАЗАХСТАНЕ ВЫЗВАЛ СКАНДАЛ В ГОСДЕПАРТАМЕНТЕ, А СОВЕТ "
        u8"ООН В ЯРОСТИ. АГЕНТСТВО, РАЗУМЕЕТСЯ, ПЕРЕКЛАДЫВАЕТ ВИНУ НА "
        u8"КОМАНДОВАНИЕ НАТО. СЕЙЧАС ТЫ НА ПОЛПУТИ ЧЕРЕЗ КРЕПОСТЬ РОМЕРА. ПО "
        u8"СХЕМАМ ИНТЕРПОЛА ВХОД В КАТАКОМБЫ, ВЕРОЯТНО, НАХОДИТСЯ В ЧАСОВНЕ "
        u8"РОЗ. НАЙДИ ЭТУ ЧАСОВНЮ.",
        u8"ВСЕ ЦЕЛИ МИССИИ ОСТАЮТСЯ В СИЛЕ: ЛИКВИДИРУЙ УЧЁНЫХ ФАРКОМ, "
        u8"ВВЕДИ АНТИГЕН ПОДОПЫТНЫМ И НАЙДИ ДЖОНАТАНА ФЭЙГАНА."},
    Utf8MissionBriefing{
        u8"УЖГОРОД, УКРАИНА", "Stronghold catacombs", "09/07 08:00",
        u8"СООБЩЕНИЕ ОТ ТОМАСА МАРКИНСОНА:\n\n"
        u8"ЕСЛИ ЭТИ КАТАКОМБЫ ПОХОЖИ НА ТЕ, ЧТО МЫ ЗАКРЫЛИ ПОД ПАРИЖЕМ, ТО "
        u8"ДЛЯ ОХРАНЫ РОМЕР ИСПОЛЬЗУЕТ СИСТЕМУ РАСПОЗНАВАНИЯ ЛАДОНИ. ТЕБЕ "
        u8"ПРИДЁТСЯ ПРОСЛЕДОВАТЬ ЗА ОХРАННИКОМ ИЛИ СОТРУДНИКОМ ЛАБОРАТОРИИ "
        u8"РОМЕРА ДО КАМЕРЫ ФЭЙГАНА И ДОЖДАТЬСЯ, ПОКА ОН ОТКРОЕТ ДВЕРЬ. "
        u8"ПОМНИ: ФЭЙГАН НУЖЕН МНЕ ЖИВЫМ.",
        u8"АГЕНТСТВО ПРЕКРАТИЛО КОНТАКТЫ С ГОСДЕПАРТАМЕНТОМ. НЕ ОТВЕЧАЙ НА "
        u8"СООБЩЕНИЯ ОТ ДРУГИХ СОТРУДНИКОВ АГЕНТСТВА. У НАС КОНЧАЕТСЯ "
        u8"ВРЕМЯ."},
    Utf8MissionBriefing{
        u8"АЛМАТЫ, КАЗАХСТАН", "PHARCOM warehouses", "09/08 03:00",
        u8"СООБЩЕНИЕ ОТ ЛИАН СИН:\n\n"
        u8"ЛЮДИ РОМЕРА КРУПНЫМИ СИЛАМИ АТАКУЮТ СКЛАДСКОЙ РАЙОН ФЭЙГАНА. ТЫ "
        u8"ДОЛЖЕН ДОБРАТЬСЯ ДО ЦЕНТРАЛЬНЫХ КОМПЬЮТЕРОВ ШАХТЫ РАНЬШЕ НИХ. "
        u8"СПУТНИКИ АГЕНТСТВА НЕ РАБОТАЮТ, НО АРАМОВА УВЕРЯЕТ, ЧТО ПОПАСТЬ "
        u8"В ШАХТУ МОЖНО ТОЛЬКО ЧЕРЕЗ СЛУЖЕБНЫЕ ЛИФТЫ НА СКЛАДЕ 76. КОДЫ "
        u8"ДОСТУПА ЕСТЬ У ЭРИКСОНА - СНАЧАЛА НАЙДИ ЕГО.",
        u8"НА ЭТОМ СКЛАДЕ ФАРКОМ СОБИРАЛА ТЕЛА, КОТОРЫЕ ИСПОЛЬЗОВАЛИСЬ ДЛЯ "
        u8"ПЕРЕВОЗКИ ВИРУСА. УСТАНОВИ МАЯК НА КАЖДОМ НАЙДЕННОМ ТЕЛЕ, ЧТОБЫ "
        u8"АГЕНТСТВО ДОСТАВИЛО ИХ НА ОБЪЕКТ БИОЛОГИЧЕСКОЙ ЗАЩИТЫ ПЯТОГО УРОВНЯ "
        u8"ДЛЯ УНИЧТОЖЕНИЯ."},
    Utf8MissionBriefing{
        u8"АЛМАТЫ, КАЗАХСТАН", "PHARCOM elite guards", "09/08 03:25",
        u8"СООБЩЕНИЕ ОТ ЛИАН СИН:\n\n"
        u8"Я ВСЁ ЕЩЁ НЕ МОГУ СВЯЗАТЬСЯ С МАРКИНСОНОМ, НО ВЗЛОМАЛА "
        u8"ЕВРОПЕЙСКУЮ КОМПЬЮТЕРНУЮ СЕТЬ АГЕНТСТВА. ПЛОХИЕ НОВОСТИ: СКОРЕЕ "
        u8"ВСЕГО, ТЕБЕ ПРИДЁТСЯ СТОЛКНУТЬСЯ С ЭЛИТНОЙ ОХРАНОЙ ФАРКОМ - "
        u8"ОТЛИЧНО ОБУЧЕННОЙ И ВООРУЖЁННОЙ. ДАЖЕ ЛЮДИ РОМЕРА ЕЩЁ НЕ "
        u8"ПРОРВАЛИ ИХ ПЕРИМЕТР.",
        u8"ПО ВОЗМОЖНОСТИ ИЗБЕГАЙ ОТКРЫТЫХ ПЕРЕСТРЕЛОК. СКЛАД 76 ДОЛЖЕН "
        u8"НАХОДИТЬСЯ К ЮГУ ОТ ТЕБЯ. ГДЕ, ЧЁРТ ВОЗЬМИ, ПОДКРЕПЛЕНИЕ АГЕНТСТВА? "
        u8"НАТО СЧИТАЕТ, ЧТО НАЧАЛАСЬ ГРАЖДАНСКАЯ ВОЙНА, НО НАМ НИКТО НЕ "
        u8"ПОМОГАЕТ."},
    Utf8MissionBriefing{
        u8"АЛМАТЫ, КАЗАХСТАН", "Warehouse 76", "09/08 04:00",
        u8"СООБЩЕНИЕ ОТ ЛИАН СИН:\n\n"
        u8"ВЕСЬ СКЛАДСКОЙ РАЙОН ОХВАЧЕН ОГНЁМ. НУЖНО НАЙТИ ГРУЗОВОЙ ЛИФТ, "
        u8"ПРЕЖДЕ ЧЕМ СКЛАД 76 РУХНЕТ ВОКРУГ ТЕБЯ. ПО СЛОВАМ АРАМОВОЙ, ОН "
        u8"НАХОДИТСЯ В ЮГО-ВОСТОЧНОМ УГЛУ ЗДАНИЯ. ВРЕМЕНИ ПОЧТИ НЕТ.",
        u8"Я ПРОВЕЛА ЭКСПРЕСС-АНАЛИЗ СЫВОРОТКИ, КОТОРУЮ ТОМАС МАРКИНСОН "
        u8"ПРИКАЗАЛ ВВОДИТЬ ПОДОПЫТНЫМ В КРЕПОСТИ ЭРИХА РОМЕРА. ГЕЙБ, ЭТО БЫЛА "
        u8"НЕ ВАКЦИНА, А КОНЦЕНТРИРОВАННЫЙ ХЛОРИД КАЛИЯ, КОТОРЫЙ ИСПОЛЬЗУЮТ "
        u8"ДЛЯ СМЕРТЕЛЬНЫХ ИНЪЕКЦИЙ. МАРКИНСОН ЗАСТАВИЛ ТЕБЯ УБИТЬ ЭТИХ "
        u8"ЛЮДЕЙ."},
    Utf8MissionBriefing{
        u8"АЛМАТЫ, КАЗАХСТАН", "Silo access tunnels", "09/08 04:15",
        u8"СООБЩЕНИЕ ОТ ЛИАН СИН:\n\n"
        u8"ПО СЛОВАМ АРАМОВОЙ, ВХОД В РАКЕТНУЮ ШАХТУ НАХОДИТСЯ ГДЕ-ТО "
        u8"ВНИЗУ. ИДИ ПО ГЛАВНОМУ ТОННЕЛЮ: ОН ЗАКАНЧИВАЕТСЯ У ЛИФТА ШАХТЫ. "
        u8"ИЗ-ЗА ПОЖАРА НАВЕРХУ НЕКОТОРЫЕ ЛИФТЫ МОГУТ НЕ РАБОТАТЬ.",
        u8"Я ПЕРЕХВАТЫВАЮ ПАНИЧЕСКИЙ РАДИООБМЕН КОМАНДОВАНИЯ НАТО, НО "
        u8"ПО-ПРЕЖНЕМУ НЕ МОГУ СВЯЗАТЬСЯ НИ С КЕМ В АГЕНТСТВЕ."},
    Utf8MissionBriefing{
        u8"АЛМАТЫ, КАЗАХСТАН", "Tunnel blackout", "09/08 04:45",
        u8"СООБЩЕНИЕ ОТ ЛИАН СИН:\n\n"
        u8"ЭЛЕКТРОСЕТЬ ТОННЕЛЕЙ ПОЛНОСТЬЮ ОТКЛЮЧЕНА, ПОЭТОМУ ЛАЗЕРНЫЕ "
        u8"ЗАГРАЖДЕНИЯ ДОЛЖНЫ ИСЧЕЗНУТЬ. У ШАХТЫ СОБСТВЕННЫЙ ИСТОЧНИК "
        u8"ПИТАНИЯ, ТАК ЧТО ТЫ ВСЁ ЕЩЁ СМОЖЕШЬ ПОЛУЧИТЬ ДОСТУП К ГЛАВНЫМ "
        u8"КОМПЬЮТЕРАМ И СКАЧАТЬ ДАННЫЕ ФАРКОМ О СЕКВЕНИРОВАНИИ ДНК.",
        u8"ОТТУДА ДОЛЖЕН БЫТЬ ПРЯМОЙ ПУТЬ К ШАХТЕ, ЕСЛИ УДАСТСЯ ВЫБРАТЬСЯ ИЗ "
        u8"ЭНЕРГЕТИЧЕСКОЙ. И, ГЕЙБ, ЛЮДЕЙ РОМЕРА ВСЁ ЕЩЁ БОЛЬШЕ. ВОЗМОЖНО, "
        u8"ТЕБЕ СТОИТ ВОСПОЛЬЗОВАТЬСЯ ОГРАНИЧЕННОЙ ВИДИМОСТЬЮ В СВОИХ "
        u8"ИНТЕРЕСАХ."},
    Utf8MissionBriefing{
        u8"АЛМАТЫ, КАЗАХСТАН", "Missile Silo", "09/08 05:00",
        u8"СООБЩЕНИЕ ОТ ЛИАН СИН:\n\n"
        u8"ТЫ БЫЛ ПРАВ. ПРОЦЕДУРА ЗАПУСКА ДЕВЯТКИ УЖЕ НАЧАЛАСЬ, И "
        u8"МЫ НЕ МОЖЕМ ЕЁ ОСТАНОВИТЬ. ТЕБЕ ПРИДЁТСЯ НАЙТИ КОДЫ "
        u8"САМОУНИЧТОЖЕНИЯ РАКЕТЫ НА ЕЁ КОРПУСЕ И ВВЕСТИ ИХ В ГЛАВНЫЙ "
        u8"ПУСКОВОЙ КОМПЬЮТЕР. ТОГДА МЫ СМОЖЕМ ПОДОРВАТЬ ЕЁ В ВЕРХНИХ "
        u8"СЛОЯХ АТМОСФЕРЫ.",
        u8"В ПОМЕЩЕНИИ С ГЛАВНЫМ КОМПЬЮТЕРОМ КОМАНДОВАНИЯ ТАКЖЕ НАХОДЯТСЯ "
        u8"ГЕНЕТИЧЕСКИЕ КОДЫ ВИРУСА СИФОН ФИЛЬТР."},
};

constexpr std::optional<char> vitByteForCyrillic(char32_t value) noexcept {
  switch (value) {
  case 0x0410U:
  case 0x0430U:
    return 'a';
  case 0x0411U:
  case 0x0431U:
    return static_cast<char>(0xe3U);
  case 0x0412U:
  case 0x0432U:
    return 'b';
  case 0x0413U:
  case 0x0433U:
    return static_cast<char>(0xf1U);
  case 0x0414U:
  case 0x0434U:
    return static_cast<char>(0xf0U);
  case 0x0415U:
  case 0x0435U:
    return 'e';
  case 0x0401U:
  case 0x0451U:
    return static_cast<char>(0xe9U);
  case 0x0416U:
  case 0x0436U:
    return static_cast<char>(0xe7U);
  case 0x0417U:
  case 0x0437U:
    return 'z';
  case 0x0418U:
  case 0x0438U:
    return static_cast<char>(0xf5U);
  case 0x0419U:
  case 0x0439U:
    return static_cast<char>(0xe5U);
  case 0x041aU:
  case 0x043aU:
    return 'k';
  case 0x041bU:
  case 0x043bU:
    return static_cast<char>(0xf4U);
  case 0x041cU:
  case 0x043cU:
    return 'm';
  case 0x041dU:
  case 0x043dU:
    return 'h';
  case 0x041eU:
  case 0x043eU:
    return 'o';
  case 0x041fU:
  case 0x043fU:
    return static_cast<char>(0xfbU);
  case 0x0420U:
  case 0x0440U:
    return 'p';
  case 0x0421U:
  case 0x0441U:
    return 'c';
  case 0x0422U:
  case 0x0442U:
    return 't';
  case 0x0423U:
  case 0x0443U:
    return 'y';
  case 0x0424U:
  case 0x0444U:
    return static_cast<char>(0xf2U);
  case 0x0425U:
  case 0x0445U:
    return 'x';
  case 0x0426U:
  case 0x0446U:
    return static_cast<char>(0xfcU);
  case 0x0427U:
  case 0x0447U:
    return static_cast<char>(0xf8U);
  case 0x0428U:
  case 0x0448U:
    return static_cast<char>(0xe0U);
  case 0x0429U:
  case 0x0449U:
    return static_cast<char>(0xe1U);
  case 0x042aU:
  case 0x044aU:
    return static_cast<char>(0xeeU);
  case 0x042bU:
  case 0x044bU:
    return 'j';
  case 0x042cU:
  case 0x044cU:
    return static_cast<char>(0xfaU);
  case 0x042dU:
  case 0x044dU:
    return static_cast<char>(0xf3U);
  case 0x042eU:
    return static_cast<char>(0xdfU);
  case 0x044eU:
    return static_cast<char>(0xeaU);
  case 0x042fU:
  case 0x044fU:
    return static_cast<char>(0xf9U);
  default:
    return std::nullopt;
  }
}

static_assert(static_cast<unsigned char>(*vitByteForCyrillic(U'Ю')) == 0xdfU &&
              static_cast<unsigned char>(*vitByteForCyrillic(U'ю')) == 0xeaU);

std::string encodeVit(std::u8string_view source) {
  std::string result;
  result.reserve(source.size());
  for (auto cursor = std::size_t{}; cursor < source.size();) {
    const auto first = static_cast<unsigned char>(source[cursor]);
    char32_t codepoint{};
    auto length = std::size_t{1U};
    if (first < 0x80U) {
      codepoint = first;
    } else if ((first & 0xe0U) == 0xc0U && cursor + 1U < source.size()) {
      codepoint = static_cast<char32_t>(first & 0x1fU) << 6U;
      codepoint |= static_cast<unsigned char>(source[cursor + 1U]) & 0x3fU;
      length = 2U;
    } else if ((first & 0xf0U) == 0xe0U && cursor + 2U < source.size()) {
      codepoint = static_cast<char32_t>(first & 0x0fU) << 12U;
      codepoint |= (static_cast<char32_t>(
                        static_cast<unsigned char>(source[cursor + 1U]) & 0x3fU)
                    << 6U);
      codepoint |= static_cast<unsigned char>(source[cursor + 2U]) & 0x3fU;
      length = 3U;
    } else {
      result.push_back('?');
      ++cursor;
      continue;
    }
    if (codepoint <= 0x7fU) {
      result.push_back(static_cast<char>(codepoint));
    } else if (const auto encoded = vitByteForCyrillic(codepoint)) {
      result.push_back(*encoded);
    } else {
      result.push_back('?');
    }
    cursor += length;
  }
  return result;
}

std::optional<std::uint32_t> readU32(std::span<const std::byte> bytes,
                                     std::size_t &cursor) noexcept {
  if (cursor > bytes.size() || bytes.size() - cursor < 4U) {
    return std::nullopt;
  }
  const auto value =
      std::to_integer<std::uint32_t>(bytes[cursor]) |
      (std::to_integer<std::uint32_t>(bytes[cursor + 1U]) << 8U) |
      (std::to_integer<std::uint32_t>(bytes[cursor + 2U]) << 16U) |
      (std::to_integer<std::uint32_t>(bytes[cursor + 3U]) << 24U);
  cursor += 4U;
  return value;
}

std::optional<std::string> readString(std::span<const std::byte> bytes,
                                      std::size_t &cursor) noexcept {
  const auto length = readU32(bytes, cursor);
  if (!length || *length > 64U * 1024U || cursor > bytes.size() ||
      bytes.size() - cursor < *length) {
    return std::nullopt;
  }
  std::string result(*length, '\0');
  for (std::size_t index = 0U; index < *length; ++index) {
    result[index] = static_cast<char>(
        std::to_integer<unsigned char>(bytes[cursor + index]));
  }
  cursor += *length;
  return result;
}

std::optional<std::string_view>
translationFor(std::string_view source) noexcept {
  using EncodedTranslation = std::pair<std::string_view, std::string>;
  const auto find_in =
      [source](const auto &translations) -> std::optional<std::string_view> {
    for (const auto &[english, translated] : translations) {
      if (english == source) {
        return translated;
      }
    }
    return std::nullopt;
  };
  const auto encode_all = [](const auto &translations) {
    std::vector<EncodedTranslation> encoded;
    encoded.reserve(translations.size());
    for (const auto &[english, translated] : translations) {
      encoded.emplace_back(english, encodeVit(translated));
    }
    return encoded;
  };

  // Both tables own their encoded strings for the lifetime of the process.
  // This lets the non-owning localizeText() API safely serve every curated
  // UTF-8 entry, not just the small historical single-byte table.
  static const auto curated = encode_all(utf8_translations);
  static const auto base = encode_all(base_utf8_translations);
  if (const auto translated = find_in(curated)) {
    return translated;
  }
  return find_in(base);
}

std::optional<std::string> translatedCopyFor(std::string_view source) {
  if (const auto translated = translationFor(source)) {
    return std::string{*translated};
  }
  return std::nullopt;
}

std::string normalizeMissionText(std::string_view source) {
  std::string result;
  result.reserve(source.size());
  auto pending_space = false;
  for (const auto character : source) {
    const auto value = static_cast<unsigned char>(character);
    if (value == ' ' || value == '\t' || value == '\r' || value == '\n') {
      pending_space = !result.empty();
      continue;
    }
    // Retail overlays are inconsistent about trailing full stops, apostrophes
    // and abbreviated labels (for example "comm." versus "comm"). They do
    // not change the objective identity, so omit ASCII punctuation while
    // retaining template markers and alphanumeric content.
    const auto alphanumeric = (value >= 'a' && value <= 'z') ||
                              (value >= 'A' && value <= 'Z') ||
                              (value >= '0' && value <= '9');
    if (!alphanumeric && value != '%') {
      continue;
    }
    if (pending_space) {
      result.push_back(' ');
      pending_space = false;
    }
    result.push_back(value >= 'a' && value <= 'z'
                         ? static_cast<char>(value - 'a' + 'A')
                         : static_cast<char>(value));
  }
  return result;
}

bool isGasGrenadePickupMessage(std::string_view source) {
  auto compact = normalizeMissionText(source);
  std::erase(compact, ' ');
  const auto gas_grenade =
      compact.starts_with("GASGRENADE") || compact.starts_with("GASGRANADE");
  return gas_grenade && compact.find("TAKEN") != std::string::npos;
}

std::optional<std::string>
translatedNormalizedBuiltInCopyFor(std::string_view source) {
  const auto normalized = normalizeMissionText(source);
  if (normalized.empty()) {
    return std::nullopt;
  }
  const auto find_in =
      [&normalized](const auto &translations) -> std::optional<std::string> {
    for (const auto &[english, translated] : translations) {
      if (english.find('%') == std::string_view::npos &&
          normalizeMissionText(english) == normalized) {
        return encodeVit(translated);
      }
    }
    return std::nullopt;
  };
  if (const auto curated = find_in(utf8_translations)) {
    return curated;
  }
  return find_in(base_utf8_translations);
}

struct MissionTemplateArguments {
  std::vector<std::string> numbers;
  std::vector<std::string> suffixes;
};

std::optional<MissionTemplateArguments>
matchMissionTemplate(std::string_view source_template,
                     std::string_view source_text) {
  const auto pattern = normalizeMissionText(source_template);
  const auto value = normalizeMissionText(source_text);
  if (pattern.find('%') == std::string::npos) {
    return std::nullopt;
  }

  MissionTemplateArguments arguments;
  auto pattern_cursor = std::size_t{};
  auto value_cursor = std::size_t{};
  while (pattern_cursor < pattern.size()) {
    const auto placeholder = pattern.find('%', pattern_cursor);
    if (placeholder == std::string::npos) {
      if (value.substr(value_cursor) != pattern.substr(pattern_cursor)) {
        return std::nullopt;
      }
      value_cursor = value.size();
      break;
    }
    if (placeholder + 1U >= pattern.size() ||
        (pattern[placeholder + 1U] != 'D' &&
         pattern[placeholder + 1U] != 'S')) {
      return std::nullopt;
    }
    const auto literal =
        pattern.substr(pattern_cursor, placeholder - pattern_cursor);
    if (!value.substr(value_cursor).starts_with(literal)) {
      return std::nullopt;
    }
    value_cursor += literal.size();
    const auto kind = pattern[placeholder + 1U];
    pattern_cursor = placeholder + 2U;
    if (kind == 'D') {
      const auto begin = value_cursor;
      while (value_cursor < value.size() && value[value_cursor] >= '0' &&
             value[value_cursor] <= '9') {
        ++value_cursor;
      }
      if (begin == value_cursor) {
        return std::nullopt;
      }
      arguments.numbers.emplace_back(value.substr(begin, value_cursor - begin));
      continue;
    }

    const auto following_placeholder = pattern.find('%', pattern_cursor);
    const auto delimiter = pattern.substr(
        pattern_cursor,
        (following_placeholder == std::string::npos ? pattern.size()
                                                    : following_placeholder) -
            pattern_cursor);
    if (delimiter.empty()) {
      if (following_placeholder == std::string::npos) {
        arguments.suffixes.emplace_back(value.substr(value_cursor));
        value_cursor = value.size();
      } else {
        arguments.suffixes.emplace_back();
      }
      continue;
    }
    const auto end = value.find(delimiter, value_cursor);
    if (end == std::string::npos) {
      return std::nullopt;
    }
    arguments.suffixes.emplace_back(
        value.substr(value_cursor, end - value_cursor));
    value_cursor = end;
  }
  if (value_cursor != value.size()) {
    return std::nullopt;
  }
  return arguments;
}

std::string renderMissionTemplate(std::string_view translated,
                                  const MissionTemplateArguments &arguments) {
  std::string result;
  result.reserve(translated.size() + 8U);
  auto number = std::size_t{};
  auto suffix = std::size_t{};
  for (auto cursor = std::size_t{}; cursor < translated.size(); ++cursor) {
    if (translated[cursor] == '%' && cursor + 1U < translated.size()) {
      const auto kind = translated[cursor + 1U];
      if (kind == 'd' && number < arguments.numbers.size()) {
        result += arguments.numbers[number++];
        ++cursor;
        continue;
      }
      if (kind == 's' && suffix < arguments.suffixes.size()) {
        result += arguments.suffixes[suffix++];
        ++cursor;
        continue;
      }
    }
    result.push_back(translated[cursor]);
  }
  return result;
}

std::optional<std::string>
translatedCuratedTemplateCopyFor(std::string_view source) {
  for (const auto &[english, translated] : utf8_translations) {
    if (english.find('%') == std::string_view::npos) {
      continue;
    }
    if (const auto arguments = matchMissionTemplate(english, source)) {
      return renderMissionTemplate(encodeVit(translated), *arguments);
    }
  }
  return std::nullopt;
}

using MissionMenuEntry = std::pair<std::string, std::string>;
using MissionMenuPack = std::vector<std::vector<MissionMenuEntry>>;

struct MissionMenuPackCache {
  std::filesystem::path root;
  bool attempted{};
  std::optional<MissionMenuPack> pack;
};

MissionMenuPackCache mission_menu_cache;

const std::optional<MissionMenuPack> &missionMenuPack() {
  if (mission_menu_cache.attempted && mission_menu_cache.root == pack_root) {
    return mission_menu_cache.pack;
  }
  mission_menu_cache = MissionMenuPackCache{pack_root, true, std::nullopt};
  const auto bytes = readLocalizedAsset("mission_menu.dat");
  constexpr std::array magic{
      std::byte{'S'}, std::byte{'F'}, std::byte{'L'}, std::byte{'M'},
      std::byte{'N'}, std::byte{'U'}, std::byte{'2'}, std::byte{0},
  };
  if (!bytes || bytes->size() < magic.size() + 4U ||
      !std::equal(magic.begin(), magic.end(), bytes->begin())) {
    return mission_menu_cache.pack;
  }
  auto cursor = magic.size();
  const auto count = readU32(*bytes, cursor);
  if (!count || *count > 128U) {
    return mission_menu_cache.pack;
  }
  MissionMenuPack pack;
  pack.reserve(*count);
  for (std::uint32_t mission = 0U; mission < *count; ++mission) {
    const auto entry_count = readU32(*bytes, cursor);
    if (!entry_count || *entry_count > 128U) {
      return mission_menu_cache.pack;
    }
    std::vector<MissionMenuEntry> entries;
    entries.reserve(*entry_count);
    for (std::uint32_t index = 0U; index < *entry_count; ++index) {
      auto source = readString(*bytes, cursor);
      auto translated = readString(*bytes, cursor);
      if (!source || !translated) {
        return mission_menu_cache.pack;
      }
      entries.emplace_back(std::move(*source), std::move(*translated));
    }
    pack.push_back(std::move(entries));
  }
  mission_menu_cache.pack = std::move(pack);
  return mission_menu_cache.pack;
}

std::optional<std::string>
translatedMissionEntry(std::span<const MissionMenuEntry> entries,
                       std::string_view text) {
  const auto normalized = normalizeMissionText(text);
  const auto exact = std::ranges::find_if(entries, [&](const auto &entry) {
    return normalizeMissionText(entry.first) == normalized;
  });
  if (exact != entries.end()) {
    return exact->second;
  }
  for (const auto &[source, translated] : entries) {
    if (const auto arguments = matchMissionTemplate(source, text)) {
      return renderMissionTemplate(translated, *arguments);
    }
  }
  return std::nullopt;
}

std::optional<std::string>
translatedMissionPackCopyFor(std::string_view source) {
  const auto &pack = missionMenuPack();
  if (!pack) {
    return std::nullopt;
  }
  for (const auto &entries : *pack) {
    if (const auto translated = translatedMissionEntry(entries, source)) {
      return translated;
    }
  }
  return std::nullopt;
}

std::string localizeLine(std::string_view source) {
  if (const auto exact = translatedCopyFor(source)) {
    return *exact;
  }
  if (const auto authored = translatedCuratedTemplateCopyFor(source)) {
    return *authored;
  }
  if (const auto mission = translatedMissionPackCopyFor(source)) {
    return *mission;
  }
  if (const auto normalized = translatedNormalizedBuiltInCopyFor(source)) {
    return *normalized;
  }
  if (source.starts_with("- ")) {
    return "- " + localizeLine(source.substr(2U));
  }
  const auto normalized_service = normalizeMissionText(source);
  // The retail pickup builder emits several inconsistent spellings and
  // layouts for the gas grenade: Grenade/Granade, singular/plural, mixed
  // case and occasionally doubled spaces before TAKEN.  Those variants must
  // be recognized as one complete message before the generic spaced-line
  // splitter can leave the English suffix rendered through Cyrillic glyphs.
  if (isGasGrenadePickupMessage(source)) {
    return encodeVit(u8"ПОЛУЧЕНО: ГАЗОВАЯ ГРАНАТА");
  }
  if (normalized_service.starts_with("PRESS ") &&
      normalized_service.find("CONTACT") != std::string::npos) {
    return encodeVit(u8"НАЖМИТЕ %x, ЧТОБЫ СВЯЗАТЬСЯ С ЛИАН");
  }

  // FUN_8005fbd4 builds pickup notifications from the item name followed by
  // either "taken", " bullet taken" or " shell taken". Recompose the full
  // Russian message instead of feeding the already-laid-out English glyphs
  // through the ViT atlas.
  const auto translated_pickup_item =
      [](std::string_view item) -> std::optional<std::string> {
    if (const auto exact = translatedCopyFor(item)) {
      return exact;
    }
    // Guest code also emits upper-case inventory names, while the authored
    // catalogue uses title case.  Match those without duplicating every item.
    return translatedNormalizedBuiltInCopyFor(item);
  };
  constexpr std::array ammo_suffixes{
      std::string_view{" bullet taken"},
      std::string_view{" bullets taken"},
      std::string_view{" shell taken"},
      std::string_view{" shells taken"},
  };
  for (const auto suffix : ammo_suffixes) {
    if (!source.ends_with(suffix)) {
      continue;
    }
    const auto item_source = source.substr(0U, source.size() - suffix.size());
    const auto item = translated_pickup_item(item_source);
    if (!item) {
      break;
    }
    return encodeVit(u8"ПАТРОНЫ: ") + *item;
  }
  constexpr auto taken_suffix = std::string_view{" taken"};
  if (source.ends_with(taken_suffix)) {
    const auto item_source =
        source.substr(0U, source.size() - taken_suffix.size());
    if (const auto item = translated_pickup_item(item_source)) {
      return encodeVit(u8"ПОЛУЧЕНО: ") + *item;
    }
  }
  constexpr auto maximum_suffix = std::string_view{" maxed"};
  if (source.ends_with(maximum_suffix)) {
    return encodeVit(u8"ЗАПАС ПОЛОН");
  }
  if (source.starts_with("Eliminate ")) {
    return encodeVit(u8"ЛИКВИДИРОВАТЬ ЦЕЛЬ");
  }
  if (source.starts_with("Find security ")) {
    return encodeVit(u8"НАЙТИ КЛЮЧ-КАРТУ ОХРАНЫ");
  }

  constexpr std::array dynamic_prefixes{
      std::string_view{"Slot"},          std::string_view{"Active"},
      std::string_view{"Completed"},     std::string_view{"Brightness"},
      std::string_view{"Ammo"},          std::string_view{"Preset config"},
      std::string_view{"Stick Layout"}, std::string_view{"Invert Aim"},
      std::string_view{"Vibration"},
      std::string_view{"Equipped"},      std::string_view{"Selected"},
      std::string_view{"Power"},         std::string_view{"Accuracy"},
      std::string_view{"Fire Rate"},     std::string_view{"Rate"},
      std::string_view{"Damage"},        std::string_view{"Clip Size"},
      std::string_view{"Max Rounds"},    std::string_view{"Change Weapon"},
      std::string_view{"Shoot"},         std::string_view{"Kneel"},
      std::string_view{"Roll/Zoom Out"}, std::string_view{"Step Right"},
      std::string_view{"Step Left"},     std::string_view{"Target Lock"},
      std::string_view{"Use/Zoom In"},   std::string_view{"Aim"},
  };
  for (const auto prefix : dynamic_prefixes) {
    if (!source.starts_with(prefix) || source.size() == prefix.size()) {
      continue;
    }
    const auto separator = source[prefix.size()];
    if (separator != ':' && separator != ' ') {
      continue;
    }
    const auto translated = translatedCopyFor(prefix);
    if (!translated) {
      continue;
    }
    auto suffix = source.substr(prefix.size());
    std::string result{*translated};
    result.append(suffix.substr(0U, separator == ':' ? 2U : 1U));
    suffix.remove_prefix(
        std::min<std::size_t>(suffix.size(), separator == ':' ? 2U : 1U));
    if (const auto translated_suffix = translatedCopyFor(suffix)) {
      result.append(*translated_suffix);
    } else {
      result.append(suffix);
    }
    return result;
  }
  return std::string{source};
}

std::string localizeSpacedLine(std::string_view source) {
  // Retail also inserts doubled spaces inside some pickup messages. Handle
  // those before treating the same spacing as a menu-column separator.
  if (isGasGrenadePickupMessage(source)) {
    return encodeVit(u8"ПОЛУЧЕНО: ГАЗОВАЯ ГРАНАТА");
  }
  std::string result;
  auto cursor = std::size_t{};
  while (cursor < source.size()) {
    auto separator = source.find("  ", cursor);
    if (separator == std::string_view::npos) {
      result += localizeLine(source.substr(cursor));
      break;
    }
    result += localizeLine(source.substr(cursor, separator - cursor));
    auto end = separator + 2U;
    while (end < source.size() && source[end] == ' ') {
      ++end;
    }
    result.append(source.substr(separator, end - separator));
    cursor = end;
  }
  if (source.empty()) {
    return {};
  }
  return result;
}

} // namespace

std::string encodeVitText(std::u8string_view source) {
  return encodeVit(source);
}

void setGameLanguage(GameLanguage value) noexcept {
  if (language.exchange(value) != value) {
    mission_menu_cache = {};
  }
}

GameLanguage gameLanguage() noexcept { return language.load(); }

bool russianLanguageActive() noexcept {
  return gameLanguage() == GameLanguage::russian_vit;
}

void setLocalizationRoot(std::filesystem::path root) {
  pack_root = std::move(root);
  mission_menu_cache = {};
}

const std::filesystem::path &localizationRoot() noexcept { return pack_root; }

bool localizationPackAvailable(GameLanguage value) noexcept {
  if (value == GameLanguage::english) {
    return true;
  }
  try {
    std::error_code error;
    return !pack_root.empty() &&
           std::filesystem::is_regular_file(pack_root / "manifest.txt",
                                            error) &&
           !error;
  } catch (...) {
    return false;
  }
}

std::optional<std::vector<std::byte>>
readLocalizedAsset(std::string_view relative_path) noexcept {
  if (!russianLanguageActive() || pack_root.empty()) {
    return std::nullopt;
  }
  try {
    const auto path = pack_root / std::filesystem::path{relative_path};
    std::ifstream input{path, std::ios::binary | std::ios::ate};
    if (!input) {
      return std::nullopt;
    }
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > 16U * 1024U * 1024U) {
      return std::nullopt;
    }
    std::vector<std::byte> result(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char *>(result.data()),
               static_cast<std::streamsize>(result.size()));
    if (!input ||
        input.gcount() != static_cast<std::streamsize>(result.size())) {
      return std::nullopt;
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::string_view localizeText(std::string_view english) noexcept {
  if (!russianLanguageActive()) {
    return english;
  }
  if (const auto translated = translationFor(english)) {
    return *translated;
  }
  return english;
}

std::string localizeTextCopy(std::string_view english) {
  if (!russianLanguageActive()) {
    return std::string{english};
  }
  if (const auto exact = translatedCopyFor(english)) {
    return *exact;
  }
  // Status builders frequently insert a newline into one formatted message.
  // Match the complete source before processing its individual visual lines;
  // otherwise templates such as the antigen and missile counters are split
  // apart and their English glyphs leak through the ViT Cyrillic page.
  if (const auto authored = translatedCuratedTemplateCopyFor(english)) {
    return *authored;
  }

  std::string result;
  auto cursor = std::size_t{};
  while (cursor <= english.size()) {
    const auto newline = english.find('\n', cursor);
    const auto end =
        newline == std::string_view::npos ? english.size() : newline;
    result += localizeSpacedLine(english.substr(cursor, end - cursor));
    if (newline == std::string_view::npos) {
      break;
    }
    result.push_back('\n');
    cursor = newline + 1U;
  }
  return result;
}

std::optional<std::string_view>
completeGameplayTextSource(std::string_view observed) noexcept {
  const auto normalized = normalizeMissionText(observed);
  // Short prefixes such as "NO" or "S" are ambiguous with ordinary HUD
  // text. Retail has already shown enough of a status to identify it safely
  // once four normalized source characters are present.
  if (normalized.size() < 4U) {
    return std::nullopt;
  }
  auto compact = normalized;
  std::erase(compact, ' ');
  constexpr std::array gas_grenade_pickup_sources{
      std::string_view{"GASGRENADETAKEN"},
      std::string_view{"GASGRANADETAKEN"},
      std::string_view{"GASGRENADESTAKEN"},
      std::string_view{"GASGRANADESTAKEN"},
  };
  if (std::ranges::any_of(gas_grenade_pickup_sources,
                          [&compact](std::string_view candidate) {
                            return candidate.starts_with(compact);
                          })) {
    // The status hook can observe either the full pickup string or the
    // currently revealed glyph prefix. Collapse every retail spelling to one
    // canonical source before Russian substitution; otherwise the early
    // English prefix is sampled from Cyrillic font cells.
    return std::string_view{"Gas Grenade taken"};
  }
  constexpr std::array candidates{
      std::string_view{"Scope Pwr On"},
      std::string_view{"No Target Available"},
      std::string_view{"Mission Parameter Failed"},
      std::string_view{"Mission Failed"},
      std::string_view{"Playing on HARD difficulty"},
      std::string_view{"Mission Objective Failed"},
      std::string_view{"Sniper Rifle"},
      std::string_view{"Nightvision Rifle"},
  };
  std::optional<std::string_view> result;
  for (const auto candidate : candidates) {
    const auto canonical = normalizeMissionText(candidate);
    if (!canonical.starts_with(normalized)) {
      continue;
    }
    if (result) {
      return std::nullopt;
    }
    result = candidate;
  }
  return result;
}

std::optional<LocalizedMissionBriefing>
localizedMissionBriefing(std::uint32_t mission_index) noexcept {
  if (!russianLanguageActive()) {
    return std::nullopt;
  }
  const auto load_pack_record =
      [&]() -> std::optional<LocalizedMissionBriefing> {
    const auto bytes = readLocalizedAsset("briefings.dat");
    constexpr std::array magic{
        std::byte{'S'}, std::byte{'F'}, std::byte{'L'}, std::byte{'B'},
        std::byte{'R'}, std::byte{'F'}, std::byte{'1'}, std::byte{0},
    };
    if (!bytes || bytes->size() < magic.size() + 4U ||
        !std::equal(magic.begin(), magic.end(), bytes->begin())) {
      return std::nullopt;
    }
    auto cursor = magic.size();
    const auto count = readU32(*bytes, cursor);
    if (!count || mission_index >= *count || *count > 128U) {
      return std::nullopt;
    }
    for (std::uint32_t index = 0U; index < *count; ++index) {
      auto location = readString(*bytes, cursor);
      auto title = readString(*bytes, cursor);
      auto date = readString(*bytes, cursor);
      auto directive = readString(*bytes, cursor);
      auto additional = readString(*bytes, cursor);
      if (!location || !title || !date || !directive || !additional) {
        return std::nullopt;
      }
      if (index == mission_index) {
        return LocalizedMissionBriefing{std::move(*location), std::move(*title),
                                        std::move(*date), std::move(*directive),
                                        std::move(*additional)};
      }
    }
    return std::nullopt;
  };

  // Briefing prose is maintained and proofread in the native table above.
  // The ViT pack supplies the legacy glyph atlas and other binary assets, but
  // its briefings.dat must not replace our corrected text for retail missions.
  if (mission_index < localized_briefings.size()) {
    const auto &source = localized_briefings[mission_index];
    LocalizedMissionBriefing result;
    result.location = encodeVit(source.location);
    result.mission_title = translatedCopyFor(source.mission_title)
                               .value_or(std::string{source.mission_title});
    result.date_time = source.date_time;
    result.directive = encodeVit(source.directive);
    result.additional_directive = encodeVit(source.additional_directive);
    return result;
  }
  return load_pack_record();
}

std::optional<LocalizedMissionMenuTexts>
localizedMissionMenuTexts(std::uint32_t mission_index,
                          std::span<const std::string> objectives,
                          std::span<const std::string> parameters) noexcept {
  // Mission-menu overrides are encoded for the ViT Cyrillic atlas. Returning
  // them while the English locale is active makes otherwise English pause
  // screens render Russian text (or Cyrillic bytes through the retail font).
  // Match every other localized asset accessor and leave the original guest
  // strings untouched unless the Russian pack is explicitly selected.
  if (!russianLanguageActive()) {
    return std::nullopt;
  }
  const auto &pack = missionMenuPack();
  if (objectives.size() > 32U || parameters.size() > 32U) {
    return std::nullopt;
  }
  const auto translate = [&](std::span<const std::string> source,
                             std::size_t mission_pack_offset,
                             std::u8string_view safe_fallback) {
    std::vector<std::string> translated;
    translated.reserve(source.size());
    for (std::size_t index = 0U; index < source.size(); ++index) {
      const auto &text = source[index];
      if (const auto authored = translatedCopyFor(text)) {
        translated.push_back(*authored);
        continue;
      }
      if (const auto authored = translatedCuratedTemplateCopyFor(text)) {
        translated.push_back(*authored);
        continue;
      }
      if (pack && mission_index < pack->size()) {
        if (const auto mission_translation =
                translatedMissionEntry((*pack)[mission_index], text)) {
          translated.push_back(*mission_translation);
          continue;
        }
      }
      // Shared overlays occasionally report the neighbouring retail mission
      // index while retaining another mission's string table. Search the full
      // authored pack before ever exposing English bytes through the Russian
      // atlas.
      std::optional<std::string> shared_translation;
      if (pack) {
        for (const auto &entries : *pack) {
          if (const auto candidate = translatedMissionEntry(entries, text)) {
            shared_translation = *candidate;
            break;
          }
        }
      }
      if (shared_translation) {
        translated.push_back(std::move(*shared_translation));
      } else if (const auto normalized =
                     translatedNormalizedBuiltInCopyFor(text)) {
        translated.push_back(*normalized);
      } else if (russianLanguageActive()) {
        // The retail overlay briefly rewrites mission tables while streaming.
        // A syntactically valid but transient pointer used to expose arbitrary
        // English bytes through the ViT atlas. Recover static entries from the
        // authored mission/slot table; dynamic templates fall back to a safe
        // Russian label until their stable formatted source is published.
        const auto pack_index = mission_pack_offset + index;
        if (pack && mission_index < pack->size() &&
            pack_index < (*pack)[mission_index].size() &&
            (*pack)[mission_index][pack_index].second.find('%') ==
                std::string::npos) {
          translated.push_back((*pack)[mission_index][pack_index].second);
        } else {
          translated.push_back(encodeVit(safe_fallback));
        }
      } else {
        translated.push_back(text);
      }
    }
    return translated;
  };

  LocalizedMissionMenuTexts result;
  // Callers pass exact guest tables in bit order; translating by key preserves
  // mission-specific subsets within shared retail overlays.
  result.objectives = translate(objectives, 0U, u8"ЦЕЛЬ МИССИИ");
  result.parameters =
      translate(parameters, objectives.size(), u8"УСЛОВИЕ МИССИИ");
  return result;
}

} // namespace sf::game
