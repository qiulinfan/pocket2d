#ifndef POCKET2D_GAME_SAVE_H
#define POCKET2D_GAME_SAVE_H
#include <string>
namespace GameSave {
// Configure before running project scripts. Empty disables persistence; each
// Engine construction clears the previous project's context before config read.
void Configure(const std::string &save_namespace);
}
#endif
