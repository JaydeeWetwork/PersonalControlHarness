// Builds widget trees from JSON (pack files under ui/, or scripts' layouts):
//   {"type": "panel", "id": "...", "style": "...", "tooltip": "...", "visible": true,
//    "layout": {"anchor_min": [x, y], "anchor_max": [x, y], "insets": [l, t, r, b] | n,
//               "offset": [x, y], "pivot": [x, y], "size": [w, h], "min_size": [w, h],
//               "margin": [l, t, r, b] | n, "flex": n},
//    "on": {"click": "void onPlay(const string &in)"},  // script handlers by event
//    "children": [ ... ]}
// plus per-type properties; see docs/UI.md for the table. Unknown types or keys are errors.
#pragma once
#include <memory>
#include <string>
#include "litholattice/core/json.h"
#include "litholattice/ui/widget.h"

namespace litholattice::ui {

/// nullptr + `err` (with the JSON path) on error.
std::unique_ptr<Widget> buildWidget(const Json& json, std::string& err);
std::unique_ptr<Widget> buildWidget(const std::string& text, std::string& err);

}  // namespace litholattice::ui
