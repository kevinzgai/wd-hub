#pragma once
#include <cstddef>

// Splits a JSON array at complete station objects; quoted braces and nested
// objects are respected. One oversized object is skipped without truncation.
// Caller owns the fixed buffer and parses/frees each object immediately.
class RadioJsonStream {
public:
    RadioJsonStream(char *buffer, std::size_t capacity) : buffer_(buffer), capacity_(capacity) {}
    template<class Emit> void Feed(const char *bytes, std::size_t size, Emit emit) {
        for (std::size_t i = 0; i < size; ++i) {
            const char c = bytes[i];
            if (bad_ || complete_) continue;
            if (!started_) {
                if (Space(c)) continue;
                if (c != '[') { bad_ = true; continue; }
                started_ = true;
                continue;
            }
            if (!depth_) {
                if (Space(c)) continue;
                if (c == ']' && !after_comma_) { complete_ = true; continue; }
                if (c == ',' && need_comma_) { need_comma_ = false; after_comma_ = true; continue; }
                if (c != '{' || need_comma_) { bad_ = true; continue; }
                depth_ = 1; used_ = 0; overflow_ = false; quoted_ = escaped_ = false;
                after_comma_ = false;
            } else if (quoted_) {
                if (escaped_) escaped_ = false;
                else if (c == '\\') escaped_ = true;
                else if (c == '"') quoted_ = false;
            } else {
                if (c == '"') quoted_ = true;
                else if (c == '{') ++depth_;
                else if (c == '}') --depth_;
            }
            if (used_ + 1 < capacity_) buffer_[used_++] = c;
            else overflow_ = true;
            if (!depth_) {
                buffer_[used_] = '\0';
                if (!overflow_) emit(buffer_);
                need_comma_ = true;
            }
        }
    }
    bool Complete() const { return complete_ && !bad_; }
private:
    static bool Space(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
    char *buffer_;
    std::size_t capacity_, used_ = 0;
    unsigned depth_ = 0;
    bool started_ = false, complete_ = false, bad_ = false, quoted_ = false,
         escaped_ = false, overflow_ = false, need_comma_ = false, after_comma_ = false;
};
