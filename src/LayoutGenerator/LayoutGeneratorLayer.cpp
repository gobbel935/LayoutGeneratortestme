#include "LayoutGeneratorLayer.hpp"
#include <Geode/ui/GeodeUI.hpp>
#include "../GameObjectPool/GameObjectPool.hpp"
#include "../PlayerData/PlayerData.hpp"
#include "../PoolObject/PoolObject.hpp"
#include "../PlayerObject.cpp"
#include "../Settings/ObjectSettings.hpp"

LayoutGeneratorLayer *LayoutGeneratorLayer::create()
{
    auto ret = new LayoutGeneratorLayer();
    if (ret->init())
    {
        ret->autorelease();
        return ret;
    }

    delete ret;
    return nullptr;
}

bool LayoutGeneratorLayer::init()
{
    if (!CCLayer::init())
        return false;

    scheduleUpdate();

    return true;
}

void LayoutGeneratorLayer::reset()
{
    auto mod = Mod::get();

    m_isBuilding = true;
    m_boundsCeil = 1300.f;
    m_boundsFloor = 90.f;
    m_canPlaceNextFrame = true;
    m_elapsedTime = LevelEditorLayer::get()->m_levelSettings->m_songOffset + .133f;
    m_fishId = 0; // FIX: reset fish ID counter on each build
    m_halfBeatCount = (int)(m_elapsedTime / (60.f / mod->getSettingValue<float>("bpm")) * 2.f);
    m_hasTappedThisGamemode = false;
    m_isClickingLastFrame = false;
    m_lastPlacedFish = nullptr;
    m_lastPlacedFishPos = CCPoint{};
    m_lastPlacedJumpIndicator = nullptr;
    m_lastPlayerGamemode = PoolState::GAMEMODE_CUBE;
    m_lastSpikeBottomPos = CCPoint{};
    m_lastSpikeTopPos = CCPoint{};
    m_placeAgainTimer = -1;
    m_playerPosPauseCheck = CCPoint{};
    m_playerTrail.clear();
    m_shouldTap = PoolTap::NO;
    m_shouldTapTimer = 0;
    // use configurable initial tap balance
    m_tapBalance = mod->getSettingValue<float>("weight-tap-balance-init");
    m_boundsDebugCounter = 0;
}

void LayoutGeneratorLayer::buildStart()
{
    reset();
    placeCreditText("made with", CCPoint{105.f, 300.f});
    placeCreditText("layout generator", CCPoint{105.f, 285.f});
    LevelEditorLayer::get()->m_editorUI->onPlaytest(nullptr);
}

void LayoutGeneratorLayer::buildStop()
{
    LevelEditorLayer::get()->m_editorUI->onStopPlaytest(nullptr);
}

void LayoutGeneratorLayer::update(float dt)
{
    CCLayer::update(dt);

    if (!m_isBuilding)
        return;

    auto mod = Mod::get();
    auto editor = LevelEditorLayer::get();
    auto pd = new PlayerData();
    pd->setPlayer(editor->m_player1);
    if (!pd->player)
    {
        log::error("Player is null, aborting!");
        buildStop();
        return;
    }
    pd->velScaled = CCPoint{pd->velUnscaled.x * 60.f * dt, pd->velUnscaled.y * 60.f * dt};

    // dev: tps-adaptive timers — LayoutGeneratorLayer::update() is a generic Cocos2d schedule that
    // fires once per RENDERED frame (display FPS), but the player's actual physics simulate on a
    // separate, decoupled tick rate (TPS) via PlayerObject::updateJump, which can run multiple times
    // per render frame (high TPS / low FPS) or be skipped some frames (low TPS / high FPS). every
    // real physics tick already gets queued into m_queuedTrail by the updateJump hook below, so the
    // number of queued entries THIS frame is exactly how many physics ticks happened since we last
    // ran — that's the true tick rate, independent of display refresh rate, with no upper bound
    // (handles >240 TPS the same as any other value, since this just measures real elapsed ticks).
    const size_t ticksThisFrame = pd->player->m_fields->m_queuedTrail.size();
    const float tickDt = ticksThisFrame > 0 ? dt / (float)ticksThisFrame : dt;

    // internal "tick count" timers (tap delays, place-again windows) were originally authored as
    // hardcoded tick counts against some assumed reference physics rate (240 TPS being GD's classic
    // default, but this is exposed as a setting rather than hardcoded since the true original
    // reference isn't something we can verify with certainty). this rescales a tick count tuned at
    // that reference rate into an equivalent tick count for the player's actual current tick rate,
    // keeping real-time behavior consistent whether you're running 60, 240, or 1000+ TPS.
    const bool tpsAdaptive = mod->getSettingValue<bool>("dev-tps-adaptive-timers");
    const float tpsReference = mod->getSettingValue<float>("dev-tps-reference");
    auto ticksForReference = [&](int referenceTicks) -> int
    {
        if (!tpsAdaptive || tickDt <= 0.f || tpsReference <= 0.f)
            return referenceTicks;
        return std::max(1, (int)std::round((referenceTicks / tpsReference) / tickDt));
    };

    // paused
    if (m_playerPosPauseCheck == pd->pos)
        return;
    m_playerPosPauseCheck = pd->pos;

    // trail
    for (auto &trail : pd->player->m_fields->m_queuedTrail)
    {
        if (trail.pos.x > pd->pos.x)
            continue;

        trail.boundsCeil = m_boundsCeil;
        trail.boundsFloor = m_boundsFloor;

        fillSpiderTrail(trail.pos);

        m_playerTrail.push_back(trail);

        if (mod->getSettingValue<bool>("debug-trail"))
            placeDebugTrailClicking(trail.pos, trail.isClicking);

        if (mod->getSettingValue<bool>("jump-indicators") && trail.makeJumpIndicator)
            placeJumpIndicator(trail.pos, trail.state);
    }
    pd->player->m_fields->m_queuedTrail.clear();

    fillSpiderTrail(pd->pos);

    if (m_playerTrail.empty())
        return;

    // dev: bounds debug markers
    if (mod->getSettingValue<bool>("dev-bounds-debug"))
    {
        m_boundsDebugCounter++;
        if (m_boundsDebugCounter >= 10)
        {
            m_boundsDebugCounter = 0;
            placeDebugBoundsMarker(CCPoint{pd->pos.x, m_boundsFloor}, false);
            placeDebugBoundsMarker(CCPoint{pd->pos.x, m_boundsCeil}, true);
        }
    }

    // update gamemode bounds
    if (pd->gamemode != m_lastPlayerGamemode)
    {
        if (pd->gamemode & PoolState::NO_BOUNDS || pd->isCameraFree())
        {
            m_boundsFloor = 90.f;
            m_boundsCeil = 1300.f;
        }
        else
        {
            float height;
            if (pd->gamemode == PoolState::GAMEMODE_BALL)
                height = 240.f;
            else if (pd->gamemode == PoolState::GAMEMODE_SPIDER)
                height = 270.f;
            else
                height = 300.f;
            m_boundsFloor = std::max(90.f, 30.f * std::ceil((pd->player->m_lastPortalPos.y - (height / 2.f + 30.f)) / 30.f));
            m_boundsCeil = m_boundsFloor + height;
        }

        m_hasTappedThisGamemode = false;
    }

    // dev: lock tap balance
    if (mod->getSettingValue<bool>("dev-tap-balance-lock"))
        m_tapBalance = mod->getSettingValue<float>("dev-tap-balance-locked");
    // FIX: clamp tap balance to prevent extreme runaway values
    else
        m_tapBalance = std::clamp(m_tapBalance, -20.f, 20.f);

    // place object on beat
    float spb = 60.f / mod->getSettingValue<float>("bpm");
    bool usePlayerClicks = mod->getSettingValue<bool>("use-player-clicks");
    bool useRandomClicks = !usePlayerClicks;
    bool onBeat = false;
    bool onHalfBeat = false;
    if ((int)(m_elapsedTime / spb * 2.f) > m_halfBeatCount)
    {
        onBeat = m_halfBeatCount % 2 == 0;
        onHalfBeat = true;
        m_halfBeatCount++;
    }

    bool shouldPlace = false;
    int excludeTags = 0;
    int requireTap = 0;
    if (m_placeAgainTimer >= 0)
        m_placeAgainTimer--;
    if (usePlayerClicks)
    {
        if (pd->isClicking() && !pd->player->m_isDashing)
        {
            requireTap |= PoolTap::TAP | PoolTap::HOLD;
            if (pd->state & PoolState::NOT_FLYING)
            {
                if (pd->player->m_stateRingJump)
                    shouldPlace = true;
                else
                    excludeTags |= PoolTag::RING_BUFFERED;
            }
            else
            {
                if (!m_isClickingLastFrame)
                    shouldPlace = true;
                else
                    excludeTags |= PoolTag::RING;
            }

            if (m_lastPlacedFish && m_lastPlacedFish->keepActive)
                m_lastPlacedFish = nullptr;
        }
        else
        {
            requireTap |= PoolTap::NO | PoolTap::ANY;
        }
    }

    // read user-configurable beat/halfbeat chances
    const float beatChance = mod->getSettingValue<float>("weight-beat-chance");
    const float halfBeatChance = mod->getSettingValue<float>("weight-halfbeat-chance");

    if (shouldPlace)
        ;
    else if (m_placeAgainTimer == 0)
    {
        shouldPlace = true;
        if (useRandomClicks && m_lastPlacedFish && m_lastPlacedFish->tags & PoolTag::SPIDER)
            requireTap |= PoolTap::NO | PoolTap::ANY;
    }
    // avoid reaching terminal velocity (-15.f)
    else if (pd->velUnscaled.y * pd->getSign() < -10.f &&
             pd->state & (PoolState::NO_BOUNDS | PoolState::GAMEMODE_BALL) && onHalfBeat)
    {
        shouldPlace = true;
        excludeTags |= PoolTag::FALL;
    }
    // beat
    else if (utils::random::chance(beatChance) && onBeat)
    {
        shouldPlace = true;
        if (useRandomClicks && !(pd->state & PoolState::HOLD_FLYING) && utils::random::chance(0.5))
            requireTap |= PoolTap::TAP_OR_HOLD;
    }
    // half beat
    else if ((pd->gamemode & PoolState::FLYING || utils::random::chance(halfBeatChance)) && onHalfBeat)
    {
        shouldPlace = true;
    }

    // dev: always place — bypass gating and canPlaceNextFrame
    if (mod->getSettingValue<bool>("dev-always-place"))
    {
        shouldPlace = true;
    }
    else if (!m_canPlaceNextFrame)
    {
        shouldPlace = false;
    }

    // dev: freeze generation — record trail but don't place
    if (mod->getSettingValue<bool>("dev-freeze-generation"))
        shouldPlace = false;

    // continue placing a block platform
    if (m_lastPlacedFish && m_lastPlacedFish->keepActive)
        placeFish(pd, m_lastPlacedFish, !shouldPlace, true);

    // failsafe when jumping into the floor in reverse gravity
    if (pd->isUpsideDown() &&
        (pd->state & PoolState::NO_BOUNDS || pd->isCameraFree()) &&
        pd->state & PoolState::RISING &&
        pd->pos.y < m_boundsFloor + 30.f)
    {
        log::debug("{} FLOOR FAILSAFE", m_fishId);
        CCPoint pos{pd->pos.x, m_boundsFloor + 6.f};
        editor->createObject(ObjectId::SPIDER_PAD, pos, true)->setFlipY(true);
    }

    // place new object
    const PoolObject *fish = nullptr;
    if (shouldPlace)
    {
        if (!onBeat)
            excludeTags |= PoolTag::GAMEMODE | PoolTag::SPEED | PoolTag::SIZE_;

        if (!mod->getSettingValue<bool>("experimental-gameplay"))
            excludeTags |= PoolTag::EXPERIMENTAL;

        fish = fishLegally(pd, dt, excludeTags, requireTap);
        if (fish)
        {
            placeFish(pd, fish);

            m_shouldTapTimer = 0;

            if (fish->tap != PoolTap::ANY)
            {
                m_shouldTap = fish->tap;
            }

            if (fish->tap == PoolTap::NO)
            {
                m_tapBalance -= pd->state & PoolState::HOLD_FLYING ? .5f : 2.f;
            }
            else if (fish->tap == PoolTap::TAP)
            {
                m_tapBalance += 1.f;
            }
            else if (fish->tap == PoolTap::TAP_DELAYED)
            {
                m_tapBalance += 1.f;
                m_shouldTap = PoolTap::TAP;
                m_shouldTapTimer = ticksForReference(3);
            }
            else if (fish->tap & (PoolTap::HOLD | PoolTap::HOLD_RANDOM | PoolTap::RANDOM))
            {
                m_tapBalance += .5f;
            }
            else if (fish->tap == PoolTap::ANY)
            {
                m_tapBalance -= 1.f;
                if (pd->gamemode & PoolState::NOT_FLYING)
                    m_shouldTap = PoolTap::NO;
                else if (pd->gamemode & PoolState::HOLD_FLYING && utils::random::chance(0.5))
                    m_shouldTap = pd->isClicking() ? PoolTap::NO : PoolTap::HOLD;

                if (utils::random::chance(0.5))
                {
                    m_placeAgainTimer = ticksForReference(2);

                    if (pd->state & PoolState::GROUNDED)
                    {
                        log::debug("{} {} GROUNDED PLACEAGAIN", m_fishId, fish->name);
                        CCPoint pos = pd->pos;
                        pos.y -= pd->getRectSize().height / 2.f * pd->getSign();
                        pos.y -= 15.f * pd->getSign();
                        if (!isOutOfBounds(pos.y, 30.f, pd))
                            editor->createObject(ObjectId::BLOCK, pos, true);
                    }
                }
            }

            if (fish->tags & PoolTag::SPIDER && utils::random::chance(0.5))
            {
                m_placeAgainTimer = ticksForReference(2);
            }

            if (useRandomClicks && fish->tap & PoolTap::TAP_OR_HOLD && pd->isClicking())
            {
                if (fish->tags & PoolTag::RING ||
                    (pd->state & PoolState::JUMP_NEEDS_BUFFER && fish->tags & PoolTag::BLOCK) ||
                    pd->player->m_isDashing)
                {
                    pd->player->releaseButton(PlayerButton::Jump);
                    m_shouldTapTimer = std::max(1, m_shouldTapTimer);
                }
            }

            m_canPlaceNextFrame = false;
        }
        else
        {
            log::warn("fishing failed! last id: {}", m_fishId);
            m_canPlaceNextFrame = true;
        }
    }
    else
    {
        m_canPlaceNextFrame = true;
    }

    m_playerTrail.back().fish = fish;

    // spikes v2
    if (mod->getSettingValue<bool>("spike-boundary-enabled"))
    {
        const float spikeMargin = mod->getSettingValue<float>("spike-margin");
        const CCSize playerSize = pd->getRectSize();

        // the spike hitbox buffer is hardcoded to (8, 14) by default because the visible spike
        // sprite is noticeably larger than its real collision hitbox, and grazing the side still
        // kills you. dev-frame-perfect-spikes lets you override this buffer for testing, so
        // spike-margin = 0 can mean an actual frame-perfect (or even intentionally too-tight) gap
        // instead of always keeping this safety cushion.
        const bool framePerfectSpikes = mod->getSettingValue<bool>("dev-frame-perfect-spikes");
        const float spikeHitboxWidth = framePerfectSpikes ? mod->getSettingValue<float>("dev-spike-hitbox-width") : 8.f;
        const float spikeHitboxHeight = framePerfectSpikes ? mod->getSettingValue<float>("dev-spike-hitbox-height") : 14.f;

        const CCSize spikeSize{spikeHitboxWidth + spikeMargin / 2.f, spikeHitboxHeight};

        const float spikeScanRight = pd->pos.x - (30.f - playerSize.width) / 2.f;

        const float spikeX = spikeScanRight - (playerSize.width + spikeSize.width) / 2.f;

        const float spikeScanLeft = spikeX - (playerSize.width + spikeSize.width) / 2.f;

        bool spikeBottom = false;
        bool spikeTop = false;
        float yMin = FLT_MAX;
        float yMax = -FLT_MAX;
        PlayerTrailData leftTrail{};
        PlayerTrailData midTrail{};
        bool closeTheSpiderGap = false;
        float midMaxShrink = 0.0;
        for (auto it = m_playerTrail.rbegin(); it != m_playerTrail.rend(); ++it)
        {
            auto trail = *it;

            if (trail.pos.x > spikeScanRight)
                continue;

            if (trail.pos.x < pd->pos.x - pd->velUnscaled.x * 30.f)
                break;

            if (
                trail.state & (PoolState::AIRBORNE | PoolState::HAS_BOUNDS) ||
                (trail.isUpsideDown() && trail.velUnscaled.y >= 15.f) ||
                (trail.fish && trail.fish->tags & PoolTag::SPIDER))
                spikeBottom = true;
            if (
                trail.state & (PoolState::AIRBORNE | PoolState::HAS_BOUNDS) ||
                (!trail.isUpsideDown() && trail.velUnscaled.y <= -15.f) ||
                (trail.fish && trail.fish->tags & PoolTag::SPIDER))
                spikeTop = true;

            if (trail.pos.x > pd->pos.x - 96.f && trail.pos.x < spikeX)
            {
                if (trail.state & PoolState::GAMEMODE_SPIDER)
                    closeTheSpiderGap = true;
                else if (trail.fish && trail.fish->tags & PoolTag::SPIDER)
                    closeTheSpiderGap = true;
            }

            if (trail.pos.x < spikeScanLeft)
                continue;

            float shrink = 0.f;
            if (trail.state & PoolState::GAMEMODE_SHIP)
                shrink = 10.f;
            else if (trail.state & PoolState::GAMEMODE_WAVE && trail.state & PoolState::SIZE_NORMAL)
                shrink = 10.f;
            else if (trail.state & PoolState::GAMEMODE_UFO && trail.state & PoolState::SIZE_MINI)
                shrink = 10.f;

            float evilSpikeMargin = (trail.rectSize.height + spikeSize.height) / 2.f;
            float shrunkSpikeMargin = evilSpikeMargin + std::max(spikeMargin - shrink, 0.f);
            leftTrail = trail;
            if (trail.pos.x > spikeX)
            {
                midTrail = trail;
                midMaxShrink = shrunkSpikeMargin - evilSpikeMargin;
            }

            yMin = std::min(yMin, trail.pos.y - shrunkSpikeMargin);
            yMax = std::max(yMax, trail.pos.y + shrunkSpikeMargin);
        }

        if (!(midTrail.state & PoolState::GAMEMODE_WAVE))
        {
            float jumpShrink = std::min(midMaxShrink, midTrail.state & PoolState::SIZE_MINI ? 30.f : 15.f);
            if (leftTrail.pos.y < midTrail.pos.y - 1.f &&
                pd->pos.y < midTrail.pos.y - 1.f &&
                !midTrail.isUpsideDown())
                yMin += jumpShrink;
            if (leftTrail.pos.y > midTrail.pos.y + 1.f &&
                pd->pos.y > midTrail.pos.y + 1.f &&
                midTrail.isUpsideDown())
                yMax -= jumpShrink;
        }

        placeSpikeBoundary(
            spikeBottom,
            CCPoint{spikeX, yMin},
            spikeTop,
            CCPoint{spikeX, yMax},
            midTrail,
            spikeMargin <= 0.f
                ? 0.f
                : (closeTheSpiderGap
                       ? 6.f
                       : playerSize.width) +
                      6.f - pd->velScaled.x);
    }

    // jumping
    if (useRandomClicks)
    {
        if (m_shouldTapTimer > 0)
        {
            m_shouldTapTimer--;
        }
        else if (m_shouldTap == PoolTap::RANDOM)
        {
            if (utils::random::chance(0.5))
            {
                m_shouldTapTimer = utils::random::generate<int>(ticksForReference(2), ticksForReference(6));
                if (pd->isClicking())
                    pd->player->releaseButton(PlayerButton::Jump);
                else
                    pd->player->pushButton(PlayerButton::Jump);
            }
        }
        else if (m_shouldTap == PoolTap::TOWARDS_CENTER)
        {
            float mid = (m_boundsFloor + m_boundsCeil) / 2.f;
            if (pd->state & PoolState::GAMEMODE_SHIP)
                mid -= pd->velScaled.y * 10.f;
            else if (pd->state & PoolState::GAMEMODE_WAVE)
                m_shouldTapTimer = ticksForReference(3);

            bool push = pd->pos.y * pd->getSign() < mid * pd->getSign();

            float dist = abs(pd->pos.y - mid);
            if (dist < 60 && utils::random::chance((1 - dist / 60.f) * .5f))
                push = !push;

            if (push != pd->isClicking())
                if (push)
                    pd->player->pushButton(PlayerButton::Jump);
                else
                    pd->player->releaseButton(PlayerButton::Jump);
        }
        else if ((bool)(m_shouldTap & PoolTap::TAP_OR_HOLD) != pd->isClicking())
        {
            if (m_shouldTap & PoolTap::TAP_OR_HOLD)
            {
                pd->player->pushButton(PlayerButton::Jump);
                m_hasTappedThisGamemode = true;

                if (m_shouldTap & (PoolTap::TAP | PoolTap::HOLD_RANDOM))
                {
                    if (!(pd->state & PoolState::TAP_FLYING))
                    {
                        if (m_shouldTap == PoolTap::HOLD_RANDOM)
                            m_shouldTapTimer = (int)(utils::random::generate<float>(.03f, .33f) / dt);
                        else
                            m_shouldTapTimer = ticksForReference(3);
                    }
                    m_shouldTap = PoolTap::NO;
                }
            }
            else
            {
                pd->player->releaseButton(PlayerButton::Jump);
            }
        }
    }
    else
    {
        if (pd->isClicking())
            m_hasTappedThisGamemode = true;
    }

    if (mod->getSettingValue<bool>("debug-trail") && fish)
        placeDebugTrailBar(pd->pos);

    m_elapsedTime += dt;
    m_isClickingLastFrame = pd->isClicking();
    m_lastPlayerGamemode = pd->gamemode;
}

const PoolObject *LayoutGeneratorLayer::fishLegally(PlayerData *pd, float dt, int excludeTags, int requireTap)
{
    auto mod = Mod::get();
    auto objectWhitelist = mod->getSettingValue<std::unordered_set<int>>("objects");

    // read spawn weight multipliers (global, per-category)
    const float wBlocks  = mod->getSettingValue<float>("weight-blocks");
    const float wPads    = mod->getSettingValue<float>("weight-pads");
    const float wRings   = mod->getSettingValue<float>("weight-rings");
    const float wPortals = mod->getSettingValue<float>("weight-portals");

    // read spawn weight multipliers (subcategories — stack multiplicatively with the global weight above)
    const float wBlocksJump      = mod->getSettingValue<float>("weight-blocks-jump");
    const float wBlocksFall      = mod->getSettingValue<float>("weight-blocks-fall");
    const float wBlocksPlatform  = mod->getSettingValue<float>("weight-blocks-platform");
    const float wBlocksSpider    = mod->getSettingValue<float>("weight-blocks-spider");
    const float wBlocksBreakable = mod->getSettingValue<float>("weight-blocks-breakable");
    const float wPadsSpider      = mod->getSettingValue<float>("weight-pads-spider");
    const float wRingsSpider     = mod->getSettingValue<float>("weight-rings-spider");
    const float wRingsDash       = mod->getSettingValue<float>("weight-rings-dash");
    const float wRingsBlack      = mod->getSettingValue<float>("weight-rings-black");
    const float wPortalsGravity  = mod->getSettingValue<float>("weight-portals-gravity");
    const float wPortalsSize     = mod->getSettingValue<float>("weight-portals-size");
    const float wPortalsSpeed    = mod->getSettingValue<float>("weight-portals-speed");
    const float wPortalsGamemode = mod->getSettingValue<float>("weight-portals-gamemode");

    const bool disableDedup       = mod->getSettingValue<bool>("dev-disable-dedup");
    const bool disableTrailCheck  = mod->getSettingValue<bool>("dev-disable-trail-check");
    const bool verboseLog         = mod->getSettingValue<bool>("dev-verbose-log");
    const std::string forceFish   = mod->getSettingValue<std::string>("dev-force-fish");

    const int blindScanBehind = (int)(.3f / dt);
    bool isBlind = false;
    if (!mod->getSettingValue<bool>("use-player-clicks") &&
        m_playerTrail.size() > blindScanBehind &&
        pd->state & PoolState::NO_BOUNDS)
        isBlind = abs(pd->pos.y - m_playerTrail[m_playerTrail.size() - blindScanBehind].pos.y) > 130.f;

    const PoolObject *result = GameObjectPool::fish(
        [&](const PoolObject *fish) -> float
        {
            // dev: force a specific fish by name — bypasses all other checks
            if (!forceFish.empty())
                return fish->name == forceFish ? 1000.f : 0.f;

            // tag blacklist
            if (fish->tags & excludeTags)
                return 0.f;

            // require tap
            if (requireTap > 0 && !(fish->tap & requireTap))
                return 0.f;

            // match state
            if (!fish->matchesPlayerState(pd->state))
                return 0.f;

            // object id whitelist
            if (ObjectSettingsNode::OBJECT_ID_WHITELISTABLE.contains(fish->objectId) &&
                !objectWhitelist.contains(fish->objectId))
                return 0.f;

            // blind jumps
            if (isBlind && fish->tap & PoolTap::TAP_OR_HOLD)
                return 0.f;

            // chaining portal into another portal looks bad
            if (m_placeAgainTimer == 0 && fish->tap == PoolTap::ANY)
                return 0.f;

            // tapping a green orb that results in dying to the floor boundary
            if ((pd->state & PoolState::NO_BOUNDS || pd->isCameraFree()) &&
                !pd->isUpsideDown() &&
                pd->pos.y < m_boundsFloor + 135.f &&
                fish->tags & PoolTag::FALL &&
                fish->tags & PoolTag::GRAVITY)
                return 0.f;

            // changing gamemode when it hasn't tapped yet
            if (!m_hasTappedThisGamemode && fish->tags & PoolTag::GAMEMODE)
                return 0.f;

            // tap balance
            float weight = 1.f;
            if (m_tapBalance < 0 && fish->tap & (PoolTap::NO | PoolTap::ANY))
                weight = 1.f / -m_tapBalance;
            if (m_tapBalance > 0 && fish->tap & (PoolTap::TAP_OR_HOLD | PoolTap::RANDOM))
                weight = 1.f / m_tapBalance;

            if (pd->state & PoolState::NO_BOUNDS || pd->isCameraFree())
            {
                if (pd->isUpsideDown())
                {
                    if (pd->pos.y < m_boundsFloor + 225.f && fish->tags & PoolTag::JUMP_HIGH)
                        return 0.f;
                    if (pd->pos.y < m_boundsFloor + 150.f && fish->tags & PoolTag::JUMP)
                        return 0.f;
                }
                else
                {
                    if (pd->pos.y > m_boundsCeil - 210.f && fish->tags & PoolTag::JUMP_HIGH)
                        return 0.f;
                    if (pd->pos.y > m_boundsCeil - 135.f && fish->tags & PoolTag::JUMP)
                        return 0.f;
                }

                if (pd->pos.y > m_boundsCeil - 210.f)
                {
                    if (pd->isUpsideDown() && fish->tags & PoolTag::FALL)
                        return 0.f;
                    if (!pd->isUpsideDown() && fish->tags & PoolTag::JUMP)
                        return 0.f;
                    if (pd->isUpsideDown() && fish->tags & PoolTag::GRAVITY)
                        weight *= 10.f;
                }
            }
            else
            {
                float mid = (m_boundsFloor + m_boundsCeil) / 2.f;
                if (pd->state & PoolState::GAMEMODE_SHIP)
                    mid -= pd->velScaled.y * 10.f;

                float dist = abs(pd->pos.y - mid);
                float penalty = 1 - dist / (pd->state & (PoolState::GAMEMODE_SHIP | PoolState::GAMEMODE_UFO) ? 30.f : 125.f);

                if (pd->pos.y * pd->getSign() < mid * pd->getSign())
                {
                    if (!(pd->state & PoolState::JUMP_CHANGES_GRAVITY))
                    {
                        if ((fish->tags & PoolTag::FALL && !(fish->tags & PoolTag::GRAVITY)) ||
                            (fish->tags & PoolTag::JUMP && fish->tags & PoolTag::GRAVITY))
                            weight *= penalty;
                    }
                    else if (fish->tags & PoolTag::FALL)
                        weight *= penalty;
                }
                else
                {
                    if (!(pd->state & PoolState::JUMP_CHANGES_GRAVITY))
                    {
                        if ((fish->tags & PoolTag::JUMP && !(fish->tags & PoolTag::GRAVITY)) ||
                            (fish->tags & PoolTag::FALL && fish->tags & PoolTag::GRAVITY))
                            weight *= penalty;
                    }
                    else if (fish->tags & PoolTag::JUMP)
                        weight *= penalty;
                }
            }

            // apply user-defined spawn weight multipliers (global category weight * matching subcategory weight)
            if (fish->tags & PoolTag::PORTAL)
            {
                weight *= wPortals;
                if (fish->tags & PoolTag::GRAVITY)
                    weight *= wPortalsGravity;
                else if (fish->tags & PoolTag::SIZE_)
                    weight *= wPortalsSize;
                else if (fish->tags & PoolTag::SPEED)
                    weight *= wPortalsSpeed;
                else if (fish->tags & PoolTag::GAMEMODE)
                    weight *= wPortalsGamemode;
            }
            else if (fish->tags & PoolTag::RING)
            {
                weight *= wRings;
                if (fish->tags & PoolTag::SPIDER)
                    weight *= wRingsSpider;
                // dash rings carry neither JUMP nor FALL tags
                else if (!(fish->tags & PoolTag::JUMP) && !(fish->tags & PoolTag::FALL))
                    weight *= wRingsDash;
                // black orbs are the only non-spider ring with FALL but no GRAVITY
                else if (fish->tags & PoolTag::FALL && !(fish->tags & PoolTag::GRAVITY))
                    weight *= wRingsBlack;
            }
            else if (fish->tags & PoolTag::PAD)
            {
                weight *= wPads;
                if (fish->tags & PoolTag::SPIDER)
                    weight *= wPadsSpider;
            }
            else if (fish->tags & PoolTag::BLOCK)
            {
                weight *= wBlocks;
                if (fish->tags & PoolTag::SPIDER)
                    weight *= wBlocksSpider;
                else if (fish->tags & PoolTag::JUMP)
                    weight *= wBlocksJump;
                else if (fish->tags & PoolTag::FALL)
                    weight *= wBlocksFall;
                else
                    weight *= wBlocksPlatform;
            }
            else if (fish->tags & PoolTag::BREAKABLE_BLOCK)
            {
                weight *= wBlocks * wBlocksBreakable;
            }

            return weight;
        });

    // dev: verbose logging
    if (verboseLog && result)
    {
        log::info("[DEV] fish #{}: '{}' | tags={:#x} | tap={} | objectId={}",
            m_fishId, result->name, result->tags, (int)result->tap, result->objectId);
    }

    return result;
}

void LayoutGeneratorLayer::placeFish(PlayerData *pd, const PoolObject *fish, bool dedup, bool useLastY)
{
    auto mod = Mod::get();
    auto editor = LevelEditorLayer::get();
    auto playerRect = pd->player->getObjectRect();

    const bool disableDedup      = mod->getSettingValue<bool>("dev-disable-dedup");
    const bool disableTrailCheck = mod->getSettingValue<bool>("dev-disable-trail-check");

    auto pos = CCPoint{
        pd->pos.x + pd->velScaled.x,
        pd->pos.y + (pd->state & PoolState::GROUNDED ? 0.f : pd->velScaled.y)};
    if (fish->alignPlayer & PoolAlign::T)
        pos.y += playerRect.size.height / 2.f * pd->getSign();
    else if (fish->alignPlayer & PoolAlign::B)
        pos.y -= playerRect.size.height / 2.f * pd->getSign();
    if (fish->alignPlayer & PoolAlign::L)
        pos.x -= playerRect.size.width / 2.f;
    else if (fish->alignPlayer & PoolAlign::R)
        pos.x += playerRect.size.width / 2.f;

    bool shouldPlace = true;
    GameObject *primaryObj = nullptr;
    if (fish->objectId >= 0)
    {
        auto tempObj = GameObject::createWithKey(fish->objectId);
        if (pd->isUpsideDown())
        {
            if (fish->canFlip())
                tempObj->setFlipY(true);
            tempObj->setRotation(-fish->rotation);
        }
        else
            tempObj->setRotation(fish->rotation);

        auto tempObjRect = getObjectRect(tempObj);
        if (fish->alignObject & PoolAlign::T)
            pos.y -= tempObjRect.getMaxY() * pd->getSign();
        else if (fish->alignObject & PoolAlign::B)
            pos.y -= tempObjRect.getMinY() * pd->getSign();
        if (fish->alignObject & PoolAlign::L)
            pos.x -= tempObjRect.getMinX();
        else if (fish->alignObject & PoolAlign::R)
            pos.x -= tempObjRect.getMaxX();
        if (useLastY)
            pos.y = m_lastPlacedFishPos.y;

        if (isOutOfBounds(pos.y, tempObjRect.size.height, pd))
            shouldPlace = false;
        // FIX: respect dev-disable-dedup flag
        else if (!disableDedup && dedup && getObjectNearPoint(pos, 24.f, fish->objectId) != nullptr)
            shouldPlace = false;

        // FIX: respect dev-disable-trail-check flag
        if (shouldPlace && !(fish->tags & PoolTag::RING) && !disableTrailCheck)
        {
            tempObjRect.origin += pos;
            if (doesRectInterfereWithTrail(tempObjRect, pd->pos.x, fish->tags & PoolTag::BLOCK, pd->state & PoolState::SIZE_MINI))
            {
                shouldPlace = false;
                log::debug("{} {} CANCELLED due to trail interference!", m_fishId, fish->name);
            }
        }

        if (shouldPlace)
        {
            primaryObj = editor->createObject(fish->objectId, pos, true);
            // FIX: null-check createObject result before using it
            if (!primaryObj)
            {
                log::warn("{} {} createObject returned null!", m_fishId, fish->name);
                shouldPlace = false;
            }
            else
            {
                if (pd->isUpsideDown())
                {
                    if (fish->canFlip())
                        primaryObj->setFlipY(true);
                    primaryObj->setRotation(-fish->rotation);
                }
                else
                    primaryObj->setRotation(fish->rotation);

                auto primaryObjRect = getObjectRect(primaryObj);

                // spider pad and ring patch
                if (fish->tags & PoolTag::SPIDER)
                {
                    if (fish->rotation == 180.f)
                    {
                        primaryObj->setFlipY(!primaryObj->m_isFlipY);
                        primaryObj->setRotation(0.f);
                    }
                    primaryObj->customSetup();
                }

                // every ring patch
                if (auto ringObj = typeinfo_cast<RingObject *>(primaryObj))
                {
                    if (primaryObjRect.intersectsRect(playerRect))
                        pd->player->addToTouchedRings(ringObj);
                }

                // enable free mode for gamemode portals
                if (fish->tags & PoolTag::GAMEMODE && mod->getSettingValue<bool>("camera-free-mode"))
                {
                    if (auto effectObj = typeinfo_cast<EffectGameObject *>(primaryObj))
                    {
                        effectObj->m_cameraIsFreeMode = true;
                    }
                }

                // enable preview for speed portals
                if (fish->tags & PoolTag::SPEED)
                {
                    if (auto effectObj = typeinfo_cast<EffectGameObject *>(primaryObj))
                    {
                        effectObj->m_shouldPreview = true;
                        editor->tryUpdateSpeedObject(effectObj, false);
                    }
                }

                // place d blocks in wave
                if (fish->objectId == ObjectId::GAMEMODE_PORTAL_WAVE ||
                    (pd->state & PoolState::GAMEMODE_WAVE && fish->objectId == ObjectId::BLOCK))
                    placeDBlock(pos);
            }
        }
    }

    // place ground below spider taps, pads, and rings
    if (fish->tags & PoolTag::SPIDER &&
        (!mod->getSettingValue<bool>("use-player-clicks") || !(fish->tags & PoolTag::BLOCK)) &&
        shouldPlace)
    {
        bool up = pd->isUpsideDown() != (bool)(fish->tags & PoolTag::GRAVITY);
        float yMin, yMax;
        if (up)
        {
            yMin = pd->pos.y + (fish->tags & PoolTag::GRAVITY ? 30.f : 60.f);
            yMax = pd->state & PoolState::HAS_BOUNDS && !pd->isCameraFree()
                       ? m_boundsCeil
                       : std::min(m_boundsCeil, yMin + 150.f);
        }
        else
        {
            yMax = pd->pos.y - (fish->tags & PoolTag::GRAVITY ? 30.f : 60.f);
            yMin = pd->state & PoolState::HAS_BOUNDS && !pd->isCameraFree()
                       ? m_boundsFloor
                       : std::max(m_boundsFloor, yMax - 150.f);
        }
        CCPoint pos{pd->pos.x + pd->velScaled.x, utils::random::generate<float>(yMin, yMax)};
        auto tempObj = GameObject::createWithKey(ObjectId::BLOCK);
        bool didPlace;
        while (true)
        {
            auto tempObjRect = getObjectRect(tempObj);
            tempObjRect.origin += pos;
            if (!doesRectInterfereWithTrail(tempObjRect, pd->pos.x, true, pd->state & PoolState::SIZE_MINI))
            {
                editor->createObject(ObjectId::BLOCK, pos, true)->setFlipY(up);
                didPlace = true;
                break;
            }
            if (pd->state & PoolState::HAS_BOUNDS && !pd->isCameraFree())
            {
                didPlace = false;
                break;
            }
            pos.y += 30.f * (up ? 1 : -1);
        }
        if (didPlace && pd->state & PoolState::GAMEMODE_WAVE)
            placeDBlock(pos);
    }

    // force trigger rings when flying
    if (mod->getSettingValue<bool>("use-player-clicks") &&
        pd->state & PoolState::FLYING &&
        fish->tags & PoolTag::RING_LATE &&
        shouldPlace)
    {
        if (pd->state & PoolState::GAMEMODE_SWING)
            pd->player->flipGravity(!pd->isUpsideDown(), true);
        pd->player->pushButton(PlayerButton::Jump);
        if (pd->state & PoolState::TAP_FLYING && m_lastPlacedJumpIndicator)
            editor->removeObject(m_lastPlacedJumpIndicator, true);
    }

    // place label
    if (mod->getSettingValue<bool>("debug-trail") && !useLastY)
    {
        placeLabel(
            fish->name,
            CCPoint{pos.x, pos.y - 60.f});
        placeLabel(
            std::to_string(m_fishId),
            CCPoint{pos.x, pos.y - 67.5f});
    }

    m_fishId++;
    m_lastPlacedFish = fish;
    m_lastPlacedFishPos = pos;
}

void LayoutGeneratorLayer::placeCreditText(std::string text, CCPoint pos)
{
    auto textObj = static_cast<TextGameObject *>(LevelEditorLayer::get()->createObject(ObjectId::TEXT, pos, true));
    textObj->updateCustomScaleX(0.5);
    textObj->updateCustomScaleY(0.5);
    textObj->m_zLayer = ZLayer::T2;
    textObj->updateTextObject(text, false);
}

void LayoutGeneratorLayer::placeDBlock(CCPoint pos)
{
    auto obj = LevelEditorLayer::get()->createObject(ObjectId::D_BLOCK, pos, true);
    obj->updateCustomScaleX(3.0);
    obj->updateCustomScaleY(3.0);
}

void LayoutGeneratorLayer::placeDebugTrailBar(CCPoint pos)
{
    auto obj = LevelEditorLayer::get()->createObject(ObjectId::TRAIL_INDICATOR_PLACING, pos, true);
    obj->updateCustomScaleY(0.25);
    obj->setRotation(90.f);
    obj->m_editorLayer = 1;
}

void LayoutGeneratorLayer::placeDebugTrailClicking(CCPoint pos, bool isClicking)
{
    auto obj = LevelEditorLayer::get()->createObject(
        isClicking ? ObjectId::TRAIL_INDICATOR_CLICKING : ObjectId::TRAIL_INDICATOR,
        pos,
        true);
    obj->updateCustomScaleX(0.25);
    obj->updateCustomScaleY(0.25);
    obj->m_editorLayer = 1;
}

void LayoutGeneratorLayer::placeJumpIndicator(CCPoint pos, int state)
{
    bool isFlying = state & PoolState::FLYING;
    bool isMini = state & PoolState::SIZE_MINI;
    int sign = state & PoolState::GRAVITY_REVERSE ? -1 : 1;

    auto obj = LevelEditorLayer::get()->createObject(
        isFlying ? ObjectId::JUMP_INDICATOR_FLYING : ObjectId::JUMP_INDICATOR_GROUNDED,
        pos + CCPoint{0.f, isFlying || isMini ? 0.f : -5.f * sign},
        true);

    if (isFlying)
    {
        obj->updateCustomScaleX(0.5);
        obj->updateCustomScaleY(0.5);
    }
    else
    {
        obj->setRotation(90.f * sign);
    }

    obj->m_editorLayer = 1;

    m_lastPlacedJumpIndicator = obj;
}

void LayoutGeneratorLayer::placeLabel(std::string text, CCPoint pos)
{
    auto textObj = static_cast<TextGameObject *>(LevelEditorLayer::get()->createObject(ObjectId::TEXT, pos, true));
    textObj->updateCustomScaleX(0.25);
    textObj->updateCustomScaleY(0.25);
    textObj->m_editorLayer = 1;
    textObj->m_zLayer = ZLayer::T2;
    textObj->updateTextObject(text, false);
}

void LayoutGeneratorLayer::placeDebugBoundsMarker(CCPoint pos, bool isTop)
{
    auto obj = LevelEditorLayer::get()->createObject(ObjectId::TRAIL_INDICATOR_PLACING, pos, true);
    obj->updateCustomScaleX(0.5);
    obj->updateCustomScaleY(0.5);
    obj->setRotation(isTop ? 90.f : -90.f);
    obj->m_editorLayer = 2;
}

void LayoutGeneratorLayer::placeSpikeBoundary(
    bool bottom,
    CCPoint bottomPos,
    bool top,
    CCPoint topPos,
    const PlayerTrailData &midTrail,
    float dedupDistance)
{
    const float verticalFillDist = midTrail.rectSize.height + 11.f;

    // bottom
    if (getObjectNearPoint(bottomPos, dedupDistance, ObjectId::SPIKE) == nullptr)
    {
        if (bottom)
        {
            placeSpikeInBounds(bottomPos, midTrail, false);
            if (m_lastSpikeBottomPos.x > 0)
            {
                CCPoint bottomPos2 = bottomPos;
                while (bottomPos2.y > m_lastSpikeBottomPos.y + verticalFillDist)
                {
                    bottomPos2.y -= verticalFillDist;
                    placeSpikeInBounds(bottomPos2, midTrail, false);
                }
                while (bottomPos.y < m_lastSpikeBottomPos.y - verticalFillDist)
                {
                    m_lastSpikeBottomPos.y -= verticalFillDist;
                    placeSpikeInBounds(m_lastSpikeBottomPos, midTrail, false);
                }
            }
        }
        m_lastSpikeBottomPos = bottomPos;
    }

    // top
    if (getObjectNearPoint(topPos, dedupDistance, ObjectId::SPIKE) == nullptr)
    {
        if (top)
        {
            placeSpikeInBounds(topPos, midTrail, true);
            if (m_lastSpikeTopPos.x > 0)
            {
                while (topPos.y > m_lastSpikeTopPos.y + verticalFillDist)
                {
                    m_lastSpikeTopPos.y += verticalFillDist;
                    placeSpikeInBounds(m_lastSpikeTopPos, midTrail, true);
                }
                CCPoint topPos2 = topPos;
                while (topPos2.y < m_lastSpikeTopPos.y - verticalFillDist)
                {
                    topPos2.y += verticalFillDist;
                    placeSpikeInBounds(topPos2, midTrail, true);
                }
            }
        }
        m_lastSpikeTopPos = topPos;
    }
}

void LayoutGeneratorLayer::placeSpikeInBounds(CCPoint pos, const PlayerTrailData &trail, bool flipY)
{
    if (!isOutOfBounds(pos.y, 12.f, trail))
    {
        if (auto spike = LevelEditorLayer::get()->createObject(ObjectId::SPIKE, pos, true))
        {
            spike->setFlipY(flipY);
        }
    }
}

bool LayoutGeneratorLayer::doesRectInterfereWithTrail(CCRect rect, float playerX, bool isBlock, bool isMini)
{
    for (auto it = m_playerTrail.rbegin(); it != m_playerTrail.rend(); ++it)
    {
        auto trail = *it;
        if (trail.pos.x > playerX - 10.f)
            continue;
        if (trail.pos.x < playerX - 45.f)
            break;

        auto playerRect = CCRect(
            trail.pos.x - trail.rectSize.width / 2.f,
            trail.pos.y - trail.rectSize.height / 2.f,
            trail.rectSize.width,
            trail.rectSize.height);

        if (isBlock)
            playerRect.inflateRect(isMini ? -2.f : -4.f);

        if (playerRect.intersectsRect(rect))
        {
            log::debug("interfere x = {}", trail.pos.x - playerX);
            return true;
        }
    }

    return false;
}

void LayoutGeneratorLayer::fillSpiderTrail(CCPoint targetPos)
{
    if (m_playerTrail.empty())
        return;

    const float fillDistance = 30.f;

    CCPoint pos = m_playerTrail.back().pos;
    while (abs(pos.y - targetPos.y) > fillDistance)
    {
        pos.y += fillDistance * (pos.y > targetPos.y ? -1 : 1);

        PlayerTrailData spiderFillTrail(m_playerTrail.back());
        spiderFillTrail.pos = pos;
        if (spiderFillTrail.state & PoolState::GROUNDED)
        {
            spiderFillTrail.state &= ~PoolState::GROUNDED;
            spiderFillTrail.state |= PoolState::AIRBORNE;
        }

        m_playerTrail.push_back(spiderFillTrail);

        if (Mod::get()->getSettingValue<bool>("debug-trail"))
            placeDebugTrailClicking(spiderFillTrail.pos, spiderFillTrail.isClicking);
    }
}

bool LayoutGeneratorLayer::isOutOfBounds(float y, float height, bool hasUpperBound, float boundsCeil, float boundsFloor)
{
    return y + height / 2.f <= boundsFloor || (y - height / 2.f >= boundsCeil && hasUpperBound);
}

bool LayoutGeneratorLayer::isOutOfBounds(float y, float height, PlayerData *pd)
{
    return isOutOfBounds(y, height, pd->state & PoolState::HAS_BOUNDS && !pd->isCameraFree(), m_boundsCeil, m_boundsFloor);
}

bool LayoutGeneratorLayer::isOutOfBounds(float y, float height, const PlayerTrailData &trail)
{
    return isOutOfBounds(y, height, trail.state & PoolState::HAS_BOUNDS && !trail.isCameraFree(), trail.boundsCeil, trail.boundsFloor);
}

GameObject *LayoutGeneratorLayer::getObjectNearPoint(CCPoint point, float radius, int objectId)
{
    auto editor = LevelEditorLayer::get();
    auto length = editor->m_objects->count();

    for (int i = length - 1; i > std::max(0, (int)length - 100); i--)
    {
        GameObject *obj = static_cast<GameObject *>(editor->m_objects->objectAtIndex(i));
        if (obj == nullptr)
            continue;
        if (objectId >= 0 && obj->m_objectID != objectId)
            continue;
        auto pos = obj->getPosition();
        if (pow(pos.x - point.x, 2) + pow(pos.y - point.y, 2) <= pow(radius, 2))
            return obj;
    }

    return nullptr;
}

CCRect LayoutGeneratorLayer::getObjectRect(GameObject *obj)
{
    if (obj->m_isObjectRectDirty)
    {
        auto save1 = obj->m_isObjectRectDirty;
        auto save2 = obj->m_boxOffsetCalculated;

        auto rect = obj->getObjectRect();

        obj->m_isObjectRectDirty = save1;
        obj->m_boxOffsetCalculated = save2;

        return rect;
    }
    else
    {
        return obj->m_objectRect;
    }
}

bool LayoutGeneratorLayer::getIsBuilding()
{
    return m_isBuilding;
}

void LayoutGeneratorLayer::onBuildButton(CCObject *)
{
    if (m_isBuilding)
        buildStop();
    else
        buildStart();
}

void LayoutGeneratorLayer::onSettingsButton(CCObject *)
{
    geode::openSettingsPopup(Mod::get(), false);
}

void LayoutGeneratorLayer::playtestStopped()
{
    m_isBuilding = false;
}
