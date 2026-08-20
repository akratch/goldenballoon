#ifndef MDKR64_UI_PHONE_PARTY_H
#define MDKR64_UI_PHONE_PARTY_H

class MdkrNativePartyHost;

/* True only when this build was compiled with a pairing origin. A build with
 * no origin cannot pair a phone over the internet, so it presents no cloud
 * Phone Party surface -- an affordance that cannot work is exactly the teaser
 * we ship without. `serviceOrigin` is the compiled MDKR_PARTY_ORIGIN. Local
 * play (no internet) does not depend on this: see PhonePartyLanControls. */
bool PhoneParty_availableInBuild(const char *serviceOrigin);

/*
 * Local-play (no internet) control surface, owned by the launcher and passed
 * into the party card each frame. The launcher fills `available`/`active`/
 * `unavailableReason`/`note` before the card draws, and reads `request` back
 * after the frame: the card draws from the live host, and switching to or from
 * the LAN transport rebuilds that host, so the switch must happen at a safe
 * point (Launcher::draw applies it after ImGui has finished the frame, exactly
 * like a deferred tab change).
 */
struct PhonePartyLanControls {
    /* A reachable LAN address exists, so local play can be offered. */
    bool available = false;
    /* The LAN transport is the live one right now (a local room, or an attempt
     * at one). While true the card shows the local-play surface and never the
     * cloud card; while false the cloud card owns the surface and the local-play
     * option appears beneath it. This is the on-screen half of the runtime
     * mutual exclusion the transport seam enforces. */
    bool active = false;
    /* When `available` is false, a short player-facing reason (may be null). */
    const char *unavailableReason = nullptr;
    /* Last local-play start failure, shown in the card (empty when none). */
    const char *note = nullptr;
    /* Set by the card; applied and cleared by the launcher after the frame. */
    enum class Request { None, Start, Stop } request = Request::None;
};

/* Full launcher card and compact in-game entry point over the same host. */
void PhoneParty_drawLauncher(MdkrNativePartyHost &host,
                             const char *serviceOrigin,
                             PhonePartyLanControls &lan);
void PhoneParty_drawOverlay(MdkrNativePartyHost &host,
                            const char *serviceOrigin);

#endif  // MDKR64_UI_PHONE_PARTY_H
