#include "filter_menu.h"

// JSON (de)serialization of the debug text filters (settings). Kept apart from the imgui menu so
// it can be built without imgui (3DS).

void to_json(json& j, const DebugTextFilter& obj) {
  j = json{{"content", obj.content}, {"type", obj.type}};
}

void from_json(const json& j, DebugTextFilter& obj) {
  j.at("content").get_to(obj.content);
  j.at("type").get_to(obj.type);
}
