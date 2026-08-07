/* com.popcap.SexyAppFramework.GooglePlay.{GooglePlayConnect, GooglePlayAchievements,
 * GooglePlayLeaderboard} and com.popcap.SexyAppFramework.cloud.Cloud
 *
 * The online services. None of them exist in this port, and none of them need
 * to: the engine treats "not signed in" as an ordinary state a real device is
 * in most of the time, so it degrades to local-only play without complaint.
 *
 * Every name below was taken from the decompiled classes.dex and cross-checked
 * against the string table of libPVZ2.so itself, because the previous version
 * of this file was written from memory of what the API "should" look like and
 * every single entry was dead: it registered `com/popcap/SexyAppFramework/Cloud`
 * and `.../GooglePlayConnect`, while the engine names
 * `com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect` -- these classes
 * live in subpackages -- and methods like Play_UnlockAchievement and Cloud_Sync
 * do not exist in any build. A hook keyed on a name nothing ever sends is
 * invisible: it does not warn, it just never runs. Verify a class path against
 * `strings libPVZ2.so` before adding one here.
 *
 * That correction changes no behaviour, and saying so is the point of this
 * file: these methods are void or boolean, so the unhooked default (nothing /
 * false) was already the answer they now give explicitly. They are written down
 * so the silence is a decision, and so a later change to the unhooked default
 * cannot quietly tell the engine it is online and leave it waiting on a
 * callback that will never arrive.
 *
 * GooglePlay/* is new in 4.5.2; 1.6 names none of these classes.
 */

#include <pvz2native/dex/dex.h>
#include <pvz2native/config_screen.h>
#include <SDL.h>

namespace pvz2native {
namespace dex {
namespace {

constexpr const char *kConnect = "com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect";
constexpr const char *kAchievements = "com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements";
constexpr const char *kLeaderboard = "com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard";

/* Cloud is reached differently: the engine never passes its class path to
 * FindClass, so it must be holding an object handed to it from Java and calling
 * GetObjectClass on it. Our GetObjectClass answers "java/lang/Object" for an
 * object it cannot identify, which is where these land -- the method name alone
 * is unambiguous enough to dispatch on. If the census ever shows them arriving
 * under a different class, move them; do not guess. */
constexpr const char *kCloud = "java/lang/Object";

/* "Are we connected", "accept and do nothing", and the two Cloud getters that
 * return a String (Java would answer null when nothing is stored, which
 * aborts the guest) all use the shared stub bodies from dex.h --
 * hook_not_connected, hook_ignored, hook_empty_string. */

/* Play_Connect is called when the player presses the Google Play button on the
 * main menu. Google Play Games is not supported in this port; the button press
 * opens the in-game configuration screen instead. */
void open_config_screen(DexCall &d) {
    (void)d;
    config_screen_request_open();
}

}  // namespace

void register_google_play(HookTable &t) {
    t.add(kConnect, "Play_IsConnected", hook_not_connected);
    t.add(kConnect, "Play_Connect", open_config_screen);
    t.add(kConnect, "Play_Connect_Silent", hook_ignored);
    t.add(kConnect, "Play_Disconnect", hook_ignored);

    t.add(kAchievements, "Play_QueueAchievement_Percentage", hook_ignored);
    t.add(kAchievements, "Play_ResetAchievements", hook_ignored);
    t.add(kAchievements, "Play_ShowAchievementView", hook_ignored);

    t.add(kLeaderboard, "Play_SubmitScoreToLeaderboard", hook_ignored);
    t.add(kLeaderboard, "Play_ShowLeaderboardView", hook_ignored);
    t.add(kLeaderboard, "Play_ShowLeaderboardViewAll", hook_ignored);

    t.add(kCloud, "Cloud_Connect", hook_ignored);
    t.add(kCloud, "Cloud_attemptSilentSync", hook_ignored);
    t.add(kCloud, "Cloud_SetPcpId", hook_ignored);
    t.add(kCloud, "Cloud_SetAge", hook_ignored);
    t.add(kCloud, "Cloud_GetPcpId", hook_empty_string);
    t.add(kCloud, "Cloud_GetAge", hook_empty_string);
}

}  // namespace dex
}  // namespace pvz2native
