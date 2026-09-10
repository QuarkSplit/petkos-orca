#pragma once

#include <atomic>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace Slic3r::GUI::ProjectLibrary {

// Canonical paths make overlapping folders and differently spelled Windows paths one file.
std::string path_key(const std::string &path);
nlohmann::json scan(const std::vector<std::string> &roots, const std::string &thumbnail_dir,
                    const nlohmann::json &previous, const std::atomic<bool> &cancel);

// Called by the library's serial preview worker, never while scanning metadata.
nlohmann::json thumbnail(const nlohmann::json &item, const std::string &thumbnail_dir,
                         const std::string &executable, const std::atomic<bool> &cancel,
                         bool force = false);

} // namespace Slic3r::GUI::ProjectLibrary
