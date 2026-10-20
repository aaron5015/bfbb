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
    uint32_t sSessionId = 0;
    uint32_t sTf2SessionId = 0;
    bool sHaveTf2SessionId = false;

    // Fire events must not be coalesced away with the "newest intent wins" rule.
    const int kEventCap = 32;
    struct EventSlot
    {
        BridgeIntentPacket pkt;
        double at;
    };
    EventSlot sEvents[kEventCap];
    int sEventHead = 0;
    int sEventCount = 0;

    const int kNpcHitCap = 64;
    BridgeNpcHitPacket sNpcHits[kNpcHitCap];
    int sNpcHitHead = 0;
    int sNpcHitCount = 0;
    uint32_t sLastNpcHitSeq = 0;
    bool sHaveNpcHitSeq = false;

    bool IsEvent(const BridgeIntentPacket& p)
    {
        return p.hitscanCount > 0 || (p.weaponflags & BRIDGE_WEAPON_FIRED) != 0;
    }

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
    sNpcHitHead = 0;
    sNpcHitCount = 0;
    sLastNpcHitSeq = 0;
    sHaveNpcHitSeq = false;
    sHaveTf2SessionId = false;
    sTf2SessionId = 0;
    const uint64_t sessionClock = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    sSessionId = (uint32_t)(sessionClock ^ (sessionClock >> 32) ^ (uintptr_t)&sIntent);
    if (sSessionId == 0)
        sSessionId = 1;
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
        uint8_t buf[sizeof(BridgeIntentPacket)];
        int n = (int)recv(sSock, (char*)buf, sizeof(buf), 0);
        if (n < 0)
            break;

        if (n == (int)sizeof(BridgeNpcHitPacket))
        {
            BridgeNpcHitPacket hit;
            memcpy(&hit, buf, sizeof(hit));
            if (hit.magic == BRIDGE_MAGIC_NPC_HIT)
            {
                if (!sHaveTf2SessionId || hit.sessionId != sTf2SessionId)
                {
                    sTf2SessionId = hit.sessionId;
                    sHaveTf2SessionId = true;
                    sHaveIntent = false;
                    sLastSeq = 0;
                    sEventHead = 0;
                    sEventCount = 0;
                    sNpcHitHead = 0;
                    sNpcHitCount = 0;
                    sHaveNpcHitSeq = false;
                }
                if (sHaveNpcHitSeq && (int32_t)(hit.seq - sLastNpcHitSeq) <= 0)
                    continue;
                sLastNpcHitSeq = hit.seq;
                sHaveNpcHitSeq = true;
                if (sNpcHitCount == kNpcHitCap)
                {
                    sNpcHitHead = (sNpcHitHead + 1) % kNpcHitCap;
                    --sNpcHitCount;
                }
                sNpcHits[(sNpcHitHead + sNpcHitCount) % kNpcHitCap] = hit;
                ++sNpcHitCount;
            }
            continue;
        }

        if (n != (int)sizeof(BridgeIntentPacket))
        {
            continue;
        }

        BridgeIntentPacket p;
        memcpy(&p, buf, sizeof(p));

        if (p.magic != BRIDGE_MAGIC_INTENT)
        {
            continue;
        }

        if (!sHaveTf2SessionId || p.sessionId != sTf2SessionId)
        {
            sTf2SessionId = p.sessionId;
            sHaveTf2SessionId = true;
            sHaveIntent = false;
            sLastSeq = 0;
            sEventHead = 0;
            sEventCount = 0;
            sNpcHitHead = 0;
            sNpcHitCount = 0;
            sHaveNpcHitSeq = false;
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

        if (IsEvent(p))
        {
            if (sEventCount == kEventCap)
            {
                sEventHead = (sEventHead + 1) % kEventCap;
                --sEventCount;
            }
            EventSlot& slot = sEvents[(sEventHead + sEventCount) % kEventCap];
            slot.pkt = p;
            slot.at = sIntentAt;
            ++sEventCount;
        }
    }
}

void iTF2BridgeSendRocketImpact(uint32_t entIndex, float x, float y, float z)
{
    if (!sActive)
        return;

    BridgeRocketImpactPacket p;
    memset(&p, 0, sizeof(p));
    p.magic = BRIDGE_MAGIC_ROCKET_IMPACT;
    p.entIndex = entIndex;
    p.x = x;
    p.y = y;
    p.z = z;

    sendto(sSock, (const char*)&p, sizeof(p), 0,
           (const sockaddr*)&sTf2Addr, sizeof(sTf2Addr));
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
    p.sessionId = sSessionId;

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

bool iTF2BridgePopEvent(BridgeIntentPacket* out)
{
    while (sEventCount > 0)
    {
        const EventSlot& slot = sEvents[sEventHead];
        const bool fresh = Now() - slot.at <= kStaleSeconds;
        if (fresh && out != NULL)
        {
            *out = slot.pkt;
        }
        sEventHead = (sEventHead + 1) % kEventCap;
        --sEventCount;
        if (fresh)
        {
            return out != NULL;
        }
    }
    return false;
}

bool iTF2BridgePopNpcHit(BridgeNpcHitPacket* out)
{
    if (sNpcHitCount <= 0 || out == NULL)
        return false;
    *out = sNpcHits[sNpcHitHead];
    sNpcHitHead = (sNpcHitHead + 1) % kNpcHitCap;
    --sNpcHitCount;
    return true;
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

    // TF2 is running the movement and the gunplay: BFBB follows its position
    // (zTF2Bridge_AfterPlayerUpdate), and no BFBB move is pressed on its behalf.
    if (in->flags & BRIDGE_INTENT_OWNS_MOVE)
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
