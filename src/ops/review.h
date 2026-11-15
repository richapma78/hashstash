#pragma once
// Reviews of commits and the review flag each commit carries.
//
// Anyone can review a commit: an AI agent (through MCP, or the CLI when it runs
// under an agent) or a person (the review pages of hs ui / hshub, or the CLI in
// a normal terminal). Reviews never block anything; they record who looked.
// The flag per commit says whether it is unreviewed, only AI reviewed, or
// reviewed by a person too, and whether a person approved it.
//
// Reviews are append-only and travel with push/pull, so the flag is the same
// on the client and on the hub.
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "ops/repo.h"

namespace hs {

struct Review {
  std::string id, commit, reviewer;
  std::string kind;     // human | ai
  std::string verdict;  // approved | changes_requested | comment
  std::string summary;
  nlohmann::json comments = nlohmann::json::array();  // [{name, line, text}]
  long long time = 0;
};

struct ReviewStatus {
  std::string level = "unreviewed";  // unreviewed | ai | human
  bool ai_reviewed = false, human_reviewed = false, human_approved = false;
  std::string ai_verdict, human_verdict;  // latest of each, or ""
  std::string label() const;  // "unreviewed", "AI reviewed", "human approved", ...
  nlohmann::json to_json() const;
};

// Resolves a commit reference: "head" (or ""), "head~N", or a hash prefix.
Hash resolve_commit(Repo& repo, const std::string& ref);

// Records a review of a commit and updates its flag. kind is "human" or "ai".
Review add_review(Repo& repo, const std::string& commit_ref, const std::string& verdict, const std::string& summary,
                  const std::string& kind, const nlohmann::json& comments = nlohmann::json::array(),
                  const std::string& reviewer = "");
// Stores a review received from elsewhere (sync); ignored if already present.
bool store_review(Store& s, const Review& r, bool synced);
std::vector<Review> reviews_of(Store& s, const Hash& commit);
ReviewStatus review_status(Store& s, const Hash& commit);

nlohmann::json review_to_json(const Review& r);
Review review_from_json(const nlohmann::json& j);

// "ai" when the CLI runs under an AI agent (Claude Code, or HS_REVIEWER=ai),
// otherwise "human". HS_REVIEWER=human|ai overrides the detection.
std::string reviewer_kind_from_env();

// Reviews made here that the hub hasn't received yet, and marking them sent.
std::vector<Review> unsynced_reviews(Store& s);
void mark_reviews_synced(Store& s, const std::vector<Review>& rs);
// Hub side: reviews stored after a sequence number (rowid), for incremental pulls.
nlohmann::json reviews_after(Store& s, long long seq);

void log_push(Store& s, const std::string& branch, const Hash& old_head, const Hash& new_head,
              const std::string& agent);
nlohmann::json push_log(Store& s, int limit);

// First-parent commits of a branch, newest first.
std::vector<Hash> branch_commits(Repo& repo, const std::string& branch, int limit);
// One commit as a summary row: hash, message, author, time, change count,
// verification and review flag.
nlohmann::json commit_row(Repo& repo, const Hash& commit);
// Everything needed to review a commit: its summary, the line diff against
// its first parent (capped at max_lines; 0 = all), per-function test
// summaries of what it changed, and its reviews.
nlohmann::json show_commit(Repo& repo, const Hash& commit, int max_lines);

// Is the approval queue shown here? where = "local" or "hub".
bool approval_queue_enabled(const Repo& repo, const std::string& where);

}  // namespace hs
