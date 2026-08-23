#pragma once
#include <string>
#include <vector>

extern std::string g_forwarderSelfPath;

std::string launcherNroPath();

bool forwarder_create(const std::string &gameKey, const std::string &name,
                      const std::string &author, const std::string &iconImgPath,
                      const std::vector<std::string> &legacyGameKeys,
                      char *err, std::size_t errSize);
bool forwarder_create_launcher(char *err, std::size_t errSize);
