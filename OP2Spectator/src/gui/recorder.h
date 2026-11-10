// recorder.h - OP2Spectator structured match recorder.
//
// Turns the decoded command/chat/roster stream into durable, analysable match files. Used ONLY from
// the single net worker thread, so it needs no internal locking. On begin() it opens, next to the exe:
//   op2spectator_match_<stamp>.log      - human-readable master log: lobby, roster, chat, every action
//   op2spectator_match_<stamp>.jsonl    - one JSON object per in-game action (for tooling/analysis)
//   op2spectator_match_<stamp>.summary  - per-player action timeline + build order (written at end())
// where <stamp> is the local match-start time. In-game lines carry the OP2 game tick/mark; lobby-phase
// lines carry tick -1.
//
// Scope note: this records player INPUT (decisions) only - the deterministic lockstep sim never puts
// derived state (resources, population, morale, HP) on the wire, so those are intentionally absent.
#pragma once

#include "op2proto.h"
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <ctime>

namespace op2 {

class Recorder {
public:
    ~Recorder() { closeFiles(); }

    // Open the match files in directory `dir` (already ends with a path separator). Idempotent: a
    // second begin() while active is ignored, so re-discovery during one run doesn't reopen.
    void begin(const std::string& dir, const std::string& hostIp, const std::string& gameName) {
        if (master_) return;
        char st[32]; std::time_t t = std::time(nullptr); std::tm* lt = std::localtime(&t);
        if (lt) std::strftime(st, sizeof(st), "%Y%m%d_%H%M%S", lt); else std::snprintf(st, sizeof(st), "match");
        base_   = dir + "op2spectator_match_" + st;
        master_ = std::fopen((base_ + ".log").c_str(),   "w");
        jsonl_  = std::fopen((base_ + ".jsonl").c_str(),  "w");
        line(std::string("=== OP2Spectator match log ===  host ") + hostIp + "  game '" + gameName + "'  started " + st);
    }

    // Once-per-match metadata, written when the game starts (final roster + host bind style known).
    void matchHeader(const LobbyInfo& lobby, uint32_t hostVer, bool nethelper, int ourSlot) {
        if (!master_ || headerWritten_) return;
        headerWritten_ = true;
        char v[16]; std::snprintf(v, sizeof(v), "%u.%u.%u.%u", (hostVer>>24)&0xff,(hostVer>>16)&0xff,(hostVer>>8)&0xff,hostVer&0xff);
        line(std::string("--- game starting ---  map '") + lobby.mapDll + "'  hostVer " + v
             + "  NetHelper " + (nethelper ? "ON" : "OFF"));
        for (int i = 0; i < 6; i++) {
            const auto& s = lobby.slot[i];
            if (!s.occupied) continue;
            slotName_[i] = s.name;
            line(std::string("  slot ") + std::to_string(i) + ": " + s.name
                 + " (" + colorName(s.color) + " " + (s.eden ? "Eden" : "Plymouth") + ")"
                 + (i == ourSlot ? "  <- us (spectator)" : "") + (s.ready ? "  [ready]" : ""));
        }
    }

    // A chat message; from=="" marks a system/roster line. tick<0 => lobby phase.
    void chat(int tick, const std::string& from, const std::string& text, bool self) {
        if (!master_) return;
        std::string pre = stamp(tick);
        if (from.empty())      line(pre + text);                               // system/roster line (already framed)
        else                   line(pre + (self ? "(us) " : "") + from + ": " + text);
    }

    // An in-game action block. Deduped here by (slot, mark, type) - the sliding window resends each
    // block many times, but always pinned to the same execution mark, so this yields each action once.
    void action(int mark, int slot, const std::string& player, uint8_t type,
                const std::string& name, const std::string& detail) {
        if (!master_) return;
        uint64_t key = ((uint64_t)(slot & 0xFF) << 40) | ((uint64_t)((uint32_t)mark) << 8) | type;
        if (dup(key)) return;
        if (slotName_[slot & 7].empty()) slotName_[slot & 7] = player;
        actionCount_++;
        actions_.push_back(Rec{ mark, slot & 7, type, name, detail });
        // master log
        line(stamp(mark) + "slot" + std::to_string(slot & 7) + " " + player + "  " + name + (detail.empty() ? "" : " " + detail));
        // jsonl
        if (jsonl_) {
            std::fprintf(jsonl_,
                "{\"mark\":%d,\"gamemark\":%d,\"slot\":%d,\"player\":\"%s\",\"type\":%u,\"action\":\"%s\",\"detail\":\"%s\"}\n",
                mark, mark / 100, slot & 7, jesc(player).c_str(), (unsigned)type, jesc(name).c_str(), jesc(detail).c_str());
            std::fflush(jsonl_);
        }
    }

    // Write the per-player summary (action counts + build order) and close the files.
    void end(const std::string& reason) {
        if (!master_) return;
        line(std::string("=== match ended: ") + reason + " ===");
        FILE* sum = std::fopen((base_ + ".summary").c_str(), "w");
        if (sum) {
            std::fprintf(sum, "=== OP2Spectator match summary ===\n%s\ntotal actions recorded: %ld\n\n",
                         reason.c_str(), actionCount_);
            for (int s = 0; s < 6; s++) {
                bool any = false; for (const auto& a : actions_) if (a.slot == s) { any = true; break; }
                if (!any) continue;
                std::fprintf(sum, "Player slot %d (%s):\n", s, slotName_[s].empty() ? "?" : slotName_[s].c_str());
                // action-type counts
                int cnt[256] = {0};
                for (const auto& a : actions_) if (a.slot == s) cnt[a.type]++;
                std::fprintf(sum, "  action counts:");
                for (const auto& a : actions_) {
                    if (a.slot != s) continue;
                    if (cnt[a.type] < 0) continue;        // already printed
                    std::fprintf(sum, " %s x%d", a.name.c_str(), cnt[a.type]); cnt[a.type] = -1;
                }
                std::fprintf(sum, "\n  build order (build / produce / research, by mark):\n");
                int n = 0;
                for (const auto& a : actions_) {
                    if (a.slot != s || !isBuildOrder(a.type)) continue;
                    std::fprintf(sum, "    [mark %d] %s %s\n", a.mark, a.name.c_str(), a.detail.c_str());
                    n++;
                }
                if (!n) std::fprintf(sum, "    (none captured)\n");
                std::fprintf(sum, "\n");
            }
            std::fclose(sum);
            line(std::string("summary written -> ") + base_ + ".summary");
        }
        closeFiles();
    }

    bool active() const { return master_ != nullptr; }

    // Path prefix of the match files (…/op2spectator_match_<stamp>), for a UI "recording" indicator.
    const std::string& baseName() const { return base_; }
    long actionCount() const { return actionCount_; }

private:
    FILE* master_ = nullptr;
    FILE* jsonl_  = nullptr;
    std::string base_;
    bool   headerWritten_ = false;
    std::string slotName_[8];
    uint64_t seen_[1024] = {0};
    int    seenPos_ = 0;
    long   actionCount_ = 0;

    struct Rec { int mark; int slot; uint8_t type; std::string name, detail; };
    std::vector<Rec> actions_;

    void line(const std::string& s) { if (master_) { std::fputs(s.c_str(), master_); std::fputc('\n', master_); std::fflush(master_); } }
    static std::string stamp(int tick) {
        char b[24];
        if (tick < 0) std::snprintf(b, sizeof(b), "[lobby]      ");
        else          std::snprintf(b, sizeof(b), "[t%-6d M%-3d] ", tick, tick / 100);
        return b;
    }
    bool dup(uint64_t key) {
        for (int i = 0; i < 1024; i++) if (seen_[i] == key) return true;
        seen_[seenPos_] = key; seenPos_ = (seenPos_ + 1) & 1023;
        return false;
    }
    static bool isBuildOrder(uint8_t t) {
        return t == 0x06 || t == 0x07 || t == 0x09 || t == 0x16 || t == 0x17 || t == 0x20;  // build/buildwall/produce/research/train/create
    }
    static const char* colorName(int c) {
        static const char* n[6] = { "Blue","Red","Green","Yellow","Cyan","Magenta" };
        return (c >= 0 && c < 6) ? n[c] : "?";
    }
    static std::string jesc(const std::string& s) {
        std::string o; o.reserve(s.size() + 4);
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
            else if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else if (c == '\t') o += "\\t";
            else if (c < 0x20)  { char h[8]; std::snprintf(h, sizeof(h), "\\u%04x", c); o += h; }
            else o += (char)c;
        }
        return o;
    }
    void closeFiles() {
        if (jsonl_)  { std::fclose(jsonl_);  jsonl_  = nullptr; }
        if (master_) { std::fclose(master_); master_ = nullptr; }
    }
};

} // namespace op2
