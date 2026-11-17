#pragma once
// Vivify diagnostic uploads. Automatic uploads send new log data after each
// level ends (and once on startup for retry); the main-menu action sends the
// complete available log files, up to twice the automatic byte budget.

#include <string>

namespace Vivify::LogUpload {

// Configure the uploader and make the startup retry.
void StartIfEnabled(std::string const& directory, std::string const& relayUrl, std::string const& version,
                    bool lite);

// Send all available Vivify logs, up to twice the automatic upload budget.
void UploadManual();

// Send newly written log data after a level ends or is quit.
void UploadAfterLevelEnd();

}  // namespace Vivify::LogUpload
