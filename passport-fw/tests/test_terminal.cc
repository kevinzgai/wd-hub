#include "ui/display_text.h"
#include "ui/utf8_tail.h"
#include "launcher/wake_gate.h"
#include <cassert>
#include <string>

int main() {
    wake_gate_t gate{};
    assert(!wake_gate_consume(&gate, true));
    wake_gate_arm(&gate);
    // Both short-click and long-hold tails must be swallowed, even after release.
    for (int event = 0; event < 5; ++event) assert(wake_gate_consume(&gate, false));
    assert(!wake_gate_consume(&gate, true));
    assert(!wake_gate_consume(&gate, false));
    wake_gate_arm(&gate);
    assert(wake_gate_consume(&gate, false));
    assert(!wake_gate_consume(&gate, true));

    assert(passport::utf8_tail("中文A", 4) == "文A");
    assert(passport::utf8_tail("中文A", 3) == "A");
    assert(passport::utf8_tail("中文A", 0).empty());
    assert(passport::utf8_tail("A😀B", 5) == "😀B");
    assert(passport::utf8_tail("A😀B", 4) == "B");
    std::string reply = "中文";
    passport::append_utf8_tail(reply, "你好", 9);
    assert(reply == "文你好");
    passport::append_utf8_tail(reply, std::string(100000, 'x') + "完", 12);
    assert(reply == std::string(9, 'x') + "完");
    for (int i = 0; i < 2000; ++i) {
        passport::append_utf8_tail(reply, "中😀A", 1200);
        assert(reply.size() <= 1200);
        assert((static_cast<unsigned char>(reply[0]) & 0xC0) != 0x80);
    }

    // Emoji and full-width ASCII would render as boxes in the subset font.
    assert(passport::fold_display_text("好的😊👍🏻") == "好的");
    assert(passport::fold_display_text("❤️✨⭐⏰") == "");
    assert(passport::fold_display_text("👨‍👩‍👧家庭") == "家庭");
    assert(passport::fold_display_text("１＋２＝３，５０％") == "1+2=3，50%");
    assert(passport::fold_display_text("ＡＩ：好！（是）？～；") == "AI：好！（是）？～；");
    assert(passport::fold_display_text("中\xE3\x80\x80" "文") == "中 文");
    assert(passport::fold_display_text("【重点】25℃，3×4") == "【重点】25℃，3×4");
    assert(passport::fold_display_text("A\xE4\xB8") == "A\xE4\xB8");  // truncated sequence
    assert(passport::fold_display_text("A\xFF" "B") == "A\xFF" "B");
}
