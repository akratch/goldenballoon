/* ROM-, renderer-, provider- and storage-free browser projection ABI. */
#ifndef MDKR_ONLINE_LOBBY_BROWSER_WASM_H
#define MDKR_ONLINE_LOBBY_BROWSER_WASM_H

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_ONLINE_BROWSER_ABI_VERSION 4u

unsigned mdkr_online_browser_version(void);
unsigned mdkr_online_browser_count(void);
int mdkr_online_browser_select(unsigned index);
const char *mdkr_online_browser_slug(void);
const char *mdkr_online_browser_title(void);
const char *mdkr_online_browser_explanation(void);
const char *mdkr_online_browser_status(void);
const char *mdkr_online_browser_verification_phrase(void);
const char *mdkr_online_browser_timeout_title(void);
const char *mdkr_online_browser_timeout_explanation(void);
unsigned mdkr_online_browser_kind(void);
unsigned mdkr_online_browser_failure(void);
unsigned mdkr_online_browser_announcement(void);
unsigned mdkr_online_browser_member_count(void);
unsigned mdkr_online_browser_seat_count(void);
unsigned mdkr_online_browser_ready_count(void);
unsigned mdkr_online_browser_requires_admission(void);
unsigned mdkr_online_browser_local_play_available(void);
unsigned mdkr_online_browser_timeout_visible(void);
unsigned mdkr_online_browser_control_action(unsigned slot);
const char *mdkr_online_browser_control_label(unsigned slot);
unsigned mdkr_online_browser_control_enabled(unsigned slot);
unsigned mdkr_online_browser_dispatch(unsigned action, unsigned supplied_value);
unsigned mdkr_online_browser_pending(void);
int mdkr_online_browser_complete(void);

int mdkr_online_browser_live_project(
    unsigned room_phase, unsigned lobby_phase, unsigned revision,
    unsigned match_epoch, unsigned leader_index, unsigned local_index,
    unsigned member_count, unsigned seat_count,
    unsigned member_seat_counts, unsigned ready_mask,
    unsigned connected_mask, unsigned loaded_mask, unsigned seat_owners,
    unsigned character_mask, unsigned vehicle_mask, unsigned vote_mask,
    unsigned selected_track, unsigned selected_vehicle_mask,
    unsigned journey, unsigned failure, unsigned invite_state);

#ifdef __cplusplus
}
#endif

#endif
