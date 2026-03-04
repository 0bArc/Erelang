#pragma once
#include <string>
#include <string_view>

namespace erelang::features {

std::string json_escape(std::string_view value);
std::string list_handle_to_json(std::string_view listHandle);
std::string dict_handle_to_json(std::string_view dictHandle);
std::string from_json_object_to_dict_handle(std::string_view json);

} // namespace erelang::features
