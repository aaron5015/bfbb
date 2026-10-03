#include "iTF2Bridge.h"

#include "iPadHost.h"
#include "xPad.h"

#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET BridgeSock;
static const BridgeSock kBadSock = INVALID_SOCKET;
static void CloseSock(BridgeSock s)
{
    closesocket(s);
}
static void SetNonBlocking(BridgeSock s)
{
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int BridgeSock;
static const BridgeSock kBadSock = -1;
static void CloseSock(BridgeSock s)
{
    close(s);
}
static void SetNonBlocking(BridgeSock s)
{
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
}
#endif

namespace
{
    // An intent older than this is dropped, so a closed TF2 window cannot leave
    // SpongeBob walking into a wall with the stick held.
    const double kStaleSeconds = 0.25;

    BridgeSock sSock = kBadSock;
    sockaddr_in sTf2Addr;
    bool sActive = false;
    bool sGameplay = false;
    double sGameplayAt = -1000.0;
    uint32_t sSendSeq = 0;

    bool sStickValid = false;
    float sStickX = 0.0f;
    float sStickY = 0.0f;

    bool sHaveIntent = false;
    BridgeIntentPacket sIntent;
    double sIntentAt = -1000.0;
    uint32_t sLastSeq = 0;

    double Now()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
} // namespace

void iTF2BridgeInit()
{
    if (getenv("BFBB_TF2BRIDGE") == NULL)
    {
        return;
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("bfbb: tf2bridge -- WSAStartup failed, bridge off\n");
        return;
    }
#endif

    sSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sSock == kBadSock)
    {
        printf("bfbb: tf2bridge -- socket() failed, bridge off\n");
        return;
    }

    sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port = htons(BRIDGE_PORT_BFBB);
    inet_pton(AF_INET, "127.0.0.1", &me.sin_addr);

    if (bind(sSock, (sockaddr*)&me, sizeof(me)) != 0)
    {
        printf("bfbb: tf2bridge -- cannot bind 127.0.0.1:%d (already running?), bridge off\n",
               BRIDGE_PORT_BFBB);
        CloseSock(sSock);
        sSock = kBadSock;
        return;
    }

    SetNonBlocking(sSock);

    memset(&sTf2Addr, 0, sizeof(sTf2Addr));
    sTf2Addr.sin_family = AF_INET;
    sTf2Addr.sin_port = htons(BRIDGE_PORT_TF2);
    inet_pton(AF_INET, "127.0.0.1", &sTf2Addr.sin_addr);

    sActive = true;
    printf("bfbb: tf2bridge up -- listening 127.0.0.1:%d, sending to :%d\n", BRIDGE_PORT_BFBB,
           BRIDGE_PORT_TF2);
}

void iTF2BridgeExit()
{
    if (sSock != kBadSock)
    {
        CloseSock(sSock);
        sSock = kBadSock;
    }
    sActive = false;
}

S32 iTF2BridgeActive()
{
    return sActive ? TRUE : FALSE;
}

void iTF2BridgePoll()
{
    if (!sActive)
    {
        return;
    }

    for (;;)
    {
        BridgeIntentPacket p;
        int n = (int)recv(sSock, (char*)&p, sizeof(p), 0);
        if (n != (int)sizeof(p))
        {
            // Nothing waiting (or a malformed datagram, which is dropped).
            if (n < 0)
            {
                break;
            }
            continue;
        }

        if (p.magic != BRIDGE_MAGIC_INTENT)
        {
            continue;
        }

        // UDP may reorder; keep only newer packets. Wraparound-safe compare.
        if (sHaveIntent && (int32_t)(p.seq - sLastSeq) <= 0)
        {
            continue;
        }

        sIntent = p;
        sLastSeq = p.seq;
        sHaveIntent = true;
        sIntentAt = Now();
    }
}

void iTF2BridgeSendState(const BridgeStatePacket* state)
{
    if (!sActive || state == NULL)
    {
        return;
    }

    BridgeStatePacket p = *state;
    p.magic = BRIDGE_MAGIC_STATE;
    p.seq = ++sSendSeq;

    sGameplay = (p.flags & BRIDGE_STATE_GAMEPLAY) != 0;
    sGameplayAt = Now();

    sendto(sSock, (const char*)&p, sizeof(p), 0, (const sockaddr*)&sTf2Addr, sizeof(sTf2Addr));
}

void iTF2BridgeSetStick(S32 valid, F32 x, F32 y)
{
    sStickValid = valid != 0;
    sStickX = x;
    sStickY = y;
}

const BridgeIntentPacket* iTF2BridgeGetIntent()
{
    if (!sActive || !sHaveIntent || Now() - sIntentAt > kStaleSeconds)
    {
        return NULL;
    }
    return &sIntent;
}

void iTF2BridgeApplyPad(iPadHostState* pad)
{
    if (!sActive || pad == NULL)
    {
        return;
    }

    iTF2BridgePoll();

    // Only in gameplay, and only while the game is still reporting it: in a
    // menu or a loading screen the TF2 buttons would press menu items (jump is
    // the confirm button), so the real pad keeps the menus.
    if (!sGameplay || Now() - sGameplayAt > kStaleSeconds)
    {
        return;
    }

    const BridgeIntentPacket* in = iTF2BridgeGetIntent();
    if (in == NULL)
    {
        return;
    }

    pad->connected = true;
    // Host convention: Y is up-positive. When the game layer has worked out a
    // camera-relative stick from the TF2 view direction, use it; otherwise the
    // raw axes (which walk relative to BFBB's camera, not TF2's view).
    pad->stick_x = sStickValid ? sStickX : in->side;
    pad->stick_y = sStickValid ? sStickY : in->forward;

    if (in->buttons & BRIDGE_IN_JUMP)
    {
        pad->buttons |= XPAD_BUTTON_X; // gamecube A: jump
    }
    if (in->buttons & BRIDGE_IN_ATTACK)
    {
        pad->buttons |= XPAD_BUTTON_TRIANGLE; // gamecube B: attack (placeholder)
    }
    if (in->buttons & BRIDGE_IN_ATTACK2)
    {
        pad->buttons |= XPAD_BUTTON_O; // gamecube X: bubble (placeholder)
    }
    if (in->buttons & BRIDGE_IN_RELOAD)
    {
        pad->buttons |= XPAD_BUTTON_SQUARE; // gamecube Y (placeholder, untested)
    }
}
