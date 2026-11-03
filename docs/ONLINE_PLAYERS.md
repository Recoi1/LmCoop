# Playing LmCoop over the internet

You and your friend each need LmCoop (the same version) and your own copy of the USA disc. Nothing else
to install, no VPN, no port forwarding in most cases.

## One-time setup

1. Someone has to run a matchmaking server (see [ONLINE_SERVER.md](ONLINE_SERVER.md)); there is no
   public one. Ask whoever runs your group's server for its address (like `wss://lmcoop.example.com`).
2. On the start menu choose **Multiplayer > Settings**, type that address into *Matchmaking server*, and
   set *Your name*. Back out.

## Host

1. **Multiplayer > Host Online.** Pick a lobby name. *Public* lists it in Browse Lobbies; leave it off
   for a private lobby. A password is optional.
2. **Create lobby.** The game starts; choose your save as usual (you play Luigi).
3. Press **F1 > Controls > Online** to see the **invite code**. Press **Copy** and send it to your friend.

## Join

* **With a code:** Multiplayer > **Join by Code**, type the code (and the password if there is one), Join.
* **From the list:** Multiplayer > **Browse Lobbies**, press Refresh, pick a lobby marked *Open*, Join.

You wait on the loading screen until the host is in the game, then you play Mario in their game. Your own
saves are not touched.

## Leaving

F1 > Controls > Online > **Leave lobby** returns to the start menu.

## If it will not connect

| Message | Meaning |
|---|---|
| no lobby with that code (or it is private) | Wrong code, the host left, or you used a lobby id of a private lobby. |
| the host runs a different LmCoop version | Both of you need the same LmCoop version. |
| the lobby is full | Two players only. |
| cannot reach the matchmaking server | Wrong address, server down, or a firewall blocks it. |
| could not connect directly (NAT/firewall) | Your networks cannot reach each other directly. Ask the server owner for a TURN relay (Settings > STUN / TURN servers), or use Direct Connect over a VPN such as Radmin. |

## Privacy

The lobby list does not show anyone's address. But in a direct connection **your friend's PC learns your
public IP address, and yours learns theirs**, and the STUN servers (Google and Cloudflare by default) see it
while the connection is set up. Only play with people you trust, or use a TURN relay, which hides the
address from the other player.

## Direct Connect (LAN / Radmin / VPN)

Unchanged: Multiplayer > Direct Connect, host enters this PC's address, the joiner enters the host's address
and the same password.
