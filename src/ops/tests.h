#pragma once
// Test cases per definition. The namespace keeps, for each definition's lineage,
// the set of test definitions designed for it (many tests per definition; a test
// may cover several). Every function and method outside test files is expected
// to have at least one; a missing list is a warning (or an error with
// [tests] require = "block").
//
// The suite hash ties a definition to its tests: it hashes the definition's
// content hash with the content hashes of all its linked tests, so it changes
// whenever the code or any of its tests change. Test results are recorded
// against it, so a result never outlives the code it was run on.
#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ops/repo.h"

namespace hs {

bool is_test_file(const std::string& file);
bool is_test_def(const std::string& file, const std::string& qname);
// Does this definition need linked tests under the project's policy?
bool needs_tests(const Config& c, const std::string& file, const std::string& qname,
                 const std::string& kind);

struct TestWarning {
  std::string qname;
  std::string message;
};

// Definitions (all, or only those named) that need tests and have none linked.
std::vector<TestWarning> missing_tests(Repo& repo, const Namespace& ns,
                                       const std::vector<std::string>* only = nullptr);

void link_tests(Repo& repo, const std::string& def, const std::vector<std::string>& tests);
void unlink_tests(Repo& repo, const std::string& def, const std::vector<std::string>& tests);
// Test definitions that call def directly, per the call graph.
std::vector<std::string> suggest_tests(Repo& repo, const std::string& def);
// Links suggested tests to every definition that has none. Returns links made.
int autolink_tests(Repo& repo);

Hash suite_hash(Repo& repo, const Namespace& ns, const std::string& qname);

// Per-function test summary: of the tests linked to it, how many passed and
// failed. Results from verifying exactly this namespace come first; other
// tests use the latest result for the same code and tests (suite hash), and
// count as not run when there is none.
struct TestSummary {
  std::string qname;
  int total = 0, passed = 0, failed = 0, not_run = 0;
  bool needs_tests = false;
  std::vector<std::pair<std::string, std::string>> results;  // (test, passed | failed | not run)
  std::string label() const;  // "3/4 passed, 1 failed", "no tests", ...
  nlohmann::json to_json() const;
};
// Stores the per-test results of a verification report (from this machine, or
// received with a push or pull) so summaries elsewhere show the same numbers.
void record_test_results(Store& s, const nlohmann::json& report);

// Summaries for the non-test definitions of ns (all, or only those named).
std::map<std::string, TestSummary> test_summaries(Repo& repo, const Namespace& ns,
                                                  const std::set<std::string>* only = nullptr);
nlohmann::json tests_info(Repo& repo, const std::string& qname);

}  // namespace hs
