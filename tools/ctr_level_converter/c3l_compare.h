#pragma once

/*!
 * @file c3l_compare.h
 * ctr_level_converter --compare: two .c3l files (or every .c3l of two folders) by content.
 */

#include <string>

namespace c3l_compare {
// true when every table and piece the runtime reads is the same (offsets and section order may
// differ); the differences in the log
bool compare(const std::string& a, const std::string& b);
}  // namespace c3l_compare
