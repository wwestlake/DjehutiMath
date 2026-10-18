#pragma once

#include <string>

namespace DjehutiCredentials
{
bool write(const std::string& name, const std::string& secret, std::string& error);
bool read(const std::string& name, std::string& secret, std::string& error);
bool remove(const std::string& name, std::string& error);
}
