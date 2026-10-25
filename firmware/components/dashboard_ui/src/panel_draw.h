// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "dashboard_core/model.h"
#include "dashboard_ui/layout.h"
#include "font_manager.h"
#include "portrait_canvas.h"

namespace eego::dashboard::ui {

bool alertEventMatches(const Dashboard& dashboard, const PanelSpec& panel, const Event& event);

// Right-aligned item in a panel's title row: the latest value of a chart, an
// active-alert count, or a threshold badge. Empty text = nothing to show.
struct HeaderAccessory {
  std::string text;
  Severity severity = Severity::Info;
  bool badge = false;  // pill (outlined / inverted by severity) instead of plain bold text
};
HeaderAccessory panelHeaderAccessory(const Dashboard& dashboard, const PanelSpec& panel);

void drawCardPanel(PortraitCanvas& canvas, FontManager& font, const Dashboard& dashboard,
                   const PanelSpec& panel, const Rect& bounds,
                   const VariableValues& variables);
void drawChartPanel(PortraitCanvas& canvas, FontManager& font, const Dashboard& dashboard,
                    const PanelSpec& panel, const Rect& bounds);
void drawTablePanel(PortraitCanvas& canvas, FontManager& font, const Dashboard& dashboard,
                    const PanelSpec& panel, const Rect& bounds);
void drawAlertPanel(PortraitCanvas& canvas, FontManager& font, const Dashboard& dashboard,
                    const PanelSpec& panel, const Rect& bounds,
                    const VariableValues& variables);
void drawButtonPanel(PortraitCanvas& canvas, FontManager& font, const PanelLayout& panel,
                     const VariableValues& variables);

}  // namespace eego::dashboard::ui
