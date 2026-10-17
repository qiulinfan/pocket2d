#include "editor/core/ProjectTemplates.h"
#include "shared/resources/ResourcePath.h"
#include <algorithm>
#include <fstream>
#include <system_error>

namespace {

bool PrepareProjectFolder(const std::filesystem::path &project_root,
                          std::string &error) {
    std::error_code fs_error;
    if (std::filesystem::exists(project_root, fs_error)) {
        if (fs_error) {
            error = "Could not inspect project folder.";
            return false;
        }
        if (!std::filesystem::is_directory(project_root, fs_error) ||
            fs_error) {
            error = "Project path already exists and is not a folder.";
            return false;
        }
        if (!std::filesystem::is_empty(project_root, fs_error) ||
            fs_error) {
            error = "Project folder already exists and is not empty.";
            return false;
        }
    } else if (!std::filesystem::create_directories(project_root, fs_error) ||
               fs_error) {
        error = "Could not create project folder.";
        return false;
    }
    return true;
}

bool WriteTextFileIfMissing(const std::filesystem::path &path,
                            const std::string &contents) {
    if (std::filesystem::exists(path)) return true;
    std::ofstream output_file(path, std::ios::out | std::ios::trunc);
    if (!output_file.is_open()) return false;
    output_file << contents;
    return output_file.good();
}

bool WriteEmptyProject(const std::filesystem::path &project_root,
                       const std::string &project_name, std::string &error) {
    std::error_code fs_error;
    const char *subdirectories[] = {
        "actor_templates", "audio", "component_types", "fonts",
        "images", "scenes"
    };
    for (const char *subdirectory : subdirectories) {
        if (!std::filesystem::create_directories(project_root / subdirectory,
                                                 fs_error) ||
            fs_error) {
            error = "Could not create project subfolders.";
            return false;
        }
    }

    const std::string game_config =
        "{\n"
        "  \"game_title\": \"" + project_name + "\",\n"
        "  \"initial_scene\": \"main\"\n"
        "}\n";
    if (!WriteTextFileIfMissing(project_root / "game.config", game_config)) {
        error = "Could not write game.config.";
        return false;
    }

    const std::string rendering_config =
        "{\n"
        "  \"clear_color_r\": 255,\n"
        "  \"clear_color_g\": 255,\n"
        "  \"clear_color_b\": 255,\n"
        "  \"x_resolution\": 640,\n"
        "  \"y_resolution\": 360\n"
        "}\n";
    if (!WriteTextFileIfMissing(project_root / "rendering.config",
                                rendering_config)) {
        error = "Could not write rendering.config.";
        return false;
    }

    const std::string main_scene =
        "{\n"
        "  \"actors\": []\n"
        "}\n";
    if (!WriteTextFileIfMissing(project_root / "scenes" / "main.scene",
                                main_scene)) {
        error = "Could not write scenes/main.scene.";
        return false;
    }

    const std::string empty_template =
        "{\n"
        "  \"name\": \"empty\",\n"
        "  \"components\": {}\n"
        "}\n";
    if (!WriteTextFileIfMissing(project_root / "actor_templates" /
                                    "empty.template",
                                empty_template)) {
        error = "Could not write actor_templates/empty.template.";
        return false;
    }

    return true;
}

// Copy the template's files, leaving out its editor-private .pocketengine state.
bool CopyTemplate(const std::filesystem::path &source,
                  const std::filesystem::path &project_root,
                  std::string &error) {
    std::error_code fs_error;
    std::filesystem::recursive_directory_iterator entry(source, fs_error), end;
    for (; !fs_error && entry != end; entry.increment(fs_error)) {
        const std::filesystem::path relative =
            entry->path().lexically_relative(source);
        if (entry->is_directory(fs_error)) {
            if (relative.filename() == ".pocketengine") {
                entry.disable_recursion_pending();
                continue;
            }
            std::filesystem::create_directories(project_root / relative, fs_error);
        } else if (entry->is_regular_file(fs_error)) {
            std::filesystem::copy_file(entry->path(), project_root / relative, fs_error);
        }
        if (fs_error) break;
    }
    if (fs_error) {
        error = "Could not copy template: " + fs_error.message();
        return false;
    }
    return true;
}

} // namespace

std::vector<ProjectTemplate> ProjectTemplates::List() {
    std::vector<ProjectTemplate> templates{{"Empty", {}}};
    std::error_code fs_error;
    for (const auto &entry : std::filesystem::directory_iterator(
             ResourcePath::ProjectTemplatesRoot(), fs_error)) {
        if (std::filesystem::exists(entry.path() / "game.config", fs_error))
            templates.push_back({entry.path().filename().string(), entry.path()});
    }
    std::sort(templates.begin() + 1, templates.end(),
              [](const ProjectTemplate &lhs, const ProjectTemplate &rhs) {
                  return lhs.name < rhs.name;
              });
    return templates;
}

bool ProjectTemplates::IsValidProjectName(const std::string &name, std::string &error) {
    if (name.empty()) {
        error = "Project name is empty.";
        return false;
    }
    if (name == "." || name == "..") {
        error = "Project name cannot be '.' or '..'.";
        return false;
    }
    if (name.back() == '.') {
        error = "Project name cannot end with a dot.";
        return false;
    }
    for (const char ch : name) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (uch < 32) {
            error = "Project name cannot contain control characters.";
            return false;
        }
        if (ch == '/' || ch == '\\') {
            error = "Project name cannot contain path separators.";
            return false;
        }
        if (ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '|' || ch == '?' || ch == '*') {
            error = "Project name contains a character Windows cannot use.";
            return false;
        }
    }
    return true;
}

std::string ProjectTemplates::UniqueProjectName(
    const std::filesystem::path &location, const std::string &base) {
    if (!std::filesystem::exists(location / base)) return base;
    for (int index = 1; index < 10000; ++index) {
        const std::string candidate = base + "(" + std::to_string(index) + ")";
        if (!std::filesystem::exists(location / candidate)) return candidate;
    }
    return base;
}

bool ProjectTemplates::Create(const ProjectTemplate &project_template,
                              const std::filesystem::path &project_root,
                              const std::string &project_name,
                              std::string &error) {
    if (!PrepareProjectFolder(project_root, error)) return false;
    if (project_template.source.empty())
        return WriteEmptyProject(project_root, project_name, error);
    return CopyTemplate(project_template.source, project_root, error);
}
