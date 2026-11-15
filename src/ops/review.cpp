#include "ops/review.h"

#include <sqlite3.h>

#include <cstdlib>
#include <set>
#include <stdexcept>

#include "ops/diff.h"
#include "ops/tests.h"
#include "ops/verify.h"

namespace hs {

using json = nlohmann::json;

static const std::set<std::string> kVerdicts = {"approved", "changes_requested", "comment"};

std::string ReviewStatus::label() const {
  if (human_reviewed) {
    if (human_verdict == "approved" || (human_approved && human_verdict == "comment")) return "human approved";
    if (human_verdict == "changes_requested") return "human: changes requested";
    return "human reviewed";
  }
  if (ai_reviewed) return ai_verdict == "changes_requested" ? "AI: changes requested" : "AI reviewed";
  return "unreviewed";
}

json ReviewStatus::to_json() const {
  return {{"level", level},           {"label", label()},          {"ai_reviewed", ai_reviewed},
          {"human_reviewed", human_reviewed}, {"human_approved", human_approved}, {"ai_verdict", ai_verdict},
          {"human_verdict", human_verdict}};
}

Hash resolve_commit(Repo& repo, const std::string& ref_in) {
  Store& s = repo.store();
  std::string ref = ref_in.empty() ? "head" : ref_in;
  if (ref == "head" || starts_with(ref, "head~")) {
    auto h = repo.head_commit();
    if (!h) throw std::runtime_error("no commits yet");
    int n = ref == "head" ? 0 : std::stoi(ref.substr(5));
    Hash c = *h;
    for (int i = 0; i < n; ++i) {
      Commit cc = load_commit(s, c);
      if (cc.parents.empty()) throw std::runtime_error(ref + " goes past the first commit");
      c = cc.parents[0];
    }
    return c;
  }
  std::string p = ref[0] == '#' ? ref.substr(1) : ref;
  auto h = s.expand(p);
  if (!h) throw std::runtime_error("no commit " + ref);
  auto o = s.get(*h);
  if (!o || o->kind != "commit") throw std::runtime_error("#" + short_hash(*h) + " is not a commit");
  return *h;
}

static void refresh_flag(Store& s, const Hash& commit) {
  ReviewStatus st;
  for (auto& r : reviews_of(s, commit)) {  // oldest first: the last one wins
    if (r.kind == "human") {
      st.human_reviewed = true;
      st.human_verdict = r.verdict;
      if (r.verdict == "approved") st.human_approved = true;
      if (r.verdict == "changes_requested") st.human_approved = false;
    } else {
      st.ai_reviewed = true;
      st.ai_verdict = r.verdict;
    }
  }
  st.level = st.human_reviewed ? "human" : st.ai_reviewed ? "ai" : "unreviewed";
  Stmt up(s.db(),
          "INSERT OR REPLACE INTO commit_review(commit_hash, level, ai_reviewed, human_reviewed, human_approved, "
          "ai_verdict, human_verdict, updated) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
  up.bind(1, commit).bind(2, st.level).bind(3, (long long)st.ai_reviewed).bind(4, (long long)st.human_reviewed)
      .bind(5, (long long)st.human_approved).bind(6, st.ai_verdict).bind(7, st.human_verdict)
      .bind(8, now_seconds()).run();
}

bool store_review(Store& s, const Review& r, bool synced) {
  if (r.id.empty() || r.commit.empty()) throw std::runtime_error("review without id or commit");
  if (!kVerdicts.count(r.verdict)) throw std::runtime_error("unknown verdict " + r.verdict);
  if (r.kind != "human" && r.kind != "ai") throw std::runtime_error("reviewer kind must be human or ai");
  bool added = false;
  s.transaction([&] {
    Stmt st(s.db(),
            "INSERT OR IGNORE INTO review(id, commit_hash, reviewer, kind, verdict, summary, comments, time, synced) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    st.bind(1, r.id).bind(2, r.commit).bind(3, r.reviewer).bind(4, r.kind).bind(5, r.verdict).bind(6, r.summary)
        .bind(7, r.comments.dump()).bind(8, r.time).bind(9, (long long)synced).run();
    added = sqlite3_changes(s.db()) > 0;
    if (added) refresh_flag(s, r.commit);
  });
  return added;
}

Review add_review(Repo& repo, const std::string& commit_ref, const std::string& verdict_in, const std::string& summary,
                  const std::string& kind, const json& comments, const std::string& reviewer) {
  std::string verdict = verdict_in;
  if (verdict == "approve") verdict = "approved";
  if (verdict == "changes" || verdict == "request_changes" || verdict == "reject") verdict = "changes_requested";
  if (!kVerdicts.count(verdict))
    throw std::runtime_error("verdict must be approved, changes_requested or comment (got '" + verdict_in + "')");
  if (verdict != "approved" && trim(summary).empty())
    throw std::runtime_error("say what you found: a summary is needed unless you approve");
  Review r;
  r.id = new_uuid();
  r.commit = resolve_commit(repo, commit_ref);
  r.reviewer = reviewer.empty() ? repo.agent() : reviewer;
  r.kind = kind;
  r.verdict = verdict;
  r.summary = trim(summary);
  r.comments = comments.is_array() ? comments : json::array();
  r.time = now_seconds();
  store_review(repo.store(), r, false);
  return r;
}

static Review read_review(Stmt& st) {
  Review r;
  r.id = st.text(0);
  r.commit = st.text(1);
  r.reviewer = st.text(2);
  r.kind = st.text(3);
  r.verdict = st.text(4);
  r.summary = st.text(5);
  try {
    r.comments = json::parse(st.text(6));
  } catch (...) {
  }
  r.time = st.integer(7);
  return r;
}

static const char* kReviewCols = "id, commit_hash, reviewer, kind, verdict, summary, comments, time";

std::vector<Review> reviews_of(Store& s, const Hash& commit) {
  Stmt st(s.db(), (std::string("SELECT ") + kReviewCols + " FROM review WHERE commit_hash = ? ORDER BY time, rowid").c_str());
  st.bind(1, commit);
  std::vector<Review> out;
  while (st.step()) out.push_back(read_review(st));
  return out;
}

ReviewStatus review_status(Store& s, const Hash& commit) {
  ReviewStatus st;
  Stmt q(s.db(),
         "SELECT level, ai_reviewed, human_reviewed, human_approved, ai_verdict, human_verdict FROM commit_review "
         "WHERE commit_hash = ?");
  q.bind(1, commit);
  if (q.step()) {
    st.level = q.text(0);
    st.ai_reviewed = q.integer(1);
    st.human_reviewed = q.integer(2);
    st.human_approved = q.integer(3);
    st.ai_verdict = q.text(4);
    st.human_verdict = q.text(5);
  }
  return st;
}

json review_to_json(const Review& r) {
  return {{"id", r.id},           {"commit", r.commit},   {"reviewer", r.reviewer}, {"kind", r.kind},
          {"verdict", r.verdict}, {"summary", r.summary}, {"comments", r.comments}, {"time", r.time}};
}

Review review_from_json(const json& j) {
  Review r;
  r.id = j.at("id");
  r.commit = j.at("commit");
  r.reviewer = j.value("reviewer", "");
  r.kind = j.at("kind");
  r.verdict = j.at("verdict");
  r.summary = j.value("summary", "");
  r.comments = j.value("comments", json::array());
  r.time = j.value("time", 0LL);
  return r;
}

std::string reviewer_kind_from_env() {
  if (const char* v = std::getenv("HS_REVIEWER"); v && *v) return std::string(v) == "ai" ? "ai" : "human";
  for (const char* var : {"CLAUDECODE", "CLAUDE_CODE_ENTRYPOINT", "CODEX_SANDBOX", "AIDER_MODEL"})
    if (const char* v = std::getenv(var); v && *v) return "ai";
  return "human";
}

std::vector<Review> unsynced_reviews(Store& s) {
  Stmt st(s.db(), (std::string("SELECT ") + kReviewCols + " FROM review WHERE synced = 0 ORDER BY time, rowid").c_str());
  std::vector<Review> out;
  while (st.step()) out.push_back(read_review(st));
  return out;
}

void mark_reviews_synced(Store& s, const std::vector<Review>& rs) {
  s.transaction([&] {
    Stmt st(s.db(), "UPDATE review SET synced = 1 WHERE id = ?");
    for (auto& r : rs) {
      st.bind(1, r.id).run();
      st.reset();
    }
  });
}

json reviews_after(Store& s, long long seq) {
  Stmt st(s.db(), (std::string("SELECT ") + kReviewCols + ", rowid FROM review WHERE rowid > ? ORDER BY rowid LIMIT 5000").c_str());
  st.bind(1, seq);
  json out = json::array();
  long long last = seq;
  while (st.step()) {
    out.push_back(review_to_json(read_review(st)));
    last = st.integer(8);
  }
  return {{"reviews", out}, {"last", last}};
}

void log_push(Store& s, const std::string& branch, const Hash& old_head, const Hash& new_head, const std::string& agent) {
  Stmt st(s.db(), "INSERT INTO push_log(branch, old_head, new_head, agent, time) VALUES (?, ?, ?, ?, ?)");
  st.bind(1, branch).bind(2, old_head).bind(3, new_head).bind(4, agent).bind(5, now_seconds()).run();
}

json push_log(Store& s, int limit) {
  Stmt st(s.db(), "SELECT id, branch, old_head, new_head, agent, time FROM push_log ORDER BY id DESC LIMIT ?");
  st.bind(1, (long long)limit);
  json out = json::array();
  while (st.step())
    out.push_back({{"id", st.integer(0)}, {"branch", st.text(1)}, {"old", st.text(2)}, {"new", st.text(3)},
                   {"agent", st.text(4)}, {"time", st.integer(5)}});
  return out;
}

std::vector<Hash> branch_commits(Repo& repo, const std::string& branch, int limit) {
  std::vector<Hash> out;
  std::string h = repo.store().ref("branch/" + branch).value_or("");
  while (!h.empty() && (limit <= 0 || (int)out.size() < limit)) {
    out.push_back(h);
    Commit c = load_commit(repo.store(), h);
    h = c.parents.empty() ? "" : c.parents[0];
  }
  return out;
}

json commit_row(Repo& repo, const Hash& h) {
  Store& s = repo.store();
  Commit c = load_commit(s, h);
  std::string v = verification_status(repo, c.ns);
  return {{"commit", h},
          {"short", short_hash(h)},
          {"author", c.author},
          {"time", c.time},
          {"when", format_time(c.time)},
          {"message", c.message},
          {"parents", c.parents},
          {"changes", c.changes.size()},
          {"verification", v.empty() ? "not verified" : v},
          {"review", review_status(s, h).to_json()}};
}

json show_commit(Repo& repo, const Hash& h, int max_lines) {
  Store& s = repo.store();
  Commit c = load_commit(s, h);
  json out = commit_row(repo, h);
  out["change_list"] = c.changes;
  Namespace now = load_namespace(s, c.ns);
  Namespace before = c.parents.empty() ? load_namespace(s, empty_namespace(s))
                                       : load_namespace(s, load_commit(s, c.parents[0]).ns);
  auto d = diff_namespaces(s, before, now);
  out["diff"] = diff_to_json(s, d, true, max_lines);
  // Test summary per changed function, against the code of this commit.
  std::set<std::string> names;
  for (auto& dc : d.defs)
    if (!dc.new_name.empty()) names.insert(dc.new_name);
  json tests = json::object();
  for (auto& [q, t] : test_summaries(repo, now, &names)) tests[q] = t.to_json();
  out["tests"] = tests;
  json rs = json::array();
  for (auto& r : reviews_of(s, h)) rs.push_back(review_to_json(r));
  out["reviews"] = rs;
  return out;
}

bool approval_queue_enabled(const Repo& repo, const std::string& where) {
  const std::string& a = repo.config().review_approval;
  return a == "both" || a == where;
}

}  // namespace hs
