// SPDX-License-Identifier: GPL-3.0-or-later
// Text and JSON output for `quillreel inspect`. Spec: specs/inspector.md
#pragma once

#include <expected>
#include <ostream>
#include <string>
#include <vector>

#include "inspect.h"

namespace quillreel {

struct InspectResult {
    std::string path;
    std::expected<FileInfo, Error> result;
};

void print_text(std::ostream& out, const FileInfo& info);
void print_json(std::ostream& out, const std::vector<InspectResult>& results);

}  // namespace quillreel
