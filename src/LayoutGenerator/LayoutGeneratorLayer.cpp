#include "LayoutGeneratorLayer.hpp"
#include <Geode/ui/GeodeUI.hpp>
#include "../GameObjectPool/GameObjectPool.hpp"
#include "../PlayerData/PlayerData.hpp"
#include "../PoolObject/PoolObject.hpp"
#include "../PlayerObject.cpp"
#include "../Settings/ObjectSettings.hpp"

namespace
{
    float getPlacementIntervalSeconds()
    {
        auto mod = Mod::get();
        if (mod->getSettingValue<bool>("constant-cps"))
            return 1.f / std::max(0.01f, mod->getSettingValue<float>("cps"));
        return 60.f / std::max(0.01f, mod->getSettingValue<float>("bpm"));
    }
}

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
    m_halfBeatCount = (int)(m_elapsedTime / getPlacementIntervalSeconds() * 2.f);
    m_hasTappedThisGamemode = false;
    m_isClickingLastFrame = false;
    m_lastPlacedFish = nullptr;
    m_lastPlacedFishPos = CCPoint{};
    m_lastPlacedJumpIndicator = nullptr;
    m_lastPlayerGamemode = PoolState::GAMEMODE_CUBE;
    m_lastSpikeBottomPos = CCPoint{};
    m_lastSpikeTopPos = CCPoint{};
    m_placeAgainTimer = -1;
    m_playerTrail.clear();
    m_shouldTap = PoolTap::NO;
    m_shouldTapTimer = 0;
    m_tapBalance = 3.f;
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

    // FIX: stack-allocate PlayerData instead of heap to avoid a per-frame
    // memory leak (original code: auto pd = new PlayerData(); — never deleted).
    PlayerData pdStorage;
    pdStorage.setPlayer(editor->m_player1);
    if (!pdStorage.player)
    {
        log::error("Player is null, aborting!");
        buildStop();
        return;
    }
    pdStorage.velScaled = CCPoint{pdStorage.velUnscaled.x * 60.f * dt,
                                   pdStorage.velUnscaled.y * 60.f * dt};
    auto pd = &pdStorage;

    // paused
    if (!m_playerTrail.empty() && m_playerTrail.back().pos == pd->pos)
        return;

    // -----------------------------------------------------------------------
    // BUG FIX — fast-click detection via trail queue
    //
    // Root cause: for NOT_FLYING gamemodes (cube/ball/robot/spider) with
    // "use-player-clicks" on, shouldPlace is gated on m_stateRingJump being
    // true at the moment update() runs.  m_stateRingJump is transient — it can
    // be set and cleared entirely inside a single pushButton() → updateJump()
    // call, BEFORE this update() frame executes.  Fast clicks therefore produce
    // no output because m_stateRingJump is already false (and sometimes
    // m_jumpBuffered / isClicking() is also false) by the time we get here.
    //
    // The MyPlayerObject::updateJump hook already queues a trail entry with
    // makeJumpIndicator=true whenever a tap is detected.  We read that queue
    // below and raise jumpDetectedInTrail as a reliable "a tap happened since
    // the last frame" signal, independent of current m_stateRingJump state.
    // -----------------------------------------------------------------------
    bool jumpDetectedInTrail = false;

    // trail processing
    for (auto &trail : pd->player->m_fields->m_queuedTrail)
    {
        if (trail.pos.x > pd->pos.x)
            continue;

        trail.boundsCeil = m_boundsCeil;
        trail.boundsFloor = m_boundsFloor;

        fillSpiderTrail(trail.pos);

        m_playerTrail.push_back(trail);

        // fast-click fix: record any jump event captured since the last update
        if (trail.makeJumpIndicator)
            jumpDetectedInTrail = true;

        // debug trail
        if (mod->getSettingValue<bool>("debug-trail"))
            placeDebugTrailClicking(trail.pos, trail.isClicking);

        // jump indicators v3
        if (mod->getSettingValue<bool>("jump-indicators") && trail.makeJumpIndicator)
            placeJumpIndicator(trail.pos, trail.state);
    }
    pd->player->m_fields->m_queuedTrail.clear();

    fillSpiderTrail(pd->pos);

    // player isn't ready yet
    if (m_playerTrail.empty())
        return;

    // update gamemode bounds
    if (pd->gamemode != m_lastPlayerGamemode)
    {
        if (pd->gamemode & PoolState::NO_BOUNDS)
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

    // place object on beat
    float spb = getPlacementIntervalSeconds();
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
        const bool fastClickFix = mod->getSettingValue<bool>("fast-click-fix");

        // A tap was fully processed before this update() frame ran (fast-click
        // path).  Only meaningful for NOT_FLYING gamemodes where m_stateRingJump
        // is the normal signal — flying gamemodes use isClicking() directly.
        const bool fastTapDetected =
            fastClickFix &&
            jumpDetectedInTrail &&
            (pd->state & PoolState::NOT_FLYING) &&
            !pd->player->m_isDashing;

        if ((pd->isClicking() || fastTapDetected) && !pd->player->m_isDashing)
        {
            requireTap |= PoolTap::TAP | PoolTap::HOLD;

            if (pd->state & PoolState::NOT_FLYING)
            {
                // Original: only m_stateRingJump.
                // Fix: also accept a tap confirmed from the trail queue.
                if (pd->player->m_stateRingJump || fastTapDetected)
                    shouldPlace = true;
                else
                    excludeTags |= PoolTag::RING_BUFFERED;
            }
            else
            {
                // Flying: trigger on the first frame the button goes down.
                // Fast-click fix is NOT_FLYING only, so check isClicking() here.
                if (pd->isClicking() && !m_isClickingLastFrame)
                    shouldPlace = true;
                else
                    excludeTags |= PoolTag::RING;
            }

            // stop placing block platform
            if (m_lastPlacedFish && m_lastPlacedFish->keepActive)
                m_lastPlacedFish = nullptr;
        }
        else
        {
            requireTap |= PoolTap::NO | PoolTap::ANY;
        }
    }

    if (shouldPlace)
        ;
    else if (m_placeAgainTimer == 0)
    {
        shouldPlace = true;
        if (useRandomClicks && m_lastPlacedFish && m_lastPlacedFish->tags & PoolTag::SPIDER)
            requireTap |= PoolTap::NO | PoolTap::ANY;
    }
    else if (pd->velUnscaled.y * pd->getSign() < -10.f &&
             pd->state & (PoolState::NO_BOUNDS | PoolState::GAMEMODE_BALL) && onHalfBeat)
    {
        shouldPlace = true;
        excludeTags |= PoolTag::FALL;
    }
    else if (utils::random::chance(15.0 / 16.0) && onBeat)
    {
        shouldPlace = true;
        if (useRandomClicks && utils::random::chance(0.5))
            requireTap |= PoolTap::TAP_OR_HOLD;
    }
    else if ((pd->gamemode & PoolState::FLYING || utils::random::chance(3.0 / 4.0)) && onHalfBeat)
    {
        shouldPlace = true;
    }

    // prevent placing two objects in two sequential frames
    if (!m_canPlaceNextFrame)
        shouldPlace = false;

    // -----------------------------------------------------------------------
    // FEATURE — spawn chance
    // After all other placement logic, apply a probabilistic filter.
    // At 100 % (default) behaviour is identical to before.
    // Lower values skip otherwise-valid placement opportunities, producing
    // sparser layouts.
    // -----------------------------------------------------------------------
    if (shouldPlace)
    {
        const float spawnChance = mod->getSettingValue<float>("spawn-chance") / 100.0f;
        if (spawnChance < 1.0f && !utils::random::chance(static_cast<double>(spawnChance)))
            shouldPlace = false;
    }

    // continue placing a block platform
    if (m_lastPlacedFish && m_lastPlacedFish->keepActive)
        placeFish(pd, m_lastPlacedFish, !shouldPlace, true);

    // cube/robot failsafe when jumping into the floor in reverse gravity
    if (pd->isUpsideDown() &&
        pd->state & PoolState::NO_BOUNDS &&
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
        // only change gamemode, speed, and size on beat
        if (!onBeat)
            excludeTags |= PoolTag::GAMEMODE | PoolTag::SPEED | PoolTag::SIZE_;

        // experimental gameplay
        if (!mod->getSettingValue<bool>("experimental-gameplay"))
            excludeTags |= PoolTag::EXPERIMENTAL;

        // -----------------------------------------------------------------------
        // FEATURE — blocks enabled toggle
        // When disabled, all BLOCK-tagged objects (ground jumps, landing blocks,
        // platforms) are excluded so the generator produces ring / pad / portal
        // only gameplay.
        // -----------------------------------------------------------------------
        if (!mod->getSettingValue<bool>("blocks-enabled"))
            excludeTags |= PoolTag::BLOCK;

        fish = fishLegally(pd, dt, excludeTags, requireTap);
        if (fish)
        {
            placeFish(pd, fish);

            m_shouldTapTimer = 0;

            if (fish->tap != PoolTap::ANY)
                m_shouldTap = fish->tap;

            if (fish->tap == PoolTap::NO)
            {
                m_tapBalance -= pd->state & PoolState::HOLD_FLYING ? 1.f : 2.f;
            }
            else if (fish->tap == PoolTap::TAP)
            {
                m_tapBalance += 1.f;
            }
            else if (fish->tap == PoolTap::TAP_DELAYED)
            {
                m_tapBalance += 1.f;
                m_shouldTap = PoolTap::TAP;
                m_shouldTapTimer = 3;
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
                    m_placeAgainTimer = 2;

                    if (pd->state & PoolState::GROUNDED)
                    {
                        log::debug("{} {} GROUNDED PLACEAGAIN", m_fishId, fish->name);
                        CCPoint pos = pd->pos;
                        pos.y -= pd->getRectSize().height / 2.f * pd->getSign();
                        pos.y -= 15.f * pd->getSign();
                        if (!isOutOfBounds(pos.y, 30.f, pd->state & PoolState::HAS_BOUNDS))
                            editor->createObject(ObjectId::BLOCK, pos, true);
                    }
                }
            }

            if (fish->tags & PoolTag::SPIDER && utils::random::chance(0.5))
                m_placeAgainTimer = 2;

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

        const CCSize spikeSize{8.f + spikeMargin / 2.f, 14.f};

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
                trail.state & PoolState::GRAVITY_REVERSE && trail.velUnscaled.y >= 15.f ||
                (trail.fish && trail.fish->tags & PoolTag::SPIDER))
                spikeBottom = true;
            if (
                trail.state & (PoolState::AIRBORNE | PoolState::HAS_BOUNDS) ||
                trail.state & PoolState::GRAVITY_NORMAL && trail.velUnscaled.y <= -15.f ||
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
                midTrail.state & PoolState::GRAVITY_NORMAL)
                yMin += jumpShrink;
            if (leftTrail.pos.y > midTrail.pos.y + 1.f &&
                pd->pos.y > midTrail.pos.y + 1.f &&
                midTrail.state & PoolState::GRAVITY_REVERSE)
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
                : (closeTheSpiderGap ? 6.f : playerSize.width) + 6.f - pd->velScaled.x);
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
                m_shouldTapTimer = utils::random::generate<int>(2, 6);
                if (pd->isClicking())
                    pd->player->releaseButton(PlayerButton::Jump);
                else
                    pd->player->pushButton(PlayerButton::Jump);
            }
        }
        else if (m_shouldTap == PoolTap::TOWARDS_CENTER)
        {
            auto mid = (m_boundsFloor + m_boundsCeil) / 2.f;
            if (pd->state & PoolState::GAMEMODE_SHIP)
                mid -= pd->velScaled.y * 10.f;
            else if (pd->state & PoolState::GAMEMODE_WAVE)
                m_shouldTapTimer = 3;

            bool push = pd->pos.y * pd->getSign() < mid * pd->getSign();

            auto dist = abs(pd->pos.y - mid);
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
                            m_shouldTapTimer = 3;
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

    const int blindScanBehind = (int)(.3f / dt);
    bool isBlind = false;
    if (!mod->getSettingValue<bool>("use-player-clicks") &&
        m_playerTrail.size() > blindScanBehind &&
        pd->state & PoolState::NO_BOUNDS)
        isBlind = abs(pd->pos.y - m_playerTrail[m_playerTrail.size() - blindScanBehind].pos.y) > 130.f;

    return GameObjectPool::fish(
        [&](const PoolObject *fish)
        {
            if (fish->tags & excludeTags)
                return 0.f;

            if (requireTap > 0 && !(fish->tap & requireTap))
                return 0.f;

            if (!fish->matchesPlayerState(pd->state))
                return 0.f;

            if (ObjectSettingsNode::OBJECT_ID_WHITELISTABLE.contains(fish->objectId) &&
                !objectWhitelist.contains(fish->objectId))
                return 0.f;

            if (isBlind && fish->tap & PoolTap::TAP_OR_HOLD)
                return 0.f;

            if (m_placeAgainTimer == 0 && fish->tap == PoolTap::ANY)
                return 0.f;

            if (pd->state & PoolState::NO_BOUNDS &&
                !pd->isUpsideDown() &&
                pd->pos.y < m_boundsFloor + 135.f &&
                fish->tags & PoolTag::FALL &&
                fish->tags & PoolTag::GRAVITY)
                return 0.f;

            if (!m_hasTappedThisGamemode && fish->tags & PoolTag::GAMEMODE)
                return 0.f;

            float weight = 1.f;
            if (m_tapBalance < 0 && fish->tap & (PoolTap::NO | PoolTap::ANY))
                weight = 1.f / -m_tapBalance;
            if (m_tapBalance > 0 && fish->tap & (PoolTap::TAP_OR_HOLD | PoolTap::RANDOM))
                weight = 1.f / m_tapBalance;

            if (pd->state & PoolState::NO_BOUNDS)
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
                auto mid = (m_boundsFloor + m_boundsCeil) / 2.f;
                if (pd->state & PoolState::GAMEMODE_SHIP)
                    mid -= pd->velScaled.y * 10.f;

                auto dist = abs(pd->pos.y - mid);

                if (pd->pos.y * pd->getSign() < mid * pd->getSign())
                {
                    if (!(pd->state & PoolState::JUMP_CHANGES_GRAVITY))
                    {
                        if ((fish->tags & PoolTag::FALL && !(fish->tags & PoolTag::GRAVITY)) ||
                            (fish->tags & PoolTag::JUMP && fish->tags & PoolTag::GRAVITY))
                            weight *= 1 - dist / 30.f;
                    }
                    else if (fish->tags & PoolTag::FALL)
                        weight *= 1 - dist / 125.f;
                }
                else
                {
                    if (!(pd->state & PoolState::JUMP_CHANGES_GRAVITY))
                    {
                        if ((fish->tags & PoolTag::JUMP && !(fish->tags & PoolTag::GRAVITY)) ||
                            (fish->tags & PoolTag::FALL && fish->tags & PoolTag::GRAVITY))
                            weight *= 1 - dist / 30.f;
                    }
                    else if (fish->tags & PoolTag::JUMP)
                        weight *= 1 - dist / 125.f;
                }
            }

            return weight;
        });
}

void LayoutGeneratorLayer::placeFish(PlayerData *pd, const PoolObject *fish, bool dedup, bool useLastY)
{
    auto mod = Mod::get();
    auto editor = LevelEditorLayer::get();
    auto playerRect = pd->player->getObjectRect();

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

        if (isOutOfBounds(pos.y, tempObjRect.size.height, pd->state & PoolState::HAS_BOUNDS))
            shouldPlace = false;
        else if (dedup && getObjectNearPoint(pos, 24.f, fish->objectId) != nullptr)
            shouldPlace = false;

        if (shouldPlace && !(fish->tags & PoolTag::RING))
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
            if (pd->isUpsideDown())
            {
                if (fish->canFlip())
                    primaryObj->setFlipY(true);
                primaryObj->setRotation(-fish->rotation);
            }
            else
                primaryObj->setRotation(fish->rotation);

            auto primaryObjRect = getObjectRect(primaryObj);

            if (fish->tags & PoolTag::SPIDER)
            {
                if (fish->rotation == 180.f)
                {
                    primaryObj->setFlipY(!primaryObj->m_isFlipY);
                    primaryObj->setRotation(0.f);
                }
                primaryObj->customSetup();
            }

            if (auto ringObj = typeinfo_cast<RingObject *>(primaryObj))
            {
                if (primaryObjRect.intersectsRect(playerRect))
                    pd->player->addToTouchedRings(ringObj);
            }

            if (fish->tags & PoolTag::SPEED)
            {
                if (auto effectObj = typeinfo_cast<EffectGameObject *>(primaryObj))
                {
                    effectObj->m_shouldPreview = true;
                    editor->tryUpdateSpeedObject(effectObj, false);
                }
            }

            if (fish->objectId == ObjectId::GAMEMODE_PORTAL_WAVE ||
                (pd->state & PoolState::GAMEMODE_WAVE && fish->objectId == ObjectId::BLOCK))
                placeDBlock(pos);
        }
    }

    if (fish->tags & PoolTag::SPIDER &&
        (!mod->getSettingValue<bool>("use-player-clicks") || !(fish->tags & PoolTag::BLOCK)) &&
        shouldPlace)
    {
        bool up = pd->isUpsideDown() != (bool)(fish->tags & PoolTag::GRAVITY);
        float yMin, yMax;
        if (up)
        {
            yMin = pd->pos.y + (fish->tags & PoolTag::GRAVITY ? 30.f : 60.f);
            yMax = pd->state & PoolState::HAS_BOUNDS ? m_boundsCeil : std::min(m_boundsCeil, yMin + 150.f);
        }
        else
        {
            yMax = pd->pos.y - (fish->tags & PoolTag::GRAVITY ? 30.f : 60.f);
            yMin = pd->state & PoolState::HAS_BOUNDS ? m_boundsFloor : std::max(m_boundsFloor, yMax - 150.f);
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
            if (pd->state & PoolState::HAS_BOUNDS)
            {
                didPlace = false;
                break;
            }
            pos.y += 30.f * (up ? 1 : -1);
        }
        if (didPlace && pd->state & PoolState::GAMEMODE_WAVE)
            placeDBlock(pos);
    }

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

    if (mod->getSettingValue<bool>("debug-trail") && !useLastY)
    {
        placeLabel(fish->name, CCPoint{pos.x, pos.y - 60.f});
        placeLabel(std::to_string(m_fishId), CCPoint{pos.x, pos.y - 67.5f});
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
        pos, true);
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

void LayoutGeneratorLayer::placeSpikeBoundary(
    bool bottom, CCPoint bottomPos,
    bool top,    CCPoint topPos,
    const PlayerTrailData &midTrail,
    float dedupDistance)
{
    const float verticalFillDist = midTrail.rectSize.height + 11.f;

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
    if (!isOutOfBounds(pos.y, 12.f, trail.state & PoolState::HAS_BOUNDS, trail.boundsCeil, trail.boundsFloor))
    {
        if (auto spike = LevelEditorLayer::get()->createObject(ObjectId::SPIKE, pos, true))
            spike->setFlipY(flipY);
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

bool LayoutGeneratorLayer::isOutOfBounds(float y, float height, bool hasUpperBound)
{
    return isOutOfBounds(y, height, hasUpperBound, m_boundsCeil, m_boundsFloor);
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
