#include "web/ui.h"

#include <httplib.h>

#include <algorithm>
#include <set>
#include <sstream>

#include "ops/context.h"
#include "ops/diff.h"
#include "ops/history.h"
#include "ops/locks.h"
#include "ops/query.h"
#include "ops/repo.h"
#include "ops/review.h"
#include "ops/sync.h"
#include "ops/tests.h"
#include "ops/verify.h"

namespace hs {

using json = nlohmann::json;

namespace {

// ---- small helpers ----------------------------------------------------------

std::string esc(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default: o += c;
    }
  }
  return o;
}

std::string url_enc(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':') o += (char)c;
    else {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    }
  }
  return o;
}

std::string first_line(const std::string& s) { return s.substr(0, s.find('\n')); }

std::vector<std::string> text_lines(const std::string& s) {
  std::vector<std::string> out;
  for (auto& l : split_lines(s)) {
    std::string t = l;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    out.push_back(t);
  }
  return out;
}

std::string badge(const std::string& cls, const std::string& text) {
  return "<span class=\"badge " + cls + "\">" + esc(text) + "</span>";
}

std::string review_badge(const json& rv) {
  std::string level = rv.value("level", "unreviewed");
  std::string label = rv.value("label", "unreviewed");
  std::string cls = level == "human" ? (rv.value("human_verdict", "") == "changes_requested" ? "bad" : "good")
                    : level == "ai"  ? "ai"
                                     : "muted";
  return badge(cls, label);
}

std::string verify_badge(const std::string& v) {
  return badge(v == "passed" ? "good" : v == "failed" ? "bad" : "muted", v == "passed" ? "tests passed" : v);
}

std::string tests_badge(const json& t) {
  if (t.is_null()) return "";
  int total = t.value("total", 0), passed = t.value("passed", 0), failed = t.value("failed", 0);
  std::string cls = failed ? "bad" : total == 0 ? "warn" : passed == total ? "good" : "muted";
  return badge(cls, t.value("summary", ""));
}

// Pass/fail/not-run bar for a test summary.
std::string tests_bar(const json& t) {
  int total = t.value("total", 0);
  if (total == 0) return "<div class=\"bar\"></div>";
  auto pct = [&](int n) { return std::to_string(100.0 * n / total) + "%"; };
  return "<div class=\"bar\"><i class=\"p\" style=\"width:" + pct(t.value("passed", 0)) + "\"></i><i class=\"f\" style=\"width:" +
         pct(t.value("failed", 0)) + "\"></i></div>";
}

const char* kCss = R"CSS(
:root{--bg:#fbfaf7;--fg:#1f2328;--mut:#5d636b;--line:#dcd8cf;--card:#fff;--soft:#f2f0ea;--acc:#2f6f5e;
--good:#2f6f5e;--goodbg:#e2efe9;--bad:#a3302a;--badbg:#f8e1df;--warn:#9a5b00;--warnbg:#fbefd9;--ai:#3b5ba5;--aibg:#e4eaf7;
--add:#e6f4ea;--addn:#cdebd6;--del:#fbe9e7;--deln:#f5d0cb;--mono:ui-monospace,"Cascadia Code",Consolas,monospace}
@media (prefers-color-scheme:dark){:root{--bg:#15171a;--fg:#e6e3dc;--mut:#a2a7ad;--line:#353a40;--card:#1d2024;--soft:#24282d;
--acc:#6fc2a6;--good:#6fc2a6;--goodbg:#1f3530;--bad:#ef8a83;--badbg:#3d2322;--warn:#e0a84a;--warnbg:#3a2f1c;--ai:#8fa9eb;--aibg:#232c40;
--add:#17301f;--addn:#1d3d27;--del:#3a1d1b;--deln:#4a2421}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 "Segoe UI",system-ui,sans-serif}
a{color:var(--acc)}header{background:var(--card);border-bottom:1px solid var(--line);position:sticky;top:0;z-index:2}
header .in{max-width:1200px;margin:0 auto;padding:10px 16px;display:flex;gap:18px;align-items:center;flex-wrap:wrap}
header b a{color:var(--fg);text-decoration:none}header nav a{margin-right:14px;color:var(--mut);text-decoration:none}
main{max-width:1200px;margin:0 auto;padding:16px 16px 60px}h1{font-size:22px;margin:8px 0}h2{font-size:18px;margin:28px 0 8px}
.meta{color:var(--mut)}.badge{display:inline-block;font-size:12px;font-weight:600;padding:1px 8px;border-radius:99px;white-space:nowrap}
.good{background:var(--goodbg);color:var(--good)}.bad{background:var(--badbg);color:var(--bad)}.warn{background:var(--warnbg);color:var(--warn)}
.ai{background:var(--aibg);color:var(--ai)}.muted{background:var(--soft);color:var(--mut)}
table{border-collapse:collapse;width:100%}th,td{text-align:left;padding:6px 8px;border-bottom:1px solid var(--line);vertical-align:top}
th{background:var(--soft);font-weight:600}td.h{font-family:var(--mono);white-space:nowrap}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:12px;margin:12px 0}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px}.card .n{font-size:24px;font-weight:700}
.change{background:var(--card);border:1px solid var(--line);border-radius:10px;margin:12px 0;overflow:hidden}
.change summary{padding:8px 12px;background:var(--soft);cursor:pointer;display:flex;gap:10px;align-items:center;flex-wrap:wrap}
.change summary code{font-weight:600}.wrap{overflow-x:auto}
table.diff{font:13px/1.45 var(--mono);width:100%}table.diff td{border:0;padding:0 8px;white-space:pre}
table.diff td.n{color:var(--mut);text-align:right;user-select:none;width:1%;padding:0 6px}
tr.a td{background:var(--add)}tr.a td.n{background:var(--addn)}tr.d td{background:var(--del)}tr.d td.n{background:var(--deln)}
tr.gap td{background:var(--soft);color:var(--mut);text-align:center;padding:2px}
.bar{height:8px;border-radius:4px;background:var(--soft);overflow:hidden;display:flex;min-width:120px}
.bar .p{background:var(--good)}.bar .f{background:var(--bad)}
form.review{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin:12px 0}
form.review textarea{width:100%;min-height:80px;font:inherit;padding:8px;background:var(--bg);color:var(--fg);border:1px solid var(--line);border-radius:6px}
form.review input[type=text],form.review input[type=password]{font:inherit;padding:4px 8px;background:var(--bg);color:var(--fg);border:1px solid var(--line);border-radius:6px}
form.review label{margin-right:16px}button{font:inherit;padding:6px 16px;border-radius:6px;border:0;background:var(--acc);color:var(--bg);font-weight:600;cursor:pointer}
.rev{border-left:4px solid var(--line);padding:6px 12px;margin:8px 0;background:var(--card)}.rev.human{border-color:var(--good)}.rev.ai{border-color:var(--ai)}
pre.code{font:13px/1.45 var(--mono);background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px;overflow-x:auto}
.flash{padding:8px 12px;border-radius:8px;margin:8px 0}
)CSS";

std::string page(const std::string& title, const std::string& project, const std::string& body) {
  std::string nav;
  if (!project.empty()) {
    std::string base = "/ui/p/" + url_enc(project);
    nav = "<nav><a href=\"" + base + "\">Commits</a><a href=\"" + base + "/work\">Uncommitted</a><a href=\"" + base +
          "/tests\">Tests</a></nav>";
  }
  return "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
         "<title>" + esc(title) + "</title><style>" + kCss + "</style></head><body><header><div class=\"in\"><b><a href=\"/ui\">hashstash</a>" +
         (project.empty() ? "" : " / <a href=\"/ui/p/" + url_enc(project) + "\">" + esc(project) + "</a>") + "</b>" + nav +
         "</div></header><main>" + body + "</main></body></html>";
}

// Line diff as a table with old/new line numbers; long unchanged runs fold.
std::string diff_table(const std::string& a, const std::string& b) {
  auto ops = diff_lines(text_lines(a), text_lines(b));
  const size_t ctx = 3;
  std::vector<bool> keep(ops.size(), false);
  for (size_t i = 0; i < ops.size(); ++i)
    if (ops[i].op != ' ')
      for (size_t j = (i >= ctx ? i - ctx : 0); j < std::min(ops.size(), i + ctx + 1); ++j) keep[j] = true;
  std::string o = "<div class=\"wrap\"><table class=\"diff\">";
  size_t i = 0;
  while (i < ops.size()) {
    if (!keep[i]) {
      size_t j = i;
      while (j < ops.size() && !keep[j]) ++j;
      o += "<tr class=\"gap\"><td colspan=\"4\">&#8943; " + std::to_string(j - i) + " unchanged line" +
           (j - i == 1 ? "" : "s") + "</td></tr>";
      i = j;
      continue;
    }
    auto& op = ops[i];
    std::string cls = op.op == '+' ? "a" : op.op == '-' ? "d" : "";
    o += "<tr class=\"" + cls + "\"><td class=\"n\">" + (op.a >= 0 ? std::to_string(op.a + 1) : "") + "</td><td class=\"n\">" +
         (op.b >= 0 ? std::to_string(op.b + 1) : "") + "</td><td class=\"n\">" + (op.op == ' ' ? "" : std::string(1, op.op)) +
         "</td><td>" + esc(op.text) + "</td></tr>";
    ++i;
  }
  return o + "</table></div>";
}

std::string def_link(const std::string& project, const std::string& qname) {
  return "<a href=\"/ui/p/" + url_enc(project) + "/def?name=" + url_enc(qname) + "\"><code>" + esc(qname) + "</code></a>";
}

// Per-definition and module-code changes of a namespace diff.
std::string changes_html(Repo& repo, const std::string& project, const NamespaceDiff& d, const json& tests, bool all,
                         const std::string& more_link) {
  Store& s = repo.store();
  std::string o;
  if (d.empty()) return "<p class=\"meta\">No changes.</p>";
  for (auto& f : d.files_added) o += "<div>" + badge("good", "new file") + " <code>" + esc(f) + "</code></div>";
  for (auto& f : d.files_removed) o += "<div>" + badge("bad", "deleted file") + " <code>" + esc(f) + "</code></div>";
  if (d.tests_changed) o += "<div>" + badge("muted", "test links changed") + "</div>";
  const size_t limit = 150;
  size_t shown = 0;
  for (auto& c : d.defs) {
    if (!all && shown >= limit) break;
    ++shown;
    auto [a, b] = change_texts(s, c);
    std::string name = c.kind == "removed" ? c.old_name : c.new_name;
    std::string title = c.kind.find("renamed") != std::string::npos ? esc(c.old_name) + " &rarr; " + def_link(project, c.new_name)
                        : c.kind == "removed"                       ? "<code>" + esc(name) + "</code>"
                                                                    : def_link(project, name);
    std::string kcls = c.kind == "added" ? "good" : c.kind == "removed" ? "bad" : "muted";
    int lines = count_lines(a) + count_lines(b);
    o += std::string("<details class=\"change\"") + (lines < 400 ? " open" : "") + "><summary>" + badge(kcls, c.kind) + " " +
         title + " " + (tests.contains(name) ? tests_badge(tests[name]) : "") + "</summary>" + diff_table(a, b) + "</details>";
  }
  if (!all && d.defs.size() > limit)
    o += "<p>" + std::to_string(d.defs.size() - limit) + " more changed definitions. <a href=\"" + more_link +
         "\">Show all</a></p>";
  for (auto& m : d.module_changes) {
    auto [a, b] = change_texts(s, m);
    std::string kcls = m.kind == "added" ? "good" : m.kind == "removed" ? "bad" : "muted";
    o += "<details class=\"change\" open><summary>" + badge(kcls, "module code " + m.kind) + " <code>" + esc(m.path) +
         "</code></summary>" + diff_table(a, b) + "</details>";
  }
  return o;
}

std::string reviews_html(Store& s, const Hash& commit) {
  std::string o;
  for (auto& r : reviews_of(s, commit)) {
    std::string verdict = r.verdict == "approved" ? badge("good", "approved")
                          : r.verdict == "changes_requested" ? badge("bad", "changes requested")
                                                             : badge("muted", "comment");
    o += "<div class=\"rev " + r.kind + "\">" + badge(r.kind == "human" ? "good" : "ai", r.kind == "human" ? "person" : "AI") + " <b>" +
         esc(r.reviewer) + "</b> " + verdict + " <span class=\"meta\">" + esc(format_time(r.time)) + "</span>";
    if (!r.summary.empty()) o += "<div>" + esc(r.summary) + "</div>";
    for (auto& c : r.comments)
      o += "<div class=\"meta\"><code>" + esc(c.value("name", "")) + (c.contains("line") ? ":" + c["line"].dump() : "") + "</code> " +
           esc(c.value("text", "")) + "</div>";
    o += "</div>";
  }
  if (o.empty()) o = "<p class=\"meta\">No reviews yet.</p>";
  return o;
}

std::string cookie(const httplib::Request& req, const std::string& name) {
  std::string all = req.get_header_value("Cookie");
  size_t p = 0;
  while (p < all.size()) {
    size_t semi = all.find(';', p);
    std::string kv = trim(all.substr(p, semi == std::string::npos ? std::string::npos : semi - p));
    if (starts_with(kv, name + "=")) return httplib::detail::decode_url(kv.substr(name.size() + 1), true);
    if (semi == std::string::npos) break;
    p = semi + 1;
  }
  return "";
}

// ---- pages ------------------------------------------------------------------

std::string projects_page(const UiSite& site) {
  std::string o = "<h1>Projects</h1><table><tr><th>Project</th><th>Head</th><th>Waiting for a person</th></tr>";
  for (auto& p : site.projects()) {
    Repo* r = site.open(p);
    if (!r) continue;
    auto commits = branch_commits(*r, r->branch(), 200);
    int waiting = 0;
    for (auto& h : commits)
      if (!review_status(r->store(), h).human_reviewed) ++waiting;
    o += "<tr><td><a href=\"/ui/p/" + url_enc(p) + "\">" + esc(p) + "</a></td><td class=\"h\">" +
         (commits.empty() ? "no commits" : "#" + short_hash(commits[0])) + "</td><td>" + std::to_string(waiting) + "</td></tr>";
  }
  return page("hashstash", "", o + "</table>");
}

std::string commit_rows(Repo& repo, const std::string& project, const std::vector<Hash>& commits) {
  std::string o = "<table><tr><th>Commit</th><th>Message</th><th>Author</th><th>When</th><th>Tests</th><th>Review</th></tr>";
  for (auto& h : commits) {
    json row = commit_row(repo, h);
    o += "<tr><td class=\"h\"><a href=\"/ui/p/" + url_enc(project) + "/commit/" + h + "\">#" + row["short"].get<std::string>() +
         "</a></td><td>" + esc(first_line(row["message"])) + " <span class=\"meta\">(" + std::to_string(row["changes"].get<size_t>()) +
         " changes)</span></td><td>" + esc(row["author"]) + "</td><td class=\"meta\">" + esc(row["when"]) + "</td><td>" +
         verify_badge(row["verification"]) + "</td><td>" + review_badge(row["review"]) + "</td></tr>";
  }
  return o + "</table>";
}

std::string overview_page(const UiSite& site, Repo& repo, const std::string& project) {
  Store& s = repo.store();
  auto commits = branch_commits(repo, repo.branch(), 100);
  int waiting = 0, ai_only = 0;
  std::vector<Hash> queue;
  for (auto& h : commits) {
    auto st = review_status(s, h);
    if (!st.human_reviewed) {
      ++waiting;
      queue.push_back(h);
      if (st.ai_reviewed) ++ai_only;
    }
  }
  auto d = diff_namespaces(s, load_namespace(s, repo.head_ns_hash()), repo.work_ns());
  int tf = 0, tfail = 0;
  for (auto& [q, t] : test_summaries(repo, repo.work_ns()))
    if (t.failed) ++tfail;
    else if (t.total == 0 && t.needs_tests) ++tf;
  std::string base = "/ui/p/" + url_enc(project);
  std::string o = "<h1>" + esc(project) + " <span class=\"meta\">on " + esc(repo.branch()) + "</span></h1>";
  o += "<div class=\"cards\">";
  o += "<div class=\"card\"><div class=\"n\">" + std::to_string(waiting) + "</div>commits not reviewed by a person" +
       (ai_only ? " (" + std::to_string(ai_only) + " AI reviewed)" : "") + "</div>";
  o += "<div class=\"card\"><div class=\"n\">" + std::to_string(d.defs.size()) + "</div><a href=\"" + base +
       "/work\">uncommitted changes</a></div>";
  o += "<div class=\"card\"><div class=\"n\">" + std::to_string(tfail) + "</div><a href=\"" + base +
       "/tests?problems=1\">functions with failing tests</a> &middot; " + std::to_string(tf) + " without tests</div>";
  o += "<div class=\"card\"><div class=\"n\">" + std::to_string(list_locks(repo).size()) + "</div>active locks</div></div>";
  if (approval_queue_enabled(repo, site.where) && !queue.empty()) {
    o += "<h2>Waiting for a person</h2><p class=\"meta\">Approval is optional; it records that someone looked.</p>";
    if (queue.size() > 20) queue.resize(20);
    o += commit_rows(repo, project, queue);
  }
  if (site.where == "hub") {
    json pushes = push_log(s, 20);
    if (!pushes.empty()) {
      o += "<h2>Pushes</h2><table><tr><th>When</th><th>Who</th><th>Branch</th><th>Commits</th><th>Reviewed by a person</th></tr>";
      for (auto& p : pushes) {
        std::string oldh = p["old"], newh = p["new"];
        int n = 0, human = 0, ai = 0;
        std::string h = newh;
        while (!h.empty() && h != oldh && n < 500) {
          ++n;
          auto st = review_status(s, h);
          if (st.human_reviewed) ++human;
          else if (st.ai_reviewed) ++ai;
          Commit c = load_commit(s, h);
          h = c.parents.empty() ? "" : c.parents[0];
        }
        std::string flag = human == n ? badge("good", "all") : human ? badge("warn", std::to_string(human) + " of " + std::to_string(n))
                           : ai == n  ? badge("ai", "AI only")
                                      : badge("muted", "none");
        o += "<tr><td class=\"meta\">" + esc(format_time(p["time"])) + "</td><td>" + esc(p["agent"]) + "</td><td>" + esc(p["branch"]) +
             "</td><td><a href=\"" + base + "/commit/" + newh + "\">#" + short_hash(newh) + "</a> (" + std::to_string(n) + ")</td><td>" +
             flag + "</td></tr>";
      }
      o += "</table>";
    }
  }
  o += "<h2>Commits</h2>" + (commits.empty() ? std::string("<p class=\"meta\">No commits yet.</p>") : commit_rows(repo, project, commits));
  return page(project, project, o);
}

std::string commit_page(const UiSite& site, Repo& repo, const std::string& project, const Hash& h, const httplib::Request& req,
                        const std::string& flash) {
  Store& s = repo.store();
  json row = commit_row(repo, h);
  Commit c = load_commit(s, h);
  Namespace now = load_namespace(s, c.ns);
  Namespace before = c.parents.empty() ? load_namespace(s, empty_namespace(s)) : load_namespace(s, load_commit(s, c.parents[0]).ns);
  auto d = diff_namespaces(s, before, now);
  std::set<std::string> names;
  for (auto& dc : d.defs)
    if (!dc.new_name.empty()) names.insert(dc.new_name);
  json tests = json::object();
  int tp = 0, tt = 0, tf = 0;
  for (auto& [q, t] : test_summaries(repo, now, &names)) {
    tests[q] = t.to_json();
    tp += t.passed, tt += t.total, tf += t.failed;
  }
  std::string base = "/ui/p/" + url_enc(project);
  std::string o = flash;
  o += "<h1>" + esc(first_line(c.message)) + "</h1>";
  if (c.message.find('\n') != std::string::npos) o += "<p>" + esc(c.message.substr(c.message.find('\n') + 1)) + "</p>";
  o += "<p class=\"meta\">#" + short_hash(h) + " by <b>" + esc(c.author) + "</b>, " + esc(format_time(c.time));
  for (auto& p : c.parents) o += " &middot; parent <a href=\"" + base + "/commit/" + p + "\">#" + short_hash(p) + "</a>";
  o += "</p><p>" + verify_badge(row["verification"]) + " " + review_badge(row["review"]) + " " +
       badge(tf ? "bad" : "muted", std::to_string(tp) + "/" + std::to_string(tt) + " tests passed on changed functions") + "</p>";
  o += "<h2>Changes</h2>" + changes_html(repo, project, d, tests, req.has_param("all"), base + "/commit/" + h + "?all=1");
  o += "<h2>Reviews</h2>" + reviews_html(s, h);
  std::string who = cookie(req, "hs_reviewer");
  if (who.empty()) who = repo.config().author;
  o += "<form class=\"review\" method=\"post\" action=\"" + base + "/commit/" + h + "/review\"><b>Your review</b> "
       "<span class=\"meta\">(recorded as a person's review; never required)</span><p>"
       "<label><input type=\"radio\" name=\"verdict\" value=\"approved\" checked> Approve</label>"
       "<label><input type=\"radio\" name=\"verdict\" value=\"changes_requested\"> Request changes</label>"
       "<label><input type=\"radio\" name=\"verdict\" value=\"comment\"> Comment</label></p>"
       "<textarea name=\"summary\" placeholder=\"What you checked, what should change (optional when approving)\"></textarea>"
       "<p>Name <input type=\"text\" name=\"reviewer\" required value=\"" + esc(who) + "\"> ";
  if (!site.review_token.empty()) o += "Review token <input type=\"password\" name=\"token\" required> ";
  o += "<button type=\"submit\">Submit review</button></p></form>";
  return page(first_line(c.message), project, o);
}

std::string work_page(Repo& repo, const std::string& project, const httplib::Request& req) {
  Store& s = repo.store();
  Namespace work = repo.work_ns();
  auto d = diff_namespaces(s, load_namespace(s, repo.head_ns_hash()), work);
  std::set<std::string> names;
  for (auto& dc : d.defs)
    if (!dc.new_name.empty()) names.insert(dc.new_name);
  json tests = json::object();
  for (auto& [q, t] : test_summaries(repo, work, &names)) tests[q] = t.to_json();
  std::string v = verification_status(repo, repo.work_ns_hash());
  std::string o = "<h1>Uncommitted changes</h1><p>" + verify_badge(v.empty() ? "not verified" : v) + "</p>";
  o += changes_html(repo, project, d, tests, req.has_param("all"), "/ui/p/" + url_enc(project) + "/work?all=1");
  return page("Uncommitted changes", project, o);
}

std::string tests_page(Repo& repo, const std::string& project, const httplib::Request& req) {
  bool problems = req.has_param("problems");
  std::string prefix = req.has_param("prefix") ? req.get_param_value("prefix") : "";
  std::vector<std::pair<int, json>> rows;
  int funcs = 0, total = 0, passed = 0, failed = 0, missing = 0;
  for (auto& [q, t] : test_summaries(repo, repo.work_ns())) {
    if (!prefix.empty() && !starts_with(q, prefix)) continue;
    if (t.total == 0 && !t.needs_tests) continue;
    ++funcs, total += t.total, passed += t.passed, failed += t.failed;
    if (t.total == 0) ++missing;
    bool ok = t.total > 0 && t.passed == t.total;
    if (problems && ok) continue;
    json j = t.to_json();
    j["name"] = q;
    rows.push_back({t.failed ? 0 : t.total == 0 ? 1 : t.not_run ? 2 : 3, j});
  }
  std::stable_sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first < b.first; });
  std::string base = "/ui/p/" + url_enc(project);
  std::string o = "<h1>Tests per function</h1><div class=\"cards\"><div class=\"card\"><div class=\"n\">" + std::to_string(funcs) +
                  "</div>functions</div><div class=\"card\"><div class=\"n\">" + std::to_string(passed) + "/" + std::to_string(total) +
                  "</div>tests passed</div><div class=\"card\"><div class=\"n\">" + std::to_string(failed) +
                  "</div>failing</div><div class=\"card\"><div class=\"n\">" + std::to_string(missing) + "</div>functions without tests</div></div>";
  o += "<p>" + std::string(problems ? "<a href=\"" + base + "/tests\">Show all</a>" : "<a href=\"" + base + "/tests?problems=1\">Only problems</a>") +
       " &middot; results count only when run on the current code and tests (hs verify)</p>";
  o += "<table><tr><th>Function</th><th>Result</th><th></th></tr>";
  for (auto& [rank, j] : rows)
    o += "<tr><td>" + def_link(project, j["name"]) + "</td><td>" + tests_badge(j) + "</td><td style=\"width:30%\">" + tests_bar(j) + "</td></tr>";
  return page("Tests", project, o + "</table>");
}

std::string def_page(Repo& repo, const std::string& project, const std::string& name) {
  json c = context_of(repo, name, true);
  std::string q = c.value("name", name);
  std::string o = "<h1><code>" + esc(q) + "</code></h1><p class=\"meta\">" + esc(c.value("kind", "")) + " &middot; #" +
                  esc(c.value("hash", "")) + "</p>";
  if (c.contains("locked_by"))
    o += "<p>" + badge("warn", "locked by " + c["locked_by"].value("holder", "") + " until " + c["locked_by"].value("until", "")) + "</p>";
  // Tests: the summary first, then each test's latest result on this code.
  json t = c["tests"];
  o += "<h2>Tests " + (t.contains("results") ? tests_badge(t["results"]) : "") + "</h2>";
  if (t["tests"].empty()) o += "<p class=\"meta\">No tests linked.</p>";
  else {
    Namespace work = repo.work_ns();
    Hash suite = suite_hash(repo, work, q);
    std::set<std::string> one{q};
    auto sums = test_summaries(repo, work, &one);
    std::map<std::string, std::string> status_of;
    if (sums.count(q))
      for (auto& [tn, stt] : sums[q].results) status_of[tn] = stt;
    Stmt st(repo.store().db(), "SELECT output FROM test_result WHERE suite = ? AND test = ?");
    o += "<table><tr><th>Test</th><th>Result on this code</th></tr>";
    for (auto& x : t["tests"]) {
      std::string tn = x["name"];
      std::string status = status_of.count(tn) ? status_of[tn] : "not run";
      std::string out;
      if (status == "failed") {
        st.bind(1, suite).bind(2, tn);
        if (st.step()) out = st.text(0);
        st.reset();
      }
      o += "<tr><td>" + def_link(project, tn) + "</td><td>" + badge(status == "passed" ? "good" : status == "failed" ? "bad" : "muted", status);
      if (!out.empty()) o += "<details><summary>output</summary><pre class=\"code\">" + esc(out.substr(0, 20000)) + "</pre></details>";
      o += "</td></tr>";
    }
    o += "</table>";
  }
  o += "<h2>Code</h2><pre class=\"code\">";
  int n = 0;
  for (auto& l : text_lines(c.value("code", ""))) {
    ++n;
    o += "<span class=\"meta\">" + std::to_string(n) + (n < 10 ? "   " : n < 100 ? "  " : " ") + "</span>" + esc(l) + "\n";
  }
  o += "</pre>";
  if (!c["notes"].empty()) {
    o += "<h2>Notes</h2>";
    for (auto& nt : c["notes"])
      o += "<div class=\"rev\"><b>lines " + nt["lines"][0].dump() + "-" + nt["lines"][1].dump() + "</b> " + esc(nt.value("text", "")) +
           " <span class=\"meta\">" + esc(nt.value("author", "")) + (nt.value("stale", false) ? " (its lines were removed)" : "") + "</span></div>";
  }
  if (!c["context"].empty()) {
    o += "<h2>Context</h2>";
    for (auto& x : c["context"])
      o += "<div class=\"rev\">" + badge("muted", x.value("kind", "")) + " " + esc(x.value("text", "")) + " <span class=\"meta\">" +
           esc(x.value("author", "")) + ", " + esc(x.value("time", "")) + "</span></div>";
  }
  if (!c["callers"].empty()) {
    o += "<h2>Called by</h2><p>";
    std::set<std::string> seen;
    for (auto& x : c["callers"])
      if (seen.insert(x.value("caller", "")).second) o += def_link(project, x.value("caller", "")) + " ";
    o += "</p>";
  }
  if (!c["history"].empty()) {
    o += "<h2>History</h2><table><tr><th>Commit</th><th>Change</th><th>Message</th><th>When</th></tr>";
    for (auto& hx : c["history"]) {
      std::string sh = hx.value("commit", "");
      o += "<tr><td class=\"h\"><a href=\"/ui/p/" + url_enc(project) + "/commit/" + sh + "\">#" + esc(sh) + "</a></td><td>" +
           esc(hx.value("change", "")) + "</td><td>" + esc(hx.value("message", "")) + "</td><td class=\"meta\">" + esc(hx.value("time", "")) +
           "</td></tr>";
    }
    o += "</table>";
  }
  return page(q, project, o);
}

}  // namespace

void mount_ui(httplib::Server& srv, const UiSite& site_in) {
  auto site = std::make_shared<UiSite>(site_in);
  auto html = [](httplib::Response& res, const std::string& body, int status = 200) {
    res.status = status;
    res.set_content(body, "text/html; charset=utf-8");
  };
  // Runs a page with the store lock held and errors shown as a page.
  auto guarded = [site, html](httplib::Response& res, const std::function<std::string()>& fn) {
    std::unique_lock<std::mutex> g;
    if (site->mu) g = std::unique_lock<std::mutex>(*site->mu);
    try {
      std::string body = fn();
      if (!body.empty()) html(res, body);  // empty: the page set a redirect
    } catch (std::exception& e) {
      html(res, page("Error", "", "<h1>Something went wrong</h1><p>" + esc(e.what()) + "</p>"), 400);
    }
  };
  auto need_repo = [site](const std::string& p) -> Repo& {
    Repo* r = site->open(p);
    if (!r) throw std::runtime_error("no project " + p);
    return *r;
  };
  const std::string P = R"(/ui/p/([A-Za-z0-9._-]+))";

  srv.Get("/ui", [=](const httplib::Request&, httplib::Response& res) {
    guarded(res, [&] {
      auto ps = site->projects();
      if (ps.size() == 1) {
        res.set_redirect("/ui/p/" + url_enc(ps[0]));
        return std::string();
      }
      return projects_page(*site);
    });
  });
  srv.Get(P, [=](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] { return overview_page(*site, need_repo(req.matches[1]), req.matches[1]); });
  });
  srv.Get(P + R"(/commit/([0-9a-z#]+))", [=](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      Repo& r = need_repo(req.matches[1]);
      std::string flash;
      if (req.has_param("saved"))
        flash = "<div class=\"flash good\">Review saved" + (req.get_param_value("saved") == "1" ? std::string(".") : ": " + esc(req.get_param_value("saved"))) + "</div>";
      return commit_page(*site, r, req.matches[1], resolve_commit(r, req.matches[2]), req, flash);
    });
  });
  srv.Post(P + R"(/commit/([0-9a-z]+)/review)", [=](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] {
      Repo& r = need_repo(req.matches[1]);
      if (!site->review_token.empty() && req.get_param_value("token") != site->review_token)
        throw std::runtime_error("wrong review token: ask the hub's owner for it");
      std::string reviewer = trim(req.get_param_value("reviewer"));
      if (reviewer.empty()) throw std::runtime_error("enter your name");
      Review rv = add_review(r, req.matches[2], req.get_param_value("verdict"), req.get_param_value("summary"), "human",
                             json::array(), reviewer);
      std::string note = site->where == "local" ? share_reviews(r) : "";
      res.set_header("Set-Cookie", "hs_reviewer=" + url_enc(reviewer) + "; Path=/; Max-Age=31536000; SameSite=Lax");
      res.set_redirect("/ui/p/" + url_enc(req.matches[1]) + "/commit/" + rv.commit + "?saved=" + (note.empty() ? "1" : url_enc(note)), 303);
      return std::string();
    });
  });
  srv.Get(P + "/work", [=](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] { return work_page(need_repo(req.matches[1]), req.matches[1], req); });
  });
  srv.Get(P + "/tests", [=](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] { return tests_page(need_repo(req.matches[1]), req.matches[1], req); });
  });
  srv.Get(P + "/def", [=](const httplib::Request& req, httplib::Response& res) {
    guarded(res, [&] { return def_page(need_repo(req.matches[1]), req.matches[1], req.get_param_value("name")); });
  });
}

}  // namespace hs
