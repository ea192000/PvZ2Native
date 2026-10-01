#pragma once
/* Auto-generated catalog definitions for PvZ2 achievements. */

namespace pvz2native {
namespace achievements {

enum AchievementLang {
    kLangDE = 0,
    kLangEN = 1,
    kLangES = 2,
    kLangFR = 3,
    kLangIT = 4,
    kLangPT = 5,
    kLangCount = 6
};

struct AchievementDef {
    const char *id;
    int icon_rc_id;
    const char *title[kLangCount];
    const char *desc[kLangCount];
};

struct AchievementServiceMapEntry {
    const char *service_id;
    const char *catalog_id;
};

constexpr int kAchievementCount = 75;

/* RC ids for UI chrome (must match windows_resources.rc / host_assets.cpp). */
constexpr int kRcAchievementPopup = 237;
constexpr int kRcAchievementMenu = 238;
constexpr int kRcAchievementItem = 239;
constexpr int kRcAchievementMedal = 240;
constexpr int kRcAchievementLocked = 241;
/* Icons: 242 .. 242+kAchievementCount-1 */

extern const AchievementDef kAchievements[kAchievementCount];
extern const AchievementServiceMapEntry kAchievementServiceMap[];
extern const int kAchievementServiceMapCount;
extern const char *kDebugLongestDescId;
extern const char *kDebugLongestTitleId;

}  // namespace achievements
}  // namespace pvz2native
