// Vita3K libretro core: stub host::dialog::filesystem implementation.
//
// The libretro frontend never calls into the host file dialogs (frontend
// already handles all file-picking via its own UI), so we provide
// no-op implementations.  Used on iOS where neither nativefiledialog-extended
// nor SDL3's native dialog support is available.

#include <host/dialog/filesystem.h>

namespace host::dialog::filesystem {

Result open_file(fs::path & /*resulting_path*/,
                 const std::vector<FileFilter> & /*file_filters*/,
                 const fs::path & /*default_path*/) {
    return Result::CANCEL;
}

Result pick_folder(fs::path & /*resulting_path*/,
                   const fs::path & /*default_path*/) {
    return Result::CANCEL;
}

std::string get_error() {
    return "Native file dialog is unavailable in the libretro core build.";
}

} // namespace host::dialog::filesystem
