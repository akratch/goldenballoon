#ifndef MDKR64_UI_PHONE_PARTY_H
#define MDKR64_UI_PHONE_PARTY_H

class MdkrNativePartyHost;

/* True only when this build was compiled with a pairing origin. A build with
 * no origin cannot pair a phone, so it must present no Phone Party surface at
 * all -- an affordance that cannot work is exactly the teaser we ship without.
 * `serviceOrigin` is the compiled MDKR_PARTY_ORIGIN. */
bool PhoneParty_availableInBuild(const char *serviceOrigin);

/* Full launcher card and compact in-game entry point over the same host. */
void PhoneParty_drawLauncher(MdkrNativePartyHost &host,
                             const char *serviceOrigin);
void PhoneParty_drawOverlay(MdkrNativePartyHost &host,
                            const char *serviceOrigin);

#endif  // MDKR64_UI_PHONE_PARTY_H
