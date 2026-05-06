/*
 * AnimationCache — Pre-baked Spine animation frame cache implementation.
 */

#include "spine/AnimationCache.h"
#include "timeline/SpineClip.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// AnimationCacheKey
// ═════════════════════════════════════════════════════════════════════════════

AnimationCacheKey AnimationCacheKey::fromClip(const SpineClip& clip,
                                              uint32_t w, uint32_t h,
                                              float fps)
{
    AnimationCacheKey key;
    key.character = clip.characterName();
    key.outfit    = clip.outfit();

    switch (clip.stance()) {
        case CharacterStance::Default: key.stance = "default"; break;
        case CharacterStance::Aim:     key.stance = "aim";     break;
        case CharacterStance::Cover:   key.stance = "cover";   break;
    }

    key.animation = clip.animationName();
    key.talking   = clip.isTalking();
    key.width     = w;
    key.height    = h;
    key.fps       = fps;
    return key;
}

std::string AnimationCacheKey::toString() const
{
    std::ostringstream o;
    o << character << '|' << outfit << '|' << stance << '|'
      << animation << '|' << (talking ? '1' : '0') << '|'
      << width << 'x' << height << '@' << std::fixed << std::setprecision(1) << fps;
    return o.str();
}

std::string AnimationCacheKey::toHash() const
{
    // Simple FNV-1a hash of toString(), output as 12-char hex string
    std::string s = toString();
    uint64_t hash = 14695981039346656037ULL;
    for (char c : s) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    std::ostringstream o;
    o << std::hex << std::setw(12) << std::setfill('0') << (hash & 0xFFFFFFFFFFFFULL);
    return o.str();
}

bool AnimationCacheKey::operator==(const AnimationCacheKey& other) const noexcept
{
    return character == other.character
        && outfit    == other.outfit
        && stance    == other.stance
        && animation == other.animation
        && talking   == other.talking
        && width     == other.width
        && height    == other.height
        && std::abs(fps - other.fps) < 0.01f;
}

// ═════════════════════════════════════════════════════════════════════════════
// AnimationCacheKeyHash
// ═════════════════════════════════════════════════════════════════════════════

size_t AnimationCacheKeyHash::operator()(const AnimationCacheKey& k) const noexcept
{
    // Golden-ratio hash mixing (same pattern as FrameCache)
    auto hashStr = std::hash<std::string>{};
    auto hashU32 = std::hash<uint32_t>{};
    auto hashBool = std::hash<bool>{};

    size_t h = hashStr(k.character);
    h ^= hashStr(k.outfit)    + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= hashStr(k.stance)    + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= hashStr(k.animation) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= hashBool(k.talking)  + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= hashU32(k.width)     + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= hashU32(k.height)    + 0x9e3779b9 + (h << 6) + (h >> 2);

    // Hash the fps as integer millifps for stability
    uint32_t millifps = static_cast<uint32_t>(k.fps * 1000.0f);
    h ^= hashU32(millifps) + 0x9e3779b9 + (h << 6) + (h >> 2);

    return h;
}

// ═════════════════════════════════════════════════════════════════════════════
// CachedAnimationEntry
// ═════════════════════════════════════════════════════════════════════════════

const CachedAnimFrame* CachedAnimationEntry::frame(size_t index) const noexcept
{
    if (frames.empty()) return nullptr;
    if (index >= frames.size()) index = frames.size() - 1;
    return &frames[index];
}

const CachedAnimFrame* CachedAnimationEntry::frameAtTime(float timeSeconds) const noexcept
{
    if (frames.empty() || duration <= 0.0f) return nullptr;

    // Wrap time for looping
    float t = std::fmod(timeSeconds, duration);
    if (t < 0.0f) t += duration;

    // Convert to frame index
    float frameIdx = (t / duration) * static_cast<float>(frames.size());
    size_t idx = static_cast<size_t>(frameIdx);
    if (idx >= frames.size()) idx = frames.size() - 1;

    return &frames[idx];
}

size_t CachedAnimationEntry::memoryUsage() const noexcept
{
    size_t total = sizeof(*this);
    for (auto& f : frames)
        total += f.memoryUsage();
    return total;
}

// ═════════════════════════════════════════════════════════════════════════════
// AnimationCache — construction
// ═════════════════════════════════════════════════════════════════════════════

AnimationCache::AnimationCache() = default;
AnimationCache::~AnimationCache() = default;

// ═════════════════════════════════════════════════════════════════════════════
// Configuration
// ═════════════════════════════════════════════════════════════════════════════

void AnimationCache::setCacheDirectory(const std::filesystem::path& dir)
{
    std::lock_guard lock(m_mutex);
    m_cacheDir = dir;
}

const std::filesystem::path& AnimationCache::cacheDirectory() const noexcept
{
    return m_cacheDir;
}

void AnimationCache::setMemoryBudget(size_t bytes)
{
    std::lock_guard lock(m_mutex);
    m_budget = bytes;
    evictUntilFits(0);
}

size_t AnimationCache::memoryBudget() const noexcept
{
    return m_budget;
}

// ═════════════════════════════════════════════════════════════════════════════
// Insert / Lookup
// ═════════════════════════════════════════════════════════════════════════════

void AnimationCache::put(std::shared_ptr<CachedAnimationEntry> entry)
{
    if (!entry) return;

    std::lock_guard lock(m_mutex);

    // If already cached, remove old entry first
    auto it = m_map.find(entry->key);
    if (it != m_map.end()) {
        m_used -= it->second.entry->memoryUsage();
        m_lru.erase(it->second.lruIt);
        m_map.erase(it);
    }

    size_t needed = entry->memoryUsage();

    // Evict LRU entries until there's room
    evictUntilFits(needed);

    // Insert at front of LRU
    m_lru.push_front(entry->key);

    CacheRecord record;
    record.entry = std::move(entry);
    record.lruIt = m_lru.begin();

    m_used += needed;
    m_map.emplace(record.entry->key, std::move(record));
}

std::shared_ptr<CachedAnimationEntry> AnimationCache::get(const AnimationCacheKey& key)
{
    std::lock_guard lock(m_mutex);

    auto it = m_map.find(key);
    if (it == m_map.end()) {
        ++m_misses;
        return nullptr;
    }

    // Validate if a validator is set
    if (m_validator && !m_validator(key)) {
        m_used -= it->second.entry->memoryUsage();
        m_lru.erase(it->second.lruIt);
        m_map.erase(it);
        ++m_misses;
        return nullptr;
    }

    // Promote to front of LRU
    m_lru.erase(it->second.lruIt);
    m_lru.push_front(key);
    it->second.lruIt = m_lru.begin();

    ++m_hits;
    return it->second.entry;
}

bool AnimationCache::contains(const AnimationCacheKey& key) const
{
    std::lock_guard lock(m_mutex);
    return m_map.count(key) > 0 || m_diskIndex.count(key) > 0;
}

bool AnimationCache::containsInMemory(const AnimationCacheKey& key) const
{
    std::lock_guard lock(m_mutex);
    return m_map.count(key) > 0;
}

bool AnimationCache::containsOnDisk(const AnimationCacheKey& key) const
{
    std::lock_guard lock(m_mutex);
    return m_diskIndex.count(key) > 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// Eviction
// ═════════════════════════════════════════════════════════════════════════════

void AnimationCache::evictCharacter(const std::string& character)
{
    std::lock_guard lock(m_mutex);

    std::vector<AnimationCacheKey> toRemove;
    for (auto& [k, _] : m_map) {
        if (k.character == character)
            toRemove.push_back(k);
    }

    for (auto& k : toRemove) {
        auto it = m_map.find(k);
        if (it != m_map.end()) {
            m_used -= it->second.entry->memoryUsage();
            m_lru.erase(it->second.lruIt);
            m_map.erase(it);
            ++m_evictions;
        }
    }
}

void AnimationCache::evict(const AnimationCacheKey& key)
{
    std::lock_guard lock(m_mutex);

    auto it = m_map.find(key);
    if (it != m_map.end()) {
        m_used -= it->second.entry->memoryUsage();
        m_lru.erase(it->second.lruIt);
        m_map.erase(it);
        ++m_evictions;
    }
}

void AnimationCache::clearMemory()
{
    std::lock_guard lock(m_mutex);
    m_map.clear();
    m_lru.clear();
    m_used = 0;
}

void AnimationCache::clearAll()
{
    std::lock_guard lock(m_mutex);
    m_map.clear();
    m_lru.clear();
    m_used = 0;
    m_diskIndex.clear();

    // Remove disk cache directory contents
    if (!m_cacheDir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(m_cacheDir, ec);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// LRU eviction helper
// ═════════════════════════════════════════════════════════════════════════════

void AnimationCache::evictUntilFits(size_t neededBytes)
{
    // Already called under lock
    while (!m_lru.empty() && (m_used + neededBytes > m_budget)) {
        // Evict least-recently-used (back of LRU list)
        auto& lruKey = m_lru.back();
        auto it = m_map.find(lruKey);
        if (it != m_map.end()) {
            m_used -= it->second.entry->memoryUsage();
            m_map.erase(it);
            ++m_evictions;
        }
        m_lru.pop_back();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Disk persistence — metadata format (simple custom, no JSON library)
// ═════════════════════════════════════════════════════════════════════════════

// Each on-disk cache entry is a directory named by the key's hex hash:
//   assets/cache/animations/<hash>/
//     metadata.txt     — key-value pairs, one per line
//     frame_0000.raw   — raw RGBA bytes
//     frame_0001.raw
//     ...
//
// metadata.txt format:
//   character=Modernia
//   outfit=default
//   stance=default
//   animation=idle
//   talking=0
//   width=1920
//   height=1080
//   fps=30.0
//   duration=2.0
//   frame_count=60

static std::filesystem::path entryDir(const std::filesystem::path& cacheDir,
                                       const AnimationCacheKey& key)
{
    // Use character_animation_hash for readability
    std::string dirName = key.character + "_" + key.animation + "_" + key.toHash();
    return cacheDir / dirName;
}

static bool writeMetadata(const std::filesystem::path& dir,
                           const CachedAnimationEntry& entry)
{
    std::ofstream f(dir / "metadata.txt");
    if (!f.is_open()) return false;

    f << "character=" << entry.key.character << '\n';
    f << "outfit="    << entry.key.outfit    << '\n';
    f << "stance="    << entry.key.stance    << '\n';
    f << "animation=" << entry.key.animation << '\n';
    f << "talking="   << (entry.key.talking ? 1 : 0) << '\n';
    f << "width="     << entry.key.width     << '\n';
    f << "height="    << entry.key.height    << '\n';
    f << "fps="       << std::fixed << std::setprecision(1) << entry.key.fps << '\n';
    f << "duration="  << std::fixed << std::setprecision(4) << entry.duration << '\n';
    f << "frame_count=" << entry.frames.size() << '\n';

    return f.good();
}

static bool readMetadata(const std::filesystem::path& dir,
                          AnimationCacheKey& key,
                          float& duration,
                          size_t& frameCount)
{
    std::ifstream f(dir / "metadata.txt");
    if (!f.is_open()) return false;

    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);

        if      (k == "character")    key.character = v;
        else if (k == "outfit")       key.outfit = v;
        else if (k == "stance")       key.stance = v;
        else if (k == "animation")    key.animation = v;
        else if (k == "talking")      key.talking = (v == "1");
        else if (k == "width")        key.width = static_cast<uint32_t>(std::stoul(v));
        else if (k == "height")       key.height = static_cast<uint32_t>(std::stoul(v));
        else if (k == "fps")          key.fps = std::stof(v);
        else if (k == "duration")     duration = std::stof(v);
        else if (k == "frame_count")  frameCount = std::stoull(v);
    }

    return true;
}

bool AnimationCache::saveToDisk(const AnimationCacheKey& key)
{
    std::shared_ptr<CachedAnimationEntry> entry;

    {
        std::lock_guard lock(m_mutex);
        auto it = m_map.find(key);
        if (it == m_map.end()) return false;
        entry = it->second.entry;
    }

    if (!entry) return false;

    std::filesystem::path dir = entryDir(m_cacheDir, key);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;

    // Write metadata
    if (!writeMetadata(dir, *entry)) return false;

    // Write frame data
    for (size_t i = 0; i < entry->frames.size(); ++i) {
        std::ostringstream name;
        name << "frame_" << std::setw(4) << std::setfill('0') << i << ".raw";

        std::ofstream ff(dir / name.str(), std::ios::binary);
        if (!ff.is_open()) return false;

        auto& frame = entry->frames[i];
        ff.write(reinterpret_cast<const char*>(frame.pixels.data()),
                 static_cast<std::streamsize>(frame.pixels.size()));

        if (!ff.good()) return false;
    }

    // Add to disk index
    {
        std::lock_guard lock(m_mutex);
        m_diskIndex[key] = dir;
    }

    return true;
}

bool AnimationCache::loadFromDisk(const AnimationCacheKey& key)
{
    std::filesystem::path dir;

    {
        std::lock_guard lock(m_mutex);

        // Already in memory?
        if (m_map.count(key) > 0) return true;

        // Find on disk
        auto it = m_diskIndex.find(key);
        if (it == m_diskIndex.end()) {
            // Try deriving the path
            dir = entryDir(m_cacheDir, key);
            if (!std::filesystem::exists(dir / "metadata.txt")) return false;
        } else {
            dir = it->second;
        }
    }

    // Read metadata
    AnimationCacheKey diskKey;
    float duration = 0.0f;
    size_t frameCount = 0;

    if (!readMetadata(dir, diskKey, duration, frameCount))
        return false;

    // Verify key matches
    if (!(diskKey == key)) return false;

    // Read frames
    auto entry = std::make_shared<CachedAnimationEntry>();
    entry->key = key;
    entry->duration = duration;
    entry->fps = key.fps;
    entry->frames.reserve(frameCount);

    size_t bytesPerFrame = static_cast<size_t>(key.width) * key.height * 4;

    for (size_t i = 0; i < frameCount; ++i) {
        std::ostringstream name;
        name << "frame_" << std::setw(4) << std::setfill('0') << i << ".raw";

        std::ifstream ff(dir / name.str(), std::ios::binary);
        if (!ff.is_open()) return false;

        CachedAnimFrame frame;
        frame.width  = key.width;
        frame.height = key.height;
        frame.pixels.resize(bytesPerFrame);

        ff.read(reinterpret_cast<char*>(frame.pixels.data()),
                static_cast<std::streamsize>(bytesPerFrame));

        if (!ff.good() && !ff.eof()) return false;

        entry->frames.push_back(std::move(frame));
    }

    // Insert into memory cache
    put(std::move(entry));
    return true;
}

void AnimationCache::scanDiskCache()
{
    std::lock_guard lock(m_mutex);

    if (m_cacheDir.empty()) return;

    std::error_code ec;
    if (!std::filesystem::exists(m_cacheDir, ec)) return;

    for (auto& dirEntry : std::filesystem::directory_iterator(m_cacheDir, ec)) {
        if (!dirEntry.is_directory()) continue;

        auto metaPath = dirEntry.path() / "metadata.txt";
        if (!std::filesystem::exists(metaPath, ec)) continue;

        AnimationCacheKey key;
        float duration = 0.0f;
        size_t frameCount = 0;

        if (readMetadata(dirEntry.path(), key, duration, frameCount)) {
            m_diskIndex[key] = dirEntry.path();
        }
    }
}

void AnimationCache::invalidateCharacter(const std::string& character)
{
    std::lock_guard lock(m_mutex);

    // Evict from memory
    std::vector<AnimationCacheKey> toRemoveMem;
    for (auto& [k, _] : m_map) {
        if (k.character == character)
            toRemoveMem.push_back(k);
    }
    for (auto& k : toRemoveMem) {
        auto it = m_map.find(k);
        if (it != m_map.end()) {
            m_used -= it->second.entry->memoryUsage();
            m_lru.erase(it->second.lruIt);
            m_map.erase(it);
            ++m_evictions;
        }
    }

    // Evict from disk index and remove disk files
    std::vector<AnimationCacheKey> toRemoveDisk;
    for (auto& [k, path] : m_diskIndex) {
        if (k.character == character) {
            toRemoveDisk.push_back(k);
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
    for (auto& k : toRemoveDisk)
        m_diskIndex.erase(k);
}

// ═════════════════════════════════════════════════════════════════════════════
// Validation
// ═════════════════════════════════════════════════════════════════════════════

void AnimationCache::setValidator(ValidatorCallback cb)
{
    std::lock_guard lock(m_mutex);
    m_validator = std::move(cb);
}

// ═════════════════════════════════════════════════════════════════════════════
// Statistics
// ═════════════════════════════════════════════════════════════════════════════

AnimationCacheStats AnimationCache::stats() const
{
    std::lock_guard lock(m_mutex);

    AnimationCacheStats s;
    s.hitCount      = m_hits;
    s.missCount     = m_misses;
    s.entryCount    = m_map.size();
    s.memoryUsed    = m_used;
    s.memoryBudget  = m_budget;
    s.diskEntries   = m_diskIndex.size();
    s.evictionCount = m_evictions;

    // Count total frames
    for (auto& [_, rec] : m_map)
        s.totalFrames += rec.entry->frameCount();

    return s;
}

size_t AnimationCache::entryCount() const
{
    std::lock_guard lock(m_mutex);
    return m_map.size();
}

size_t AnimationCache::memoryUsed() const
{
    std::lock_guard lock(m_mutex);
    return m_used;
}

// ═════════════════════════════════════════════════════════════════════════════
// Iteration
// ═════════════════════════════════════════════════════════════════════════════

std::vector<AnimationCacheKey> AnimationCache::keys() const
{
    std::lock_guard lock(m_mutex);

    std::vector<AnimationCacheKey> result;
    result.reserve(m_map.size());
    for (auto& [k, _] : m_map)
        result.push_back(k);
    return result;
}

std::vector<AnimationCacheKey> AnimationCache::diskKeys() const
{
    std::lock_guard lock(m_mutex);

    std::vector<AnimationCacheKey> result;
    result.reserve(m_diskIndex.size());
    for (auto& [k, _] : m_diskIndex)
        result.push_back(k);
    return result;
}

} // namespace rt
