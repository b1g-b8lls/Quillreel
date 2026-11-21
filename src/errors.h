// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace quillreel {

// Values double as the process exit codes documented in `quillreel --help`.
enum class ErrorKind {
    Usage = 1,        // bad arguments or option combinations
    Open = 2,         // file missing, unreadable, or not recognised by FFmpeg
    NotBink = 3,      // readable media file, but not a Bink container
    Unsupported = 4,  // no decoder/encoder for a stream this build would need
    Failed = 5,       // output exists, or writing/encoding failed
};

struct Error {
    ErrorKind kind;
    std::string message;
};

}  // namespace quillreel
