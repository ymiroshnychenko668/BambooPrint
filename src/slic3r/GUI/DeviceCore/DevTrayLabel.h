#pragma once

#include "DevDefs.h"
#include <algorithm>
#include <optional>
#include <string>

namespace Slic3r::GUI {

inline std::string filament_tray_label(int tray_id, std::optional<int> extruder_count = std::nullopt)
{
    if (tray_id == VIRTUAL_TRAY_MAIN_ID || tray_id == VIRTUAL_TRAY_DEPUTY_ID) {
        if (extruder_count == 2)
            return tray_id == VIRTUAL_TRAY_MAIN_ID ? "Ext-R" : "Ext-L";
        return "Ext";
    }
    if (tray_id >= 128 * 4) {
        const int index = (tray_id - 128 * 4) / 4;
        return index < 26 ? std::string(1, static_cast<char>('A' + index)) : std::string();
    }
    if (tray_id >= 0x80 && tray_id <= 0x87)
        return std::string("HT-") + static_cast<char>('A' + tray_id - 0x80);
    if (tray_id >= 24 && tray_id <= 27)
        return "Q" + std::to_string(tray_id - 24 + 1);
    return std::string(1, static_cast<char>('A' + std::clamp(tray_id / 4, 0, 25))) + std::to_string(tray_id % 4 + 1);
}

} // namespace Slic3r::GUI
