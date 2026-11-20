#include <set>
#include <string>

#include "core/help.h"
#include "core/settings_schema.h"
#include "test.h"

using namespace kamil;

TEST(help_examples_are_valid_settings) {
    // Every settings.yaml example in Kamil: Help loads without a single diagnostic, so the help
    // cannot drift away from the settings.
    size_t examples = 0;
    for (const auto& t : help_topics()) {
        if (!t.yaml) continue;
        ++examples;
        auto r = load_settings_text(builtin_schema(), t.yaml, std::string("help-") + t.id + ".yaml");
        for (const auto& d : r.diagnostics) std::printf("    %s: %s\n", t.id, d.to_string().c_str());
        CHECK(!r.parse_failed);
        CHECK(r.diagnostics.empty());
    }
    CHECK(examples >= 8);
}

TEST(help_page) {
    std::set<std::string> ids;
    for (const auto& t : help_topics()) {
        CHECK(ids.insert(t.id).second);  // unique anchors
        CHECK(t.title_en && *t.title_en && t.title_tr && *t.title_tr && t.body_en && *t.body_en && t.body_tr && *t.body_tr);
    }
    for (bool tr : {false, true}) {
        const std::string page = help_html(tr, "1.2 <abc>");
        CHECK(page.rfind("<!doctype html>", 0) == 0);
        for (const auto& t : help_topics()) {
            CHECK(page.find(std::string("<section id=\"") + t.id + "\">") != std::string::npos);
            CHECK(page.find(tr ? t.title_tr : t.title_en) != std::string::npos);
        }
        CHECK(page.find("1.2 &lt;abc&gt;") != std::string::npos);  // escaped
        CHECK(page.find(tr ? "Kopyala" : "Copy") != std::string::npos);
        // Markup is balanced.
        auto count = [&](const char* s) {
            size_t n = 0;
            for (size_t p = page.find(s); p != std::string::npos; p = page.find(s, p + 1)) ++n;
            return n;
        };
        CHECK_EQ(count("<code>"), count("</code>"));
        CHECK_EQ(count("<b>"), count("</b>"));
        CHECK_EQ(count("<ul>"), count("</ul>"));
        CHECK_EQ(count("<section"), count("</section>"));
    }
    // A Windows path in an example is shown as written.
    CHECK(help_html(false, "").find("'D:\\tools\\scripts'") != std::string::npos);
}
