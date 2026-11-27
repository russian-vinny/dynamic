#include <Geode/Geode.hpp>
#include <Geode/utils/general.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/cocos.hpp>
#include <Geode/utils/VersionInfo.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/utils/Keyboard.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/SceneEvent.hpp>
#include <Geode/cocos/label_nodes/CCLabelTTF.h>
#include <Geode/cocos/touch_dispatcher/CCTouchDispatcher.h>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/RingObject.hpp>
#include <Geode/cocos/layers_scenes_transitions_nodes/CCTransition.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#ifdef GEODE_IS_MACOS
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

using namespace geode::prelude;

namespace {

    constexpr int MOD_HOOK_PRIORITY = -1000000;
    constexpr float TICK_DURATION = 0.25f;
    constexpr char const* KEYBIND_STAGE = "keybind-stage";
    constexpr char const* KEYBIND_TERMINAL = "keybind-terminal";
    constexpr char const* KEYBIND_INTRO_DONE = "keybind-intro-done";

    constexpr size_t RECENT_LOG_CAPACITY = 64;
    std::mutex g_recentLogMutex;
    std::deque<std::string> g_recentLog;

#ifdef _WIN32
    HANDLE g_terminalLogHandle = INVALID_HANDLE_VALUE;
    std::string g_windowsTerminalLogFileName = "dynamic-terminal.log";
#endif

    constexpr uint64_t typeBit(GameObjectType objectType) {
        return 1ull << static_cast<int>(objectType);
    }

    constexpr uint64_t COLLIDING_OBJECT_TYPES = typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::Slope) |
                                         typeBit(GameObjectType::CollisionObject) | typeBit(GameObjectType::AnimatedHazard);

    constexpr uint64_t HAZARD_OBJECT_TYPES = typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::AnimatedHazard);

    constexpr uint64_t INTERACTABLE_OBJECT_TYPES =
        typeBit(GameObjectType::YellowJumpPad) | typeBit(GameObjectType::PinkJumpPad) | typeBit(GameObjectType::RedJumpPad) |
        typeBit(GameObjectType::GravityPad) | typeBit(GameObjectType::SpiderPad) | typeBit(GameObjectType::YellowJumpRing) |
        typeBit(GameObjectType::PinkJumpRing) | typeBit(GameObjectType::RedJumpRing) | typeBit(GameObjectType::GravityRing) |
        typeBit(GameObjectType::GreenRing) | typeBit(GameObjectType::DropRing) | typeBit(GameObjectType::CustomRing) |
        typeBit(GameObjectType::DashRing) | typeBit(GameObjectType::GravityDashRing) | typeBit(GameObjectType::SpiderOrb) |
        typeBit(GameObjectType::InverseGravityPortal) | typeBit(GameObjectType::NormalGravityPortal) |
        typeBit(GameObjectType::GravityTogglePortal);

    constexpr uint64_t MODE_PORTAL_OBJECT_TYPES =
        typeBit(GameObjectType::ShipPortal) | typeBit(GameObjectType::CubePortal) | typeBit(GameObjectType::BallPortal) |
        typeBit(GameObjectType::UfoPortal) | typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
        typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal) | typeBit(GameObjectType::DualPortal) |
        typeBit(GameObjectType::SoloPortal) | typeBit(GameObjectType::TeleportPortal) | typeBit(GameObjectType::TeleportOrb);

    constexpr uint64_t SIZE_PORTAL_OBJECT_TYPES = typeBit(GameObjectType::RegularSizePortal) | typeBit(GameObjectType::MiniSizePortal);

    // the hold profile is a real, persisted memory now: 32 samples instead of 8 means the
    // learned press length stops twitching between two adjacent values while still decaying
    // old habits out of the window.
    constexpr int HOLD_SAMPLE_COUNT = 32;
    constexpr int SPIDER_LAND_TICKS = 12;
    constexpr int RAPID_STREAK_COUNT = 3;
    // rapid-input detection now only preserves the old one-tick-apart compaction for ufo/ball/
    // swing. the spam guard that used it to bypass the buffer is gone (ADR-015), so a fast
    // click is judged by the same simulation as every other click.
    constexpr int RAPID_INPUT_GAP_TICKS = 16;
    // a measured wave rhythm this short reads as high-cps straight flying. only then is a wave
    // press judged as press -> short hold -> release -> short gap -> press instead of the old
    // "hold forever" guess; deliberate wave holds keep the original behaviour (ADR-016).
    constexpr int WAVE_UP_FALLBACK_TICKS = 6;
    constexpr int WAVE_RAPID_MAX_TICKS = 24;
    constexpr int WAVE_TURN_MARGIN_TICKS[] = { 4, 8, 12 };
    constexpr int WAVE_MIN_TURN_TICKS = 6;
    constexpr int WAVE_ESCAPE_TICKS = 16;
    // how far ahead the orb helper sweeps for a ring by trying later presses. the probe alone only
    // sees rings the no-jump path touches, which misses a cube jump that lands on an orb. this is
    // the "i clicked a bit early" distance, not a bot: 48 ticks is 12 frames, and a press moved
    // further than that is a different click, which reads as the buffer being inaccurate.
    constexpr int ORB_REACH_TICKS = 48;
    // a press only gets rescued when it is a dud: it dies on/right after the press tick instead of
    // carrying the player through the orb. a press that keeps the player alive past this does hit
    // the ring, and re-timing it (even by half a frame) moves a click that already connects - that
    // is what "the smart orb buffering is insanely bad" felt like. 12 ticks = 3 frames.
    constexpr int ORB_RESCUE_MAX_RESULT_TICKS = 12;
    // a shift has to be a real save to be worth moving the player's click: either the press now
    // survives the whole horizon, or it buys this much extra life while staying inside the normal
    // decision window. a "+2t for 3 more ticks before dying anyway" pick is what made a connected
    // click feel wrong, so those are refused and the raw press goes through untouched.
    constexpr int ORB_CONFIDENT_GAIN_TICKS = 24;
    // a press already inside a ring can connect on the first usable tick and still launch too
    // early. a same-ring run that buys this much extra life is a real save even when it does not
    // reach the full horizon, so it is allowed to move an in-range click (8 raw ticks = 2 frames).
    constexpr int ORB_IN_RANGE_GAIN_TICKS = 8;
    // a let-go only has to hold down to the next press. the full wave lookahead is mostly an
    // up-slope, so asking a release to stay released for it says every release is fatal and
    // none of them ever got a shift. a release is judged on the short descend window instead.
    constexpr int WAVE_RELEASE_HORIZON_TICKS = 24;
    // the down phase a release is judged with when no release -> press gap has been measured
    // yet. 6 raw ticks = 1.5 frames, which is a normal wave rhythm and far closer to play than
    // "fall forever". the measured gap takes over after the first real release -> press pair.
    constexpr int WAVE_DOWN_FALLBACK_TICKS = 6;
    // a press is judged over the window the user actually selected (the tick slider is the
    // contract: "28 ticks" means "keep me alive for the next 28 ticks"), but never shorter
    // than 12 or longer than 32. the long end is a ceiling so a huge custom window cannot
    // demand one unbroken hold through the whole corridor, which always hits a wall and made
    // every candidate "fail" before the requested window was even reached.
    constexpr int WAVE_PRESS_HORIZON_MIN_TICKS = 12;
    constexpr int WAVE_PRESS_HORIZON_MAX_TICKS = 32;
    constexpr int ROBOT_HOLD_TICKS[] = { 6, 14, 26, 45 };
    constexpr int SPIDER_RETAP_TICKS[] = { 4, 8, 12 };
    constexpr int SPIDER_MIN_RETAP_TICKS = 4;
    constexpr int SAME_CRASH_TRY_COUNT = 4;
    constexpr int SHIP_RECOVERY_TICKS[] = { 8, 16, 24 };
    // a ship input is a two-sided gesture: a press is held for a few ticks, then released, then
    // pressed again. judging either side as "hold/release forever" made almost every ship input
    // look fatal, so the helper shifted it and the shift is what the player felt as dying more.
    // the two fallbacks below are only used until the player's own hold and release->press gap
    // have been measured; they mirror the defaults from the deck (hold ~3t, gap ~6t).
    constexpr int SHIP_HOLD_FALLBACK_TICKS = 8;
    constexpr int SHIP_GAP_FALLBACK_TICKS = 16;
    // smart straight-fly. a ship holds a line by alternating short taps, so "i need to straight
    // fly" is visible from the player's own rhythm: short holds, short gaps. when that rhythm is
    // present the helper looks at where the ghost predicts the ship will be over the window and
    // delays only the edge that flattens the line. it never adds or removes a press, and a tap
    // that is already holding its line is left untouched.
    constexpr int SHIP_STRAIGHT_MIN_SAMPLES = 2;         // measured taps before the helper acts
    constexpr int SHIP_STRAIGHT_LOG_GAP_TICKS = 120;     // evidence without flooding the log
    // how far into the input the straight helper samples the line (ADR-014, tightened in ADR-017).
    // a flat line is visible well before the 48t evasion window, and a 24t judge still left most
    // nudges below the drift/gain floor. 16t makes the assist legible while every candidate still
    // has to survive the raw input's own floor and flatten the measured line.
    constexpr int SHIP_STRAIGHT_JUDGE_TICKS = 16;
    // the `straight` tab dials. three independent 0..100 numbers, because one preset ladder is not
    // enough control: strength is how far an edge may move, sensitivity is how small a drift is
    // worth helping, rhythm is how wide a hold/gap still reads as straight-flying. the derived
    // values below are the only place those three turn into ticks/units.
    constexpr int SHIP_STRAIGHT_STRENGTH_MIN = 0;
    constexpr int SHIP_STRAIGHT_STRENGTH_MAX = 100;
    constexpr int SHIP_STRAIGHT_STRENGTH_DEFAULT = 50;
    constexpr int SHIP_STRAIGHT_SENSITIVITY_DEFAULT = 55;
    constexpr int SHIP_STRAIGHT_RHYTHM_DEFAULT = 45;
    constexpr int SHIP_STRAIGHT_NUDGE_MAX_TICKS = 8;       // strength 100 -> this many ticks (ADR-017)
    constexpr float SHIP_STRAIGHT_DRIFT_MAX_UNITS = 34.f;  // sensitivity 0: only big drifts
    constexpr float SHIP_STRAIGHT_DRIFT_MIN_UNITS = 5.f;   // sensitivity 100: helps almost always
    constexpr int SHIP_STRAIGHT_HOLD_MIN_TICKS = 20;       // rhythm 0
    constexpr int SHIP_STRAIGHT_HOLD_MAX_TICKS = 40;       // rhythm 100
    constexpr int SHIP_STRAIGHT_GAP_MIN_TICKS = 32;        // rhythm 0
    constexpr int SHIP_STRAIGHT_GAP_MAX_TICKS = 76;        // rhythm 100
    constexpr float SHIP_STRAIGHT_GAIN_MAX_UNITS = 3.5f;   // strength 0: must flatten clearly
    constexpr float SHIP_STRAIGHT_GAIN_MIN_UNITS = 1.5f;   // strength 100: small gain is enough
    // defined with the straight-fly assist further down; declared here because the modes-tab
    // straight tile, the `straight` tab, the status line and the config export all read them.
    int shipStraightStrength();
    int shipStraightSensitivity();
    int shipStraightRhythm();
    int shipStraightNudgeTicks();
    float shipStraightDriftUnits();
    float shipStraightGainUnits();
    int shipStraightHoldMaxTicks();
    int shipStraightGapMaxTicks();
    constexpr int UFO_REFLAP_TICKS[] = { 6, 12, 18 };

    bool hasType(uint64_t typeMask, GameObject* gameObject) {
        auto objectType = static_cast<unsigned>(gameObject->m_objectType);
        return objectType < 64 && ((typeMask >> objectType) & 1);
    }

    float speedPortalValue(int objectId) {
        switch (objectId) {
            case 200: return 0.7f;
            case 201: return 0.9f;
            case 202: return 1.1f;
            case 203: return 1.3f;
            case 1334: return 1.6f;
            default: return 0.f;
        }
    }

    struct Stage {
        bool isEnabled;
        int ticksAllModes;
        int ticksWaveMode;
        bool isCustom;
    };

    constexpr Stage STAGE_TABLE[] = {
        { true, 2, 2, false },
        { true, 4, 4, false },
        { true, 8, 8, false },
        { true, 16, 8, false },
        { true, 24, 12, false },
        { true, 32, 16, false },
        { true, 40, 20, false },
        { false, 0, 0, false },
    };
    constexpr int STAGE_TABLE_SIZE = sizeof(STAGE_TABLE) / sizeof(STAGE_TABLE[0]);

    constexpr int STAGE_MAX_STAGES = 24;
    constexpr int CUSTOM_STAGE_LIMIT = 4;
    constexpr int CUSTOM_TICKS_MIN = 1;
    constexpr int CUSTOM_TICKS_MAX = 240;
    constexpr int CUSTOM_TICKS_SAFE = 120;
    constexpr int MODE_TICK_COUNT = 8;
    char const* const MODE_TICK_NAMES[MODE_TICK_COUNT] = {
        "cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing"
    };

    struct CustomStage {
        int ticksAll;
        int ticksWave;
    };

    std::vector<CustomStage> g_customStages;
    bool g_customStagesLoaded = false;

    int clampCustomTicks(int value) {
        return std::clamp(value, CUSTOM_TICKS_MIN, CUSTOM_TICKS_MAX);
    }

    void showHighTicksPopup() {
        std::string message = fmt::format(
            "delays over {0} ticks may lag or work worse than {0}.\n"
            "your game might stutter or the stage might act weird.",
            CUSTOM_TICKS_SAFE
        );
        FLAlertLayer::create("warning", message.c_str(), "ok i understand")->show();
    }

    void loadCustomStages() {
        if (g_customStagesLoaded) {
            return;
        }
        g_customStagesLoaded = true;
        g_customStages.clear();
        std::string raw = Mod::get()->getSavedValue<std::string>("custom-stages", "");
        size_t start = 0;
        while (start <= raw.size()) {
            size_t end = raw.find(';', start);
            if (end == std::string::npos) {
                end = raw.size();
            }
            std::string piece = raw.substr(start, end - start);
            if (!piece.empty() && static_cast<int>(g_customStages.size()) < CUSTOM_STAGE_LIMIT) {
                int ticksAll = 0;
                int ticksWave = 0;
                if (std::sscanf(piece.c_str(), "%d:%d", &ticksAll, &ticksWave) == 2) {
                    g_customStages.push_back({ clampCustomTicks(ticksAll), clampCustomTicks(ticksWave) });
                }
                else {
                    g_customStages.push_back({ 8, 8 });
                }
            }
            start = end + 1;
        }
    }

    void saveCustomStages() {
        std::string raw;
        for (size_t index = 0; index < g_customStages.size(); ++index) {
            if (index > 0) { raw += ';'; }
            raw += fmt::format("{}:{}", g_customStages[index].ticksAll, g_customStages[index].ticksWave);
        }
        Mod::get()->setSavedValue<std::string>("custom-stages", raw);
        (void)Mod::get()->saveData();
    }

    CustomStage* customStageFor(int stageIndex) {
        loadCustomStages();
        int custom = stageIndex - STAGE_TABLE_SIZE;
        if (custom >= 0 && custom < static_cast<int>(g_customStages.size())) {
            return &g_customStages[custom];
        }
        return nullptr;
    }

    int stageTotalCount() {
        loadCustomStages();
        return STAGE_TABLE_SIZE + static_cast<int>(g_customStages.size());
    }

    bool stageEnabledAt(int index) {
        if (index >= 0 && index < STAGE_TABLE_SIZE) {
            return STAGE_TABLE[index].isEnabled;
        }
        loadCustomStages();
        return index >= STAGE_TABLE_SIZE && index < stageTotalCount();
    }

    int g_modeTicks[MODE_TICK_COUNT] = {};
    bool g_modeTicksLoaded = false;

    int clampModeTicks(int value) {
        return std::clamp(value, 0, CUSTOM_TICKS_MAX);
    }

    void loadModeTicks() {
        if (g_modeTicksLoaded) {
            return;
        }
        g_modeTicksLoaded = true;
        for (int index = 0; index < MODE_TICK_COUNT; ++index) {
            g_modeTicks[index] = 0;
        }
        std::string raw = Mod::get()->getSavedValue<std::string>("mode-ticks", "");
        size_t start = 0;
        int slot = 0;
        while (start <= raw.size() && slot < MODE_TICK_COUNT) {
            size_t end = raw.find(',', start);
            if (end == std::string::npos) {
                end = raw.size();
            }
            if (end > start) {
                int value = 0;
                bool found = false;
                for (size_t index = start; index < end; ++index) {
                    char character = raw[index];
                    if (character >= '0' && character <= '9') {
                        value = value * 10 + (character - '0');
                        found = true;
                    }
                }
                if (found) {
                    g_modeTicks[slot] = clampModeTicks(value);
                }
                ++slot;
            }
            start = end + 1;
        }
    }

    void saveModeTicks() {
        std::string raw;
        for (int index = 0; index < MODE_TICK_COUNT; ++index) {
            if (index > 0) {
                raw += ',';
            }
            raw += std::to_string(g_modeTicks[index]);
        }
        Mod::get()->setSavedValue<std::string>("mode-ticks", raw);
        (void)Mod::get()->saveData();
    }

    int modeTicksAt(int mode) {
        loadModeTicks();
        if (mode < 0 || mode >= MODE_TICK_COUNT) {
            return 0;
        }
        return g_modeTicks[mode];
    }

    void setModeTicksAt(int mode, int value) {
        loadModeTicks();
        if (mode < 0 || mode >= MODE_TICK_COUNT) {
            return;
        }
        g_modeTicks[mode] = clampModeTicks(value);
        saveModeTicks();
    }

    int clearModeTicks() {
        loadModeTicks();
        int cleared = 0;
        for (int index = 0; index < MODE_TICK_COUNT; ++index) {
            if (g_modeTicks[index] > 0) {
                ++cleared;
            }
            g_modeTicks[index] = 0;
        }
        saveModeTicks();
        return cleared;
    }

    struct QueuedInput {
        int ticksRemaining;
        bool isDown;
    };

    struct ObjSnapshot {
        GameObject* gameObject;
        bool isObjectActivated;
        EffectGameObject* effectObject;
        bool isActivatedByEffect;
        bool byPlayer1;
        bool byPlayer2;
    };

    struct ProbeOrb {
        RingObject* ringObject = nullptr;
        PlayerObject* playerObject = nullptr;
        float touchDistanceSquared = 0.f;
        float closeDistanceSquared = 0.f;
        int touchSampleTick = -1;
        int closeSampleTick = -1;
    };

    struct HoldStats {
        int holdSamples[HOLD_SAMPLE_COUNT] = {};
        int sampleCount = 0;
        int nextSampleIndex = 0;

        void add(int tickCount) {
            holdSamples[nextSampleIndex] = std::clamp(tickCount, 1, 240);
            nextSampleIndex = (nextSampleIndex + 1) % HOLD_SAMPLE_COUNT;
            sampleCount = std::min(sampleCount + 1, HOLD_SAMPLE_COUNT);
        }

        int pick(int fallbackValue, int percentile) const {
            if (sampleCount == 0) {
                return fallbackValue;
            }
            int scratchSamples[HOLD_SAMPLE_COUNT];
            std::copy(holdSamples, holdSamples + sampleCount, scratchSamples);
            int sampleIndex = std::min(sampleCount - 1, sampleCount * percentile / 100);
            std::nth_element(scratchSamples, scratchSamples + sampleIndex, scratchSamples + sampleCount);
            return scratchSamples[sampleIndex];
        }

        // median of the newest `count` samples. the straight-fly detector needs the raw rhythm,
        // because pick() is clamped to 24t and cannot tell a fast alternation from a long climb
        // hold. returns 0 when nothing has been measured yet.
        int recent(int count) const {
            if (sampleCount == 0) {
                return 0;
            }
            int scratchSamples[HOLD_SAMPLE_COUNT];
            int const take = std::clamp(count, 1, sampleCount);
            for (int index = 0; index < take; ++index) {
                int const slot = (nextSampleIndex - 1 - index + HOLD_SAMPLE_COUNT * 2) % HOLD_SAMPLE_COUNT;
                scratchSamples[index] = holdSamples[slot];
            }
            std::sort(scratchSamples, scratchSamples + take);
            return scratchSamples[take / 2];
        }
    };

    enum class InputKind { Tap, Wave, Robot, Ship, Other };

    struct Buffer {

        PlayerObject* ghostPlayer1 = nullptr;
        PlayerObject* ghostPlayer2 = nullptr;
        CCLayer* ghostRenderLayer = nullptr;
        unsigned ghostLayerBaseIndex = 0;
        bool performanceMode = false;
        double userBudgetMilliseconds = 6.0;
        bool automaticBudget = true;
        int keyStates[6] = { 0, 0, 0, 0, 0, 0 };
        std::unordered_set<GameObject*> simulationDisabledObjects;

        bool isSimulating = false;
        bool isInjecting = false;
        bool hasGhostDied = false;
        bool hasGhostHitPortal = false;
        bool hasRealInput = false;
        bool hasAppliedInput = false;
        bool isFirstPlayer = true;

        bool isEnabled = true;
        int stageIndex = 0;
        int ticksAllModes = 2;
        int ticksWaveMode = 2;

        std::deque<QueuedInput> queuedInputs;
        int inputShiftTicks = 0;

        float tickLengthSeconds = TICK_DURATION;
        int ticksSinceInput = 999;
        int rapidInputTicks = 0;

        bool smartEnabled = true;
        bool consoleEnabled = false;
        double budgetMilliseconds = 6.0;
        bool practiceModeOnly = true;
        int groundHorizonTicks = 130;
        int releaseShiftTicks = 0;
        int pressShiftTicks = 0;
        int queuedPressTick = -1;
        int queuedReleaseTick = -1;
        int waveHorizonTicks = 65;
        bool assumeOrbHits = true;
        bool isHeld1 = false, isHeld2 = false;
        bool isAuto1 = false, isAuto2 = false;
        bool orbTimingEnabled = true;
        int orbClickDelayTicks = 0;
        int autoReleaseTick1 = -1, autoReleaseTick2 = -1;
        bool wasOrbTiming = false;
        bool wasOrbReach = false;
        bool tryBothOrbCandidates = true;
        bool orbClickEnabled = true;
        bool hasTouchedOrb = false;
        bool skipPressHandling = false;
        bool avoidMovingObjects = true;
        bool hasSeenMover = false;
        bool trackMovingObjects = false;
        bool autoReleaseEnabled = false;
        int autoReleaseWindowTicks = 6;
        bool wasAutoReleased = false;
        bool orbBufferEnabled = true;
        bool wasOrbInRange = false;
        bool waveMarginEnabled = true;
        bool waveVersion46Mode = false;
        int waveBandLow = 0;
        int waveBandHigh = 0;
        // the shift this level's learned profile suggested for the last wave input, 0 when the
        // profile was still too cold to say anything. logged next to the band so the learning
        // can be followed from the terminal/Geode log without a single extra keypress.
        int waveLearnTicks = 0;
        bool orbAlignEnabled = true;
        bool orbAlignToGround = false;
        bool ignoreTouchingObjects = true;
        bool hasRealRectangle = false;
        CCRect realRectanglePlayer1, realRectanglePlayer2;
        std::unordered_set<GameObject*> harmlessObjects;
        int baseEscapeTicks = -1;
        int baseCrashTicks = -1;

        bool lowLatencyMode = false;
        bool explainEnabled = false;
        bool deathLogEnabled = false;
        bool captureKillerObject = false;
        bool hasBaseTouchedDown = false;
        bool hasSeenKiller = false;
        int killerCollisionTick = -1;
        int killerObjectId = 0, killerObjectType = -1;
        bool killerWithoutTouch = false, isKillerDisabled = false;
        CCRect killerRectangle, ghostDeathRectangle;
        bool robustSaveMode = true;
        float ghostHorizontalShift = 0.f;
        int lateCrashTicks = 90;
        bool predictMovingObjects = false;
        bool shipBufferEnabled = false;
        int shipMaxDelayTicks = 2;
        // smart straight-fly: only ever nudges taps the player already made (see ADR-013).
        bool shipStraightEnabled = true;
        int shipStraightStrength = SHIP_STRAIGHT_STRENGTH_DEFAULT;
        int shipStraightSensitivity = SHIP_STRAIGHT_SENSITIVITY_DEFAULT;
        int shipStraightRhythm = SHIP_STRAIGHT_RHYTHM_DEFAULT;
        int lastStraightLogTick = -1000;

        bool performanceLogEnabled = false;
        int simulationRunCount = 0;
        long simulatedTickCount = 0;
        bool clickBetweenFramesSync = true;
        float firstStepScale = 1.f;
        bool isInRealUpdate = false;
        float tickStartPositionX = 0.f;
        float tickDeltaX = 0.f;
        bool dashAutoReleaseEnabled = false;
        bool autoDashEnabled = false;
        std::unordered_set<GameObject*> usedOrbs;
        bool moreTimeEnabled = true;
        bool ballSmartEnabled = true;
        bool swingSmartEnabled = true;
        bool trainingMode = false;
        bool perLevelStats = true;
        bool heatmapEnabled = true;

        CCLabelBMFont* hintTextLabel = nullptr;
        CCDrawNode* heatmapNode = nullptr;
        bool ufoSmartEnabled = true;
        bool spiderSmartEnabled = true;
        int retapTick = -1;
        bool robotSmartEnabled = true;
        bool robotAutoReleaseEnabled = false;
        bool autoNormalRunsEnabled = false;
        CCLabelBMFont* autoTagLabel = nullptr;
        CCLabelBMFont* watermarkLabel = nullptr;
        bool wasUsedThisAttempt = false;
        bool learningEnabled = true;
        bool fullSimulationState = true;

        int orbWindowTicks = 10;
        int simulationTick = 0;
        int firstOrbTouchTick = -1;
        int orbUseTick = -1;
        RingObject* firstOrbRingObject = nullptr;
        PlayerObject* firstOrbPlayerObject = nullptr;
        int orbCloseSampleTick = -1;
        float orbTouchDistanceSquared = 0.f;
        float orbCloseDistanceSquared = 0.f;
        RingObject* intendedOrbObject = nullptr;
        RingObject* pressOrbRingObject = nullptr;
        int inputTickIndex = 0;
        bool isInputDown = false;
        std::vector<ProbeOrb> probeOrbCandidates;
        bool isProbing = false;
        bool driftCheckEnabled = false;
        std::vector<CCPoint>* recordedPath = nullptr;
        bool showPathEnabled = false;
        CCDrawNode* pathDrawNode = nullptr;
        int pathHoldUntilTick = 0;

        std::unordered_set<GameObject*> simulationUsedObjects;
        std::vector<ObjSnapshot> objectSnapshots;
    } state;

    HoldStats tapHoldHistory, robotHoldHistory;
    // how long the player waits between letting go and pressing again on a wave. this is the
    // half of the wave rhythm that was never measured: a release was judged as "fall forever",
    // which is a situation that never happens in play.
    HoldStats waveGapHistory;
    // press -> release is the other half of the wave rhythm. without it a high-cps press was still
    // simulated as "hold forever", so the search delayed it through a wall the player never held.
    HoldStats waveHoldHistory;
    // ship rhythm, measured the same passive way: how long a press is held and how long the
    // player waits after letting go before pressing again. the two sides are what a ship input
    // is actually judged against now instead of "hold forever" / "release forever".
    HoldStats shipHoldHistory, shipGapHistory;
    int currentTick = 0;
    int lastPressTick = -1;
    int lastReleaseTick = -1;
    InputKind lastPressKind = InputKind::Other;
    InputKind lastReleaseKind = InputKind::Other;
    std::chrono::steady_clock::time_point clickStartTime;
    int debugBestDelay = 0;
    int debugBestResult = -1;
    int robotUsedHoldTicks = -1;
    bool isQuickScore = false;
    bool isCrashFixed = false;
    bool isMarginUsed = false;

    // ---- passive learning ---------------------------------------------------------------
    // nothing here ever asks the player for input. dynamic only looks at what it already
    // sees while playing: the hold length the player really performed, and the corrections
    // its own wave search ends up applying. a learned value can never make an unsafe tick
    // count as safe - it only changes which delay is simulated first, and nudges the pick
    // inside a band of delays the simulation already proved survive the whole window. the
    // profile is written to the mod save, so every session starts where the last one stopped
    // and keeps getting closer to how this player actually times their clicks.
    constexpr int LEARN_LEVEL_SLOTS = 24;
    constexpr int LEARN_MIN_SAMPLES = 3;
    // how much of the way to the learned shift the pick is allowed to lean inside the proven
    // band. 50% of a half keeps at least a quarter of the band as margin on both sides, so
    // learning never walks the click onto the killing edge.
    constexpr int LEARN_BAND_PULL_PERCENT = 50;
    constexpr int LEARN_FLUSH_EVERY = 32;
    constexpr int LEARN_SAMPLE_CAP = 64;

    struct LevelAdjust {
        int levelKey = 0;
        int savedCount = 0;
        // tenths of a tick, exponential moving average: recent corrections weigh more than
        // old ones, so a player who gets cleaner is followed instead of averaged backwards.
        int delayTenths = 0;
        int bandLow = 0;
        int bandHigh = 0;
        int bandSamples = 0;
        // presses and releases are different distributions: a press is how late the player
        // jumps, a release is how late they let go, and the release numbers are bigger. one
        // shared value made the press prior lean toward release-sized shifts and the release
        // prior lean toward press-sized ones, which is exactly the "presses are fine, releases
        // are not" split the player reported.
        int releaseTenths = 0;
        int releaseCount = 0;
    };

    std::vector<LevelAdjust> g_levelAdjusts;
    int g_currentLevelKey = 0;
    bool g_learnDirty = false;
    int g_learnEventsSinceFlush = 0;

    std::vector<int> parseIntList(std::string const& raw) {
        std::vector<int> values;
        int value = 0;
        bool hasDigits = false;
        for (size_t index = 0; index <= raw.size(); ++index) {
            char const character = index < raw.size() ? raw[index] : ',';
            if (character >= '0' && character <= '9') {
                value = value * 10 + (character - '0');
                hasDigits = true;
                continue;
            }
            if (hasDigits) {
                values.push_back(value);
                value = 0;
                hasDigits = false;
            }
        }
        return values;
    }

    int levelKeyFor(GJGameLevel* level) {
        if (!level) {
            return 0;
        }
        int const levelID = static_cast<int>(level->m_levelID);
        if (levelID > 0) {
            return static_cast<int>((static_cast<uint32_t>(levelID) * 2654435761u) & 0x7fffffffu) | 1;
        }
        // local / unsaved levels: hash the name so two different levels never share a profile.
        std::string const name(level->m_levelName.c_str());
        if (name.empty()) {
            return 0;
        }
        uint32_t hash = 2166136261u;
        for (unsigned char character : name) {
            hash ^= character;
            hash *= 16777619u;
        }
        return static_cast<int>(hash & 0x7fffffffu) | 1;
    }

    LevelAdjust* levelAdjust(int key, bool create) {
        if (key == 0) {
            return nullptr;
        }
        for (auto& adjust : g_levelAdjusts) {
            if (adjust.levelKey == key) {
                return &adjust;
            }
        }
        if (!create) {
            return nullptr;
        }
        if (static_cast<int>(g_levelAdjusts.size()) >= LEARN_LEVEL_SLOTS) {
            // forget the level we know the least about; the level being played is never the
            // victim, otherwise the profile would wipe itself while it is being built.
            size_t victim = g_levelAdjusts.size();
            int leastKnown = 1 << 30;
            for (size_t index = 0; index < g_levelAdjusts.size(); ++index) {
                if (g_levelAdjusts[index].levelKey == g_currentLevelKey) {
                    continue;
                }
                if (g_levelAdjusts[index].savedCount < leastKnown) {
                    leastKnown = g_levelAdjusts[index].savedCount;
                    victim = index;
                }
            }
            if (victim < g_levelAdjusts.size()) {
                g_levelAdjusts.erase(g_levelAdjusts.begin() + static_cast<std::ptrdiff_t>(victim));
            }
        }
        g_levelAdjusts.push_back(LevelAdjust { key });
        return &g_levelAdjusts.back();
    }

    int learnedWaveDelay(bool isRelease) {
        if (!state.learningEnabled) {
            return 0;
        }
        auto* adjust = levelAdjust(g_currentLevelKey, false);
        if (!adjust) {
            return 0;
        }
        int const count = isRelease ? adjust->releaseCount : adjust->savedCount;
        if (count < LEARN_MIN_SAMPLES) {
            return 0;
        }
        int const tenths = isRelease ? adjust->releaseTenths : adjust->delayTenths;
        return (tenths + 5) / 10;
    }

    std::string encodeLearnStore() {
        std::string raw;
        for (auto const& adjust : g_levelAdjusts) {
            if (adjust.savedCount <= 0 && adjust.bandSamples <= 0 && adjust.releaseCount <= 0) {
                continue;
            }
            raw += fmt::format("{}:{}:{}:{}:{}:{}:{}:{},", adjust.levelKey, adjust.savedCount, adjust.delayTenths, adjust.bandLow, adjust.bandHigh,
                adjust.bandSamples, adjust.releaseTenths, adjust.releaseCount);
        }
        return raw;
    }

    void decodeLearnStore(std::string const& raw) {
        g_levelAdjusts.clear();
        std::string piece;
        for (size_t index = 0; index <= raw.size(); ++index) {
            if (index < raw.size() && raw[index] != ',') {
                piece.push_back(raw[index]);
                continue;
            }
            auto const values = parseIntList(piece);
            piece.clear();
            if (values.size() < 6) {
                continue;
            }
            if (static_cast<int>(g_levelAdjusts.size()) >= LEARN_LEVEL_SLOTS) {
                break;
            }
            LevelAdjust adjust;
            adjust.levelKey = values[0];
            adjust.savedCount = std::clamp(values[1], 0, LEARN_SAMPLE_CAP);
            adjust.delayTenths = std::clamp(values[2], 0, 240 * 10);
            adjust.bandLow = std::max(0, values[3]);
            adjust.bandHigh = std::max(adjust.bandLow, values[4]);
            adjust.bandSamples = std::max(0, values[5]);
            if (values.size() >= 8) {
                adjust.releaseTenths = std::clamp(values[6], 0, 240 * 10);
                adjust.releaseCount = std::clamp(values[7], 0, LEARN_SAMPLE_CAP);
            }
            else {
                // a profile written before presses and releases were separated: keep the old
                // shared value for releases so a played level does not lose its memory.
                adjust.releaseTenths = adjust.delayTenths;
                adjust.releaseCount = adjust.savedCount;
            }
            if (adjust.levelKey == 0) {
                continue;
            }
            g_levelAdjusts.push_back(adjust);
        }
    }

    std::string encodeHoldStats(HoldStats const& stats) {
        std::string raw = fmt::format("{}:{}", stats.sampleCount, stats.nextSampleIndex);
        for (int index = 0; index < stats.sampleCount; ++index) {
            raw += fmt::format(",{}", stats.holdSamples[index]);
        }
        return raw;
    }

    void loadHoldStats(std::string const& key, HoldStats& stats) {
        auto const values = parseIntList(Mod::get()->getSavedValue<std::string>(key, ""));
        if (values.size() < 2) {
            return;
        }
        int const count = std::clamp(values[0], 0, HOLD_SAMPLE_COUNT);
        stats.nextSampleIndex = std::clamp(values[1], 0, HOLD_SAMPLE_COUNT - 1);
        stats.sampleCount = 0;
        for (int index = 0; index < count && static_cast<size_t>(index + 2) < values.size(); ++index) {
            stats.holdSamples[index] = std::clamp(values[index + 2], 1, 240);
            ++stats.sampleCount;
        }
    }

    void flushLearnStore(bool force) {
        if (!g_learnDirty) {
            return;
        }
        if (!force && g_learnEventsSinceFlush < LEARN_FLUSH_EVERY) {
            return;
        }
        g_learnDirty = false;
        g_learnEventsSinceFlush = 0;
        Mod::get()->setSavedValue<std::string>("learn-waves", encodeLearnStore());
        Mod::get()->setSavedValue<std::string>("holds-tap", encodeHoldStats(tapHoldHistory));
        Mod::get()->setSavedValue<std::string>("holds-robot", encodeHoldStats(robotHoldHistory));
        Mod::get()->setSavedValue<std::string>("holds-wave", encodeHoldStats(waveGapHistory));
        Mod::get()->setSavedValue<std::string>("holds-wave-hold", encodeHoldStats(waveHoldHistory));
        Mod::get()->setSavedValue<std::string>("holds-ship", encodeHoldStats(shipHoldHistory));
        Mod::get()->setSavedValue<std::string>("gaps-ship", encodeHoldStats(shipGapHistory));
        (void)Mod::get()->saveData();
    }

    void markLearnActivity() {
        // no file i/o on the click path: this only marks the profile dirty. it is written at
        // natural boundaries (death, reset, pause, quit), so a busy wave section never waits
        // on the disk to deliver a click.
        g_learnDirty = true;
        ++g_learnEventsSinceFlush;
    }

    void noteWaveCorrection(int delayTicks, int bandLow, int bandHigh, bool isRelease) {
        if (!state.learningEnabled || delayTicks <= 0) {
            return;
        }
        auto* adjust = levelAdjust(g_currentLevelKey, true);
        if (!adjust) {
            return;
        }
        int& tenths = isRelease ? adjust->releaseTenths : adjust->delayTenths;
        int& count = isRelease ? adjust->releaseCount : adjust->savedCount;
        int const scaled = std::clamp(delayTicks, 1, 240) * 10;
        if (count == 0) {
            tenths = scaled;
        }
        else {
            // 25% weight on the newest correction: adapts to a player who is improving,
            // instead of letting a bad early session dominate forever.
            tenths += (scaled - tenths) / 4;
        }
        count = std::min(count + 1, LEARN_SAMPLE_CAP);
        if (bandHigh > bandLow) {
            if (adjust->bandSamples == 0) {
                adjust->bandLow = bandLow;
                adjust->bandHigh = bandHigh;
            }
            else {
                adjust->bandLow += (bandLow - adjust->bandLow) / 4;
                adjust->bandHigh += (bandHigh - adjust->bandHigh) / 4;
            }
            adjust->bandSamples = std::min(adjust->bandSamples + 1, LEARN_SAMPLE_CAP);
        }
        markLearnActivity();
    }

    void beginLevelLearning(GJGameLevel* level) {
        g_currentLevelKey = levelKeyFor(level);
        auto* adjust = levelAdjust(g_currentLevelKey, false);
        if (!adjust || (adjust->savedCount <= 0 && adjust->releaseCount <= 0)) {
            log::info("[learn] level {} | no profile yet, learning passively", g_currentLevelKey);
            return;
        }
        log::info("[learn] level {} | {} press / {} release saves | learned shift press +{:.1f}t release +{:.1f}t | band {}-{} ({} band saves)", g_currentLevelKey,
            adjust->savedCount, adjust->releaseCount, adjust->delayTenths / 10.0, adjust->releaseTenths / 10.0, adjust->bandLow, adjust->bandHigh,
            adjust->bandSamples);
    }

    // ---- level scan ---------------------------------------------------------------------
    // one pass over the level's object graph the first time a level is played. this is an
    // index, not a solver: it records what the level is made of (orbs, pads, portals,
    // hazards, solids), where the input points sit, and how dense the level is along its
    // length. the index is cached per level, so opening the same level again reads the saved
    // scan instead of scanning. a failed scan (objects not created yet) retries itself.
    constexpr int SCAN_SLOTS = 12;
    constexpr int SCAN_ROUTE_CAP = 64;
    constexpr int SCAN_ROUTE_CANDIDATES = 4096;
    constexpr int SCAN_DENSITY_BUCKETS = 16;
    constexpr int SCAN_SCHEMA_VERSION = 1;

    constexpr uint64_t SCAN_ORB_TYPES =
        typeBit(GameObjectType::YellowJumpRing) | typeBit(GameObjectType::PinkJumpRing) | typeBit(GameObjectType::RedJumpRing) |
        typeBit(GameObjectType::GravityRing) | typeBit(GameObjectType::GreenRing) | typeBit(GameObjectType::DropRing) |
        typeBit(GameObjectType::CustomRing) | typeBit(GameObjectType::DashRing) | typeBit(GameObjectType::GravityDashRing) |
        typeBit(GameObjectType::SpiderOrb) | typeBit(GameObjectType::TeleportOrb);

    constexpr uint64_t SCAN_PAD_TYPES =
        typeBit(GameObjectType::YellowJumpPad) | typeBit(GameObjectType::PinkJumpPad) | typeBit(GameObjectType::RedJumpPad) |
        typeBit(GameObjectType::GravityPad) | typeBit(GameObjectType::SpiderPad);

    constexpr uint64_t SCAN_PORTAL_TYPES =
        typeBit(GameObjectType::InverseGravityPortal) | typeBit(GameObjectType::NormalGravityPortal) |
        typeBit(GameObjectType::GravityTogglePortal) | typeBit(GameObjectType::ShipPortal) | typeBit(GameObjectType::CubePortal) |
        typeBit(GameObjectType::BallPortal) | typeBit(GameObjectType::UfoPortal) | typeBit(GameObjectType::WavePortal) |
        typeBit(GameObjectType::RobotPortal) | typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal) |
        typeBit(GameObjectType::DualPortal) | typeBit(GameObjectType::SoloPortal) | typeBit(GameObjectType::TeleportPortal) |
        typeBit(GameObjectType::InverseMirrorPortal) | typeBit(GameObjectType::NormalMirrorPortal) |
        typeBit(GameObjectType::RegularSizePortal) | typeBit(GameObjectType::MiniSizePortal);

    constexpr uint64_t SCAN_SOLID_TYPES =
        typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Slope) | typeBit(GameObjectType::Breakable);

    struct ScanPoint {
        int x = 0;
        int y = 0;
        int kind = 0; // 0 orb, 1 pad, 2 portal
    };

    struct LevelScan {
        int levelKey = 0;
        int fingerprint = 0;
        int objectCount = 0;
        int hazardCount = 0;
        int solidCount = 0;
        int orbCount = 0;
        int padCount = 0;
        int portalCount = 0;
        int maxX = 0;
        int durationMs = 0;
        int attempts = 1;
        int64_t stamp = 0;
        unsigned char density[SCAN_DENSITY_BUCKETS] = {};
        std::vector<ScanPoint> route;
        std::string name;
    };

    std::vector<LevelScan> g_levelScans;
    LevelScan g_scanCurrent;
    bool g_scanHasCurrent = false;
    bool g_scanPending = false;
    bool g_scanRunning = false;
    bool g_scanSucceeded = false;
    bool g_scanCached = false;
    bool g_scanFailed = false;
    bool g_scanDirty = false;
    int g_scanAttempts = 0;
    int64_t g_scanLastAttemptMs = 0;
    std::string g_scanLastError;

    int64_t scanNowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    int64_t scanNowStamp() {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::vector<std::string> splitText(std::string const& raw, char separator) {
        std::vector<std::string> parts;
        std::string piece;
        for (size_t index = 0; index <= raw.size(); ++index) {
            char const character = index < raw.size() ? raw[index] : separator;
            if (character == separator) {
                parts.push_back(piece);
                piece.clear();
                continue;
            }
            piece.push_back(character);
        }
        return parts;
    }

    std::string hexEncode(std::string const& value) {
        static char const* digits = "0123456789abcdef";
        std::string out;
        out.reserve(value.size() * 2);
        for (unsigned char character : value) {
            out.push_back(digits[character >> 4]);
            out.push_back(digits[character & 15]);
        }
        return out;
    }

    int hexNibble(char character) {
        if (character >= '0' && character <= '9') { return character - '0'; }
        if (character >= 'a' && character <= 'f') { return character - 'a' + 10; }
        if (character >= 'A' && character <= 'F') { return character - 'A' + 10; }
        return -1;
    }

    std::string hexDecode(std::string const& value) {
        auto nibble = [](char character) -> int {
            return hexNibble(character);
        };
        std::string out;
        for (size_t index = 0; index + 1 < value.size(); index += 2) {
            int const high = nibble(value[index]);
            int const low = nibble(value[index + 1]);
            if (high < 0 || low < 0) {
                break;
            }
            out.push_back(static_cast<char>((high << 4) | low));
        }
        return out;
    }

    std::string encodeScans() {
        std::string raw;
        for (auto const& scan : g_levelScans) {
            if (scan.levelKey == 0 || scan.objectCount <= 0) {
                continue;
            }
            std::string density;
            density.reserve(SCAN_DENSITY_BUCKETS * 2);
            for (int index = 0; index < SCAN_DENSITY_BUCKETS; ++index) {
                density += fmt::format("{:02x}", static_cast<int>(scan.density[index]));
            }
            std::string route;
            for (size_t index = 0; index < scan.route.size(); ++index) {
                if (index > 0) {
                    route.push_back('|');
                }
                route += fmt::format("{}:{}:{}", scan.route[index].x, scan.route[index].y, scan.route[index].kind);
            }
            raw += fmt::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{};", scan.levelKey, scan.fingerprint, scan.objectCount,
                scan.hazardCount, scan.solidCount, scan.orbCount, scan.padCount, scan.portalCount, scan.maxX, scan.durationMs,
                scan.attempts, scan.stamp, density, route, hexEncode(scan.name));
        }
        return raw;
    }

    void decodeScans(std::string const& raw) {
        g_levelScans.clear();
        for (auto const& record : splitText(raw, ';')) {
            if (record.empty()) {
                continue;
            }
            auto const fields = splitText(record, ',');
            if (fields.size() < 15) {
                continue;
            }
            auto intAt = [&fields](size_t index) -> int {
                auto const values = parseIntList(fields[index]);
                return values.empty() ? 0 : values[0];
            };
            LevelScan scan;
            scan.levelKey = intAt(0);
            if (scan.levelKey == 0) {
                continue;
            }
            scan.fingerprint = intAt(1);
            scan.objectCount = std::max(0, intAt(2));
            if (scan.objectCount <= 0) {
                continue;
            }
            scan.hazardCount = std::max(0, intAt(3));
            scan.solidCount = std::max(0, intAt(4));
            scan.orbCount = std::max(0, intAt(5));
            scan.padCount = std::max(0, intAt(6));
            scan.portalCount = std::max(0, intAt(7));
            scan.maxX = std::max(0, intAt(8));
            scan.durationMs = std::max(0, intAt(9));
            scan.attempts = std::clamp(intAt(10), 1, 100000);
            scan.stamp = std::strtoll(fields[11].c_str(), nullptr, 10);

            std::string const& density = fields[12];
            for (int index = 0; index < SCAN_DENSITY_BUCKETS && index * 2 + 1 < static_cast<int>(density.size()); ++index) {
                int const high = std::max(0, hexNibble(density[static_cast<size_t>(index) * 2]));
                int const low = std::max(0, hexNibble(density[static_cast<size_t>(index) * 2 + 1]));
                scan.density[index] = static_cast<unsigned char>(std::clamp((high << 4) | low, 0, 255));
            }

            for (auto const& pointText : splitText(fields[13], '|')) {
                auto const numbers = parseIntList(pointText);
                if (numbers.size() < 3 || static_cast<int>(scan.route.size()) >= SCAN_ROUTE_CAP) {
                    continue;
                }
                scan.route.push_back(ScanPoint { numbers[0], numbers[1], std::clamp(numbers[2], 0, 2) });
            }
            scan.name = hexDecode(fields[14]);

            if (static_cast<int>(g_levelScans.size()) >= SCAN_SLOTS) {
                break;
            }
            g_levelScans.push_back(std::move(scan));
        }
    }

    LevelScan* findScan(int levelKey) {
        if (levelKey == 0) {
            return nullptr;
        }
        for (auto& scan : g_levelScans) {
            if (scan.levelKey == levelKey) {
                return &scan;
            }
        }
        return nullptr;
    }

    void forgetScan(int levelKey) {
        for (size_t index = 0; index < g_levelScans.size(); ++index) {
            if (g_levelScans[index].levelKey == levelKey) {
                g_levelScans.erase(g_levelScans.begin() + static_cast<std::ptrdiff_t>(index));
                g_scanDirty = true;
                return;
            }
        }
    }

    void upsertScan(LevelScan const& scan) {
        for (auto& existing : g_levelScans) {
            if (existing.levelKey == scan.levelKey) {
                existing = scan;
                g_scanDirty = true;
                return;
            }
        }
        if (static_cast<int>(g_levelScans.size()) >= SCAN_SLOTS) {
            size_t victim = 0;
            int64_t oldest = -1;
            for (size_t index = 0; index < g_levelScans.size(); ++index) {
                if (oldest < 0 || g_levelScans[index].stamp < oldest) {
                    oldest = g_levelScans[index].stamp;
                    victim = index;
                }
            }
            g_levelScans.erase(g_levelScans.begin() + static_cast<std::ptrdiff_t>(victim));
        }
        g_levelScans.push_back(scan);
        g_scanDirty = true;
    }

    void flushScanStore() {
        if (!g_scanDirty) {
            return;
        }
        g_scanDirty = false;
        Mod::get()->setSavedValue<std::string>("level-scans", encodeScans());
        Mod::get()->setSavedValue<int>("scan-schema", SCAN_SCHEMA_VERSION);
        (void)Mod::get()->saveData();
    }

    int scanFingerprint(GJGameLevel* level, int levelKey) {
        size_t const levelChars = level ? level->m_levelString.size() : 0;
        uint32_t hash = static_cast<uint32_t>(levelKey) * 2654435761u;
        hash ^= static_cast<uint32_t>(levelChars) * 2246822519u;
        return static_cast<int>(hash & 0x7fffffffu);
    }

    bool buildLevelScan(PlayLayer* layer, LevelScan& out, std::string& error) {
        int64_t const start = scanNowMs();
        if (!layer || !layer->m_level) {
            error = "no level data";
            return false;
        }
        auto* level = layer->m_level;
        out.name = std::string(level->m_levelName.c_str());
        out.levelKey = levelKeyFor(level);
        out.fingerprint = scanFingerprint(level, out.levelKey);
        if (out.levelKey == 0) {
            error = "level has no id or name";
            return false;
        }
        auto* objects = layer->m_objects;
        if (!objects || objects->count() == 0) {
            error = "objects not ready";
            return false;
        }

        int maxX = 0;
        for (auto* object : CCArrayExt<GameObject*>(objects)) {
            if (!object) {
                continue;
            }
            float const positionX = object->getPositionX();
            if (!std::isfinite(positionX)) {
                continue;
            }
            maxX = std::max(maxX, static_cast<int>(std::lround(positionX)));
        }
        out.maxX = std::max(0, maxX);

        int bucketCounts[SCAN_DENSITY_BUCKETS] = {};
        std::vector<ScanPoint> candidates;
        candidates.reserve(256);

        for (auto* object : CCArrayExt<GameObject*>(objects)) {
            if (!object) {
                continue;
            }
            ++out.objectCount;
            float const positionX = object->getPositionX();
            float const positionY = object->getPositionY();
            int const x = std::isfinite(positionX) ? std::max(0, static_cast<int>(std::lround(positionX))) : 0;
            int const y = std::isfinite(positionY) ? static_cast<int>(std::lround(positionY)) : 0;
            int bucket = 0;
            if (out.maxX > 0) {
                bucket = static_cast<int>((static_cast<int64_t>(x) * SCAN_DENSITY_BUCKETS) / (out.maxX + 1));
            }
            bucket = std::clamp(bucket, 0, SCAN_DENSITY_BUCKETS - 1);
            ++bucketCounts[bucket];

            if (hasType(HAZARD_OBJECT_TYPES, object)) {
                ++out.hazardCount;
            }
            else if (hasType(SCAN_SOLID_TYPES, object)) {
                ++out.solidCount;
            }

            int kind = -1;
            if (hasType(SCAN_ORB_TYPES, object)) {
                ++out.orbCount;
                kind = 0;
            }
            else if (hasType(SCAN_PAD_TYPES, object)) {
                ++out.padCount;
                kind = 1;
            }
            else if (hasType(SCAN_PORTAL_TYPES, object)) {
                ++out.portalCount;
                kind = 2;
            }
            if (kind >= 0 && static_cast<int>(candidates.size()) < SCAN_ROUTE_CANDIDATES) {
                candidates.push_back(ScanPoint { x, y, kind });
            }
        }

        int maxBucket = 0;
        for (int index = 0; index < SCAN_DENSITY_BUCKETS; ++index) {
            maxBucket = std::max(maxBucket, bucketCounts[index]);
        }
        for (int index = 0; index < SCAN_DENSITY_BUCKETS; ++index) {
            int const scaled = maxBucket > 0 ? (bucketCounts[index] * 255) / maxBucket : 0;
            out.density[index] = static_cast<unsigned char>(std::clamp(scaled, 0, 255));
        }

        std::sort(candidates.begin(), candidates.end(), [](ScanPoint const& left, ScanPoint const& right) {
            if (left.x != right.x) {
                return left.x < right.x;
            }
            return left.y < right.y;
        });
        int const candidateCount = static_cast<int>(candidates.size());
        int const keep = std::min(candidateCount, SCAN_ROUTE_CAP);
        out.route.clear();
        out.route.reserve(static_cast<size_t>(std::max(0, keep)));
        for (int index = 0; index < keep; ++index) {
            int source = keep == candidateCount
                ? index
                : static_cast<int>((static_cast<int64_t>(index) * candidateCount) / std::max(1, keep));
            source = std::clamp(source, 0, candidateCount - 1);
            out.route.push_back(candidates[source]);
        }

        out.durationMs = static_cast<int>(std::clamp<int64_t>(scanNowMs() - start, 0, 60000));
        return true;
    }

    bool tryLevelScan(PlayLayer* layer) {
        g_scanLastAttemptMs = scanNowMs();
        ++g_scanAttempts;
        g_scanRunning = true;
        g_scanFailed = false;

        LevelScan scan;
        std::string error;
        if (!buildLevelScan(layer, scan, error)) {
            g_scanRunning = false;
            g_scanFailed = true;
            g_scanSucceeded = false;
            g_scanLastError = error;
            g_scanPending = error == "objects not ready";
            if (g_scanAttempts <= 3 || g_scanAttempts % 10 == 0) {
                log::info("[scan] attempt {} failed: {}", g_scanAttempts, error);
            }
            return false;
        }

        scan.attempts = g_scanAttempts;
        scan.stamp = scanNowStamp();
        g_scanCurrent = scan;
        g_scanHasCurrent = true;
        g_scanSucceeded = true;
        g_scanCached = false;
        g_scanFailed = false;
        g_scanRunning = false;
        g_scanPending = false;
        g_scanLastError.clear();
        upsertScan(g_scanCurrent);
        flushScanStore();
        log::info("[scan] level {} ({}) | scan successful in {} ms after {} attempt(s) | {} objects | {} orbs {} pads {} portals | {} hazards {} solids | max x {} | {} route points",
            scan.levelKey, scan.name, scan.durationMs, scan.attempts, scan.objectCount, scan.orbCount, scan.padCount, scan.portalCount,
            scan.hazardCount, scan.solidCount, scan.maxX, static_cast<int>(scan.route.size()));
        return true;
    }

    void beginLevelScan(PlayLayer* layer, bool force = false) {
        g_scanAttempts = 0;
        g_scanPending = false;
        g_scanRunning = false;
        g_scanSucceeded = false;
        g_scanCached = false;
        g_scanFailed = false;
        g_scanLastAttemptMs = 0;
        g_scanLastError.clear();
        g_scanHasCurrent = false;
        g_scanCurrent = LevelScan {};

        if (!layer || !layer->m_level) {
            return;
        }
        auto* level = layer->m_level;
        g_scanCurrent.levelKey = levelKeyFor(level);
        g_scanCurrent.name = std::string(level->m_levelName.c_str());
        g_scanCurrent.fingerprint = scanFingerprint(level, g_scanCurrent.levelKey);
        g_scanHasCurrent = g_scanCurrent.levelKey != 0;

        if (!force) {
            if (auto* existing = findScan(g_scanCurrent.levelKey)) {
                if (existing->fingerprint == g_scanCurrent.fingerprint) {
                    g_scanCurrent = *existing;
                    g_scanHasCurrent = true;
                    g_scanSucceeded = true;
                    g_scanCached = true;
                    g_scanAttempts = existing->attempts;
                    log::info("[scan] level {} | using saved scan ({} objects, {} orbs) | no rescan", existing->levelKey,
                        existing->objectCount, existing->orbCount);
                    return;
                }
                log::info("[scan] level {} | level fingerprint changed, rescanning", g_scanCurrent.levelKey);
            }
        }
        else {
            forgetScan(g_scanCurrent.levelKey);
            log::info("[scan] manual rescan requested for level {}", g_scanCurrent.levelKey);
        }

        g_scanPending = true;
        tryLevelScan(layer);
    }

    void requestLevelScanRescan() {
        auto* manager = GameManager::get();
        auto* layer = manager ? manager->m_playLayer : nullptr;
        if (!layer || !layer->m_level) {
            log::info("[scan] rescan ignored: no level open");
            return;
        }
        beginLevelScan(layer, true);
    }

    // retry while the level reports "objects not ready". normal levels pass on the first
    // attempt; the retry exists so a scan is never silently lost to a slow object build.
    void pumpLevelScan(PlayLayer* layer) {
        if (!g_scanPending || !layer) {
            return;
        }
        int64_t const now = scanNowMs();
        int64_t const interval = g_scanAttempts < 20 ? 350 : 2000;
        if (now - g_scanLastAttemptMs < interval) {
            return;
        }
        tryLevelScan(layer);
    }

    void cycleStage(int direction);
    void setStageIndex(int stageIndex);
    void sendButton(PlayLayer* gameLayer, bool isDown);
    void toggleTerminal();
    void toggleDynamicPanel();
    std::string stageText();

    float stepScale() {
        float deltaTime = state.tickLengthSeconds;
        if (!(deltaTime > 0.001f && deltaTime < 2.f)) {
            return 1.f;
        }
        return std::clamp(TICK_DURATION / deltaTime, 0.25f, 8.f);
    }

    int st(int ticksAt240Hz) {
        if (ticksAt240Hz <= 0) {
            return ticksAt240Hz;
        }
        return std::max(1, static_cast<int>(std::lround(ticksAt240Hz * stepScale())));
    }

    // how many ticks a wave input has to survive to count as safe. a press is judged over the
    // window the player selected (the tick value is the contract), clamped so tiny windows
    // still check something and huge ones do not demand an impossible unbroken hold.
    //
    // a release used to be judged over a fixed 24 ticks no matter what the player selected.
    // with a small window (the live setting was 13) that made almost every release "unsafe"
    // and the search answered by holding the current direction longer and longer, up to the
    // whole delay budget. a release that already survives the player's own window must be
    // applied unchanged. the 24 stays as the upper bound for the descent check.
    int waveSurvivalTicks(bool isDown, int maxDelayTicks) {
        int const selectedWindow = std::clamp(
            maxDelayTicks,
            st(WAVE_PRESS_HORIZON_MIN_TICKS),
            st(WAVE_PRESS_HORIZON_MAX_TICKS)
        );
        if (!isDown) {
            return std::min(st(WAVE_RELEASE_HORIZON_TICKS), selectedWindow);
        }
        return selectedWindow;
    }

    int activeModeIndex() {
        auto* manager = GameManager::get();
        if (!manager || !manager->m_playLayer) {
            return -1;
        }
        auto* player = manager->m_playLayer->m_player1;
        if (!player) {
            return -1;
        }
        if (player->m_isShip) { return 1; }
        if (player->m_isBall) { return 2; }
        if (player->m_isBird) { return 3; }
        if (player->m_isDart) { return 4; }
        if (player->m_isRobot) { return 5; }
        if (player->m_isSpider) { return 6; }
        if (player->m_isSwing) { return 7; }
        return 0;
    }

    int stageTicksForMode(int mode) {
        if (mode == 4) {
            return state.ticksWaveMode;
        }
        return state.ticksAllModes;
    }

    // the value that is actually used at runtime: a per-mode override wins, otherwise
    // the selected stage's value is used. the panel shows this everywhere so the number
    // on screen is the number the buffer really gets.
    int effectiveTicksForMode(int mode) {
        int const overrideTicks = modeTicksAt(mode);
        return overrideTicks > 0 ? overrideTicks : stageTicksForMode(mode);
    }
    std::string wineUser() {
        if (auto* userName = std::getenv("USER"); userName && *userName) {
            return userName;
        }
        if (auto* logName = std::getenv("LOGNAME"); logName && *logName) {
            return logName;
        }
        return "unknown";
    }

    std::filesystem::path wineRuntimeDir() {
        static std::filesystem::path const runtimeDirectory = [] {
            std::string user = wineUser();
            std::error_code errorCode;
            auto sharedMemoryRoot = std::filesystem::path("Z:\\dev\\shm");
            auto sharedMemoryDirectory = sharedMemoryRoot / ("dynamic-" + user);
            if (std::filesystem::is_directory(sharedMemoryRoot, errorCode)) {
                std::filesystem::create_directories(sharedMemoryDirectory, errorCode);
                if (!errorCode) {
                    return sharedMemoryDirectory;
                }
            }
            auto fallbackDirectory = std::filesystem::path("Z:\\tmp") / ("dynamic-" + user);
            std::filesystem::create_directories(fallbackDirectory, errorCode);
            return fallbackDirectory;
        }();
        return runtimeDirectory;
    }

    enum class HostOS {
        Windows,
        Linux,
        Mac,
        Android,
        IOS,
    };

    bool isMobileHost() {
#ifdef GEODE_IS_MOBILE
        return true;
#else
        return false;
#endif
    }

    bool showFloatingLauncher() {
        if (!isMobileHost()) {
            return false;
        }
        return Mod::get()->getSettingValue<bool>("mobile-floating-button");
    }

    HostOS hostOS() {
#if defined(GEODE_IS_ANDROID)
        return HostOS::Android;
#elif defined(GEODE_IS_IOS)
        return HostOS::IOS;
#elif defined(GEODE_IS_MACOS)
        return HostOS::Mac;
#else
        static HostOS const hostOperatingSystem = geode::utils::platform::isWine() ? HostOS::Linux : HostOS::Windows;
        return hostOperatingSystem;
#endif
    }

    char const* hostOSName() {
        switch (hostOS()) {
            case HostOS::Linux: return "linux";
            case HostOS::Mac: return "macos";
            case HostOS::Android: return "android";
            case HostOS::IOS: return "ios";
            default: return "windows";
        }
    }

    char const* hostOSDescription() {
        switch (hostOS()) {
            case HostOS::Linux: return "proton/wine";
            case HostOS::Mac: return "native";
            case HostOS::Android: return "mobile";
            case HostOS::IOS: return "mobile";
            default: return "native";
        }
    }

    void ensureWineRuntimeDir() {
        if (hostOS() != HostOS::Linux) {
            return;
        }
        auto runtimeDirectory = wineRuntimeDir();
        std::error_code errorCode;
        std::filesystem::create_directories(runtimeDirectory, errorCode);
        std::filesystem::permissions(
            runtimeDirectory,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace,
            errorCode
        );
    }

    std::filesystem::path tempDirectory() {
        std::error_code errorCode;
        auto directory = std::filesystem::temp_directory_path(errorCode);
        if (errorCode || directory.empty()) {
            return {};
        }
        return directory;
    }

    std::filesystem::path macRuntimeDir() {
        static std::filesystem::path const runtimeDirectory = [] {
            auto directory = tempDirectory();
            auto name = "dynamic-" + wineUser();
            return directory.empty() ? std::filesystem::path(name) : directory / name;
        }();
        return runtimeDirectory;
    }

    void ensureMacRuntimeDir() {
        if (hostOS() != HostOS::Mac) {
            return;
        }
        auto runtimeDirectory = macRuntimeDir();
        std::error_code errorCode;
        std::filesystem::create_directories(runtimeDirectory, errorCode);
        std::filesystem::permissions(
            runtimeDirectory,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace,
            errorCode
        );
    }

    std::string shellQuote(std::string const& value) {
        std::string result = "'";
        for (char character : value) {
            if (character == '\'') {
                result += "'\\''";
            }
            else {
                result += character;
            }
        }
        result += '\'';
        return result;
    }

    bool isTerminalEnabled() {
        if (isMobileHost()) {
            return false;
        }
        return Mod::get()->getSettingValue<bool>("terminal-enabled");
    }

    ccColor3B getUIPrimaryColor() {
        return Mod::get()->getSettingValue<ccColor3B>("ui-primary-color");
    }

    ccColor3B getUISecondaryColor() {
        return Mod::get()->getSettingValue<ccColor3B>("ui-secondary-color");
    }

    ccColor3B getUISurfaceColor() {
        return Mod::get()->getSettingValue<ccColor3B>("ui-surface-color");
    }

    ccColor3B getUITextColor() {
        return Mod::get()->getSettingValue<ccColor3B>("ui-text-color");
    }

    bool getUIGlow() {
        return Mod::get()->getSettingValue<bool>("ui-glow");
    }

    float getUIPanelScale() {
        return static_cast<float>(Mod::get()->getSettingValue<double>("ui-panel-scale"));
    }

    constexpr float FONT_SCALE_BOOST = 0.70f;

    float getUIFontScale() {
        return static_cast<float>(Mod::get()->getSettingValue<double>("ui-font-scale")) * FONT_SCALE_BOOST;
    }

    std::filesystem::path macTerminalCommandPath() {
        return macRuntimeDir() / "dynamic-terminal.command";
    }

    std::filesystem::path macTerminalPidPath() {
        return macRuntimeDir() / "terminal.pid";
    }

    std::filesystem::path macTerminalClosePath() {
        return macRuntimeDir() / "close";
    }

    std::filesystem::path terminalEnabledFlagPath() {
        return wineRuntimeDir() / "terminal-enabled";
    }

    void writeTerminalFlags() {
        if (hostOS() != HostOS::Linux) {
            return;
        }
        ensureWineRuntimeDir();
        std::ofstream terminalEnabledFile(terminalEnabledFlagPath(), std::ios::trunc);
        if (terminalEnabledFile) {
            terminalEnabledFile << (isTerminalEnabled() ? "1\n" : "0\n");
        }
    }

    std::filesystem::path terminalLogPath() {
        if (hostOS() == HostOS::Linux) {
            return wineRuntimeDir() / "terminal.log";
        }
        if (hostOS() == HostOS::Mac) {
            return macRuntimeDir() / "terminal.log";
        }
        auto directory = tempDirectory();
#ifdef _WIN32
        auto fileName = g_windowsTerminalLogFileName;
#else
        std::string fileName = "dynamic-terminal.log";
#endif
        return directory.empty() ? std::filesystem::path(fileName) : directory / fileName;
    }

    std::filesystem::path windowsTerminalScriptPath() {
        auto directory = tempDirectory();
        return directory.empty() ? std::filesystem::path("dynamic-terminal.vbs") : directory / "dynamic-terminal.vbs";
    }

    std::filesystem::path windowsTerminalLegacyBatchPath() {
        auto directory = tempDirectory();
        return directory.empty() ? std::filesystem::path("dynamic-terminal.bat") : directory / "dynamic-terminal.bat";
    }

    std::filesystem::path windowsTerminalPowerShellPath() {
        auto directory = tempDirectory();
        return directory.empty() ? std::filesystem::path("dynamic-terminal.ps1") : directory / "dynamic-terminal.ps1";
    }

    std::filesystem::path windowsTerminalPidPath() {
        auto directory = tempDirectory();
        return directory.empty() ? std::filesystem::path("dynamic-terminal.pid") : directory / "dynamic-terminal.pid";
    }

    std::filesystem::path windowsTerminalClosePath() {
        auto directory = tempDirectory();
        return directory.empty() ? std::filesystem::path("dynamic-terminal.close") : directory / "dynamic-terminal.close";
    }

#ifdef _WIN32
    std::filesystem::path windowsTerminalErrorPath() {
        auto directory = tempDirectory();
        return directory.empty() ? std::filesystem::path("dynamic-terminal.error") : directory / "dynamic-terminal.error";
    }
#endif

    std::filesystem::path terminalScriptPath() {
        if (hostOS() == HostOS::Linux) {
            return wineRuntimeDir() / "terminal.sh";
        }
        if (hostOS() == HostOS::Mac) {
            return macTerminalCommandPath();
        }
        return windowsTerminalScriptPath();
    }

    std::filesystem::path cycleTriggerPath() {
        if (hostOS() == HostOS::Linux) {
            return wineRuntimeDir() / "cycle-stage";
        }
        if (hostOS() == HostOS::Mac) {
            return macRuntimeDir() / "cycle-stage";
        }
        auto directory = tempDirectory();
        return directory.empty() ? std::filesystem::path("dynamic-cycle-stage") : directory / "dynamic-cycle-stage";
    }

    void writeWineTerminalScript() {
        ensureWineRuntimeDir();
        std::ofstream outputFile(terminalScriptPath(), std::ios::trunc | std::ios::binary);
        if (!outputFile) {
            return;
        }
        outputFile << R"SH(#!/usr/bin/env bash
set -u
export PATH="/usr/local/bin:/usr/bin:/bin:${PATH:-}"
RUNDIR="/dev/shm/dynamic-$(id -un)"
if [ ! -d /dev/shm ] || [ ! -w /dev/shm ]; then
    RUNDIR="/tmp/dynamic-$(id -un)"
fi
mkdir -p "$RUNDIR" 2>/dev/null || true
chmod 700 "$RUNDIR" 2>/dev/null || true
LOG="$RUNDIR/terminal.log"
CLOSE="$RUNDIR/close"
PIDFILE="$RUNDIR/kitty.pid"
CYCLE="$RUNDIR/cycle-stage"
CYCLE_HELPER="$RUNDIR/cycle-helper.sh"
KITTY_CONF="$RUNDIR/kitty.conf"
CLASS="dynamic"

if [ "${1:-}" = "--inside" ]; then
    clear
    rm -f "$CLOSE"
    printf '\n\033[1;38;5;141mDynamic\033[0m  predictive input buffer\n\n'
    stty -echo -icanon min 1 time 0 2>/dev/null || true
    tail -n 12 -F "$LOG" &
    TAIL_PID=$!
    cleanup() {
        kill "$TAIL_PID" 2>/dev/null || true
        wait "$TAIL_PID" 2>/dev/null || true
        rm -f "$CLOSE"
        stty echo icanon 2>/dev/null || true
    }
    trap 'exit 0' INT TERM HUP
    trap cleanup EXIT
    while true; do
        if [ -e "$CLOSE" ]; then
            rm -f "$CLOSE"
            exit 0
        fi
        if ! IFS= read -rsn1 -t 0.25 ch; then
            continue
        fi
        if [ "$ch" = $'\e' ]; then
            seq="$ch"
            while IFS= read -rsn1 -t 0.15 c; do
                seq+="$c"
                case "$seq" in
                    $'\e[17~'|$'\e[17u'|$'\e[17;'*'~'|$'\e[17;'*'u')
                        printf 'cycle\n' > "$CYCLE"
                        break
                        ;;
                    $'\e[20~'|$'\e[20;'*'~'|$'\e[20u'|$'\e[20;'*'u')
                        exit 0
                        ;;
                esac
                [ "${#seq}" -ge 16 ] && break
            done
        fi
    done
fi

cd "$HOME" 2>/dev/null || cd /tmp

printf '#!/bin/sh\nprintf "cycle\\n" > "%s"\n' "$CYCLE" > "$CYCLE_HELPER" 2>/dev/null || true
chmod 700 "$CYCLE_HELPER" 2>/dev/null || true
: > "$KITTY_CONF" 2>/dev/null || true
KITTY_USER_CONF="${KITTY_CONFIG_DIRECTORY:-$HOME/.config/kitty}/kitty.conf"
if [ -f "$KITTY_USER_CONF" ]; then
    printf 'include %s\n' "$KITTY_USER_CONF" >> "$KITTY_CONF" 2>/dev/null || true
fi
printf 'map f6 launch --type=background /bin/sh %s\n' "$CYCLE_HELPER" >> "$KITTY_CONF" 2>/dev/null || true

raise_window() {
    command -v hyprctl >/dev/null 2>&1 || return 0
    local ws
    if command -v jq >/dev/null 2>&1; then
        ws="$(hyprctl activeworkspace -j 2>/dev/null | jq -r '.id // empty' | head -n 1)"
    fi
    if [ -z "${ws:-}" ]; then
        ws="$(hyprctl activeworkspace -j 2>/dev/null | sed -n 's/.*"id": *\([0-9][0-9]*\).*/\1/p' | head -n 1)"
    fi
    [ -n "${ws:-}" ] || ws=1
    local cls
    for cls in dynamic dynamic-nocapture; do
        hyprctl dispatch "hl.dsp.window.move({ workspace = $ws, window = \"class:$cls\" })" >/dev/null 2>&1 || true
        hyprctl dispatch "hl.dsp.window.center({ window = \"class:$cls\" })" >/dev/null 2>&1 || true
        hyprctl dispatch "hl.dsp.window.bring_to_top({ window = \"class:$cls\" })" >/dev/null 2>&1 || true
        hyprctl dispatch "hl.dsp.window.alter_zorder({ mode = \"top\", window = \"class:$cls\" })" >/dev/null 2>&1 || true
        hyprctl dispatch "hl.dsp.focus({ window = \"class:$cls\" })" >/dev/null 2>&1 || true
    done
}

terminal_running() {
    if command -v hyprctl >/dev/null 2>&1; then
        if hyprctl clients -j 2>/dev/null | grep -q '"class": "dynamic'; then
            return 0
        fi
    fi
    if [ -r "$PIDFILE" ]; then
        local pid
        pid="$(cat "$PIDFILE" 2>/dev/null || true)"
        if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
            local args
            args="$(ps -p "$pid" -o args= 2>/dev/null || true)"
            case "$args" in
                *--class*dynamic*|*dynamic-terminal.sh*) return 0 ;;
            esac
        fi
        rm -f "$PIDFILE"
    fi
    return 1
}

if terminal_running; then
    if command -v jq >/dev/null 2>&1; then
        active_ws="$(hyprctl activeworkspace -j 2>/dev/null | jq -r '.id // empty' | head -n 1)"
        active_addr="$(hyprctl activewindow -j 2>/dev/null | jq -r '.address // empty' | head -n 1)"
        terminal_info="$(hyprctl clients -j 2>/dev/null | jq -r '.[] | select(.class == "dynamic" or .class == "dynamic-nocapture") | "\(.address) \(.workspace.id)"' | head -n 1)"
        terminal_addr="${terminal_info%% *}"
        terminal_ws="${terminal_info##* }"
        if [ -n "${terminal_addr:-}" ] && { [ "$terminal_addr" != "${active_addr:-}" ] || [ "$terminal_ws" != "${active_ws:-}" ]; }; then
            raise_window
            exit 0
        fi
    fi
    : > "$CLOSE"
    exit 0
fi

KITTY="$(command -v kitty || true)"
if [ -z "$KITTY" ]; then
    echo "kitty not found on path" >"$RUNDIR/kitty.log"
    exit 1
fi
nohup "$KITTY" --config "$KITTY_CONF" --title "Dynamic" --class "$CLASS" --instance-group dynamic \
    -o confirm_os_window_close=0 -o close_on_child_death=yes \
    -o initial_window_width=100c -o initial_window_height=30c -o remember_window_size=no \
    /bin/bash "$0" --inside >"$RUNDIR/kitty.log" 2>&1 &
KITTY_PID=$!
printf '%s\n' "$KITTY_PID" > "$PIDFILE"
sleep 0.4

if command -v hyprctl >/dev/null 2>&1; then
    ( sleep 0.4; raise_window; sleep 0.7; raise_window ) &
fi
)SH";
    }

    void writeMacTerminalScript() {
        ensureMacRuntimeDir();
        std::ofstream outputFile(macTerminalCommandPath(), std::ios::trunc | std::ios::binary);
        if (!outputFile) {
            return;
        }

        std::string script = R"SH(#!/usr/bin/env bash
set -u

export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:${PATH:-}"

RUNDIR="__DYNAMIC_RUNDIR__"
LOG="$RUNDIR/terminal.log"
CLOSE="$RUNDIR/close"
PIDFILE="$RUNDIR/terminal.pid"
CYCLE="$RUNDIR/cycle-stage"

mkdir -p "$RUNDIR" 2>/dev/null || true
chmod 700 "$RUNDIR" 2>/dev/null || true
printf '%s\n' "$$" > "$PIDFILE"

clear
rm -f "$CLOSE"
printf '\033]0;Dynamic\007'
printf '\n\033[1;38;5;141mDynamic\033[0m  predictive input buffer\n\n'

stty -echo -icanon min 1 time 0 2>/dev/null || true
tail -n 12 -F "$LOG" &
TAIL_PID=$!

cleanup() {
    kill "$TAIL_PID" 2>/dev/null || true
    wait "$TAIL_PID" 2>/dev/null || true
    rm -f "$CLOSE" "$PIDFILE"
    stty echo icanon 2>/dev/null || true
    osascript -e 'tell application "Terminal" to close (every window whose name contains "Dynamic")' >/dev/null 2>&1 || true
}
trap 'exit 0' INT TERM HUP
trap cleanup EXIT

while true; do
    if [ -e "$CLOSE" ]; then
        rm -f "$CLOSE"
        exit 0
    fi
    if ! IFS= read -rsn1 -t 1 ch; then
        continue
    fi
    if [ "$ch" = $'\e' ]; then
        seq="$ch"
        while IFS= read -rsn1 -t 1 c; do
            seq+="$c"
            case "$seq" in
                $'\e[17~'|$'\e[17u'|$'\e[17;'*'~'|$'\e[17;'*'u')
                    printf 'cycle\n' > "$CYCLE"
                    break
                    ;;
                $'\e[20~'|$'\e[20;'*'~'|$'\e[20u'|$'\e[20;'*'u')
                    exit 0
                    ;;
            esac
            [ "${#seq}" -ge 16 ] && break
        done
    fi
done
)SH";

        std::string marker = "RUNDIR=\"__DYNAMIC_RUNDIR__\"";
        auto replacement = "RUNDIR=" + shellQuote(macRuntimeDir().string());
        auto markerPosition = script.find(marker);
        if (markerPosition != std::string::npos) {
            script.replace(markerPosition, marker.size(), replacement);
        }
        outputFile << script;
        outputFile.close();

        std::error_code errorCode;
        std::filesystem::permissions(
            macTerminalCommandPath(),
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace,
            errorCode
        );
    }

    void writeWindowsTerminalPowerShell() {
        std::ofstream outputFile(windowsTerminalPowerShellPath(), std::ios::trunc | std::ios::binary);
        if (!outputFile) {
            return;
        }
        std::string script = R"PS($ErrorActionPreference = 'Stop'

$log = Join-Path $PSScriptRoot 'dynamic-terminal.log'
$trigger = Join-Path $PSScriptRoot 'dynamic-cycle-stage'
$pidFile = Join-Path $PSScriptRoot 'dynamic-terminal.pid'
$closeFile = Join-Path $PSScriptRoot 'dynamic-terminal.close'
$errorFile = Join-Path $PSScriptRoot 'dynamic-terminal.error'
$script:lastCycleAt = [DateTime]::MinValue
$script:closing = $false
$script:triggerPaths = @($trigger)
if ($env:TEMP) {
    $script:triggerPaths += (Join-Path $env:TEMP 'dynamic-cycle-stage')
}
$script:triggerPaths = @($script:triggerPaths | Select-Object -Unique)

Remove-Item -LiteralPath $closeFile -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $errorFile -Force -ErrorAction SilentlyContinue
Set-Content -LiteralPath $pidFile -Value $PID -Encoding ASCII -NoNewline

$timer = $null
$form = $null
try {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing

    [System.Windows.Forms.Application]::EnableVisualStyles()

    $form = New-Object System.Windows.Forms.Form
    $form.Text = 'Dynamic'
    $form.StartPosition = [System.Windows.Forms.FormStartPosition]::CenterScreen
    $form.Size = New-Object System.Drawing.Size(1000, 700)
    $form.MinimumSize = New-Object System.Drawing.Size(480, 280)
    $form.BackColor = [System.Drawing.Color]::FromArgb(16, 17, 22)
    $form.ForeColor = [System.Drawing.Color]::FromArgb(230, 230, 235)
    $form.Font = New-Object System.Drawing.Font('Consolas', 11)
    $form.KeyPreview = $true
    $form.TopMost = $true
    $form.ShowInTaskbar = $true
    $form.WindowState = [System.Windows.Forms.FormWindowState]::Normal

    $text = New-Object System.Windows.Forms.RichTextBox
    $text.Dock = [System.Windows.Forms.DockStyle]::Fill
    $text.ReadOnly = $true
    $text.BorderStyle = [System.Windows.Forms.BorderStyle]::None
    $text.BackColor = [System.Drawing.Color]::FromArgb(16, 17, 22)
    $text.ForeColor = [System.Drawing.Color]::FromArgb(210, 235, 215)
    $text.Font = New-Object System.Drawing.Font('Consolas', 11)
    $text.WordWrap = $false
    $text.DetectUrls = $false
    $text.ScrollBars = [System.Windows.Forms.RichTextBoxScrollBars]::Vertical
    $text.TabStop = $true
    $form.Controls.Add($text)

    $text.Text = "Dynamic  predictive input buffer`r`n`r`n"

    $script:lastLength = 0
    $script:share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete

    $keyHandler = {
        param($sender, $eventArgs)
        if ($eventArgs.KeyCode -eq [System.Windows.Forms.Keys]::F6) {
            $eventArgs.SuppressKeyPress = $true
            $eventArgs.Handled = $true
            $now = [DateTime]::UtcNow
            if (($now - $script:lastCycleAt).TotalMilliseconds -ge 120) {
                $script:lastCycleAt = $now
                foreach ($path in $script:triggerPaths) {
                    try {
                        [System.IO.File]::WriteAllText($path, 'cycle', [System.Text.Encoding]::ASCII)
                    }
                    catch {
                    }
                }
            }
        }
        elseif ($eventArgs.KeyCode -eq [System.Windows.Forms.Keys]::F9) {
            $eventArgs.SuppressKeyPress = $true
            $eventArgs.Handled = $true
            if (-not $script:closing) {
                $script:closing = $true
                $form.Close()
            }
        }
    }

    $previewKeyHandler = {
        param($sender, $eventArgs)
        if ($eventArgs.KeyCode -eq [System.Windows.Forms.Keys]::F6 -or $eventArgs.KeyCode -eq [System.Windows.Forms.Keys]::F9) {
            $eventArgs.IsInputKey = $true
        }
    }

    $form.Add_KeyDown($keyHandler)
    $form.Add_PreviewKeyDown($previewKeyHandler)
    $text.Add_KeyDown($keyHandler)
    $text.Add_PreviewKeyDown($previewKeyHandler)

    $timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = 100
    $timer.Add_Tick({
        if (Test-Path -LiteralPath $closeFile) {
            Remove-Item -LiteralPath $closeFile -Force -ErrorAction SilentlyContinue
            $form.Close()
            return
        }

        if (-not (Test-Path -LiteralPath $log)) {
            return
        }

        $item = Get-Item -LiteralPath $log -ErrorAction SilentlyContinue
        if (-not $item) {
            return
        }
        if ($item.Length -lt $script:lastLength) {
            $script:lastLength = 0
        }
        if ($item.Length -eq $script:lastLength) {
            return
        }

        try {
            $stream = [System.IO.File]::Open($log, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $script:share)
        }
        catch {
            return
        }
        try {
            [void]$stream.Seek($script:lastLength, [System.IO.SeekOrigin]::Begin)
            $count = [int][Math]::Min([int64]($item.Length - $script:lastLength), [int64]65536)
            if ($count -le 0) {
                return
            }
            $buffer = New-Object byte[] $count
            $read = $stream.Read($buffer, 0, $count)
            if ($read -le 0) {
                return
            }
            $script:lastLength += $read
            $chunk = [System.Text.Encoding]::UTF8.GetString($buffer, 0, $read)
            if ($chunk.Length -gt 0) {
                $text.AppendText($chunk)
                $text.SelectionStart = $text.TextLength
                $text.ScrollToCaret()
            }
        }
        catch {
            return
        }
        finally {
            $stream.Dispose()
        }
    })
    $timer.Start()

    $form.Add_Shown({
        [void]$form.Activate()
        $form.BringToFront()
        [void]$text.Focus()
    })

    [System.Windows.Forms.Application]::Run($form)
}
catch {
    $_ | Out-String | Set-Content -LiteralPath $errorFile -Encoding UTF8 -ErrorAction SilentlyContinue
}
finally {
    if ($timer) {
        $timer.Stop()
    }
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $closeFile -Force -ErrorAction SilentlyContinue
}
)PS";

#ifdef _WIN32
        auto logName = "'" + g_windowsTerminalLogFileName + "'";
        auto logPosition = script.find("'dynamic-terminal.log'");
        if (logPosition != std::string::npos) {
            script.replace(logPosition, std::string("'dynamic-terminal.log'").size(), logName);
        }
#endif
        outputFile << script;
    }

    void writeWindowsTerminalScript() {
        writeWindowsTerminalPowerShell();

        std::error_code legacyErrorCode;
        std::filesystem::remove(windowsTerminalLegacyBatchPath(), legacyErrorCode);

        std::ofstream outputFile(windowsTerminalScriptPath(), std::ios::trunc | std::ios::binary);
        if (!outputFile) {
            return;
        }

        outputFile << "Set fso = CreateObject(\"Scripting.FileSystemObject\")\r\n"
                      "scriptDir = fso.GetParentFolderName(WScript.ScriptFullName)\r\n"
                      "ps = fso.BuildPath(fso.GetSpecialFolder(1).Path, \"WindowsPowerShell\\v1.0\\powershell.exe\")\r\n"
                      "q = Chr(34)\r\n"
                      "command = q & ps & q & \" -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File \" & q & scriptDir & \"\\dynamic-terminal.ps1\" & q\r\n"
                      "CreateObject(\"WScript.Shell\").Run command, 0, False\r\n";
    }

    void writeTerminalScript() {
        if (hostOS() == HostOS::Linux) {
            writeWineTerminalScript();
        }
        else if (hostOS() == HostOS::Mac) {
            writeMacTerminalScript();
        }
        else {
            writeWindowsTerminalScript();
        }
    }

#ifdef _WIN32
    void closeTerminalLog() {
        if (g_terminalLogHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(g_terminalLogHandle);
            g_terminalLogHandle = INVALID_HANDLE_VALUE;
        }
    }
#endif

    void removeLegacyTerminalLogs() {
        if (isMobileHost()) {
            return;
        }
        if (hostOS() == HostOS::Mac) {
            return;
        }
        if (hostOS() == HostOS::Windows) {
            auto directory = tempDirectory();
            if (!directory.empty()) {
                std::error_code errorCode;
                std::filesystem::remove(directory / "dynamic-terminal.log", errorCode);
            }
            return;
        }
        auto legacyRuntimeDirectory = std::filesystem::path("Z:\\tmp") / ("dynamic-" + wineUser());
        if (wineRuntimeDir() == legacyRuntimeDirectory) {
            return;
        }
        for (auto const* fileName : { "terminal.log", "watch.log", "kitty.log" }) {
            std::error_code errorCode;
            std::filesystem::remove(legacyRuntimeDirectory / fileName, errorCode);
        }
    }

#ifdef _WIN32
    void resetWindowsTerminalState() {
        std::error_code errorCode;
        std::filesystem::remove(windowsTerminalPidPath(), errorCode);
        std::filesystem::remove(windowsTerminalClosePath(), errorCode);
        std::filesystem::remove(windowsTerminalErrorPath(), errorCode);
    }
#endif

    void resetTerminalLog() {
        if (isMobileHost()) {
            return;
        }
        writeTerminalFlags();
        if (hostOS() == HostOS::Linux) {
            ensureWineRuntimeDir();
            auto logPath = terminalLogPath();
            std::ofstream outputFile(logPath, std::ios::trunc);
            outputFile.close();
            std::error_code errorCode;
            std::filesystem::permissions(
                logPath,
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                std::filesystem::perm_options::replace,
                errorCode
            );
        }
        else if (hostOS() == HostOS::Mac) {
            ensureMacRuntimeDir();
            auto logPath = terminalLogPath();
            std::ofstream outputFile(logPath, std::ios::trunc);
            outputFile.close();
            std::error_code errorCode;
            std::filesystem::permissions(
                logPath,
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                std::filesystem::perm_options::replace,
                errorCode
            );
        }
        else {
#ifdef _WIN32
            g_windowsTerminalLogFileName = fmt::format("dynamic-terminal-{}.log", GetCurrentProcessId());
            closeTerminalLog();
            resetWindowsTerminalState();
            auto logPath = terminalLogPath().wstring();
            g_terminalLogHandle = CreateFileW(
                logPath.c_str(),
                GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                nullptr
            );
            if (g_terminalLogHandle == INVALID_HANDLE_VALUE) {
                log::warn("could not create volatile terminal log (error {})", GetLastError());
            }
#endif
        }

        writeTerminalScript();
    }

    void pushRecentLog(std::string const& message) {
        std::lock_guard<std::mutex> lock(g_recentLogMutex);
        g_recentLog.push_back(message);
        while (g_recentLog.size() > RECENT_LOG_CAPACITY) {
            g_recentLog.pop_front();
        }
    }

    std::vector<std::string> recentLogSnapshot() {
        std::lock_guard<std::mutex> lock(g_recentLogMutex);
        return std::vector<std::string>(g_recentLog.begin(), g_recentLog.end());
    }

    void appendTerminalLog(std::string const& message) {
        pushRecentLog(message);
        if (isMobileHost()) {
            return;
        }
        if (hostOS() == HostOS::Linux || hostOS() == HostOS::Mac) {
            std::ofstream outputFile(terminalLogPath(), std::ios::app);
            if (outputFile) {
                outputFile << "[dynamic] " << message << '\n' << std::flush;
            }
            return;
        }

#ifdef _WIN32
        if (g_terminalLogHandle == INVALID_HANDLE_VALUE) {
            return;
        }
        auto line = "[dynamic] " + message + "\n";
        DWORD bytesWritten = 0;
        WriteFile(g_terminalLogHandle, line.data(), static_cast<DWORD>(line.size()), &bytesWritten, nullptr);
#endif
    }

    template <class... Args>
    void say(fmt::format_string<Args...> formatString, Args&&... arguments) {
        auto message = fmt::format(formatString, std::forward<Args>(arguments)...);
        appendTerminalLog(message);
    }

    // wave decisions are the ones that matter when the player reports deaths, and the panel
    // terminal is not always open. mirror just those lines into the Geode log so a later
    // session has evidence of the applied shift without a rebuild.
    template <class... Args>
    void sayWave(fmt::format_string<Args...> formatString, Args&&... arguments) {
        auto message = fmt::format(formatString, std::forward<Args>(arguments)...);
        appendTerminalLog(message);
        log::info("[wave] {}", message);
    }

    // ship decisions need the same on-disk evidence as wave decisions. the panel terminal is
    // not always open, and the whole point of this pass is to prove the ship is no longer
    // shifting inputs on a "you will die eventually" simulation.
    template <class... Args>
    void sayShip(fmt::format_string<Args...> formatString, Args&&... arguments) {
        auto message = fmt::format(formatString, std::forward<Args>(arguments)...);
        appendTerminalLog(message);
        log::info("[ship] {}", message);
    }

    // orb helper lines go to the same activity feed as the wave lines, but a play session is
    // only evidence if it survives on disk. mirror them into the Geode log as [orb].
    template <class... Args>
    void sayOrb(fmt::format_string<Args...> formatString, Args&&... arguments) {
        auto message = fmt::format(formatString, std::forward<Args>(arguments)...);
        appendTerminalLog(message);
        log::info("[orb] {}", message);
    }

    // a press can be skipped by the orb helper for several reasons (wave, ship, grounded,
    // pills off) or run but find nothing worth shifting. those are throttled so the log stays
    // readable while still proving which side of the gate each press landed on.
    int64_t g_orbPressLogMs = 0;
    int64_t g_orbSkipLogMs = 0;
    int64_t g_orbProbeLogMs = 0;
    int64_t g_orbRetimeLogMs = 0;

    void logOrbPress(int windowTicks, int holdTicks, int horizonTicks) {
        int64_t const now = scanNowMs();
        if (now - g_orbPressLogMs < 500) {
            return;
        }
        g_orbPressLogMs = now;
        log::info("[orb] press: helper running (window {}t, hold {}t, horizon {}t)", windowTicks, holdTicks, horizonTicks);
    }

    void logOrbSkip(char const* reason) {
        int64_t const now = scanNowMs();
        if (now - g_orbSkipLogMs < 3000) {
            return;
        }
        g_orbSkipLogMs = now;
        log::info("[orb] press skipped: {}", reason);
    }

    std::string keybindList(std::string_view settingName) {
        auto keybinds = Mod::get()->getSettingValue<std::vector<Keybind>>(settingName);
        if (keybinds.empty()) {
            return "unbound";
        }
        std::string outputText;
        for (auto const& keybind : keybinds) {
            if (!outputText.empty()) {
                outputText += " / ";
            }
            outputText += keybind.toString();
        }
        return outputText;
    }

    std::string lowercaseText(std::string text) {
        for (auto& character : text) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        return text;
    }

    std::string stageKey() {
        return lowercaseText(keybindList(KEYBIND_STAGE));
    }

    std::string panelKey() {
        return lowercaseText(keybindList(KEYBIND_TERMINAL));
    }

    void finishKeybindIntro() {
        Mod::get()->setSavedValue(KEYBIND_INTRO_DONE, true);
        (void)Mod::get()->saveData();
    }

    bool showKeybindIntro() {
        if (Mod::get()->getSavedValue<bool>(KEYBIND_INTRO_DONE, false)) {
            return false;
        }
        auto* menuScene = CCScene::get();
        if (!menuScene || !menuScene->getChildByType<MenuLayer>(0)) {
            return false;
        }

        if (menuScene->getChildByType<FLAlertLayer>(0)) {
            static bool isWaiting = false;
            static int attemptCount = 0;
            if (!isWaiting && attemptCount < 20) {
                isWaiting = true;
                ++attemptCount;
                menuScene->runAction(CCSequence::create(
                    CCDelayTime::create(0.75f),
                    CallFuncExt::create([] {
                        isWaiting = false;
                        showKeybindIntro();
                    }),
                    nullptr
                ));
            }
            return false;
        }

        std::string contentText;
        if (isMobileHost()) {
            contentText =
                "Tap the floating Dynamic logo to open the control deck.\n\n"
                "Use the stage arrows or numbered segments to change stages.\n\n"
                "Drag the Dynamic logo anywhere to move it.";
        }
        else {
            contentText = fmt::format(
                "Dynamic uses Geode keybinds, so both controls can be changed.\n\n"
                "{}  open control deck\n{}  cycle stage\n\n"
                "You can set these in Geode's mod settings or keybind popup.",
                panelKey(),
                stageKey()
            );
        }
        createQuickPopup(
            "Dynamic controls",
            contentText,
            "Defaults are fine",
            "Open settings",
            [](FLAlertLayer*, bool openSettingsOnStart) {
                finishKeybindIntro();
                if (openSettingsOnStart) {
                    Loader::get()->queueInMainThread([] {
                        openSettingsPopup(Mod::get());
                    });
                }
            }
        );
        return true;
    }

    void setupKeybinds() {
        listenForKeybindSettingPresses(KEYBIND_STAGE, [](Keybind const&, bool isDown, bool isRepeat, double) {
            if (isDown && !isRepeat) {
                cycleStage(1);
            }
            return true;
        });
        listenForKeybindSettingPresses(KEYBIND_TERMINAL, [](Keybind const&, bool isDown, bool isRepeat, double) {
            if (isDown && !isRepeat) {
                toggleDynamicPanel();
            }
            return true;
        });
    }

    constexpr char const* VERSION_URL =
        "https://raw.githubusercontent.com/russian-vinny/dynamic/main/version.txt";
    constexpr char const* RELEASE_URL_PREFIX =
        "https://github.com/russian-vinny/dynamic/releases/tag/";
    constexpr char const* UPDATE_ASSET_URL =
        "https://github.com/russian-vinny/dynamic/releases/latest/download/dynamic.geode";

    std::string cleanVersion(std::string versionText) {
        auto isWhitespace = [](char character) {
            return std::isspace(static_cast<unsigned char>(character)) != 0;
        };
        while (!versionText.empty() && isWhitespace(versionText.front())) {
            versionText.erase(versionText.begin());
        }
        while (!versionText.empty() && isWhitespace(versionText.back())) {
            versionText.pop_back();
        }
        versionText = versionText.substr(0, versionText.find_first_of(" \t\r\n"));
        if (!versionText.empty() && (versionText.front() == 'v' || versionText.front() == 'V')) {
            versionText.erase(versionText.begin());
        }
        for (auto& character : versionText) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        return versionText;
    }

    bool isNewerVersion(std::string const& latestVersion, std::string const& installedVersion) {
        auto parsedLatestVersion = VersionInfo::parse(latestVersion);
        auto parsedInstalledVersion = VersionInfo::parse(installedVersion);
        if (!parsedLatestVersion.isOk() || !parsedInstalledVersion.isOk()) {
            return latestVersion != installedVersion;
        }
        return parsedInstalledVersion.unwrap() < parsedLatestVersion.unwrap();
    }

    constexpr float UPDATE_PANEL_WIDTH = 520.f;
    constexpr float UPDATE_PANEL_HEIGHT = 156.f;
    constexpr float UPDATE_BAR_WIDTH = 420.f;
    constexpr float UPDATE_BAR_HEIGHT = 16.f;
    constexpr float UPDATE_BAR_Y = -18.f;

    class UpdateOverlayLayer : public cocos2d::CCLayerColor {
    public:
        static UpdateOverlayLayer* create() {
            auto layer = new UpdateOverlayLayer();
            if (layer && layer->init()) {
                layer->autorelease();
                return layer;
            }
            CC_SAFE_DELETE(layer);
            return nullptr;
        }

        bool init() override {
            if (!CCLayerColor::initWithColor(ccc4(6, 8, 12, 210))) {
                return false;
            }
            this->setTouchEnabled(true);
            this->setKeypadEnabled(true);
            return true;
        }

        void registerWithTouchDispatcher() override {
            cocos2d::CCTouchDispatcher::get()->addTargetedDelegate(this, -500, true);
        }
        bool ccTouchBegan(cocos2d::CCTouch*, cocos2d::CCEvent*) override { return true; }
        void ccTouchMoved(cocos2d::CCTouch*, cocos2d::CCEvent*) override {}
        void ccTouchEnded(cocos2d::CCTouch*, cocos2d::CCEvent*) override {}
        void ccTouchCancelled(cocos2d::CCTouch*, cocos2d::CCEvent*) override {}
        void keyBackClicked() override {}
    };

    struct UpdateOverlayState {
        UpdateOverlayLayer* layer = nullptr;
        cocos2d::CCLabelBMFont* statusLabel = nullptr;
        cocos2d::CCLabelBMFont* hintLabel = nullptr;
        cocos2d::CCDrawNode* barFill = nullptr;
        bool isActive = false;
        std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    } updateOverlay;

    void setUpdateOverlayText(std::string const& text) {
        if (updateOverlay.statusLabel) {
            updateOverlay.statusLabel->setString(text.c_str());
        }
    }

    void setUpdateBar(float fraction) {
        if (!updateOverlay.barFill) {
            return;
        }
        float clamped = std::max(0.f, std::min(1.f, fraction));
        updateOverlay.barFill->clear();
        float filledWidth = UPDATE_BAR_WIDTH * clamped;
        if (filledWidth > 0.5f) {
            updateOverlay.barFill->drawRect(
                cocos2d::CCRect(0.f, -UPDATE_BAR_HEIGHT * 0.5f, filledWidth, UPDATE_BAR_HEIGHT),
                ccc4f(0.52f, 0.84f, 0.72f, 1.f),
                0.f,
                ccc4f(0.f, 0.f, 0.f, 0.f)
            );
        }
    }

    void hideUpdateOverlay() {
        updateOverlay.isActive = false;
        if (updateOverlay.layer) {
            updateOverlay.layer->removeFromParentAndCleanup(true);
        }
        updateOverlay.layer = nullptr;
        updateOverlay.statusLabel = nullptr;
        updateOverlay.hintLabel = nullptr;
        updateOverlay.barFill = nullptr;
    }

    void failUpdateDownload(std::string const& message) {
        hideUpdateOverlay();
        log::warn("dynamic update failed: {}", message);
        FLAlertLayer::create("update failed", message.c_str(), "ok")->show();
    }

    void showUpdateOverlay() {
        auto* scene = cocos2d::CCScene::get();
        if (!scene || typeinfo_cast<CCTransitionScene*>(scene) || updateOverlay.layer) {
            return;
        }
        auto* layer = UpdateOverlayLayer::create();
        if (!layer) {
            return;
        }
        updateOverlay.layer = layer;

        auto winSize = cocos2d::CCDirector::get()->getWinSize();
        layer->setContentSize(winSize);
        layer->ignoreAnchorPointForPosition(false);
        layer->setAnchorPoint(CCPoint(0.f, 0.f));
        layer->setPosition(CCPoint(0.f, 0.f));
        scene->addChild(layer, 9999);

        auto* root = cocos2d::CCNode::create();
        root->setPosition(CCPoint(winSize.width * 0.5f, winSize.height * 0.5f));
        layer->addChild(root, 1);

        if (auto* panel = cocos2d::CCDrawNode::create()) {
            panel->drawRect(
                cocos2d::CCRect(
                    -UPDATE_PANEL_WIDTH * 0.5f,
                    -UPDATE_PANEL_HEIGHT * 0.5f,
                    UPDATE_PANEL_WIDTH,
                    UPDATE_PANEL_HEIGHT
                ),
                ccc4f(0.09f, 0.11f, 0.15f, 0.97f),
                0.f,
                ccc4f(0.f, 0.f, 0.f, 0.f)
            );
            root->addChild(panel, 1);
        }

        auto* title = cocos2d::CCLabelBMFont::create("updating dynamic", "bigFont.fnt");
        if (title) {
            title->setScale(0.62f);
            title->setPosition(CCPoint(0.f, 46.f));
            root->addChild(title, 2);
        }

        updateOverlay.statusLabel = cocos2d::CCLabelBMFont::create("Preparing download...", "bigFont.fnt");
        if (updateOverlay.statusLabel) {
            updateOverlay.statusLabel->setScale(0.44f);
            updateOverlay.statusLabel->setPosition(CCPoint(0.f, 8.f));
            root->addChild(updateOverlay.statusLabel, 2);
        }

        if (auto* track = cocos2d::CCDrawNode::create()) {
            track->drawRect(
                cocos2d::CCRect(-UPDATE_BAR_WIDTH * 0.5f, -UPDATE_BAR_HEIGHT * 0.5f, UPDATE_BAR_WIDTH, UPDATE_BAR_HEIGHT),
                ccc4f(1.f, 1.f, 1.f, 0.12f),
                0.f,
                ccc4f(0.f, 0.f, 0.f, 0.f)
            );
            track->setPosition(CCPoint(0.f, UPDATE_BAR_Y));
            root->addChild(track, 2);
        }

        updateOverlay.barFill = cocos2d::CCDrawNode::create();
        if (updateOverlay.barFill) {
            updateOverlay.barFill->setPosition(CCPoint(-UPDATE_BAR_WIDTH * 0.5f, UPDATE_BAR_Y));
            root->addChild(updateOverlay.barFill, 3);
        }

        updateOverlay.hintLabel = cocos2d::CCLabelBMFont::create("do not close the game", "goldFont.fnt");
        if (updateOverlay.hintLabel) {
            updateOverlay.hintLabel->setScale(0.46f);
            updateOverlay.hintLabel->setPosition(CCPoint(0.f, -52.f));
            root->addChild(updateOverlay.hintLabel, 2);
        }

        setUpdateBar(0.f);
    }

    void updateUpdateOverlay(size_t downloaded, size_t total) {
        if (!updateOverlay.isActive) {
            return;
        }
        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - updateOverlay.startTime
        ).count();
        double downloadedMb = static_cast<double>(downloaded) / (1024.0 * 1024.0);
        double totalMb = static_cast<double>(total) / (1024.0 * 1024.0);
        double speed = elapsed > 0.25 ? downloadedMb / elapsed : 0.0;
        float percent = total > 0
            ? static_cast<float>(downloaded) * 100.f / static_cast<float>(total)
            : 0.f;

        setUpdateOverlayText(fmt::format(
            "Downloading: {:.0f}% ({:.1f} MB / {:.1f} MB) @ {:.1f} MB/s",
            percent,
            downloadedMb,
            totalMb,
            speed
        ));
        setUpdateBar(total > 0
            ? static_cast<float>(downloaded) / static_cast<float>(total)
            : 0.f);
    }

    void startUpdateDownload(std::string const& latestVersion) {
        if (updateOverlay.isActive) {
            return;
        }
        updateOverlay.isActive = true;
        updateOverlay.startTime = std::chrono::steady_clock::now();
        showUpdateOverlay();

        std::string assetUrl = UPDATE_ASSET_URL;
        say("downloading update from {} (target v{})", assetUrl, latestVersion);

        auto request = web::WebRequest();
        request.timeout(std::chrono::seconds(120));
        request.onProgress([](web::WebProgress const& progress) {
            size_t downloaded = progress.downloaded();
            size_t total = progress.downloadTotal();
            geode::queueInMainThread([downloaded, total] {
                updateUpdateOverlay(downloaded, total);
            });
        });

        async::spawn(
            request.get(assetUrl),
            [](web::WebResponse response) {
                if (!response.ok()) {
                    int code = response.code();
                    geode::queueInMainThread([code] {
                        failUpdateDownload(fmt::format("download failed (http {})", code));
                    });
                    return;
                }

                ByteVector bytes = response.data();
                if (bytes.empty()) {
                    geode::queueInMainThread([] {
                        failUpdateDownload("download failed: empty response");
                    });
                    return;
                }

                geode::queueInMainThread([bytes] {
                    setUpdateOverlayText("Installing... Please wait.");
                    setUpdateBar(1.f);

                    auto packagePath = Mod::get()->getPackagePath();
                    auto writeResult = geode::utils::file::writeBinarySafe(packagePath, bytes);
                    if (!writeResult.isOk()) {
                        failUpdateDownload(fmt::format("could not write update file: {}", writeResult.unwrapErr()));
                        return;
                    }

                    setUpdateOverlayText("Update complete! Restarting in 3 seconds...");
                    if (updateOverlay.hintLabel) {
                        updateOverlay.hintLabel->setString("the game will reopen");
                    }
                    if (updateOverlay.layer) {
                        updateOverlay.layer->runAction(CCSequence::create(
                            cocos2d::CCDelayTime::create(3.f),
                            CallFuncExt::create([] {
                                geode::utils::game::restart(true);
                            }),
                            nullptr
                        ));
                    }
                });
            }
        );
    }

    struct UpdateNotice {
        bool isShown = false;
        std::string urlText;
        std::string installedVersion;
        std::string latestVersion;
    } pendingUpdateNotice;

    bool showUpdatePopup() {
        if (pendingUpdateNotice.isShown || pendingUpdateNotice.urlText.empty()) {
            return false;
        }
        auto* menuScene = CCScene::get();
        if (!menuScene || !menuScene->getChildByType<MenuLayer>(0)) {
            return false;
        }
        pendingUpdateNotice.isShown = true;
        auto latestVersion = pendingUpdateNotice.latestVersion;
        createQuickPopup(
            "dynamic out of date",
            fmt::format("v{} installed, latest is {}", pendingUpdateNotice.installedVersion, pendingUpdateNotice.latestVersion),
            "dismiss", "update",
            [latestVersion](FLAlertLayer*, bool isUpdate) {
                if (isUpdate) {
                    startUpdateDownload(latestVersion);
                }
                if (auto* menuScene = CCScene::get()) {
                    menuScene->runAction(CCSequence::create(
                        CCDelayTime::create(0.3f),
                        CallFuncExt::create([] { showKeybindIntro(); }),
                        nullptr
                    ));
                }
            }
        );
        return true;
    }

    std::string tagUrlFor(std::string const& version, bool withPrefix) {
        return fmt::format("{}{}{}", RELEASE_URL_PREFIX, withPrefix ? "v" : "", version);
    }

    bool tagPageExists(web::WebResponse const& response) {
        auto code = response.code();
        return code != 0 && code != 404;
    }

    void announceUpdate(bool isLoud, std::string const& installedVersion, std::string const& latestVersion) {
        auto updateUrlText = fmt::format("{}{}", RELEASE_URL_PREFIX, latestVersion);
        say("out of date: v{} installed, latest is {}", installedVersion, latestVersion);
        say("update at {}", updateUrlText);
        log::warn("dynamic is out of date: v{} installed, latest is {}", installedVersion, latestVersion);
        log::warn("update at {}", updateUrlText);

        pendingUpdateNotice.installedVersion = installedVersion;
        pendingUpdateNotice.latestVersion = latestVersion;
        pendingUpdateNotice.urlText = updateUrlText;
        showUpdatePopup();
    }

    void checkUpdateTag(bool isLoud, std::string const& installedVersion, std::string const& latestVersion) {
        async::spawn(
            web::WebRequest().get(tagUrlFor(latestVersion, false)),
            [isLoud, installedVersion, latestVersion](web::WebResponse plainResponse) {
                if (tagPageExists(plainResponse)) {
                    announceUpdate(isLoud, installedVersion, latestVersion);
                    return;
                }
                async::spawn(
                    web::WebRequest().get(tagUrlFor(latestVersion, true)),
                    [isLoud, installedVersion, latestVersion](web::WebResponse prefixedResponse) {
                        if (tagPageExists(prefixedResponse)) {
                            announceUpdate(isLoud, installedVersion, latestVersion);
                            return;
                        }
                        if (isLoud) {
                            say("version check: v{} is in version.txt but the tag is not pushed yet, not showing an update", latestVersion);
                        }
                        log::info("dynamic update held back, no tag for v{} yet", latestVersion);
                    }
                );
            }
        );
    }

    void checkForUpdates(bool isLoud) {
        static bool hasChecked = false;
        static auto lastCheckTime = std::chrono::steady_clock::now() - std::chrono::minutes(11);
        auto currentTime = std::chrono::steady_clock::now();
        if (hasChecked && currentTime - lastCheckTime < std::chrono::minutes(10)) {
            return;
        }
        hasChecked = true;
        lastCheckTime = currentTime;

        auto installedVersion = cleanVersion(Mod::get()->getVersion().toVString());
        async::spawn(
            web::WebRequest().get(VERSION_URL),
            [isLoud, installedVersion](web::WebResponse webResponse) {
                if (!webResponse.ok()) {
                    // The Dynamic update repo is not published yet. A missing endpoint
                    // is not a mod error, so keep it out of the in-game log.
                    log::info("dynamic version check skipped (http {}), no published update endpoint", webResponse.code());
                    return;
                }

                auto responseBody = webResponse.string();
                if (!responseBody.isOk()) {
                    log::info("dynamic version check skipped: empty response");
                    return;
                }

                auto latestVersion = cleanVersion(responseBody.unwrap());
                if (latestVersion.empty()) {
                    log::info("dynamic version check skipped: version.txt is empty");
                    return;
                }

                if (!isNewerVersion(latestVersion, installedVersion)) {
                    if (isLoud) {
                        say("version check: up to date (v{})", installedVersion);
                    }
                    return;
                }

                checkUpdateTag(isLoud, installedVersion, latestVersion);
            }
        );
    }

#ifdef _WIN32

    std::filesystem::path windowsPowerShellExecutablePath() {
        wchar_t systemDirectory[MAX_PATH] = {};
        UINT systemDirectoryLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (systemDirectoryLength > 0 && systemDirectoryLength < MAX_PATH) {
            auto powerShellPath = std::filesystem::path(systemDirectory) / "WindowsPowerShell" / "v1.0" / "powershell.exe";
            std::error_code errorCode;
            if (std::filesystem::exists(powerShellPath, errorCode)) {
                return powerShellPath;
            }
        }

        for (auto const* executableName : { L"powershell.exe", L"pwsh.exe" }) {
            wchar_t foundPath[MAX_PATH] = {};
            DWORD foundLength = SearchPathW(nullptr, executableName, nullptr, MAX_PATH, foundPath, nullptr);
            if (foundLength > 0 && foundLength < MAX_PATH) {
                return std::filesystem::path(foundPath);
            }
        }

        return {};
    }

    void bringWindowsTerminalToFront(HWND terminalWindow) {
        if (!terminalWindow) {
            return;
        }
        if (IsIconic(terminalWindow)) {
            ShowWindow(terminalWindow, SW_RESTORE);
        }
        else {
            ShowWindow(terminalWindow, SW_SHOW);
        }
        SetWindowPos(
            terminalWindow,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
        );
        SetForegroundWindow(terminalWindow);
    }

    void focusWindowsTerminalWhenReady(DWORD processId) {
        std::thread([processId] {
            for (int attempt = 0; attempt < 50; ++attempt) {
                Sleep(100);
                HWND terminalWindow = FindWindowW(nullptr, L"Dynamic");
                if (!terminalWindow) {
                    continue;
                }
                DWORD windowProcessId = 0;
                GetWindowThreadProcessId(terminalWindow, &windowProcessId);
                if (windowProcessId == processId) {
                    bringWindowsTerminalToFront(terminalWindow);
                    return;
                }
            }
        }).detach();
    }

    void monitorWindowsTerminalProcess(DWORD processId) {
        std::thread([processId] {
            HANDLE processHandle = OpenProcess(
                SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId
            );
            if (!processHandle) {
                return;
            }
            DWORD waitResult = WaitForSingleObject(processHandle, 5000);
            CloseHandle(processHandle);
            if (waitResult != WAIT_OBJECT_0) {
                return;
            }

            std::ifstream errorFile(windowsTerminalErrorPath());
            std::string errorLine;
            if (!std::getline(errorFile, errorLine) || errorLine.empty()) {
                return;
            }
            log::warn("terminal process exited: {}", errorLine);
            geode::queueInMainThread([errorLine] {
                say("terminal failed to open: {}", errorLine);
            });
        }).detach();
    }

    bool launchWindowsTerminal() {
        writeWindowsTerminalPowerShell();

        auto powerShellPath = windowsPowerShellExecutablePath();
        if (powerShellPath.empty()) {
            log::warn("could not find powershell.exe or pwsh.exe");
            return false;
        }

        auto scriptPath = windowsTerminalPowerShellPath();
        std::error_code errorCode;
        if (!std::filesystem::exists(scriptPath, errorCode)) {
            log::warn("terminal script was not created: {}", scriptPath.string());
            return false;
        }

        auto commandLine = L"\"" + powerShellPath.wstring()
            + L"\" -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File \""
            + scriptPath.wstring() + L"\"";
        auto workingDirectory = scriptPath.parent_path().wstring();

        STARTUPINFOW startupInfo = {};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo = {};
        BOOL created = CreateProcessW(
            powerShellPath.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            &startupInfo,
            &processInfo
        );
        if (!created) {
            log::warn("could not start powershell terminal (error {})", GetLastError());
            return false;
        }

        {
            std::ofstream pidFile(windowsTerminalPidPath(), std::ios::trunc);
            if (pidFile) {
                pidFile << processInfo.dwProcessId;
            }
        }

        log::info("started terminal process {}", processInfo.dwProcessId);
        focusWindowsTerminalWhenReady(processInfo.dwProcessId);
        monitorWindowsTerminalProcess(processInfo.dwProcessId);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        return true;
    }

    bool launchWindowsTerminalWithWScript() {
        writeWindowsTerminalScript();

        auto scriptPath = windowsTerminalScriptPath();
        std::error_code errorCode;
        if (!std::filesystem::exists(scriptPath, errorCode)) {
            log::warn("terminal launcher was not created: {}", scriptPath.string());
            return false;
        }

        auto workingDirectory = scriptPath.parent_path().wstring();
        auto parameters = L"\"" + scriptPath.wstring() + L"\"";
        auto shellResult = ShellExecuteW(
            nullptr,
            L"open",
            L"wscript.exe",
            parameters.c_str(),
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            SW_HIDE
        );
        auto shellResultCode = reinterpret_cast<INT_PTR>(shellResult);
        if (shellResultCode <= 32) {
            log::warn("could not open terminal launcher (shell execute returned {})", shellResultCode);
            return false;
        }
        return true;
    }

    bool windowsTerminalRunning() {
        std::ifstream inputFile(windowsTerminalPidPath());
        unsigned long processId = 0;
        if (!(inputFile >> processId) || processId == 0) {
            return false;
        }

        HANDLE processHandle = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(processId)
        );
        if (!processHandle) {
            std::error_code processErrorCode;
            std::filesystem::remove(windowsTerminalPidPath(), processErrorCode);
            return false;
        }

        DWORD waitResult = WaitForSingleObject(processHandle, 0);
        wchar_t imagePath[MAX_PATH] = {};
        DWORD imagePathLength = MAX_PATH;
        bool hasImagePath = QueryFullProcessImageNameW(processHandle, 0, imagePath, &imagePathLength) != 0;
        CloseHandle(processHandle);

        std::wstring processName(imagePath, hasImagePath ? imagePathLength : 0);
        for (auto& character : processName) {
            character = static_cast<wchar_t>(std::towlower(character));
        }
        bool isPowerShellProcess = processName.find(L"powershell.exe") != std::wstring::npos
            || processName.find(L"pwsh.exe") != std::wstring::npos;
        if (waitResult == WAIT_TIMEOUT && isPowerShellProcess) {
            return true;
        }

        std::error_code cleanupErrorCode;
        std::filesystem::remove(windowsTerminalPidPath(), cleanupErrorCode);
        return false;
    }
#endif

#ifdef GEODE_IS_MACOS
    bool macTerminalRunning() {
        std::ifstream inputFile(macTerminalPidPath());
        long processId = 0;
        if (!(inputFile >> processId) || processId <= 0) {
            return false;
        }
        if (kill(static_cast<pid_t>(processId), 0) == 0) {
            return true;
        }

        std::error_code errorCode;
        std::filesystem::remove(macTerminalPidPath(), errorCode);
        return false;
    }
#endif

    void closeTerminal() {
        if (isMobileHost()) {
            return;
        }
        if (hostOS() == HostOS::Linux) {
            ensureWineRuntimeDir();
            auto closePath = wineRuntimeDir() / "close";
            std::ofstream closeFile(closePath, std::ios::trunc);
            if (closeFile) {
                closeFile << "close\n";
            }
            else {
                log::warn("could not write terminal close trigger {}", closePath.string());
            }

            std::error_code errorCode;
            std::filesystem::remove(wineRuntimeDir() / "open-kitty", errorCode);
            return;
        }

        if (hostOS() == HostOS::Mac) {
#ifdef GEODE_IS_MACOS
            if (!macTerminalRunning()) {
                return;
            }
            std::ofstream closeFile(macTerminalClosePath(), std::ios::trunc);
            if (closeFile) {
                closeFile << "close\n";
            }
            else {
                log::warn("could not write terminal close trigger");
            }
#endif
            return;
        }

#ifdef _WIN32
        if (windowsTerminalRunning()) {
            std::ofstream closeFile(windowsTerminalClosePath(), std::ios::trunc);
            if (closeFile) {
                closeFile << "close\n";
            }
            else {
                log::warn("could not write terminal close trigger");
            }
        }

        if (HWND terminalWindow = FindWindowW(nullptr, L"Dynamic")) {
            PostMessageW(terminalWindow, WM_CLOSE, 0, 0);
        }
#endif
    }

    void toggleTerminal() {
        if (!isTerminalEnabled()) {
            log::info("terminal toggle ignored (disabled)");
            return;
        }

        std::error_code errorCode;
        if (!std::filesystem::exists(terminalScriptPath(), errorCode)) {
            writeTerminalScript();
        }

        if (hostOS() == HostOS::Linux) {
            ensureWineRuntimeDir();
            std::error_code cleanupErrorCode;
            std::filesystem::remove(wineRuntimeDir() / "close", cleanupErrorCode);

            auto openTriggerPath = wineRuntimeDir() / "open-kitty";
            std::ofstream triggerFile(openTriggerPath, std::ios::trunc);
            if (triggerFile) {
                triggerFile << "toggle\n";
                log::info("wrote kitty trigger {}", openTriggerPath.string());
            }
            else {
                log::warn("could not write kitty trigger {}", openTriggerPath.string());
            }
            return;
        }

        if (hostOS() == HostOS::Mac) {
#ifdef GEODE_IS_MACOS
            ensureMacRuntimeDir();
            if (macTerminalRunning()) {
                std::ofstream closeFile(macTerminalClosePath(), std::ios::trunc);
                if (closeFile) {
                    closeFile << "close\n";
                }
                else {
                    log::warn("could not write terminal close trigger");
                }
                return;
            }

            auto command = "/usr/bin/open -a Terminal " + shellQuote(terminalScriptPath().string());
            int openResult = std::system(command.c_str());
            if (openResult != 0) {
                log::warn("could not open terminal launcher (open returned {})", openResult);
            }
#else
            log::warn("mac terminal is only available on macos");
#endif
            return;
        }

#ifdef _WIN32
        if (HWND terminalWindow = FindWindowW(nullptr, L"Dynamic")) {
            if (GetForegroundWindow() != terminalWindow) {
                bringWindowsTerminalToFront(terminalWindow);
                return;
            }
            closeTerminal();
            return;
        }

        if (windowsTerminalRunning()) {
            closeTerminal();
            return;
        }

        if (!launchWindowsTerminal() && !launchWindowsTerminalWithWScript()) {
            say("terminal failed to open");
        }
#else
        log::warn("terminal is not available on this platform");
#endif
    }

    bool isGhost(PlayerObject* playerObject) {
        return playerObject && (playerObject == state.ghostPlayer1 || playerObject == state.ghostPlayer2);
    }

    void resetPending() {
        state.queuedInputs.clear();
        state.inputShiftTicks = 0;
        state.releaseShiftTicks = 0;
        state.pressShiftTicks = 0;
        state.queuedPressTick = -1;
        state.queuedReleaseTick = -1;
    }

    void readSettings() {
        state.smartEnabled = true;
        state.budgetMilliseconds = 8.0;
        state.automaticBudget = false;
        state.practiceModeOnly = false;
        state.groundHorizonTicks = 130;
        state.waveHorizonTicks = 65;
        state.assumeOrbHits = true;
        state.orbClickEnabled = true;
        state.tryBothOrbCandidates = true;
        state.avoidMovingObjects = false;
        state.autoReleaseEnabled = false;
        state.autoReleaseWindowTicks = 6;
        state.driftCheckEnabled = false;
        state.orbAlignEnabled = true;
        state.orbTimingEnabled = true;
        state.orbAlignToGround = false;
        state.fullSimulationState = true;
        state.robotSmartEnabled = true;
        state.spiderSmartEnabled = true;
        state.ufoSmartEnabled = true;
        state.ballSmartEnabled = true;
        state.swingSmartEnabled = true;
        state.trainingMode = false;
        state.perLevelStats = false;
        state.heatmapEnabled = false;
        state.moreTimeEnabled = false;
        state.dashAutoReleaseEnabled = false;
        state.performanceLogEnabled = false;
        state.shipBufferEnabled = true;
        state.lateCrashTicks = 90;
        state.robustSaveMode = false;
        state.ignoreTouchingObjects = true;
        state.deathLogEnabled = false;
        state.explainEnabled = false;
        state.lowLatencyMode = false;
        state.predictMovingObjects = false;
        state.shipMaxDelayTicks = 4;
        state.shipStraightEnabled = Mod::get()->getSavedValue<bool>("ship-straight", true);
        state.shipStraightStrength = std::clamp(Mod::get()->getSavedValue<int>("ship-straight-strength", SHIP_STRAIGHT_STRENGTH_DEFAULT),
            SHIP_STRAIGHT_STRENGTH_MIN, SHIP_STRAIGHT_STRENGTH_MAX);
        state.shipStraightSensitivity = std::clamp(Mod::get()->getSavedValue<int>("ship-straight-sensitivity", SHIP_STRAIGHT_SENSITIVITY_DEFAULT), 0, 100);
        state.shipStraightRhythm = std::clamp(Mod::get()->getSavedValue<int>("ship-straight-rhythm", SHIP_STRAIGHT_RHYTHM_DEFAULT), 0, 100);
        // migrate the old L1..L4 save once: the level keys are only read when the dial keys are absent.
        if (Mod::get()->hasSavedValue("ship-straight-level") && !Mod::get()->hasSavedValue("ship-straight-strength")) {
            int const level = std::clamp(Mod::get()->getSavedValue<int>("ship-straight-level", 2), 1, 4);
            static int const strengthByLevel[4] = { 25, 50, 75, 100 };
            static int const sensitivityByLevel[4] = { 25, 55, 75, 90 };
            static int const rhythmByLevel[4] = { 25, 45, 65, 85 };
            state.shipStraightStrength = strengthByLevel[level - 1];
            state.shipStraightSensitivity = sensitivityByLevel[level - 1];
            state.shipStraightRhythm = rhythmByLevel[level - 1];
        }
        state.lastStraightLogTick = -1000;
        state.clickBetweenFramesSync = true;
        state.robotAutoReleaseEnabled = false;
        state.autoNormalRunsEnabled = false;
        state.waveVersion46Mode = false;
        state.waveMarginEnabled = true;
        state.orbWindowTicks = 10;
        state.performanceMode = false;
        state.userBudgetMilliseconds = 8.0;
        state.wasAutoReleased = false;
        state.consoleEnabled = false;
        state.showPathEnabled = false;
    }

    void clickBudget() {
        double budgetScale = std::clamp(static_cast<double>(stepScale()), 1.0, 3.0);
        double budgetMilliseconds = state.userBudgetMilliseconds * budgetScale;
        if (state.automaticBudget) {
            auto* cocosDirector = CCDirector::get();
            double frameInterval = cocosDirector->getAnimationInterval() * 1000.0;
            double frameDuration = cocosDirector->getDeltaTime() * 1000.0;
            if (frameInterval > 0.0 && frameDuration > std::max(frameInterval * 2.0, 20.0)) {
                budgetMilliseconds = std::min(budgetMilliseconds, 4.0 * budgetScale);
            }
        }
        state.budgetMilliseconds = budgetMilliseconds;
    }

    void applyStage() {
        if (state.stageIndex >= 0 && state.stageIndex < STAGE_TABLE_SIZE) {
            auto const& stageDefinition = STAGE_TABLE[state.stageIndex];
            state.isEnabled = stageDefinition.isEnabled;
            state.ticksAllModes = stageDefinition.ticksAllModes;
            state.ticksWaveMode = stageDefinition.ticksWaveMode;
        } else if (auto* custom = customStageFor(state.stageIndex)) {
            state.isEnabled = true;
            state.ticksAllModes = custom->ticksAll;
            state.ticksWaveMode = custom->ticksWave;
        } else {
            state.isEnabled = true;
            state.ticksAllModes = 2;
            state.ticksWaveMode = 2;
        }
    }

    std::string stageText() {
        if (!state.isEnabled) {
            return "off (raw input)";
        }
        return fmt::format("{}t all / {}t wave", state.ticksAllModes, state.ticksWaveMode);
    }

    std::string stageBanner() {
        if (!state.isEnabled) {
            return "stage off (100% raw input)";
        }
        return fmt::format("stage {}: {} ticks all | {} ticks wave ({:g}f all | {:g}f wave @ 60fps)",
            state.stageIndex + 1, state.ticksAllModes, state.ticksWaveMode, state.ticksAllModes / 4.f, state.ticksWaveMode / 4.f);
    }

    constexpr float DECK_WIDTH = 700.f;
    constexpr float DECK_HEIGHT = 460.f;
    constexpr float DECK_RADIUS = 10.f;

    // ADR-029: the old 700px control surface becomes the main pane of a wider
    // frame. The sidebar lives at negative x in the main pane's node space, so
    // every existing control/hit rectangle keeps its original coordinates.
    constexpr float SIDEBAR_WIDTH = 154.f;
    constexpr float FRAME_WIDTH = DECK_WIDTH + SIDEBAR_WIDTH;
    constexpr float SIDE_NAV_X = -SIDEBAR_WIDTH + 12.f;
    constexpr float SIDE_NAV_WIDTH = SIDEBAR_WIDTH - 24.f;
    constexpr float SIDE_NAV_CENTER_Y = 352.f;
    constexpr float SIDE_NAV_STEP = 52.f;
    constexpr float SIDE_NAV_HEIGHT = 42.f;
    constexpr float BUFFER_X = -SIDEBAR_WIDTH + 12.f;
    constexpr float BUFFER_Y = 22.f;
    constexpr float BUFFER_WIDTH = SIDEBAR_WIDTH - 24.f;
    constexpr float BUFFER_HEIGHT = 46.f;

    constexpr float MARGIN = 24.f;
    constexpr float CONTENT_X = MARGIN;
    constexpr float CONTENT_WIDTH = DECK_WIDTH - MARGIN * 2.f;
    constexpr float CONTENT_RIGHT = CONTENT_X + CONTENT_WIDTH;

    constexpr float HEADER_Y = 428.f;
    constexpr float STATUS_HEIGHT = 28.f;

    constexpr int TAB_COUNT = 6;
    constexpr int GROUP_LABEL_COUNT = 4;
    constexpr float TAB_LABEL_Y = 398.f;
    constexpr float TAB_UNDERLINE_Y = 376.f;
    constexpr float TAB_HIT_HEIGHT = 36.f;
    constexpr float TAB_SLOT_WIDTH = CONTENT_WIDTH / static_cast<float>(TAB_COUNT);

    // Visible order differs from the internal tab indices so gameplay/tutorial indices
    // stay stable: Play (stage/modes/straight) -> Read (log/scan) -> Tune (settings).
    constexpr int TAB_DISPLAY_ORDER[TAB_COUNT] = { 1, 3, 4, 0, 5, 2 };

    constexpr float CARD_TOP = 366.f;
    constexpr float CARD_BOTTOM = 70.f;
    constexpr float CARD_HEIGHT = CARD_TOP - CARD_BOTTOM;
    constexpr float CARD_RADIUS = 8.f;

    constexpr float LOG_X = CONTENT_X;
    constexpr float LOG_Y = CARD_BOTTOM;
    constexpr float LOG_WIDTH = CONTENT_WIDTH;
    constexpr float LOG_HEIGHT = CARD_HEIGHT;
    constexpr float LOG_HEADER_Y = 336.f;
    constexpr float SETTINGS_HEADER_Y = 346.f;
    constexpr float LOG_DIVIDER_Y = 318.f;
    constexpr float LOG_LINE_START_Y = 298.f;
    constexpr float LOG_LINE_STEP = 44.f;
    constexpr int LOG_LINE_COUNT = 5;

    constexpr float STAGE_NAME_Y = 330.f;
    constexpr float STAGE_META_Y = 290.f;

    constexpr float STAGE_TRACK_X = CONTENT_X + 20.f;
    constexpr float STAGE_TRACK_Y = 226.f;
    constexpr float STAGE_TRACK_WIDTH = CONTENT_WIDTH - 40.f;
    constexpr float STAGE_TRACK_HEIGHT = 36.f;
    constexpr float STAGE_TRACK_PADDING = 4.f;
    constexpr float STAGE_TRACK_GAP = 3.f;
    float stageSlotWidth() {
        int count = stageTotalCount();
        if (count < 1) { count = 1; }
        return (STAGE_TRACK_WIDTH - STAGE_TRACK_PADDING * 2.f
            - STAGE_TRACK_GAP * static_cast<float>(count - 1)) / static_cast<float>(count);
    }
    constexpr float STAGE_CELL_HEIGHT = 28.f;
    constexpr float STAGE_LABEL_Y = STAGE_TRACK_Y + STAGE_TRACK_HEIGHT * 0.5f;

    constexpr float STAGE_TIMING_ALL_Y = 180.f;
    constexpr float STAGE_TIMING_WAVE_Y = 132.f;
    constexpr float STAGE_HINT_Y = 98.f;
    constexpr float STAGE_BAR_X = 430.f;
    constexpr float STAGE_BAR_WIDTH = CONTENT_RIGHT - 22.f - STAGE_BAR_X;

    constexpr float TICK_BTN_W = 42.f;
    constexpr float TICK_BTN_H = 30.f;
    constexpr float TICK_BTN_X[4] = { 146.f, 192.f, 324.f, 370.f };
    constexpr float TICK_VALUE_X = 238.f;
    constexpr float TICK_VALUE_W = 80.f;
    constexpr float TICK_LABEL_MAX_WIDTH = 92.f;

    constexpr float COPY_X = CONTENT_RIGHT - 20.f - 74.f;
    constexpr float COPY_Y = LOG_HEADER_Y - 16.f;
    constexpr float COPY_WIDTH = 74.f;
    constexpr float COPY_HEIGHT = 32.f;

    constexpr float LOG_EXPAND_WIDTH = 86.f;
    constexpr float LOG_EXPAND_X = COPY_X - 10.f - LOG_EXPAND_WIDTH;
    constexpr float LOG_EXPAND_HEIGHT = COPY_HEIGHT;
    constexpr float LOG_PANE_TOP = LOG_DIVIDER_Y - 8.f;
    constexpr float LOG_PANE_BOTTOM = CARD_BOTTOM + 6.f;

    // scan tab: the level name row is the "level I'm playing" readout, the density strip is
    // the level shape, and the three rows at the bottom are the remembered scans.
    constexpr int SCAN_LIST_ROWS = 3;
    constexpr float SCAN_LEVEL_Y = 320.f;
    constexpr float SCAN_META_Y = 294.f;
    constexpr float SCAN_DENSITY_LABEL_Y = 272.f;
    constexpr float SCAN_DENSITY_BOTTOM = 244.f;
    constexpr float SCAN_DENSITY_HEIGHT = 18.f;
    constexpr float SCAN_ROUTE_Y = 226.f;
    constexpr float SCAN_DIVIDER_Y = 212.f;
    constexpr float SCAN_LIST_TITLE_Y = 198.f;
    constexpr float SCAN_ROW_Y[SCAN_LIST_ROWS] = { 174.f, 148.f, 122.f };
    constexpr float SCAN_HINT_Y = 92.f;

    CCRect scanRescanRect() {
        return CCRect(CONTENT_RIGHT - 22.f - 88.f, SETTINGS_HEADER_Y - 12.f, 88.f, 24.f);
    }

    CCRect scanDensityRect() {
        return CCRect(CONTENT_X + 22.f, SCAN_DENSITY_BOTTOM, CONTENT_WIDTH - 44.f, SCAN_DENSITY_HEIGHT);
    }

    CCRect scanRowRect(int index) {
        index = std::clamp(index, 0, SCAN_LIST_ROWS - 1);
        return CCRect(CONTENT_X + 20.f, SCAN_ROW_Y[index] - 12.f, CONTENT_WIDTH - 40.f, 24.f);
    }

    constexpr int LOG_VIEW_LINE_COUNT = 12;
    constexpr float LOG_VIEW_LINE_STEP = 29.f;
    constexpr float LOG_VIEW_LINE_SCALE = 0.62f;
    constexpr float LOG_VIEW_CARD_X = 14.f;
    constexpr float LOG_VIEW_CARD_Y = 14.f;
    constexpr float LOG_VIEW_CARD_WIDTH = DECK_WIDTH - 28.f;
    constexpr float LOG_VIEW_CARD_HEIGHT = DECK_HEIGHT - 28.f;

    constexpr float DISMISS_X = CONTENT_RIGHT - 30.f;
    constexpr float DISMISS_Y = HEADER_Y - 15.f;
    constexpr float DISMISS_WIDTH = 30.f;
    constexpr float DISMISS_HEIGHT = 30.f;

    constexpr int THEME_COUNT = 15;
    constexpr int THEMES_PER_ROW = 8;
    constexpr int THEME_ROW_COUNT = (THEME_COUNT + THEMES_PER_ROW - 1) / THEMES_PER_ROW;
    constexpr float SWATCH_WIDTH = 76.f;
    constexpr float SWATCH_HEIGHT = 38.f;
    constexpr float SWATCH_GAP = 6.f;
    constexpr float SWATCH_ROW_GAP = 8.f;
    constexpr float SWATCH_X = CONTENT_X + 20.f;
    constexpr float SWATCH_Y = 288.f;

    constexpr float CREDITS_CARD_WIDTH = 616.f;
    constexpr float CREDITS_CARD_HEIGHT = 330.f;
    constexpr float SETTINGS_ROW_Y[2] = { 206.f, 158.f };
    constexpr int SETTINGS_CELL_COUNT = 3;
    constexpr float SETTINGS_CELL_GAP = 10.f;
    constexpr float SETTINGS_CELL_WIDTH = (CONTENT_WIDTH - 32.f - SETTINGS_CELL_GAP) * 0.5f;

    // `straight` tab. one full-width toggle row plus three full-width dial rows, each with the
    // same four-step control the stage tab uses: -5 / - / + / +5.
    constexpr float STRAIGHT_SUMMARY_Y = 322.f;
    constexpr float STRAIGHT_TOGGLE_Y = 274.f;
    constexpr float STRAIGHT_DIAL_Y[3] = { 220.f, 166.f, 112.f };
    constexpr float STRAIGHT_ROW_H = 42.f;
    constexpr int STRAIGHT_DIAL_COUNT = 3;

    constexpr int MODE_ICON_COUNT = 8;
    constexpr float MODE_ICON_STEP = 32.f;
    constexpr float MODE_ICON_SIZE = 28.f;
    constexpr float MODE_ICON_X =
        CONTENT_X + (CONTENT_WIDTH - MODE_ICON_STEP * static_cast<float>(MODE_ICON_COUNT)) * 0.5f;
    constexpr float MODE_ICON_Y = 256.f;

    constexpr float CONFIG_BUTTON_Y = 104.f;
    constexpr float CONFIG_BUTTON_WIDTH = 120.f;
    constexpr float CONFIG_BUTTON_HEIGHT = 34.f;
    constexpr float CONFIG_BUTTON_X = CONTENT_X + 16.f;
    constexpr float CONFIG_BUTTON_GAP = 10.f;
    constexpr float CONFIG_EXPORT_X = CONFIG_BUTTON_X + CONFIG_BUTTON_WIDTH + CONFIG_BUTTON_GAP;

    constexpr int CHIP_COUNT = 3;
    constexpr float CHIP_WIDTH = 118.f;
    constexpr float CHIP_HEIGHT = 30.f;
    constexpr float CHIP_GAP = 3.f;
    constexpr float CHIP_Y = 20.f;
    constexpr float CHIP_X = MARGIN;
    constexpr float TERMINAL_Y = 20.f;
    constexpr float TERMINAL_WIDTH = 118.f;
    constexpr float TERMINAL_HEIGHT = 30.f;
    constexpr float TERMINAL_X = CONTENT_RIGHT - TERMINAL_WIDTH;

    ccColor4F color4(int red, int green, int blue, int alpha = 255) {
        return ccc4f(red / 255.f, green / 255.f, blue / 255.f, alpha / 255.f);
    }

    ccColor3B color3(int red, int green, int blue) {
        return ccc3(static_cast<GLubyte>(red), static_cast<GLubyte>(green), static_cast<GLubyte>(blue));
    }

    ccColor4F color4(ccColor3B color, int alpha = 255) {
        return ccc4f(color.r / 255.f, color.g / 255.f, color.b / 255.f, alpha / 255.f);
    }

    ccColor3B hexColor(int rgb) {
        return color3((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    }

    bool jsonInt(std::string const& json, char const* key, int& out) {
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos) {
            return false;
        }
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos) {
            return false;
        }
        ++pos;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) {
            ++pos;
        }
        bool negative = false;
        if (pos < json.size() && (json[pos] == '-' || json[pos] == '+')) {
            negative = json[pos] == '-';
            ++pos;
        }
        if (pos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[pos]))) {
            return false;
        }
        long long value = 0;
        while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
            value = value * 10 + static_cast<long long>(json[pos] - '0');
            ++pos;
            if (value > 10000000) {
                break;
            }
        }
        out = static_cast<int>(negative ? -value : value);
        return true;
    }

    struct UITheme {
        char const* name;
        int primary;
        int secondary;
        int surface;
        int text;
    };

    UITheme const THEMES[THEME_COUNT] = {
        { "violet",  0x7C4DFF, 0xB79CFF, 0x16161A, 0xE8E9EE },
        { "ocean",   0x3BA3FF, 0x8FD0FF, 0x121820, 0xE6EDF5 },
        { "mint",    0x34D399, 0x8EE8C4, 0x101A17, 0xE4F2EC },
        { "amber",   0xFFB454, 0xFFD79B, 0x1B1712, 0xF4EDE3 },
        { "rose",    0xFF6B8A, 0xFFB3C3, 0x1B1418, 0xF5E8EC },
        { "mono",    0xC9CDD6, 0xFFFFFF, 0x15161A, 0xEDEEF2 },
        { "ice",     0x67D5FF, 0xBFEAFF, 0x101820, 0xE6F1F7 },
        { "sunset",  0xFF8A5B, 0xFFC29E, 0x1C1512, 0xF7EAE2 },
        { "grape",   0xA855F7, 0xD5B4FF, 0x17131F, 0xEFE7F7 },
        { "lime",    0xA3E635, 0xD9F99D, 0x141A10, 0xEAF3DF },
        { "crimson", 0xF43F5E, 0xFF9EB0, 0x1B1115, 0xF5E4E8 },
        { "slate",   0x94A3B8, 0xE2E8F0, 0x151A21, 0xE8EDF3 },
        { "dark",    0xF2F3F7, 0x14161B, 0x0A0B0E, 0xF2F3F7 },
        { "white",   0x3B82F6, 0xFFFFFF, 0xF4F2ED, 0x1C1D22 },
        { "graphene", 0x8B94A3, 0xC9D1DC, 0x111318, 0xE6EAF0 },
    };

    ccColor3B accentColor() {
        return getUIPrimaryColor();
    }

    ccColor3B secondaryColor() {
        return getUISecondaryColor();
    }

    ccColor3B warningColor() {
        return color3(255, 176, 102);
    }

    ccColor3B blendTextColor(float factor) {
        ccColor3B base = getUITextColor();
        return color3(static_cast<int>(base.r * factor), static_cast<int>(base.g * factor), static_cast<int>(base.b * factor));
    }

    ccColor3B mixColor(ccColor3B from, ccColor3B to, float t) {
        t = std::clamp(t, 0.f, 1.f);
        auto blend = [t](unsigned char a, unsigned char b) {
            return static_cast<unsigned char>(static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t);
        };
        return color3(blend(from.r, to.r), blend(from.g, to.g), blend(from.b, to.b));
    }

    ccColor3B softTextColor() {
        return blendTextColor(0.68f);
    }

    ccColor3B logTextColor() {
        return getUITextColor();
    }

    ccColor3B faintTextColor() {
        return blendTextColor(0.46f);
    }

    bool lightSurface() {
        ccColor3B surface = getUISurfaceColor();
        float const luminance = 0.299f * surface.r + 0.587f * surface.g + 0.114f * surface.b;
        return luminance > 145.f;
    }

    ccColor4F panelTint(int alpha) {
        return lightSurface() ? color4(0, 0, 0, alpha) : color4(255, 255, 255, alpha);
    }

    ccColor3B onSurfaceStrong() {
        return lightSurface() ? color3(24, 25, 30) : color3(244, 245, 250);
    }

    char const* displayFont() {
        static char const* font = geode::cocos::fileExistsInSearchPaths("dynamic-display.fnt"_spr) ? "dynamic-display.fnt"_spr : "bigFont.fnt";
        return font;
    }

    char const* bodyFont() {
        static char const* font = geode::cocos::fileExistsInSearchPaths("dynamic-body.fnt"_spr) ? "dynamic-body.fnt"_spr : "bigFont.fnt";
        return font;
    }

    CCLabelBMFont* makeLabel(std::string const& text, bool isDisplay, float scale, ccColor3B color) {
        auto* label = CCLabelBMFont::create(text.c_str(), isDisplay ? displayFont() : bodyFont());
        if (!label) {
            return nullptr;
        }
        label->setScale(scale * getUIFontScale());
        label->setColor(color);
        if (auto* texture = label->getTexture()) {
            texture->setAntiAliasTexParameters();
        }
        return label;
    }

    CCDrawNode* makeDrawNode() {
        auto* node = CCDrawNode::create();
        if (node) {
            node->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
        }
        return node;
    }

    void animateDynamicLogo(CCSprite* sprite) {
        if (!sprite) {
            return;
        }
        static char const* frameNames[] = {
            "dynamic-logo-f00.png"_spr,
            "dynamic-logo-f01.png"_spr,
            "dynamic-logo-f02.png"_spr,
            "dynamic-logo-f03.png"_spr,
            "dynamic-logo-f04.png"_spr,
            "dynamic-logo-f05.png"_spr,
            "dynamic-logo-f06.png"_spr,
            "dynamic-logo-f07.png"_spr,
            "dynamic-logo-f08.png"_spr,
            "dynamic-logo-f09.png"_spr,
            "dynamic-logo-f10.png"_spr,
            "dynamic-logo-f11.png"_spr,
            "dynamic-logo-f12.png"_spr,
            "dynamic-logo-f13.png"_spr,
            "dynamic-logo-f14.png"_spr,
            "dynamic-logo-f15.png"_spr,
            "dynamic-logo-f16.png"_spr,
            "dynamic-logo-f17.png"_spr,
            "dynamic-logo-f18.png"_spr,
            "dynamic-logo-f19.png"_spr,
            "dynamic-logo-f20.png"_spr,
            "dynamic-logo-f21.png"_spr,
            "dynamic-logo-f22.png"_spr,
            "dynamic-logo-f23.png"_spr,
            "dynamic-logo-f24.png"_spr,
            "dynamic-logo-f25.png"_spr,
            "dynamic-logo-f26.png"_spr,
            "dynamic-logo-f27.png"_spr,
            "dynamic-logo-f28.png"_spr,
            "dynamic-logo-f29.png"_spr,
        };
        auto* animation = CCAnimation::create();
        CCSpriteFrame* firstFrame = nullptr;
        for (char const* name : frameNames) {
            auto* frameSprite = CCSprite::create(name);
            if (!frameSprite) {
                continue;
            }
            auto* frame = CCSpriteFrame::createWithTexture(
                frameSprite->getTexture(), frameSprite->getTextureRect()
            );
            if (!frame) {
                continue;
            }
            if (!firstFrame) {
                firstFrame = frame;
            }
            animation->addSpriteFrame(frame);
        }
        if (!firstFrame) {
            return;
        }
        animation->setDelayPerUnit(0.0167f);
        animation->setRestoreOriginalFrame(false);
        sprite->initWithSpriteFrame(firstFrame);
        sprite->runAction(CCRepeatForever::create(CCAnimate::create(animation)));
    }

    CCSprite* makeLogoMark(float size) {
        auto* sprite = CCSprite::create("dynamic-logo.png"_spr);
        if (!sprite) {
            return nullptr;
        }
        animateDynamicLogo(sprite);
        auto content = sprite->getContentSize();
        float longestSide = std::max(content.width, content.height);
        if (longestSide > 0.f) {
            sprite->setScale(size / longestSide);
        }
        return sprite;
    }

    CCNode* makeLogoNode(float size) {
        // The animated mark is drawn clean: no halo rings behind it.
        auto* node = CCNode::create();
        node->setContentSize(CCSize(size, size));
        node->setAnchorPoint(CCPoint(0.5f, 0.5f));
        if (auto* sprite = makeLogoMark(size)) {
            sprite->setPosition(CCPoint(size * 0.5f, size * 0.5f));
            node->addChild(sprite, 1);
        }
        return node;
    }

    CCNode* makeBrandWordmark(std::string const& text, float scale) {
        auto* root = CCNode::create();
        ccColor3B const from = getUIPrimaryColor();
        ccColor3B const to = getUISecondaryColor();
        ccColor3B const surface = getUISurfaceColor();
        float const surfaceLuma = 0.299f * surface.r + 0.587f * surface.g + 0.114f * surface.b;
        float cursor = 0.f;
        int const count = static_cast<int>(text.size());
        for (int index = 0; index < count; ++index) {
            float const t = count > 1 ? static_cast<float>(index) / static_cast<float>(count - 1) : 0.f;
            ccColor3B letter = mixColor(from, to, t);
            float const letterLuma = 0.299f * letter.r + 0.587f * letter.g + 0.114f * letter.b;
            if (std::abs(letterLuma - surfaceLuma) < 60.f) {
                letter = mixColor(letter, onSurfaceStrong(), 0.6f);
            }
            std::string const glyph(1, text[static_cast<size_t>(index)]);
            auto* main = makeLabel(glyph, true, scale, letter);
            if (!main) {
                continue;
            }
            float const width = main->getContentSize().width * main->getScale();
            float const centerX = cursor + width * 0.5f;
            main->setAnchorPoint(CCPoint(0.5f, 0.5f));
            main->setPosition(CCPoint(centerX, 0.f));
            if (auto* wideGlow = makeLabel(glyph, true, scale * 1.18f, accentColor())) {
                wideGlow->setAnchorPoint(CCPoint(0.5f, 0.5f));
                wideGlow->setPosition(CCPoint(centerX, 0.f));
                wideGlow->setOpacity(26);
                root->addChild(wideGlow, 0);
            }
            if (auto* nearGlow = makeLabel(glyph, true, scale * 1.08f, accentColor())) {
                nearGlow->setAnchorPoint(CCPoint(0.5f, 0.5f));
                nearGlow->setPosition(CCPoint(centerX, 0.f));
                nearGlow->setOpacity(64);
                root->addChild(nearGlow, 1);
            }
            root->addChild(main, 2);
            cursor += width;
        }
        return root;
    }

    void drawCurvedRect(CCDrawNode* node, CCRect const& rect, float radius, ccColor4F fill, ccColor4F border, float borderWidth) {
        if (!node || rect.size.width <= 0.f || rect.size.height <= 0.f) {
            return;
        }
        float left = rect.getMinX();
        float right = rect.getMaxX();
        float bottom = rect.getMinY();
        float top = rect.getMaxY();
        radius = std::clamp(radius, 0.f, std::min(rect.size.width, rect.size.height) * 0.5f);
        if (radius <= 0.01f) {
            CCPoint points[4] = {
                CCPoint(left, bottom),
                CCPoint(right, bottom),
                CCPoint(right, top),
                CCPoint(left, top),
            };
            node->drawPolygon(points, 4, fill, borderWidth, border);
            return;
        }

        constexpr int SEGMENTS = 12;
        constexpr float PI = 3.14159265358979323846f;
        CCPoint points[4 * (SEGMENTS + 1)];
        int count = 0;
        auto addArc = [&](float centerX, float centerY, float startAngle, float endAngle) {
            for (int index = 0; index <= SEGMENTS; ++index) {
                float angle = startAngle + (endAngle - startAngle) * static_cast<float>(index) / static_cast<float>(SEGMENTS);
                points[count++] = CCPoint(centerX + std::cos(angle) * radius, centerY + std::sin(angle) * radius);
            }
        };
        addArc(right - radius, bottom + radius, -PI * 0.5f, 0.f);
        addArc(right - radius, top - radius, 0.f, PI * 0.5f);
        addArc(left + radius, top - radius, PI * 0.5f, PI);
        addArc(left + radius, bottom + radius, PI, PI * 1.5f);
        CCPoint filtered[4 * (SEGMENTS + 1)];
        int filteredCount = 0;
        auto isNear = [](CCPoint const& first, CCPoint const& second) {
            return std::abs(first.x - second.x) < 0.05f && std::abs(first.y - second.y) < 0.05f;
        };
        for (int index = 0; index < count; ++index) {
            if (filteredCount == 0 || !isNear(points[index], filtered[filteredCount - 1])) {
                filtered[filteredCount++] = points[index];
            }
        }
        if (filteredCount > 1 && isNear(filtered[0], filtered[filteredCount - 1])) {
            --filteredCount;
        }
        if (filteredCount < 3) {
            return;
        }
        node->drawPolygon(filtered, static_cast<unsigned int>(filteredCount), fill, borderWidth, border);
    }

    // Dynamic's signature plate: a clipped top-left / bottom-right corner instead of
    // a generic rounded card. Decorative only - no hit rect or layout value changes.
    void drawCutRect(CCDrawNode* node, CCRect const& rect, float cut, ccColor4F fill, ccColor4F border, float borderWidth) {
        if (!node || rect.size.width <= 0.f || rect.size.height <= 0.f) {
            return;
        }
        float const left = rect.getMinX();
        float const right = rect.getMaxX();
        float const bottom = rect.getMinY();
        float const top = rect.getMaxY();
        cut = std::clamp(cut, 0.f, std::min(rect.size.width, rect.size.height) * 0.5f);
        if (cut <= 0.01f) {
            CCPoint points[4] = {
                CCPoint(left, bottom),
                CCPoint(right, bottom),
                CCPoint(right, top),
                CCPoint(left, top),
            };
            node->drawPolygon(points, 4, fill, borderWidth, border);
            return;
        }
        CCPoint points[6] = {
            CCPoint(left, bottom),
            CCPoint(right - cut, bottom),
            CCPoint(right, bottom + cut),
            CCPoint(right, top),
            CCPoint(left + cut, top),
            CCPoint(left, top - cut),
        };
        node->drawPolygon(points, 6, fill, borderWidth, border);
    }

    CCRect customAddRect() {
        return CCRect(CONTENT_RIGHT - 22.f - 104.f, STAGE_META_Y - 15.f, 104.f, 30.f);
    }

    CCRect customRemoveRect() {
        return CCRect(CONTENT_RIGHT - 22.f - 104.f - 8.f - 88.f, STAGE_META_Y - 15.f, 88.f, 30.f);
    }

    float tickRowY(int row) {
        return row == 0 ? STAGE_TIMING_ALL_Y : STAGE_TIMING_WAVE_Y;
    }

    CCRect tickButtonRect(int row, int which) {
        return CCRect(TICK_BTN_X[which], tickRowY(row) - TICK_BTN_H * 0.5f, TICK_BTN_W, TICK_BTN_H);
    }

    CCRect tickValueRect(int row) {
        return CCRect(TICK_VALUE_X, tickRowY(row) - TICK_BTN_H * 0.5f, TICK_VALUE_W, TICK_BTN_H);
    }

    CCRect tickSliderRect(int row) {
        return CCRect(STAGE_BAR_X - 10.f, tickRowY(row) - 15.f, STAGE_BAR_WIDTH + 20.f, 30.f);
    }

    CCRect stageSlotRect(int index) {
        return CCRect(
            STAGE_TRACK_X + STAGE_TRACK_PADDING + static_cast<float>(index) * (stageSlotWidth() + STAGE_TRACK_GAP),
            STAGE_TRACK_Y,
            stageSlotWidth(),
            STAGE_TRACK_HEIGHT
        );
    }

    CCRect stageCellRect(int index) {
        auto slot = stageSlotRect(index);
        return CCRect(
            slot.getMinX(),
            STAGE_TRACK_Y + (STAGE_TRACK_HEIGHT - STAGE_CELL_HEIGHT) * 0.5f,
            slot.size.width,
            STAGE_CELL_HEIGHT
        );
    }

    CCRect chipRect(int index) {
        return CCRect(CHIP_X + static_cast<float>(index) * (CHIP_WIDTH + CHIP_GAP), CHIP_Y, CHIP_WIDTH, CHIP_HEIGHT);
    }

    char const* chipName(int index) {
        static char const* names[CHIP_COUNT] = { "console", "wave", "path" };
        if (index < 0 || index >= CHIP_COUNT) {
            return "";
        }
        return names[index];
    }

    int tabDisplayPosition(int index) {
        for (int position = 0; position < TAB_COUNT; ++position) {
            if (TAB_DISPLAY_ORDER[position] == index) {
                return position;
            }
        }
        return index;
    }

    CCRect tabSlotRect(int index) {
        float const centerY = SIDE_NAV_CENTER_Y - static_cast<float>(tabDisplayPosition(index)) * SIDE_NAV_STEP;
        return CCRect(
            SIDE_NAV_X,
            centerY - SIDE_NAV_HEIGHT * 0.5f,
            SIDE_NAV_WIDTH,
            SIDE_NAV_HEIGHT
        );
    }

    char const* tabName(int index) {
        static char const* names[TAB_COUNT] = { "log", "stage", "settings", "modes", "straight", "scan" };
        if (index < 0 || index >= TAB_COUNT) {
            return "";
        }
        return names[index];
    }

    constexpr float MODE_TICK_TILE_W = 150.f;
    constexpr float MODE_TICK_TILE_H = 100.f;
    constexpr float MODE_TICK_TILE_GAP = 10.f;
    constexpr float MODE_TICK_ROW_TOP = 306.f;
    constexpr float MODE_TICKS_HINT_Y = 324.f;

    CCRect modeTickCellRect(int index) {
        int const column = index % 4;
        int const row = index / 4;
        float const totalWidth = MODE_TICK_TILE_W * 4.f + MODE_TICK_TILE_GAP * 3.f;
        float const startX = CONTENT_X + (CONTENT_WIDTH - totalWidth) * 0.5f;
        return CCRect(
            startX + static_cast<float>(column) * (MODE_TICK_TILE_W + MODE_TICK_TILE_GAP),
            MODE_TICK_ROW_TOP - static_cast<float>(row) * (MODE_TICK_TILE_H + MODE_TICK_TILE_GAP) - MODE_TICK_TILE_H,
            MODE_TICK_TILE_W,
            MODE_TICK_TILE_H
        );
    }

    CCRect modeTickStepRect(int index, int which) {
        auto cell = modeTickCellRect(index);
        float const boxW = 42.f;
        float const boxH = 20.f;
        float const gapX = 16.f;
        float const totalWidth = boxW * 2.f + gapX;
        float const startX = cell.getMinX() + (cell.size.width - totalWidth) * 0.5f;
        float const x = startX + ((which % 2) == 0 ? 0.f : boxW + gapX);
        float const y = which < 2 ? cell.getMinY() + 30.f : cell.getMinY() + 6.f;
        return CCRect(x, y, boxW, boxH);
    }

    CCRect modeTickIconRect(int index) {
        auto cell = modeTickCellRect(index);
        return CCRect(cell.getMidX() - 9.f, cell.getMaxY() - 35.f, 18.f, 18.f);
    }

    CCRect modeTicksAutoRect() {
        return CCRect(CONTENT_RIGHT - 96.f, SETTINGS_HEADER_Y - 12.f, 96.f, 24.f);
    }

    CCRect logExpandRect() {
        return CCRect(LOG_EXPAND_X, COPY_Y, LOG_EXPAND_WIDTH, LOG_EXPAND_HEIGHT);
    }

    CCRect logPaneRect() {
        return CCRect(LOG_X + 8.f, LOG_PANE_BOTTOM, LOG_WIDTH - 16.f, LOG_PANE_TOP - LOG_PANE_BOTTOM);
    }

    CCRect logViewCardRect() {
        return CCRect(LOG_VIEW_CARD_X, LOG_VIEW_CARD_Y, LOG_VIEW_CARD_WIDTH, LOG_VIEW_CARD_HEIGHT);
    }

    CCRect logViewCloseRect() {
        auto card = logViewCardRect();
        return CCRect(card.getMaxX() - 42.f, card.getMaxY() - 42.f, 28.f, 28.f);
    }

    CCRect logViewListRect() {
        auto card = logViewCardRect();
        return CCRect(card.getMinX() + 8.f, card.getMinY() + 10.f, card.size.width - 16.f, card.size.height - 78.f);
    }

    CCRect creditsRect() {
        return CCRect(CONTENT_RIGHT - 20.f - 62.f, SETTINGS_HEADER_Y - 12.f, 62.f, 24.f);
    }

    CCRect creditsCardRect() {
        return CCRect(
            (DECK_WIDTH - CREDITS_CARD_WIDTH) * 0.5f,
            (DECK_HEIGHT - CREDITS_CARD_HEIGHT) * 0.5f,
            CREDITS_CARD_WIDTH,
            CREDITS_CARD_HEIGHT
        );
    }

    CCRect creditsCloseRect() {
        auto card = creditsCardRect();
        return CCRect(card.getMaxX() - 46.f, card.getMaxY() - 46.f, 30.f, 30.f);
    }

    CCRect configButtonRect(int index) {
        float x = (index == 0) ? CONFIG_BUTTON_X
            : (index == 1 ? CONFIG_EXPORT_X : CONFIG_EXPORT_X + CONFIG_BUTTON_WIDTH + CONFIG_BUTTON_GAP);
        return CCRect(x, CONFIG_BUTTON_Y - CONFIG_BUTTON_HEIGHT * 0.5f, CONFIG_BUTTON_WIDTH, CONFIG_BUTTON_HEIGHT);
    }

    CCRect swatchRect(int index) {
        int const row = index / THEMES_PER_ROW;
        int const column = index % THEMES_PER_ROW;
        return CCRect(
            SWATCH_X + static_cast<float>(column) * (SWATCH_WIDTH + SWATCH_GAP),
            SWATCH_Y - static_cast<float>(row) * (SWATCH_HEIGHT + SWATCH_ROW_GAP),
            SWATCH_WIDTH,
            SWATCH_HEIGHT
        );
    }

    CCRect settingsCellRect(int index) {
        float const innerWidth = CONTENT_WIDTH - 32.f;
        if (index == 0) {
            return CCRect(CONTENT_X + 16.f, SETTINGS_ROW_Y[0] - 22.f, innerWidth, 44.f);
        }
        int const column = index - 1;
        return CCRect(
            CONTENT_X + 16.f + static_cast<float>(column) * (SETTINGS_CELL_WIDTH + SETTINGS_CELL_GAP),
            SETTINGS_ROW_Y[1] - 22.f,
            SETTINGS_CELL_WIDTH,
            44.f
        );
    }

    CCRect settingsStepRect(int cell, bool plus) {
        auto rect = settingsCellRect(cell);
        float const size = 26.f;
        float const right = rect.getMaxX() - 14.f;
        float x = plus ? right - size : right - size * 2.f - 8.f;
        return CCRect(x, rect.getMidY() - size * 0.5f, size, size);
    }

    // index 0 = the assist toggle row, 1..3 = strength / sensitivity / rhythm
    CCRect straightCellRect(int index) {
        float const innerWidth = CONTENT_WIDTH - 32.f;
        float const y = index <= 0 ? STRAIGHT_TOGGLE_Y : STRAIGHT_DIAL_Y[index - 1];
        return CCRect(CONTENT_X + 16.f, y - STRAIGHT_ROW_H * 0.5f, innerWidth, STRAIGHT_ROW_H);
    }

    // which: 0 = -5, 1 = -, 2 = +, 3 = +5
    CCRect straightStepRect(int cell, int which) {
        auto rect = straightCellRect(cell);
        static float const widths[4] = { 36.f, 26.f, 26.f, 36.f };
        float const gap = 6.f;
        float total = 0.f;
        for (int i = 0; i < 4; ++i) {
            total += widths[i] + (i > 0 ? gap : 0.f);
        }
        float x = rect.getMaxX() - 14.f - total;
        for (int i = 0; i < which; ++i) {
            x += widths[i] + gap;
        }
        return CCRect(x, rect.getMidY() - 13.f, widths[which], 26.f);
    }

    char const* stageName(int index) {
        static char const* names[STAGE_TABLE_SIZE] = {
            "tight", "quick", "steady", "flow", "deep", "wide", "max", "raw",
        };
        if (index < 0 || index >= STAGE_TABLE_SIZE) {
            return "unknown";
        }
        return names[index];
    }

    char const* stageLabelAt(int index) {
        if (index >= 0 && index < STAGE_TABLE_SIZE) {
            return stageName(index);
        }
        static char const* customNames[CUSTOM_STAGE_LIMIT] = { "c1", "c2", "c3", "c4" };
        loadCustomStages();
        int custom = index - STAGE_TABLE_SIZE;
        if (custom >= 0 && custom < static_cast<int>(g_customStages.size()) && custom < CUSTOM_STAGE_LIMIT) {
            return customNames[custom];
        }
        return "?";
    }

    std::string stageTitleAt(int index) {
        if (index >= STAGE_TABLE_SIZE) {
            return fmt::format("custom {}", index - STAGE_TABLE_SIZE + 1);
        }
        return stageLabelAt(index);
    }

    void fitLabelToWidth(CCLabelBMFont* label, float maxWidth) {
        if (!label || maxWidth <= 0.f) {
            return;
        }
        float width = label->getContentSize().width * label->getScale();
        if (width > maxWidth && width > 0.f) {
            label->setScale(label->getScale() * maxWidth / width);
        }
    }

    CCLabelTTF* makeTTFLabel(std::string const& text, int fontSize, float scale, ccColor3B color) {
        auto* label = CCLabelTTF::create(text.c_str(), "Arial", static_cast<float>(fontSize));
        if (!label) {
            return nullptr;
        }
        label->setScale(scale * getUIFontScale());
        label->setColor(color);
        return label;
    }

    void fitTTFLabelToWidth(CCLabelTTF* label, float maxWidth) {
        if (!label || maxWidth <= 0.f) {
            return;
        }
        float width = label->getContentSize().width * label->getScale();
        if (width > maxWidth && width > 0.f) {
            label->setScale(label->getScale() * maxWidth / width);
        }
    }

    CCNode* makeDeckButton(CCNode* parent, CCRect const& rect, std::string const& text, bool isFilled, bool useDisplayFont, float fontScale, bool isTinted = false) {
        auto* button = CCNode::create();
        button->setContentSize(CCSize(rect.size.width, rect.size.height));
        button->setAnchorPoint(CCPoint(0.5f, 0.5f));
        button->setPosition(CCPoint(rect.getMidX(), rect.getMidY()));

        ccColor4F none = color4(0, 0, 0, 0);
        auto* background = makeDrawNode();
        float radius = std::min(rect.size.height * 0.25f, 7.f);
        auto const bounds = CCRect(0.f, 0.f, rect.size.width, rect.size.height);
        if (isFilled) {
            drawCurvedRect(background, CCRect(0.f, -2.f, rect.size.width, rect.size.height), radius, color4(0, 0, 0, 56), none, 0.f);
            drawCurvedRect(background, bounds, radius, color4(accentColor(), 238), color4(secondaryColor(), 150), 1.f);
            drawCurvedRect(
                background,
                CCRect(2.f, rect.size.height - 8.f, rect.size.width - 4.f, 6.f),
                std::min(radius, 3.f),
                color4(secondaryColor(), 54),
                none,
                0.f
            );
        }
        else if (isTinted) {
            drawCurvedRect(background, bounds, radius, color4(accentColor(), 28), color4(accentColor(), 92), 1.f);
            drawCurvedRect(
                background,
                CCRect(2.f, rect.size.height - 6.f, rect.size.width - 4.f, 4.f),
                std::min(radius, 2.f),
                color4(secondaryColor(), 34),
                none,
                0.f
            );
            background->drawSegment(CCPoint(3.f, 5.f), CCPoint(3.f, rect.size.height - 5.f), 1.5f, color4(accentColor(), 110));
        }
        else {
            drawCurvedRect(background, bounds, radius, panelTint(8), panelTint(28), 1.f);
            background->drawSegment(
                CCPoint(5.f, rect.size.height - 3.f),
                CCPoint(rect.size.width - 5.f, rect.size.height - 3.f),
                1.f,
                color4(secondaryColor(), 34)
            );
        }
        button->addChild(background);

        ccColor3B labelTint = softTextColor();
        if (isFilled) {
            labelTint = secondaryColor();
        }
        else if (isTinted) {
            labelTint = lightSurface() ? accentColor() : secondaryColor();
        }
        if (auto* label = makeLabel(text, useDisplayFont, fontScale, labelTint)) {
            label->setPosition(CCPoint(rect.size.width * 0.5f, rect.size.height * 0.5f));
            button->addChild(label);
        }
        parent->addChild(button, 4);
        return button;
    }

    CCNode* makeStepButton(CCNode* parent, CCRect const& rect, bool pointsForward) {
        auto* button = CCNode::create();
        button->setContentSize(CCSize(rect.size.width, rect.size.height));
        button->setAnchorPoint(CCPoint(0.5f, 0.5f));
        button->setPosition(CCPoint(rect.getMidX(), rect.getMidY()));

        auto* background = makeDrawNode();

        float midX = rect.size.width * 0.5f;
        float midY = rect.size.height * 0.5f;
        drawCurvedRect(
            background,
            CCRect(0.f, 0.f, rect.size.width, rect.size.height),
            6.f,
            panelTint(7),
            color4(accentColor(), 58),
            1.f
        );
        background->drawSegment(
            CCPoint(4.f, rect.size.height - 3.f),
            CCPoint(rect.size.width - 4.f, rect.size.height - 3.f),
            1.f,
            color4(secondaryColor(), 30)
        );

        CCPoint top = CCPoint(pointsForward ? midX - 3.5f : midX + 3.5f, midY + 8.f);
        CCPoint tip = CCPoint(pointsForward ? midX + 4.5f : midX - 4.5f, midY);
        CCPoint bottom = CCPoint(pointsForward ? midX - 3.5f : midX + 3.5f, midY - 8.f);
        background->drawSegment(top, tip, 3.5f, color4(accentColor(), 48));
        background->drawSegment(tip, bottom, 3.5f, color4(accentColor(), 48));
        background->drawSegment(top, tip, 2.f, color4(softTextColor(), 245));
        background->drawSegment(tip, bottom, 2.f, color4(softTextColor(), 245));
        button->addChild(background);
        parent->addChild(button, 4);
        return button;
    }

    std::string singleLineLog(std::string text) {
        for (auto& character : text) {
            if (character == '\n' || character == '\r' || character == '\t') {
                character = ' ';
            }
        }
        constexpr size_t MAX_LENGTH = 30;
        if (text.size() > MAX_LENGTH) {
            text.resize(MAX_LENGTH - 3);
            text += "...";
        }
        return text;
    }

    std::string fitLogLine(CCLabelBMFont* label, std::string text, float maxWidth) {
        if (!label) {
            return text;
        }
        label->setString(text.c_str());
        if (label->getContentSize().width * label->getScale() <= maxWidth) {
            return text;
        }
        while (text.size() > 3) {
            text.pop_back();
            std::string candidate = text + "...";
            label->setString(candidate.c_str());
            if (label->getContentSize().width * label->getScale() <= maxWidth) {
                return candidate;
            }
        }
        label->setString("");
        return {};
    }

    void renderLogRows(
        CCLabelBMFont* const* labels,
        int rowCount,
        std::vector<std::string> const& logs,
        int scroll,
        float maxWidth,
        float baseScale,
        bool markLatest
    ) {
        int const newest = static_cast<int>(logs.size()) - 1 - scroll;
        for (int row = 0; row < rowCount; ++row) {
            auto* label = labels[row];
            if (!label) {
                continue;
            }
            int const entry = newest - (rowCount - 1 - row);
            if (entry < 0 || entry >= static_cast<int>(logs.size())) {
                label->setString("");
                continue;
            }
            std::string text = singleLineLog(logs[static_cast<size_t>(entry)]);
            bool const isLatest = markLatest && scroll <= 0 && row == rowCount - 1;
            text = (isLatest ? "> " : "  ") + text;
            fitLogLine(label, text, maxWidth);
            bool lowPriority = text.find("host os") != std::string::npos
                || text.find("loader") != std::string::npos
                || text.find("version check") != std::string::npos;
            label->setScale(isLatest ? baseScale : (lowPriority ? baseScale * 0.88f : baseScale * 0.97f));
            label->setColor(isLatest ? (lightSurface() ? accentColor() : secondaryColor()) : logTextColor());
            label->setOpacity(isLatest ? 255 : (lowPriority ? 150 : 232));
        }
    }

    void drawLogScrollbar(CCDrawNode* node, CCRect const& track, int total, int visible, float scroll) {
        if (!node || total <= visible || track.size.height <= 4.f) {
            return;
        }
        float const top = track.getMaxY();
        float const bottom = track.getMinY();
        float const height = top - bottom;
        float const thumbH = std::max(14.f, height * static_cast<float>(visible) / static_cast<float>(total));
        int const maxScroll = std::max(0, total - visible);
        float const progress = maxScroll > 0 ? std::clamp(scroll / static_cast<float>(maxScroll), 0.f, 1.f) : 0.f;
        float const thumbTop = top - (height - thumbH) * (1.f - progress);
        float const x = track.getMidX();
        node->drawSegment(CCPoint(x, bottom), CCPoint(x, top), 3.f, panelTint(20));
        node->drawSegment(CCPoint(x, thumbTop - thumbH), CCPoint(x, thumbTop), 3.f, color4(accentColor(), 150));
    }

    template <class T>
    T* findDescendant(CCNode* node) {
        if (!node) {
            return nullptr;
        }
        if (auto* match = node->getChildByType<T>(0)) {
            return match;
        }
        if (auto* children = node->getChildren()) {
            for (unsigned int index = 0; index < children->count(); ++index) {
                auto* child = static_cast<CCNode*>(children->objectAtIndex(index));
                if (auto* match = findDescendant<T>(child)) {
                    return match;
                }
            }
        }
        return nullptr;
    }

    constexpr char const* TUTORIAL_DONE_KEY = "tutorial-done";
    constexpr float TUTORIAL_CARD_W = 410.f;

    constexpr int TF_NONE = -1;
    constexpr int TF_LOGO = 0;
    constexpr int TF_HEADER = 1;
    constexpr int TF_DISMISS = 2;
    constexpr int TF_TAB0 = 3;
    constexpr int TF_TAB1 = 4;
    constexpr int TF_TAB2 = 5;
    constexpr int TF_LOGPANE = 6;
    constexpr int TF_COPY = 7;
    constexpr int TF_STAGETRACK = 8;
    constexpr int TF_CUSTOM = 9;
    constexpr int TF_SLIDER = 10;
    constexpr int TF_SWATCHES = 11;
    constexpr int TF_CONFIG = 12;

    struct TutorialStep {
        std::string title;
        int tab;
        std::vector<std::string> lines;
        int cursor;
        bool tap;
        int focus;
        int cursor2;
    };

    std::vector<TutorialStep> tutorialSteps() {
        bool const mobile = isMobileHost();
        std::vector<TutorialStep> steps;
        steps.push_back({ "welcome to dynamic", -1, {
            "dynamic fixes your early clicks so they land",
            "accurately. this quick tour shows what each tab",
            "does, and you can skip it right now.",
        }, TF_NONE, false, TF_LOGO, TF_NONE });
        steps.push_back({ "the panel", 0, {
            "the game pauses while this panel is open.",
            mobile ? std::string("close it any time with the x in the top right.")
                   : fmt::format("close it with the x or {}.", panelKey()),
            "the orb at the sidebar bottom shows whether the buffer is on.",
            "tap the dynamic logo any time to replay this tour.",
        }, TF_DISMISS, false, TF_HEADER, TF_NONE });
        TutorialStep logStep{ "log tab", -1, {
            "a live feed of what dynamic is doing:",
            "stage changes, predictions and update checks.",
            "copy puts the whole log on your clipboard.",
            "the chips at the bottom show which extras are on:",
            "console, wave and path.",
        }, TF_TAB0, true, TF_LOGPANE, TF_COPY };
        if (!mobile) {
            logStep.lines.push_back("terminal opens the log in its own window.");
        }
        steps.push_back(logStep);
        steps.push_back({ "stage tab", -1, {
            "a stage is how far dynamic may delay a click to fix it.",
            "the numbers are ticks: 4 ticks is one frame at 60 fps.",
            "a higher stage can correct earlier clicks.",
            mobile ? std::string("tap a stage to switch to it.")
                   : fmt::format("tap a stage to switch, or press {} to cycle.", stageKey()),
            "the last stage is off, so your clicks are left untouched.",
        }, TF_TAB1, true, TF_STAGETRACK, TF_NONE });
        steps.push_back({ "custom stages", 1, {
            "tap + custom to copy the selected stage (up to 4).",
            "then drag the slider, tap the number and type, or use",
            "-5 / -1 / +1 / +5 to set the delay for all and wave.",
            "remove deletes the selected custom stage.",
            "going over 120 ticks may lag or work worse.",
        }, TF_CUSTOM, false, TF_CUSTOM, TF_SLIDER });
        steps.push_back({ "settings tab", -1, {
            "pick a theme to change the panel colors.",
            "glow turns the soft glow behind the panel on or off.",
            "credits (top right) shows who helped make this.",
            "export copies your tuning as json, import loads it back.",
        }, TF_TAB2, true, TF_SWATCHES, TF_CONFIG });
        steps.push_back({ "you're all set", -1, {
            "that's everything.",
            "tap the dynamic logo any time to see this tour again.",
        }, TF_NONE, false, TF_LOGO, TF_NONE });
        return steps;
    }

    float tutorialCardHeight(size_t lineCount) {
        return 142.f + 22.f * static_cast<float>(lineCount);
    }

    CCRect tutorialSkipRect(CCPoint const& origin) {
        return CCRect(origin.x + 22.f, origin.y + 16.f, 100.f, 32.f);
    }

    CCRect tutorialNextRect(CCPoint const& origin) {
        return CCRect(origin.x + TUTORIAL_CARD_W - 22.f - 120.f, origin.y + 16.f, 120.f, 32.f);
    }

    CCRect logoRect() {
        return CCRect(-SIDEBAR_WIDTH + 8.f, HEADER_Y - 22.f, SIDEBAR_WIDTH - 16.f, 44.f);
    }

    CCRect tutorialRectFor(int kind) {
        switch (kind) {
            case TF_LOGO: return logoRect();
            case TF_HEADER: return CCRect(BUFFER_X, BUFFER_Y, BUFFER_WIDTH, BUFFER_HEIGHT);
            case TF_DISMISS: return CCRect(DISMISS_X, DISMISS_Y, DISMISS_WIDTH, DISMISS_HEIGHT);
            case TF_TAB0:
            case TF_TAB1:
            case TF_TAB2: return tabSlotRect(kind - TF_TAB0);
            case TF_LOGPANE: return CCRect(LOG_X, 250.f, LOG_WIDTH, CARD_TOP - 250.f);
            case TF_COPY: return CCRect(COPY_X, COPY_Y, COPY_WIDTH, COPY_HEIGHT);
            case TF_STAGETRACK: return CCRect(STAGE_TRACK_X, STAGE_TRACK_Y, STAGE_TRACK_WIDTH, STAGE_TRACK_HEIGHT);
            case TF_CUSTOM: return customAddRect();
            case TF_SLIDER: return CCRect(
                STAGE_BAR_X - 2.f, STAGE_TIMING_WAVE_Y - 20.f,
                STAGE_BAR_WIDTH + 14.f, STAGE_TIMING_ALL_Y - STAGE_TIMING_WAVE_Y + 40.f
            );
            case TF_SWATCHES: return CCRect(
                SWATCH_X,
                SWATCH_Y - static_cast<float>(THEME_ROW_COUNT - 1) * (SWATCH_HEIGHT + SWATCH_ROW_GAP),
                static_cast<float>(THEMES_PER_ROW) * SWATCH_WIDTH + static_cast<float>(THEMES_PER_ROW - 1) * SWATCH_GAP,
                static_cast<float>(THEME_ROW_COUNT) * SWATCH_HEIGHT + static_cast<float>(THEME_ROW_COUNT - 1) * SWATCH_ROW_GAP
            );
            case TF_CONFIG: {
                auto first = configButtonRect(0);
                auto second = configButtonRect(1);
                float left = std::min(first.getMinX(), second.getMinX());
                float right = std::max(first.getMaxX(), second.getMaxX());
                return CCRect(left, first.getMinY(), right - left, first.size.height);
            }
            default: return CCRect(0.f, 0.f, DECK_WIDTH, DECK_HEIGHT);
        }
    }

    CCRect padRect(CCRect const& rect, float pad) {
        return CCRect(rect.origin.x - pad, rect.origin.y - pad, rect.size.width + pad * 2.f, rect.size.height + pad * 2.f);
    }

    float rectOverlapArea(CCRect const& first, CCRect const& second) {
        float width = std::min(first.getMaxX(), second.getMaxX()) - std::max(first.getMinX(), second.getMinX());
        float height = std::min(first.getMaxY(), second.getMaxY()) - std::max(first.getMinY(), second.getMinY());
        return (width > 0.f && height > 0.f) ? width * height : 0.f;
    }

    CCPoint tutorialPickCardOrigin(float height, std::vector<CCRect> const& avoid, bool centered) {
        float const xs[3] = { (DECK_WIDTH - TUTORIAL_CARD_W) * 0.5f, 12.f, DECK_WIDTH - 12.f - TUTORIAL_CARD_W };
        if (centered) {
            return CCPoint(xs[0], (DECK_HEIGHT - height) * 0.5f);
        }
        float const ys[2] = { 10.f, DECK_HEIGHT - 10.f - height };
        CCPoint best(xs[0], ys[0]);
        float bestScore = 1e9f;
        for (float y : ys) {
            for (float x : xs) {
                CCRect card(x, y, TUTORIAL_CARD_W, height);
                float score = 0.f;
                for (auto const& rect : avoid) {
                    score += rectOverlapArea(card, padRect(rect, 4.f));
                }
                if (score < bestScore - 0.5f) {
                    bestScore = score;
                    best = CCPoint(x, y);
                }
            }
        }
        return best;
    }

    class DynamicOverlay : public CCLayer {
    public:
        static DynamicOverlay* create();
        bool init() override;
        void onEnter() override;
        void onExit() override;
        void refresh();
        void rebuildDeck();
        void openPanel();
        void closePanel();
        void togglePanel();

    protected:
        bool ccTouchBegan(CCTouch* touch, CCEvent* event) override;
        void ccTouchMoved(CCTouch* touch, CCEvent* event) override;
        void ccTouchEnded(CCTouch* touch, CCEvent* event) override;
        void ccTouchCancelled(CCTouch* touch, CCEvent* event) override;
        void registerWithTouchDispatcher() override;
        void scrollWheel(float y, float x) override;

    private:
        enum class HitTarget {
            None,
            Outside,
            Dismiss,
            StageBack,
            StageForward,
            CopyLog,
            LogExpand,
            LogPane,
            Terminal,
            StageSlot,
            Tab,
            Chip,
            Theme,
            SettingsRow,
            SettingsStep,
            ImportConfig,
            ExportConfig,
            ModeTick,
            ModeTickReset,
            ModeTicksAuto,
            StraightToggle,
            StraightStep,
            ReplayTutorial,
            Credits,
            BufferToggle,
            ModeIcon,
            ModeIconAdd,
            StageAdd,
            CustomRemove,
            TickAdjust,
            TickField,
            TickSlider,
            ScanRescan,
            TutorialSkip,
            TutorialNext,
            Logo,
        };

        void buildDeck();
        void applyTab();
        void applyChip();
        void applyTheme(int index);
        void buildCreditsLayer();
        void closeCreditsLayer();
        void buildLogLayer();
        void closeLogLayer();
        void exportConfig();
        void importConfig();
        void updateModeIcons();
        void setConfigStatus(std::string const& text, bool ok);
        void layout();
        CCPoint defaultLauncherPosition() const;
        CCPoint clampLauncherPosition(CCPoint const& position) const;
        void saveLauncherPosition();
        HitTarget hitTest(CCPoint const& point, int* stageSlot = nullptr) const;
        CCRect statusPillRect() const;
        CCNode* nodeForTarget(HitTarget target) const;
        void pressTarget(HitTarget target);
        void restorePressed();
        void performTarget(HitTarget target);
        void startTutorial();
        void showTutorialStep();
        void advanceTutorial();
        void finishTutorial();
        void buildTutorialLayer();
        void beginTutorialMotion(TutorialStep const& step);
        void tutorialFrame(float dt);
        void clearTutorialLayer();
        void drawTabIndicator();
        void drawGroups();
        void drawOrbBeacon(float dt);
        void drawSidebar();
        void animTick(float dt);
        HitTarget tutorialHit(CCPoint const& point) const;
        void beginTickEdit(int row);
        void endTickEdit();
        void onTickInput(int row, std::string const& text);
        void setSliderFromX(float x);
        void copyLog();
        void pauseForPanel();
        void resumeFromPanel();
        void removePauseLayers();
        void updateHints();
        void tickRefresh(float dt);
        void popupTick(float dt);

        CCLayerColor* m_scrim = nullptr;
        CCNodeRGBA* m_deck = nullptr;
        CCNode* m_content = nullptr;
        CCDrawNode* m_sidebarBar = nullptr;
        CCNode* m_launcher = nullptr;
        CCDrawNode* m_shell = nullptr;
        CCDrawNode* m_stageTrack = nullptr;
        CCDrawNode* m_logHighlight = nullptr;
        CCDrawNode* m_statusBeacon = nullptr;
        CCLabelBMFont* m_statusLabel = nullptr;
        CCLabelBMFont* m_statusDetailLabel = nullptr;
        CCLabelBMFont* m_stageIndexLabel = nullptr;
        CCLabelBMFont* m_stageNameLabel = nullptr;
        CCLabelBMFont* m_stageDescriptionLabel = nullptr;
        CCLabelBMFont* m_stagePlaceholderLabel = nullptr;
        CCLabelBMFont* m_activityLabel = nullptr;
        CCLabelBMFont* m_activityCountLabel = nullptr;
        CCLabelBMFont* m_activityLines[LOG_LINE_COUNT] = {};
        CCLabelBMFont* m_stageNameLabels[STAGE_MAX_STAGES] = {};
        CCLabelBMFont* m_stageHintKeyLabel = nullptr;
        CCLabelBMFont* m_stageHintLabel = nullptr;
        CCLabelBMFont* m_panelHintKeyLabel = nullptr;
        CCLabelBMFont* m_panelHintLabel = nullptr;
        CCLabelBMFont* m_copyLabel = nullptr;
        CCDrawNode* m_tabBar = nullptr;
        CCLabelBMFont* m_tabLabels[TAB_COUNT] = {};
        CCDrawNode* m_groupBar = nullptr;
        CCLabelBMFont* m_groupLabels[GROUP_LABEL_COUNT] = {};
        CCNode* m_logPane = nullptr;
        CCNode* m_stagePane = nullptr;
        CCNode* m_modeTicksPane = nullptr;
        CCDrawNode* m_modeTicksBar = nullptr;
        CCLabelBMFont* m_modeTicksHeaderLabel = nullptr;
        CCLabelBMFont* m_modeTicksHintLabel = nullptr;
        CCLabelBMFont* m_modeTickNameLabels[MODE_TICK_COUNT] = {};
        CCLabelBMFont* m_modeTickValueLabels[MODE_TICK_COUNT] = {};
        CCLabelBMFont* m_modeTickBtnLabels[MODE_TICK_COUNT * 4] = {};
        CCLabelBMFont* m_modeTicksAutoLabel = nullptr;
        SimplePlayer* m_modeTickIcons[MODE_TICK_COUNT] = {};
        CCNode* m_straightPane = nullptr;
        CCDrawNode* m_straightBar = nullptr;
        CCLabelBMFont* m_straightHeaderLabel = nullptr;
        CCLabelBMFont* m_straightHintLabel = nullptr;
        CCLabelBMFont* m_straightSummaryLabel = nullptr;
        CCLabelBMFont* m_straightToggleLabel = nullptr;
        CCLabelBMFont* m_straightDialLabels[STRAIGHT_DIAL_COUNT] = {};
        CCLabelBMFont* m_straightDetailLabels[STRAIGHT_DIAL_COUNT] = {};
        CCLabelBMFont* m_straightStepLabels[STRAIGHT_DIAL_COUNT * 4] = {};
        CCNode* m_scanPane = nullptr;
        CCDrawNode* m_scanBar = nullptr;
        CCLabelBMFont* m_scanLevelLabel = nullptr;
        CCLabelBMFont* m_scanStatusLabel = nullptr;
        CCLabelBMFont* m_scanMetaLabel = nullptr;
        CCLabelBMFont* m_scanDensityLabel = nullptr;
        CCLabelBMFont* m_scanRouteLabel = nullptr;
        CCLabelBMFont* m_scanListTitleLabel = nullptr;
        CCLabelBMFont* m_scanRowNameLabels[SCAN_LIST_ROWS] = {};
        CCLabelBMFont* m_scanRowDetailLabels[SCAN_LIST_ROWS] = {};
        CCLabelBMFont* m_scanHintLabel = nullptr;
        CCNode* m_scanRescanButton = nullptr;
        CCNode* m_modesPane = nullptr;
        CCNode* m_toolsPane = nullptr;
        CCLabelBMFont* m_modesWaveLabel = nullptr;
        CCLabelBMFont* m_modesStageLabel = nullptr;
        CCLabelBMFont* m_modesNameLabels[4] = {};
        CCLabelBMFont* m_modesValueLabels[4] = {};
        CCLabelBMFont* m_toolsTitleLabels[3] = {};
        CCLabelBMFont* m_toolsDescLabels[3] = {};
        CCLabelBMFont* m_stageTimingLabels[4] = {};
        CCLabelBMFont* m_tickBtnLabels[8] = {};
        CCLabelBMFont* m_tickValueLabels[2] = {};
        CCNode* m_tutorialLayer = nullptr;
        int m_tutorialStep = -1;
        CCDrawNode* m_tutShade = nullptr;
        CCDrawNode* m_tutFx = nullptr;
        CCDrawNode* m_tutCursor = nullptr;
        CCDrawNode* m_tutProgress = nullptr;
        CCNode* m_tutCard = nullptr;
        CCRect m_tutSpot = CCRect(0.f, 0.f, 0.f, 0.f);
        CCRect m_tutSpotTarget = CCRect(0.f, 0.f, 0.f, 0.f);
        CCPoint m_tutCardTarget = CCPoint(0.f, 0.f);
        CCPoint m_tutCursorPos = CCPoint(0.f, 0.f);
        float m_tutCardH = 200.f;
        float m_tutTime = 0.f;
        float m_tutAlpha = 0.f;
        float m_tutCursorVis = 0.f;
        float m_tutProgShown = 0.f;
        float m_tutRippleT = -1.f;
        float m_tutPhaseTime = 0.f;
        int m_tutPhase = 2;
        int m_tutKindNow = -1;
        int m_tutFocusNow = -1;
        int m_tutKind2 = -1;
        int m_tutTotal = 1;
        bool m_tutTapNow = false;
        bool m_tutActionDone = false;
        bool m_tutClosing = false;
        float m_tabIndX = -1.f;
        float m_tabIndTarget = 0.f;
        float m_orbTime = 0.f;
        float m_orbBurst = 0.f;
        int m_lastAppliedTab = -1;
        float m_barShown[2] = { -1.f, -1.f };
        float m_barTarget[2] = { 0.f, 0.f };
        TextInput* m_tickInputs[2] = {};
        int m_editingRow = -1;
        bool m_suppressTickInput = false;
        bool m_draggingSlider = false;
        bool m_sliderStartedOver = false;
        int m_sliderRow = -1;
        CCLabelBMFont* m_customAddLabel = nullptr;
        CCLabelBMFont* m_customRemoveLabel = nullptr;
        CCLabelBMFont* m_settingsHeaderLabel = nullptr;
        CCLabelBMFont* m_settingsValueLabels[SETTINGS_CELL_COUNT] = {};
        CCLabelBMFont* m_settingsStepLabels[4] = {};
        CCLabelBMFont* m_settingsHintLabel = nullptr;
        CCLabelBMFont* m_settingsThemeLabels[THEME_COUNT] = {};
        CCDrawNode* m_settingsBar = nullptr;
        CCDrawNode* m_themeSwatches = nullptr;
        CCNode* m_modeIconContainer = nullptr;
        std::vector<SimplePlayer*> m_modeIcons;
        std::vector<int>& modeSlots();
        void saveModeSlots();
        std::vector<int> m_modeSlots;
        bool m_modeSlotsLoaded = false;
        CCLabelBMFont* m_configStatusLabel = nullptr;
        CCNode* m_importButton = nullptr;
        CCNode* m_exportButton = nullptr;
        CCNode* m_tutorialButton = nullptr;
        CCNode* m_creditsButton = nullptr;
        CCNode* m_creditsLayer = nullptr;
        CCNode* m_logLayer = nullptr;
        CCNode* m_logExpandButton = nullptr;
        CCLabelBMFont* m_logViewLines[LOG_VIEW_LINE_COUNT] = {};
        CCLabelBMFont* m_logViewTitleLabel = nullptr;
        CCLabelBMFont* m_logViewCountLabel = nullptr;
        CCDrawNode* m_logViewBar = nullptr;
        float m_logScroll = 0.f;
        float m_logViewScroll = 0.f;
        size_t m_logLastCount = 0;
        bool m_logDragging = false;
        bool m_logDragMoved = false;
        float m_logDragStartY = 0.f;
        float m_logDragBase = 0.f;
        bool m_logViewDragging = false;
        bool m_logViewDragMoved = false;
        float m_logViewDragStartY = 0.f;
        float m_logViewDragBase = 0.f;
        int m_activeTab = 0;
        CCDrawNode* m_chipBar = nullptr;
        CCLabelBMFont* m_chipLabels[CHIP_COUNT] = {};
        CCNode* m_stageBackButton = nullptr;
        CCNode* m_stageForwardButton = nullptr;
        CCNode* m_copyButton = nullptr;
        CCNode* m_dismissButton = nullptr;
        CCNode* m_terminalButton = nullptr;
        CCRect m_safeArea;
        CCPoint m_launcherPosition;
        CCPoint m_launcherDragStart;
        CCPoint m_launcherDragOffset;
        float m_deckScale = 1.f;
        bool m_isOpen = false;
        bool m_popupHidden = false;
        bool m_didPauseForPanel = false;
        bool m_isDraggingLauncher = false;
        bool m_launcherMoved = false;
        HitTarget m_pressedTarget = HitTarget::None;
        int m_pressedStageSlot = -1;
        CCNode* m_pressedNode = nullptr;
    };

    DynamicOverlay* DynamicOverlay::create() {
        auto* overlay = new DynamicOverlay();
        if (overlay && overlay->init()) {
            overlay->autorelease();
            return overlay;
        }
        delete overlay;
        return nullptr;
    }

    bool DynamicOverlay::init() {
        if (!CCLayer::init()) {
            return false;
        }
        auto winSize = CCDirector::get()->getWinSize();
        setContentSize(winSize);
        setAnchorPoint(CCPoint(0.f, 0.f));
        setPosition(CCPoint(0.f, 0.f));
        setTouchMode(kCCTouchesOneByOne);
        setTouchPriority(-1000);
        setTouchEnabled(isMobileHost());

        m_scrim = CCLayerColor::create(ccc4(0, 0, 0, 255), winSize.width, winSize.height);
        m_scrim->setOpacity(0);
        m_scrim->setVisible(false);
        addChild(m_scrim, 0);

        m_deck = CCNodeRGBA::create();
        m_deck->setContentSize(CCSize(FRAME_WIDTH, DECK_HEIGHT));
        m_deck->setAnchorPoint(CCPoint(0.5f, 0.5f));
        m_deck->setVisible(false);
        addChild(m_deck, 1);

        buildDeck();

        m_launcher = CCNode::create();
        m_launcher->setContentSize(CCSize(68.f, 68.f));
        m_launcher->setAnchorPoint(CCPoint(0.5f, 0.5f));

        auto* launcherLogo = makeLogoNode(56.f);
        if (launcherLogo) {
            launcherLogo->setPosition(CCPoint(34.f, 34.f));
            m_launcher->addChild(launcherLogo);
        }
        m_launcher->setVisible(false);
        addChild(m_launcher, 2);

        layout();
        refresh();
        return true;
    }

    void DynamicOverlay::buildDeck() {
        ccColor4F none = color4(0, 0, 0, 0);
        m_content = CCNode::create();
        m_content->setContentSize(CCSize(DECK_WIDTH, DECK_HEIGHT));
        m_content->setAnchorPoint(CCPoint(0.f, 0.f));
        m_content->setPosition(CCPoint(SIDEBAR_WIDTH, 0.f));
        m_deck->addChild(m_content, 0);

        m_sidebarBar = makeDrawNode();
        m_content->addChild(m_sidebarBar, 1);
        drawSidebar();

        m_shell = nullptr;
        m_stageTrack = nullptr;
        m_logHighlight = nullptr;
        m_statusBeacon = nullptr;
        m_statusLabel = nullptr;
        m_statusDetailLabel = nullptr;
        m_stageIndexLabel = nullptr;
        m_stageNameLabel = nullptr;
        m_stageDescriptionLabel = nullptr;
        m_stagePlaceholderLabel = nullptr;
        m_activityLabel = nullptr;
        m_activityCountLabel = nullptr;
        m_stageHintKeyLabel = nullptr;
        m_stageHintLabel = nullptr;
        m_panelHintKeyLabel = nullptr;
        m_panelHintLabel = nullptr;
        m_copyLabel = nullptr;
        m_tabBar = nullptr;
        m_groupBar = nullptr;
        for (int index = 0; index < GROUP_LABEL_COUNT; ++index) { m_groupLabels[index] = nullptr; }
        m_logPane = nullptr;
        m_stagePane = nullptr;
        m_modeTicksPane = nullptr;
        m_modeTicksBar = nullptr;
        m_modeTicksHeaderLabel = nullptr;
        m_modeTicksHintLabel = nullptr;
        m_modeTicksAutoLabel = nullptr;
        for (int index = 0; index < MODE_TICK_COUNT; ++index) {
            m_modeTickNameLabels[index] = nullptr;
            m_modeTickValueLabels[index] = nullptr;
            m_modeTickIcons[index] = nullptr;
        }
        for (int index = 0; index < MODE_TICK_COUNT * 4; ++index) { m_modeTickBtnLabels[index] = nullptr; }
        m_straightPane = nullptr;
        m_straightBar = nullptr;
        m_straightHeaderLabel = nullptr;
        m_straightHintLabel = nullptr;
        m_straightSummaryLabel = nullptr;
        m_straightToggleLabel = nullptr;
        for (int dial = 0; dial < STRAIGHT_DIAL_COUNT; ++dial) { m_straightDialLabels[dial] = nullptr; m_straightDetailLabels[dial] = nullptr; }
        for (int index = 0; index < STRAIGHT_DIAL_COUNT * 4; ++index) { m_straightStepLabels[index] = nullptr; }
        m_scanPane = nullptr;
        m_scanBar = nullptr;
        m_scanLevelLabel = nullptr;
        m_scanStatusLabel = nullptr;
        m_scanMetaLabel = nullptr;
        m_scanDensityLabel = nullptr;
        m_scanRouteLabel = nullptr;
        m_scanListTitleLabel = nullptr;
        m_scanHintLabel = nullptr;
        m_scanRescanButton = nullptr;
        for (int index = 0; index < SCAN_LIST_ROWS; ++index) {
            m_scanRowNameLabels[index] = nullptr;
            m_scanRowDetailLabels[index] = nullptr;
        }
        m_modesPane = nullptr;
        m_toolsPane = nullptr;
        m_modesWaveLabel = nullptr;
        m_modesStageLabel = nullptr;
        m_chipBar = nullptr;
        m_stageBackButton = nullptr;
        m_stageForwardButton = nullptr;
        m_copyButton = nullptr;
        m_dismissButton = nullptr;
        m_terminalButton = nullptr;
        m_settingsHeaderLabel = nullptr;
        m_settingsHintLabel = nullptr;
        m_settingsBar = nullptr;
        m_themeSwatches = nullptr;
        m_modeIconContainer = nullptr;
        m_configStatusLabel = nullptr;
        m_importButton = nullptr;
        m_exportButton = nullptr;
        m_tutorialButton = nullptr;
        m_creditsButton = nullptr;
        m_creditsLayer = nullptr;
        m_logExpandButton = nullptr;
        m_logLayer = nullptr;
        m_logViewTitleLabel = nullptr;
        m_logViewCountLabel = nullptr;
        m_logViewBar = nullptr;
        for (int index = 0; index < LOG_VIEW_LINE_COUNT; ++index) { m_logViewLines[index] = nullptr; }
        m_modeIcons.clear();
        for (int index = 0; index < LOG_LINE_COUNT; ++index) { m_activityLines[index] = nullptr; }
        for (int index = 0; index < STAGE_MAX_STAGES; ++index) { m_stageNameLabels[index] = nullptr; }
        for (int index = 0; index < 8; ++index) { m_tickBtnLabels[index] = nullptr; }
        for (int index = 0; index < 2; ++index) { m_tickValueLabels[index] = nullptr; }
        for (int index = 0; index < 2; ++index) { m_tickInputs[index] = nullptr; }
        m_tutorialLayer = nullptr;
        m_tutShade = nullptr;
        m_tutFx = nullptr;
        m_tutCursor = nullptr;
        m_tutProgress = nullptr;
        m_tutCard = nullptr;
        m_tutClosing = false;
        m_tabIndX = -1.f;
        m_lastAppliedTab = -1;
        m_barShown[0] = -1.f;
        m_barShown[1] = -1.f;
        m_editingRow = -1;
        m_draggingSlider = false;
        m_customAddLabel = nullptr;
        m_customRemoveLabel = nullptr;
        for (int index = 0; index < TAB_COUNT; ++index) { m_tabLabels[index] = nullptr; }
        for (int index = 0; index < CHIP_COUNT; ++index) { m_chipLabels[index] = nullptr; }
        for (int index = 0; index < 4; ++index) {
            m_stageTimingLabels[index] = nullptr;
            m_modesNameLabels[index] = nullptr;
            m_modesValueLabels[index] = nullptr;
        }
        for (int index = 0; index < 3; ++index) {
            m_toolsTitleLabels[index] = nullptr;
            m_toolsDescLabels[index] = nullptr;
        }
        for (int index = 0; index < THEME_COUNT; ++index) { m_settingsThemeLabels[index] = nullptr; }
        for (int index = 0; index < SETTINGS_CELL_COUNT; ++index) { m_settingsValueLabels[index] = nullptr; }
        for (int index = 0; index < 4; ++index) { m_settingsStepLabels[index] = nullptr; }
        ccColor3B surface = getUISurfaceColor();

        m_shell = makeDrawNode();
        m_content->addChild(m_shell, 1);
        // ADR-030: one flat shell plate and one flat card. Every theme uses the
        // same simple geometry; only its four saved colors change.
        drawCutRect(m_shell, CCRect(0.f, 0.f, DECK_WIDTH, DECK_HEIGHT), 14.f, color4(surface, 244), panelTint(34), 1.f);
        drawCutRect(
            m_shell,
            CCRect(CONTENT_X, CARD_BOTTOM, CONTENT_WIDTH, CARD_HEIGHT),
            12.f,
            color4(mixColor(surface, accentColor(), 0.08f), 232),
            panelTint(18),
            1.f
        );

        // Non-interactive section plates. They group controls without moving a hit rect.
        m_groupBar = makeDrawNode();
        m_content->addChild(m_groupBar, 2);
        for (int index = 0; index < GROUP_LABEL_COUNT; ++index) {
            auto* caption = makeLabel("", true, 0.44f, faintTextColor());
            if (caption) {
                caption->setAnchorPoint(CCPoint(0.f, 0.5f));
                caption->setVisible(false);
                m_content->addChild(caption, 3);
                m_groupLabels[index] = caption;
            }
        }

        m_tabBar = makeDrawNode();
        m_content->addChild(m_tabBar, 2);
        for (int tabIndex = 0; tabIndex < TAB_COUNT; ++tabIndex) {
            auto slot = tabSlotRect(tabIndex);
            if (auto* tabLabel = makeLabel(tabName(tabIndex), true, 0.66f, faintTextColor())) {
                tabLabel->setAnchorPoint(CCPoint(0.5f, 0.5f));
                tabLabel->setPosition(CCPoint(slot.getMidX(), slot.getMidY()));
                fitLabelToWidth(tabLabel, SIDE_NAV_WIDTH - 12.f);
                m_content->addChild(tabLabel, 3);
                m_tabLabels[tabIndex] = tabLabel;
            }
        }

        if (auto* mark = makeLogoNode(34.f)) {
            mark->setPosition(CCPoint(-SIDEBAR_WIDTH + 24.f, HEADER_Y));
            m_content->addChild(mark, 3);
        }
        if (auto* brandName = makeBrandWordmark("dynamic", 0.58f)) {
            brandName->setPosition(CCPoint(-SIDEBAR_WIDTH + 46.f, HEADER_Y - 5.f));
            m_content->addChild(brandName, 3);
        }

        m_statusBeacon = makeDrawNode();
        m_content->addChild(m_statusBeacon, 2);

        m_statusLabel = makeLabel("buffer on", false, 0.68f, lightSurface() ? softTextColor() : secondaryColor());
        m_statusLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
        m_statusLabel->setPosition(CCPoint(BUFFER_X + 42.f, BUFFER_Y + BUFFER_HEIGHT * 0.5f));
        fitLabelToWidth(m_statusLabel, BUFFER_WIDTH - 48.f);
        m_content->addChild(m_statusLabel, 3);

        m_dismissButton = makeDeckButton(
            m_content,
            CCRect(DISMISS_X, DISMISS_Y, DISMISS_WIDTH, DISMISS_HEIGHT),
            "x",
            false,
            true,
            0.62f
        );

        m_logHighlight = makeDrawNode();
        m_content->addChild(m_logHighlight, 1);

        m_activityLabel = makeLabel("log", true, 0.68f, logTextColor());
        m_activityLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
        m_activityLabel->setPosition(CCPoint(LOG_X + 22.f, LOG_HEADER_Y));
        m_content->addChild(m_activityLabel, 3);

        float activityWidth = m_activityLabel->getContentSize().width * m_activityLabel->getScale();
        m_activityCountLabel = makeLabel("/ 0 events", false, 0.62f, softTextColor());
        m_activityCountLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
        m_activityCountLabel->setPosition(CCPoint(LOG_X + 22.f + activityWidth + 10.f, LOG_HEADER_Y));
        m_content->addChild(m_activityCountLabel, 3);

        m_copyButton = makeDeckButton(
            m_content,
            CCRect(COPY_X, COPY_Y, COPY_WIDTH, COPY_HEIGHT),
            "copy",
            false,
            false,
            0.60f
        );
        m_copyLabel = m_copyButton->getChildByType<CCLabelBMFont>(0);

        m_logExpandButton = makeDeckButton(m_content, logExpandRect(), "expand", false, false, 0.44f);
        if (m_logExpandButton) {
            if (auto* label = m_logExpandButton->getChildByType<CCLabelBMFont>(0)) {
                fitLabelToWidth(label, LOG_EXPAND_WIDTH - 16.f);
            }
        }

        for (int index = 0; index < LOG_LINE_COUNT; ++index) {
            auto* line = makeLabel("", false, 0.96f, logTextColor());
            line->setAnchorPoint(CCPoint(0.f, 0.5f));
            line->setPosition(CCPoint(LOG_X + 22.f, LOG_LINE_START_Y - static_cast<float>(index) * LOG_LINE_STEP));
            m_content->addChild(line, 3);
            m_activityLines[index] = line;
        }

        m_stageNameLabel = makeLabel("tight", true, 0.88f, onSurfaceStrong());
        m_stageNameLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
        m_stageNameLabel->setPosition(CCPoint(LOG_X + 22.f, STAGE_NAME_Y));
        m_content->addChild(m_stageNameLabel, 3);

        m_stageIndexLabel = makeLabel("01/08", true, 0.58f, accentColor());
        m_stageIndexLabel->setAnchorPoint(CCPoint(1.f, 0.5f));
        m_stageIndexLabel->setPosition(CCPoint(CONTENT_RIGHT - 22.f, STAGE_NAME_Y));
        m_content->addChild(m_stageIndexLabel, 3);

        m_stageDescriptionLabel = makeLabel("", false, 0.74f, softTextColor());
        m_stageDescriptionLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
        m_stageDescriptionLabel->setPosition(CCPoint(LOG_X + 22.f, STAGE_META_Y));
        m_content->addChild(m_stageDescriptionLabel, 3);

        m_stageTrack = makeDrawNode();
        m_content->addChild(m_stageTrack, 1);
        for (int index = 0; index < stageTotalCount() && index < STAGE_MAX_STAGES; ++index) {
            auto cell = stageCellRect(index);
            auto* nameLabel = makeLabel(stageLabelAt(index), false, 0.58f, softTextColor());
            if (!nameLabel) {
                continue;
            }
            float natural = nameLabel->getContentSize().width * nameLabel->getScale();
            if (natural > cell.size.width - 8.f && natural > 0.f) {
                nameLabel->setScale(nameLabel->getScale() * (cell.size.width - 8.f) / natural);
            }
            nameLabel->setPosition(CCPoint(cell.getMidX(), STAGE_LABEL_Y));
            m_content->addChild(nameLabel, 3);
            m_stageNameLabels[index] = nameLabel;
        }

        if (auto* allLabel = makeLabel("all modes", false, 0.66f, softTextColor())) {
            fitLabelToWidth(allLabel, TICK_LABEL_MAX_WIDTH);
            allLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
            allLabel->setPosition(CCPoint(LOG_X + 22.f, STAGE_TIMING_ALL_Y));
            m_content->addChild(allLabel, 3);
            m_stageTimingLabels[0] = allLabel;
        }
        if (auto* waveLabel = makeLabel("wave", false, 0.66f, softTextColor())) {
            fitLabelToWidth(waveLabel, TICK_LABEL_MAX_WIDTH);
            waveLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
            waveLabel->setPosition(CCPoint(LOG_X + 22.f, STAGE_TIMING_WAVE_Y));
            m_content->addChild(waveLabel, 3);
            m_stageTimingLabels[1] = waveLabel;
        }
        if (auto* hint = makeLabel("tap a stage above to apply it", false, 0.60f, faintTextColor())) {
            hint->setAnchorPoint(CCPoint(0.f, 0.5f));
            hint->setPosition(CCPoint(LOG_X + 22.f, STAGE_HINT_Y));
            m_content->addChild(hint, 3);
            m_stageTimingLabels[2] = hint;
        }

        for (int row = 0; row < 2; ++row) {
            auto valueRect = tickValueRect(row);
            if (auto* valueLabel = makeLabel("0t", true, 0.62f, onSurfaceStrong())) {
                valueLabel->setPosition(CCPoint(valueRect.getMidX(), valueRect.getMidY()));
                m_content->addChild(valueLabel, 3);
                m_tickValueLabels[row] = valueLabel;
            }
            if (auto* tickInput = TextInput::create(TICK_VALUE_W - 6.f, "ticks")) {
                tickInput->setFilter("0123456789");
                tickInput->setMaxCharCount(3);
                tickInput->setPosition(CCPoint(valueRect.getMidX(), valueRect.getMidY()));
                tickInput->setVisible(false);
                tickInput->setCallback([this, row](std::string const& text) {
                    this->onTickInput(row, text);
                });
                m_content->addChild(tickInput, 4);
                m_tickInputs[row] = tickInput;
            }
            static char const* glyphs[4] = { "-5", "-1", "+1", "+5" };
            for (int which = 0; which < 4; ++which) {
                auto buttonRect = tickButtonRect(row, which);
                if (auto* glyph = makeLabel(glyphs[which], false, 0.62f, accentColor())) {
                    fitLabelToWidth(glyph, TICK_BTN_W - 8.f);
                    glyph->setPosition(CCPoint(buttonRect.getMidX(), buttonRect.getMidY()));
                    m_content->addChild(glyph, 3);
                    m_tickBtnLabels[row * 4 + which] = glyph;
                }
            }
        }
        if (auto* addLabel = makeLabel("+ custom", false, 0.62f, accentColor())) {
            auto addRect = customAddRect();
            fitLabelToWidth(addLabel, addRect.size.width - 12.f);
            addLabel->setPosition(CCPoint(addRect.getMidX(), addRect.getMidY()));
            m_content->addChild(addLabel, 3);
            m_customAddLabel = addLabel;
        }
        if (auto* removeLabel = makeLabel("remove", false, 0.62f, warningColor())) {
            auto removeRect = customRemoveRect();
            fitLabelToWidth(removeLabel, removeRect.size.width - 12.f);
            removeLabel->setPosition(CCPoint(removeRect.getMidX(), removeRect.getMidY()));
            m_content->addChild(removeLabel, 3);
            m_customRemoveLabel = removeLabel;
        }

        m_settingsHeaderLabel = makeLabel("appearance", true, 0.62f, logTextColor());
        m_settingsHeaderLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
        m_settingsHeaderLabel->setPosition(CCPoint(LOG_X + 22.f, SETTINGS_HEADER_Y));
        m_content->addChild(m_settingsHeaderLabel, 3);

        m_settingsBar = makeDrawNode();
        m_content->addChild(m_settingsBar, 1);
        for (int cell = 0; cell < SETTINGS_CELL_COUNT; ++cell) {
            drawCurvedRect(
                m_settingsBar,
                settingsCellRect(cell),
                7.f,
                panelTint(8),
                panelTint(22),
                1.f
            );
        }
        for (int cell = 1; cell < SETTINGS_CELL_COUNT; ++cell) {
            for (int which = 0; which < 2; ++which) {
                drawCurvedRect(
                    m_settingsBar,
                    settingsStepRect(cell, which == 1),
                    6.f,
                    color4(accentColor(), 26),
                    color4(accentColor(), 90),
                    1.f
                );
            }
        }

        m_themeSwatches = makeDrawNode();
        m_content->addChild(m_themeSwatches, 1);
        ccColor3B currentPrimary = getUIPrimaryColor();
        for (int index = 0; index < THEME_COUNT; ++index) {
            auto rect = swatchRect(index);
            ccColor3B themePrimary = hexColor(THEMES[index].primary);
            ccColor3B themeSurface = hexColor(THEMES[index].surface);
            ccColor3B themeText = hexColor(THEMES[index].text);
            bool selected = themePrimary.r == currentPrimary.r
                && themePrimary.g == currentPrimary.g
                && themePrimary.b == currentPrimary.b;
            drawCurvedRect(
                m_themeSwatches,
                rect,
                6.f,
                color4(themeSurface, 255),
                selected ? color4(themePrimary, 255) : panelTint(26),
                selected ? 2.f : 1.f
            );
            m_themeSwatches->drawSegment(
                CCPoint(rect.getMinX() + 7.f, rect.getMinY() + 6.f),
                CCPoint(rect.getMinX() + 7.f, rect.getMaxY() - 6.f),
                3.f,
                color4(themePrimary, 255)
            );
            if (auto* swatchLabel = makeLabel(THEMES[index].name, false, 0.60f, themeText)) {
                swatchLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
                swatchLabel->setPosition(CCPoint(rect.getMinX() + 14.f, rect.getMidY()));
                fitLabelToWidth(swatchLabel, rect.size.width - 20.f);
                m_content->addChild(swatchLabel, 3);
                m_settingsThemeLabels[index] = swatchLabel;
            }
        }

        char const* settingsNames[SETTINGS_CELL_COUNT] = { "glow", "panel", "font" };
        for (int cell = 0; cell < SETTINGS_CELL_COUNT; ++cell) {
            auto rect = settingsCellRect(cell);
            if (auto* name = makeLabel(settingsNames[cell], false, 0.72f, softTextColor())) {
                name->setAnchorPoint(CCPoint(0.f, 0.5f));
                name->setPosition(CCPoint(rect.getMinX() + 18.f, rect.getMidY()));
                m_content->addChild(name, 3);
                m_settingsValueLabels[cell] = name;
            }
        }
        for (int cell = 1; cell < SETTINGS_CELL_COUNT; ++cell) {
            for (int which = 0; which < 2; ++which) {
                auto rect = settingsStepRect(cell, which == 1);
                if (auto* glyph = makeLabel(which == 1 ? "+" : "-", false, 0.62f, accentColor())) {
                    glyph->setPosition(CCPoint(rect.getMidX(), rect.getMidY()));
                    m_content->addChild(glyph, 3);
                    m_settingsStepLabels[(cell - 1) * 2 + which] = glyph;
                }
            }
        }

        m_importButton = makeDeckButton(m_content, configButtonRect(0), "import", false, false, 0.58f);
        m_exportButton = makeDeckButton(m_content, configButtonRect(1), "export", false, false, 0.58f);
        m_tutorialButton = makeDeckButton(m_content, configButtonRect(2), "tutorial", false, false, 0.58f, true);

        m_configStatusLabel = makeLabel("", false, 0.54f, faintTextColor());
        m_configStatusLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
        m_configStatusLabel->setPosition(CCPoint(configButtonRect(2).getMaxX() + 14.f, CONFIG_BUTTON_Y));
        m_content->addChild(m_configStatusLabel, 3);

        m_modeTicksPane = CCNode::create();
        m_modeTicksPane->setPosition(CCPoint(0.f, 0.f));
        m_content->addChild(m_modeTicksPane, 3);

        m_modeTicksBar = makeDrawNode();
        m_modeTicksPane->addChild(m_modeTicksBar, 1);
        for (int index = 0; index < MODE_TICK_COUNT; ++index) {
            drawCurvedRect(m_modeTicksBar, modeTickCellRect(index), 7.f, panelTint(8), panelTint(22), 1.f);
            // the ship tile is the straight-fly control, not a tick override: its four steppers are
            // a coarse / fine strength dial (-5 / +5 top, - / + bottom).
            for (int which = 0; which < 4; ++which) {
                drawCurvedRect(
                    m_modeTicksBar,
                    modeTickStepRect(index, which),
                    6.f,
                    color4(accentColor(), 26),
                    color4(accentColor(), 90),
                    1.f
                );
            }
        }
        drawCurvedRect(m_modeTicksBar, modeTicksAutoRect(), 7.f, panelTint(10), panelTint(28), 1.f);

        if (auto* header = makeLabel("ticks per mode", true, 0.62f, logTextColor())) {
            header->setAnchorPoint(CCPoint(0.f, 0.5f));
            header->setPosition(CCPoint(LOG_X + 22.f, SETTINGS_HEADER_Y));
            m_modeTicksPane->addChild(header, 2);
            m_modeTicksHeaderLabel = header;
        }
        if (auto* hint = makeLabel("top row -5 / +5, bottom row - / +; tap a tile for auto; straight: tap on/off, any stepper = strength", false, 0.58f, faintTextColor())) {
            fitLabelToWidth(hint, CONTENT_WIDTH - 44.f);
            hint->setAnchorPoint(CCPoint(0.f, 0.5f));
            hint->setPosition(CCPoint(LOG_X + 22.f, MODE_TICKS_HINT_Y));
            m_modeTicksPane->addChild(hint, 2);
            m_modeTicksHintLabel = hint;
        }

        {
            auto* manager = GameManager::get();
            int modeFrames[MODE_TICK_COUNT] = {};
            IconType modeTypes[MODE_TICK_COUNT] = {
                IconType::Cube, IconType::Ship, IconType::Ball, IconType::Ufo,
                IconType::Wave, IconType::Robot, IconType::Spider, IconType::Swing,
            };
            ccColor3B modePrimary = color3(255, 255, 255);
            ccColor3B modeSecondary = color3(255, 255, 255);
            if (manager) {
                modeFrames[0] = manager->getPlayerFrame();
                modeFrames[1] = manager->getPlayerShip();
                modeFrames[2] = manager->getPlayerBall();
                modeFrames[3] = manager->getPlayerBird();
                modeFrames[4] = manager->getPlayerDart();
                modeFrames[5] = manager->getPlayerRobot();
                modeFrames[6] = manager->getPlayerSpider();
                modeFrames[7] = manager->getPlayerSwing();
                modePrimary = manager->colorForIdx(manager->getPlayerColor());
                modeSecondary = manager->colorForIdx(manager->getPlayerColor2());
            }

            for (int index = 0; index < MODE_TICK_COUNT; ++index) {
                auto cell = modeTickCellRect(index);
                auto iconRect = modeTickIconRect(index);
                if (manager) {
                    auto* icon = SimplePlayer::create(1);
                    if (icon) {
                        icon->updatePlayerFrame(modeFrames[index], modeTypes[index]);
                        icon->setColors(modePrimary, modeSecondary);
                        icon->setScale(iconRect.size.width / 40.f);
                        icon->setPosition(CCPoint(iconRect.getMidX(), iconRect.getMidY()));
                        icon->setOpacity(150);
                        m_modeTicksPane->addChild(icon, 2);
                        m_modeTickIcons[index] = icon;
                    }
                }
                if (auto* name = makeLabel(index == 1 ? "straight" : MODE_TICK_NAMES[index], true, 0.56f, softTextColor())) {
                    fitLabelToWidth(name, MODE_TICK_TILE_W - 20.f);
                    name->setAnchorPoint(CCPoint(0.5f, 0.5f));
                    name->setPosition(CCPoint(cell.getMidX(), cell.getMaxY() - 10.f));
                    m_modeTicksPane->addChild(name, 2);
                    m_modeTickNameLabels[index] = name;
                }
                if (auto* value = makeLabel("auto", false, 0.54f, faintTextColor())) {
                    value->setAnchorPoint(CCPoint(0.5f, 0.5f));
                    value->setPosition(CCPoint(cell.getMidX(), cell.getMaxY() - 46.f));
                    m_modeTicksPane->addChild(value, 2);
                    m_modeTickValueLabels[index] = value;
                }
                static char const* stepText[4] = { "-5", "+5", "-", "+" };
                for (int which = 0; which < 4; ++which) {
                    auto stepRect = modeTickStepRect(index, which);
                    if (auto* glyph = makeLabel(stepText[which], false, 0.56f, accentColor())) {
                        fitLabelToWidth(glyph, stepRect.size.width - 6.f);
                        glyph->setPosition(CCPoint(stepRect.getMidX(), stepRect.getMidY()));
                        m_modeTicksPane->addChild(glyph, 2);
                        m_modeTickBtnLabels[index * 4 + which] = glyph;
                    }
                }
            }
        }
        if (auto* autoLabel = makeLabel("all auto", false, 0.60f, accentColor())) {
            auto rect = modeTicksAutoRect();
            fitLabelToWidth(autoLabel, rect.size.width - 12.f);
            autoLabel->setPosition(CCPoint(rect.getMidX(), rect.getMidY()));
            m_modeTicksPane->addChild(autoLabel, 2);
            m_modeTicksAutoLabel = autoLabel;
        }

        m_straightPane = CCNode::create();
        m_straightPane->setPosition(CCPoint(0.f, 0.f));
        m_content->addChild(m_straightPane, 3);

        m_straightBar = makeDrawNode();
        m_straightPane->addChild(m_straightBar, 1);
        {
            for (int cell = 0; cell <= STRAIGHT_DIAL_COUNT; ++cell) {
                auto rect = straightCellRect(cell);
                drawCurvedRect(m_straightBar, rect, 7.f, panelTint(8), panelTint(22), 1.f);
                if (cell >= 1) {
                    for (int which = 0; which < 4; ++which) {
                        drawCurvedRect(m_straightBar, straightStepRect(cell, which), 6.f, color4(accentColor(), 26), color4(accentColor(), 90), 1.f);
                    }
                }
            }
        }
        if (auto* header = makeLabel("straight fly", true, 0.62f, logTextColor())) {
            header->setAnchorPoint(CCPoint(0.f, 0.5f));
            header->setPosition(CCPoint(LOG_X + 22.f, SETTINGS_HEADER_Y));
            m_straightPane->addChild(header, 2);
            m_straightHeaderLabel = header;
        }
        if (auto* summary = makeLabel("", false, 0.56f, faintTextColor())) {
            summary->setAnchorPoint(CCPoint(0.f, 0.5f));
            summary->setPosition(CCPoint(LOG_X + 22.f, STRAIGHT_SUMMARY_Y));
            m_straightPane->addChild(summary, 2);
            m_straightSummaryLabel = summary;
        }
        if (auto* hint = makeLabel("assist: tap the row. dials: -5 / - / + / +5. values are 0-100", false, 0.54f, faintTextColor())) {
            fitLabelToWidth(hint, CONTENT_WIDTH - 44.f);
            hint->setAnchorPoint(CCPoint(0.f, 0.5f));
            hint->setPosition(CCPoint(LOG_X + 22.f, STRAIGHT_TOGGLE_Y + 34.f));
            m_straightPane->addChild(hint, 2);
            m_straightHintLabel = hint;
        }
        if (auto* toggle = makeLabel("", false, 0.72f, softTextColor())) {
            toggle->setAnchorPoint(CCPoint(0.f, 0.5f));
            toggle->setPosition(CCPoint(straightCellRect(0).getMinX() + 18.f, straightCellRect(0).getMidY()));
            m_straightPane->addChild(toggle, 3);
            m_straightToggleLabel = toggle;
        }
        static char const* dialNames[STRAIGHT_DIAL_COUNT] = { "strength", "sensitivity", "rhythm" };
        static char const* stepText[4] = { "-5", "-", "+", "+5" };
        for (int dial = 0; dial < STRAIGHT_DIAL_COUNT; ++dial) {
            auto rect = straightCellRect(dial + 1);
            if (auto* name = makeLabel(dialNames[dial], false, 0.68f, softTextColor())) {
                name->setAnchorPoint(CCPoint(0.f, 0.5f));
                name->setPosition(CCPoint(rect.getMinX() + 18.f, rect.getMidY()));
                m_straightPane->addChild(name, 3);
                m_straightDialLabels[dial] = name;
            }
            auto firstStep = straightStepRect(dial + 1, 0);
            if (auto* detail = makeLabel("", false, 0.54f, faintTextColor())) {
                detail->setAnchorPoint(CCPoint(1.f, 0.5f));
                detail->setPosition(CCPoint(firstStep.getMinX() - 10.f, rect.getMidY()));
                m_straightPane->addChild(detail, 3);
                m_straightDetailLabels[dial] = detail;
            }
            for (int which = 0; which < 4; ++which) {
                auto stepRect = straightStepRect(dial + 1, which);
                if (auto* glyph = makeLabel(stepText[which], false, 0.56f, accentColor())) {
                    fitLabelToWidth(glyph, stepRect.size.width - 4.f);
                    glyph->setPosition(CCPoint(stepRect.getMidX(), stepRect.getMidY()));
                    m_straightPane->addChild(glyph, 3);
                    m_straightStepLabels[dial * 4 + which] = glyph;
                }
            }
        }

        m_scanPane = CCNode::create();
        m_scanPane->setPosition(CCPoint(0.f, 0.f));
        m_content->addChild(m_scanPane, 3);

        m_scanBar = makeDrawNode();
        m_scanPane->addChild(m_scanBar, 1);

        if (auto* header = makeLabel("level scan", true, 0.62f, logTextColor())) {
            header->setAnchorPoint(CCPoint(0.f, 0.5f));
            header->setPosition(CCPoint(LOG_X + 22.f, SETTINGS_HEADER_Y));
            m_scanPane->addChild(header, 2);
        }

        m_scanRescanButton = makeDeckButton(m_scanPane, scanRescanRect(), "rescan", false, false, 0.54f);
        if (m_scanRescanButton) {
            if (auto* label = m_scanRescanButton->getChildByType<CCLabelBMFont>(0)) {
                fitLabelToWidth(label, scanRescanRect().size.width - 12.f);
            }
        }

        m_scanLevelLabel = makeLabel("not in a level", true, 0.78f, onSurfaceStrong());
        if (m_scanLevelLabel) {
            m_scanLevelLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
            m_scanLevelLabel->setPosition(CCPoint(LOG_X + 22.f, SCAN_LEVEL_Y));
            m_scanPane->addChild(m_scanLevelLabel, 2);
        }

        m_scanMetaLabel = makeLabel("", false, 0.56f, softTextColor());
        if (m_scanMetaLabel) {
            m_scanMetaLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
            m_scanMetaLabel->setPosition(CCPoint(LOG_X + 22.f, SCAN_META_Y));
            m_scanPane->addChild(m_scanMetaLabel, 2);
        }

        m_scanStatusLabel = makeLabel("no level open", false, 0.56f, faintTextColor());
        if (m_scanStatusLabel) {
            m_scanStatusLabel->setAnchorPoint(CCPoint(1.f, 0.5f));
            m_scanStatusLabel->setPosition(CCPoint(CONTENT_RIGHT - 22.f, SCAN_META_Y));
            m_scanPane->addChild(m_scanStatusLabel, 2);
        }

        m_scanDensityLabel = makeLabel("structure density / input points", false, 0.52f, faintTextColor());
        if (m_scanDensityLabel) {
            m_scanDensityLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
            m_scanDensityLabel->setPosition(CCPoint(LOG_X + 22.f, SCAN_DENSITY_LABEL_Y));
            m_scanPane->addChild(m_scanDensityLabel, 2);
        }

        m_scanRouteLabel = makeLabel("", false, 0.58f, softTextColor());
        if (m_scanRouteLabel) {
            m_scanRouteLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
            m_scanRouteLabel->setPosition(CCPoint(LOG_X + 22.f, SCAN_ROUTE_Y));
            m_scanPane->addChild(m_scanRouteLabel, 2);
        }

        m_scanListTitleLabel = makeLabel("remembered levels", false, 0.54f, faintTextColor());
        if (m_scanListTitleLabel) {
            m_scanListTitleLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
            m_scanListTitleLabel->setPosition(CCPoint(LOG_X + 22.f, SCAN_LIST_TITLE_Y));
            m_scanPane->addChild(m_scanListTitleLabel, 2);
        }

        for (int index = 0; index < SCAN_LIST_ROWS; ++index) {
            if (auto* name = makeLabel("", false, 0.62f, softTextColor())) {
                name->setAnchorPoint(CCPoint(0.f, 0.5f));
                name->setPosition(CCPoint(LOG_X + 32.f, SCAN_ROW_Y[index]));
                m_scanPane->addChild(name, 2);
                m_scanRowNameLabels[index] = name;
            }
            if (auto* detail = makeLabel("", false, 0.52f, faintTextColor())) {
                detail->setAnchorPoint(CCPoint(1.f, 0.5f));
                detail->setPosition(CCPoint(CONTENT_RIGHT - 32.f, SCAN_ROW_Y[index]));
                m_scanPane->addChild(detail, 2);
                m_scanRowDetailLabels[index] = detail;
            }
        }

        m_scanHintLabel = makeLabel("scans once per level and stays saved / rescan only if the level changed", false, 0.50f, faintTextColor());
        if (m_scanHintLabel) {
            fitLabelToWidth(m_scanHintLabel, CONTENT_WIDTH - 44.f);
            m_scanHintLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
            m_scanHintLabel->setPosition(CCPoint(LOG_X + 22.f, SCAN_HINT_Y));
            m_scanPane->addChild(m_scanHintLabel, 2);
        }

        {
            auto rect = creditsRect();
            auto* button = CCNode::create();
            button->setContentSize(CCSize(rect.size.width, rect.size.height));
            button->setAnchorPoint(CCPoint(0.5f, 0.5f));
            button->setPosition(CCPoint(rect.getMidX(), rect.getMidY()));
            auto* back = makeDrawNode();
            ccColor3B gold = color3(255, 214, 72);
            drawCurvedRect(back, CCRect(0.f, -1.f, rect.size.width, rect.size.height), 6.f, color4(0, 0, 0, 42), none, 0.f);
            drawCurvedRect(back, CCRect(0.f, 0.f, rect.size.width, rect.size.height), 6.f, color4(gold, 30), color4(gold, 128), 1.f);
            back->drawSegment(
                CCPoint(4.f, rect.size.height - 3.f),
                CCPoint(rect.size.width - 4.f, rect.size.height - 3.f),
                1.f,
                color4(secondaryColor(), 54)
            );
            back->drawCircle(CCPoint(8.f, rect.size.height * 0.5f), 2.4f, color4(gold, 210), 0.f, color4(0, 0, 0, 0), 12);
            button->addChild(back);
            if (auto* label = makeLabel("credits", false, 0.5f, gold)) {
                label->setPosition(CCPoint(rect.size.width * 0.5f + 3.f, rect.size.height * 0.5f));
                fitLabelToWidth(label, rect.size.width - 16.f);
                button->addChild(label);
            }
            m_content->addChild(button, 4);
            m_creditsButton = button;
        }

        applyTab();
        buildTutorialLayer();
    }

    void DynamicOverlay::rebuildDeck() {
        if (!m_deck) {
            return;
        }
        endTickEdit();
        m_pressedTarget = HitTarget::None;
        m_pressedNode = nullptr;
        m_creditsLayer = nullptr;
        m_logLayer = nullptr;
        m_deck->removeAllChildrenWithCleanup(true);
        buildDeck();
        layout();
        refresh();
        if (m_isOpen) {
            m_deck->setVisible(true);
        }
    }

    void DynamicOverlay::layout() {
        auto winSize = CCDirector::get()->getWinSize();
        setContentSize(winSize);
        if (m_scrim) {
            m_scrim->changeWidthAndHeight(winSize.width, winSize.height);
        }

        auto safeArea = geode::utils::getSafeAreaRect();
        if (safeArea.size.width <= 1.f || safeArea.size.height <= 1.f) {
            safeArea = CCRect(0.f, 0.f, winSize.width, winSize.height);
        }
        m_safeArea = safeArea;

        float fitScale = std::min(0.86f * safeArea.size.width / FRAME_WIDTH, 0.86f * safeArea.size.height / DECK_HEIGHT);
        float comfortableScale = std::min(fitScale, 0.94f);
        m_deckScale = std::min(std::max(comfortableScale * getUIPanelScale(), 0.2f), fitScale);
        m_deck->setScale(m_deckScale);
        m_deck->setPosition(CCPoint(safeArea.getMidX(), safeArea.getMidY()));

        double savedX = Mod::get()->getSavedValue<double>("ui-button-x", -1.0);
        double savedY = Mod::get()->getSavedValue<double>("ui-button-y", -1.0);
        if (savedX >= 0.0 && savedX <= 1.0 && savedY >= 0.0 && savedY <= 1.0) {
            m_launcherPosition = CCPoint(
                safeArea.getMinX() + static_cast<float>(savedX) * safeArea.size.width,
                safeArea.getMinY() + static_cast<float>(savedY) * safeArea.size.height
            );
        }
        else {
            m_launcherPosition = defaultLauncherPosition();
        }
        m_launcherPosition = clampLauncherPosition(m_launcherPosition);
        m_launcher->setPosition(m_launcherPosition);
    }

    CCPoint DynamicOverlay::defaultLauncherPosition() const {
        return CCPoint(m_safeArea.getMaxX() - 44.f, m_safeArea.getMaxY() - 104.f);
    }

    CCPoint DynamicOverlay::clampLauncherPosition(CCPoint const& position) const {
        float margin = 36.f;
        float minX = m_safeArea.getMinX() + margin;
        float maxX = m_safeArea.getMaxX() - margin;
        float minY = m_safeArea.getMinY() + margin;
        float maxY = m_safeArea.getMaxY() - margin;
        if (maxX < minX) {
            minX = maxX = m_safeArea.getMidX();
        }
        if (maxY < minY) {
            minY = maxY = m_safeArea.getMidY();
        }
        return CCPoint(std::clamp(position.x, minX, maxX), std::clamp(position.y, minY, maxY));
    }

    void DynamicOverlay::saveLauncherPosition() {
        float width = std::max(1.f, m_safeArea.size.width);
        float height = std::max(1.f, m_safeArea.size.height);
        double normalizedX = (m_launcherPosition.x - m_safeArea.getMinX()) / width;
        double normalizedY = (m_launcherPosition.y - m_safeArea.getMinY()) / height;
        Mod::get()->setSavedValue("ui-button-x", normalizedX);
        Mod::get()->setSavedValue("ui-button-y", normalizedY);
    }

    void DynamicOverlay::applyTheme(int index) {
        if (index < 0 || index >= THEME_COUNT) {
            return;
        }
        auto const& theme = THEMES[index];
        Mod::get()->setSettingValue<ccColor3B>("ui-primary-color", hexColor(theme.primary));
        Mod::get()->setSettingValue<ccColor3B>("ui-secondary-color", hexColor(theme.secondary));
        Mod::get()->setSettingValue<ccColor3B>("ui-surface-color", hexColor(theme.surface));
        Mod::get()->setSettingValue<ccColor3B>("ui-text-color", hexColor(theme.text));
        rebuildDeck();
    }

    void DynamicOverlay::buildCreditsLayer() {
        if (!m_deck || m_creditsLayer) {
            return;
        }
        auto const card = creditsCardRect();
        float const width = card.size.width;
        float const height = card.size.height;

        m_creditsLayer = CCNode::create();
        m_creditsLayer->setContentSize(CCSize(DECK_WIDTH, DECK_HEIGHT));
        m_content->addChild(m_creditsLayer, 40);

        if (auto* shade = makeDrawNode()) {
            shade->drawRect(
                CCRect(0.f, 0.f, DECK_WIDTH, DECK_HEIGHT),
                color4(4, 5, 8, 196),
                0.f,
                color4(0, 0, 0, 0)
            );
            m_creditsLayer->addChild(shade, 0);
        }

        auto* sheet = CCNode::create();
        sheet->setPosition(card.origin);
        m_creditsLayer->addChild(sheet, 1);

        if (auto* glow = makeDrawNode()) {
            drawCurvedRect(
                glow,
                CCRect(-5.f, -5.f, width + 10.f, height + 10.f),
                18.f,
                color4(accentColor(), 18),
                color4(0, 0, 0, 0),
                0.f
            );
            sheet->addChild(glow, 0);
        }

        if (auto* back = makeDrawNode()) {
            drawCurvedRect(
                back,
                CCRect(0.f, 0.f, width, height),
                14.f,
                color4(getUISurfaceColor(), 252),
                color4(accentColor(), 96),
                1.f
            );
            back->drawSegment(
                CCPoint(20.f, height - 76.f),
                CCPoint(width - 20.f, height - 76.f),
                1.f,
                panelTint(24)
            );
            back->drawSegment(
                CCPoint(20.f, height - 75.f),
                CCPoint(width - 20.f, height - 75.f),
                1.f,
                color4(accentColor(), 48)
            );
            sheet->addChild(back, 1);
        }

        if (auto* title = makeLabel("credits", true, 0.88f, accentColor())) {
            title->setAnchorPoint(CCPoint(0.f, 0.5f));
            title->setPosition(CCPoint(22.f, height - 30.f));
            sheet->addChild(title, 2);
        }
        if (auto* subtitle = makeLabel("dynamic - russian crew", false, 0.44f, faintTextColor())) {
            subtitle->setAnchorPoint(CCPoint(0.f, 0.5f));
            subtitle->setPosition(CCPoint(22.f, height - 54.f));
            fitLabelToWidth(subtitle, width - 110.f);
            sheet->addChild(subtitle, 2);
        }

        {
            auto close = creditsCloseRect();
            CCPoint local(close.getMinX() - card.getMinX(), close.getMinY() - card.getMinY());
            auto* closeBack = makeDrawNode();
            drawCurvedRect(closeBack, CCRect(0.f, 0.f, close.size.width, close.size.height), 6.f,
                panelTint(10), panelTint(34), 1.f);
            closeBack->setPosition(local);
            sheet->addChild(closeBack, 2);
            if (auto* x = makeLabel("x", false, 0.58f, softTextColor())) {
                x->setPosition(CCPoint(close.size.width * 0.5f, close.size.height * 0.5f));
                closeBack->addChild(x);
            }
        }

        struct CreditRow {
            char const* badge;
            char const* name;
            char const* role;
        };
        CreditRow const rows[5] = {
            { "01", "ВинниGD", "vinny - the mind behind dynamic" },
            { "02", "Кисель", "early testing + ideas" },
            { "03", "Мортис", "orb timing feedback" },
            { "04", "Тёмный дракон", "wave + ship stress tests" },
            { "05", "Славик404", "russian playtest crew" },
        };
        float const rowX = 18.f;
        float const rowW = width - 36.f;
        float const rowH = 40.f;
        float const rowGap = 8.f;
        float const rowBottom = 18.f;
        for (int index = 0; index < 5; ++index) {
            float const y = rowBottom + static_cast<float>(4 - index) * (rowH + rowGap);
            CCRect const rect(rowX, y, rowW, rowH);
            auto* rowBack = makeDrawNode();
            if (!rowBack) {
                continue;
            }
            drawCurvedRect(rowBack, rect, 8.f, panelTint(index % 2 == 0 ? 10 : 6), panelTint(22), 1.f);
            CCPoint const badge(rect.getMinX() + 24.f, rect.getMidY());
            ccColor3B const badgeTint = accentColor();
            rowBack->drawCircle(badge, 13.f, color4(badgeTint, 40), 1.f, color4(badgeTint, 170), 24);
            rowBack->drawSegment(
                CCPoint(rect.getMinX() + 0.5f, rect.getMinY() + 8.f),
                CCPoint(rect.getMinX() + 0.5f, rect.getMaxY() - 8.f),
                2.f,
                color4(badgeTint, 175)
            );
            rowBack->drawSegment(
                CCPoint(rect.getMinX() + 42.f, rect.getMaxY() - 3.f),
                CCPoint(rect.getMaxX() - 10.f, rect.getMaxY() - 3.f),
                1.f,
                color4(badgeTint, 30)
            );
            sheet->addChild(rowBack, 2);

            if (auto* badgeLabel = makeLabel(rows[index].badge, false, 0.34f, badgeTint)) {
                badgeLabel->setPosition(badge);
                sheet->addChild(badgeLabel, 3);
            }
            if (auto* name = makeTTFLabel(rows[index].name, 14, 0.7f, logTextColor())) {
                name->setAnchorPoint(CCPoint(0.f, 0.5f));
                name->setPosition(CCPoint(rect.getMinX() + 46.f, rect.getMidY() + 9.f));
                fitTTFLabelToWidth(name, rect.size.width - 64.f);
                sheet->addChild(name, 3);
            }
            if (auto* role = makeTTFLabel(rows[index].role, 11, 0.62f, softTextColor())) {
                role->setAnchorPoint(CCPoint(0.f, 0.5f));
                role->setPosition(CCPoint(rect.getMinX() + 46.f, rect.getMidY() - 10.f));
                fitTTFLabelToWidth(role, rect.size.width - 64.f);
                sheet->addChild(role, 3);
            }
        }
    }

    void DynamicOverlay::closeCreditsLayer() {
        if (m_creditsLayer) {
            m_creditsLayer->removeFromParentAndCleanup(true);
            m_creditsLayer = nullptr;
        }
    }

    void DynamicOverlay::buildLogLayer() {
        if (!m_deck || m_logLayer) {
            return;
        }
        auto const card = logViewCardRect();
        float const width = card.size.width;
        float const height = card.size.height;

        m_logViewScroll = 0.f;
        m_logLayer = CCNode::create();
        m_logLayer->setContentSize(CCSize(DECK_WIDTH, DECK_HEIGHT));
        m_content->addChild(m_logLayer, 40);

        if (auto* shade = makeDrawNode()) {
            shade->drawRect(
                CCRect(0.f, 0.f, DECK_WIDTH, DECK_HEIGHT),
                color4(4, 5, 8, 196),
                0.f,
                color4(0, 0, 0, 0)
            );
            m_logLayer->addChild(shade, 0);
        }

        auto* sheet = CCNode::create();
        sheet->setPosition(card.origin);
        m_logLayer->addChild(sheet, 1);

        if (auto* glow = makeDrawNode()) {
            drawCurvedRect(
                glow,
                CCRect(-5.f, -5.f, width + 10.f, height + 10.f),
                18.f,
                color4(accentColor(), 18),
                color4(0, 0, 0, 0),
                0.f
            );
            sheet->addChild(glow, 0);
        }

        if (auto* back = makeDrawNode()) {
            drawCurvedRect(
                back,
                CCRect(0.f, 0.f, width, height),
                14.f,
                color4(getUISurfaceColor(), 252),
                color4(accentColor(), 96),
                1.f
            );
            back->drawSegment(
                CCPoint(20.f, height - 56.f),
                CCPoint(width - 20.f, height - 56.f),
                1.f,
                panelTint(24)
            );
            sheet->addChild(back, 1);
        }

        m_logViewTitleLabel = makeLabel("activity log", true, 0.82f, accentColor());
        if (m_logViewTitleLabel) {
            m_logViewTitleLabel->setAnchorPoint(CCPoint(0.f, 0.5f));
            m_logViewTitleLabel->setPosition(CCPoint(24.f, height - 32.f));
            sheet->addChild(m_logViewTitleLabel, 2);
        }

        m_logViewCountLabel = makeLabel("0 events", false, 0.52f, faintTextColor());
        if (m_logViewCountLabel) {
            m_logViewCountLabel->setAnchorPoint(CCPoint(1.f, 0.5f));
            m_logViewCountLabel->setPosition(CCPoint(width - 60.f, height - 32.f));
            fitLabelToWidth(m_logViewCountLabel, 220.f);
            sheet->addChild(m_logViewCountLabel, 2);
        }

        {
            auto close = logViewCloseRect();
            CCPoint local(close.getMinX() - card.getMinX(), close.getMinY() - card.getMinY());
            auto* closeBack = makeDrawNode();
            drawCurvedRect(closeBack, CCRect(0.f, 0.f, close.size.width, close.size.height), 6.f,
                panelTint(10), panelTint(34), 1.f);
            closeBack->setPosition(local);
            sheet->addChild(closeBack, 2);
            if (auto* x = makeLabel("x", false, 0.58f, softTextColor())) {
                x->setPosition(CCPoint(close.size.width * 0.5f, close.size.height * 0.5f));
                closeBack->addChild(x);
            }
        }

        for (int row = 0; row < LOG_VIEW_LINE_COUNT; ++row) {
            auto* line = makeLabel("", false, LOG_VIEW_LINE_SCALE, logTextColor());
            if (!line) {
                continue;
            }
            line->setAnchorPoint(CCPoint(0.f, 0.5f));
            line->setPosition(CCPoint(26.f, height - 74.f - static_cast<float>(row) * LOG_VIEW_LINE_STEP));
            sheet->addChild(line, 3);
            m_logViewLines[row] = line;
        }

        if (auto* hint = makeLabel("drag or scroll to move - tap the list to jump back to latest", false, 0.4f, faintTextColor())) {
            hint->setAnchorPoint(CCPoint(0.5f, 0.5f));
            hint->setPosition(CCPoint(width * 0.5f, 12.f));
            fitLabelToWidth(hint, width - 40.f);
            sheet->addChild(hint, 3);
        }

        m_logViewBar = makeDrawNode();
        sheet->addChild(m_logViewBar, 2);

        refresh();
    }

    void DynamicOverlay::closeLogLayer() {
        m_logViewDragging = false;
        m_logViewDragMoved = false;
        if (m_logLayer) {
            m_logLayer->removeFromParentAndCleanup(true);
            m_logLayer = nullptr;
        }
        for (int row = 0; row < LOG_VIEW_LINE_COUNT; ++row) {
            m_logViewLines[row] = nullptr;
        }
        m_logViewTitleLabel = nullptr;
        m_logViewCountLabel = nullptr;
        m_logViewBar = nullptr;
    }

    void DynamicOverlay::applyChip() {
        if (!m_chipBar) {
            return;
        }
        m_chipBar->clear();
        bool values[CHIP_COUNT] = { state.consoleEnabled, state.waveMarginEnabled, state.showPathEnabled };
        for (int index = 0; index < CHIP_COUNT; ++index) {
            auto cell = chipRect(index);
            float dotX = cell.getMinX() + 12.f;
            float dotY = cell.getMidY();
            bool isOn = values[index];
            float radius = 5.f;
            if (isOn) {
                drawCurvedRect(m_chipBar, cell, radius, color4(accentColor(), 34), color4(accentColor(), 120), 1.f);
                m_chipBar->drawCircle(CCPoint(dotX, dotY), 4.f, color4(accentColor(), 255), 0.f, color4(0, 0, 0, 0), 18);
            }
            else {
                drawCurvedRect(m_chipBar, cell, radius, panelTint(8), panelTint(26), 1.f);
                m_chipBar->drawCircle(CCPoint(dotX, dotY), 3.f, panelTint(70), 0.f, color4(0, 0, 0, 0), 18);
            }
            if (m_chipLabels[index]) {
                m_chipLabels[index]->setColor(isOn ? logTextColor() : softTextColor());
                m_chipLabels[index]->setOpacity(isOn ? 255 : 190);
            }
        }
    }

    void DynamicOverlay::drawGroups() {
        if (!m_groupBar) {
            return;
        }
        // ADR-029 removes the rejected `current` / section captions and plates.
        // The bar and labels stay allocated so existing rebuild code is unchanged.
        m_groupBar->clear();
        for (int index = 0; index < GROUP_LABEL_COUNT; ++index) {
            if (m_groupLabels[index]) {
                m_groupLabels[index]->setVisible(false);
            }
        }
    }

    void DynamicOverlay::applyTab() {
        if (m_activeTab < 0) { m_activeTab = 0; }
        if (m_activeTab >= TAB_COUNT) { m_activeTab = TAB_COUNT - 1; }

        if (m_tabBar) {
            auto slot = tabSlotRect(m_activeTab);
            m_tabIndTarget = slot.getMidY();
            if (m_tabIndX < 0.f || !m_isOpen) {
                m_tabIndX = m_tabIndTarget;
            }
            drawTabIndicator();
        }
        drawGroups();

        for (int index = 0; index < TAB_COUNT; ++index) {
            if (m_tabLabels[index]) {
                bool isActive = index == m_activeTab;
                m_tabLabels[index]->stopAllActions();
                m_tabLabels[index]->setColor(isActive ? onSurfaceStrong() : faintTextColor());
                m_tabLabels[index]->setOpacity(isActive ? 255 : 185);
                if (isActive && m_lastAppliedTab >= 0 && m_lastAppliedTab != m_activeTab && m_isOpen) {
                    m_tabLabels[index]->setOpacity(110);
                    m_tabLabels[index]->runAction(CCFadeTo::create(0.22f, 255));
                }
            }
        }
        m_lastAppliedTab = m_activeTab;
        if (m_modeTicksPane) { m_modeTicksPane->setVisible(m_activeTab == 3); }
        if (m_straightPane) { m_straightPane->setVisible(m_activeTab == 4); }
        if (m_scanPane) { m_scanPane->setVisible(m_activeTab == 5); }

        bool const showLog = m_activeTab == 0;
        bool const showStage = m_activeTab == 1;
        bool const showSettings = m_activeTab == 2;

        if (m_activityLabel) { m_activityLabel->setVisible(showLog); }
        if (m_activityCountLabel) { m_activityCountLabel->setVisible(showLog); }
        for (int i = 0; i < static_cast<int>(sizeof(m_activityLines) / sizeof(m_activityLines[0])); ++i) {
            if (m_activityLines[i]) { m_activityLines[i]->setVisible(showLog); }
        }
        if (m_logHighlight) { m_logHighlight->setVisible(showLog); }
        if (m_copyButton) { m_copyButton->setVisible(showLog); }
        if (m_logExpandButton) { m_logExpandButton->setVisible(showLog); }

        if (m_stageNameLabel) { m_stageNameLabel->setVisible(showStage); }
        if (m_stageIndexLabel) { m_stageIndexLabel->setVisible(showStage); }
        if (m_stageDescriptionLabel) { m_stageDescriptionLabel->setVisible(showStage); }
        for (int i = 0; i < static_cast<int>(sizeof(m_stageNameLabels) / sizeof(m_stageNameLabels[0])); ++i) {
            if (m_stageNameLabels[i]) { m_stageNameLabels[i]->setVisible(showStage); }
        }
        for (int i = 0; i < static_cast<int>(sizeof(m_stageTimingLabels) / sizeof(m_stageTimingLabels[0])); ++i) {
            if (m_stageTimingLabels[i]) { m_stageTimingLabels[i]->setVisible(showStage); }
        }
        if (m_stageTrack) { m_stageTrack->setVisible(showStage); }
        for (int i = 0; i < 2; ++i) {
            bool customShown = showStage && customStageFor(state.stageIndex);
            if (m_tickValueLabels[i]) { m_tickValueLabels[i]->setVisible(showStage && !customShown); }
            if (m_tickInputs[i]) { m_tickInputs[i]->setVisible(customShown); }
        }
        for (int i = 0; i < 8; ++i) {
            if (m_tickBtnLabels[i]) { m_tickBtnLabels[i]->setVisible(showStage && customStageFor(state.stageIndex)); }
        }
        if (m_customAddLabel) { m_customAddLabel->setVisible(showStage); }
        if (m_customRemoveLabel) { m_customRemoveLabel->setVisible(showStage && customStageFor(state.stageIndex)); }
        if (m_stageBackButton) { m_stageBackButton->setVisible(false); }
        if (m_stageForwardButton) { m_stageForwardButton->setVisible(false); }

        if (m_settingsHeaderLabel) { m_settingsHeaderLabel->setVisible(showSettings); }
        if (m_settingsBar) { m_settingsBar->setVisible(showSettings); }
        if (m_themeSwatches) { m_themeSwatches->setVisible(showSettings); }
        if (m_settingsHintLabel) { m_settingsHintLabel->setVisible(showSettings); }
        if (m_modeIconContainer) { m_modeIconContainer->setVisible(showSettings); }
        if (m_importButton) { m_importButton->setVisible(showSettings); }
        if (m_exportButton) { m_exportButton->setVisible(showSettings); }
        if (m_tutorialButton) { m_tutorialButton->setVisible(showSettings); }
        if (m_creditsButton) { m_creditsButton->setVisible(showSettings); }
        if (m_configStatusLabel) { m_configStatusLabel->setVisible(showSettings); }
        for (int i = 0; i < SETTINGS_CELL_COUNT; ++i) {
            if (m_settingsValueLabels[i]) { m_settingsValueLabels[i]->setVisible(showSettings); }
        }
        for (int i = 0; i < 4; ++i) {
            if (m_settingsStepLabels[i]) { m_settingsStepLabels[i]->setVisible(showSettings); }
        }
        for (int i = 0; i < THEME_COUNT; ++i) {
            if (m_settingsThemeLabels[i]) { m_settingsThemeLabels[i]->setVisible(showSettings); }
        }

        if (m_terminalButton) { m_terminalButton->setVisible(false); }
    }

    void DynamicOverlay::refresh() {
        if (!m_deck) {
            return;
        }

        ccColor3B live = state.isEnabled ? accentColor() : warningColor();

        if (m_stageIndexLabel) {
            m_stageIndexLabel->setString(fmt::format("{:02}/{:02}", state.stageIndex + 1, stageTotalCount()).c_str());
            m_stageIndexLabel->setColor(live);
        }
        if (m_stageNameLabel) {
            m_stageNameLabel->setString(stageTitleAt(state.stageIndex).c_str());
            m_stageNameLabel->setColor(state.isEnabled ? onSurfaceStrong() : warningColor());
        }
        if (m_stageDescriptionLabel) {
            std::string description;
            if (!state.isEnabled) {
                description = "raw input / stage disabled";
            }
            else {
                description = fmt::format("{} ticks all  /  {} ticks wave", state.ticksAllModes, state.ticksWaveMode);
                int const waveOverride = modeTicksAt(4);
                if (waveOverride > 0) {
                    description = fmt::format("{} ticks all  /  using {} ticks wave (mode)", state.ticksAllModes, waveOverride);
                }
            }
            m_stageDescriptionLabel->setString(description.c_str());
            m_stageDescriptionLabel->setColor(state.isEnabled ? softTextColor() : warningColor());
        }

        if (m_statusLabel) {
            bool const bufferOn = state.isEnabled && state.orbBufferEnabled;
            m_statusLabel->setString(bufferOn ? "buffer on" : "buffer off");
            m_statusLabel->setColor(bufferOn ? (lightSurface() ? softTextColor() : secondaryColor()) : warningColor());
            fitLabelToWidth(m_statusLabel, BUFFER_WIDTH - 48.f);

            float const centerY = BUFFER_Y + BUFFER_HEIGHT * 0.5f;
            m_statusLabel->setPosition(CCPoint(BUFFER_X + 42.f, centerY));

            drawOrbBeacon(0.f);
        }

        if (m_stageTrack) {
            m_stageTrack->clear();
            drawCurvedRect(
                m_stageTrack,
                CCRect(STAGE_TRACK_X, STAGE_TRACK_Y, STAGE_TRACK_WIDTH, STAGE_TRACK_HEIGHT),
                8.f,
                panelTint(7),
                panelTint(14),
                1.f
            );

            bool selectedAvailable = stageEnabledAt(state.stageIndex);
            ccColor3B selectedTint = selectedAvailable ? accentColor() : warningColor();
            drawCurvedRect(
                m_stageTrack,
                stageCellRect(state.stageIndex),
                6.f,
                color4(selectedTint, 48),
                color4(selectedTint, 150),
                1.f
            );

            for (int index = 0; index < stageTotalCount(); ++index) {
                auto* nameLabel = m_stageNameLabels[index];
                if (!nameLabel) {
                    continue;
                }
                bool isSelected = index == state.stageIndex;
                bool isAvailable = stageEnabledAt(index);
                nameLabel->setColor(isSelected ? onSurfaceStrong() : (isAvailable ? softTextColor() : warningColor()));
                nameLabel->setOpacity(isSelected ? 255 : 205);
            }

            auto* customStage = customStageFor(state.stageIndex);
            bool const isCustomSelected = customStage != nullptr;
            bool const showStageTab = m_activeTab == 1;

            int shownAll = state.isEnabled ? state.ticksAllModes : 0;
            int shownWave = state.isEnabled ? state.ticksWaveMode : 0;
            int barMax = isCustomSelected ? CUSTOM_TICKS_MAX : std::max({ 40, shownAll, shownWave });

            float const barHeight = 8.f;
            m_barTarget[0] = static_cast<float>(shownAll);
            m_barTarget[1] = static_cast<float>(shownWave);
            for (int barIndex = 0; barIndex < 2; ++barIndex) {
                if (m_barShown[barIndex] < 0.f || m_draggingSlider || !m_isOpen) {
                    m_barShown[barIndex] = m_barTarget[barIndex];
                }
            }
            auto drawBar = [&](int barIndex, float y, ccColor3B tint) {
                float const ticks = m_barShown[barIndex];
                drawCurvedRect(
                    m_stageTrack,
                    CCRect(STAGE_BAR_X, y - barHeight * 0.5f, STAGE_BAR_WIDTH, barHeight),
                    3.5f,
                    color4(0, 0, 0, 70),
                    panelTint(22),
                    1.f
                );
                if (ticks > 0.5f) {
                    float ratio = std::clamp(ticks / static_cast<float>(barMax), 0.03f, 1.f);
                    if (isCustomSelected) {
                        ratio = std::clamp(
                            (ticks - static_cast<float>(CUSTOM_TICKS_MIN)) / static_cast<float>(CUSTOM_TICKS_MAX - CUSTOM_TICKS_MIN),
                            0.f, 1.f
                        );
                    }
                    float fillWidth = std::max(8.f, STAGE_BAR_WIDTH * ratio);
                    drawCurvedRect(
                        m_stageTrack,
                        CCRect(STAGE_BAR_X - 1.5f, y - barHeight * 0.5f - 2.5f, fillWidth + 3.f, barHeight + 5.f),
                        5.f,
                        color4(tint, 38),
                        color4(0, 0, 0, 0),
                        0.f
                    );
                    drawCurvedRect(
                        m_stageTrack,
                        CCRect(STAGE_BAR_X, y - barHeight * 0.5f, fillWidth, barHeight),
                        3.5f,
                        color4(tint, 255),
                        color4(tint, 255),
                        1.f
                    );
                    if (fillWidth > 10.f) {
                        drawCurvedRect(
                            m_stageTrack,
                            CCRect(STAGE_BAR_X + 3.f, y - 1.25f, fillWidth - 6.f, 2.5f),
                            1.2f,
                            color4(255, 255, 255, 120),
                            color4(0, 0, 0, 0),
                            0.f
                        );
                    }
                    if (isCustomSelected) {
                        float knobX = STAGE_BAR_X + STAGE_BAR_WIDTH * ratio;
                        drawCurvedRect(
                            m_stageTrack,
                            CCRect(knobX - 5.f, y - 11.f, 10.f, 22.f),
                            4.f,
                            color4(tint, 55),
                            color4(0, 0, 0, 0),
                            0.f
                        );
                        drawCurvedRect(
                            m_stageTrack,
                            CCRect(knobX - 3.f, y - 9.f, 6.f, 18.f),
                            2.8f,
                            color4(235, 255, 250, 255),
                            color4(tint, 255),
                            1.f
                        );
                    }
                }
            };
            drawBar(0, STAGE_TIMING_ALL_Y, live);
            drawBar(1, STAGE_TIMING_WAVE_Y, live);

            for (int row = 0; row < 2; ++row) {
                drawCurvedRect(
                    m_stageTrack,
                    tickValueRect(row),
                    6.f,
                    panelTint(8),
                    panelTint(24),
                    1.f
                );
                if (m_tickValueLabels[row]) {
                    int ticks = row == 0 ? shownAll : shownWave;
                    m_tickValueLabels[row]->setScale(0.62f * getUIFontScale());
                    m_tickValueLabels[row]->setString(state.isEnabled ? fmt::format("{}t", ticks).c_str() : "off");
                    m_tickValueLabels[row]->setColor(state.isEnabled ? onSurfaceStrong() : warningColor());
                    fitLabelToWidth(m_tickValueLabels[row], TICK_VALUE_W - 10.f);
                    m_tickValueLabels[row]->setVisible(showStageTab && !isCustomSelected);
                }
                if (auto* tickInput = m_tickInputs[row]) {
                    bool inputShown = showStageTab && isCustomSelected;
                    tickInput->setVisible(inputShown);
                    if (inputShown && row != m_editingRow) {
                        auto wanted = fmt::format("{}", row == 0 ? customStage->ticksAll : customStage->ticksWave);
                        if (tickInput->getString() != wanted) {
                            m_suppressTickInput = true;
                            tickInput->setString(wanted);
                            m_suppressTickInput = false;
                        }
                    }
                    if (!inputShown && m_editingRow == row) {
                        endTickEdit();
                    }
                }
            }

            for (int row = 0; row < 2; ++row) {
                int ticks = isCustomSelected ? (row == 0 ? customStage->ticksAll : customStage->ticksWave) : 0;
                for (int which = 0; which < 4; ++which) {
                    auto* glyph = m_tickBtnLabels[row * 4 + which];
                    if (glyph) {
                        glyph->setVisible(showStageTab && isCustomSelected);
                    }
                    if (!isCustomSelected) {
                        continue;
                    }
                    bool atLimit = (which < 2) ? ticks <= CUSTOM_TICKS_MIN : ticks >= CUSTOM_TICKS_MAX;
                    drawCurvedRect(
                        m_stageTrack,
                        tickButtonRect(row, which),
                        6.f,
                        color4(accentColor(), atLimit ? 8 : 30),
                        color4(accentColor(), atLimit ? 40 : 95),
                        1.f
                    );
                    if (glyph) {
                        glyph->setOpacity(atLimit ? 90 : 255);
                    }
                }
            }

            bool canAddCustom = stageTotalCount() - STAGE_TABLE_SIZE < CUSTOM_STAGE_LIMIT;
            drawCurvedRect(
                m_stageTrack,
                customAddRect(),
                6.f,
                color4(accentColor(), canAddCustom ? 30 : 8),
                color4(accentColor(), canAddCustom ? 95 : 40),
                1.f
            );
            if (m_customAddLabel) {
                m_customAddLabel->setOpacity(canAddCustom ? 255 : 90);
                m_customAddLabel->setVisible(showStageTab);
            }
            if (isCustomSelected) {
                drawCurvedRect(
                    m_stageTrack,
                    customRemoveRect(),
                    6.f,
                    color4(warningColor(), 26),
                    color4(warningColor(), 95),
                    1.f
                );
            }
            if (m_customRemoveLabel) {
                m_customRemoveLabel->setVisible(showStageTab && isCustomSelected);
            }

            if (m_stageTimingLabels[2]) {
                bool const overSafeTicks = isCustomSelected
                    && (customStage->ticksAll > CUSTOM_TICKS_SAFE || customStage->ticksWave > CUSTOM_TICKS_SAFE);
                m_stageTimingLabels[2]->setColor(overSafeTicks ? warningColor() : faintTextColor());
                if (!state.isEnabled) {
                    m_stageTimingLabels[2]->setString("raw input, nothing is buffered");
                }
                else if (overSafeTicks) {
                    m_stageTimingLabels[2]->setString(fmt::format("warning: over {} ticks may lag or work worse than {}", CUSTOM_TICKS_SAFE, CUSTOM_TICKS_SAFE).c_str());
                }
                else if (isCustomSelected) {
                    m_stageTimingLabels[2]->setString(fmt::format("tune each delay with -5 / -1 / +1 / +5, up to {} ticks", CUSTOM_TICKS_MAX).c_str());
                }
                else if (canAddCustom) {
                    m_stageTimingLabels[2]->setString("tap + custom to make an editable copy of this stage");
                }
                else {
                    m_stageTimingLabels[2]->setString("custom stage limit reached");
                }
            }
        }

        if (m_stageTimingLabels[0]) {
            m_stageTimingLabels[0]->setColor(state.isEnabled ? softTextColor() : warningColor());
        }
        if (m_stageTimingLabels[1]) {
            m_stageTimingLabels[1]->setColor(state.isEnabled ? softTextColor() : warningColor());
        }

        auto logs = recentLogSnapshot();
        if (logs.size() != m_logLastCount) {
            float const delta = static_cast<float>(logs.size()) - static_cast<float>(m_logLastCount);
            if (m_logScroll > 0.f) {
                m_logScroll = std::max(0.f, m_logScroll + delta);
            }
            if (m_logViewScroll > 0.f) {
                m_logViewScroll = std::max(0.f, m_logViewScroll + delta);
            }
            m_logLastCount = logs.size();
        }
        int const logTotal = static_cast<int>(logs.size());
        int const paneMax = std::max(0, logTotal - LOG_LINE_COUNT);
        m_logScroll = std::clamp(m_logScroll, 0.f, static_cast<float>(paneMax));
        int const paneScroll = static_cast<int>(std::lround(m_logScroll));
        int const viewMax = std::max(0, logTotal - LOG_VIEW_LINE_COUNT);
        m_logViewScroll = std::clamp(m_logViewScroll, 0.f, static_cast<float>(viewMax));
        int const viewScroll = static_cast<int>(std::lround(m_logViewScroll));

        if (m_activityCountLabel) {
            m_activityCountLabel->setScale(0.62f * getUIFontScale());
            auto entryWord = logs.size() == 1 ? "event" : "events";
            std::string countText = fmt::format("/ {} {}", logs.size(), entryWord);
            if (paneScroll > 0) {
                countText += fmt::format(" - back {}", paneScroll);
            }
            m_activityCountLabel->setString(countText.c_str());
            float natural = m_activityCountLabel->getContentSize().width * m_activityCountLabel->getScale();
            float right = m_activityCountLabel->getPositionX() + natural;
            float const limit = LOG_EXPAND_X - 12.f;
            if (right > limit && natural > 0.f) {
                float available = limit - m_activityCountLabel->getPositionX();
                if (available > 8.f) {
                    m_activityCountLabel->setScale(m_activityCountLabel->getScale() * available / natural);
                }
            }
        }

        if (m_logHighlight) {
            m_logHighlight->clear();
            ccColor4F none = color4(0, 0, 0, 0);
            if (!logs.empty() && paneScroll == 0) {
                float latestY = LOG_LINE_START_Y - static_cast<float>(LOG_LINE_COUNT - 1) * LOG_LINE_STEP;
                drawCurvedRect(
                    m_logHighlight,
                    CCRect(LOG_X + 5.f, latestY - 13.f, LOG_WIDTH - 10.f, 26.f),
                    5.f,
                    color4(accentColor(), 8),
                    none,
                    0.f
                );
                drawCurvedRect(
                    m_logHighlight,
                    CCRect(LOG_X + 8.f, latestY - 10.f, LOG_WIDTH - 16.f, 20.f),
                    4.f,
                    color4(accentColor(), 22),
                    color4(secondaryColor(), 38),
                    1.f
                );
                m_logHighlight->drawSegment(
                    CCPoint(LOG_X + 9.f, latestY - 9.f),
                    CCPoint(LOG_X + 9.f, latestY + 9.f),
                    2.f,
                    color4(secondaryColor(), 150)
                );
            }
            drawLogScrollbar(
                m_logHighlight,
                CCRect(LOG_X + LOG_WIDTH - 15.f, LOG_PANE_BOTTOM + 2.f, 3.f, LOG_PANE_TOP - LOG_PANE_BOTTOM - 4.f),
                logTotal,
                LOG_LINE_COUNT,
                m_logScroll
            );
        }

        renderLogRows(m_activityLines, LOG_LINE_COUNT, logs, paneScroll, LOG_WIDTH - 130.f, 0.96f * getUIFontScale(), true);
        if (logs.empty() && m_activityLines[0]) {
            auto* line = m_activityLines[0];
            fitLogLine(line, "> no activity yet / inputs will appear here", LOG_WIDTH - 130.f);
            line->setColor(faintTextColor());
        }

        if (m_logLayer) {
            renderLogRows(m_logViewLines, LOG_VIEW_LINE_COUNT, logs, viewScroll, LOG_VIEW_CARD_WIDTH - 70.f, LOG_VIEW_LINE_SCALE * getUIFontScale(), true);
            if (logs.empty() && m_logViewLines[0]) {
                auto* line = m_logViewLines[0];
                fitLogLine(line, "> no activity yet / inputs will appear here", LOG_VIEW_CARD_WIDTH - 70.f);
                line->setColor(faintTextColor());
            }
            if (m_logViewCountLabel) {
                auto entryWord = logs.size() == 1 ? "event" : "events";
                std::string countText = fmt::format("{} {}", logs.size(), entryWord);
                if (viewScroll > 0) {
                    countText += fmt::format(" - back {}", viewScroll);
                }
                m_logViewCountLabel->setString(countText.c_str());
            }
            if (m_logViewBar) {
                m_logViewBar->clear();
                drawLogScrollbar(
                    m_logViewBar,
                    CCRect(LOG_VIEW_CARD_WIDTH - 20.f, 20.f, 3.f, LOG_VIEW_CARD_HEIGHT - 100.f),
                    logTotal,
                    LOG_VIEW_LINE_COUNT,
                    m_logViewScroll
                );
            }
        }

        if (m_settingsValueLabels[0]) {
            bool glowOn = getUIGlow();
            m_settingsValueLabels[0]->setString(glowOn ? "glow  on" : "glow  off");
            m_settingsValueLabels[0]->setColor(glowOn ? logTextColor() : faintTextColor());
        }
        if (m_settingsValueLabels[1]) {
            m_settingsValueLabels[1]->setString(fmt::format("panel  {}%", std::lround(getUIPanelScale() * 100.f)).c_str());
            m_settingsValueLabels[1]->setColor(logTextColor());
        }
        if (m_settingsValueLabels[2]) {
            m_settingsValueLabels[2]->setString(fmt::format("font  {}%", std::lround(getUIFontScale() * 100.f)).c_str());
            m_settingsValueLabels[2]->setColor(logTextColor());
        }

        if (m_straightToggleLabel) {
            m_straightToggleLabel->setString(state.shipStraightEnabled ? "assist  on" : "assist  off");
            m_straightToggleLabel->setColor(state.shipStraightEnabled ? logTextColor() : faintTextColor());
        }
        if (m_straightDialLabels[0]) {
            m_straightDialLabels[0]->setString(fmt::format("strength  {}", shipStraightStrength()).c_str());
            m_straightDialLabels[0]->setColor(logTextColor());
        }
        if (m_straightDetailLabels[0]) {
            m_straightDetailLabels[0]->setString(fmt::format("{}t max", shipStraightNudgeTicks()).c_str());
        }
        if (m_straightDialLabels[1]) {
            m_straightDialLabels[1]->setString(fmt::format("sensitivity  {}", shipStraightSensitivity()).c_str());
            m_straightDialLabels[1]->setColor(logTextColor());
        }
        if (m_straightDetailLabels[1]) {
            m_straightDetailLabels[1]->setString(fmt::format("drift {:.0f}u", shipStraightDriftUnits()).c_str());
        }
        if (m_straightDialLabels[2]) {
            m_straightDialLabels[2]->setString(fmt::format("rhythm  {}", shipStraightRhythm()).c_str());
            m_straightDialLabels[2]->setColor(logTextColor());
        }
        if (m_straightDetailLabels[2]) {
            m_straightDetailLabels[2]->setString(fmt::format("hold {}t / gap {}t", shipStraightHoldMaxTicks(), shipStraightGapMaxTicks()).c_str());
        }
        if (m_straightSummaryLabel) {
            m_straightSummaryLabel->setString(fmt::format("cap {}t  -  drift {:.0f}u  -  gain {:.1f}u{}", shipStraightNudgeTicks(),
                    shipStraightDriftUnits(), shipStraightGainUnits(), state.shipStraightEnabled ? "" : "  -  assist off")
                    .c_str());
            m_straightSummaryLabel->setColor(state.shipStraightEnabled ? softTextColor() : faintTextColor());
        }

        int const modeNow = activeModeIndex();
        for (int index = 0; index < MODE_TICK_COUNT; ++index) {
            int const ticks = modeTicksAt(index);
            if (m_modeTickValueLabels[index]) {
                if (index == 1) {
                    // the ship tile is the straight-fly switch + strength dial (ADR-013)
                    m_modeTickValueLabels[index]->setString(fmt::format("{}  {}", state.shipStraightEnabled ? "on" : "off", shipStraightStrength()).c_str());
                    m_modeTickValueLabels[index]->setColor(state.shipStraightEnabled ? onSurfaceStrong() : faintTextColor());
                }
                // on auto, show what auto actually resolves to. this is the number the
                // buffer uses, so "28t" and "auto Â· 18t" can never be mistaken for each other.
                else if (ticks > 0) {
                    m_modeTickValueLabels[index]->setString(fmt::format("{}t", ticks).c_str());
                }
                else if (state.isEnabled) {
                    m_modeTickValueLabels[index]->setString(fmt::format("auto {}t", stageTicksForMode(index)).c_str());
                }
                else {
                    m_modeTickValueLabels[index]->setString("auto");
                }
                if (index != 1) {
                    m_modeTickValueLabels[index]->setColor(ticks > 0 ? onSurfaceStrong() : faintTextColor());
                }
            }
            if (auto* icon = m_modeTickIcons[index]) {
                bool const isActive = index == modeNow;
                float const baseScale = modeTickIconRect(index).size.width / 40.f;
                icon->setOpacity(isActive ? 255 : 150);
                icon->setScale(baseScale * (isActive ? 1.14f : 1.f));
            }
        }

        int activeMode = -1;
        if (auto* manager = GameManager::get()) {
            if (auto* playLayer = manager->m_playLayer) {
                if (auto* player = playLayer->m_player1) {
                    if (player->m_isShip) { activeMode = 1; }
                    else if (player->m_isBall) { activeMode = 2; }
                    else if (player->m_isBird) { activeMode = 3; }
                    else if (player->m_isDart) { activeMode = 4; }
                    else if (player->m_isRobot) { activeMode = 5; }
                    else if (player->m_isSpider) { activeMode = 6; }
                    else if (player->m_isSwing) { activeMode = 7; }
                    else { activeMode = 0; }
                }
            }
        }
        for (int index = 0; index < static_cast<int>(m_modeIcons.size()); ++index) {
            auto* icon = m_modeIcons[index];
            if (!icon) {
                continue;
            }
            bool isActive = index < static_cast<int>(m_modeSlots.size()) && m_modeSlots[index] == activeMode;
            float baseScale = MODE_ICON_SIZE / 40.f;
            float targetScale = (activeMode < 0) ? baseScale : (isActive ? baseScale : baseScale * 0.8f);
            int targetOpacity = (activeMode < 0) ? 150 : (isActive ? 255 : 100);
            icon->setScale(targetScale);
            icon->setOpacity(targetOpacity);
        }

        if (m_scanPane) {
            auto* manager = GameManager::get();
            auto* playLayer = manager ? manager->m_playLayer : nullptr;
            bool const inLevel = playLayer && playLayer->m_level;
            int const playingKey = inLevel ? levelKeyFor(playLayer->m_level) : 0;
            bool const currentScanned = inLevel && g_scanHasCurrent && playingKey != 0 && playingKey == g_scanCurrent.levelKey;

            std::string levelName = "not in a level";
            if (inLevel) {
                levelName = std::string(playLayer->m_level->m_levelName.c_str());
                if (levelName.empty()) {
                    levelName = "unnamed level";
                }
            }

            std::string statusText = "no level open";
            ccColor3B statusColor = faintTextColor();
            if (inLevel) {
                if (g_scanSucceeded && currentScanned) {
                    statusText = g_scanCached ? "scan successful / saved" : "scan successful";
                    statusColor = accentColor();
                }
                else if (g_scanPending || g_scanFailed) {
                    statusText = g_scanPending ? "scan failed - retrying" : "scan failed";
                    statusColor = warningColor();
                }
                else if (g_scanRunning) {
                    statusText = "scanning...";
                    statusColor = softTextColor();
                }
                else {
                    statusText = "waiting for objects";
                    statusColor = softTextColor();
                }
            }

            if (m_scanLevelLabel) {
                m_scanLevelLabel->setString(levelName.c_str());
                m_scanLevelLabel->setColor(inLevel ? onSurfaceStrong() : faintTextColor());
                m_scanLevelLabel->setScale(0.78f * getUIFontScale());
                fitLabelToWidth(m_scanLevelLabel, CONTENT_WIDTH - 44.f);
            }
            if (m_scanStatusLabel) {
                m_scanStatusLabel->setString(statusText.c_str());
                m_scanStatusLabel->setColor(statusColor);
                m_scanStatusLabel->setScale(0.56f * getUIFontScale());
                fitLabelToWidth(m_scanStatusLabel, 240.f);
            }

            std::string metaText = "one scan per level / saved after success";
            if (currentScanned && g_scanSucceeded) {
                metaText = fmt::format("{} objects / {} orbs / {} pads / {} portals / max x {}", g_scanCurrent.objectCount,
                    g_scanCurrent.orbCount, g_scanCurrent.padCount, g_scanCurrent.portalCount, g_scanCurrent.maxX);
            }
            else if (inLevel && !g_scanLastError.empty()) {
                metaText = g_scanLastError;
            }
            if (m_scanMetaLabel) {
                m_scanMetaLabel->setString(metaText.c_str());
                m_scanMetaLabel->setColor(currentScanned && g_scanSucceeded ? softTextColor() : faintTextColor());
                float const statusWidth = m_scanStatusLabel
                    ? m_scanStatusLabel->getContentSize().width * m_scanStatusLabel->getScale()
                    : 0.f;
                float const available = CONTENT_WIDTH - 44.f - statusWidth - 18.f;
                m_scanMetaLabel->setScale(0.56f * getUIFontScale());
                fitLabelToWidth(m_scanMetaLabel, std::max(60.f, available));
            }

            std::string routeText = "input points and hazards are indexed when a scan succeeds";
            if (currentScanned && g_scanSucceeded) {
                routeText = fmt::format("{} hazards / {} solids / {} route points / indexed in {} ms", g_scanCurrent.hazardCount,
                    g_scanCurrent.solidCount, static_cast<int>(g_scanCurrent.route.size()), g_scanCurrent.durationMs);
            }
            if (m_scanRouteLabel) {
                m_scanRouteLabel->setString(routeText.c_str());
                m_scanRouteLabel->setColor(currentScanned && g_scanSucceeded ? softTextColor() : faintTextColor());
                m_scanRouteLabel->setScale(0.58f * getUIFontScale());
                fitLabelToWidth(m_scanRouteLabel, CONTENT_WIDTH - 44.f);
            }

            std::vector<int> scanOrder;
            scanOrder.reserve(g_levelScans.size());
            for (int index = 0; index < static_cast<int>(g_levelScans.size()); ++index) {
                scanOrder.push_back(index);
            }
            std::sort(scanOrder.begin(), scanOrder.end(), [](int left, int right) {
                int64_t const leftStamp = g_levelScans[static_cast<size_t>(left)].stamp;
                int64_t const rightStamp = g_levelScans[static_cast<size_t>(right)].stamp;
                if (leftStamp != rightStamp) {
                    return leftStamp > rightStamp;
                }
                return g_levelScans[static_cast<size_t>(left)].levelKey < g_levelScans[static_cast<size_t>(right)].levelKey;
            });

            if (m_scanBar) {
                m_scanBar->clear();
                auto const densityRect = scanDensityRect();
                drawCurvedRect(m_scanBar, densityRect, 4.f, panelTint(7), panelTint(18), 1.f);
                bool const hasDensity = currentScanned && g_scanSucceeded;
                float const gap = 3.f;
                float const padding = 6.f;
                float const cellWidth = (densityRect.size.width - padding * 2.f - gap * static_cast<float>(SCAN_DENSITY_BUCKETS - 1))
                    / static_cast<float>(SCAN_DENSITY_BUCKETS);
                float const bottom = densityRect.getMinY() + 3.f;
                float const maxHeight = densityRect.size.height - 6.f;
                for (int index = 0; index < SCAN_DENSITY_BUCKETS; ++index) {
                    float height = 1.5f;
                    if (hasDensity) {
                        height = std::max(1.5f, maxHeight * static_cast<float>(g_scanCurrent.density[index]) / 255.f);
                    }
                    drawCurvedRect(
                        m_scanBar,
                        CCRect(densityRect.getMinX() + padding + static_cast<float>(index) * (cellWidth + gap), bottom, cellWidth, height),
                        1.4f,
                        color4(accentColor(), hasDensity ? 200 : 45),
                        color4(0, 0, 0, 0),
                        0.f
                    );
                }

                m_scanBar->drawSegment(
                    CCPoint(CONTENT_X + 22.f, SCAN_DIVIDER_Y),
                    CCPoint(CONTENT_RIGHT - 22.f, SCAN_DIVIDER_Y),
                    1.f,
                    panelTint(20)
                );

                for (int row = 0; row < SCAN_LIST_ROWS; ++row) {
                    bool const hasRow = row < static_cast<int>(scanOrder.size());
                    bool const isCurrentRow = hasRow && inLevel && playingKey != 0
                        && g_levelScans[static_cast<size_t>(scanOrder[static_cast<size_t>(row)])].levelKey == playingKey;
                    if (isCurrentRow) {
                        drawCurvedRect(
                            m_scanBar,
                            scanRowRect(row),
                            5.f,
                            color4(accentColor(), 22),
                            color4(accentColor(), 70),
                            1.f
                        );
                    }
                }
            }

            for (int row = 0; row < SCAN_LIST_ROWS; ++row) {
                auto* nameLabel = m_scanRowNameLabels[row];
                auto* detailLabel = m_scanRowDetailLabels[row];
                bool const hasRow = row < static_cast<int>(scanOrder.size());
                std::string rowName;
                std::string rowDetail;
                bool rowIsCurrent = false;
                if (hasRow) {
                    auto const& scan = g_levelScans[static_cast<size_t>(scanOrder[static_cast<size_t>(row)])];
                    rowName = scan.name.empty() ? fmt::format("level {}", scan.levelKey) : scan.name;
                    rowDetail = fmt::format("{} objects / {} orbs", scan.objectCount, scan.orbCount);
                    rowIsCurrent = inLevel && playingKey != 0 && scan.levelKey == playingKey;
                }
                else if (row == 0) {
                    rowName = "no levels scanned yet";
                }

                if (nameLabel) {
                    nameLabel->setString(rowName.c_str());
                    nameLabel->setColor(rowIsCurrent ? accentColor() : (hasRow ? softTextColor() : faintTextColor()));
                    nameLabel->setScale(0.62f * getUIFontScale());
                }
                if (detailLabel) {
                    detailLabel->setString(rowDetail.c_str());
                    detailLabel->setColor(faintTextColor());
                    detailLabel->setScale(0.52f * getUIFontScale());
                }
                if (nameLabel && detailLabel) {
                    float const detailWidth = detailLabel->getContentSize().width * detailLabel->getScale();
                    float const nameMax = (CONTENT_RIGHT - 32.f) - (LOG_X + 32.f) - detailWidth - 18.f;
                    fitLabelToWidth(nameLabel, std::max(60.f, nameMax));
                }
            }
        }

        updateHints();
    }

    void DynamicOverlay::updateHints() {
        if (m_stageHintKeyLabel) { m_stageHintKeyLabel->setVisible(false); }
        if (m_stageHintLabel) { m_stageHintLabel->setVisible(false); }
        if (m_panelHintKeyLabel) { m_panelHintKeyLabel->setVisible(false); }
        if (m_panelHintLabel) { m_panelHintLabel->setVisible(false); }
    }
    void DynamicOverlay::tickRefresh(float) {
        if (m_isOpen) {
            refresh();
        }
    }

    void DynamicOverlay::openPanel() {
        if (m_isOpen) {
            return;
        }
        m_isOpen = true;
        m_logScroll = 0.f;
        m_logViewScroll = 0.f;
        m_logLastCount = 0;
        if (!isMobileHost()) {
            setTouchEnabled(true);
        }
        if (m_launcher) {
            m_launcher->setVisible(false);
        }
        pauseForPanel();
        layout();
        refresh();
        schedule(schedule_selector(DynamicOverlay::tickRefresh), 0.2f);
        schedule(schedule_selector(DynamicOverlay::animTick));

        m_scrim->stopAllActions();
        m_scrim->setVisible(true);
        m_scrim->setOpacity(0);
        m_scrim->runAction(CCFadeTo::create(0.16f, 150));

        m_deck->stopAllActions();
        m_deck->setVisible(true);
        m_deck->setScale(m_deckScale * 0.88f);
        m_deck->runAction(CCEaseBackOut::create(CCScaleTo::create(0.3f, m_deckScale)));

        if (m_tutorialStep < 0 && !Mod::get()->getSavedValue<bool>(TUTORIAL_DONE_KEY, false)) {
            startTutorial();
        }
    }

    void DynamicOverlay::closePanel() {
        if (!m_isOpen) {
            return;
        }
        closeCreditsLayer();
        closeLogLayer();
        endTickEdit();
        m_logDragging = false;
        m_logDragMoved = false;
        m_isOpen = false;
        unschedule(schedule_selector(DynamicOverlay::tickRefresh));
        unschedule(schedule_selector(DynamicOverlay::animTick));

        m_deck->stopAllActions();
        m_deck->runAction(CCSequence::create(
            CCEaseIn::create(CCScaleTo::create(0.12f, m_deckScale * 0.96f), 1.5f),
            CallFuncExt::create([this] {
                if (!m_isOpen && m_deck) {
                    m_deck->setVisible(false);
                }
            }),
            nullptr
        ));

        m_scrim->stopAllActions();
        m_scrim->runAction(CCSequence::create(
            CCFadeTo::create(0.12f, 0),
            CallFuncExt::create([this] {
                if (!m_isOpen && m_scrim) {
                    m_scrim->setVisible(false);
                }
            }),
            nullptr
        ));

        resumeFromPanel();

        if (isMobileHost()) {
            if (m_launcher) {
                m_launcher->setVisible(showFloatingLauncher());
                m_launcher->setScale(1.f);
            }
        }
        else {
            runAction(CCSequence::create(
                CCDelayTime::create(0.16f),
                CallFuncExt::create([this] {
                    if (!m_isOpen && !isMobileHost()) {
                        setTouchEnabled(false);
                    }
                }),
                nullptr
            ));
        }
    }

    void DynamicOverlay::togglePanel() {
        if (m_isOpen) {
            closePanel();
        }
        else {
            openPanel();
        }
    }

    void DynamicOverlay::popupTick(float) {
        auto* scene = CCScene::get();
        bool popupOpen = false;
        if (scene) {
            auto* children = scene->getChildren();
            for (unsigned int index = 0; children && index < children->count(); ++index) {
                auto* node = typeinfo_cast<CCNode*>(children->objectAtIndex(index));
                if (node && node != this && node->isVisible() && typeinfo_cast<FLAlertLayer*>(node)) {
                    popupOpen = true;
                    break;
                }
            }
        }
        if (popupOpen == m_popupHidden) {
            return;
        }
        m_popupHidden = popupOpen;
        setVisible(!popupOpen);
        setTouchEnabled(!popupOpen && isMobileHost());
    }

    void DynamicOverlay::onEnter() {
        if (!isMobileHost()) {
            setMouseEnabled(true);
        }
        CCLayer::onEnter();
        layout();
        refresh();
        unschedule(schedule_selector(DynamicOverlay::popupTick));
        schedule(schedule_selector(DynamicOverlay::popupTick), 0.15f);
        if (m_deck && !m_isOpen) {
            m_deck->setVisible(false);
        }
        if (m_scrim && !m_isOpen) {
            m_scrim->setVisible(false);
        }
        if (m_launcher) {
            m_launcher->setVisible(showFloatingLauncher() && !m_isOpen);
        }
    }

    void DynamicOverlay::onExit() {
        m_isOpen = false;
        m_didPauseForPanel = false;
        m_isDraggingLauncher = false;
        m_pressedTarget = HitTarget::None;
        m_pressedNode = nullptr;
        unschedule(schedule_selector(DynamicOverlay::tickRefresh));
        unschedule(schedule_selector(DynamicOverlay::animTick));
        unschedule(schedule_selector(DynamicOverlay::popupTick));
        CCLayer::onExit();
    }

    void DynamicOverlay::registerWithTouchDispatcher() {
        CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -1000, true);
    }

    void DynamicOverlay::scrollWheel(float y, float) {
        if (!m_isOpen || !m_deck || !m_deck->isVisible() || m_tutorialStep >= 0 || m_creditsLayer) {
            return;
        }
        float const steps = std::clamp(y, -3.f, 3.f);
        if (steps == 0.f) {
            return;
        }
        CCPoint const mouse = geode::cocos::getMousePos();
        CCPoint const deckPoint = m_content->convertToNodeSpace(mouse);
        if (!CCRect(-SIDEBAR_WIDTH, 0.f, FRAME_WIDTH, DECK_HEIGHT).containsPoint(deckPoint)) {
            return;
        }
        if (m_logLayer) {
            if (logViewListRect().containsPoint(deckPoint)) {
                m_logViewScroll += steps;
                refresh();
            }
            return;
        }
        if (m_activeTab == 0 && logPaneRect().containsPoint(deckPoint)) {
            m_logScroll += steps;
            refresh();
        }
    }

    bool DynamicOverlay::ccTouchBegan(CCTouch* touch, CCEvent*) {
        if (!touch) {
            return false;
        }
        if (m_isOpen) {
            CCPoint deckPoint = m_content->convertToNodeSpace(touch->getLocation());
            if (m_tutorialStep >= 0) {
                m_pressedTarget = tutorialHit(deckPoint);
                m_pressedStageSlot = -1;
                return true;
            }
            if (m_creditsLayer) {
                m_pressedTarget = HitTarget::None;
                m_pressedStageSlot = -1;
                return true;
            }
            if (m_logLayer) {
                m_pressedTarget = HitTarget::None;
                m_pressedStageSlot = -1;
                m_logViewDragging = false;
                m_logViewDragMoved = false;
                if (logViewListRect().containsPoint(deckPoint)) {
                    m_logViewDragging = true;
                    m_logViewDragStartY = deckPoint.y;
                    m_logViewDragBase = m_logViewScroll;
                }
                return true;
            }
            if (!CCRect(-SIDEBAR_WIDTH, 0.f, FRAME_WIDTH, DECK_HEIGHT).containsPoint(deckPoint)) {
                endTickEdit();
                m_pressedTarget = HitTarget::Outside;
                m_pressedStageSlot = -1;
                return true;
            }
            int stageSlot = -1;
            m_pressedTarget = hitTest(deckPoint, &stageSlot);
            m_pressedStageSlot = stageSlot;
            if (m_pressedTarget == HitTarget::LogPane) {
                m_logDragging = true;
                m_logDragMoved = false;
                m_logDragStartY = deckPoint.y;
                m_logDragBase = m_logScroll;
            }
            if (m_editingRow >= 0 && !(m_pressedTarget == HitTarget::TickField && stageSlot == m_editingRow)) {
                endTickEdit();
            }
            if (m_pressedTarget == HitTarget::TickSlider) {
                m_draggingSlider = true;
                m_sliderRow = stageSlot;
                m_sliderStartedOver = false;
                if (auto* sliderStage = customStageFor(state.stageIndex)) {
                    int startTicks = stageSlot == 0 ? sliderStage->ticksAll : sliderStage->ticksWave;
                    m_sliderStartedOver = startTicks > CUSTOM_TICKS_SAFE;
                }
                setSliderFromX(deckPoint.x);
                return true;
            }
            if (m_pressedTarget != HitTarget::None) {
                pressTarget(m_pressedTarget);
            }
            return true;
        }

        if (isMobileHost() && m_launcher && m_launcher->isVisible()) {
            CCPoint local = convertTouchToNodeSpace(touch);
            float deltaX = local.x - m_launcherPosition.x;
            float deltaY = local.y - m_launcherPosition.y;
            if (deltaX * deltaX + deltaY * deltaY <= 38.f * 38.f) {
                m_isDraggingLauncher = true;
                m_launcherMoved = false;
                m_launcherDragStart = local;
                m_launcherDragOffset = CCPoint(m_launcherPosition.x - local.x, m_launcherPosition.y - local.y);
                m_launcher->stopAllActions();
                m_launcher->setScale(1.08f);
                return true;
            }
        }
        return false;
    }

    void DynamicOverlay::ccTouchMoved(CCTouch* touch, CCEvent*) {
        if (!touch) {
            return;
        }
        if (m_isDraggingLauncher) {
            CCPoint local = convertTouchToNodeSpace(touch);
            float deltaX = local.x - m_launcherDragStart.x;
            float deltaY = local.y - m_launcherDragStart.y;
            if (deltaX * deltaX + deltaY * deltaY > 64.f) {
                m_launcherMoved = true;
            }
            m_launcherPosition = clampLauncherPosition(CCPoint(local.x + m_launcherDragOffset.x, local.y + m_launcherDragOffset.y));
            m_launcher->setPosition(m_launcherPosition);
            return;
        }

        if (m_logViewDragging && m_logLayer) {
            CCPoint deckPoint = m_content->convertToNodeSpace(touch->getLocation());
            float const travel = m_logViewDragStartY - deckPoint.y;
            if (std::abs(travel) > 6.f) {
                m_logViewDragMoved = true;
            }
            if (m_logViewDragMoved) {
                m_logViewScroll = m_logViewDragBase + travel / LOG_VIEW_LINE_STEP;
                refresh();
            }
            return;
        }

        if (m_tutorialStep >= 0 || m_creditsLayer) {
            return;
        }

        if (m_draggingSlider) {
            if (m_isOpen) {
                setSliderFromX(m_content->convertToNodeSpace(touch->getLocation()).x);
            }
            return;
        }

        if (m_logDragging && m_isOpen) {
            CCPoint deckPoint = m_content->convertToNodeSpace(touch->getLocation());
            float const travel = m_logDragStartY - deckPoint.y;
            if (std::abs(travel) > 5.f) {
                m_logDragMoved = true;
            }
            if (m_logDragMoved) {
                m_logScroll = m_logDragBase + travel / LOG_LINE_STEP;
                refresh();
            }
            return;
        }

        if (m_isOpen && m_pressedTarget != HitTarget::None) {
            CCPoint deckPoint = m_content->convertToNodeSpace(touch->getLocation());
            int stageSlot = -1;
            HitTarget target = hitTest(deckPoint, &stageSlot);
            if (target != m_pressedTarget || (target == HitTarget::StageSlot && stageSlot != m_pressedStageSlot)) {
                restorePressed();
                m_pressedTarget = HitTarget::None;
            }
        }
    }

    void DynamicOverlay::ccTouchEnded(CCTouch* touch, CCEvent*) {
        if (!touch) {
            return;
        }
        if (m_isDraggingLauncher) {
            m_isDraggingLauncher = false;
            if (m_launcher) {
                m_launcher->setScale(1.f);
            }
            if (m_launcherMoved) {
                saveLauncherPosition();
            }
            else {
                openPanel();
            }
            return;
        }

        if (m_draggingSlider) {
            int releasedRow = m_sliderRow;
            m_draggingSlider = false;
            m_sliderRow = -1;
            m_pressedTarget = HitTarget::None;
            saveCustomStages();
            if (releasedRow >= 0 && releasedRow <= 1 && !m_sliderStartedOver) {
                if (auto* sliderStage = customStageFor(state.stageIndex)) {
                    int endTicks = releasedRow == 0 ? sliderStage->ticksAll : sliderStage->ticksWave;
                    if (endTicks > CUSTOM_TICKS_SAFE) {
                        showHighTicksPopup();
                    }
                }
            }
            return;
        }

        if (!m_isOpen) {
            restorePressed();
            m_pressedTarget = HitTarget::None;
            return;
        }

        if (m_creditsLayer) {
            CCPoint deckPoint = m_content->convertToNodeSpace(touch->getLocation());
            if (creditsCloseRect().containsPoint(deckPoint) || !creditsCardRect().containsPoint(deckPoint)) {
                closeCreditsLayer();
            }
            return;
        }

        if (m_logLayer) {
            CCPoint deckPoint = m_content->convertToNodeSpace(touch->getLocation());
            bool const wasDragging = m_logViewDragging;
            bool const moved = m_logViewDragMoved;
            m_logViewDragging = false;
            m_logViewDragMoved = false;
            if (logViewCloseRect().containsPoint(deckPoint) || !logViewCardRect().containsPoint(deckPoint)) {
                closeLogLayer();
            }
            else if (wasDragging && !moved) {
                m_logViewScroll = 0.f;
                refresh();
            }
            return;
        }

        if (m_tutorialStep >= 0) {
            HitTarget pressed = m_pressedTarget;
            m_pressedTarget = HitTarget::None;
            if (pressed == HitTarget::TutorialSkip || pressed == HitTarget::TutorialNext) {
                CCPoint deckPoint = m_content->convertToNodeSpace(touch->getLocation());
                if (tutorialHit(deckPoint) == pressed) {
                    if (pressed == HitTarget::TutorialSkip) {
                        finishTutorial();
                    }
                    else {
                        advanceTutorial();
                    }
                }
            }
            return;
        }

        if (m_pressedTarget == HitTarget::LogPane) {
            m_pressedTarget = HitTarget::None;
            bool const moved = m_logDragMoved;
            m_logDragging = false;
            m_logDragMoved = false;
            if (!moved) {
                m_logScroll = 0.f;
                refresh();
            }
            return;
        }

        HitTarget target = m_pressedTarget;
        m_pressedTarget = HitTarget::None;
        if (target == HitTarget::Outside) {
            restorePressed();
            closePanel();
            return;
        }
        if (target != HitTarget::None) {
            CCPoint deckPoint = m_content->convertToNodeSpace(touch->getLocation());
            int stageSlot = -1;
            HitTarget released = hitTest(deckPoint, &stageSlot);
            bool isSameTarget = released == target && (target != HitTarget::StageSlot || stageSlot == m_pressedStageSlot);
            restorePressed();
            if (isSameTarget) {
                performTarget(target);
            }
            return;
        }
        restorePressed();
    }

    void DynamicOverlay::ccTouchCancelled(CCTouch*, CCEvent*) {
        m_logDragging = false;
        m_logDragMoved = false;
        m_logViewDragging = false;
        m_logViewDragMoved = false;
        if (m_draggingSlider) {
            m_draggingSlider = false;
            m_sliderRow = -1;
            saveCustomStages();
        }
        if (m_isDraggingLauncher) {
            m_isDraggingLauncher = false;
            if (m_launcher) {
                m_launcher->setScale(1.f);
            }
            if (m_launcherMoved) {
                saveLauncherPosition();
            }
        }
        restorePressed();
        m_pressedTarget = HitTarget::None;
    }

    DynamicOverlay::HitTarget DynamicOverlay::tutorialHit(CCPoint const& point) const {
        if (m_tutorialStep < 0 || !m_tutCard) {
            return HitTarget::None;
        }
        CCPoint origin = m_tutCard->getPosition();
        bool const isLast = m_tutorialStep >= m_tutTotal - 1;
        if (!isLast && tutorialSkipRect(origin).containsPoint(point)) {
            return HitTarget::TutorialSkip;
        }
        if (tutorialNextRect(origin).containsPoint(point)) {
            return HitTarget::TutorialNext;
        }
        return HitTarget::None;
    }

    void DynamicOverlay::clearTutorialLayer() {
        if (m_tutorialLayer) {
            m_tutorialLayer->removeFromParentAndCleanup(true);
        }
        m_tutorialLayer = nullptr;
        m_tutShade = nullptr;
        m_tutFx = nullptr;
        m_tutCursor = nullptr;
        m_tutProgress = nullptr;
        m_tutCard = nullptr;
        m_tutClosing = false;
    }

    void DynamicOverlay::startTutorial() {
        endTickEdit();
        m_draggingSlider = false;
        clearTutorialLayer();
        m_tutorialStep = 0;
        showTutorialStep();
    }

    void DynamicOverlay::showTutorialStep() {
        auto steps = tutorialSteps();
        if (m_tutorialStep < 0 || m_tutorialStep >= static_cast<int>(steps.size())) {
            return;
        }
        auto const& step = steps[m_tutorialStep];
        int tab = step.tab;
        if (tab >= 0 && tab < TAB_COUNT && tab != m_activeTab) {
            m_activeTab = tab;
            applyTab();
            refresh();
        }
        buildTutorialLayer();
        beginTutorialMotion(step);
    }

    void DynamicOverlay::advanceTutorial() {
        if (m_tutTapNow && !m_tutActionDone) {
            int pendingTab = m_tutKindNow - TF_TAB0;
            if (pendingTab >= 0 && pendingTab < TAB_COUNT && pendingTab != m_activeTab) {
                m_activeTab = pendingTab;
                applyTab();
                refresh();
            }
        }
        ++m_tutorialStep;
        if (m_tutorialStep >= static_cast<int>(tutorialSteps().size())) {
            finishTutorial();
            return;
        }
        showTutorialStep();
    }

    void DynamicOverlay::finishTutorial() {
        m_tutorialStep = -1;
        Mod::get()->setSavedValue<bool>(TUTORIAL_DONE_KEY, true);
        (void)Mod::get()->saveData();
        if (m_tutorialLayer) {
            m_tutClosing = true;
            m_tutKindNow = -1;
            m_tutKind2 = -1;
            m_tutCardTarget = CCPoint(m_tutCardTarget.x, -m_tutCardH - 40.f);
            m_tutSpotTarget = CCRect(-40.f, -40.f, DECK_WIDTH + 80.f, DECK_HEIGHT + 80.f);
        }
    }

    void DynamicOverlay::buildTutorialLayer() {
        auto steps = tutorialSteps();
        if (!m_deck || m_tutorialStep < 0 || m_tutorialStep >= static_cast<int>(steps.size())) {
            return;
        }
        auto const& step = steps[m_tutorialStep];
        bool const isFirst = m_tutorialStep == 0;
        bool const isLast = m_tutorialStep == static_cast<int>(steps.size()) - 1;
        m_tutTotal = static_cast<int>(steps.size());
        float const cardHeight = tutorialCardHeight(step.lines.size());
        m_tutCardH = cardHeight;
        ccColor4F const clear = color4(0, 0, 0, 0);

        if (!m_tutorialLayer) {
            m_tutorialLayer = CCNode::create();
            m_tutorialLayer->setContentSize(CCSize(DECK_WIDTH, DECK_HEIGHT));
            m_content->addChild(m_tutorialLayer, 30);

            m_tutShade = makeDrawNode();
            m_tutorialLayer->addChild(m_tutShade, 0);

            m_tutCard = CCNode::create();
            m_tutorialLayer->addChild(m_tutCard, 3);
            m_tutCard->setPosition(CCPoint((DECK_WIDTH - TUTORIAL_CARD_W) * 0.5f, -cardHeight - 30.f));

            m_tutFx = makeDrawNode();
            m_tutorialLayer->addChild(m_tutFx, 4);

            m_tutCursor = makeDrawNode();
            ccColor4F shadow = color4(0, 0, 0, 90);
            CCPoint headShadow[3] = { CCPoint(2.f, -2.f), CCPoint(2.f, -21.f), CCPoint(15.f, -15.f) };
            m_tutCursor->drawPolygon(headShadow, 3, shadow, 0.f, clear);
            CCPoint head[3] = { CCPoint(0.f, 0.f), CCPoint(0.f, -19.f), CCPoint(13.f, -13.f) };
            m_tutCursor->drawPolygon(head, 3, color4(250, 251, 255, 255), 0.f, clear);
            CCPoint tail[4] = { CCPoint(3.5f, -13.5f), CCPoint(7.5f, -21.5f), CCPoint(11.f, -20.f), CCPoint(8.f, -12.5f) };
            m_tutCursor->drawPolygon(tail, 4, color4(250, 251, 255, 255), 0.f, clear);
            ccColor4F edge = color4(14, 15, 22, 255);
            m_tutCursor->drawSegment(CCPoint(0.f, 0.f), CCPoint(0.f, -19.f), 0.7f, edge);
            m_tutCursor->drawSegment(CCPoint(0.f, -19.f), CCPoint(4.f, -14.6f), 0.7f, edge);
            m_tutCursor->drawSegment(CCPoint(4.f, -14.6f), CCPoint(7.5f, -21.5f), 0.7f, edge);
            m_tutCursor->drawSegment(CCPoint(7.5f, -21.5f), CCPoint(11.f, -20.f), 0.7f, edge);
            m_tutCursor->drawSegment(CCPoint(11.f, -20.f), CCPoint(8.f, -12.8f), 0.7f, edge);
            m_tutCursor->drawSegment(CCPoint(8.f, -12.8f), CCPoint(13.f, -13.f), 0.7f, edge);
            m_tutCursor->drawSegment(CCPoint(13.f, -13.f), CCPoint(0.f, 0.f), 0.7f, edge);
            m_tutCursor->setScale(0.f);
            m_tutCursor->setVisible(false);
            m_tutorialLayer->addChild(m_tutCursor, 5);

            m_tutAlpha = 0.f;
            m_tutClosing = false;
            m_tutTime = 0.f;
            m_tutSpot = CCRect(-40.f, -40.f, DECK_WIDTH + 80.f, DECK_HEIGHT + 80.f);
            m_tutSpotTarget = m_tutSpot;
            m_tutCursorPos = CCPoint(DECK_WIDTH * 0.5f, 140.f);
            m_tutCursor->setPosition(m_tutCursorPos);
            m_tutCursorVis = 0.f;
            m_tutProgShown = 0.f;
            m_tutRippleT = -1.f;
            m_tutKindNow = -1;
            m_tutKind2 = -1;
        }

        m_tutProgress = nullptr;
        m_tutCard->removeAllChildrenWithCleanup(true);
        m_tutCard->setContentSize(CCSize(TUTORIAL_CARD_W, cardHeight));

        auto* glow = makeDrawNode();
        drawCurvedRect(glow, CCRect(-4.f, -4.f, TUTORIAL_CARD_W + 8.f, cardHeight + 8.f), 15.f, clear, color4(accentColor(), 34), 2.f);
        m_tutCard->addChild(glow, 0);
        auto* bg = makeDrawNode();
        drawCurvedRect(bg, CCRect(0.f, 0.f, TUTORIAL_CARD_W, cardHeight), 12.f, color4(getUISurfaceColor(), 250), color4(accentColor(), 150), 1.5f);
        m_tutCard->addChild(bg, 1);

        m_tutProgress = makeDrawNode();
        m_tutCard->addChild(m_tutProgress, 2);

        auto popIn = [](CCNode* node, float delay, float dx, float dy) {
            node->runAction(CCSequence::create(
                CCDelayTime::create(delay),
                CCSpawn::create(
                    CCFadeIn::create(0.24f),
                    CCEaseOut::create(CCMoveBy::create(0.24f, CCPoint(dx, dy)), 2.f),
                    nullptr
                ),
                nullptr
            ));
        };

        if (auto* title = makeLabel(step.title, true, 0.82f, onSurfaceStrong())) {
            title->setAnchorPoint(CCPoint(0.f, 0.5f));
            title->setPosition(CCPoint(26.f - 14.f, cardHeight - 50.f));
            fitLabelToWidth(title, TUTORIAL_CARD_W - 130.f);
            title->setOpacity(0);
            m_tutCard->addChild(title, 3);
            popIn(title, 0.05f, 14.f, 0.f);
        }
        if (auto* counter = makeLabel(fmt::format("{}/{}", m_tutorialStep + 1, steps.size()), false, 0.58f, faintTextColor())) {
            counter->setAnchorPoint(CCPoint(1.f, 0.5f));
            counter->setPosition(CCPoint(TUTORIAL_CARD_W - 24.f, cardHeight - 22.f));
            m_tutCard->addChild(counter, 3);
        }
        float lineY = cardHeight - 76.f;
        float delay = 0.12f;
        for (auto const& line : step.lines) {
            if (auto* label = makeLabel(line, false, 0.6f, softTextColor())) {
                label->setAnchorPoint(CCPoint(0.f, 0.5f));
                label->setPosition(CCPoint(26.f, lineY - 8.f));
                fitLabelToWidth(label, TUTORIAL_CARD_W - 52.f);
                label->setOpacity(0);
                m_tutCard->addChild(label, 3);
                popIn(label, delay, 0.f, 8.f);
            }
            lineY -= 22.f;
            delay += 0.06f;
        }

        CCPoint const zero(0.f, 0.f);
        if (!isLast) {
            makeDeckButton(m_tutCard, tutorialSkipRect(zero), "skip", false, false, 0.62f);
        }
        makeDeckButton(m_tutCard, tutorialNextRect(zero), isLast ? "done" : (isFirst ? "go" : "next"), true, false, 0.62f);
    }

    void DynamicOverlay::beginTutorialMotion(TutorialStep const& step) {
        if (!m_tutorialLayer) {
            return;
        }
        m_tutKindNow = step.cursor;
        m_tutTapNow = step.tap;
        m_tutFocusNow = step.focus;
        m_tutKind2 = step.cursor2;
        m_tutPhase = 0;
        m_tutPhaseTime = 0.f;
        m_tutActionDone = false;

        std::vector<CCRect> avoid;
        for (int kind : { step.cursor, step.focus, step.cursor2 }) {
            if (kind >= 0) {
                avoid.push_back(tutorialRectFor(kind));
            }
        }
        m_tutCardTarget = tutorialPickCardOrigin(m_tutCardH, avoid, step.cursor < 0);

        if (step.cursor >= 0) {
            m_tutSpotTarget = tutorialRectFor(step.cursor);
        }
        else {
            m_tutSpotTarget = tutorialRectFor(step.focus);
            m_tutPhase = 2;
        }
    }

    void DynamicOverlay::tutorialFrame(float dt) {
        if (!m_tutorialLayer || !m_tutShade || !m_tutCard || !m_tutCursor || !m_tutFx) {
            return;
        }
        dt = std::min(dt, 0.05f);
        m_tutTime += dt;
        auto ease = [dt](float current, float target, float rate) {
            return current + (target - current) * (1.f - std::exp(-rate * dt));
        };

        if (m_tutClosing) {
            m_tutAlpha -= dt * 3.2f;
            if (m_tutAlpha <= 0.f) {
                clearTutorialLayer();
                return;
            }
        }
        else if (m_tutAlpha < 1.f) {
            m_tutAlpha = std::min(1.f, m_tutAlpha + dt * 4.f);
        }

        float pressScale = 1.f;
        if (m_tutKindNow >= 0) {
            CCRect targetRect = tutorialRectFor(m_tutKindNow);
            CCPoint goal(targetRect.getMidX(), targetRect.getMidY());
            if (m_tutPhase == 0) {
                float dx = goal.x - m_tutCursorPos.x;
                float dy = goal.y - m_tutCursorPos.y;
                float dist = std::sqrt(dx * dx + dy * dy);
                if (dist < 2.5f) {
                    m_tutCursorPos = goal;
                    m_tutPhaseTime = 0.f;
                    if (m_tutTapNow) {
                        m_tutPhase = 1;
                        m_tutActionDone = false;
                    }
                    else {
                        m_tutPhase = 2;
                        m_tutSpotTarget = tutorialRectFor(m_tutFocusNow);
                    }
                }
                else {
                    float stepLen = std::max(dist * (1.f - std::exp(-6.5f * dt)), 160.f * dt);
                    stepLen = std::min(stepLen, dist);
                    m_tutCursorPos.x += dx / dist * stepLen;
                    m_tutCursorPos.y += dy / dist * stepLen;
                }
            }
            else if (m_tutPhase == 1) {
                m_tutPhaseTime += dt;
                float t = std::clamp(m_tutPhaseTime / 0.3f, 0.f, 1.f);
                pressScale = 1.f - 0.2f * std::sin(3.14159265f * t);
                if (!m_tutActionDone && m_tutPhaseTime >= 0.13f) {
                    m_tutActionDone = true;
                    m_tutRippleT = 0.f;
                    int tab = m_tutKindNow - TF_TAB0;
                    if (tab >= 0 && tab < TAB_COUNT && tab != m_activeTab) {
                        m_activeTab = tab;
                        applyTab();
                        refresh();
                    }
                    m_tutSpotTarget = tutorialRectFor(m_tutFocusNow);
                }
                if (m_tutPhaseTime >= 0.34f) {
                    m_tutPhase = 2;
                    m_tutPhaseTime = 0.f;
                }
            }
            else {
                m_tutPhaseTime += dt;
                if (m_tutKind2 >= 0 && m_tutPhaseTime >= 1.6f) {
                    m_tutKindNow = m_tutKind2;
                    m_tutKind2 = -1;
                    m_tutTapNow = false;
                    m_tutFocusNow = m_tutKindNow;
                    m_tutSpotTarget = tutorialRectFor(m_tutKindNow);
                    m_tutPhase = 0;
                }
            }
        }
        m_tutCursorVis = ease(m_tutCursorVis, m_tutKindNow >= 0 ? 1.f : 0.f, 10.f);
        float bob = (m_tutPhase == 2 && m_tutKindNow >= 0) ? std::sin(m_tutTime * 3.f) * 1.6f : 0.f;
        m_tutCursor->setPosition(CCPoint(m_tutCursorPos.x, m_tutCursorPos.y + bob));
        m_tutCursor->setScale(1.3f * m_tutCursorVis * pressScale);
        m_tutCursor->setVisible(m_tutCursorVis > 0.03f);

        m_tutFx->clear();
        if (m_tutRippleT >= 0.f) {
            m_tutRippleT += dt;
            float t = m_tutRippleT / 0.55f;
            if (t >= 1.f) {
                m_tutRippleT = -1.f;
            }
            else {
                float fade = 1.f - t;
                for (int ring = 0; ring < 2; ++ring) {
                    float ringT = std::clamp(t - 0.18f * static_cast<float>(ring), 0.f, 1.f);
                    if (ringT <= 0.f) {
                        continue;
                    }
                    float radius = 5.f + 30.f * (1.f - (1.f - ringT) * (1.f - ringT));
                    m_tutFx->drawCircle(
                        m_tutCursorPos, radius, color4(0, 0, 0, 0), 2.f,
                        color4(accentColor(), static_cast<int>(200.f * fade * (ring == 0 ? 1.f : 0.6f))), 32
                    );
                }
            }
        }

        m_tutSpot.origin.x = ease(m_tutSpot.origin.x, m_tutSpotTarget.origin.x, 9.f);
        m_tutSpot.origin.y = ease(m_tutSpot.origin.y, m_tutSpotTarget.origin.y, 9.f);
        m_tutSpot.size.width = ease(m_tutSpot.size.width, m_tutSpotTarget.size.width, 9.f);
        m_tutSpot.size.height = ease(m_tutSpot.size.height, m_tutSpotTarget.size.height, 9.f);

        m_tutShade->clear();
        CCRect lit = padRect(m_tutSpot, 7.f);
        float x0 = std::clamp(lit.getMinX(), 0.f, DECK_WIDTH);
        float x1 = std::clamp(lit.getMaxX(), 0.f, DECK_WIDTH);
        float y0 = std::clamp(lit.getMinY(), 0.f, DECK_HEIGHT);
        float y1 = std::clamp(lit.getMaxY(), 0.f, DECK_HEIGHT);
        ccColor4F shadeColor = color4(0, 0, 0, static_cast<int>(168.f * std::clamp(m_tutAlpha, 0.f, 1.f)));
        ccColor4F clearColor = color4(0, 0, 0, 0);
        auto fillBlock = [&](float ax, float ay, float bx, float by) {
            if (bx - ax < 0.5f || by - ay < 0.5f) {
                return;
            }
            CCPoint points[4] = { CCPoint(ax, ay), CCPoint(bx, ay), CCPoint(bx, by), CCPoint(ax, by) };
            m_tutShade->drawPolygon(points, 4, shadeColor, 0.f, clearColor);
        };
        fillBlock(0.f, 0.f, DECK_WIDTH, y0);
        fillBlock(0.f, y1, DECK_WIDTH, DECK_HEIGHT);
        fillBlock(0.f, y0, x0, y1);
        fillBlock(x1, y0, DECK_WIDTH, y1);

        float pulse = 0.5f + 0.5f * std::sin(m_tutTime * 3.4f);
        float alpha = std::clamp(m_tutAlpha, 0.f, 1.f);
        for (int ring = 3; ring >= 1; --ring) {
            float spread = 2.5f * static_cast<float>(ring) + 1.5f * pulse;
            drawCurvedRect(
                m_tutShade, padRect(lit, spread), 9.f + spread, clearColor,
                color4(accentColor(), static_cast<int>(48.f / static_cast<float>(ring) * alpha)), 1.5f
            );
        }
        drawCurvedRect(m_tutShade, lit, 9.f, clearColor, color4(accentColor(), static_cast<int>((150.f + 90.f * pulse) * alpha)), 2.f);

        CCPoint cardPos = m_tutCard->getPosition();
        m_tutCard->setPosition(CCPoint(
            ease(cardPos.x, m_tutCardTarget.x, 10.f),
            ease(cardPos.y, m_tutCardTarget.y, 10.f)
        ));

        if (m_tutProgress) {
            m_tutProgShown = ease(m_tutProgShown, static_cast<float>(std::max(m_tutorialStep, 0)), 9.f);
            m_tutProgress->clear();
            float x = 26.f;
            float y = m_tutCardH - 22.f;
            for (int index = 0; index < m_tutTotal; ++index) {
                float nearness = std::clamp(1.f - std::abs(static_cast<float>(index) - m_tutProgShown), 0.f, 1.f);
                float width = 10.f + 22.f * nearness;
                bool reached = static_cast<float>(index) <= m_tutProgShown + 0.5f;
                drawCurvedRect(
                    m_tutProgress, CCRect(x, y - 2.5f, width, 5.f), 2.5f,
                    reached ? color4(accentColor(), 235) : panelTint(40),
                    color4(0, 0, 0, 0), 0.f
                );
                x += width + 5.f;
            }
        }
    }

    void DynamicOverlay::drawOrbBeacon(float dt) {
        if (!m_statusBeacon) {
            return;
        }
        m_orbTime += std::clamp(dt, 0.f, 0.05f);
        m_orbBurst = std::max(0.f, m_orbBurst - dt * 2.2f);
        m_statusBeacon->clear();

        ccColor4F const clear = color4(0, 0, 0, 0);
        bool const bufferOn = state.isEnabled && state.orbBufferEnabled;
        float const visibility = bufferOn ? 1.f : 0.42f;
        ccColor3B const gold = color3(255, 210, 74);
        ccColor3B const goldHi = color3(255, 246, 207);
        ccColor3B const goldCore = color3(255, 233, 138);
        ccColor3B const goldCenter = color3(255, 251, 230);
        ccColor3B const rippleGold = color3(255, 224, 120);

        drawCurvedRect(
            m_statusBeacon,
            CCRect(BUFFER_X, BUFFER_Y, BUFFER_WIDTH, BUFFER_HEIGHT),
            10.f,
            color4(mixColor(getUISurfaceColor(), accentColor(), 0.10f), 236),
            panelTint(26),
            1.f
        );

        CCPoint const orb(BUFFER_X + 22.f, BUFFER_Y + BUFFER_HEIGHT * 0.5f);
        float const pulse = 1.f + 0.06f * std::sin(m_orbTime * 4.f) + m_orbBurst * 0.25f;

        // Soft radial glow, approximated with a few transparent discs.
        for (int ring = 4; ring >= 1; --ring) {
            float const radius = 8.f + static_cast<float>(ring) * 4.6f * pulse;
            m_statusBeacon->drawCircle(
                orb,
                radius,
                color4(gold, static_cast<int>(18.f * visibility / static_cast<float>(ring))),
                0.f,
                clear,
                24
            );
        }

        // Two continuous ripples; clicking the buffer adds a wider pulse.
        for (int wave = 0; wave < 2; ++wave) {
            float const phase = std::fmod(m_orbTime * 0.9f + static_cast<float>(wave) * 0.5f, 1.f);
            float const radius = 6.f + phase * 17.f + m_orbBurst * 4.f;
            int const alpha = static_cast<int>((1.f - phase) * 80.f * visibility);
            m_statusBeacon->drawCircle(
                orb,
                radius,
                clear,
                radius > 16.f ? 1.f : 1.5f,
                color4(rippleGold, alpha),
                32
            );
        }

        // Deterministic orbiting sparks at different speeds, like the reference.
        for (int index = 0; index < 16; ++index) {
            float const seed = static_cast<float>(index);
            float const direction = index % 2 == 0 ? 0.9f : -0.7f;
            float const angle = seed * 2.3999632f + m_orbTime * direction + m_orbBurst * 0.4f;
            float const radius = 12.5f + 1.5f * std::sin(m_orbTime * 2.f + seed) + m_orbBurst * 3.f;
            CCPoint const point(orb.x + radius * std::cos(angle), orb.y + radius * std::sin(angle));
            float const dot = 0.7f + 0.6f * (0.5f + 0.5f * std::sin(m_orbTime * 3.f + seed));
            int const alpha = static_cast<int>((110.f + 100.f * (0.5f + 0.5f * std::sin(m_orbTime * 3.f + seed))) * visibility);
            m_statusBeacon->drawDot(point, dot, color4(goldHi, alpha));
        }

        m_statusBeacon->drawCircle(
            orb,
            10.f * pulse,
            clear,
            3.f,
            color4(gold, static_cast<int>(255.f * visibility)),
            40
        );
        m_statusBeacon->drawCircle(
            orb,
            12.f * pulse,
            clear,
            1.f,
            color4(goldHi, static_cast<int>(235.f * visibility)),
            40
        );
        m_statusBeacon->drawCircle(
            orb,
            4.6f * pulse,
            color4(goldCore, static_cast<int>(255.f * visibility)),
            0.f,
            clear,
            32
        );
        m_statusBeacon->drawCircle(
            orb,
            2.f * pulse,
            color4(goldCenter, static_cast<int>(255.f * visibility)),
            0.f,
            clear,
            24
        );
    }

    void DynamicOverlay::drawSidebar() {
        if (!m_sidebarBar) {
            return;
        }
        m_sidebarBar->clear();
        ccColor3B const surface = getUISurfaceColor();
        ccColor3B const rail = mixColor(surface, accentColor(), lightSurface() ? 0.10f : 0.16f);

        drawCutRect(
            m_sidebarBar,
            CCRect(-SIDEBAR_WIDTH, 0.f, SIDEBAR_WIDTH, DECK_HEIGHT),
            12.f,
            color4(rail, 244),
            panelTint(32),
            1.f
        );

        for (int index = 0; index < TAB_COUNT; ++index) {
            auto const slot = tabSlotRect(index);
            drawCurvedRect(
                m_sidebarBar,
                CCRect(slot.getMinX() + 2.f, slot.getMinY() + 1.f, slot.size.width - 4.f, slot.size.height - 2.f),
                8.f,
                panelTint(8),
                panelTint(16),
                1.f
            );
        }

        drawCurvedRect(
            m_sidebarBar,
            CCRect(BUFFER_X - 2.f, BUFFER_Y - 2.f, BUFFER_WIDTH + 4.f, BUFFER_HEIGHT + 4.f),
            10.f,
            panelTint(8),
            color4(accentColor(), 50),
            1.f
        );
    }

    void DynamicOverlay::drawTabIndicator() {
        if (!m_tabBar) {
            return;
        }
        m_tabBar->clear();
        drawCurvedRect(
            m_tabBar,
            CCRect(
                SIDE_NAV_X - 6.f,
                m_tabIndX - SIDE_NAV_HEIGHT * 0.5f - 2.f,
                SIDE_NAV_WIDTH + 8.f,
                SIDE_NAV_HEIGHT + 4.f
            ),
            8.f,
            color4(accentColor(), 70),
            color4(secondaryColor(), 110),
            1.f
        );
    }

    void DynamicOverlay::animTick(float dt) {
        if (!m_isOpen) {
            return;
        }
        dt = std::min(dt, 0.05f);
        drawOrbBeacon(dt);

        if (m_tabIndX >= 0.f && std::abs(m_tabIndTarget - m_tabIndX) > 0.05f) {
            m_tabIndX += (m_tabIndTarget - m_tabIndX) * (1.f - std::exp(-16.f * dt));
            if (std::abs(m_tabIndTarget - m_tabIndX) < 0.3f) {
                m_tabIndX = m_tabIndTarget;
            }
            drawTabIndicator();
        }

        bool barsMoving = false;
        for (int index = 0; index < 2; ++index) {
            if (m_barShown[index] < 0.f) {
                continue;
            }
            float diff = m_barTarget[index] - m_barShown[index];
            if (std::abs(diff) > 0.05f) {
                m_barShown[index] += diff * (1.f - std::exp(-14.f * dt));
                if (std::abs(m_barTarget[index] - m_barShown[index]) < 0.08f) {
                    m_barShown[index] = m_barTarget[index];
                }
                barsMoving = true;
            }
        }
        if (barsMoving && m_activeTab == 1 && !m_draggingSlider) {
            refresh();
        }

        tutorialFrame(dt);
    }

    void DynamicOverlay::beginTickEdit(int row) {
        if (row < 0 || row > 1 || !m_tickInputs[row] || !customStageFor(state.stageIndex)) {
            return;
        }
        m_editingRow = row;
        m_suppressTickInput = true;
        m_tickInputs[row]->setString("");
        m_suppressTickInput = false;
        m_tickInputs[row]->focus();
    }

    void DynamicOverlay::endTickEdit() {
        if (m_editingRow >= 0 && m_editingRow < 2 && m_tickInputs[m_editingRow]) {
            m_tickInputs[m_editingRow]->defocus();
        }
        m_editingRow = -1;
    }

    void DynamicOverlay::onTickInput(int row, std::string const& text) {
        if (m_suppressTickInput || row < 0 || row > 1) {
            return;
        }
        auto* custom = customStageFor(state.stageIndex);
        if (!custom || text.empty()) {
            return;
        }
        if (m_editingRow < 0) {
            m_editingRow = row;
        }
        int raw = std::atoi(text.c_str());
        int value = clampCustomTicks(raw);
        if (raw > CUSTOM_TICKS_MAX && m_tickInputs[row]) {
            m_suppressTickInput = true;
            m_tickInputs[row]->setString(fmt::format("{}", value));
            m_suppressTickInput = false;
        }
        int& ticks = row == 0 ? custom->ticksAll : custom->ticksWave;
        if (ticks == value) {
            return;
        }
        if (row == 1 && modeTicksAt(4) > 0) {
            setModeTicksAt(4, 0);
        }
        bool const crossedSafe = ticks <= CUSTOM_TICKS_SAFE && value > CUSTOM_TICKS_SAFE;
        ticks = value;
        saveCustomStages();
        setStageIndex(state.stageIndex);
        if (crossedSafe) {
            endTickEdit();
            showHighTicksPopup();
        }
    }

    void DynamicOverlay::setSliderFromX(float x) {
        auto* custom = customStageFor(state.stageIndex);
        if (!custom || m_sliderRow < 0 || m_sliderRow > 1) {
            return;
        }
        // the stage slider is the authority for its row. if the wave row was pinned by a
        // mode override, dragging the wave slider hands control back to the stage value so
        // the number under the knob is the number that gets used.
        if (m_sliderRow == 1 && modeTicksAt(4) > 0) {
            setModeTicksAt(4, 0);
        }
        float ratio = std::clamp((x - STAGE_BAR_X) / STAGE_BAR_WIDTH, 0.f, 1.f);
        int value = clampCustomTicks(static_cast<int>(std::lround(
            static_cast<float>(CUSTOM_TICKS_MIN) + ratio * static_cast<float>(CUSTOM_TICKS_MAX - CUSTOM_TICKS_MIN)
        )));
        int& ticks = m_sliderRow == 0 ? custom->ticksAll : custom->ticksWave;
        if (ticks == value) {
            return;
        }
        ticks = value;
        setStageIndex(state.stageIndex);
    }

    CCRect DynamicOverlay::statusPillRect() const {
        return CCRect(BUFFER_X, BUFFER_Y, BUFFER_WIDTH, BUFFER_HEIGHT);
    }

    DynamicOverlay::HitTarget DynamicOverlay::hitTest(CCPoint const& point, int* stageSlot) const {
        if (logoRect().containsPoint(point)) {
            return HitTarget::Logo;
        }
        for (int tabIndex = 0; tabIndex < TAB_COUNT; ++tabIndex) {
            if (tabSlotRect(tabIndex).containsPoint(point)) {
                if (stageSlot) { *stageSlot = tabIndex; }
                return HitTarget::Tab;
            }
        }
        if (m_activeTab == 3) {
            for (int index = 0; index < MODE_TICK_COUNT; ++index) {
                for (int which = 0; which < 4; ++which) {
                    if (modeTickStepRect(index, which).containsPoint(point)) {
                        if (stageSlot) { *stageSlot = index * 4 + which; }
                        return HitTarget::ModeTick;
                    }
                }
                if (modeTickCellRect(index).containsPoint(point)) {
                    if (stageSlot) { *stageSlot = index; }
                    return HitTarget::ModeTickReset;
                }
            }
            if (modeTicksAutoRect().containsPoint(point)) {
                return HitTarget::ModeTicksAuto;
            }
        }
        if (m_activeTab == 4) {
            for (int dial = 0; dial < STRAIGHT_DIAL_COUNT; ++dial) {
                for (int which = 0; which < 4; ++which) {
                    if (straightStepRect(dial + 1, which).containsPoint(point)) {
                        if (stageSlot) { *stageSlot = dial * 4 + which; }
                        return HitTarget::StraightStep;
                    }
                }
            }
            if (straightCellRect(0).containsPoint(point)) {
                return HitTarget::StraightToggle;
            }
        }
        if (m_activeTab == 5 && scanRescanRect().containsPoint(point)) {
            return HitTarget::ScanRescan;
        }
        if (m_activeTab == 2 && creditsRect().containsPoint(point)) {
            return HitTarget::Credits;
        }
        if (m_activeTab == 2) {
            for (int index = 0; index < THEME_COUNT; ++index) {
                if (swatchRect(index).containsPoint(point)) {
                    if (stageSlot) { *stageSlot = index; }
                    return HitTarget::Theme;
                }
            }
            for (int cell = 1; cell < SETTINGS_CELL_COUNT; ++cell) {
                for (int which = 0; which < 2; ++which) {
                    if (settingsStepRect(cell, which == 1).containsPoint(point)) {
                        if (stageSlot) { *stageSlot = (cell - 1) * 2 + which; }
                        return HitTarget::SettingsStep;
                    }
                }
            }
            for (int cell = 0; cell < SETTINGS_CELL_COUNT; ++cell) {
                if (settingsCellRect(cell).containsPoint(point)) {
                    if (stageSlot) { *stageSlot = cell; }
                    return HitTarget::SettingsRow;
                }
            }
        }
        if (m_activeTab == 2) {
            if (configButtonRect(0).containsPoint(point)) {
                if (stageSlot) { *stageSlot = 0; }
                return HitTarget::ImportConfig;
            }
            if (configButtonRect(1).containsPoint(point)) {
                if (stageSlot) { *stageSlot = 1; }
                return HitTarget::ExportConfig;
            }
            if (configButtonRect(2).containsPoint(point)) {
                if (stageSlot) { *stageSlot = 2; }
                return HitTarget::ReplayTutorial;
            }
        }
        if (statusPillRect().containsPoint(point)) {
            return HitTarget::BufferToggle;
        }
        if (CCRect(DISMISS_X, DISMISS_Y, DISMISS_WIDTH, DISMISS_HEIGHT).containsPoint(point)) {
            return HitTarget::Dismiss;
        }
        if (m_activeTab == 0 && logExpandRect().containsPoint(point)) {
            return HitTarget::LogExpand;
        }
        if (m_activeTab == 0 && CCRect(COPY_X, COPY_Y, COPY_WIDTH, COPY_HEIGHT).containsPoint(point)) {
            return HitTarget::CopyLog;
        }
        if (m_activeTab == 0 && logPaneRect().containsPoint(point)) {
            return HitTarget::LogPane;
        }
        if (m_activeTab == 1) {
            for (int index = 0; index < stageTotalCount(); ++index) {
                if (stageSlotRect(index).containsPoint(point)) {
                    if (stageSlot) {
                        *stageSlot = index;
                    }
                    return HitTarget::StageSlot;
                }
            }
        }
        if (m_activeTab == 2 && creditsRect().containsPoint(point)) {
            return HitTarget::Credits;
        }
        if (m_activeTab == 1) {
            if (customAddRect().containsPoint(point)) {
                return HitTarget::StageAdd;
            }
            if (customStageFor(state.stageIndex)) {
                if (customRemoveRect().containsPoint(point)) {
                    return HitTarget::CustomRemove;
                }
                for (int row = 0; row < 2; ++row) {
                    if (tickValueRect(row).containsPoint(point)) {
                        if (stageSlot) { *stageSlot = row; }
                        return HitTarget::TickField;
                    }
                }
                for (int row = 0; row < 2; ++row) {
                    if (tickSliderRect(row).containsPoint(point)) {
                        if (stageSlot) { *stageSlot = row; }
                        return HitTarget::TickSlider;
                    }
                }
                for (int row = 0; row < 2; ++row) {
                    for (int which = 0; which < 4; ++which) {
                        if (tickButtonRect(row, which).containsPoint(point)) {
                            if (stageSlot) { *stageSlot = row * 4 + which; }
                            return HitTarget::TickAdjust;
                        }
                    }
                }
            }
        }
        return HitTarget::None;
    }

    CCNode* DynamicOverlay::nodeForTarget(HitTarget target) const {
        switch (target) {
            case HitTarget::Dismiss: return m_dismissButton;
            case HitTarget::StageBack: return m_stageBackButton;
            case HitTarget::StageForward: return m_stageForwardButton;
            case HitTarget::CopyLog: return m_copyButton;
            case HitTarget::LogExpand: return m_logExpandButton;
            case HitTarget::Terminal: return m_terminalButton;
            case HitTarget::ImportConfig: return m_importButton;
            case HitTarget::ExportConfig: return m_exportButton;
            case HitTarget::ReplayTutorial: return m_tutorialButton;
            case HitTarget::Credits: return m_creditsButton;
            case HitTarget::ScanRescan: return m_scanRescanButton;
            case HitTarget::BufferToggle: return m_statusLabel;
            default: return nullptr;
        }
    }

    void DynamicOverlay::pressTarget(HitTarget target) {
        m_pressedNode = nodeForTarget(target);
        if (m_pressedNode) {
            m_pressedNode->stopAllActions();
            m_pressedNode->runAction(CCEaseOut::create(CCScaleTo::create(0.06f, 0.93f), 2.f));
        }
    }

    void DynamicOverlay::restorePressed() {
        if (m_pressedNode) {
            m_pressedNode->stopAllActions();
            m_pressedNode->runAction(CCEaseBackOut::create(CCScaleTo::create(0.12f, 1.f)));
            m_pressedNode = nullptr;
        }
    }

    void DynamicOverlay::performTarget(HitTarget target) {
        // one place that changes a straight-fly dial, so the modes-tab tile and the `straight` tab
        // cannot drift apart in clamping, keys, or logging. dial 0 = strength, 1 = sensitivity,
        // 2 = rhythm.
        auto bumpStraightDial = [this](int dial, int delta) {
            int* slot = dial == 0 ? &state.shipStraightStrength
                : (dial == 1 ? &state.shipStraightSensitivity : &state.shipStraightRhythm);
            char const* key = dial == 0 ? "ship-straight-strength"
                : (dial == 1 ? "ship-straight-sensitivity" : "ship-straight-rhythm");
            char const* name = dial == 0 ? "strength" : (dial == 1 ? "sensitivity" : "rhythm");
            int const before = std::clamp(*slot, 0, 100);
            int const next = std::clamp(before + delta, 0, 100);
            if (next == before) {
                setConfigStatus(fmt::format("straight {} already at {}", name, before).c_str(), true);
                sayShip("straight {} {} (no change)", name, before);
                return;
            }
            *slot = next;
            Mod::get()->setSavedValue<int>(key, next);
            setConfigStatus(fmt::format("straight {} {}", name, next).c_str(), true);
            sayShip("straight {} {} -> {} ({}t max, drift {:.0f}u, hold {}t, gap {}t)", name, before, next, shipStraightNudgeTicks(),
                shipStraightDriftUnits(), shipStraightHoldMaxTicks(), shipStraightGapMaxTicks());
        };

        if (target == HitTarget::Tab) {
            if (m_pressedStageSlot >= 0 && m_pressedStageSlot < TAB_COUNT) {
                m_activeTab = m_pressedStageSlot;
                applyTab();
                refresh();
            }
            return;
        }

        if (target == HitTarget::Theme) {
            if (m_pressedStageSlot >= 0 && m_pressedStageSlot < THEME_COUNT) {
                applyTheme(m_pressedStageSlot);
            }
            return;
        }

        if (target == HitTarget::ImportConfig) {
            importConfig();
            return;
        }

        if (target == HitTarget::ExportConfig) {
            exportConfig();
            return;
        }

        if (target == HitTarget::ModeTick) {
            static int const modeDeltas[4] = { -5, 5, -1, 1 };
            int const index = m_pressedStageSlot / 4;
            int const which = m_pressedStageSlot % 4;
            if (index < 0 || index >= MODE_TICK_COUNT || which < 0 || which >= 4) {
                refresh();
                return;
            }
            if (index == 1) {
                // the ship tile's four steppers are the straight-fly strength dial, not a tick
                // override: -5 / +5 on top, - / + on the bottom.
                bumpStraightDial(0, modeDeltas[which]);
                refresh();
                return;
            }
            int const before = modeTicksAt(index);
            int const base = before > 0 ? before : stageTicksForMode(index);
            int const next = clampModeTicks(base + modeDeltas[which]);
            setModeTicksAt(index, next);
            setConfigStatus(fmt::format("{} -> {} ticks", MODE_TICK_NAMES[index], next).c_str(), true);
            refresh();
            return;
        }

        if (target == HitTarget::ModeTickReset) {
            if (m_pressedStageSlot == 1) {
                // the ship tile is the straight-fly helper switch, not a tick reset (ADR-013)
                state.shipStraightEnabled = !state.shipStraightEnabled;
                Mod::get()->setSavedValue<bool>("ship-straight", state.shipStraightEnabled);
                setConfigStatus(state.shipStraightEnabled
                        ? fmt::format("ship straight fly on (S{}, {}t max)", shipStraightStrength(), shipStraightNudgeTicks()).c_str()
                        : "ship straight fly off",
                    true);
                sayShip("straight {} (S{}, {}t max)", state.shipStraightEnabled ? "on" : "off", shipStraightStrength(), shipStraightNudgeTicks());
                refresh();
                return;
            }
            setModeTicksAt(m_pressedStageSlot, 0);
            refresh();
            return;
        }

        if (target == HitTarget::StraightToggle) {
            state.shipStraightEnabled = !state.shipStraightEnabled;
            Mod::get()->setSavedValue<bool>("ship-straight", state.shipStraightEnabled);
            setConfigStatus(state.shipStraightEnabled
                    ? fmt::format("ship straight fly on (S{}, {}t max)", shipStraightStrength(), shipStraightNudgeTicks()).c_str()
                    : "ship straight fly off",
                true);
            sayShip("straight {} (S{}, {}t max)", state.shipStraightEnabled ? "on" : "off", shipStraightStrength(), shipStraightNudgeTicks());
            refresh();
            return;
        }

        if (target == HitTarget::StraightStep) {
            if (m_pressedStageSlot >= 0 && m_pressedStageSlot < STRAIGHT_DIAL_COUNT * 4) {
                static int const deltas[4] = { -5, -1, 1, 5 };
                int const dial = m_pressedStageSlot / 4;
                int const which = m_pressedStageSlot % 4;
                bumpStraightDial(dial, deltas[which]);
            }
            refresh();
            return;
        }

        if (target == HitTarget::ModeTicksAuto) {
            int const cleared = clearModeTicks();
            if (cleared > 0) {
                setConfigStatus("every mode follows the stage again", true);
            }
            refresh();
            return;
        }

        if (target == HitTarget::ReplayTutorial) {
            startTutorial();
            return;
        }

        if (target == HitTarget::Credits) {
            buildCreditsLayer();
            return;
        }

        if (target == HitTarget::BufferToggle) {
            if (!state.isEnabled) {
                say("buffer is off: the selected stage is raw");
                refresh();
                return;
            }
            state.orbBufferEnabled = !state.orbBufferEnabled;
            m_orbBurst = 1.f;
            Mod::get()->setSavedValue("orb-buffer", state.orbBufferEnabled);
            resetPending();
            auto* gameLayer = GameManager::get()->m_playLayer;
            if (gameLayer && !state.wasAutoReleased && state.hasAppliedInput != state.hasRealInput) {
                sendButton(gameLayer, state.hasRealInput);
            }
            say("orb buffer {}", state.orbBufferEnabled ? "on" : "off");
            refresh();
            return;
        }

        if (target == HitTarget::ScanRescan) {
            requestLevelScanRescan();
            refresh();
            return;
        }

        if (target == HitTarget::LogExpand) {
            buildLogLayer();
            return;
        }

        if (target == HitTarget::SettingsStep) {
            int const slot = m_pressedStageSlot;
            if (slot >= 0 && slot < 4) {
                int const cell = 1 + slot / 2;
                bool const plus = (slot % 2) == 1;
                char const* key = cell == 1 ? "ui-panel-scale" : "ui-font-scale";
                double const low = cell == 1 ? 0.4 : 0.8;
                double const high = 2.0;
                double value = Mod::get()->getSettingValue<double>(key) + (plus ? 0.05 : -0.05);
                value = std::round(std::clamp(value, low, high) * 100.0) / 100.0;
                Mod::get()->setSettingValue<double>(key, value);
                rebuildDeck();
            }
            return;
        }

        if (target == HitTarget::SettingsRow) {
            if (m_pressedStageSlot == 0) {
                Mod::get()->setSettingValue<bool>("ui-glow", !getUIGlow());
                rebuildDeck();
            }
            return;
        }

        switch (target) {
            case HitTarget::StageAdd: {
                loadCustomStages();
                if (static_cast<int>(g_customStages.size()) < CUSTOM_STAGE_LIMIT) {
                    int copyAll = state.isEnabled ? state.ticksAllModes : 8;
                    int copyWave = state.isEnabled ? state.ticksWaveMode : 8;
                    g_customStages.push_back({ clampCustomTicks(copyAll), clampCustomTicks(copyWave) });
                    saveCustomStages();
                    rebuildDeck();
                    setStageIndex(STAGE_TABLE_SIZE + static_cast<int>(g_customStages.size()) - 1);
                }
                break;
            }
            case HitTarget::CustomRemove: {
                loadCustomStages();
                int custom = state.stageIndex - STAGE_TABLE_SIZE;
                if (custom >= 0 && custom < static_cast<int>(g_customStages.size())) {
                    g_customStages.erase(g_customStages.begin() + custom);
                    saveCustomStages();
                    int nextIndex = custom > 0 ? STAGE_TABLE_SIZE + custom - 1 : STAGE_TABLE_SIZE - 2;
                    rebuildDeck();
                    setStageIndex(nextIndex);
                }
                break;
            }
            case HitTarget::Logo:
                startTutorial();
                break;
            case HitTarget::TutorialSkip:
            case HitTarget::TutorialNext:
                break;
            case HitTarget::TickField: {
                beginTickEdit(m_pressedStageSlot);
                break;
            }
            case HitTarget::TickSlider:
                break;
            case HitTarget::TickAdjust: {
                if (auto* custom = customStageFor(state.stageIndex)) {
                    static int const deltas[4] = { -5, -1, 1, 5 };
                    int row = m_pressedStageSlot / 4;
                    int which = m_pressedStageSlot % 4;
                    if (row >= 0 && row < 2 && which >= 0 && which < 4) {
                        if (row == 1 && modeTicksAt(4) > 0) {
                            setModeTicksAt(4, 0);
                        }
                        int& ticks = row == 0 ? custom->ticksAll : custom->ticksWave;
                        int const before = ticks;
                        ticks = clampCustomTicks(ticks + deltas[which]);
                        saveCustomStages();
                        setStageIndex(state.stageIndex);
                        refresh();
                        if (before <= CUSTOM_TICKS_SAFE && ticks > CUSTOM_TICKS_SAFE) {
                            showHighTicksPopup();
                        }
                    }
                }
                break;
            }
            case HitTarget::Dismiss:
                closePanel();
                break;
            case HitTarget::CopyLog:
                copyLog();
                break;
            case HitTarget::Terminal:
                toggleTerminal();
                closePanel();
                break;
            case HitTarget::StageSlot:
                setStageIndex(m_pressedStageSlot);
                break;
            default:
                break;
        }
    }

    void DynamicOverlay::setConfigStatus(std::string const& text, bool ok) {
        if (!m_configStatusLabel) {
            return;
        }
        m_configStatusLabel->stopAllActions();
        m_configStatusLabel->setString(text.c_str());
        m_configStatusLabel->setColor(ok ? accentColor() : warningColor());
        m_configStatusLabel->setOpacity(255);
        m_configStatusLabel->runAction(CCSequence::create(
            CCDelayTime::create(2.0f),
            CallFuncExt::create([this] {
                if (m_configStatusLabel) {
                    m_configStatusLabel->setString("");
                }
            }),
            nullptr
        ));
    }

    void DynamicOverlay::exportConfig() {
        std::string text = fmt::format(
            "{{\"groundHorizonTicks\":{},\"waveHorizonTicks\":{},"
            "\"autoReleaseWindowTicks\":{},\"orbWindowTicks\":{},\"shipMaxDelayTicks\":{},"
            "\"robotHoldTicks\":{},\"shipStraightEnabled\":{},"
            "\"shipStraightStrength\":{},\"shipStraightSensitivity\":{},\"shipStraightRhythm\":{}}}",
            state.groundHorizonTicks,
            state.waveHorizonTicks,
            state.autoReleaseWindowTicks,
            state.orbWindowTicks,
            state.shipMaxDelayTicks,
            robotHoldHistory.pick(20, 50),
            state.shipStraightEnabled ? 1 : 0,
            shipStraightStrength(),
            shipStraightSensitivity(),
            shipStraightRhythm()
        );
        bool ok = geode::utils::clipboard::write(text.c_str());
        setConfigStatus(ok ? "exported to clipboard" : "export failed", ok);
    }

    void DynamicOverlay::importConfig() {
        std::string text = geode::utils::clipboard::read();
        if (text.empty()) {
            setConfigStatus("clipboard is empty", false);
            return;
        }
        if (text.find('{') == std::string::npos) {
            setConfigStatus("clipboard is not a config", false);
            return;
        }

        int value = 0;
        int applied = 0;
        if (jsonInt(text, "groundHorizonTicks", value)) {
            state.groundHorizonTicks = std::clamp(value, 48, 240);
            ++applied;
        }
        if (jsonInt(text, "waveHorizonTicks", value)) {
            state.waveHorizonTicks = std::clamp(value, 20, 160);
            ++applied;
        }
        if (jsonInt(text, "autoReleaseWindowTicks", value)) {
            state.autoReleaseWindowTicks = std::clamp(value, 1, 32);
            ++applied;
        }
        if (jsonInt(text, "orbWindowTicks", value)) {
            state.orbWindowTicks = std::clamp(value, 1, 64);
            ++applied;
        }
        if (jsonInt(text, "shipMaxDelayTicks", value)) {
            state.shipMaxDelayTicks = std::clamp(value, 0, 16);
            ++applied;
        }
        if (jsonInt(text, "robotHoldTicks", value)) {
            robotHoldHistory.add(std::clamp(value, 1, 200));
            ++applied;
        }
        if (jsonInt(text, "shipStraightEnabled", value)) {
            state.shipStraightEnabled = value != 0;
            Mod::get()->setSavedValue<bool>("ship-straight", state.shipStraightEnabled);
            ++applied;
        }
        if (jsonInt(text, "shipStraightStrength", value)) {
            state.shipStraightStrength = std::clamp(value, SHIP_STRAIGHT_STRENGTH_MIN, SHIP_STRAIGHT_STRENGTH_MAX);
            Mod::get()->setSavedValue<int>("ship-straight-strength", state.shipStraightStrength);
            ++applied;
        }
        if (jsonInt(text, "shipStraightSensitivity", value)) {
            state.shipStraightSensitivity = std::clamp(value, 0, 100);
            Mod::get()->setSavedValue<int>("ship-straight-sensitivity", state.shipStraightSensitivity);
            ++applied;
        }
        if (jsonInt(text, "shipStraightRhythm", value)) {
            state.shipStraightRhythm = std::clamp(value, 0, 100);
            Mod::get()->setSavedValue<int>("ship-straight-rhythm", state.shipStraightRhythm);
            ++applied;
        }

        if (applied == 0) {
            setConfigStatus("no known keys found", false);
            return;
        }

        setConfigStatus(fmt::format("imported {} values", applied), true);
        refresh();
    }

    std::vector<int>& DynamicOverlay::modeSlots() {
    if (!m_modeSlotsLoaded) {
        m_modeSlotsLoaded = true;
        m_modeSlots.clear();
        std::string raw = Mod::get()->getSavedValue<std::string>("mode-slots", "");
        std::string piece;
        for (size_t index = 0; index <= raw.size(); ++index) {
            if (index == raw.size() || raw[index] == ',') {
                if (!piece.empty()) {
                    int value = 0;
                    for (size_t d = 0; d < piece.size(); ++d) {
                        char c = piece[d];
                        if (c >= '0' && c <= '9') { value = value * 10 + (c - '0'); }
                    }
                    m_modeSlots.push_back(std::clamp(value, 0, 7));
                }
                piece.clear();
            } else {
                piece += raw[index];
            }
        }
        if (m_modeSlots.empty()) {
            m_modeSlots = { 0, 1, 2, 3, 4, 5, 6, 7 };
        }
    }
    return m_modeSlots;
    }

    void DynamicOverlay::saveModeSlots() {
    std::string raw;
    for (size_t index = 0; index < m_modeSlots.size(); ++index) {
        if (index > 0) { raw += ','; }
        raw += std::to_string(m_modeSlots[index]);
    }
    Mod::get()->setSavedValue<std::string>("mode-slots", raw);
    (void)Mod::get()->saveData();
    }

    void DynamicOverlay::updateModeIcons() {
        if (!m_modeIconContainer) {
            return;
        }
        m_modeIconContainer->removeAllChildrenWithCleanup(true);
        m_modeIcons.clear();

        if (auto* iconBack = makeDrawNode()) {
            int slotCount = static_cast<int>(modeSlots().size());
            for (int index = 0; index <= slotCount; ++index) {
                bool isAdd = index == slotCount;
                drawCurvedRect(
                    iconBack,
                    CCRect(
                        MODE_ICON_STEP * static_cast<float>(index) + 3.f,
                        -MODE_ICON_SIZE * 0.5f - 4.f,
                        MODE_ICON_STEP - 6.f,
                        MODE_ICON_SIZE + 8.f
                    ),
                    5.f,
                    panelTint(isAdd ? 6 : 12),
                    color4(0, 0, 0, 0),
                    0.f
                );
            }
            m_modeIconContainer->addChild(iconBack, -1);
        }

        auto* manager = GameManager::get();
        if (!manager) {
            return;
        }

        int const frames[8] = {
            manager->getPlayerFrame(),
            manager->getPlayerShip(),
            manager->getPlayerBall(),
            manager->getPlayerBird(),
            manager->getPlayerDart(),
            manager->getPlayerRobot(),
            manager->getPlayerSpider(),
            manager->getPlayerSwing(),
        };
        IconType const types[8] = {
            IconType::Cube, IconType::Ship, IconType::Ball, IconType::Ufo,
            IconType::Wave, IconType::Robot, IconType::Spider, IconType::Swing,
        };
        ccColor3B primary = manager->colorForIdx(manager->getPlayerColor());
        ccColor3B secondary = manager->colorForIdx(manager->getPlayerColor2());

        float baseScale = MODE_ICON_SIZE / 40.f;
        auto& slots = modeSlots();
        m_modeIcons.assign(slots.size(), nullptr);
        for (int index = 0; index < static_cast<int>(slots.size()); ++index) {
            int mode = std::clamp(slots[index], 0, 7);
            auto* icon = SimplePlayer::create(1);
            if (!icon) {
                continue;
            }
            icon->updatePlayerFrame(frames[mode], types[mode]);
            icon->setColors(primary, secondary);
            icon->setAnchorPoint(CCPoint(0.5f, 0.5f));
            icon->setScale(baseScale);
            icon->setOpacity(110);
            icon->setPosition(CCPoint(MODE_ICON_STEP * (static_cast<float>(index) + 0.5f), 0.f));
            m_modeIconContainer->addChild(icon);
            m_modeIcons[index] = icon;
        }

        if (auto* plus = makeLabel("+", false, 0.62f, accentColor())) {
            plus->setAnchorPoint(CCPoint(0.5f, 0.5f));
            plus->setPosition(CCPoint(MODE_ICON_STEP * (static_cast<float>(slots.size()) + 0.5f), 0.f));
            m_modeIconContainer->addChild(plus, 2);
        }
    }

    void DynamicOverlay::copyLog() {
        auto logs = recentLogSnapshot();
        std::string text;
        for (auto const& line : logs) {
            text += line;
            text += '\n';
        }
        if (text.empty()) {
            text = "no dynamic activity yet\n";
        }
        bool didCopy = geode::utils::clipboard::write(text.c_str());
        if (m_copyLabel) {
            m_copyLabel->stopAllActions();
            m_copyLabel->setString(didCopy ? "copied" : "failed");
            m_copyLabel->runAction(CCSequence::create(
                CCDelayTime::create(1.2f),
                CallFuncExt::create([this] {
                    if (m_copyLabel) {
                        m_copyLabel->setString("copy");
                    }
                }),
                nullptr
            ));
        }
    }

    void DynamicOverlay::pauseForPanel() {
        m_didPauseForPanel = false;
        auto* gameLayer = GameManager::get()->m_playLayer;
        if (!gameLayer || gameLayer->m_isPaused) {
            return;
        }
        m_didPauseForPanel = true;
        gameLayer->pauseGame(false);
        removePauseLayers();
        runAction(CCSequence::create(
            CCDelayTime::create(0.12f),
            CallFuncExt::create([this] {
                if (m_isOpen) {
                    removePauseLayers();
                }
            }),
            nullptr
        ));
    }

    void DynamicOverlay::resumeFromPanel() {
        if (!m_didPauseForPanel) {
            return;
        }
        m_didPauseForPanel = false;
        removePauseLayers();
        auto* gameLayer = GameManager::get()->m_playLayer;
        if (gameLayer && gameLayer->m_isPaused) {
            gameLayer->resume();
        }
    }

    void DynamicOverlay::removePauseLayers() {
        auto* scene = CCScene::get();
        if (!scene) {
            return;
        }
        for (int attempt = 0; attempt < 4; ++attempt) {
            auto* pauseLayer = findDescendant<PauseLayer>(scene);
            if (!pauseLayer) {
                break;
            }
            pauseLayer->removeFromParentAndCleanup(true);
        }
    }

    DynamicOverlay* activeDynamicOverlay() {
        auto* scene = CCScene::get();
        if (!scene) {
            return nullptr;
        }
        return scene->getChildByType<DynamicOverlay>(0);
    }

    void refreshDynamicOverlay() {
        if (auto* overlay = activeDynamicOverlay()) {
            overlay->refresh();
        }
    }

    void buildDynamicOverlay(CCScene* scene) {
        if (!scene || scene->getChildByType<DynamicOverlay>(0)) {
            return;
        }
        if (typeinfo_cast<CCTransitionScene*>(scene)) {
            return;
        }
        static bool announcedBuild = false;
        if (!announcedBuild) {
            announcedBuild = true;
            log::info("dynamic: building overlay on first scene");
        }
        auto* overlay = DynamicOverlay::create();
        if (!overlay) {
            return;
        }
        overlay->setZOrder(9999);
        scene->addChild(overlay);
        static bool announcedAttach = false;
        if (!announcedAttach) {
            announcedAttach = true;
            log::info("dynamic: overlay attached");
        }
    }

    void queueOverlayBuild(int attempts) {
        Loader::get()->queueInMainThread([attempts] {
            auto* runningScene = CCScene::get();
            if (!runningScene) {
                return;
            }
            if (typeinfo_cast<CCTransitionScene*>(runningScene)) {
                if (attempts < 120) {
                    queueOverlayBuild(attempts + 1);
                }
                return;
            }
            buildDynamicOverlay(runningScene);
        });
    }

    void attachDynamicOverlay(CCScene* scene) {
        if (!scene || scene->getChildByType<DynamicOverlay>(0)) {
            return;
        }
        queueOverlayBuild(0);
    }

    void ensureDynamicOverlay() {
        attachDynamicOverlay(CCScene::get());
    }

    void scheduleDynamicOverlay(CCNode* host) {
        if (!host) {
            return;
        }
        host->runAction(CCSequence::create(
            CCDelayTime::create(0.15f),
            CallFuncExt::create([] {
                ensureDynamicOverlay();
            }),
            nullptr
        ));
    }

    void toggleDynamicPanel() {
        auto* scene = CCScene::get();
        if (!scene || typeinfo_cast<CCTransitionScene*>(scene)) {
            return;
        }
        auto* overlay = scene->getChildByType<DynamicOverlay>(0);
        if (!overlay) {
            buildDynamicOverlay(scene);
            overlay = scene->getChildByType<DynamicOverlay>(0);
        }
        if (overlay) {
            overlay->togglePanel();
        }
    }

    void dropGhosts() {
        for (auto** ghostSlot : { &state.ghostPlayer1, &state.ghostPlayer2 }) {
            if (*ghostSlot) {
                (*ghostSlot)->removeFromParentAndCleanup(true);
                (*ghostSlot)->release();
                *ghostSlot = nullptr;
            }
        }
        if (state.ghostRenderLayer) {
            state.ghostRenderLayer->removeAllChildrenWithCleanup(true);
            state.ghostRenderLayer->release();
            state.ghostRenderLayer = nullptr;
        }
        state.isSimulating = false;
        state.isInjecting = false;
        state.rapidInputTicks = 0;
        state.wasAutoReleased = false;
        state.ghostLayerBaseIndex = 0;
        state.pathDrawNode = nullptr;
        resetPending();
    }

    PlayerObject* makeGhost(PlayLayer* gameLayer) {
        if (!state.ghostRenderLayer) {
            state.ghostRenderLayer = CCLayer::create();
            if (!state.ghostRenderLayer) {
                return nullptr;
            }
            state.ghostRenderLayer->retain();
        }
        auto* ghostPlayer = PlayerObject::create(1, 1, gameLayer, state.ghostRenderLayer, true);
        if (!ghostPlayer) {
            return nullptr;
        }
        ghostPlayer->retain();
        ghostPlayer->setPosition(CCPoint(0.f, 105.f));
        ghostPlayer->setVisible(false);
        ghostPlayer->m_playEffects = false;
        ghostPlayer->unscheduleUpdate();
        ghostPlayer->stopAllActions();
        state.ghostRenderLayer->addChild(ghostPlayer);
        return ghostPlayer;
    }

    void sweepGhostLayer() {
        if (!state.ghostRenderLayer) {
            return;
        }
        auto* childArray = state.ghostRenderLayer->getChildren();
        while (childArray && childArray->count() > state.ghostLayerBaseIndex) {
            auto* childNode = static_cast<CCNode*>(childArray->lastObject());
            state.ghostRenderLayer->removeChild(childNode, true);
        }
    }

    void sendButton(PlayLayer* gameLayer, bool isDown) {
        state.isInjecting = true;
        state.hasAppliedInput = isDown;
        gameLayer->handleButton(isDown, 1, state.isFirstPlayer);
        state.isInjecting = false;
    }

    void pressGhost(PlayerObject* ghostPlayer, bool isDown) {
        (ghostPlayer == state.ghostPlayer1 ? state.isHeld1 : state.isHeld2) = isDown;
        if (isDown) {
            ghostPlayer->pushButton(PlayerButton::Jump);
        }
        else {
            ghostPlayer->releaseButton(PlayerButton::Jump);
        }
    }

    void copyState(PlayerObject* ghostPlayer, PlayerObject* realPlayer) {
        ghostPlayer->setVisible(false);
        ghostPlayer->resetTouchedRings(true);
        ghostPlayer->copyAttributes(realPlayer);
        ghostPlayer->m_playEffects = false;
        ghostPlayer->setScale(realPlayer->getScale());
        ghostPlayer->setContentSize(realPlayer->getContentSize());
        ghostPlayer->m_width = realPlayer->m_width;
        ghostPlayer->m_height = realPlayer->m_height;
        ghostPlayer->m_gravityMod = realPlayer->m_gravityMod;
        ghostPlayer->m_isOnGround = realPlayer->m_isOnGround;
        ghostPlayer->m_isUpsideDown = realPlayer->m_isUpsideDown;
        ghostPlayer->m_vehicleSize = realPlayer->m_vehicleSize;
        ghostPlayer->m_playerSpeed = realPlayer->m_playerSpeed;
        ghostPlayer->m_isDashing = realPlayer->m_isDashing;
        ghostPlayer->m_lastActivatedPortal = realPlayer->m_lastActivatedPortal;
        if (state.ghostHorizontalShift != 0.f) {

            CCPoint position = realPlayer->getPosition();
            position.x += state.ghostHorizontalShift;
            ghostPlayer->setPosition(position);
            ghostPlayer->m_position = position;
        }

        ghostPlayer->m_holdingButtons = realPlayer->m_holdingButtons;
        ghostPlayer->m_jumpBuffered = realPlayer->m_jumpBuffered;
        ghostPlayer->m_wasJumpBuffered = realPlayer->m_wasJumpBuffered;
        ghostPlayer->m_stateJumpBuffered = realPlayer->m_stateJumpBuffered;
        ghostPlayer->m_stateRingJump = realPlayer->m_stateRingJump;
        ghostPlayer->m_stateRingJump2 = realPlayer->m_stateRingJump2;
        ghostPlayer->m_wasRobotJump = realPlayer->m_wasRobotJump;
        ghostPlayer->m_touchedRing = realPlayer->m_touchedRing;
        ghostPlayer->m_touchedPad = realPlayer->m_touchedPad;
        ghostPlayer->m_touchedGravityPortal = realPlayer->m_touchedGravityPortal;
        if (!state.fullSimulationState) {
            return;
        }

        ghostPlayer->m_isShip = realPlayer->m_isShip;
        ghostPlayer->m_isBird = realPlayer->m_isBird;
        ghostPlayer->m_isBall = realPlayer->m_isBall;
        ghostPlayer->m_isDart = realPlayer->m_isDart;
        ghostPlayer->m_isRobot = realPlayer->m_isRobot;
        ghostPlayer->m_isSpider = realPlayer->m_isSpider;
        ghostPlayer->m_isSwing = realPlayer->m_isSwing;
        ghostPlayer->m_isGoingLeft = realPlayer->m_isGoingLeft;
        ghostPlayer->m_isSideways = realPlayer->m_isSideways;
        ghostPlayer->m_reverseSync = realPlayer->m_reverseSync;
        ghostPlayer->m_reverseRelated = realPlayer->m_reverseRelated;
        ghostPlayer->m_maybeReverseSpeed = realPlayer->m_maybeReverseSpeed;
        ghostPlayer->m_maybeReverseAcceleration = realPlayer->m_maybeReverseAcceleration;
        ghostPlayer->m_speedMultiplier = realPlayer->m_speedMultiplier;
        ghostPlayer->m_gravity = realPlayer->m_gravity;
        ghostPlayer->m_yStart = realPlayer->m_yStart;
        ghostPlayer->m_fixGravityBug = realPlayer->m_fixGravityBug;
        ghostPlayer->m_fixRobotJump = realPlayer->m_fixRobotJump;
        ghostPlayer->m_enable22Changes = realPlayer->m_enable22Changes;
        ghostPlayer->m_enableImpulseFix = realPlayer->m_enableImpulseFix;
        ghostPlayer->m_totalTime = realPlayer->m_totalTime;

        ghostPlayer->m_lastPosition = realPlayer->m_lastPosition;
        ghostPlayer->m_yVelocity = realPlayer->m_yVelocity;
        ghostPlayer->m_fallSpeed = realPlayer->m_fallSpeed;
        ghostPlayer->m_yVelocityBeforeSlope = realPlayer->m_yVelocityBeforeSlope;
        ghostPlayer->m_yVelocityRelated = realPlayer->m_yVelocityRelated;
        ghostPlayer->m_yVelocityRelated3 = realPlayer->m_yVelocityRelated3;
        ghostPlayer->m_groundYVelocity = realPlayer->m_groundYVelocity;
        ghostPlayer->m_accelerationOrSpeed = realPlayer->m_accelerationOrSpeed;
        ghostPlayer->m_isAccelerating = realPlayer->m_isAccelerating;
        ghostPlayer->m_maybeIsFalling = realPlayer->m_maybeIsFalling;
        ghostPlayer->m_isOnGround2 = realPlayer->m_isOnGround2;
        ghostPlayer->m_isOnGround3 = realPlayer->m_isOnGround3;
        ghostPlayer->m_isOnGround4 = realPlayer->m_isOnGround4;
        ghostPlayer->m_stateOnGround = realPlayer->m_stateOnGround;
        ghostPlayer->m_lastLandTime = realPlayer->m_lastLandTime;
        ghostPlayer->m_lastJumpTime = realPlayer->m_lastJumpTime;
        ghostPlayer->m_lastFlipTime = realPlayer->m_lastFlipTime;
        ghostPlayer->m_lastSpiderFlipTime = realPlayer->m_lastSpiderFlipTime;
        ghostPlayer->m_lastGroundedPos = realPlayer->m_lastGroundedPos;
        ghostPlayer->m_blackOrbRelated = realPlayer->m_blackOrbRelated;
        ghostPlayer->m_padRingRelated = realPlayer->m_padRingRelated;
        ghostPlayer->m_ringJumpRelated = realPlayer->m_ringJumpRelated;

        ghostPlayer->m_isOnSlope = realPlayer->m_isOnSlope;
        ghostPlayer->m_wasOnSlope = realPlayer->m_wasOnSlope;
        ghostPlayer->m_slopeVelocity = realPlayer->m_slopeVelocity;
        ghostPlayer->m_maybeUpsideDownSlope = realPlayer->m_maybeUpsideDownSlope;
        ghostPlayer->m_currentSlope = realPlayer->m_currentSlope;
        ghostPlayer->m_currentSlope2 = realPlayer->m_currentSlope2;
        ghostPlayer->m_currentPotentialSlope = realPlayer->m_currentPotentialSlope;
        ghostPlayer->m_isCollidingWithSlope = realPlayer->m_isCollidingWithSlope;
        ghostPlayer->m_collidingWithSlopeId = realPlayer->m_collidingWithSlopeId;
        ghostPlayer->m_slopeFlipGravityRelated = realPlayer->m_slopeFlipGravityRelated;
        ghostPlayer->m_slopeAngle = realPlayer->m_slopeAngle;
        ghostPlayer->m_slopeAngleRadians = realPlayer->m_slopeAngleRadians;
        ghostPlayer->m_slopeRotation = realPlayer->m_slopeRotation;
        ghostPlayer->m_currentSlopeYVelocity = realPlayer->m_currentSlopeYVelocity;
        ghostPlayer->m_isCurrentSlopeTop = realPlayer->m_isCurrentSlopeTop;
        ghostPlayer->m_slopeStartTime = realPlayer->m_slopeStartTime;
        ghostPlayer->m_slopeEndTime = realPlayer->m_slopeEndTime;
        ghostPlayer->m_slopeSlidingMaybeRotated = realPlayer->m_slopeSlidingMaybeRotated;
        ghostPlayer->m_stateDartSlide = realPlayer->m_stateDartSlide;

        ghostPlayer->m_lastGroundObject = realPlayer->m_lastGroundObject;
        ghostPlayer->m_preLastGroundObject = realPlayer->m_preLastGroundObject;
        ghostPlayer->m_maybeLastGroundObject = realPlayer->m_maybeLastGroundObject;
        ghostPlayer->m_collidedObject = realPlayer->m_collidedObject;
        ghostPlayer->m_objectSnappedTo = realPlayer->m_objectSnappedTo;
        ghostPlayer->m_snapDistance = realPlayer->m_snapDistance;
        ghostPlayer->m_lastCollisionBottom = realPlayer->m_lastCollisionBottom;
        ghostPlayer->m_lastCollisionTop = realPlayer->m_lastCollisionTop;
        ghostPlayer->m_lastCollisionLeft = realPlayer->m_lastCollisionLeft;
        ghostPlayer->m_lastCollisionRight = realPlayer->m_lastCollisionRight;
        ghostPlayer->m_collidedTopMinY = realPlayer->m_collidedTopMinY;
        ghostPlayer->m_collidedBottomMaxY = realPlayer->m_collidedBottomMaxY;
        ghostPlayer->m_collidedLeftMaxX = realPlayer->m_collidedLeftMaxX;
        ghostPlayer->m_collidedRightMinX = realPlayer->m_collidedRightMinX;

        ghostPlayer->m_dashX = realPlayer->m_dashX;
        ghostPlayer->m_dashY = realPlayer->m_dashY;
        ghostPlayer->m_dashAngle = realPlayer->m_dashAngle;
        ghostPlayer->m_dashStartTime = realPlayer->m_dashStartTime;
        ghostPlayer->m_dashRing = realPlayer->m_dashRing;
        ghostPlayer->m_dashFireFrame = realPlayer->m_dashFireFrame;

        ghostPlayer->m_stateForce = realPlayer->m_stateForce;
        ghostPlayer->m_stateForceVector = realPlayer->m_stateForceVector;
        ghostPlayer->m_affectedByForces = realPlayer->m_affectedByForces;
        ghostPlayer->m_stateBoostX = realPlayer->m_stateBoostX;
        ghostPlayer->m_stateBoostY = realPlayer->m_stateBoostY;
        ghostPlayer->m_stateNoStickX = realPlayer->m_stateNoStickX;
        ghostPlayer->m_stateNoStickY = realPlayer->m_stateNoStickY;
        ghostPlayer->m_stateHitHead = realPlayer->m_stateHitHead;
        ghostPlayer->m_stateFlipGravity = realPlayer->m_stateFlipGravity;
        ghostPlayer->m_stateNoAutoJump = realPlayer->m_stateNoAutoJump;
        ghostPlayer->m_stateScale = realPlayer->m_stateScale;
        ghostPlayer->m_maybeStateForce2 = realPlayer->m_maybeStateForce2;
    }

    InputKind kindOf(PlayerObject* playerObject) {
        if (!playerObject) {
            return InputKind::Other;
        }
        if (playerObject->m_isShip) {
            return InputKind::Ship;
        }
        if (playerObject->m_isDart) {
            return InputKind::Wave;
        }
        return playerObject->m_isRobot ? InputKind::Robot : InputKind::Tap;
    }

    void recordRealInput(bool isDown, PlayerObject* player1) {
        if (isDown) {
            // release -> press is the down phase of the wave the player is really flying. it is
            // what a release decision is missing: a wave release is not "let go and fall until
            // something kills you", it is "fall for this long, then press again".
            if (lastReleaseTick >= 0) {
                if (lastReleaseKind == InputKind::Wave) {
                    waveGapHistory.add(currentTick - lastReleaseTick);
                    markLearnActivity();
                }
                else if (lastReleaseKind == InputKind::Ship) {
                    // ship release -> press is the other half of the ship rhythm. without it the
                    // release side of a ship input had to be simulated as "stay down forever".
                    shipGapHistory.add(currentTick - lastReleaseTick);
                    markLearnActivity();
                }
            }
            lastReleaseTick = -1;
            lastReleaseKind = InputKind::Other;
            lastPressTick = currentTick;
            lastPressKind = kindOf(player1);
            return;
        }
        if (lastPressTick < 0) {
            return;
        }
        int heldTicks = currentTick - lastPressTick;
        lastPressTick = -1;
        if (state.wasAutoReleased) {
            return;
        }
        // only a real release starts a measured down phase; an auto release by the mod is not
        // the player's rhythm and must never teach the profile.
        lastReleaseTick = currentTick;
        lastReleaseKind = lastPressKind;
        switch (lastPressKind) {
            case InputKind::Tap: tapHoldHistory.add(heldTicks); break;
            case InputKind::Robot: robotHoldHistory.add(heldTicks); break;
            case InputKind::Ship: shipHoldHistory.add(heldTicks); break;
            // the up half of a wave tap. paired with the existing release -> press gap it gives the
            // simulator a real alternating gesture instead of pretending the player holds forever.
            case InputKind::Wave: waveHoldHistory.add(heldTicks); break;
            default: return;
        }
        // the hold profile is real memory: persist it so the next session does not start from
        // a guess again. this is the whole point of "passive" - playing is the training.
        markLearnActivity();
    }

    void forgetPress() {
        lastPressTick = -1;
        lastReleaseTick = -1;
        lastReleaseKind = InputKind::Other;
    }

    int holdEstimate() {
        return tapHoldHistory.pick(st(12), 50);
    }

    // the down phase a wave release is judged with. a wave that flies straight has the same
    // time up as down, so the measured release -> press gap is the player's own rhythm; until
    // one has been recorded, a short normal rhythm is used instead of "fall forever".
    int waveGapEstimate() {
        int const gap = waveGapHistory.pick(st(WAVE_DOWN_FALLBACK_TICKS), 50);
        return std::clamp(gap, st(2), st(24));
    }

    // the up half of a wave tap, measured the same passive way. cold start uses a short normal
    // flick; one odd long hold cannot turn the next gesture into a 240-tick "hold forever" run.
    int waveHoldEstimate() {
        int const hold = waveHoldHistory.pick(st(WAVE_UP_FALLBACK_TICKS), 50);
        return std::clamp(hold, st(2), st(24));
    }

    // high-cps wave detection. `rapidInputTicks` catches the burst immediately; the measured hold
    // and gap keep it engaged for the rest of a section after the profile has been learned. a slow
    // deliberate wave keeps the original press behaviour untouched.
    bool isRapidWaveGesture(int gapTicks) {
        if (state.rapidInputTicks >= RAPID_STREAK_COUNT) {
            return true;
        }
        if (waveHoldHistory.sampleCount < 2 || waveGapHistory.sampleCount < 2) {
            return false;
        }
        int const hold = waveHoldHistory.recent(3);
        int const gap = waveGapHistory.recent(3);
        int const maxTicks = st(WAVE_RAPID_MAX_TICKS);
        return hold > 0 && gap > 0 && hold <= maxTicks && gap <= maxTicks && gapTicks <= maxTicks;
    }

    // how long the player really holds a ship press, and how long they stay off between
    // presses. both are clamped the same way the wave down phase is, so one odd mash cannot
    // turn a gesture into an eight-frame hold.
    int shipHoldEstimate() {
        int const hold = shipHoldHistory.pick(st(SHIP_HOLD_FALLBACK_TICKS), 50);
        return std::clamp(hold, st(2), st(24));
    }

    int shipGapEstimate() {
        int const gap = shipGapHistory.pick(st(SHIP_GAP_FALLBACK_TICKS), 50);
        return std::clamp(gap, st(2), st(24));
    }

    void snapshotObject(GameObject* gameObject) {
        if (!gameObject || state.simulationUsedObjects.contains(gameObject)) {
            return;
        }
        state.simulationUsedObjects.insert(gameObject);
        ObjSnapshot snapshot { gameObject, gameObject->m_isActivated, nullptr, false, false, false };
        if (auto* effectObject = typeinfo_cast<EffectGameObject*>(gameObject)) {
            snapshot.effectObject = effectObject;
            snapshot.isActivatedByEffect = effectObject->m_activated;
            snapshot.byPlayer1 = effectObject->m_activatedByPlayer1;
            snapshot.byPlayer2 = effectObject->m_activatedByPlayer2;
        }
        state.objectSnapshots.push_back(snapshot);
    }

    void restoreObjects() {
        for (auto const& snapshot : state.objectSnapshots) {
            snapshot.gameObject->m_isActivated = snapshot.isObjectActivated;
            if (snapshot.effectObject) {
                snapshot.effectObject->m_activated = snapshot.isActivatedByEffect;
                snapshot.effectObject->m_activatedByPlayer1 = snapshot.byPlayer1;
                snapshot.effectObject->m_activatedByPlayer2 = snapshot.byPlayer2;
            }
        }
        state.objectSnapshots.clear();
        state.simulationUsedObjects.clear();
        state.usedOrbs.clear();
        state.simulationDisabledObjects.clear();
    }

    float orbDistanceSq(PlayerObject* playerObject, RingObject* ringObject) {
        if (!playerObject || !ringObject) {
            return 0.f;
        }
        CCPoint pointA = playerObject->getPosition();
        CCPoint pointB = ringObject->getPosition();
        float deltaX = pointA.x - pointB.x;
        float deltaY = pointA.y - pointB.y;
        return deltaX * deltaX + deltaY * deltaY;
    }

    void advanceGhost(PlayerObject* ghostPlayer, float deltaTime) {
        ghostPlayer->update(deltaTime);
        ghostPlayer->updateRotation(deltaTime);
    }

    void clearCollisionLog(CCDictionary* log) {
        if (log) {
            log->removeAllObjects();
        }
    }

    void checkGhost(PlayLayer* gameLayer, PlayerObject* ghostPlayer, float deltaTime) {
        clearCollisionLog(ghostPlayer->m_collisionLogTop);
        clearCollisionLog(ghostPlayer->m_collisionLogBottom);
        clearCollisionLog(ghostPlayer->m_collisionLogLeft);
        clearCollisionLog(ghostPlayer->m_collisionLogRight);
        gameLayer->checkCollisions(ghostPlayer, deltaTime, false);
    }

    bool isFlying(PlayerObject* playerObject) {
        return playerObject->m_isDart || playerObject->m_isBird || playerObject->m_isSwing || playerObject->m_isShip;
    }

    void releaseAutoClicks() {
        if (state.isAuto1) {
            if (!state.isHeld1 && state.ghostPlayer1) {
                state.ghostPlayer1->releaseButton(PlayerButton::Jump);
            }
            state.isAuto1 = false;
        }
        if (state.isAuto2) {
            if (!state.isHeld2 && state.ghostPlayer2) {
                state.ghostPlayer2->releaseButton(PlayerButton::Jump);
            }
            state.isAuto2 = false;
        }
    }

    enum class SimEnd { Ok, StopSearch };

    int simulate(PlayLayer* gameLayer, PlayerObject* player1, PlayerObject* player2, bool isDual, int delayTicks, bool isDown, int horizonTicks, int releaseAfterTicks,
        int stopAtTick, SimEnd* simulationEnd, int secondFlipTicks = -1, double* altitudeDelta = nullptr, int altitudeSampleTick = -1) {
        *simulationEnd = SimEnd::Ok;
        if (!state.ghostPlayer1 || !player1) {
            return horizonTicks;
        }
        state.isSimulating = true;
        state.hasGhostDied = false;
        state.hasGhostHitPortal = false;
        state.firstOrbTouchTick = -1;
        state.orbUseTick = -1;
        state.firstOrbRingObject = nullptr;
        state.firstOrbPlayerObject = nullptr;
        state.orbCloseSampleTick = -1;
        state.orbTouchDistanceSquared = 0.f;
        state.orbCloseDistanceSquared = 0.f;
        state.pressOrbRingObject = nullptr;
        state.inputTickIndex = delayTicks;
        state.isInputDown = isDown;

        bool isSecondPlayer = isDual && state.ghostPlayer2 && player2;
        state.hasRealRectangle = state.ignoreTouchingObjects;
        state.harmlessObjects.clear();
        if (state.hasRealRectangle) {
            state.realRectanglePlayer1 = player1->getObjectRect();
            state.realRectanglePlayer2 = isSecondPlayer ? player2->getObjectRect() : state.realRectanglePlayer1;
        }
        copyState(state.ghostPlayer1, player1);
        if (isSecondPlayer) {
            copyState(state.ghostPlayer2, player2);
        }
        state.isHeld1 = state.isHeld2 = state.hasAppliedInput;
        state.isAuto1 = state.isAuto2 = false;
        state.autoReleaseTick1 = state.autoReleaseTick2 = -1;

        // straight-fly sampling: the ghost starts at the real ship's altitude, and the helper
        // compares where each candidate plan puts it at the same absolute tick (same distance
        // down the level), not after "delay + horizon" ticks.
        double const startAltitude = state.ghostPlayer1->getPositionY();
        bool hasAltitudeSample = false;
        double sampledAltitude = 0.0;
        // the absolute tick the line is read at. the straight helper passes a short window so it
        // can judge a nudge before the full evasion window is over; every other caller leaves this
        // at "last tick of the window", which is the original behaviour.
        int const altitudeSampleAt = altitudeSampleTick >= 0 ? altitudeSampleTick : horizonTicks - 1;

        bool isWaveRun = player1->m_isDart || (isSecondPlayer && player2->m_isDart);
        if (state.waveVersion46Mode && isWaveRun && delayTicks > 0 && !state.skipPressHandling) {
            pressGhost(state.ghostPlayer1, !isDown);
            if (isSecondPlayer) {
                pressGhost(state.ghostPlayer2, !isDown);
            }
        }

        float deltaTime = state.tickLengthSeconds;
        if (!(deltaTime > 0.001f && deltaTime < 2.0f)) {
            deltaTime = TICK_DURATION;
        }

        bool isFlyingRun = isFlying(player1) || (isDual && player2 && isFlying(player2));
        bool isSpiderRun = state.spiderSmartEnabled && (player1->m_isSpider || (isSecondPlayer && player2->m_isSpider));
        bool isUfoRun = state.ufoSmartEnabled && (player1->m_isBird || (isSecondPlayer && player2->m_isBird));

        bool isBallRun = state.ballSmartEnabled && (player1->m_isBall || (isSecondPlayer && player2->m_isBall));
        int totalTicks = delayTicks + horizonTicks;
        int survivedTicks = 0;
        int airborneTicks = 0;
        int groundedTicks = 0;
        int currentTick = 0;
        bool hasDied = false;
        bool isSafe = false;
        // the tick the optional second flip (release -> press, or press -> release) lands on.
        // it is scheduled from the moment the first flip actually happens, so a dash-extended
        // hold does not put the second half of the gesture on top of the first.
        int secondFlipAtTick = -1;

        for (; currentTick < totalTicks; ++currentTick) {
            state.simulationTick = currentTick;

            if (state.autoReleaseTick1 >= 0 && currentTick >= state.autoReleaseTick1) {
                if (!state.isHeld1 && state.ghostPlayer1->m_touchingRings->count() > 0) {
                    state.ghostPlayer1->pushButton(PlayerButton::Jump);
                    state.isAuto1 = true;
                }
                state.autoReleaseTick1 = -2;
            }
            if (isSecondPlayer && state.autoReleaseTick2 >= 0 && currentTick >= state.autoReleaseTick2) {
                if (!state.isHeld2 && state.ghostPlayer2->m_touchingRings->count() > 0) {
                    state.ghostPlayer2->pushButton(PlayerButton::Jump);
                    state.isAuto2 = true;
                }
                state.autoReleaseTick2 = -2;
            }
            if (currentTick == delayTicks && !state.skipPressHandling) {
                pressGhost(state.ghostPlayer1, isDown);
                if (isSecondPlayer) {
                    pressGhost(state.ghostPlayer2, isDown);
                }
            }
            if (state.retapTick >= 0) {
                int tick = currentTick - delayTicks - state.retapTick;
                if (tick == 0 || tick == 1 || tick == 5) {
                    pressGhost(state.ghostPlayer1, tick == 1);
                    if (isSecondPlayer) {
                        pressGhost(state.ghostPlayer2, tick == 1);
                    }
                }
            }
            if (isDown && releaseAfterTicks >= 0 && currentTick == delayTicks + releaseAfterTicks && state.ghostPlayer1->m_isDashing) {
                ++releaseAfterTicks;
            }
            if (releaseAfterTicks >= 0 && currentTick == delayTicks + releaseAfterTicks) {
                pressGhost(state.ghostPlayer1, !isDown);
                if (isSecondPlayer) {
                    pressGhost(state.ghostPlayer2, !isDown);
                }
                if (secondFlipTicks >= 0) {
                    secondFlipAtTick = currentTick + secondFlipTicks;
                }
            }
            if (secondFlipAtTick >= 0 && currentTick >= secondFlipAtTick) {
                secondFlipAtTick = -1;
                pressGhost(state.ghostPlayer1, isDown);
                if (isSecondPlayer) {
                    pressGhost(state.ghostPlayer2, isDown);
                }
            }
            float stepDeltaTime = currentTick == 0 ? deltaTime * state.firstStepScale : deltaTime;
            advanceGhost(state.ghostPlayer1, stepDeltaTime);
            if (isSecondPlayer) {
                advanceGhost(state.ghostPlayer2, stepDeltaTime);
            }
            releaseAutoClicks();
            if (state.hasGhostDied) {
                hasDied = true;
                break;
            }
            // one altitude sample per run, at the same absolute tick for raw and every candidate,
            // so a straight-fly nudge is judged against the line the player is already holding.
            // (defaults to the last tick of the window: a delay-0 run stops at horizonTicks - 1)
            if (currentTick == altitudeSampleAt) {
                hasAltitudeSample = true;
                sampledAltitude = state.ghostPlayer1->getPositionY();
            }

            checkGhost(gameLayer, state.ghostPlayer1, deltaTime);
            if (!state.hasGhostDied && isSecondPlayer) {
                checkGhost(gameLayer, state.ghostPlayer2, deltaTime);
            }

            if (state.firstOrbRingObject && state.firstOrbPlayerObject && state.orbCloseSampleTick >= 0) {
                float orbDistanceSquared = orbDistanceSq(state.firstOrbPlayerObject, state.firstOrbRingObject);
                if (orbDistanceSquared < state.orbCloseDistanceSquared) {
                    state.orbCloseDistanceSquared = orbDistanceSquared;
                    state.orbCloseSampleTick = state.simulationTick;
                }
            }
            if (state.isProbing) {
                for (auto& probeOrb : state.probeOrbCandidates) {
                    float probeDistanceSquared = orbDistanceSq(probeOrb.playerObject, probeOrb.ringObject);
                    if (probeDistanceSquared < probeOrb.closeDistanceSquared) {
                        probeOrb.closeDistanceSquared = probeDistanceSquared;
                        probeOrb.closeSampleTick = state.simulationTick;
                    }
                }
            }
            if (state.hasGhostDied) {
                hasDied = true;
                break;
            }
            if (state.hasGhostHitPortal) {
                isSafe = currentTick >= delayTicks;
                break;
            }

            if (currentTick < delayTicks) {
                continue;
            }
            ++survivedTicks;
            if (survivedTicks >= stopAtTick) {
                break;
            }
            if (isFlyingRun && !isUfoRun) {
                continue;
            }

            bool isGrounded = state.ghostPlayer1->m_isOnGround && (!isSecondPlayer || state.ghostPlayer2->m_isOnGround);
            if (isGrounded && airborneTicks > 0) {
                state.hasBaseTouchedDown = true;
            }
            if (!isGrounded) {
                ++airborneTicks;
                groundedTicks = 0;
                continue;
            }

            if (isSpiderRun) {
                if (++groundedTicks >= st(SPIDER_LAND_TICKS)) {
                    isSafe = true;
                    break;
                }
                continue;
            }
            if (airborneTicks < st(isBallRun ? 2 : 6)) {
                continue;
            }
            if (groundedTicks > st(isBallRun ? 8 : 2)) {
                isSafe = true;
                break;
            }
            ++groundedTicks;
        }

        if ((hasDied || state.hasGhostHitPortal) && currentTick < delayTicks) {
            *simulationEnd = SimEnd::StopSearch;
        }
        if (altitudeDelta) {
            // a run that never reached the sample tick (died, hit a portal, or stopped early)
            // must not read as "zero drift": NaN makes the straight-fly helper reject it.
            *altitudeDelta = hasAltitudeSample ? sampledAltitude - startAltitude : std::numeric_limits<double>::quiet_NaN();
        }
        restoreObjects();
        state.ghostPlayer1->stopAllActions();
        if (state.ghostPlayer2) {
            state.ghostPlayer2->stopAllActions();
        }
        state.isSimulating = false;
        return isSafe ? horizonTicks : survivedTicks;
    }

    int simulateOrbs(PlayLayer* gameLayer, PlayerObject* player1, PlayerObject* player2, bool isDual, int delayTicks, bool isDown, int horizonTicks, int releaseAfterTicks,
        int stopAtTick, SimEnd* simulationEnd, int secondFlipTicks = -1, double* altitudeDelta = nullptr, int altitudeSampleTick = -1) {
        state.orbClickEnabled = state.assumeOrbHits;
        state.hasTouchedOrb = false;
        int isFirst = simulate(gameLayer, player1, player2, isDual, delayTicks, isDown, horizonTicks, releaseAfterTicks, stopAtTick, simulationEnd, secondFlipTicks,
            altitudeDelta, altitudeSampleTick);
        if (!state.orbBufferEnabled || !state.tryBothOrbCandidates || !state.smartEnabled || !state.hasTouchedOrb || isFirst >= stopAtTick) {
            return isFirst;
        }
        SimEnd firstSimulationEnd = *simulationEnd;
        state.orbClickEnabled = !state.assumeOrbHits;
        int isSecondPlayer = simulate(gameLayer, player1, player2, isDual, delayTicks, isDown, horizonTicks, releaseAfterTicks, stopAtTick, simulationEnd, secondFlipTicks);
        state.orbClickEnabled = state.assumeOrbHits;
        if (firstSimulationEnd != SimEnd::StopSearch) {
            *simulationEnd = SimEnd::Ok;
        }
        int bestResult = std::max(isFirst, isSecondPlayer);

        if (state.orbBufferEnabled && state.orbTimingEnabled && bestResult < stopAtTick) {
            for (int lateTick : { 3, 6 }) {
                state.orbClickEnabled = true;
                state.orbClickDelayTicks = st(lateTick);
                SimEnd lateSimulationEnd;
                int candidateResult = simulate(gameLayer, player1, player2, isDual, delayTicks, isDown, horizonTicks, releaseAfterTicks, stopAtTick, &lateSimulationEnd, secondFlipTicks);
                state.orbClickDelayTicks = 0;
                state.orbClickEnabled = state.assumeOrbHits;
                bestResult = std::max(bestResult, candidateResult);
                if (bestResult >= stopAtTick) {
                    break;
                }
            }
        }
        return bestResult;
    }

    double clickMs() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clickStartTime).count();
    }

    int fullWaveHorizon(bool isMini) {
        int waveHorizonTicks = std::clamp(state.waveHorizonTicks, 20, 160);
        return st(isMini ? std::max(20, waveHorizonTicks - 15) : waveHorizonTicks);
    }

    float orbSpriteRadius(RingObject* ringObject) {
        if (!ringObject) {
            return 16.f;
        }
        CCSize spriteSize = ringObject->getScaledContentSize();
        float orbRadius = std::max(spriteSize.width, spriteSize.height) * 0.5f;
        return std::max(orbRadius, 12.f);
    }

    int alignOrb(PlayLayer* gameLayer, PlayerObject* player1, PlayerObject* player2, bool isDual, int horizonTicks, int releaseAfterTicks, int learnedDelay, int* landingTick,
        bool pressUnderRing = false) {
        int searchWindowTicks = st(std::clamp(state.orbWindowTicks, 1, 64));
        int foundTick = 0;
        SimEnd simulationEnd;
        state.orbClickEnabled = false;
        state.wasOrbTiming = false;
        state.wasOrbReach = false;
        state.wasOrbInRange = false;
        state.intendedOrbObject = nullptr;

        if (learnedDelay > 0 && learnedDelay <= searchWindowTicks) {
            int learnedResult = simulate(gameLayer, player1, player2, isDual, learnedDelay, true, horizonTicks, releaseAfterTicks, horizonTicks, &simulationEnd);
            if (state.pressOrbRingObject && learnedResult >= horizonTicks) {
                *landingTick = learnedResult;
                state.intendedOrbObject = state.pressOrbRingObject;
                state.orbClickEnabled = state.assumeOrbHits;
                return learnedDelay;
            }
        }

        state.probeOrbCandidates.clear();
        state.isProbing = true;
        state.skipPressHandling = true;
        // the probe is also the only measurement of how far ahead the next ring sits. run it over a
        // wider reach than the decision window so a press that is simply too early to reach shows up
        // as "OUT OF RANGE" instead of finding nothing and looking like there is no orb at all.
        int const probeWindowTicks = std::max(searchWindowTicks + 2, st(96));
        simulate(gameLayer, player1, player2, isDual, 0, true, probeWindowTicks, -1, probeWindowTicks, &simulationEnd);
        state.skipPressHandling = false;
        state.isProbing = false;
        std::string traceText;

        int nearestContactTick = -1;
        for (auto const& probeOrb : state.probeOrbCandidates) {
            if (probeOrb.closeSampleTick >= 0 && (nearestContactTick < 0 || probeOrb.closeSampleTick < nearestContactTick)) {
                nearestContactTick = probeOrb.closeSampleTick;
            }
        }

        int currentResult = simulate(gameLayer, player1, player2, isDual, 0, true, horizonTicks, releaseAfterTicks, horizonTicks, &simulationEnd);
        RingObject* currentRing = state.pressOrbRingObject;
        bool currentUsesOrb = currentRing != nullptr;
        int maxDelayTicks = searchWindowTicks + st(4);
        // a shift is only worth applying when it is a real save: full-horizon survival at any
        // reach, or a large survival gain from inside the normal decision window. anything smaller
        // is a coin flip, and moving a click that already connects for a coin flip is exactly what
        // "the buffering is inaccurate" means.
        int const confidentGainTicks = st(ORB_CONFIDENT_GAIN_TICKS);
        auto isConfidentShift = [&](int resultTicks, int delayTicks) {
            return resultTicks >= horizonTicks || (delayTicks <= maxDelayTicks && resultTicks >= currentResult + confidentGainTicks);
        };
        // only dud presses are candidates for a shift. once the press has carried the player past
        // right-after-the-press, the click connected and the player owns the next input.
        int const rescueMaxResultTicks = st(ORB_RESCUE_MAX_RESULT_TICKS);
        bool const pressIsDud = currentResult < rescueMaxResultTicks;

        // An in-range press fires the orb on the exact tick it arrives. "click already lands" is
        // therefore not enough: a press on the first usable tick can survive a few ticks and still
        // launch too early. When the ring is already under the click, sweep the whole orb reach for
        // every tick that still uses the SAME ring. A full-horizon band wins; when none exists, a
        // band that buys a real gain over pressing now wins. Take the middle of the longest band,
        // the same margin rule wave uses (ADR-018, extended to every mode and orb type).
        if (pressUnderRing && currentUsesOrb && state.orbTimingEnabled) {
            std::vector<std::pair<int, int>> inRangeFull;
            std::vector<std::pair<int, int>> inRangeBetter;
            int inRangeBandLow = -1;
            int inRangeBandHigh = -1;
            int const inRangeMaxDelay = std::max(maxDelayTicks, st(ORB_REACH_TICKS));
            int const inRangeGainTicks = st(ORB_IN_RANGE_GAIN_TICKS);
            for (int inRangeDelay = 1; inRangeDelay <= inRangeMaxDelay; ++inRangeDelay) {
                if (clickMs() > state.budgetMilliseconds) {
                    break;
                }
                int const inRangeResult = simulate(gameLayer, player1, player2, isDual, inRangeDelay, true, horizonTicks, releaseAfterTicks, horizonTicks, &simulationEnd);
                bool const isUseful = inRangeResult >= horizonTicks || inRangeResult >= currentResult + inRangeGainTicks;
                if (state.pressOrbRingObject == currentRing && isUseful) {
                    if (inRangeResult >= horizonTicks) {
                        inRangeFull.emplace_back(inRangeDelay, inRangeResult);
                    }
                    else {
                        inRangeBetter.emplace_back(inRangeDelay, inRangeResult);
                    }
                }
                if (simulationEnd == SimEnd::StopSearch) {
                    break;
                }
            }
            auto pickInRangeBand = [&](std::vector<std::pair<int, int>> const& candidates, int* landingResult) -> int {
                if (candidates.empty()) {
                    return 0;
                }
                size_t bestStart = 0, bestLength = 0, runStart = 0;
                int bestResult = -1;
                for (size_t index = 1; index <= candidates.size(); ++index) {
                    bool const runEnds = index == candidates.size() || candidates[index].first != candidates[index - 1].first + 1;
                    if (!runEnds) {
                        continue;
                    }
                    size_t const length = index - runStart;
                    int runResult = -1;
                    for (size_t runIndex = runStart; runIndex < index; ++runIndex) {
                        runResult = std::max(runResult, candidates[runIndex].second);
                    }
                    if (length > bestLength || (length == bestLength && runResult > bestResult)) {
                        bestLength = length;
                        bestStart = runStart;
                        bestResult = runResult;
                    }
                    runStart = index;
                }
                inRangeBandLow = candidates[bestStart].first;
                inRangeBandHigh = candidates[bestStart + bestLength - 1].first;
                *landingResult = bestResult;
                return inRangeBandLow + (inRangeBandHigh - inRangeBandLow) / 2;
            };
            int landingResult = currentResult;
            int chosenDelay = pickInRangeBand(inRangeFull, &landingResult);
            if (chosenDelay <= 0) {
                chosenDelay = pickInRangeBand(inRangeBetter, &landingResult);
            }
            if (chosenDelay > 0 && landingResult > currentResult) {
                foundTick = chosenDelay;
                *landingTick = landingResult;
                state.wasOrbInRange = true;
                state.intendedOrbObject = currentRing;
            }
            int64_t const inRangeNowMs = scanNowMs();
            if (inRangeNowMs - g_orbRetimeLogMs >= 500) {
                g_orbRetimeLogMs = inRangeNowMs;
                log::info("[orb] in-range: press now {}t, band {}..{}, picked {}, landing {}t", currentResult, inRangeBandLow, inRangeBandHigh,
                    foundTick > 0 ? fmt::format("+{}t", foundTick) : std::string("none"), foundTick > 0 ? *landingTick : currentResult);
            }
        }

        constexpr float CLOSE_DISTANCE_FRACTION = 0.60f;
        ProbeOrb* targetProbe = nullptr;
        float targetDistanceSquared = 0.f;
        int targetCloseTick = -1;
        if (!currentUsesOrb) {
            for (auto& candidateProbe : state.probeOrbCandidates) {
                if (!candidateProbe.ringObject || candidateProbe.touchDistanceSquared <= 1e-6f) {
                    continue;
                }
                bool isAlreadyTouching = candidateProbe.touchSampleTick == 0;
                if (!isAlreadyTouching && candidateProbe.closeSampleTick > searchWindowTicks) {
                    continue;
                }
                if (candidateProbe.closeDistanceSquared > candidateProbe.touchDistanceSquared * (CLOSE_DISTANCE_FRACTION * CLOSE_DISTANCE_FRACTION)) {
                    continue;
                }

                if (typeinfo_cast<DashRingObject*>(candidateProbe.ringObject)) {
                    float orbRadius = orbSpriteRadius(candidateProbe.ringObject);
                    if (candidateProbe.closeDistanceSquared > orbRadius * orbRadius * 2.25f) {
                        continue;
                    }
                }
                if (!targetProbe || candidateProbe.closeDistanceSquared < targetDistanceSquared) {
                    targetProbe = &candidateProbe;
                    targetDistanceSquared = candidateProbe.closeDistanceSquared;
                    targetCloseTick = candidateProbe.closeSampleTick;
                }
            }
        }

        if (currentUsesOrb && pressIsDud && state.orbTimingEnabled) {
            int closeTick = 0;
            for (auto const& timingProbe : state.probeOrbCandidates) {
                if (timingProbe.ringObject == currentRing) {
                    closeTick = timingProbe.closeSampleTick;
                    break;
                }
            }
            int timingBestResult = -1;
            int timingTriedLow = -1;
            int timingTriedHigh = -1;
            int timingWeakResult = -1;
            auto tryTimingDelay = [&](int timingDelay) -> bool {
                timingTriedLow = timingTriedLow < 0 ? timingDelay : std::min(timingTriedLow, timingDelay);
                timingTriedHigh = std::max(timingTriedHigh, timingDelay);
                int timingResult = simulate(gameLayer, player1, player2, isDual, timingDelay, true, horizonTicks, releaseAfterTicks, horizonTicks, &simulationEnd);
                bool isTimingOrbUsed = state.pressOrbRingObject == currentRing;
                if (state.explainEnabled) {
                    traceText += fmt::format(" +{}:{}{}", timingDelay, timingResult, isTimingOrbUsed ? "o" : "-");
                }
                bool stop = simulationEnd == SimEnd::StopSearch;
                if (isTimingOrbUsed && !isConfidentShift(timingResult, timingDelay) && timingResult > timingWeakResult) {
                    timingWeakResult = timingResult;
                }
                if (isTimingOrbUsed && isConfidentShift(timingResult, timingDelay) && timingResult > timingBestResult) {
                    timingBestResult = timingResult;
                    foundTick = timingDelay;
                    *landingTick = timingResult;
                    state.wasOrbTiming = true;
                    if (timingResult >= horizonTicks) {
                        stop = true;
                    }
                }
                return stop;
            };
            // only trust the close tick when the window it implies actually fits inside the search
            // range. a ring the player is already inside reports a close tick far beyond the delay
            // budget, which used to make timingLow > timingHigh and try nothing at all ("tried 0..0").
            if (closeTick > 0 && closeTick - 1 <= maxDelayTicks) {
                int timingLow = std::max(1, closeTick - 1);
                int timingHigh = std::min(closeTick + st(4), maxDelayTicks);
                for (int timingDelay = timingLow; timingDelay <= timingHigh; ++timingDelay) {
                    if (tryTimingDelay(timingDelay)) {
                        break;
                    }
                }
            }
            else {
                // the probe only sees rings the no-jump path physically touches, so the ring this
                // press actually used has no close tick and the old window collapsed to delays 1-4.
                // on a frame-perfect chain the useful shift is much larger, so sweep a real window
                // (coarse, then refine) while still requiring the same ring.
                int const timingWideHigh = std::max(1, std::min(maxDelayTicks, st(24)));
                int const timingWideStride = std::max(1, st(2));
                int coarseBestTick = 0;
                for (int timingDelay = 1; timingDelay <= timingWideHigh; timingDelay += timingWideStride) {
                    if (clickMs() > state.budgetMilliseconds) {
                        if (state.explainEnabled) {
                            say("orb retime: stopped at +{}/{} ticks ({:.1f} ms)", timingDelay, timingWideHigh, clickMs());
                        }
                        break;
                    }
                    if (tryTimingDelay(timingDelay)) {
                        break;
                    }
                    if (foundTick > 0) {
                        coarseBestTick = foundTick;
                    }
                }
                if (coarseBestTick > 1) {
                    int const refineLow = std::max(1, coarseBestTick - (timingWideStride - 1));
                    int const refineHigh = std::min(timingWideHigh, coarseBestTick + timingWideStride);
                    for (int timingDelay = refineLow; timingDelay <= refineHigh; ++timingDelay) {
                        if (clickMs() > state.budgetMilliseconds) {
                            break;
                        }
                        if (tryTimingDelay(timingDelay)) {
                            break;
                        }
                    }
                }
            }
            // evidence on disk: a press that already used a ring is exactly the case the previous
            // build could not retime, so log the window that was tried and what it found.
            int64_t const retimeNowMs = scanNowMs();
            if (retimeNowMs - g_orbRetimeLogMs >= 500) {
                g_orbRetimeLogMs = retimeNowMs;
                log::info("[orb] retime: press now {}t, close tick {}, tried {}..{}, best {} -> {}t{}", currentResult, closeTick,
                    timingTriedLow < 0 ? 0 : timingTriedLow, timingTriedHigh < 0 ? 0 : timingTriedHigh,
                    foundTick > 0 ? fmt::format("+{}t", foundTick) : std::string("none"),
                    foundTick > 0 ? *landingTick : currentResult,
                    foundTick <= 0 && timingWeakResult > currentResult ? fmt::format(", refused weak +{}t", timingWeakResult - currentResult) : std::string());
            }
            if (foundTick > 0) {
                state.intendedOrbObject = currentRing;
            }
        }
        if (foundTick <= 0 && !currentUsesOrb && targetProbe && targetCloseTick >= 0 && targetCloseTick <= searchWindowTicks) {

            int nearBestResult = -1;
            // the click only has to land while the player is inside the ring, so the scan starts
            // at the first contact tick instead of waiting for the closest approach. on a frame
            // perfect orb the closest tick can be a tick or two after the first usable click.
            int const firstUsableTick = std::min(targetCloseTick, targetProbe->touchSampleTick);
            int nearLow = std::max(1, firstUsableTick);
            int nearHigh = std::min(targetCloseTick + st(4), maxDelayTicks);
            for (int nearDelay = nearLow; nearDelay <= nearHigh; ++nearDelay) {
                int nearResult = simulate(gameLayer, player1, player2, isDual, nearDelay, true, horizonTicks, releaseAfterTicks, horizonTicks, &simulationEnd);
                bool isNearOrbUsed = state.pressOrbRingObject == targetProbe->ringObject;
                if (state.explainEnabled) {
                    traceText += fmt::format(" +{}:{}{}", nearDelay, nearResult, isNearOrbUsed ? "o" : "-");
                }
                if (simulationEnd == SimEnd::StopSearch) {
                    break;
                }
                // this branch only runs when press-now misses the ring entirely, so there is no
                // connected click to protect: landing on the nearby ring is the whole point, and
                // "not worse than pressing now" is enough. the confidence gate stays on the wide
                // reach sweep, which moves the press by up to 12 frames.
                if (!isNearOrbUsed || nearResult < std::min(currentResult, horizonTicks)) {
                    continue;
                }
                if (nearResult > nearBestResult) {
                    nearBestResult = nearResult;
                    foundTick = nearDelay;
                    *landingTick = nearResult;
                }
                if (nearResult >= horizonTicks) {
                    break;
                }
            }
            if (foundTick > 0) {
                state.intendedOrbObject = targetProbe->ringObject;
            }
        }
        // any orb press that does not reach the horizon is one the player needs help with, so the
        // "take the next orb" search is no longer limited to the first ~24 ticks of survival. it only
        // runs when nothing better was found and stays budget-guarded.
        // the fallback is only for presses that need help: a press that already uses a ring and
        // carries the player past the press tick is left alone, and a press that survives the whole
        // horizon with no ring nearby has nothing to save either.
        bool const wantsOrbFallback = currentUsesOrb ? (pressIsDud && state.orbTimingEnabled)
                                                     : (currentResult < horizonTicks || targetProbe != nullptr);
        if (foundTick <= 0 && wantsOrbFallback && !(g_scanSucceeded && g_scanCurrent.orbCount == 0)) {
            // the probe only finds rings the no-jump path touches, so a cube press that misses every
            // ring can still be a few ticks away from a jump that lands on one. the simulation is the
            // only reliable detector here: sweep later delays and keep the one that actually uses a
            // ring and is a confident save (see isConfidentShift). when the press already used a ring
            // but died early, this is also the "take the next orb" fallback, and it must not wobble a
            // tick or two for a death that just moves a few frames later.
            int const reachTicks = std::clamp(st(ORB_REACH_TICKS), 1, 200);
            // cover the whole reach in at most ~16 coarse simulations, so a longer reach costs about
            // the same as the old stride-2 sweep. the stride-1 refine pass around the best coarse tick
            // keeps the final pick precise, so coarse spacing does not cost accuracy.
            int const sweepStride = std::max(1, (reachTicks + 15) / 16);
            // the coarse pass is judged on the same horizon as the final pick, otherwise a "kept the
            // whole coarse window" tick and a "dies early" tick are not comparable and the sweep can
            // march to a huge blind delay that only postpones the death.
            int detectedTick = 0;
            int detectedResult = -1;
            bool detectedIsFull = false;
            for (int sweepDelay = 1; sweepDelay <= reachTicks; sweepDelay += sweepStride) {
                if (clickMs() > state.budgetMilliseconds) {
                    if (state.explainEnabled) {
                        say("orb reach: stopped at +{}/{} ticks ({:.1f} ms)", sweepDelay, reachTicks, clickMs());
                    }
                    break;
                }
                int const sweepResult = simulate(gameLayer, player1, player2, isDual, sweepDelay, true, horizonTicks, releaseAfterTicks, horizonTicks, &simulationEnd);
                if (state.pressOrbRingObject && isConfidentShift(sweepResult, sweepDelay)) {
                    bool const sweepIsFull = sweepResult >= horizonTicks;
                    // a full-horizon save always outranks a partial one, and the earliest full save
                    // wins because it is the smallest move away from the click the player made.
                    bool const isBetter = (sweepIsFull && !detectedIsFull) || (sweepIsFull == detectedIsFull && sweepResult > detectedResult);
                    if (isBetter) {
                        detectedResult = sweepResult;
                        detectedTick = sweepDelay;
                        detectedIsFull = sweepIsFull;
                    }
                }
                if (simulationEnd == SimEnd::StopSearch) {
                    break;
                }
            }
            if (detectedTick > 0) {
                int const refineLow = std::max(1, detectedTick - sweepStride);
                int const refineHigh = std::min(reachTicks, detectedTick + sweepStride);
                int bestResult = -1;
                for (int refineDelay = refineLow; refineDelay <= refineHigh; ++refineDelay) {
                    if (clickMs() > state.budgetMilliseconds) {
                        break;
                    }
                    int const refineResult = simulate(gameLayer, player1, player2, isDual, refineDelay, true, horizonTicks, releaseAfterTicks, horizonTicks, &simulationEnd);
                    if (!state.pressOrbRingObject || !isConfidentShift(refineResult, refineDelay)) {
                        continue;
                    }
                    if (refineResult > bestResult) {
                        bestResult = refineResult;
                        foundTick = refineDelay;
                        *landingTick = refineResult;
                        state.intendedOrbObject = state.pressOrbRingObject;
                        state.wasOrbReach = true;
                    }
                    if (refineResult >= horizonTicks) {
                        break;
                    }
                }
            }
        }

        if (currentUsesOrb && !state.wasOrbReach) {
            state.intendedOrbObject = currentRing;
        }
        else if (targetProbe && !state.wasOrbReach) {
            state.intendedOrbObject = targetProbe->ringObject;
        }
        // reach diagnostics: a press where no ring is ever touched inside the decision window is the
        // "clicked a bit early" case, and it must not look identical to "no orb anywhere". throttled
        // so a tapping burst does not flood the log with one line per press.
        int64_t const probeNowMs = scanNowMs();
        if (probeNowMs - g_orbProbeLogMs >= 500) {
            g_orbProbeLogMs = probeNowMs;
            if (nearestContactTick < 0) {
                log::info("[orb] probe: window {}t, no ring within {}t, press now {}t", searchWindowTicks, probeWindowTicks, currentResult);
            }
            else {
                log::info("[orb] probe: window {}t, nearest ring {}t ({}), press now {}t", searchWindowTicks, nearestContactTick,
                    nearestContactTick <= searchWindowTicks ? "in range" : "OUT OF RANGE", currentResult);
            }
        }
        // the orb helper's decision always goes to the panel activity log, so an orb input that
        // gets no correction is visible too, not only the ones that were shifted.
        if (foundTick > 0) {
            sayOrb("orb helper: {} +{}t ({:g}f) -> {}t", state.wasOrbInRange ? "in-range" : state.wasOrbTiming ? "timing" : state.wasOrbReach ? "reach" : "press", foundTick,
                foundTick * TICK_DURATION, *landingTick);
        }
        else if (currentUsesOrb && !pressIsDud && currentResult >= horizonTicks) {
            // the click did its job for the whole judged horizon. saying "no safer delay" here read
            // like a failure and hid the real decision: the helper deliberately left it alone. a
            // press that only survives part of the horizon is not "already lands" and must report
            // the failed search instead (ADR-020).
            sayOrb("orb helper: kept raw (click already lands, {}t of {})", currentResult, horizonTicks);
        }
        else if (currentUsesOrb || targetProbe) {
            sayOrb("orb helper: no safer delay (nearest {}t, {}% close, press now {}t, window {}t)",
                targetProbe ? targetCloseTick : -1,
                targetProbe && targetProbe->touchDistanceSquared > 1e-6f
                    ? std::sqrt(targetProbe->closeDistanceSquared / targetProbe->touchDistanceSquared) * 100.f
                    : 0.f,
                currentResult, searchWindowTicks);
        }
        if (state.explainEnabled) {
            say("orb: closest {} ({}), press now {} -> {}t |{} => {}", targetProbe ? targetCloseTick : -1,
                targetProbe && targetProbe->touchDistanceSquared > 1e-6f
                    ? std::sqrt(targetProbe->closeDistanceSquared / targetProbe->touchDistanceSquared) * 100.f
                    : 0.f,
                currentUsesOrb ? "uses it" : "misses it", currentResult, traceText.empty() ? " (nothing to try)" : traceText,
                foundTick > 0 ? fmt::format("+{}t", foundTick) : std::string("no pick"));
        }
        state.orbClickEnabled = state.assumeOrbHits;
        return foundTick;
    }

    int waveScore(PlayLayer* gameLayer, PlayerObject* player1, PlayerObject* player2, bool isDual, int delayTicks, bool isDown, int horizonTicks, int targetTick,
        int nextInputGapTicks, SimEnd* simulationEnd, int secondFlipTicks = -1) {
        int candidateResult = simulateOrbs(gameLayer, player1, player2, isDual, delayTicks, isDown, horizonTicks, nextInputGapTicks, targetTick, simulationEnd, secondFlipTicks);
        if (candidateResult >= targetTick || *simulationEnd == SimEnd::StopSearch) {
            return candidateResult;
        }
        for (int marginTicks : WAVE_TURN_MARGIN_TICKS) {
            int turnTick = candidateResult - st(marginTicks);
            if (turnTick < st(WAVE_MIN_TURN_TICKS)) {
                break;
            }
            int runLengthTicks = std::min(horizonTicks, turnTick + st(WAVE_ESCAPE_TICKS));
            SimEnd turnEndReason;
            int turnedTicks = simulateOrbs(gameLayer, player1, player2, isDual, delayTicks, isDown, runLengthTicks, turnTick, runLengthTicks, &turnEndReason, secondFlipTicks);
            if (turnedTicks >= runLengthTicks) {
                if (state.captureKillerObject && state.baseEscapeTicks < 0) {
                    state.baseEscapeTicks = turnTick;
                    state.baseCrashTicks = candidateResult;
                }
                return horizonTicks;
            }
        }
        return candidateResult;
    }

    [[maybe_unused]] int robotScore(PlayLayer* gameLayer, PlayerObject* player1, PlayerObject* player2, bool isDual, int delayTicks, int horizonTicks, int targetTick, SimEnd* simulationEnd) {
        int usualHoldTicks = robotHoldHistory.pick(st(20), 50);
        int bestResult = simulateOrbs(gameLayer, player1, player2, isDual, delayTicks, true, horizonTicks, usualHoldTicks, targetTick, simulationEnd);
        robotUsedHoldTicks = usualHoldTicks;
        if (bestResult >= targetTick || *simulationEnd == SimEnd::StopSearch || isQuickScore) {
            return bestResult;
        }
        for (int holdTicks : ROBOT_HOLD_TICKS) {
            if (std::abs(st(holdTicks) - usualHoldTicks) < st(3)) {
                continue;
            }
            SimEnd trialSimulationEnd;
            int trialResult = simulateOrbs(gameLayer, player1, player2, isDual, delayTicks, true, horizonTicks, st(holdTicks), targetTick, &trialSimulationEnd);
            if (trialResult > bestResult) {
                bestResult = trialResult;
                robotUsedHoldTicks = st(holdTicks);
            }
            if (bestResult >= targetTick) {
                break;
            }
        }
        return bestResult;
    }

    [[maybe_unused]] int spiderScore(PlayLayer* gameLayer, PlayerObject* player1, PlayerObject* player2, bool isDual, int delayTicks, int horizonTicks, int releaseAfterTicks, int targetTick,
        SimEnd* simulationEnd) {
        int baseResult = simulateOrbs(gameLayer, player1, player2, isDual, delayTicks, true, horizonTicks, releaseAfterTicks, targetTick, simulationEnd);
        if (baseResult >= targetTick || *simulationEnd == SimEnd::StopSearch || isQuickScore) {
            return baseResult;
        }
        for (int marginTicks : SPIDER_RETAP_TICKS) {
            int retapTick = baseResult - st(marginTicks);
            if (retapTick < st(SPIDER_MIN_RETAP_TICKS)) {
                break;
            }
            int runLengthTicks = std::min(horizonTicks, retapTick + st(16));
            state.retapTick = retapTick;
            SimEnd retapSimulationEnd;
            int retappedTicks = simulateOrbs(gameLayer, player1, player2, isDual, delayTicks, true, runLengthTicks, releaseAfterTicks, runLengthTicks, &retapSimulationEnd);
            state.retapTick = -1;
            if (retappedTicks >= runLengthTicks) {
                if (state.captureKillerObject && state.baseEscapeTicks < 0) {
                    state.baseEscapeTicks = retapTick;
                    state.baseCrashTicks = baseResult;
                }
                return horizonTicks;
            }
        }
        return baseResult;
    }

    bool sameCrash(int candidateDelay, int candidateResult, int baseDelay, int& sameCrashStreak) {
        if (sameCrashStreak >= 0 && std::abs(candidateDelay + candidateResult - baseDelay) <= 1) {
            ++sameCrashStreak;
        }
        else {
            sameCrashStreak = -1;
        }
        if (sameCrashStreak >= SAME_CRASH_TRY_COUNT) {
            isCrashFixed = true;
            return true;
        }
        return false;
    }

    int searchSmallest(int horizonTicks, int maxDelayTicks, int baseDelay, int* landingTick, std::function<int(int, int, SimEnd*)> const& scoreFunction,
        double budgetMilliseconds = -1.0, bool isQuiet = false, bool needsFullSearch = false) {
        if (budgetMilliseconds < 0.0) {
            budgetMilliseconds = state.budgetMilliseconds;
        }
        using clock = std::chrono::steady_clock;
        auto startTime = clock::now();
        (void)needsFullSearch;
        debugBestDelay = 0;
        debugBestResult = -1;
        // the whole lookahead is the bar, not "base + a dozen ticks". a short bar is what
        // let the first delay or two pass no matter how big the window was, so a 40 tick
        // setting still answered +2. first delay that clears the lookahead wins; if none
        // does, the longest surviving one does.
        int bestDelay = 0;
        int bestResult = -1;
        for (int candidateDelay = 1; candidateDelay <= maxDelayTicks; ++candidateDelay) {
            SimEnd simulationEnd;
            int candidateResult = scoreFunction(candidateDelay, horizonTicks, &simulationEnd);
            if (candidateResult > debugBestResult) {
                debugBestResult = candidateResult;
                debugBestDelay = candidateDelay;
            }
            if (simulationEnd == SimEnd::StopSearch) {
                break;
            }
            if (candidateResult > bestResult) {
                bestResult = candidateResult;
                bestDelay = candidateDelay;
            }
            if (candidateResult >= horizonTicks) {
                break;
            }
            if (std::chrono::duration<double, std::milli>(clock::now() - startTime).count() > budgetMilliseconds) {
                if (!isQuiet) {
                    say("prediction budget hit at +{} ticks", candidateDelay);
                }
                break;
            }
        }
        if (bestDelay <= 0 || bestResult <= baseDelay) {
            return 0;
        }
        *landingTick = bestResult;
        return bestDelay;
    }

    [[maybe_unused]] int searchTwoPass(int horizonTicks, int maxDelayTicks, int baseDelay, int* landingTick, std::function<int(int, int, SimEnd*)> const& scoreFunction,
        bool needsFullSearch = false) {
        isQuickScore = true;
        int candidateDelay = searchSmallest(horizonTicks, maxDelayTicks, baseDelay, landingTick, scoreFunction, state.budgetMilliseconds * 0.5, true, needsFullSearch);
        isQuickScore = false;
        if (candidateDelay > 0) {
            return candidateDelay;
        }
        isCrashFixed = false;
        double remainingBudget = state.budgetMilliseconds - clickMs();
        if (remainingBudget < 0.5) {
            say("prediction budget hit (quick pass only)");
            return 0;
        }
        return searchSmallest(horizonTicks, maxDelayTicks, baseDelay, landingTick, scoreFunction, remainingBudget, false, needsFullSearch);
    }

    [[maybe_unused]] int shipScore(PlayLayer* gameLayer, PlayerObject* player1, PlayerObject* player2, bool isDual, int delayTicks, bool isDown, int horizonTicks, int targetTick,
        SimEnd* simulationEnd) {
        int candidateResult = simulateOrbs(gameLayer, player1, player2, isDual, delayTicks, isDown, horizonTicks, -1, targetTick, simulationEnd);
        if (candidateResult >= targetTick || *simulationEnd == SimEnd::StopSearch) {
            return candidateResult;
        }
        for (int marginTicks : SHIP_RECOVERY_TICKS) {
            int turnTick = candidateResult - st(marginTicks);
            if (turnTick < st(WAVE_MIN_TURN_TICKS)) {
                break;
            }
            int runLengthTicks = std::min(horizonTicks, turnTick + st(WAVE_ESCAPE_TICKS));
            SimEnd turnEndReason;
            int turnedTicks = simulateOrbs(gameLayer, player1, player2, isDual, delayTicks, isDown, runLengthTicks, turnTick, runLengthTicks, &turnEndReason);
            if (turnedTicks >= runLengthTicks) {
                if (state.captureKillerObject && state.baseEscapeTicks < 0) {
                    state.baseEscapeTicks = turnTick;
                    state.baseCrashTicks = candidateResult;
                }
                return horizonTicks;
            }
        }
        return candidateResult;
    }

    [[maybe_unused]] int ufoScore(PlayLayer* gameLayer, PlayerObject* player1, PlayerObject* player2, bool isDual, int delayTicks, int horizonTicks, int releaseAfterTicks, int targetTick,
        SimEnd* simulationEnd) {
        int baseResult = simulateOrbs(gameLayer, player1, player2, isDual, delayTicks, true, horizonTicks, releaseAfterTicks, targetTick, simulationEnd);
        if (baseResult >= targetTick || *simulationEnd == SimEnd::StopSearch || isQuickScore) {
            return baseResult;
        }
        for (int marginTicks : UFO_REFLAP_TICKS) {
            int reflapTick = baseResult - st(marginTicks);
            if (reflapTick < st(4)) {
                break;
            }
            int runLengthTicks = std::min(horizonTicks, reflapTick + st(20));
            state.retapTick = reflapTick;
            SimEnd reflapSimulationEnd;
            int reflappedTicks = simulateOrbs(gameLayer, player1, player2, isDual, delayTicks, true, runLengthTicks, releaseAfterTicks, runLengthTicks, &reflapSimulationEnd);
            state.retapTick = -1;
            if (reflappedTicks >= runLengthTicks) {
                if (state.captureKillerObject && state.baseEscapeTicks < 0) {
                    state.baseEscapeTicks = reflapTick;
                    state.baseCrashTicks = baseResult;
                }
                return horizonTicks;
            }
        }
        return baseResult;
    }

    int searchWave(PlayLayer* gameLayer, PlayerObject* player1, PlayerObject* player2, bool isDual, bool isDown, int horizonTicks, int maxDelayTicks, int baseDelay,
        int* landingTick, int minDelayTicks = 0, int nextInputGapTicks = -1, int secondFlipTicks = -1) {
        isMarginUsed = false;
        using clock = std::chrono::steady_clock;
        auto startTime = clock::now();
        debugBestDelay = 0;
        debugBestResult = -1;

        auto scoreAt = [&](int candidateDelay, int targetTick, SimEnd* simulationEnd) {
            int candidateResult = waveScore(gameLayer, player1, player2, isDual, candidateDelay, isDown, horizonTicks, targetTick, nextInputGapTicks, simulationEnd, secondFlipTicks);
            if (candidateResult > debugBestResult) {
                debugBestResult = candidateResult;
                debugBestDelay = candidateDelay;
            }
            return candidateResult;
        };

        // a release starts at the shift the press in front of it used and a press starts at
        // the shift the release in front of it used, so the hold/up-time the player really
        // performed is tested first. if that exact timing is not safe, the whole 1..max
        // window has to be fair game: the carry is exactly what was wrong in that case, so
        // scanning only the ticks above it just locks the error in and walks it upward.
        int const carryDelay = std::clamp(minDelayTicks, 0, std::max(1, maxDelayTicks));

        // a wave input only has to stay alive until the next input. judging a press on the
        // full 65 tick lookahead means simulating one unbroken hold through the whole
        // corridor, which ends on a wall almost every time. every candidate then "fails",
        // the scan runs out of budget long before the user's window is reached, and a
        // half-tested delay is applied. instead the selected window is the contract: a press
        // is judged for the number of ticks the user asked for (clamped to a sane range) and
        // a release keeps its short descend window.
        int const practicalHorizon = std::min(horizonTicks, waveSurvivalTicks(isDown, maxDelayTicks));

        // keep the full selected window searchable inside the click budget. this is a wave
        // only floor, so the 28 tick case is not silently cut down to the first few ticks.
        double const waveBudgetMilliseconds = std::max(state.budgetMilliseconds, 10.0);
        auto overWaveBudget = [&] {
            return std::chrono::duration<double, std::milli>(clock::now() - startTime).count() > waveBudgetMilliseconds;
        };
        auto logCut = [&](int candidateDelay) {
            say("wave window cut at +{}/{} ticks", candidateDelay, maxDelayTicks);
            static auto lastBudgetLog = std::chrono::steady_clock::time_point{};
            auto const now = clock::now();
            if (lastBudgetLog.time_since_epoch().count() == 0 || now - lastBudgetLog > std::chrono::seconds(2)) {
                lastBudgetLog = now;
                log::warn("dynamic: wave window cut at +{}/{} ticks ({:.1f} ms budget)", candidateDelay, maxDelayTicks, waveBudgetMilliseconds);
            }
        };

        if (!state.waveMarginEnabled) {
            int const targetTick = std::min(horizonTicks, baseDelay + st(12));
            int bestDelay = 0;
            int bestResult = -1;
            auto consider = [&](int candidateDelay) -> int {
                SimEnd simulationEnd;
                int candidateResult = scoreAt(candidateDelay, targetTick, &simulationEnd);
                if (simulationEnd == SimEnd::StopSearch) {
                    return -1;
                }
                if (candidateResult > bestResult) {
                    bestResult = candidateResult;
                    bestDelay = candidateDelay;
                }
                return candidateResult >= targetTick ? 1 : 0;
            };

            if (carryDelay > 0) {
                int const carried = consider(carryDelay);
                if (carried > 0) {
                    *landingTick = targetTick;
                    return carryDelay;
                }
                if (carried < 0) {
                    return 0;
                }
            }
            for (int candidateDelay = 1; candidateDelay <= maxDelayTicks; ++candidateDelay) {
                if (candidateDelay == carryDelay && carryDelay > 0) {
                    continue;
                }
                int const considered = consider(candidateDelay);
                if (considered > 0) {
                    *landingTick = targetTick;
                    isMarginUsed = candidateDelay != (carryDelay > 0 ? carryDelay : 1);
                    return candidateDelay;
                }
                // a candidate whose ghost dies before the input can be applied only means
                // this delay (and every larger one) is impossible. stop scanning, but keep
                // the best candidate already measured instead of throwing the search away.
                if (considered < 0) {
                    break;
                }
                if (overWaveBudget()) {
                    logCut(candidateDelay);
                    break;
                }
            }
            if (bestDelay <= 0 || bestResult <= baseDelay) {
                return 0;
            }
            // nothing cleared the target. the old code applied the longest survivor even when
            // it was still short, which is how a 13 tick window produced +13t presses that
            // arrived a quarter second late. an on-time input is more predictable than a late
            // one, so leave the player's own timing alone instead of gambling.
            say("wave fallback skipped (+{}t only reached {}/{}t)", bestDelay, bestResult, targetTick);
            log::info("[wave] fallback skipped (+{}t only reached {}/{}t)", bestDelay, bestResult, targetTick);
            return 0;
        }

        // already safe through the window that matters: do not delay a fine input, and do
        // not turn a short raw survival into an unnecessary shift.
        if (baseDelay >= practicalHorizon) {
            return 0;
        }

        // margin (default). every delay the player's window allows is judged, and the one
        // that keeps the wave alive longest in absolute time wins.
        //
        // delaying an input by d costs d ticks before it is even applied, so survival after
        // the input alone is the wrong score: a candidate that dies 2 ticks after a +20t
        // delay is alive for 22 ticks, while a candidate that dies 15 ticks after a +2t delay
        // is alive for 17. the absolute total (delay + survival) is what "being saved" means,
        // and scoring by it is what turns the old "+13t for +1t" gamble into a real fix.
        int bestDelay = 0;
        int bestResult = baseDelay;
        int bestTotal = baseDelay;
        // the range of delays that survive the whole window. the applied correction is the
        // middle of that range: as far from "too early" as it is from "too late". the earliest
        // survivor nudged almost-safe clicks by +2/+3 ticks (so the setting could not be felt),
        // and the latest survivor sat on the edge where the next tick kills. the centre is the
        // correction with real margin on both sides.
        std::vector<std::pair<int, int>> fullSurvivors;
        state.waveBandLow = 0;
        state.waveBandHigh = 0;
        state.waveLearnTicks = 0;
        int const learnedDelay = learnedWaveDelay(!isDown);

        auto consider = [&](int candidateDelay) -> int {
            SimEnd candidateEnd;
            int const candidateResult = simulateOrbs(gameLayer, player1, player2, isDual, candidateDelay, isDown, practicalHorizon, nextInputGapTicks, practicalHorizon, &candidateEnd);
            if (candidateResult > debugBestResult) {
                debugBestResult = candidateResult;
                debugBestDelay = candidateDelay;
            }
            if (candidateEnd == SimEnd::StopSearch) {
                // the ghost dies while the input is still waiting. every larger delay waits
                // longer and dies the same way, so the window is exhausted here.
                return -1;
            }
            if (candidateResult >= practicalHorizon) {
                fullSurvivors.push_back({ candidateDelay, candidateResult });
            }
            int const candidateTotal = candidateDelay + candidateResult;
            if (candidateTotal > bestTotal) {
                bestTotal = candidateTotal;
                bestResult = candidateResult;
                bestDelay = candidateDelay;
            }
            return 0;
        };

        if (carryDelay > 0) {
            int const carried = consider(carryDelay);
            if (carried < 0) {
                return 0;
            }
        }

        // passive learning: the shift this level has needed before is simulated first, then
        // the usual ascending scan follows. the hint is deliberately cheap and additive:
        // - if it is right, the very first simulation already proves a safe band, so even a
        //   click budget too tight to reach that far up the window still returns a real save
        //   instead of "kept raw";
        // - if it is wrong, exactly one simulation is wasted and the scan continues exactly
        //   where the old code started. a stale profile can never cost a save.
        std::vector<int> candidateOrder;
        candidateOrder.reserve(static_cast<size_t>(std::max(1, maxDelayTicks)));
        auto pushCandidate = [&](int delayTicks) {
            if (delayTicks <= 0 || delayTicks > maxDelayTicks || delayTicks == carryDelay) {
                return;
            }
            if (std::find(candidateOrder.begin(), candidateOrder.end(), delayTicks) != candidateOrder.end()) {
                return;
            }
            candidateOrder.push_back(delayTicks);
        };
        pushCandidate(learnedDelay);
        for (int candidateDelay = 1; candidateDelay <= maxDelayTicks; ++candidateDelay) {
            pushCandidate(candidateDelay);
        }

        // with a learned order the scan is not ascending any more, so "the ghost died while
        // waiting" no longer means the scan can simply stop: it means every delay at or above
        // this one is impossible, while the smaller ones may still be worth testing.
        int impossibleFrom = maxDelayTicks + 1;
        for (int candidateDelay : candidateOrder) {
            if (candidateDelay >= impossibleFrom) {
                continue;
            }
            int const considered = consider(candidateDelay);
            if (considered < 0) {
                impossibleFrom = std::min(impossibleFrom, candidateDelay);
                continue;
            }
            if (overWaveBudget()) {
                logCut(candidateDelay);
                break;
            }
        }

        // a full-window survivor exists: apply the middle of the longest contiguous safe run,
        // i.e. the delay with the most margin on both sides. this is what makes the tick
        // setting felt without sitting on the killing edge like the latest-survivor rule did.
        if (!fullSurvivors.empty()) {
            // the learned order visited these out of sequence; the run detection below needs
            // them ascending.
            std::sort(fullSurvivors.begin(), fullSurvivors.end());
            size_t bestStart = 0, bestLength = 0, runStart = 0;
            for (size_t index = 1; index <= fullSurvivors.size(); ++index) {
                bool const runEnds = index == fullSurvivors.size() || fullSurvivors[index].first != fullSurvivors[index - 1].first + 1;
                if (!runEnds) {
                    continue;
                }
                size_t const length = index - runStart;
                if (length > bestLength) {
                    bestLength = length;
                    bestStart = runStart;
                }
                runStart = index;
            }

            // every delay in this run survived the whole window, so any of them is safe. the
            // middle leaves the most margin on both sides; passive learning pulls the pick
            // toward the shift this level keeps needing, but never past the midpoint of either
            // half, so the click keeps at least a quarter of the band as margin. memory nudges,
            // the simulation still decides.
            size_t const runLast = bestStart + bestLength - 1;
            int const bandLow = fullSurvivors[bestStart].first;
            int const bandHigh = fullSurvivors[runLast].first;
            state.waveBandLow = bandLow;
            state.waveBandHigh = bandHigh;

            int const bandCentre = bandLow + (bandHigh - bandLow) / 2;
            int const halfWidth = (bandHigh - bandLow) / 2;
            int const maxPull = halfWidth * LEARN_BAND_PULL_PERCENT / 100;
            int chosenDelay = bandCentre;
            if (learnedDelay > 0) {
                chosenDelay = bandCentre + std::clamp(learnedDelay - bandCentre, -maxPull, maxPull);
                chosenDelay = std::clamp(chosenDelay, bandLow, bandHigh);
                state.waveLearnTicks = learnedDelay;
            }

            size_t chosenIndex = bestStart;
            for (size_t index = bestStart; index <= runLast; ++index) {
                if (fullSurvivors[index].first == chosenDelay) {
                    chosenIndex = index;
                    break;
                }
            }
            *landingTick = fullSurvivors[chosenIndex].second;
            isMarginUsed = fullSurvivors[chosenIndex].first != (carryDelay > 0 ? carryDelay : 1);
            return fullSurvivors[chosenIndex].first;
        }

        // nothing survived the whole window. a short input is still worth moving when the
        // move keeps the wave alive noticeably longer in absolute time, the new flip itself
        // gives the player a real chance (at least 8 ticks), and the click's own outcome is
        // better than the raw one. that accepts "2t death -> +2t, 26t survival" and rejects
        // the old "+13t to gain +1t" gamble that made the buffer feel late and wrong.
        int const minimumLanding = st(8);
        int const minimumGain = st(2);
        if (bestDelay > 0 && bestResult >= minimumLanding && bestResult >= baseDelay && bestTotal > baseDelay + minimumGain) {
            *landingTick = bestResult;
            isMarginUsed = bestDelay != (carryDelay > 0 ? carryDelay : 1);
            return bestDelay;
        }
        if (bestDelay > 0) {
            say("wave kept raw: best +{}t -> {}t (total {}t vs raw {}t)", bestDelay, bestResult, bestTotal, baseDelay);
            log::info("[wave] kept raw: best +{}t -> {}t (total {}t vs raw {}t)", bestDelay, bestResult, bestTotal, baseDelay);
        }
        return 0;
    }

    [[maybe_unused]] int searchDelay(PlayLayer* gameLayer, PlayerObject* player1, PlayerObject* player2, bool isDual, bool isDown, bool hasWave, int horizonTicks, int maxDelayTicks,
        int releaseAfterTicks, int baseDelay, int* landingTick, bool needsFullSearch = false) {
        using clock = std::chrono::steady_clock;
        auto startTime = clock::now();
        auto hasExceededBudget = [&] {
            return std::chrono::duration<double, std::milli>(clock::now() - startTime).count() > state.budgetMilliseconds;
        };

        int targetTick = needsFullSearch ? horizonTicks : std::min(horizonTicks, baseDelay + st(12));
        debugBestDelay = 0;
        debugBestResult = -1;
        int sameCrashStreak = 0;
        int waveBestDelay = 0;
        int waveBestResult = baseDelay;
        for (int candidateDelay = 1; candidateDelay <= maxDelayTicks; ++candidateDelay) {
            SimEnd simulationEnd;
            int candidateResult = simulateOrbs(gameLayer, player1, player2, isDual, candidateDelay, isDown, horizonTicks, releaseAfterTicks, hasWave ? horizonTicks : targetTick, &simulationEnd);
            if (candidateResult > debugBestResult) {
                debugBestResult = candidateResult;
                debugBestDelay = candidateDelay;
            }
            if (simulationEnd == SimEnd::StopSearch) {
                break;
            }
            if (hasWave) {

                if (candidateResult >= horizonTicks && candidateResult - baseDelay >= candidateDelay * 2 + 6) {
                    waveBestDelay = candidateDelay;
                    waveBestResult = candidateResult;
                    break;
                }
                if (candidateResult >= baseDelay + candidateDelay * 2 + 10 && candidateResult > waveBestResult + 2) {
                    waveBestDelay = candidateDelay;
                    waveBestResult = candidateResult;
                    if (candidateResult >= horizonTicks) {
                        break;
                    }
                }
            }
            else if (candidateResult < targetTick && sameCrash(candidateDelay, candidateResult, baseDelay, sameCrashStreak)) {
                break;
            }
            else if (candidateResult >= targetTick) {

                SimEnd fullSearchEnd;
                if (candidateResult < horizonTicks && simulateOrbs(gameLayer, player1, player2, isDual, candidateDelay, isDown, horizonTicks, releaseAfterTicks, horizonTicks, &fullSearchEnd) < horizonTicks) {
                    for (int innerIndex = candidateDelay + 1; innerIndex <= maxDelayTicks && !hasExceededBudget(); ++innerIndex) {
                        SimEnd innerSimulationEnd;
                        int innerResult = simulateOrbs(gameLayer, player1, player2, isDual, innerIndex, isDown, horizonTicks, releaseAfterTicks, horizonTicks, &innerSimulationEnd);
                        if (innerSimulationEnd == SimEnd::StopSearch) {
                            break;
                        }
                        if (innerResult >= horizonTicks) {
                            *landingTick = innerResult;
                            return innerIndex;
                        }
                    }
                }
                *landingTick = candidateResult;
                return candidateDelay;
            }
            if (hasExceededBudget()) {
                say("prediction budget hit at +{} ticks", candidateDelay);
                break;
            }
        }
        if (hasWave && waveBestDelay > 0) {
            *landingTick = waveBestResult;
            return waveBestDelay;
        }
        return 0;
    }

    void queueInput(int tickCount, bool isDown) {
        state.queuedInputs.push_back({ tickCount, isDown });
        if (isDown) {
            state.queuedPressTick = currentTick;
        }
        else {
            state.queuedReleaseTick = currentTick;
        }
    }

    // ---- smart straight-fly (ADR-013, window in ADR-014) --------------------------------
    // "i need to straight fly" is read from the player's own inputs: a ship holds a line by
    // alternating short taps, so a run of short holds and short gaps is the intent. the ghost
    // then says where that rhythm is actually taking the ship over the judge window. a climb is
    // flattened by delaying the press (the tap, and its lift, land later in the level); a sink
    // is flattened by delaying the release (the nose stays up for the extra ticks). only the
    // edge that opposes the drift is touched, only by <= the strength cap, and only after the
    // delayed gesture is verified to still be flying at the end of that window and to land
    // measurably closer to the line. no press is ever added or removed, and a tap that already
    // holds its line is left alone. the three dials in the `straight` tab (strength / sensitivity
    // / rhythm) feed the four functions below and nothing else.
    int shipStraightStrength() {
        return std::clamp(state.shipStraightStrength, SHIP_STRAIGHT_STRENGTH_MIN, SHIP_STRAIGHT_STRENGTH_MAX);
    }

    int shipStraightSensitivity() {
        return std::clamp(state.shipStraightSensitivity, 0, 100);
    }

    int shipStraightRhythm() {
        return std::clamp(state.shipStraightRhythm, 0, 100);
    }

    int shipStraightNudgeTicks() {
        float const t = static_cast<float>(shipStraightStrength()) / static_cast<float>(SHIP_STRAIGHT_STRENGTH_MAX);
        return static_cast<int>(std::lround(t * static_cast<float>(SHIP_STRAIGHT_NUDGE_MAX_TICKS)));
    }

    float shipStraightDriftUnits() {
        float const t = static_cast<float>(shipStraightSensitivity()) / 100.f;
        return SHIP_STRAIGHT_DRIFT_MAX_UNITS + (SHIP_STRAIGHT_DRIFT_MIN_UNITS - SHIP_STRAIGHT_DRIFT_MAX_UNITS) * t;
    }

    float shipStraightGainUnits() {
        float const t = static_cast<float>(shipStraightStrength()) / static_cast<float>(SHIP_STRAIGHT_STRENGTH_MAX);
        return SHIP_STRAIGHT_GAIN_MAX_UNITS + (SHIP_STRAIGHT_GAIN_MIN_UNITS - SHIP_STRAIGHT_GAIN_MAX_UNITS) * t;
    }

    int shipStraightHoldMaxTicks() {
        return SHIP_STRAIGHT_HOLD_MIN_TICKS + (SHIP_STRAIGHT_HOLD_MAX_TICKS - SHIP_STRAIGHT_HOLD_MIN_TICKS) * shipStraightRhythm() / 100;
    }

    int shipStraightGapMaxTicks() {
        return SHIP_STRAIGHT_GAP_MIN_TICKS + (SHIP_STRAIGHT_GAP_MAX_TICKS - SHIP_STRAIGHT_GAP_MIN_TICKS) * shipStraightRhythm() / 100;
    }

    bool isStraightFlyRhythm() {
        if (shipHoldHistory.sampleCount < SHIP_STRAIGHT_MIN_SAMPLES || shipGapHistory.sampleCount < SHIP_STRAIGHT_MIN_SAMPLES) {
            return false;
        }
        int const hold = shipHoldHistory.recent(3);
        int const gap = shipGapHistory.recent(3);
        return hold > 0 && hold <= st(shipStraightHoldMaxTicks()) && gap > 0 && gap <= st(shipStraightGapMaxTicks());
    }

    bool shipStraightAssist(PlayLayer* playLayer, PlayerObject* player1, PlayerObject* player2, bool isDual, bool isDown, int horizonTicks,
        int releaseAfterTicks, int secondFlipTicks, int shipHoldTicks, int shipGapTicks, int shipCapTicks, double rawAltitudeDelta, int judgeTicks,
        int requiredSurvivalTicks) {
        if (!state.shipStraightEnabled || !isStraightFlyRhythm()) {
            return false;
        }
        // the dials are calibrated on the full evasion window, but the line is read early, so the
        // units are scaled by how much of the window was actually flown. that keeps "drift 18u"
        // meaning the same thing to the player no matter where the sample tick sits.
        double const windowScale = horizonTicks > 0 ? static_cast<double>(judgeTicks) / static_cast<double>(horizonTicks) : 1.0;
        double const driftFloor = shipStraightDriftUnits() * windowScale;
        double const gainFloor = shipStraightGainUnits() * windowScale;
        if (!std::isfinite(rawAltitudeDelta) || std::abs(rawAltitudeDelta) < driftFloor) {
            return false;
        }
        // a climb (higher y at the sample tick) is pushed down by delaying the press; a sink is
        // pushed up by delaying the release. if this input is the other edge, let it through
        // untouched and wait for the half of the gesture that can actually help.
        bool const isClimbing = rawAltitudeDelta > 0.0;
        if (isClimbing != isDown) {
            return false;
        }
        // ADR-017: the straight assist no longer inherits the emergency-search cap (4t in the
        // live save). It has its own dial-driven cap so the nudge is actually visible, while every
        // candidate still has to survive the same horizon and flatten the measured line.
        int const straightCapTicks = st(shipStraightNudgeTicks());
        if (straightCapTicks <= 0) {
            return false;
        }
        int const sampleTick = std::max(0, judgeTicks - 1);

        double const rawMagnitude = std::abs(rawAltitudeDelta);
        int bestDelay = 0;
        double bestDelta = rawAltitudeDelta;
        int bestSurvival = -1;
        for (int delay = 1; delay <= straightCapTicks; ++delay) {
            SimEnd simulationEnd;
            double candidateDelta = 0.0;
            // the line is read on the short judge window (ADR-014). every candidate is also
            // simulated through the real evasion window, so the log can show how long it lives.
            int const survivedTicks = simulateOrbs(playLayer, player1, player2, isDual, delay, isDown, horizonTicks, releaseAfterTicks, horizonTicks,
                &simulationEnd, secondFlipTicks, &candidateDelta, sampleTick);
            if (survivedTicks < requiredSurvivalTicks || !std::isfinite(candidateDelta)) {
                continue;
            }
            double const candidateMagnitude = std::abs(candidateDelta);
            if (candidateMagnitude > rawMagnitude - gainFloor) {
                continue; // must measurably flatten the line it is judged against
            }
            // among the nudges that flatten the line, keep the one that lives longest, then the
            // smallest shift, so a bigger delay never wins just by being bigger.
            if (survivedTicks > bestSurvival) {
                bestSurvival = survivedTicks;
                bestDelay = delay;
                bestDelta = candidateDelta;
            }
        }

        bool const canLog = currentTick - state.lastStraightLogTick >= st(SHIP_STRAIGHT_LOG_GAP_TICKS);
        if (bestDelay <= 0) {
            if (canLog) {
                state.lastStraightLogTick = currentTick;
                sayShip("straight kept S{}: drift {:+.0f}u under floor {:.0f}u, no safe nudge (hold {}t, gap {}t, judge {}t of {}t, must keep {}t, cap {}t, ship cap {}t)",
                    shipStraightStrength(), rawAltitudeDelta, driftFloor, shipHoldTicks, shipGapTicks, judgeTicks, horizonTicks, requiredSurvivalTicks, straightCapTicks, shipCapTicks);
            }
            return false;
        }
        // an applied nudge is always logged: it is the only proof the dials reached the game.
        state.lastStraightLogTick = currentTick;
        sayShip("straight {} S{}: drift {:+.0f}u -> {:+.0f}u with +{}t (hold {}t, gap {}t, judge {}t of {}t, kept {}t, cap {}t, ship cap {}t)", isDown ? "press" : "release",
            shipStraightStrength(), rawAltitudeDelta, bestDelta, bestDelay, shipHoldTicks, shipGapTicks, judgeTicks, horizonTicks, requiredSurvivalTicks,
            straightCapTicks, shipCapTicks);
        state.inputShiftTicks = bestDelay;
        queueInput(bestDelay, isDown);
        return true;
    }

    bool intercept(GJBaseGameLayer* baseGameLayer, bool isDown, bool isFirstPlayer) {
        clickStartTime = std::chrono::steady_clock::now();
        clickBudget();
        auto* playLayer = GameManager::get()->m_playLayer;
        if (!playLayer) {
            return false;
        }

        if (playLayer->m_isPaused) {
            state.hasAppliedInput = isDown;
            return false;
        }

        auto* gameLevel = playLayer->m_level;
        bool isTwoPlayer = gameLevel && gameLevel->m_twoPlayerMode;
        if (isTwoPlayer && !isFirstPlayer && playLayer->m_gameState.m_isDualMode) {
            return false;
        }
        bool isDual = !isTwoPlayer && playLayer->m_gameState.m_isDualMode && playLayer->m_player2;

        state.isFirstPlayer = isFirstPlayer;
        state.hasRealInput = isDown;
        state.intendedOrbObject = nullptr;
        if (!state.isEnabled) {
            state.hasAppliedInput = isDown;
            return false;
        }
        if (playLayer->m_isPlatformer) {
            state.hasAppliedInput = isDown;
            return false;
        }

        auto* player1 = baseGameLayer->m_player1;
        auto* player2 = isDual ? playLayer->m_player2 : nullptr;

        float midTickFraction = 0.f;
        if (state.clickBetweenFramesSync && state.isInRealUpdate && player1 && std::abs(state.tickDeltaX) > 0.01f) {
            midTickFraction = std::clamp((player1->getPosition().x - state.tickStartPositionX) / state.tickDeltaX, 0.f, 0.999f);
        }
        state.firstStepScale = 1.f - midTickFraction;
        struct FirstStepReset {
            ~FirstStepReset() {
                state.firstStepScale = 1.f;
            }
        } firstStepResetState;
        int earlyTicks = midTickFraction > 0.f ? 1 : 0;

        bool hasShip = player1 && (player1->m_isShip || (player2 && player2->m_isShip));
        if (!player1 || player1->m_isDead || (hasShip && !state.shipBufferEnabled)) {
            resetPending();
            state.hasAppliedInput = isDown;
            return false;
        }

        bool isOrbUnderClick = player1->m_touchingRings && player1->m_touchingRings->count() > 0;
        if (isDual && player2 && player2->m_touchingRings && player2->m_touchingRings->count() > 0) {
            isOrbUnderClick = true;
        }
        // a press that is already inside a ring used to be handed straight to the engine, so the orb
        // helper never saw it. that left the one case the player cannot fix by hand - clicking while
        // in range - with no help at all, while an early click outside the range got shifted onto the
        // ring perfectly. the helper now runs for in-range presses too; if it has nothing better the
        // press still goes raw (never through the general buffer, which would move a press that is
        // already on the orb).
        bool const orbUnderClickNow = isDown && isOrbUnderClick && state.queuedInputs.empty();

        if (isDown && state.queuedInputs.empty()) {
            state.releaseShiftTicks = 0;
        }

        bool isPlayer1Wave = player1->m_isDart;
        bool hasWave = isPlayer1Wave || (player2 && player2->m_isDart);
        bool isMini = (isPlayer1Wave && player1->m_vehicleSize < 1.f) || (player2 && player2->m_isDart && player2->m_vehicleSize < 1.f);

        bool const orbAssistWanted = state.orbBufferEnabled && state.orbAlignEnabled && state.smartEnabled;
        // a ring under the click is compatible in every gamemode and of every type (jump, gravity,
        // dash, spider, drop). the mode/ground exclusions only apply to presses that are merely
        // near a ring; once the player is touching one, the helper must get the press (ADR-020).
        bool const orbUnderClickHelped = orbUnderClickNow && orbAssistWanted;
        if (orbUnderClickNow && !orbUnderClickHelped) {
            // helper off: this press gets no orb help, so keep the old raw pass
            state.ticksSinceInput = 0;
            state.hasAppliedInput = isDown;
            return false;
        }

        // the spam guard is gone (ADR-015). rapid-input counting survives only because the
        // existing ufo/ball/swing one-tick compaction still uses it; it never bypasses the
        // search or drops queued inputs now. fast wave/ship clicks keep their own rhythm.
        if (state.ticksSinceInput < st(RAPID_INPUT_GAP_TICKS)) {
            ++state.rapidInputTicks;
        }
        else {
            state.rapidInputTicks = 0;
        }
        state.ticksSinceInput = 0;

        bool hasUfo = player1->m_isBird || (player2 && player2->m_isBird);
        bool hasBall = player1->m_isBall || (player2 && player2->m_isBall);
        bool hasSwing = player1->m_isSwing || (player2 && player2->m_isSwing);
        // wave and ship are deliberately left out of legacy streak compaction. compaction
        // repacks pending taps one tick apart, which is the exact
        // opposite of what rapid wave taps need: it flattens the player's rhythm and eats the
        // flicks. ship is excluded for the same reason: its inputs are short alternating taps,
        // and repacking them one tick apart turns a real ship rhythm into a jitter the player
        // never performed. every wave/ship input keeps its own hold/up-time through the queue.
        if (!hasWave && !hasShip && ((hasUfo && state.ufoSmartEnabled) || (hasBall && state.ballSmartEnabled) || (hasSwing && state.swingSmartEnabled)) &&
            state.rapidInputTicks >= RAPID_STREAK_COUNT) {
            if (!state.queuedInputs.empty()) {
                for (size_t index = 0; index < state.queuedInputs.size(); ++index) {
                    state.queuedInputs[index].ticksRemaining = static_cast<int>(index);
                }
                if (state.queuedInputs.back().isDown != isDown) {
                    queueInput(static_cast<int>(state.queuedInputs.size()), isDown);
                }
                state.inputShiftTicks = 0;
                return true;
            }
            state.hasAppliedInput = isDown;
            return false;
        }

        if (!state.queuedInputs.empty()) {
            int afterTicks = state.queuedInputs.back().ticksRemaining;
            // a click shorter than the delay its press was given still has to come out as a
            // real tap. park the follow-up by the ticks the player really held (never zero),
            // otherwise press and release land on the same tick and the engine sees a down
            // and an up inside one update, which cancels out and swallows the tap.
            int gapTicks = 1;
            if (isDown) {
                if (state.queuedReleaseTick >= 0) {
                    gapTicks = currentTick - state.queuedReleaseTick;
                }
            }
            else if (state.queuedPressTick >= 0) {
                gapTicks = currentTick - state.queuedPressTick;
            }
            gapTicks = std::clamp(gapTicks, 1, st(90));
            queueInput(afterTicks + gapTicks, isDown);
            return true;
        }

        if (!isPlayer1Wave) {
            bool hasRobot = player1->m_isRobot;
            bool hasSecondWave = false;
            if (player2 && !hasRobot) {
                hasSecondWave = player2->m_isDart;
                if (!hasSecondWave) {
                    hasRobot = player2->m_isRobot;
                }
            }
            bool isDashing = player1->m_isDashing || (player2 && player2->m_isDashing);
            if (!hasSecondWave && !isDown && !hasRobot && !isDashing && !hasShip) {
                if (state.releaseShiftTicks > 0) {
                    state.inputShiftTicks = state.releaseShiftTicks;
                    state.releaseShiftTicks = 0;
                    queueInput(state.inputShiftTicks, false);
                    return true;
                }
                state.hasAppliedInput = false;
                return false;
            }
        }

        bool hasFlyer = isFlying(player1) || (player2 && isFlying(player2));
        int groundHorizon = std::clamp(state.groundHorizonTicks, 48, 240);
        if (!hasFlyer && state.ticksAllModes + 48 > groundHorizon) {
            groundHorizon = std::min(state.ticksAllModes + 48, 288);
        }
        int horizonTicks = st(hasFlyer ? 48 : groundHorizon);
        if (hasWave) {
            horizonTicks = fullWaveHorizon(isMini);
            if (!isDown) {
                horizonTicks = std::min(horizonTicks, st(WAVE_RELEASE_HORIZON_TICKS));
            }
        }
        // every mode's buffer comes from the value the ui shows: the wave bar for wave
        // and the "all modes" value for everything else, with a per-mode override winning
        // when it is set. the effective value is what the panel reports too.
        int const effectiveDelayTicks = effectiveTicksForMode(hasWave ? 4 : activeModeIndex());
        int maxDelayTicks = st(effectiveDelayTicks);
        if (maxDelayTicks <= 0) {
            state.hasAppliedInput = isDown;
            return false;
        }

        bool hasAnyRobot = player1->m_isRobot || (player2 && player2->m_isRobot);
        int releaseAfterTicks = -1;
        if (isDown && !hasWave && !hasShip) {
            releaseAfterTicks = hasAnyRobot ? robotHoldHistory.pick(-1, 50) : holdEstimate();
        }

        bool isGrounded = player1->m_isOnGround && (!player2 || player2->m_isOnGround);
        bool const orbGateOpen = isDown && orbAssistWanted
            && (orbUnderClickNow || (!hasWave && !hasShip && (!isGrounded || state.orbAlignToGround)));
        if (orbGateOpen) {
            logOrbPress(st(std::clamp(state.orbWindowTicks, 1, 64)), releaseAfterTicks, horizonTicks);
            int orbLandingTick = 0;
            int candidateDelay = alignOrb(playLayer, player1, player2, isDual, horizonTicks, releaseAfterTicks, 0, &orbLandingTick, orbUnderClickNow);
            if (candidateDelay > 0) {
                state.inputShiftTicks = candidateDelay;
                queueInput(candidateDelay - earlyTicks, isDown);
                return true;
            }
            if (orbUnderClickNow) {
                // the press is already on the ring and the helper found nothing better, so it has to
                // come out on this very tick. queueing it would move a click that already connects.
                state.hasAppliedInput = isDown;
                return false;
            }
        }
        else if (isDown) {
            char const* reason = hasWave ? "wave" : hasShip ? "ship" : isGrounded ? "grounded" :
                !state.orbBufferEnabled ? "orb buffer off" : !state.orbAlignEnabled ? "orb align off" :
                !state.smartEnabled ? "smart off" : "unknown";
            logOrbSkip(reason);
            if (orbUnderClickNow) {
                state.hasAppliedInput = isDown;
                return false;
            }
        }

        SimEnd simulationEnd;
        bool isCubeStyleWave = hasWave;
        bool isRobotPress = isDown && hasAnyRobot && !hasWave && state.robotSmartEnabled;
        bool hasSpider = player1->m_isSpider || (player2 && player2->m_isSpider);
        bool isSpiderPress = isDown && hasSpider && !hasWave && !isRobotPress && state.spiderSmartEnabled;
        bool isUfoPress = isDown && hasUfo && !hasWave && !hasShip && state.ufoSmartEnabled;
        bool isSwingPress = isDown && hasSwing && !hasWave && !hasShip && !isUfoPress && state.swingSmartEnabled;
        isUfoPress = isUfoPress || isSwingPress;
        bool isShipInput = hasShip && !hasWave;

        // the ship shift is capped separately from the mode window. a ship tap is a short
        // rhythm; shifting it by the full per-mode override (12t in the live save) is three
        // frames late and reads in game as "the mod moved my click and killed me".
        int shipCapTicks = 0;
        if (isShipInput) {
            shipCapTicks = st(std::clamp(state.shipMaxDelayTicks, 1, 16));
            maxDelayTicks = std::min(maxDelayTicks, shipCapTicks);
        }

        state.hasBaseTouchedDown = false;
        // every mode is judged on the raw plan for the input it was actually handed: the
        // press/release held for the learned hold length, with no rescue turn, reflap or
        // spare retap invented for it. those rescues count an input as fine because "you
        // could turn/release it later", which is true for almost any delay, and that is
        // exactly why a big window still answered +1/+2. the wave margin chip keeps the
        // old rescue search, the rest use the raw one.
        int rawReleaseAfterTicks = releaseAfterTicks;
        bool rawPressDown = isDown;
        // a ship input is a full alternating gesture, not one half of it. a press is held for
        // the player's own hold length and then released; a release lasts the player's own
        // release->press gap and then presses again. the second flip completes the gesture, so
        // the simulation stops treating "still holding" or "still released" as the end state.
        int secondFlipTicks = -1;
        int shipHoldTicks = -1;
        int shipGapTicks = -1;
        // a wave release is not the end of the input, it is the bottom of a V. a rapid wave press is
        // not "hold forever" either - the player taps up, releases after a short hold, then presses
        // again after a short gap. judging either half alone made high-cps wave search delay presses
        // through walls the player never held (ADR-016). both halves are now measured passively and
        // the full alternating gesture is simulated only when the current rhythm reads as rapid;
        // deliberate holds keep the original press behaviour untouched.
        int waveNextInputGapTicks = -1;
        int waveHoldTicks = -1;
        bool rapidWaveGesture = false;
        if (isShipInput) {
            shipHoldTicks = shipHoldEstimate();
            shipGapTicks = shipGapEstimate();
            if (isDown) {
                rawReleaseAfterTicks = shipHoldTicks;
                secondFlipTicks = shipGapTicks;
            }
            else {
                rawReleaseAfterTicks = shipGapTicks;
                secondFlipTicks = shipHoldTicks;
            }
        }
        else if (isCubeStyleWave) {
            waveNextInputGapTicks = std::max(1, waveGapEstimate());
            waveHoldTicks = std::max(1, waveHoldEstimate());
            rapidWaveGesture = isRapidWaveGesture(waveNextInputGapTicks);
            if (isDown) {
                if (rapidWaveGesture) {
                    rawReleaseAfterTicks = waveHoldTicks;
                    secondFlipTicks = waveNextInputGapTicks;
                }
                else {
                    rawReleaseAfterTicks = -1;
                }
            }
            else {
                rawReleaseAfterTicks = waveNextInputGapTicks;
                if (rapidWaveGesture) {
                    secondFlipTicks = waveHoldTicks;
                }
            }
        }
        else if (isRobotPress) {
            robotUsedHoldTicks = robotHoldHistory.pick(st(20), 50);
            rawPressDown = true;
            rawReleaseAfterTicks = robotUsedHoldTicks;
        }
        else if (isSpiderPress || isUfoPress) {
            rawPressDown = true;
        }
        auto rawScore = [&](int candidateDelay, int targetTick, SimEnd* simulationEnd) {
            return simulateOrbs(playLayer, player1, player2, isDual, candidateDelay, rawPressDown, horizonTicks, rawReleaseAfterTicks, targetTick, simulationEnd,
                secondFlipTicks);
        };
        // the raw plan is also the straight-fly baseline: where it leaves the ship at the same
        // absolute tick (same distance down the level) is the line the player is holding. the
        // straight helper reads that line at the end of its own short judge window (ADR-014).
        int const straightJudgeTicks = std::min(horizonTicks, st(SHIP_STRAIGHT_JUDGE_TICKS));
        int const straightSampleTick = std::max(0, straightJudgeTicks - 1);
        double rawAltitudeDelta = 0.0;
        int baseDelay = simulateOrbs(playLayer, player1, player2, isDual, 0, rawPressDown, horizonTicks, rawReleaseAfterTicks, horizonTicks, &simulationEnd,
            secondFlipTicks, &rawAltitudeDelta, straightSampleTick);

        std::string tickTagText = midTickFraction > 0.f ? fmt::format(" @{:.2f}", midTickFraction) : std::string();
        if (baseDelay >= horizonTicks) {
            // smart straight-fly only ever layers on an input the crash search already cleared;
            // a straight nudge must never stack on a crash rescue.
            if (isShipInput && shipStraightAssist(playLayer, player1, player2, isDual, isDown, horizonTicks, rawReleaseAfterTicks, secondFlipTicks,
                    shipHoldTicks, shipGapTicks, shipCapTicks, rawAltitudeDelta, straightJudgeTicks, std::min(horizonTicks, baseDelay))) {
                return true;
            }
            state.hasAppliedInput = isDown;
            return false;
        }

        bool isGroundPress = isDown && !hasFlyer && !hasAnyRobot && !hasSpider && !hasWave;
        if (isGroundPress && state.lateCrashTicks > 0 && baseDelay >= st(state.lateCrashTicks) && state.hasBaseTouchedDown) {
            state.hasAppliedInput = isDown;
            return false;
        }

        int landingTick = 0;
        int savedTick = 0;
        isCrashFixed = false;

        // the carry used to seed a wave search with the shift of the opposite input. that made
        // one bad shift propagate: a press saved at +13t started its release search at +13t,
        // and a release saved at +8t started the next press at +8t, so a single guessed tick
        // value kept re-timing the player's whole section. each wave input is now judged on its
        // own timing; the hold length is still preserved by the queue spacing rule (ADR-006).
        int releaseCarryTicks = 0;

        int pressCarryTicks = 0;
        if (isDown) {
            state.pressShiftTicks = 0;
        }

        savedTick = isCubeStyleWave ? searchWave(playLayer, player1, player2, isDual, isDown, horizonTicks, maxDelayTicks, baseDelay, &landingTick,
                                       isDown ? pressCarryTicks : releaseCarryTicks, rawReleaseAfterTicks, secondFlipTicks)
                                  : searchSmallest(horizonTicks, maxDelayTicks, baseDelay, &landingTick, rawScore);

        if (savedTick <= 0) {
            // the rescue could not extend this input at all, and it was never going to last the full
            // evasion window - but it stayed alive for the whole straight judge window, so there is
            // a line to judge. the nudge must still be flying at the end of that window and must
            // measurably flatten the line; beyond the window the extrapolated death tick is the
            // rescue's business, not the straight helper's (ADR-014). no press is added or removed.
            if (isShipInput && baseDelay >= straightJudgeTicks &&
                shipStraightAssist(playLayer, player1, player2, isDual, isDown, horizonTicks, rawReleaseAfterTicks, secondFlipTicks, shipHoldTicks, shipGapTicks,
                    shipCapTicks, rawAltitudeDelta, straightJudgeTicks, straightJudgeTicks)) {
                return true;
            }
            int const practicalWindow = std::min(horizonTicks, waveSurvivalTicks(isDown, maxDelayTicks));
            if (isCubeStyleWave && baseDelay >= practicalWindow) {
                sayWave("raw {}{} is already safe for {}t (window {}t, down {}t)", isDown ? "press" : "release", tickTagText, baseDelay, maxDelayTicks,
                    waveNextInputGapTicks);
            }
            else {
                // this branch also runs for non-wave inputs, so it must not put a "[wave]" tag on
                // a ship or cube release: a log that says "no save: release dies in 29t" under
                // [wave] made a wave release look fatal when it was not even a wave input.
                if (isCubeStyleWave) {
                    sayWave("no save: {}{} dies in {}t (max {}t, best +{}t -> {}t, window {}t)", isDown ? "press" : "release", tickTagText, baseDelay,
                        maxDelayTicks, debugBestDelay, debugBestResult, practicalWindow);
                }
                else if (isShipInput) {
                    sayShip("no save: {}{} dies in {}t (window {}t, cap {}t, best +{}t -> {}t, hold {}t, gap {}t)", isDown ? "press" : "release",
                        tickTagText, baseDelay, horizonTicks, shipCapTicks, debugBestDelay, debugBestResult, shipHoldTicks, shipGapTicks);
                }
                else {
                    say("no save: {}{} dies in {}t (best +{}t -> {}t)", isDown ? "press" : "release", tickTagText, baseDelay, debugBestDelay, debugBestResult);
                }
            }
            state.hasAppliedInput = isDown;
            return false;
        }

        if (isDown && !state.skipPressHandling) {
            SimEnd verificationEnd;
            // the ring a reach save picks is often touched a few ticks *after* the press (the press
            // puts the player airborne and the ring follows). 8 ticks was too short and rejected valid
            // saves whose ring contact sat at 9-15 ticks, so the identity check now looks 16 ticks out.
            int verificationHorizonTicks = st(16);
            state.retapTick = -1;
            state.orbClickEnabled = false;
            simulate(playLayer, player1, player2, isDual, savedTick, isDown, verificationHorizonTicks, -1, verificationHorizonTicks, &verificationEnd);
            if (state.pressOrbRingObject != state.intendedOrbObject) {
                sayOrb("orb helper: save +{}t rejected (press would use {}, not the clicked orb)", savedTick,
                    state.pressOrbRingObject ? "a different ring" : "no ring");
                state.hasAppliedInput = isDown;
                return false;
            }
        }

        if (isRobotPress) {
            releaseAfterTicks = robotUsedHoldTicks;
        }
        std::string extraTag = isCubeStyleWave && isMarginUsed ? std::string("  +margin") : std::string();
        if (isCubeStyleWave && state.waveBandHigh > 0) {
            extraTag += fmt::format("  center {}-{}", state.waveBandLow, state.waveBandHigh);
        }
        if (waveNextInputGapTicks > 0) {
            // proves the release was judged with the player's real down phase, not "fall forever".
            extraTag += fmt::format("  down {}t", waveNextInputGapTicks);
        }
        if (isCubeStyleWave && rapidWaveGesture) {
            extraTag += fmt::format("  rapid {}t/{}t", waveHoldTicks, waveNextInputGapTicks);
        }
        if (isCubeStyleWave) {
            int const practicalWindow = std::min(horizonTicks, waveSurvivalTicks(isDown, maxDelayTicks));
            if (landingTick < practicalWindow) {
                extraTag += fmt::format("  partial {}", landingTick);
            }
            if (state.waveLearnTicks > 0) {
                extraTag += fmt::format("  learn +{}t", state.waveLearnTicks);
            }
            // this is the training step: a correction that survived the whole window is what
            // the next input on this level starts from. no input from the player, no menu, no
            // confirmation - the shift itself is the lesson.
            noteWaveCorrection(savedTick, state.waveBandLow, state.waveBandHigh, !isDown);
        }
        int const carryTicks = isDown ? pressCarryTicks : releaseCarryTicks;
        if (carryTicks > 0) {
            extraTag += fmt::format("  {} carry +{}t", isDown ? "release" : "press", carryTicks);
        }
        if (isRobotPress) {
            extraTag += fmt::format("  hold {}t", robotUsedHoldTicks);
        }
        if (isShipInput) {
            // proves the ship input was judged on its real two-sided gesture and not as
            // "hold forever" / "release forever", and that the shift was capped.
            extraTag += fmt::format("  window {}t, cap {}t, hold {}t, gap {}t", horizonTicks, shipCapTicks, shipHoldTicks, shipGapTicks);
        }
        if (isCubeStyleWave) {
            sayWave("saved {}{} +{}t ({:g}f)  {}->{}  [max {}]{}", isDown ? "press" : "release", tickTagText, savedTick, savedTick * TICK_DURATION, baseDelay, landingTick, maxDelayTicks, extraTag);
        }
        else if (isShipInput) {
            sayShip("saved {}{} +{}t ({:g}f)  {}->{}  [max {}]{}", isDown ? "press" : "release", tickTagText, savedTick, savedTick * TICK_DURATION, baseDelay, landingTick, maxDelayTicks, extraTag);
        }
        else {
            say("saved {}{} +{}t ({:g}f)  {}->{}  [max {}]{}", isDown ? "press" : "release", tickTagText, savedTick, savedTick * TICK_DURATION, baseDelay, landingTick, maxDelayTicks, extraTag);
        }
        state.inputShiftTicks = savedTick;
        queueInput(savedTick - earlyTicks, isDown);
        return true;
    }

    void setStageIndex(int stageIndex) {
        int nextIndex = (stageIndex % stageTotalCount() + stageTotalCount()) % stageTotalCount();
        bool didChange = nextIndex != state.stageIndex;
        state.stageIndex = nextIndex;
        Mod::get()->setSavedValue("stage", state.stageIndex);
        (void)Mod::get()->saveData();
        resetPending();
        auto* gameLayer = GameManager::get()->m_playLayer;
        if (gameLayer && !state.wasAutoReleased && state.hasAppliedInput != state.hasRealInput) {
            sendButton(gameLayer, state.hasRealInput);
        }
        applyStage();
        refreshDynamicOverlay();
        if (didChange) {
            say("stage set -> {}", stageBanner());
        }
    }

    void cycleStage(int stageDirection = 1) {
        int previousIndex = state.stageIndex;
        setStageIndex(previousIndex + stageDirection);
        if (state.stageIndex != previousIndex) {
            say("{} cycle -> {}", stageKey(), stageBanner());
        }
    }

    void startCycleWatcher() {
        std::error_code errorCode;
        std::filesystem::remove(cycleTriggerPath(), errorCode);
        try {
            std::thread([] {
                while (true) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    auto triggerPath = cycleTriggerPath();
                    std::error_code errorCode;
                    if (!std::filesystem::exists(triggerPath, errorCode) || errorCode) {
                        continue;
                    }
                    std::filesystem::remove(triggerPath, errorCode);
                    geode::queueInMainThread([] {
                        cycleStage();
                    });
                }
            }).detach();
        }
        catch (...) {
            log::warn("could not start cycle watcher");
        }
    }

    void deliverPending(PlayLayer* gameLayer, PlayerObject* playerObject) {
        if (gameLayer->m_isPaused) {
            return;
        }
        if (state.ticksSinceInput <= 998) {
            ++state.ticksSinceInput;
        }
        bool isTwoPlayer = gameLayer->m_level && gameLayer->m_level->m_twoPlayerMode;
        bool isDual = !isTwoPlayer && gameLayer->m_gameState.m_isDualMode && gameLayer->m_player2;
        if (!state.shipBufferEnabled && (playerObject->m_isShip || (isDual && gameLayer->m_player2->m_isShip))) {
            resetPending();
            if (state.hasAppliedInput != state.hasRealInput) {
                sendButton(gameLayer, state.hasRealInput);
            }
            return;
        }

        if (state.queuedInputs.empty()) {
            if (state.hasAppliedInput != state.hasRealInput) {
                sendButton(gameLayer, state.hasRealInput);
            }
            sweepGhostLayer();
            return;
        }

        while (!state.queuedInputs.empty() && state.queuedInputs.front().ticksRemaining <= 0) {
            bool isDown = state.queuedInputs.front().isDown;
            state.queuedInputs.pop_front();
            state.releaseShiftTicks = isDown ? state.inputShiftTicks : 0;
            state.pressShiftTicks = isDown ? 0 : state.inputShiftTicks;
            if (isDown != state.hasAppliedInput) {
                sendButton(gameLayer, isDown);
            }
        }
        for (auto& queuedInput : state.queuedInputs) {
            --queuedInput.ticksRemaining;
        }
        if (state.queuedInputs.empty()) {
            state.inputShiftTicks = 0;
        }
    }
}

$on_mod(Loaded) {

    log::info("dynamic: load start ({})", hostOSName());

    constexpr int STAGE_SCHEMA_VERSION = 2;
    int savedStageIndex = std::clamp(Mod::get()->getSavedValue<int>("stage", 0), 0, stageTotalCount() - 1);
    if (Mod::get()->getSavedValue<int>("stage-schema", 1) < STAGE_SCHEMA_VERSION) {
        if (savedStageIndex == 6) {
            savedStageIndex = stageTotalCount() - 1;
        }
        Mod::get()->setSavedValue("stage", savedStageIndex);
        Mod::get()->setSavedValue("stage-schema", STAGE_SCHEMA_VERSION);
    }
    state.stageIndex = savedStageIndex;
    state.orbBufferEnabled = Mod::get()->getSavedValue<bool>("orb-buffer", true);
    readSettings();
    // passive learning is on by default and has no switch in the deck: it is always learning,
    // it just needs to be played. the saved key stays so it can be turned off from the mod
    // settings file by anyone who wants a frozen profile.
    state.learningEnabled = Mod::get()->getSavedValue<bool>("learn", true);
    decodeLearnStore(Mod::get()->getSavedValue<std::string>("learn-waves", ""));
    loadHoldStats("holds-tap", tapHoldHistory);
    loadHoldStats("holds-robot", robotHoldHistory);
    loadHoldStats("holds-wave", waveGapHistory);
    loadHoldStats("holds-wave-hold", waveHoldHistory);
    loadHoldStats("holds-ship", shipHoldHistory);
    loadHoldStats("gaps-ship", shipGapHistory);
    int const scanSchema = Mod::get()->getSavedValue<int>("scan-schema", 0);
    if (scanSchema != SCAN_SCHEMA_VERSION) {
        Mod::get()->setSavedValue<std::string>("level-scans", "");
        Mod::get()->setSavedValue<int>("scan-schema", SCAN_SCHEMA_VERSION);
    }
    else {
        decodeScans(Mod::get()->getSavedValue<std::string>("level-scans", ""));
    }
    applyStage();
    removeLegacyTerminalLogs();
    resetTerminalLog();
    if (!isMobileHost()) {
        startCycleWatcher();
    }
    setupKeybinds();
    static auto sceneListener = SceneEvent().listen([](CCScene* scene) {
        attachDynamicOverlay(scene);
    });
    (void)sceneListener;
    listenForSettingChanges<bool>("terminal-enabled", [](bool enabled) {
        writeTerminalFlags();
        if (!enabled) {
            closeTerminal();
        }
    });
    static auto primaryColorListener = listenForSettingChanges<ccColor3B>("ui-primary-color", [](ccColor3B) {
        if (auto* overlay = activeDynamicOverlay()) {
            overlay->rebuildDeck();
        }
    });
    static auto secondaryColorListener = listenForSettingChanges<ccColor3B>("ui-secondary-color", [](ccColor3B) {
        if (auto* overlay = activeDynamicOverlay()) {
            overlay->rebuildDeck();
        }
    });
    static auto panelScaleListener = listenForSettingChanges<double>("ui-panel-scale", [](double) {
        if (auto* overlay = activeDynamicOverlay()) {
            overlay->rebuildDeck();
        }
    });
    static auto fontScaleListener = listenForSettingChanges<double>("ui-font-scale", [](double) {
        if (auto* overlay = activeDynamicOverlay()) {
            overlay->rebuildDeck();
        }
    });
    (void)primaryColorListener;
    (void)secondaryColorListener;
    (void)panelScaleListener;
    (void)fontScaleListener;
    say("starting mode: {}", stageBanner());
    say("{} cycles anywhere: 2/2 -> 4/4 -> 8/8 -> 16/8 -> 24/12 -> 32/16 -> 40/20 -> off", stageKey());
    if (isMobileHost()) {
        say("tap the Dynamic logo to open the control deck");
    }
    else {
        say("{} opens the control deck", panelKey());
    }
    say("host os: {} ({})", hostOSName(), hostOSDescription());
    log::info("[learn] passive learning {} | {} level profiles loaded | tap hold {}t | robot hold {}t | wave down {}t | ship hold {}t | ship gap {}t | ship straight {} S{} ({}t max, drift {:.0f}u, hold {}t, gap {}t)",
        state.learningEnabled ? "on" : "off", g_levelAdjusts.size(), tapHoldHistory.pick(0, 50), robotHoldHistory.pick(0, 50),
        waveGapEstimate(), shipHoldEstimate(), shipGapEstimate(), state.shipStraightEnabled ? "on" : "off", shipStraightStrength(),
        shipStraightNudgeTicks(), shipStraightDriftUnits(), shipStraightHoldMaxTicks(), shipStraightGapMaxTicks());
    log::info("[scan] {} saved level scans loaded", g_levelScans.size());
    log::info("dynamic: load complete");
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::pauseGame", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::resume", MOD_HOOK_PRIORITY);
    }

    // runs for the whole level (not just while the panel is open) so a scan that could not
    // read the object graph yet retries itself instead of silently giving up.
    void scanTick(float) {
        if (!g_scanPending) {
            return;
        }
        pumpLevelScan(this);
    }

    void destroyPlayer(PlayerObject* playerObject, GameObject* gameObject) {

        if (state.isSimulating || isGhost(playerObject)) {
            if (state.isSimulating) {
                state.hasGhostDied = true;
            }
            return;
        }
        bool wasAlreadyDead = playerObject && playerObject->m_isDead;
        PlayLayer::destroyPlayer(playerObject, gameObject);
        if (!playerObject || wasAlreadyDead || !playerObject->m_isDead) {
            return;
        }
        resetPending();
        state.hasAppliedInput = false;
        state.rapidInputTicks = 0;
        forgetPress();
        // death is the natural place to persist what the attempt taught us.
        flushLearnStore(false);
    }

    bool init(GJGameLevel* gameLevel, bool useReplayData, bool skipObjectCreation) {
        if (!PlayLayer::init(gameLevel, useReplayData, skipObjectCreation)) {
            return false;
        }
        dropGhosts();
        readSettings();
        applyStage();
        beginLevelLearning(gameLevel);
        if (!skipObjectCreation) {
            beginLevelScan(this);
            schedule(schedule_selector(BufPlayLayer::scanTick), 0.25f);
        }
        auto* ghost1 = makeGhost(this);
        auto* ghost2 = makeGhost(this);
        if (!ghost1 || !ghost2 || !state.ghostRenderLayer) {
            dropGhosts();
            log::warn("dynamic: could not create buffer players, buffering disabled for this level");
        }
        else {
            state.ghostPlayer1 = ghost1;
            state.ghostPlayer2 = ghost2;
            state.ghostLayerBaseIndex = state.ghostRenderLayer->getChildrenCount();
            say("buffer ready | {} | {} cycles", stageText(), stageKey());
            log::info("buffer ready | stage {} ({}t all / {}t wave) | orb buffer {} | no spam guard",
                state.stageIndex, state.ticksAllModes, state.ticksWaveMode,
                state.orbBufferEnabled ? "on" : "off");
        }
        checkForUpdates(false);
        state.hasRealInput = false;
        state.hasAppliedInput = false;
        state.isFirstPlayer = true;
        state.ticksSinceInput = 999;
        state.rapidInputTicks = 0;
        state.wasAutoReleased = false;
        forgetPress();
        resetPending();
        scheduleDynamicOverlay(this);
        return true;
    }

    void onQuit() {
        resetPending();
        flushLearnStore(true);
        unschedule(schedule_selector(BufPlayLayer::scanTick));
        g_scanPending = false;
        g_scanRunning = false;
        flushScanStore();
        dropGhosts();
        PlayLayer::onQuit();
    }

    void resetLevel() {
        resetPending();
        forgetPress();
        state.hasAppliedInput = false;
        state.wasAutoReleased = false;
        flushLearnStore(false);
        PlayLayer::resetLevel();
    }

    void pauseGame(bool isUnfocused) {
        resetPending();
        state.rapidInputTicks = 0;
        state.ticksSinceInput = 999;
        state.wasAutoReleased = false;
        forgetPress();
        flushLearnStore(true);
        flushScanStore();
        sweepGhostLayer();
        PlayLayer::pauseGame(isUnfocused);
    }

    void resume() {
        resetPending();
        state.rapidInputTicks = 0;
        state.ticksSinceInput = 999;
        state.wasAutoReleased = false;
        forgetPress();
        sweepGhostLayer();
        PlayLayer::resume();
    }

    void playEndAnimationToPos(CCPoint targetPosition) {
        if (state.isSimulating) {
            return;
        }
        PlayLayer::playEndAnimationToPos(targetPosition);
    }
};

namespace {
    CCMenuItemSpriteExtra* findPauseSettingsButton(CCNode* root) {
        if (auto* byId = typeinfo_cast<CCMenuItemSpriteExtra*>(root->getChildByIDRecursive("options-button"))) {
            return byId;
        }
        auto* gearFrame = CCSpriteFrameCache::get()->spriteFrameByName("GJ_optionsBtn02_001.png");
        if (!gearFrame) {
            return nullptr;
        }
        std::function<CCMenuItemSpriteExtra*(CCNode*)> search = [&](CCNode* node) -> CCMenuItemSpriteExtra* {
            auto* children = node->getChildren();
            if (!children) {
                return nullptr;
            }
            for (auto* child : CCArrayExt<CCNode*>(children)) {
                if (auto* item = typeinfo_cast<CCMenuItemSpriteExtra*>(child)) {
                    if (auto* sprite = typeinfo_cast<CCSprite*>(item->getNormalImage())) {
                        if (sprite->isFrameDisplayed(gearFrame)) {
                            return item;
                        }
                    }
                }
                if (auto* found = search(child)) {
                    return found;
                }
            }
            return nullptr;
        };
        return search(root);
    }
}

namespace {
    constexpr char const* PAUSE_BUTTON_X_KEY = "pause-button-x";
    constexpr char const* PAUSE_BUTTON_Y_KEY = "pause-button-y";
    constexpr float PAUSE_BUTTON_DRAG_SLOP = 12.f;
    constexpr float PAUSE_BUTTON_TOUCH_PAD = 12.f;

    CCRect pauseSafeArea() {
        auto safeArea = geode::utils::getSafeAreaRect();
        if (safeArea.size.width <= 1.f || safeArea.size.height <= 1.f) {
            auto winSize = CCDirector::get()->getWinSize();
            safeArea = CCRect(0.f, 0.f, winSize.width, winSize.height);
        }
        return safeArea;
    }

    CCNode* makePauseButtonFace(float size) {
        auto* face = CCNode::create();
        if (!face) {
            return nullptr;
        }
        face->setContentSize(CCSize(size, size));
        face->setAnchorPoint(CCPoint(0.5f, 0.5f));

        // No additive tint copies: they read as a white halo around the mark.

        if (auto* logo = makeLogoMark(size)) {
            logo->setAnchorPoint(CCPoint(0.5f, 0.5f));
            logo->setPosition(CCPoint(size * 0.5f, size * 0.5f));
            face->addChild(logo, 1);
        }
        return face;
    }
}

class DynamicPauseButton : public CCLayer {
public:
    static DynamicPauseButton* create(float size) {
        auto* node = new DynamicPauseButton();
        if (node && node->init(size)) {
            node->autorelease();
            return node;
        }
        CC_SAFE_DELETE(node);
        return nullptr;
    }

    void settleAt(CCPoint const& fallback) {
        auto safeArea = pauseSafeArea();
        CCPoint target = fallback;
        double savedX = Mod::get()->getSavedValue<double>(PAUSE_BUTTON_X_KEY, -1.0);
        double savedY = Mod::get()->getSavedValue<double>(PAUSE_BUTTON_Y_KEY, -1.0);
        if (savedX >= 0.0 && savedX <= 1.0 && savedY >= 0.0 && savedY <= 1.0) {
            CCPoint world(
                safeArea.getMinX() + static_cast<float>(savedX) * safeArea.size.width,
                safeArea.getMinY() + static_cast<float>(savedY) * safeArea.size.height
            );
            target = windowToLocal(world);
        }
        setHomePosition(target);
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        if (!touch || !m_face) {
            return false;
        }
        CCPoint local = convertTouchToNodeSpace(touch);
        float const reach = m_size * 0.5f + PAUSE_BUTTON_TOUCH_PAD;
        float deltaX = local.x - m_home.x;
        float deltaY = local.y - m_home.y;
        if (deltaX * deltaX + deltaY * deltaY > reach * reach) {
            return false;
        }
        m_dragging = true;
        m_moved = false;
        m_dragStart = local;
        m_dragOffset = CCPoint(m_home.x - local.x, m_home.y - local.y);
        m_face->stopAllActions();
        m_face->runAction(CCScaleTo::create(0.08f, 0.9f));
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        if (!m_dragging || !m_face || !touch) {
            return;
        }
        CCPoint local = convertTouchToNodeSpace(touch);
        float deltaX = local.x - m_dragStart.x;
        float deltaY = local.y - m_dragStart.y;
        if (!m_moved && deltaX * deltaX + deltaY * deltaY > PAUSE_BUTTON_DRAG_SLOP * PAUSE_BUTTON_DRAG_SLOP) {
            m_moved = true;
            m_face->stopAllActions();
            m_face->runAction(CCScaleTo::create(0.08f, 1.08f));
        }
        if (!m_moved) {
            return;
        }
        setHomePosition(CCPoint(local.x + m_dragOffset.x, local.y + m_dragOffset.y));
    }

    void ccTouchEnded(CCTouch* touch, CCEvent*) override {
        if (!m_dragging) {
            return;
        }
        m_dragging = false;
        if (m_face) {
            m_face->stopAllActions();
            m_face->runAction(CCScaleTo::create(0.1f, 1.f));
        }
        if (m_moved) {
            m_moved = false;
            savePosition();
            return;
        }

        Loader::get()->queueInMainThread([] {
            toggleDynamicPanel();
        });
    }

    void ccTouchCancelled(CCTouch*, CCEvent*) override {
        m_dragging = false;
        m_moved = false;
        if (m_face) {
            m_face->stopAllActions();
            m_face->setScale(1.f);
        }
    }

protected:
    bool init(float size) {
        if (!CCLayer::init()) {
            return false;
        }
        auto winSize = CCDirector::get()->getWinSize();
        setContentSize(winSize);
        setAnchorPoint(CCPoint(0.f, 0.f));
        setPosition(CCPoint(0.f, 0.f));
        setTouchMode(kCCTouchesOneByOne);
        setTouchPriority(-1001);
        setTouchEnabled(true);

        m_size = size;
        m_home = CCPoint(winSize.width * 0.5f, winSize.height * 0.5f);
        m_face = makePauseButtonFace(size);
        if (m_face) {
            m_face->setPosition(m_home);
            addChild(m_face, 1);
        }
        return true;
    }

private:
    void setHomePosition(CCPoint const& position) {
        m_home = position;
        clampHome();
        if (m_face) {
            m_face->setPosition(m_home);
        }
    }

    void clampHome() {
        auto safeArea = pauseSafeArea();
        CCPoint low = windowToLocal(CCPoint(safeArea.getMinX(), safeArea.getMinY()));
        CCPoint high = windowToLocal(CCPoint(safeArea.getMaxX(), safeArea.getMaxY()));
        float const margin = m_size * 0.5f + 2.f;
        float minX = std::min(low.x, high.x) + margin;
        float maxX = std::max(low.x, high.x) - margin;
        float minY = std::min(low.y, high.y) + margin;
        float maxY = std::max(low.y, high.y) - margin;
        if (maxX <= minX) {
            minX = maxX = (low.x + high.x) * 0.5f;
        }
        if (maxY <= minY) {
            minY = maxY = (low.y + high.y) * 0.5f;
        }
        m_home = CCPoint(std::clamp(m_home.x, minX, maxX), std::clamp(m_home.y, minY, maxY));
    }

    void savePosition() {
        CCPoint world = localToWindow(m_home);
        auto safeArea = pauseSafeArea();
        double normalizedX = (world.x - safeArea.getMinX()) / std::max(1.f, safeArea.size.width);
        double normalizedY = (world.y - safeArea.getMinY()) / std::max(1.f, safeArea.size.height);
        Mod::get()->setSavedValue(PAUSE_BUTTON_X_KEY, std::clamp(normalizedX, 0.0, 1.0));
        Mod::get()->setSavedValue(PAUSE_BUTTON_Y_KEY, std::clamp(normalizedY, 0.0, 1.0));
    }

    CCPoint windowToLocal(CCPoint const& world) {
        auto* parent = getParent();
        return parent ? parent->convertToNodeSpace(world) : world;
    }

    CCPoint localToWindow(CCPoint const& local) {
        auto* parent = getParent();
        return parent ? parent->convertToWorldSpace(local) : local;
    }

    float m_size = 0.f;
    CCPoint m_home;
    CCPoint m_dragStart;
    CCPoint m_dragOffset;
    bool m_dragging = false;
    bool m_moved = false;
    CCNode* m_face = nullptr;
};

class $modify(BufPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!isMobileHost()) {
            return;
        }
        if (this->getChildByID("dynamic-pause-button"_spr)) {
            return;
        }

        auto* settingsButton = findPauseSettingsButton(this);
        if (!settingsButton || !settingsButton->getParent()) {
            log::warn("dynamic: could not find the settings button in the pause menu");
            return;
        }

        float scaleX = 1.f;
        float scaleY = 1.f;
        for (CCNode* node = settingsButton; node && node != this; node = node->getParent()) {
            scaleX *= node->getScaleX();
            scaleY *= node->getScaleY();
        }
        auto settingsContent = settingsButton->getContentSize();
        float width = settingsContent.width * scaleX;
        float height = settingsContent.height * scaleY;
        if (width <= 1.f || height <= 1.f) {
            return;
        }

        float size = std::min(width, height) * 0.9f;
        CCPoint worldPosition = settingsButton->getParent()->convertToWorldSpace(settingsButton->getPosition());
        CCPoint localPosition = this->convertToNodeSpace(worldPosition);

        float const gap = 8.f;
        CCPoint fallback(localPosition.x, localPosition.y - height * 0.5f - gap - size * 0.5f);
        if (fallback.y - size * 0.5f < 4.f) {
            fallback = CCPoint(localPosition.x - width * 0.5f - gap - size * 0.5f, localPosition.y);
        }

        auto* button = DynamicPauseButton::create(size);
        if (!button) {
            return;
        }
        button->setID("dynamic-pause-button"_spr);
        this->addChild(button, 100);
        button->settleAt(fallback);
    }
};

class $modify(BufMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }

        this->runAction(CCSequence::create(
            CCDelayTime::create(0.2f),
            CallFuncExt::create([] {

                log::info("dynamic: menu ready");
                checkForUpdates(true);
                if (!showUpdatePopup()) {
                    showKeybindIntro();
                }
            }),
            nullptr
        ));
        scheduleDynamicOverlay(this);
        return true;
    }
};

class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* gameLevel, bool withoutUI) {
        bool isSuccess = LevelEditorLayer::init(gameLevel, withoutUI);
        dropGhosts();
        scheduleDynamicOverlay(this);
        return isSuccess;
    }
};

class $modify(BufHardStreak, HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", MOD_HOOK_PRIORITY);
    }

    void addPoint(CCPoint touchPoint) {
        if (state.isSimulating) {
            return;
        }
        HardStreak::addPoint(touchPoint);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", MOD_HOOK_PRIORITY);
    }

    void triggerObject(GJBaseGameLayer* gameLayer, int uniqueId, gd::vector<int> const* remapKeyVector) {
        if (state.isSimulating) {
            return;
        }
        EffectGameObject::triggerObject(gameLayer, uniqueId, remapKeyVector);
    }
};

class $modify(BufGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", MOD_HOOK_PRIORITY);
    }

    void playShineEffect() {
        if (state.isSimulating) {
            return;
        }
        GameObject::playShineEffect();
    }
};

class $modify(BufRingObject, RingObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("RingObject::spawnCircle", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("RingObject::triggerActivated", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("RingObject::powerOnObject", MOD_HOOK_PRIORITY);
    }

    void spawnCircle() {
        if (state.isSimulating) {
            return;
        }
        RingObject::spawnCircle();
    }

    // Ghost simulation used to call the real ring's activation/animation paths. shine and circle
    // effects were already suppressed, but a ghost touching a distant orb still flashed it as if the
    // player had clicked. Simulation must not animate real objects at all (orb-flash bug).
    void triggerActivated(float xPosition) {
        if (state.isSimulating) {
            return;
        }
        RingObject::triggerActivated(xPosition);
    }

    void powerOnObject(int objectState) {
        if (state.isSimulating) {
            return;
        }
        RingObject::powerOnObject(objectState);
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", MOD_HOOK_PRIORITY);

        (void)self.setHookPriority("PlayerObject::update", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::incrementJumps", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::ringJump", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::playDeathEffect", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::playerDestroyed", MOD_HOOK_PRIORITY);
    }

    void update(float deltaTime) {
        if (state.isSimulating) {
            PlayerObject::update(deltaTime);
            return;
        }
        auto* gameLayer = GameManager::get()->m_playLayer;
        if (gameLayer && this == gameLayer->m_player1 && !gameLayer->m_isPaused) {
            ++currentTick;
            if (state.isEnabled) {
                deliverPending(gameLayer, this);
            }
        }

        bool isFirstPlayer = gameLayer && this == gameLayer->m_player1;
        if (isFirstPlayer) {

            state.tickStartPositionX = getPosition().x;
            state.isInRealUpdate = true;
        }
        PlayerObject::update(deltaTime);
        if (isFirstPlayer) {
            state.isInRealUpdate = false;
            float deltaX = getPosition().x - state.tickStartPositionX;
            if (std::abs(deltaX) > 0.01f) {
                state.tickDeltaX = deltaX;
            }
            if (deltaTime > 0.f) {
                float average = state.tickLengthSeconds;
                state.tickLengthSeconds = (average > 0.f && std::abs(deltaTime - average) < average * 0.3f) ? average * 0.9f + deltaTime * 0.1f : deltaTime;
            }
        }
    }

    void incrementJumps() {
        if (state.isSimulating) {
            return;
        }
        PlayerObject::incrementJumps();
    }

    bool ghostEffect() {
        return state.isSimulating && isGhost(this);
    }

    void spawnCircle() {
        if (!ghostEffect()) PlayerObject::spawnCircle();
    }

    void spawnDualCircle() {
        if (!ghostEffect()) PlayerObject::spawnDualCircle();
    }

    void spawnPortalCircle(ccColor3B portalColor, float initialRadius) {
        if (!ghostEffect()) PlayerObject::spawnPortalCircle(portalColor, initialRadius);
    }

    void spawnScaleCircle() {
        if (!ghostEffect()) PlayerObject::spawnScaleCircle();
    }

    void playBumpEffect(int gameObjectType, GameObject* gameObject) {
        if (!ghostEffect()) PlayerObject::playBumpEffect(gameObjectType, gameObject);
    }

#ifndef GEODE_IS_IOS
    void playSpawnEffect() {
        if (!ghostEffect()) PlayerObject::playSpawnEffect();
    }

    void createFadeOutDartStreak() {
        if (!ghostEffect()) PlayerObject::createFadeOutDartStreak();
    }

    void fadeOutStreak2(float fadeDuration) {
        if (!ghostEffect()) PlayerObject::fadeOutStreak2(fadeDuration);
    }

    void playDynamicSpiderRun() {
        if (!ghostEffect()) PlayerObject::playDynamicSpiderRun();
    }

    void animatePlatformerJump(float jumpScale) {
        if (!ghostEffect()) PlayerObject::animatePlatformerJump(jumpScale);
    }

    void runBallRotation(float speedValue) {
        if (!ghostEffect()) PlayerObject::runBallRotation(speedValue);
    }

    void runBallRotation2() {
        if (!ghostEffect()) PlayerObject::runBallRotation2();
    }

    void runNormalRotation(bool isNotNormalMode, float speedValue) {
        if (!ghostEffect()) PlayerObject::runNormalRotation(isNotNormalMode, speedValue);
    }
#endif

    void toggleGhostEffect(GhostType ghostType) {
        if (!ghostEffect()) PlayerObject::toggleGhostEffect(ghostType);
    }

    void activateStreak() {
        if (!ghostEffect()) PlayerObject::activateStreak();
    }

    void playSpiderDashEffect(CCPoint fromPoint, CCPoint toPoint) {
        if (state.isSimulating) {
            return;
        }
        PlayerObject::playSpiderDashEffect(fromPoint, toPoint);
    }

    void playDeathEffect() {
        if (state.isSimulating || isGhost(this)) {
            return;
        }
        PlayerObject::playDeathEffect();
    }

    void playerDestroyed(bool suppressEffects) {
        if (state.isSimulating || isGhost(this)) {
            if (state.isSimulating) {
                state.hasGhostDied = true;
            }
            return;
        }
        PlayerObject::playerDestroyed(suppressEffects);
    }

    void playCompleteEffect(bool suppressEffects, bool isInstant) {
        if (!ghostEffect()) PlayerObject::playCompleteEffect(suppressEffects, isInstant);
    }

    void ringJump(RingObject* ringObject, bool skipCollisionCheck) {
        if (state.isSimulating) {
            if (!state.smartEnabled || !isGhost(this)) {
                return;
            }
            if (state.usedOrbs.contains(ringObject)) {
                return;
            }
            snapshotObject(ringObject);

            double verticalVelocity = m_yVelocity;
            bool isUpsideDown = m_isUpsideDown;
            bool isDashing = m_isDashing;
            PlayerObject::ringJump(ringObject, skipCollisionCheck);
            if (m_yVelocity != verticalVelocity || m_isUpsideDown != isUpsideDown || m_isDashing != isDashing) {
                state.usedOrbs.insert(ringObject);
                if (state.orbUseTick < 0) {
                    state.orbUseTick = state.simulationTick;
                }

                if (state.isInputDown && state.simulationTick >= state.inputTickIndex && !state.pressOrbRingObject) {
                    state.pressOrbRingObject = ringObject;
                }
            }
            return;
        }
        PlayerObject::ringJump(ringObject, skipCollisionCheck);
    }

    void startDashing(DashRingObject* dashRing) {
        if (state.isSimulating && isGhost(this)) {
            state.usedOrbs.insert(dashRing);
            if (state.orbUseTick < 0) {
                state.orbUseTick = state.simulationTick;
            }
            if (state.isInputDown && state.simulationTick >= state.inputTickIndex && !state.pressOrbRingObject) {
                state.pressOrbRingObject = dashRing;
            }
        }
        PlayerObject::startDashing(dashRing);
    }
};

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::flipGravity", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::gameEventTriggered", MOD_HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerWillSwitchMode", MOD_HOOK_PRIORITY);
    }

    bool canBeActivatedByPlayer(PlayerObject* playerObject, EffectGameObject* effectObject) {
        if (state.isSimulating) {
            if (!state.smartEnabled || !isGhost(playerObject) || !effectObject) {
                return false;
            }

            if (typeinfo_cast<RingObject*>(effectObject)) {
                if (state.usedOrbs.contains(effectObject)) {
                    return false;
                }
                snapshotObject(effectObject);
                return true;
            }
            if (state.simulationUsedObjects.contains(effectObject)) {
                return false;
            }
            snapshotObject(effectObject);
            return true;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(playerObject, effectObject);
    }

    void collisionCheckObjects(PlayerObject* playerObject, gd::vector<GameObject*>* gameObjects, int gameObjectCount, float deltaTime) {
        if (!state.isSimulating) {
            GJBaseGameLayer::collisionCheckObjects(playerObject, gameObjects, gameObjectCount, deltaTime);
            return;
        }
        static gd::vector<GameObject*> relevantObjects;
        relevantObjects.clear();
        if (static_cast<int>(relevantObjects.capacity()) < gameObjectCount) {
            relevantObjects.reserve(gameObjectCount);
        }

        uint64_t keptTypesMask = state.smartEnabled ? (COLLIDING_OBJECT_TYPES | INTERACTABLE_OBJECT_TYPES) : COLLIDING_OBJECT_TYPES;
        bool hasRectangle = false;
        CCRect ghostRectangle;
        auto& objectList = *gameObjects;
        for (int index = 0; index < gameObjectCount; ++index) {
            auto* gameObject = objectList[index];
            if (!gameObject) {
                continue;
            }
            auto objectType = static_cast<unsigned>(gameObject->m_objectType);
            uint64_t typeBitMask = objectType < 64 ? (1ull << objectType) : 0;
            if (keptTypesMask & typeBitMask) {

                if (!state.simulationDisabledObjects.empty() && state.simulationDisabledObjects.contains(gameObject)) {
                    continue;
                }
                if (state.hasRealRectangle && (typeBitMask & HAZARD_OBJECT_TYPES)) {
                    if (!state.harmlessObjects.empty() && state.harmlessObjects.contains(gameObject)) {
                        continue;
                    }
                    if (state.simulationTick <= 1) {
                        auto const& rectangle = gameObject->getObjectRect();
                        if (rectangle.intersectsRect(state.realRectanglePlayer1) || rectangle.intersectsRect(state.realRectanglePlayer2)) {
                            state.harmlessObjects.insert(gameObject);
                            continue;
                        }
                    }
                }
                relevantObjects.push_back(gameObject);
                continue;
            }
            if (!state.smartEnabled) {
                continue;
            }
            bool isStopPortal = (MODE_PORTAL_OBJECT_TYPES & typeBitMask) != 0;
            bool isSizePortal = (SIZE_PORTAL_OBJECT_TYPES & typeBitMask) != 0;
            float speedValue = (isStopPortal || isSizePortal) ? 0.f : speedPortalValue(gameObject->m_objectID);
            if (!isStopPortal && !isSizePortal && speedValue <= 0.f) {
                continue;
            }
            if (!hasRectangle) {
                ghostRectangle = playerObject->getObjectRect();
                hasRectangle = true;
            }
            if (ghostRectangle.intersectsRect(gameObject->getObjectRect())) {
                if (isStopPortal) {
                    state.hasGhostHitPortal = true;
                }
                else if (isSizePortal) {
                    bool isMini = gameObject->m_objectType == GameObjectType::MiniSizePortal;
                    if ((playerObject->m_vehicleSize < 1.f) != isMini) {
                        playerObject->togglePlayerScale(isMini, true);
                    }
                }
                else {
                    playerObject->m_playerSpeed = speedValue;
                }
            }
        }
        GJBaseGameLayer::collisionCheckObjects(playerObject, &relevantObjects, static_cast<int>(relevantObjects.size()), deltaTime);
    }

    void handleButton(bool isDown, int buttonId, bool isFirstPlayer) {
        if (!state.isSimulating && buttonId == 1 && !state.isInjecting) {
            auto* player1 = this->m_player1;
            recordRealInput(isDown, player1);
            bool isHeld = intercept(this, isDown, isFirstPlayer);
            sweepGhostLayer();
            if (isHeld) {
                return;
            }
        }
        GJBaseGameLayer::handleButton(isDown, buttonId, isFirstPlayer);
    }

    void playerTouchedRing(PlayerObject* playerObject, RingObject* ringObject) {
        if (state.isSimulating) {
            if (!state.smartEnabled || !isGhost(playerObject)) {
                return;
            }
            snapshotObject(ringObject);
            state.hasTouchedOrb = true;
            if (state.firstOrbTouchTick < 0) {
                state.firstOrbTouchTick = state.simulationTick;
                state.firstOrbRingObject = ringObject;
                state.firstOrbPlayerObject = playerObject;
                state.orbTouchDistanceSquared = orbDistanceSq(playerObject, ringObject);
                state.orbCloseSampleTick = state.simulationTick;
                state.orbCloseDistanceSquared = state.orbTouchDistanceSquared;
            }

            if (state.isProbing) {
                bool isKnown = false;
                for (auto const& existingProbe : state.probeOrbCandidates) {
                    if (existingProbe.ringObject == ringObject) {
                        isKnown = true;
                        break;
                    }
                }
                if (!isKnown) {
                    float touchDistanceSquared = orbDistanceSq(playerObject, ringObject);
                    ProbeOrb newProbe;
                    newProbe.ringObject = ringObject;
                    newProbe.playerObject = playerObject;
                    newProbe.touchDistanceSquared = touchDistanceSquared;
                    newProbe.closeDistanceSquared = touchDistanceSquared;
                    newProbe.touchSampleTick = state.simulationTick;
                    newProbe.closeSampleTick = state.simulationTick;
                    state.probeOrbCandidates.push_back(newProbe);
                }
            }
        }
        GJBaseGameLayer::playerTouchedRing(playerObject, ringObject);

        if (state.isSimulating && state.orbClickEnabled) {
            // every RingObject type is click-activated: jump/gravity/spider/drop rings call
            // ringJump, and DashRingObject calls startDashing. skipping dash rings here made the
            // buffer blind to that whole orb type, so the ghost never tested a real dash click.
            bool isFirst = playerObject == state.ghostPlayer1;
            bool& isHeld = isFirst ? state.isHeld1 : state.isHeld2;
            bool& isAutoClick = isFirst ? state.isAuto1 : state.isAuto2;
            int& autoReleaseTick = isFirst ? state.autoReleaseTick1 : state.autoReleaseTick2;
            if (state.orbClickDelayTicks > 0) {
                if (!isHeld && autoReleaseTick == -1) {
                    autoReleaseTick = state.simulationTick + state.orbClickDelayTicks;
                }
            }
            else if (!isHeld && !isAutoClick) {
                playerObject->pushButton(PlayerButton::Jump);
                isAutoClick = true;
            }
        }
    }

    void flipGravity(PlayerObject* playerObject, bool shouldFlip, bool suppressEffects) {
        if (state.isSimulating) {
            if (state.smartEnabled && isGhost(playerObject)) {
                playerObject->flipGravity(shouldFlip, true);
            }
            return;
        }
        GJBaseGameLayer::flipGravity(playerObject, shouldFlip, suppressEffects);
    }

    void gameEventTriggered(GJGameEvent gameEvent, int materialId, int playerId) {
        if (state.isSimulating) {
            return;
        }
        GJBaseGameLayer::gameEventTriggered(gameEvent, materialId, playerId);
    }

    void toggleGroupTriggered(int groupId, bool shouldActivate, gd::vector<int> const& remapKeyVector, int triggerId, int controlId) {
        if (state.isSimulating) {

            if (!shouldActivate) {
                if (auto* gameObjects = this->getGroup(groupId)) {
                    for (auto* gameObject : CCArrayExt<GameObject*>(gameObjects)) {
                        state.simulationDisabledObjects.insert(gameObject);
                    }
                }
            }
            return;
        }
        GJBaseGameLayer::toggleGroupTriggered(groupId, shouldActivate, remapKeyVector, triggerId, controlId);
    }

    void spawnGroup(int groupId, bool isOrdered, double delaySeconds, gd::vector<int> const& remapKeyVector, int triggerId, int controlId) {
        if (state.isSimulating) {
            return;
        }
        GJBaseGameLayer::spawnGroup(groupId, isOrdered, delaySeconds, remapKeyVector, triggerId, controlId);
    }

    void toggleGroup(int groupId, bool shouldActivate) {
        if (state.isSimulating) {
            return;
        }
        GJBaseGameLayer::toggleGroup(groupId, shouldActivate);
    }

    void pickupItem(EffectGameObject* effectObject) {
        if (state.isSimulating) {
            return;
        }
        GJBaseGameLayer::pickupItem(effectObject);
    }

    void updateCounters(int counterItemId, int counterValue) {
        if (state.isSimulating) {
            return;
        }
        GJBaseGameLayer::updateCounters(counterItemId, counterValue);
    }

    void activateItemEditTrigger(ItemTriggerGameObject* itemTrigger) {
        if (state.isSimulating) {
            return;
        }
        GJBaseGameLayer::activateItemEditTrigger(itemTrigger);
    }

    void activateItemCompareTrigger(ItemTriggerGameObject* itemTrigger, gd::vector<int> const& remapKeyVector) {
        if (state.isSimulating) {
            return;
        }
        GJBaseGameLayer::activateItemCompareTrigger(itemTrigger, remapKeyVector);
    }

    void playerWillSwitchMode(PlayerObject* playerObject, GameObject* gameObject) {
        if (state.isSimulating) {
            state.hasGhostHitPortal = true;
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(playerObject, gameObject);
    }
};
