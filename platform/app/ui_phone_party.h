#ifndef MDKR64_UI_PHONE_PARTY_H
#define MDKR64_UI_PHONE_PARTY_H

class MdkrNativePartyHost;

/* Full launcher card and compact in-game entry point over the same host. */
void PhoneParty_drawLauncher(MdkrNativePartyHost &host,
                             const char *serviceOrigin);
void PhoneParty_drawOverlay(MdkrNativePartyHost &host,
                            const char *serviceOrigin);

#endif  // MDKR64_UI_PHONE_PARTY_H
