#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Vivify::Updater {

// Where an update comes from (0.15.12).
enum class Channel {
  Stable,   // the latest numbered GitHub release
  Nightly,  // the rolling `nightly` pre-release (nightly.yml)
  Dev,      // the latest build of main (build.yml's artifact, through nightly.link)
  Version,  // one numbered release, by tag
};

// Downloads the chosen build and puts its library in place of this one, for
// the next start of the game. On a worker thread; `status` is called on the
// main thread with progress and the outcome. Refused when the build needs a
// version of another mod that is not installed. `tag` is used with
// Channel::Version only.
void Install(Channel channel, std::string const& tag, std::function<void(std::string const&)> status);

// The numbered releases that can be installed (0.13.5 and later), newest
// first, on the main thread; empty with `error` set on failure.
void ListVersions(std::function<void(std::vector<std::string> const& tags, std::string const& error)> done);

bool Busy();

}  // namespace Vivify::Updater
