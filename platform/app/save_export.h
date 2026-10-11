#ifndef MDKR64_SAVE_EXPORT_H
#define MDKR64_SAVE_EXPORT_H
#include <string>
namespace SaveExport {
std::string suggestedFilename();
// Reads the persisted EEPROM only. Never edits it or replaces an export.
bool progress(const std::string &saveDirectory, const std::string &destination,
              const char *version, std::string &error);
}
#endif
