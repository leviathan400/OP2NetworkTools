# OP2Session - external Outpost 2 multiplayer client

A standalone C++/Winsock console tool that joins a live **Outpost 2**
multiplayer session **without running the game** - speaking OP2's own
discovery/join/lobby/GUR/in-game protocol from scratch. It discovers a hosted game,
completes the join handshake, runs the reliable transport, appears in the GAME SETTINGS
lobby as a named player, chats, readies up, survives the start handshake, and then keeps the
running game's lockstep simulation advancing by exchanging in-game command turns.
