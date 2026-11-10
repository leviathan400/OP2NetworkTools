// main.cpp - OP2DummyPlayer GUI (Dear ImGui + GLFW + OpenGL3).
//
// This file is ONLY presentation: it owns an op2::Client, reads a thread-safe ClientState
// snapshot once per frame, and renders it. It never touches sockets or wire bytes. All
// networking lives in the shared library ../../../common/op2net (op2client / op2proto), so the
// GUI and the net-client are cleanly separable (the net layer has no GUI dependency at all).
#include "op2client.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstring>

static void glfw_error(int e, const char* d) { fprintf(stderr, "GLFW error %d: %s\n", e, d); }

// ImGui color for a given OP2 player color index (Blue,Red,Green,Yellow,Cyan,Magenta).
static ImVec4 op2_color(int c) {
    switch (c) {
        case 0: return ImVec4(0.30f,0.45f,1.00f,1); // Blue
        case 1: return ImVec4(1.00f,0.30f,0.30f,1); // Red
        case 2: return ImVec4(0.30f,0.90f,0.30f,1); // Green
        case 3: return ImVec4(0.95f,0.90f,0.25f,1); // Yellow
        case 4: return ImVec4(0.30f,0.90f,0.95f,1); // Cyan
        case 5: return ImVec4(0.95f,0.40f,0.95f,1); // Magenta
    }
    return ImVec4(0.7f,0.7f,0.7f,1);
}

int main(int argc, char** argv) {
    op2::set_debug_log_name("op2dummyplayer_debug.txt");
    op2::debug_log("=== OP2DummyPlayer started ===");

    // Optional startup flags (so the app can be driven headlessly without clicking the GUI):
    //   --scan / --autojoin   scan the LAN and join the first hosted game
    //   --ip <addr>           join a specific host IP directly
    //   --ver a.b.c.d         advertise this TApp::Version (default 1.4.0.1 for OPU 1.4.1; pass
    //                         1.3.0.4 for retail 1.3.6). The host drops a peer whose version != its own.
    //   --debug               self-test run: scan+join, post a "leaving" chat, leave cleanly, then exit.
    bool autoScan = false, debugRun = false;
    std::string autoIp;
    uint32_t tappVer = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--scan" || a == "--autojoin") autoScan = true;
        else if (a == "--debug") { debugRun = true; autoScan = true; }
        else if ((a == "--ip" || a == "--join") && i + 1 < argc) autoIp = argv[++i];
        else if (a == "--ver" && i + 1 < argc) {
            unsigned va=0,vb=0,vc=0,vd=0;
            if (sscanf(argv[++i], "%u.%u.%u.%u", &va,&vb,&vc,&vd) == 4)
                tappVer = ((va&0xff)<<24)|((vb&0xff)<<16)|((vc&0xff)<<8)|(vd&0xff);
        }
    }

    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) { op2::debug_log("FATAL: glfwInit failed"); fprintf(stderr, "glfwInit failed\n"); return 1; }
#if defined(__APPLE__)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    const char* glsl_version = "#version 150";
#else
    const char* glsl_version = "#version 130";
#endif
    GLFWwindow* win = glfwCreateWindow(720, 760, "OP2DummyPlayer", nullptr, nullptr);
    if (!win) { fprintf(stderr, "window creation failed\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    op2::Client client;
    { op2::ClientOptions o; o.helloMessage = "Hello from OP2DummyPlayer!"; o.commandCensus = false; client.setOptions(o); }
    if (tappVer) client.setTappVersion(tappVer);   // apply --ver before any connect

    // Fire the requested auto-connect once, before the UI loop (the GUI still works normally after).
    if (!autoIp.empty()) { op2::debug_log("auto-connect (cmdline): join " + autoIp); client.connectIp(autoIp); }
    else if (autoScan)   { op2::debug_log("auto-connect (cmdline): scan LAN"); client.connectScan(); }

    // UI-owned input buffers
    static char nameBuf[16] = "OPU";
    static char ipBuf[64]   = "";
    static char chatBuf[128] = "";
    bool nameApplied = false;

    // --debug self-test run (a tiny per-frame state machine driven below):
    //   DBG_WAIT    -> waiting to reach the lobby (or to fail). On lobby: post a chat, go DBG_SENT.
    //   DBG_SENT    -> +2 s (let the chat reliably send), then disconnect cleanly, go DBG_LEAVING.
    //   DBG_LEAVING -> +0.3 s, then ask the window to close -> normal shutdown, go DBG_DONE.
    enum { DBG_WAIT, DBG_SENT, DBG_LEAVING, DBG_DONE } dbgState = DBG_WAIT;
    double dbgMark = 0.0;

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        op2::ClientState st = client.snapshot();
        bool busy = client.running();

        // --debug: drive the whole run, then close the window for a clean exit.
        if (debugRun) {
            double now = glfwGetTime();
            bool inLobby = (st.phase == op2::Phase::InLobby || st.phase == op2::Phase::InGame);
            if (dbgState == DBG_WAIT && inLobby) {
                client.sendChat("Doing debug run, Leaving");
                op2::debug_log("[debug] joined - posted leaving chat");
                dbgMark = now; dbgState = DBG_SENT;
            } else if (dbgState == DBG_WAIT && (st.phase == op2::Phase::Error || st.disconnected)) {
                if (dbgMark == 0.0) dbgMark = now;                    // join failed - hold briefly then close
                if (now - dbgMark > 1.5) { op2::debug_log("[debug] join failed - closing"); glfwSetWindowShouldClose(win, 1); }
            } else if (dbgState == DBG_SENT && now - dbgMark > 2.0) {
                op2::debug_log("[debug] leaving cleanly");
                client.disconnect();                                  // clean lobby/in-game quit, joins worker
                dbgMark = now; dbgState = DBG_LEAVING;
            } else if (dbgState == DBG_LEAVING && now - dbgMark > 0.3) {
                op2::debug_log("[debug] done - closing app");
                glfwSetWindowShouldClose(win, 1);                     // clean window-close -> normal shutdown
                dbgState = DBG_DONE;
            }
        }

        // one full-window panel
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("OP2DummyPlayer", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::TextColored(ImVec4(0.4f,0.9f,0.4f,1), "OP2DummyPlayer");
        ImGui::SameLine();
        ImGui::TextDisabled("- external Outpost 2 multiplayer client");
        ImGui::Separator();

        // ---- identity + connection ----
        ImGui::Text("Player Name:"); ImGui::SameLine(120);
        ImGui::SetNextItemWidth(160);
        // Fixed identity - the name box is always read-only (users cannot edit it).
        ImGui::InputText("##name", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_ReadOnly);
        ImGui::Text("Our IP:");      ImGui::SameLine(120);
        ImGui::TextUnformatted(st.localIp.empty() ? "(not connected)" : st.localIp.c_str());

        ImGui::Spacing();
        if (!busy) {
            client.setPlayerName(nameBuf);
            if (ImGui::Button("Scan LAN & Join")) client.connectScan();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            ImGui::InputText("##ip", ipBuf, sizeof(ipBuf));
            ImGui::SameLine();
            if (ImGui::Button("Join IP") && ipBuf[0]) client.connectIp(ipBuf);
        } else {
            if (ImGui::Button("Disconnect")) { op2::debug_log("user clicked Disconnect"); client.disconnect(); }
        }

        // ---- status line ----
        ImGui::Spacing(); ImGui::Separator();
        ImVec4 phc = st.disconnected ? ImVec4(1,0.4f,0.4f,1) : ImVec4(0.9f,0.9f,0.4f,1);
        ImGui::TextColored(phc, "Status: %s", op2::phase_name(st.phase));
        ImGui::SameLine(); ImGui::TextDisabled("%s", st.statusText.c_str());

        if (st.disconnected) {
            ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "DISCONNECTED: %s", st.disconnectReason.c_str());
        }

        // ---- lobby ----
        if (st.phase == op2::Phase::InLobby || st.phase == op2::Phase::Starting ||
            st.phase == op2::Phase::InGame || st.lobby.valid) {
            ImGui::Spacing(); ImGui::SeparatorText("Lobby");
            ImGui::Text("Host IP:  %s", st.hostIp.c_str());
            ImGui::Text("Game:     %s", st.gameName.c_str());
            ImGui::Text("Map DLL:  %s", st.lobby.mapDll.c_str());
            if (st.lobby.tappVersion) {
                uint32_t v = st.lobby.tappVersion;
                ImGui::Text("Host Ver: %u.%u.%u.%u", (v>>24)&0xff, (v>>16)&0xff, (v>>8)&0xff, v&0xff);
            }
            if (st.ourColor >= 0) {
                ImGui::Text("Our Color:"); ImGui::SameLine();
                ImGui::TextColored(op2_color(st.ourColor), "%s", op2::color_name(st.ourColor));
            }
            if (ImGui::BeginTable("players", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("#");
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Color");
                ImGui::TableSetupColumn("Race");
                ImGui::TableSetupColumn("Ready");
                ImGui::TableHeadersRow();
                for (int i = 0; i < 6; i++) {
                    const auto& s = st.lobby.slot[i];
                    if (!s.occupied) continue;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%d", i);
                    ImGui::TableSetColumnIndex(1);
                    bool me = (i == st.ourSlot);
                    ImGui::TextColored(me ? ImVec4(0.5f,1,0.5f,1) : ImVec4(1,1,1,1),
                                       "%s%s", s.name.c_str(), me ? " (you)" : "");
                    ImGui::TableSetColumnIndex(2); ImGui::TextColored(op2_color(s.color), "%s", op2::color_name(s.color));
                    ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(s.eden ? "Eden" : "Plymouth");
                    ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(s.ready ? "YES" : "no");
                }
                ImGui::EndTable();
            }
        }

        // ---- in-game stats ----
        if (st.phase == op2::Phase::InGame) {
            ImGui::Spacing(); ImGui::SeparatorText("In game");
            int m = (int)st.gameDurationSec / 60, s = (int)st.gameDurationSec % 60;
            ImGui::Text("Duration: %d:%02d", m, s);
            ImGui::SameLine(180); ImGui::Text("Game Tick: %d", st.gameTick);
            ImGui::SameLine(330); ImGui::Text("Game Mark: %d", st.gameMark);
            ImGui::Text("Cmd packets  sent: %ld", st.cmdSent);
            ImGui::SameLine(220); ImGui::Text("received: %ld", st.cmdRecv);
        }

        // ---- chat log + input ----
        ImGui::Spacing(); ImGui::SeparatorText("Chat");
        float inputH = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("chatlog", ImVec2(0, -inputH), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& c : st.chat) {
            // dim timestamp prefix, then the message
            ImGui::TextColored(ImVec4(0.45f,0.45f,0.45f,1), "%s", c.ts.c_str());
            ImGui::SameLine(0.0f, 6.0f);
            if (c.from.empty()) ImGui::TextColored(ImVec4(0.6f,0.6f,0.6f,1), "%s", c.text.c_str());
            else ImGui::TextColored(c.self ? ImVec4(0.5f,1,0.5f,1) : ImVec4(0.8f,0.9f,1,1),
                                    "%s: %s", c.from.c_str(), c.text.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        bool canChat = (st.phase == op2::Phase::InLobby || st.phase == op2::Phase::InGame);
        ImGui::BeginDisabled(!canChat);
        ImGui::SetNextItemWidth(-80);
        bool enter = ImGui::InputText("##chat", chatBuf, sizeof(chatBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        bool sendBtn = ImGui::Button("Send");
        if ((enter || sendBtn) && chatBuf[0]) { client.sendChat(chatBuf); chatBuf[0] = 0; }
        ImGui::EndDisabled();

        ImGui::End();

        ImGui::Render();
        int w, h; glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    op2::debug_log("window closed - shutting down");
    client.disconnect();
    op2::debug_log("=== OP2DummyPlayer exit ===");
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
