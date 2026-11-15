#pragma once
// Review pages in the browser, served by `hs ui` (your local database) and by
// hshub (the hub's database) from the same code:
//
//   /ui                          projects
//   /ui/p/<project>              commits with verification and review flags,
//                                what waits for a person, pushes (hub)
//   /ui/p/<project>/commit/<h>   one commit: per-function diffs with test
//                                summaries, reviews, and the review form
//   /ui/p/<project>/work         uncommitted changes
//   /ui/p/<project>/tests        test results per function
//   /ui/p/<project>/def?name=    one definition: code, tests, notes, history
//
// Reviews recorded here are human reviews. They never block anything.
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace httplib {
class Server;
}

namespace hs {

class Repo;

struct UiSite {
  std::string where = "local";  // "local" or "hub": which approval queue setting applies
  std::function<std::vector<std::string>()> projects;
  std::function<Repo*(const std::string&)> open;  // nullptr if there is no such project
  std::mutex* mu = nullptr;                      // held while a page reads the store
  std::string review_token;                      // hub: needed to record a human review ("" = none)
  // Called after a page changed the project (a review), so a hub can write
  // its workspace back to the backend. Optional.
  std::function<void(const std::string&)> changed;
};

void mount_ui(httplib::Server& srv, const UiSite& site);

}  // namespace hs
