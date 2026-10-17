#ifndef EDITOR_PROJECT_TEMPLATES_H
#define EDITOR_PROJECT_TEMPLATES_H

#include <filesystem>
#include <string>
#include <vector>

// A starting point for a new project. As with Unity Hub templates, the
// template is copied into the new project's folder and never edited in place.
// An empty source means the built-in empty project.
struct ProjectTemplate {
    std::string name;
    std::filesystem::path source;
};

namespace ProjectTemplates {

// The built-in empty project, then every folder under .engine/templates that
// holds a game.config, by name.
std::vector<ProjectTemplate> List();

// Accept only a single folder name that every supported platform can create.
bool IsValidProjectName(const std::string &name, std::string &error);

// The first of base, base(1), base(2), ... not yet present under location.
std::string UniqueProjectName(const std::filesystem::path &location,
                              const std::string &base);

// Fill project_root, which must be missing or an empty folder.
bool Create(const ProjectTemplate &project_template,
            const std::filesystem::path &project_root,
            const std::string &project_name, std::string &error);

} // namespace ProjectTemplates

#endif
